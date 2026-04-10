/*
 * Custom Firmware for NOUS A5T (ESP8266 / ESP8285)
 * Based on Tasmota template: {"NAME":"NOUS A5T","GPIO":[0,3072,544,3104,0,259,0,0,225,226,224,0,35,4704],"FLAG":1,"BASE":18}
 */
#define FW_VERSION "2.7.1-EN"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <stdlib.h>

// ================= PINS =================
const int RELAY_PINS[] = {14, 12, 13, 5}; // R1, R2, R3, R4(USB)
const bool RELAY_INVERTED[] = {false, false, false, true}; // R4 is inverted (relay4i)
const int RELAY_COUNT = 4;
const int BTN_PIN = 16; // GPIO16
const int LED_PIN = 2;  // GPIO2 (LED Link)

// ================= CONSTANTS =================
// Factory Default values for calibration
#define DEFAULT_CAL_V 2.380000f
#define DEFAULT_CAL_I 0.930000f
#define DEFAULT_CAL_P 2.510000f

// Time Intervals (ms)
#define MQTT_PUB_INTERVAL 1000
#define MQTT_RECONNECT_INTERVAL 15000
#define CAL_STABILIZE_DELAY 1000
#define CAL_TOTAL_DURATION 15000
#define CFG_SAVE_DELAY 5000
#define CSE_RETRY_INTERVAL 3000
#define LOW_POWER_THRESHOLD 15.0f // Power threshold (W) below which 15s average is applied
#define LONG_AVG_INTERVAL 15000   // Long averaging interval (15 seconds in ms)
#define P_NOISE_MIN          0.30f     // below 0.3W = noise
#define I_NOISE_MIN          0.03f     // below 30mA = noise
#define P_DEADBAND           0.4f      // below 0.4 W -> consider 0
#define P_OFFSET             4.6f      // determined offset for internal consumption
#define EMA_ALPHA            0.10f     // Global Smoothing Factor (EMA)
#define MAX_P_SAMPLES        2500      // Support for 300s (5 min) with safety margin
#define MAX_POWER_LIMIT 3680.0f // 16A limit at 230V
#define CONFIG_MAGIC 0xA5A50005 // Configuration version

// ADC Button Thresholds
#define ADC_BTN1_MIN 720
#define ADC_BTN1_MAX 770
#define ADC_BTN2_MIN 450
#define ADC_BTN2_MAX 500
#define ADC_BTN3_MIN 200
#define ADC_BTN3_MAX 250

// ================= STRUCTS =================
struct WifiConfig {
  char ssid[32];
  char pass[32];
};

struct MqttConfig {
  char host[32];
  uint16_t port;
  char user[32];
  char pass[32];
  char client_id[32];
  char topic[32];
  bool enabled;
  char mdns_hostname[32];
  uint16_t pub_interval; // Reporting interval in seconds
};

struct AppConfig {
  uint32_t magic;
  bool relay_state[4]; // Save the state of each relay
  float cal_voltage;   // Voltage calibration factor
  float cal_current;   // Current calibration factor
  float cal_power;     // Power calibration factor
  bool child_lock;     // Physical buttons lock
  uint8_t power_on_behavior; // 0: OFF, 1: ON, 2: PREVIOUS
  
  // Security Settings
  bool auth_config;    // Auth for config pages
  char user_config[16];
  char pass_config[16];
  bool auth_root;      // Auth for status page
  char user_root[16];
  char pass_root[16];
  char ota_pass[32];   // OTA Password
  bool mask_wifi;      // Mask WiFi password in Setup
  bool mask_mqtt;      // Mask MQTT password in Config
  bool mask_auth_root; // Mask Root password
  bool mask_auth_config; // Mask Config password
};

WifiConfig wifiCfg;
MqttConfig mqttCfg;
AppConfig appCfg;

// ================= OBJECTS =================
ESP8266WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);
SoftwareSerial cseSerial(3, 1); // RX=GPIO3, TX=GPIO1 (Configuration confirmed functional)

// ================= STATE =================
bool setupMode = false;
unsigned long wifiConnectCount = 0;
unsigned long wifiDisconnectCount = 0;
unsigned long mqttConnectCount = 0;
unsigned long mqttDisconnectCount = 0;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
bool configDirty = false;
unsigned long lastChangeTime = 0;
bool lastMqttConnectedState = false;

// CSE7766 Vars
double cse_voltage = 0.0;
double cse_current = 0.0;
double cse_power = 0.0;
double cse_pf = 0.0;
double ema_power = 0.0;

// Raw values from CSE7766 (instantaneous, uncalibrated)
double cse_raw_v = 0.0;
double cse_raw_i = 0.0;
double cse_raw_p = 0.0;

// Accumulators for 1-second average (used by mqttPublishEnergy to get a stable 1s value)
double raw_v_sum_1s = 0.0;
double raw_i_sum_1s = 0.0;
double raw_p_sum_1s = 0.0;
int raw_count_1s = 0;

uint8_t cse_buff[32];
int cse_idx = 0;
unsigned long lastCsePacketTime = 0; // Last valid packet time (for retry)

// Accumulators for MQTT reporting (average over set interval)
double mqtt_raw_v_sum = 0.0;
double mqtt_raw_i_sum = 0.0;
double mqtt_raw_p_sum = 0.0;
int mqtt_raw_count = 0;
unsigned long last_mqtt_report_time = 0; // Timestamp of the last MQTT publication

// Buffer for advanced filtering (Median + Trimmed Mean)
float pBuffer[MAX_P_SAMPLES];
int pCount = 0;

// Calibration State
bool isCalibrating = false;
unsigned long calStartTime = 0;
float calTargetP = 0;
double sumRawV = 0, sumRawI = 0, sumRawP = 0;
int calSamples = 0;
String calStatusMsg = "System ready";

// Forward declarations
void mqttPublishState(int relayIdx);
void handleButtons();
void handlePeriodicUpdates();
void sendDiscoveryMessages();
void startNormalServices();
void pageSecurity();
void handleSaveSecurity();
void handleSaveInterval();

// Auth helper
bool checkAuth(bool required, const char* user, const char* pass) {
  if (!required) return true;
  if (!server.authenticate(user, pass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ================= FILTERING HELPERS (LOW POWER STABILITY) =================
// Comparison function for qsort
int compareFloats(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

float getMedian(float *arr, int n) {
    if (n <= 0) return 0.0f;
    float* temp = (float*)malloc(n * sizeof(float));
    if (!temp) return 0.0f; // Fail safe if RAM is critical

    memcpy(temp, arr, n * sizeof(float));
    qsort(temp, n, sizeof(float), compareFloats);
    float result = (n % 2 == 1) ? temp[n/2] : (temp[n/2 - 1] + temp[n/2]) * 0.5f;
    free(temp);
    return result;
}

float getTrimmedMean(float *arr, int n) {
    if (n < 5) return getMedian(arr, n);
    float* temp = (float*)malloc(n * sizeof(float));
    if (!temp) return getMedian(arr, n);

    memcpy(temp, arr, n * sizeof(float));
    qsort(temp, n, sizeof(float), compareFloats);
    int cut = n * 0.2f; // Remove 20% of extreme values
    if (cut < 1) cut = 1;
    float sum = 0;
    int count = 0;
    for (int i = cut; i < n - cut; i++) {
        sum += temp[i];
        count++;
    }
    float result = (count > 0) ? (sum / count) : 0.0f;
    free(temp);
    return (result > 0) ? result : getMedian(arr, n);
}

void ingestPowerSample(double RawP, double P, double I) {
    // Spike prevention
    if (RawP > 5000.0 || P > 10000.0) return;

    float val = (float)P - P_OFFSET;
    if (val < 0.0f) val = 0.0f;
    if (val < P_DEADBAND) val = 0.0f;
    if (I < I_NOISE_MIN) val = 0.0f;

    // EMA Filter (Exponential Moving Average)
    ema_power = ema_power + (double)EMA_ALPHA * ((double)val - ema_power);

    if (P < LOW_POWER_THRESHOLD && pCount < MAX_P_SAMPLES) {
        pBuffer[pCount] = (float)ema_power;
        pCount++;
    }
}

// Page and service registration for normal mode
void startNormalServices() {
    MDNS.begin(mqttCfg.mdns_hostname);
    ArduinoOTA.setHostname("NOUS A5T");
    ArduinoOTA.setPassword(appCfg.ota_pass);
    ArduinoOTA.begin();
    MDNS.addService("http", "tcp", 80);
    
    server.on("/", pageStatus);
    server.on("/toggle", handleToggle);
    server.on("/toggle_child_lock", handleToggleChildLock);
    server.on("/set_pob", handleSetPob);
    server.on("/api/sensors", handleApiSensors);
    server.on("/api/test_mqtt", handleTestMqtt);
    server.on("/config", pageConfig);
    server.on("/security", pageSecurity);
    server.on("/save_security", handleSaveSecurity);
    server.on("/openhab", handleOpenHABGen);
    server.on("/esphome", handleESPHomeGen);
    server.on("/calibration", pageCalibration);
    server.on("/manual_calibration", pageManualCalibration);
    server.on("/save_connection", handleSaveConnection);
    server.on("/save_mdns", handleSaveMdns);
    server.on("/save_topic", handleSaveTopic);
    server.on("/save_interval", handleSaveInterval);
    server.on("/save_mqtt_control", handleSaveMqttControl);
    server.on("/force_discovery", handleForceDiscovery);
    server.on("/do_calibrate", handleDoCalibrate);
    server.on("/reset_calibration", handleResetCalibration);
    server.on("/save_calibration", handleSaveCalibration);
    server.on("/reset_stats", handleResetStats);
    server.on("/reset", handleReset);
    server.on("/update", HTTP_GET, handleUpdatePage);
    server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
    
    mqtt.setServer(mqttCfg.host, mqttCfg.port);
    mqtt.setBufferSize(768);
    mqtt.setCallback(mqttCallback);
}

// ================= FILESYSTEM =================
void saveWifiConfig() {
  File f = LittleFS.open("/wifi.bin.tmp", "w");
  if (f) {
    size_t written = f.write((uint8_t*)&wifiCfg, sizeof(WifiConfig));
    f.close();
    if (written == sizeof(WifiConfig)) {
      LittleFS.remove("/wifi.bin");
      LittleFS.rename("/wifi.bin.tmp", "/wifi.bin");
    }
  }
}

void saveMqttConfig() {
  File f = LittleFS.open("/mqtt.bin.tmp", "w");
  if (f) {
    size_t written = f.write((uint8_t*)&mqttCfg, sizeof(MqttConfig));
    f.close();
    if (written == sizeof(MqttConfig)) {
      LittleFS.remove("/mqtt.bin");
      LittleFS.rename("/mqtt.bin.tmp", "/mqtt.bin");
    }
  }
}

void saveAppConfig() {
  File f = LittleFS.open("/app.bin.tmp", "w");
  if (f) {
    size_t written = f.write((uint8_t*)&appCfg, sizeof(AppConfig));
    f.close();
    if (written == sizeof(AppConfig)) {
      // Atomic operation: delete old config only if new is written completely
      LittleFS.remove("/app.bin");
      LittleFS.rename("/app.bin.tmp", "/app.bin");
    }
  }
}

void loadConfig() {
  if (!LittleFS.begin()) {
    if (LittleFS.format()) {
      LittleFS.begin();
    }
  }

  // WiFi
  if (LittleFS.exists("/wifi.bin")) {
    File f = LittleFS.open("/wifi.bin", "r");
    f.read((uint8_t*)&wifiCfg, sizeof(WifiConfig));
    f.close();
  } else {
    memset(&wifiCfg, 0, sizeof(WifiConfig));
  }

  // MQTT
  if (LittleFS.exists("/mqtt.bin")) {
    File f = LittleFS.open("/mqtt.bin", "r");
    f.read((uint8_t*)&mqttCfg, sizeof(MqttConfig));
    f.close();
  } else {
    memset(&mqttCfg, 0, sizeof(MqttConfig));
    strlcpy(mqttCfg.host, "", sizeof(mqttCfg.host));
    mqttCfg.port = 1883;
    strlcpy(mqttCfg.user, "", sizeof(mqttCfg.user));
    strlcpy(mqttCfg.pass, "", sizeof(mqttCfg.pass));
    strlcpy(mqttCfg.client_id, "nous_a5t", sizeof(mqttCfg.client_id));
    strlcpy(mqttCfg.topic, "nous", sizeof(mqttCfg.topic));
    mqttCfg.enabled = false;
    strlcpy(mqttCfg.mdns_hostname, "NOUS-A5T", sizeof(mqttCfg.mdns_hostname));
    mqttCfg.pub_interval = 1;
  }
  if (strlen(mqttCfg.mdns_hostname) == 0) strlcpy(mqttCfg.mdns_hostname, "NOUS-A5T", sizeof(mqttCfg.mdns_hostname));
  if (mqttCfg.pub_interval < 1) mqttCfg.pub_interval = 1;
  if (mqttCfg.pub_interval > 300) mqttCfg.pub_interval = 300; // Capped at 5 minutes

  // App
  if (LittleFS.exists("/app.bin")) {
    File f = LittleFS.open("/app.bin", "r");
    f.read((uint8_t*)&appCfg, sizeof(AppConfig));
    f.close();
  } else {
    appCfg.magic = CONFIG_MAGIC;
    for(int i=0; i<RELAY_COUNT; i++) appCfg.relay_state[i] = false;
    appCfg.child_lock = false;
    appCfg.power_on_behavior = 2; // Default: Previous

    // DEFAULT CALIBRATION VALUES
    appCfg.cal_voltage = DEFAULT_CAL_V; 
    appCfg.cal_current = DEFAULT_CAL_I; 
    appCfg.cal_power = DEFAULT_CAL_P;   
  }
  
  // Validate configuration
  if (appCfg.magic != CONFIG_MAGIC) {
    // If structure changed, reset to safe values
    appCfg.magic = CONFIG_MAGIC;
    appCfg.cal_voltage = DEFAULT_CAL_V;
    appCfg.cal_current = DEFAULT_CAL_I;
    appCfg.cal_power = DEFAULT_CAL_P;
    appCfg.power_on_behavior = 2;
    
    // Default Security
    appCfg.auth_config = false;
    strlcpy(appCfg.user_config, "admin", sizeof(appCfg.user_config));
    strlcpy(appCfg.pass_config, "admin", sizeof(appCfg.pass_config));
    appCfg.auth_root = false;
    strlcpy(appCfg.user_root, "admin", sizeof(appCfg.user_root));
    strlcpy(appCfg.pass_root, "admin", sizeof(appCfg.pass_root));
    strlcpy(appCfg.ota_pass, "admin", sizeof(appCfg.ota_pass));
    appCfg.mask_wifi = false;
    appCfg.mask_mqtt = true;
    appCfg.mask_auth_root = true;
    appCfg.mask_auth_config = true;
    
    saveAppConfig();
  }

  if (isnan(appCfg.cal_voltage) || appCfg.cal_voltage < 0.01) appCfg.cal_voltage = DEFAULT_CAL_V;
  if (isnan(appCfg.cal_current) || appCfg.cal_current < 0.01) appCfg.cal_current = DEFAULT_CAL_I;
  if (isnan(appCfg.cal_power) || appCfg.cal_power < 0.01) appCfg.cal_power = DEFAULT_CAL_P;
}

// ================= HARDWARE CONTROL =================
void setRelay(int relayIdx, bool state) {
  if (relayIdx < 0 || relayIdx >= RELAY_COUNT) return;
  
  bool pinState = state;
  if (RELAY_INVERTED[relayIdx]) {
    pinState = !state;
  }

  digitalWrite(RELAY_PINS[relayIdx], pinState ? HIGH : LOW);
  appCfg.relay_state[relayIdx] = state;
  mqttPublishState(relayIdx);
  configDirty = true;
  lastChangeTime = millis();
}

void toggleAll() {
  // Logic: If any is OFF -> All ON. If all are ON -> All OFF.
  bool anyOff = false;
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!appCfg.relay_state[i]) {
      anyOff = true;
      break;
    }
  }

  bool newState = anyOff; // true (ON) if we found one turned off
  for (int i = 0; i < RELAY_COUNT; i++) {
    setRelay(i, newState);
  }
}

// ================= WIFI =================
bool connectWiFi() {
  if (strlen(wifiCfg.ssid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // Critical for ESP8285
  
  WiFi.hostname(mqttCfg.mdns_hostname);
  WiFi.begin(wifiCfg.ssid, wifiCfg.pass);

  unsigned long start = millis();
  unsigned long lastBlink = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    handleButtons(); // Allow button operation during connection
    
    if (millis() - lastBlink > 250) {
      lastBlink = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Blink LED
    }
    yield();
  }
  digitalWrite(LED_PIN, HIGH); // LED OFF (Inverted)
  return WiFi.status() == WL_CONNECTED;
}

// ================= MQTT =================
void mqttPublishState(int relayIdx) {
  if (mqtt.connected() && relayIdx >= 0 && relayIdx < RELAY_COUNT) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/relay/%d", mqttCfg.topic, relayIdx);
    mqtt.publish(topic, appCfg.relay_state[relayIdx] ? "ON" : "OFF");
  }
}

void mqttPublishEnergy() {
  // Capture raw sums and counts from the last second
  double temp_raw_v_sum_1s = raw_v_sum_1s;
  double temp_raw_i_sum_1s = raw_i_sum_1s;
  double temp_raw_p_sum_1s = raw_p_sum_1s;
  int temp_raw_count_1s = raw_count_1s;

  // Reset 1s accumulators
  raw_v_sum_1s = 0; raw_i_sum_1s = 0; raw_p_sum_1s = 0; raw_count_1s = 0;

  // Calculate 1s average
  double avg_v_1s = 0.0, avg_i_1s = 0.0, avg_p_1s = 0.0;
  if (temp_raw_count_1s > 0) {
    avg_v_1s = temp_raw_v_sum_1s / temp_raw_count_1s;
    avg_i_1s = temp_raw_i_sum_1s / temp_raw_count_1s;
    avg_p_1s = temp_raw_p_sum_1s / temp_raw_count_1s;
    // Update decision variables
    cse_voltage = avg_v_1s * appCfg.cal_voltage;
    cse_current = avg_i_1s * appCfg.cal_current;
    cse_power   = avg_p_1s * appCfg.cal_power;
  }

  // Calculate calibrated 1s power to decide reporting mode
  double calculated_p_1s = avg_p_1s * appCfg.cal_power;
  
  // Apply offset/deadband for low power reporting decision
  if (calculated_p_1s < LOW_POWER_THRESHOLD) {
      calculated_p_1s -= P_OFFSET;
      if (calculated_p_1s < P_DEADBAND) calculated_p_1s = 0.0;
  }

  // If power is low, wait for enough samples before reporting
  if (calculated_p_1s < LOW_POWER_THRESHOLD && pCount < MAX_P_SAMPLES) return;

  // 1. Accumulate raw data into MQTT collector
  mqtt_raw_v_sum += temp_raw_v_sum_1s;
  mqtt_raw_i_sum += temp_raw_i_sum_1s;
  mqtt_raw_p_sum += temp_raw_p_sum_1s;
  mqtt_raw_count += temp_raw_count_1s;

  // Calculate target reporting interval
  unsigned long interval_ms = (unsigned long)mqttCfg.pub_interval * 1000;
  if (calculated_p_1s < LOW_POWER_THRESHOLD) {
    // If power is below 15W, reporting time is AT LEAST 15s.
    if (interval_ms < 15000) interval_ms = 15000;
  }

  // 2. Check if reporting time has passed
  if (millis() - last_mqtt_report_time >= interval_ms) {
    // Calculate raw averages
    double avg_v = mqtt_raw_v_sum / mqtt_raw_count;
    double avg_i = mqtt_raw_i_sum / mqtt_raw_count;
    double avg_p = mqtt_raw_p_sum / mqtt_raw_count;

    double pub_power = 0;
    if (calculated_p_1s < LOW_POWER_THRESHOLD) {
      // Low power: Apply Median + Trimmed Mean algorithm on sample buffer
      pub_power = (double)(getMedian(pBuffer, pCount) + getTrimmedMean(pBuffer, pCount)) * 0.5;
    } else {
      // High power: Arithmetic mean over interval
      pub_power = avg_p * appCfg.cal_power;
    }

    // Reset MQTT session accumulators and low power buffer
    mqtt_raw_v_sum = 0; mqtt_raw_i_sum = 0; mqtt_raw_p_sum = 0; mqtt_raw_count = 0;
    pCount = 0; 
    last_mqtt_report_time = millis();

    // Apply calibration factors to calculated MEAN
    double pub_voltage = avg_v * appCfg.cal_voltage;
    double pub_current = avg_i * appCfg.cal_current;
    double pub_pf      = 0.0;

    // Apply noise filters (noise floor & suppression)
    if (pub_current < 0.05f) pub_current *= 0.25f;
    if (pub_power < 3.0f && calculated_p_1s >= LOW_POWER_THRESHOLD) pub_power *= 0.40f; 

    // Power Factor (PF) calculation based on averages
    double apparent = pub_voltage * pub_current;
    if (apparent > 0.5) {
      pub_pf = pub_power / apparent;
      if (pub_pf > 1.0) pub_pf = 1.0;
      else if (pub_pf < 0.0) pub_pf = 0.0;
    } else {
      pub_pf = 1.0;
    }

    // Final noise threshold for negligible values
    if (pub_power < 0.1) {
      pub_power = 0.0; pub_current = 0.0; pub_pf = 0.0;
    }

    // Publish average results to MQTT
    if (mqtt.connected()) {
      char topic[64];
      char val[16];
      
      snprintf(topic, sizeof(topic), "%s/voltage", mqttCfg.topic);
      dtostrf(pub_voltage, 1, 1, val); mqtt.publish(topic, val);
      
      snprintf(topic, sizeof(topic), "%s/current", mqttCfg.topic);
      dtostrf(pub_current, 1, 3, val); mqtt.publish(topic, val);
      
      snprintf(topic, sizeof(topic), "%s/power", mqttCfg.topic);
      dtostrf(pub_power, 1, 1, val); mqtt.publish(topic, val);
      
      snprintf(topic, sizeof(topic), "%s/pf", mqttCfg.topic);
      dtostrf(pub_pf, 1, 2, val); mqtt.publish(topic, val);

      // Send average raw data for debugging
      char rawBuf[128];
      snprintf(topic, sizeof(topic), "%s/raw", mqttCfg.topic);
      snprintf(rawBuf, sizeof(rawBuf), "AvgV: %.2f | AvgI: %.4f | AvgP: %.2f (Interval: %lus)", avg_v, avg_i, avg_p, interval_ms/1000);
      mqtt.publish(topic, rawBuf);
    }
  }
}

void mqttPublishAll() {
  for(int i=0; i<RELAY_COUNT; i++) mqttPublishState(i);
  mqttPublishEnergy();
  if (mqtt.connected()) {
    char topic[64];
    char val[16];
    snprintf(topic, sizeof(topic), "%s/stats/wifi", mqttCfg.topic);
    ltoa(wifiConnectCount, val, 10); mqtt.publish(topic, val);
    
    snprintf(topic, sizeof(topic), "%s/stats/mqtt", mqttCfg.topic);
    ltoa(mqttConnectCount, val, 10); mqtt.publish(topic, val);
    
    snprintf(topic, sizeof(topic), "%s/child_lock", mqttCfg.topic);
    mqtt.publish(topic, appCfg.child_lock ? "ON" : "OFF");
    
    snprintf(topic, sizeof(topic), "%s/power_on_behavior", mqttCfg.topic);
    mqtt.publish(topic, appCfg.power_on_behavior == 0 ? "OFF" : (appCfg.power_on_behavior == 1 ? "ON" : "PREVIOUS"));
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Allocate message buffer on the stack
  char msg[length + 1];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  size_t baseLen = strlen(mqttCfg.topic);
  
  // Check if the message is from Home Assistant
  if (strcmp(topic, "homeassistant/status") == 0 && strcasecmp(msg, "online") == 0) {
    sendDiscoveryMessages();
    return;
  }

  // Check if the topic starts with our prefix
  if (strncmp(topic, mqttCfg.topic, baseLen) != 0) return;

  // Pointer to the part after the base topic (e.g., /relay/0/set)
  const char* sub = topic + baseLen;

  if (strncmp(sub, "/relay/", 7) == 0) {
    const char* end = sub + strlen(sub);
    if (strcmp(end - 4, "/set") == 0) {
      int relayIdx = atoi(sub + 7);
      if (relayIdx >= 0 && relayIdx < RELAY_COUNT) {
        bool newState = (strcmp(msg, "1") == 0 || strcasecmp(msg, "ON") == 0 || strcasecmp(msg, "TRUE") == 0);
        setRelay(relayIdx, newState);
      }
    } else if (strcmp(sub, "/relay/all/set") == 0) {
      bool newState = (strcmp(msg, "1") == 0 || strcasecmp(msg, "ON") == 0 || strcasecmp(msg, "TRUE") == 0);
      for(int i=0; i<RELAY_COUNT; i++) setRelay(i, newState);
    }
  } else if (strcmp(sub, "/stats/reset") == 0) {
    wifiConnectCount = 0; mqttConnectCount = 0;
    mqttPublishAll();
  } else if (strcmp(sub, "/child_lock/set") == 0) {
    appCfg.child_lock = (strcmp(msg, "1") == 0 || strcasecmp(msg, "ON") == 0 || strcasecmp(msg, "TRUE") == 0);
    configDirty = true; lastChangeTime = millis();
    mqttPublishAll();
  } else if (strcmp(sub, "/power_on_behavior/set") == 0) {
    if (strcasecmp(msg, "OFF") == 0 || strcmp(msg, "0") == 0) appCfg.power_on_behavior = 0;
    else if (strcasecmp(msg, "ON") == 0 || strcmp(msg, "1") == 0) appCfg.power_on_behavior = 1;
    else if (strcasecmp(msg, "PREVIOUS") == 0 || strcasecmp(msg, "RESTORE") == 0 || strcmp(msg, "2") == 0) appCfg.power_on_behavior = 2;
    configDirty = true; lastChangeTime = millis();
    mqttPublishAll();
  }
}

void sendDiscoveryMessages() {
  if (!mqtt.connected()) return;

  // Obtain MAC address once as a fixed string
  char mac[13];
  uint8_t m[6];
  WiFi.macAddress(m);
  snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);

  // Reuse two String buffers to avoid repeated heap allocations
  String topic;
  String payload;
  topic.reserve(128);
  payload.reserve(768);

  // Helper to add the Device Info fragment (saves RAM)
  auto appendDeviceConfig = [&]() {
    payload += F(",\"dev\":{\"ids\":[\"");
    payload += mac;
    payload += F("\"],\"name\":\"");
    payload += mqttCfg.mdns_hostname;
    payload += F("\",\"mdl\":\"NOUS A5T\",\"mf\":\"NOUS\",\"sw\":\"");
    payload += FW_VERSION;
    payload += F("\"}}");
  };

  // 1. Relays (Switches)
  for (int i = 0; i < RELAY_COUNT; i++) {
    topic = F("homeassistant/switch/"); topic += mac; topic += F("/relay_"); topic += i; topic += F("/config");

    payload = F("{\"name\":\"");
    if (i == 3) payload += F("USB"); else { payload += F("Socket "); payload += (i + 1); }
    payload += F("\",\"uniq_id\":\""); payload += mac; payload += F("_relay_"); payload += i;
    payload += F("\",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/relay/"); payload += i;
    payload += F("\",\"cmd_t\":\""); payload += mqttCfg.topic; payload += F("/relay/"); payload += i;
    payload += F("/set\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",");
    payload += F("\"icon\":\""); payload += (i == 3 ? F("mdi:usb-port") : F("mdi:power-socket-eu"));
    payload += F("\"");
    appendDeviceConfig();
    payload += F("}");

    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }

  // 2. Child Lock (Switch)
  {
    topic = F("homeassistant/switch/"); topic += mac; topic += F("/child_lock/config");
    payload = F("{\"name\":\"Child Lock\",\"uniq_id\":\""); payload += mac; 
    payload += F("_child_lock\",\"stat_t\":\""); payload += mqttCfg.topic; 
    payload += F("/child_lock\",\"cmd_t\":\""); payload += mqttCfg.topic; 
    payload += F("/child_lock/set\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"icon\":\"mdi:lock\"");
    appendDeviceConfig();
    payload += F("}");
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }

  // 3. Power ON Behavior (Select)
  {
    topic = F("homeassistant/select/"); topic += mac; topic += F("/power_on/config");
    payload = F("{\"name\":\"Power On Behavior\",\"uniq_id\":\""); payload += mac; 
    payload += F("_power_on\",\"stat_t\":\""); payload += mqttCfg.topic; 
    payload += F("/power_on_behavior\",\"cmd_t\":\""); payload += mqttCfg.topic; 
    payload += F("/power_on_behavior/set\",\"options\":[\"OFF\",\"ON\",\"PREVIOUS\"],\"icon\":\"mdi:power-settings\"");
    appendDeviceConfig();
    payload += F("}");
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }

  // 4. Sensors
  const char* sensorIds[] = {"voltage", "current", "power"};
  const char* sensorNames[] = {"Voltage", "Current", "Power"};
  const char* units[] = {"V", "A", "W"};
  const char* classes[] = {"voltage", "current", "power"};
  const char* icons[] = {"mdi:flash", "mdi:current-ac", "mdi:gauge"};
  
  for (int i = 0; i < 3; i++) {
    topic = F("homeassistant/sensor/"); topic += mac; topic += F("/"); topic += sensorIds[i]; topic += F("/config");
    payload = F("{\"name\":\""); payload += sensorNames[i];
    payload += F("\",\"uniq_id\":\""); payload += mac; payload += F("_"); payload += sensorIds[i];
    payload += F("\",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/"); payload += sensorIds[i];
    payload += F("\",\"unit_of_meas\":\""); payload += units[i];
    payload += F("\",\"dev_cla\":\""); payload += classes[i];
    payload += F("\",\"stat_cla\":\"measurement\",\"icon\":\""); payload += icons[i];
    payload += F("\"");
    appendDeviceConfig();
    payload += F("}");
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }
  
  // 5. Power Factor Sensor
  {
    topic = F("homeassistant/sensor/"); topic += mac; topic += F("/pf/config");
    payload = F("{\"name\":\"Power Factor\",\"uniq_id\":\""); payload += mac;
    payload += F("_pf\",\"stat_t\":\""); payload += mqttCfg.topic;
    payload += F("/pf\",\"unit_of_meas\":\"\",\"icon\":\"mdi:angle-acute\"");
    appendDeviceConfig();
    payload += F("}");
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }

  // 6. Raw Data Sensor
  {
    topic = F("homeassistant/sensor/"); topic += mac; topic += F("/raw/config");
    payload = F("{\"name\":\"Raw Data\",\"uniq_id\":\""); payload += mac;
    payload += F("_raw\",\"stat_t\":\""); payload += mqttCfg.topic;
    payload += F("/raw\",\"icon\":\"mdi:code-braces\"");
    appendDeviceConfig();
    payload += F("}");
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }
}

void mqttConnect() {
  if (!mqttCfg.enabled) return;
  if (mqtt.connected()) return;

  static unsigned long lastMqttRetry = 0;
  if (millis() - lastMqttRetry < MQTT_RECONNECT_INTERVAL) return;
  lastMqttRetry = millis();

  mqtt.setServer(mqttCfg.host, mqttCfg.port);
  
  bool connected = false;
  if (strlen(mqttCfg.user) > 0) {
    connected = mqtt.connect(mqttCfg.client_id, mqttCfg.user, mqttCfg.pass);
  } else {
    connected = mqtt.connect(mqttCfg.client_id);
  }

  if (connected) {
    mqttConnectCount++;
    char subTopic[64];
    // Subscribe to individual topics
    for(int i=0; i<RELAY_COUNT; i++) {
      snprintf(subTopic, sizeof(subTopic), "%s/relay/%d/set", mqttCfg.topic, i);
      mqtt.subscribe(subTopic);
    }
    snprintf(subTopic, sizeof(subTopic), "%s/relay/all/set", mqttCfg.topic); mqtt.subscribe(subTopic);
    snprintf(subTopic, sizeof(subTopic), "%s/stats/reset", mqttCfg.topic); mqtt.subscribe(subTopic);
    snprintf(subTopic, sizeof(subTopic), "%s/child_lock/set", mqttCfg.topic); mqtt.subscribe(subTopic);
    snprintf(subTopic, sizeof(subTopic), "%s/power_on_behavior/set", mqttCfg.topic); mqtt.subscribe(subTopic);

    mqtt.subscribe("homeassistant/status"); // Listen for Home Assistant status
    mqttPublishAll();
    sendDiscoveryMessages();
  }
}

// ================= CSE7766 DRIVER =================
void handleCSE7766() {
  static unsigned long lastByteMs = 0;
  if (cse_idx > 0 && millis() - lastByteMs > 50) {
    cse_idx = 0; // Reset buffer on inter-char timeout
  }

  while (cseSerial.available() > 0) {
    uint8_t c = cseSerial.read();
    lastByteMs = millis();
    
    // Header synchronization
    if (cse_idx == 0 && c != 0x55) continue;
    if (cse_idx == 1 && c != 0x5A) { 
      // If 0x55 is received instead of 0x5A, it might be a new packet start
      if (c == 0x55) cse_idx = 1; 
      else cse_idx = 0; 
      continue; 
    }

    if (cse_idx < sizeof(cse_buff)) {
        cse_buff[cse_idx++] = c;
    } else {
        cse_idx = 0; // Reset safe if overflow
    }
    
    if (cse_idx >= 24) {
      // Checksum verification
      uint8_t sum = 0;
      for (int i = 2; i < 23; i++) sum += cse_buff[i]; // Correction: Sum starts from byte 2
      
      if (sum == cse_buff[23]) {
        unsigned long v_coeff = ((unsigned long)cse_buff[2] << 16) | ((unsigned long)cse_buff[3] << 8) | cse_buff[4];
        unsigned long v_cycle = ((unsigned long)cse_buff[5] << 16) | ((unsigned long)cse_buff[6] << 8) | cse_buff[7];
        unsigned long i_coeff = ((unsigned long)cse_buff[8] << 16) | ((unsigned long)cse_buff[9] << 8) | cse_buff[10];
        unsigned long i_cycle = ((unsigned long)cse_buff[11] << 16) | ((unsigned long)cse_buff[12] << 8) | cse_buff[13];
        unsigned long p_coeff = ((unsigned long)cse_buff[14] << 16) | ((unsigned long)cse_buff[15] << 8) | cse_buff[16];
        unsigned long p_cycle = ((unsigned long)cse_buff[17] << 16) | ((unsigned long)cse_buff[18] << 8) | cse_buff[19];
        uint8_t adj = cse_buff[20];

        // Calculate instantaneous RAW values
        double t_raw_v = (v_cycle > 0) ? (double)v_coeff / v_cycle : 0.0;
        double t_raw_i = (i_cycle > 0) ? (double)i_coeff / i_cycle : 0.0;
        double t_raw_p = (p_cycle > 0) ? (double)p_coeff / p_cycle : 0.0;
        double t_power = t_raw_p * appCfg.cal_power;

        // ERROR FILTER: Ignore impossible readings
        if (t_raw_p > 5000.0 || t_power > 10000.0) {
          cse_idx = 0;
          return;
        }

        cse_raw_v = t_raw_v;
        cse_raw_i = t_raw_i;
        cse_raw_p = t_raw_p;
        cse_power = t_power;

        // Accumulate valid raw data for 1s average
        raw_v_sum_1s += t_raw_v;
        raw_i_sum_1s += t_raw_i;
        raw_p_sum_1s += t_raw_p;
        raw_count_1s++;

        cse_voltage = t_raw_v * appCfg.cal_voltage;
        cse_current = t_raw_i * appCfg.cal_current;
        
        // Ingest sample into buffer
        ingestPowerSample(t_raw_p, t_power, cse_current);
        
        // Instant filter for UI (User Interface)
        cse_power = ema_power;

        if (cse_current < 0.05f) cse_current *= 0.25f;
        
        // Noise correction and Power Factor calculation
        if (cse_power < 0.1) { 
          cse_power = 0.0;
          cse_current = 0.0;
          cse_pf = 0.0;
        } else {
          double apparentPower = cse_voltage * cse_current;
          if (apparentPower > 0.5) { // 0.5 VA threshold for PF stability
            cse_pf = cse_power / apparentPower;
            if (cse_pf > 1.0) cse_pf = 1.0;
            else if (cse_pf < 0.0) cse_pf = 0.0;
          } else {
            cse_pf = 1.0; // Small resistive load or undeterminable
          }
        }
        
        // Accumulate for calibration after 1s stabilization
        if (isCalibrating && (millis() - calStartTime > CAL_STABILIZE_DELAY) && v_cycle > 0 && i_cycle > 0 && p_cycle > 0) {
          sumRawV += (double)v_coeff / v_cycle;
          sumRawI += (double)i_coeff / i_cycle;
          sumRawP += (double)p_coeff / p_cycle;
          calSamples++;
        }

        lastCsePacketTime = millis();

        // Hardware Over-Power Protection
        if (t_power > MAX_POWER_LIMIT) {
          for(int i=0; i<RELAY_COUNT; i++) setRelay(i, false);
          calStatusMsg = F("ALARM: POWER LIMIT EXCEEDED!");
          mqttPublishAll();
        }
      }
      cse_idx = 0;
    }
  }
}

// ================= WEB PAGES =================
String renderPage(const String& title, const String& content) {
  String html;
  html.reserve(content.length() + 1500);
  html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
  html += "<title>" + title + "</title>";
  html += F("<style>");
  html += F("body{font-family:Arial,sans-serif;margin:0 auto;padding:20px;max-width:1000px;background:#f4f4f9;}");
  html += F("h2{text-align:center;color:#333;}");
  html += F("input{width:100%;padding:10px;margin:5px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}");
  html += F("button, .btn{display:block;width:100%;background-color:#4CAF50;color:white;padding:12px 0;border:none;border-radius:4px;cursor:pointer;margin-top:10px;font-size:16px;text-align:center;text-decoration:none;box-sizing:border-box;}");
  html += F("button:hover, .btn:hover{background-color:#45a049;}");
  html += F("button.secondary, .btn.secondary{background-color:#2196F3;} button.secondary:hover, .btn.secondary:hover{background-color:#0b7dda;}");
  html += F("button.danger, .btn.danger{background-color:#f44336;} button.danger:hover, .btn.danger:hover{background-color:#da190b;}");
  html += F("button.off, .btn.off{background-color:#777;}");
  html += F(".card{background:#fff;padding:15px;border-radius:5px;border:1px solid #ddd;margin-bottom:15px;display:flex;align-items:center;justify-content:space-between;}");
  html += F(".info{background:#fff;padding:15px;border-radius:5px;border:1px solid #ddd;margin-bottom:15px;}");
  html += F("a{text-decoration:none;color:#2196F3;}");
  html += F(".result{margin-top:10px; font-weight:bold;}");
  html += F(".row{display:flex;flex-wrap:wrap;gap:20px;}");
  html += F(".col{flex:1;min-width:300px;}");
  html += F("</style></head><body>");
  html += content;
  html += "</body></html>";
  return html;
}

void pageStatus() {
  if (!checkAuth(appCfg.auth_root, appCfg.user_root, appCfg.pass_root)) return;
  String content;
  content.reserve(3000);
  content = F("<h2>NOUS A5T Control (v");
  content += FW_VERSION;
  content += F(")</h2>");
  
  // Flex container for buttons
  content += F("<div style='display:flex; flex-wrap:wrap; gap:10px; margin-bottom:20px;'>");
  for(int i=0; i<RELAY_COUNT; i++) {
    String name = (i == 3) ? "USB" : ("Socket " + String(i+1));
    String btnColor = appCfg.relay_state[i] ? "" : "off";
    String btnText = appCfg.relay_state[i] ? "ON" : "OFF";
    
    content += F("<div style='flex:1 1 0px; background:#fff; padding:10px; border:1px solid #ddd; border-radius:5px; text-align:center;'><b>");
    content += name;
    content += F("</b><br><a href='/toggle?id=");
    content += i;
    content += F("' id='btnRelay");
    content += i;
    content += F("' class='btn ");
    content += btnColor;
    content += F("'>");
    content += btnText;
    content += F("</a></div>");
  }

  // Child Lock button
  String clBtnColor = appCfg.child_lock ? "" : "off"; // ON = Active, OFF = Inactive
  content += F("<div style='flex:1 1 0px; background:#fff; padding:10px; border:1px solid #ddd; border-radius:5px; text-align:center;'>");
  content += "<b>Child Lock</b><br>";
  content += F("<a href='/toggle_child_lock' id='btnCL' class='btn ");
  content += clBtnColor;
  content += F("'>");
  content += (appCfg.child_lock ? "ON" : "OFF");
  content += F("</a></div>");
  content += "</div>"; // End Flex Container

  // Row: Power on Behavior
  content += F("<div class='card' style='display:block;'><b style='display:block;margin-bottom:10px;text-align:center;'>Power-on Behavior</b>");
  content += F("<div style='display:flex; gap:10px;'>");
  
  String pob_off = (appCfg.power_on_behavior == 0) ? "" : "off";
  String pob_on = (appCfg.power_on_behavior == 1) ? "" : "off";
  String pob_prev = (appCfg.power_on_behavior == 2) ? "" : "off";

  content += F("<a href='/set_pob?val=0' style='flex:1' class='btn "); content += pob_off; content += F("'>OFF</a>");
  content += F("<a href='/set_pob?val=1' style='flex:1' class='btn "); content += pob_on; content += F("'>ON</a>");
  content += F("<a href='/set_pob?val=2' style='flex:1' class='btn "); content += pob_prev; content += F("'>PREVIOUS</a>");
  
  content += F("</div></div>");

  content += F("<div class='row'>");

  // 1. Consumption
  content += F("<div class='col'><fieldset><legend>Instant Consumption (Unfiltered)</legend>");
  content += F("<b>Voltage:</b> <span id='volt'>"); content += cse_voltage; content += F("</span> V<br>");
  content += F("<b>Current:</b> <span id='curr'>"); content += cse_current; content += F("</span> A<br>");
  content += F("<b>Power:</b> <span id='pwr'>"); content += cse_power; content += F("</span> W<br>");
  content += F("<b>Power Factor:</b> <span id='pf'>"); content += String(cse_pf, 2); content += F("</span><br>");
  content += F("</fieldset></div>");

  // 2. WiFi
  content += F("<div class='col'><fieldset><legend>System & WiFi</legend>");
  content += F("<b>Status:</b> "); 
  content += (WiFi.status() == WL_CONNECTED ? F("<span style='color:green'>Connected</span>") : F("<span style='color:red'>Disconnected</span>"));
  content += F("<br><b>Connections:</b> <span id='wifiCon'>"); content += wifiConnectCount; content += F("</span>");
  content += F("<br><div style='display:flex;align-items:center;gap:10px;margin-top:5px;'><b>Disconnects:</b> <span id='wifiDis'>");
  content += wifiDisconnectCount;
  content += F("</span><form action='/reset_stats' method='POST' style='margin:0;'><input type='hidden' name='type' value='wifi'><button type='submit' class='secondary' style='width:auto;padding:2px 8px;font-size:12px;margin:0;'>Reset</button></form></div>");
  content += F("<b>IP:</b> "); content += WiFi.localIP().toString();
  content += F("<br><b>Chip ID:</b> "); content += String(ESP.getChipId(), HEX);
  content += F("<br><b>Flash:</b> "); content += (ESP.getFlashChipRealSize() / 1024);
  content += F(" KB<br><b>Free RAM:</b> <span id='freeRam'>"); content += String((float)(ESP.getFreeHeap() / 1024.0), 1);
  content += F(" KB<br>");
  content += "</fieldset></div>";

  // 3. MQTT
  content += "<div class='col'>";
  content += "<fieldset><legend>MQTT</legend>";
  content += F("<b>Status:</b> ");
  content += (mqtt.connected() ? F("<span style='color:green'>Connected</span>") : F("<span style='color:red'>Disconnected</span>"));
  content += F("<br><b>Connections:</b> <span id='mqttCon'>"); content += mqttConnectCount; content += F("</span>");
  content += F("<br><div style='display:flex;align-items:center;gap:10px;margin-top:5px;'><b>Disconnects:</b> <span id='mqttDis'>");
  content += mqttDisconnectCount;
  content += F("</span><form action='/reset_stats' method='POST' style='margin:0;'><input type='hidden' name='type' value='mqtt'><button type='submit' class='secondary' style='width:auto;padding:2px 8px;font-size:12px;margin:0;'>Reset</button></form></div>");
  content += F("<b>Mode:</b> "); content += (mqttCfg.enabled ? F("Active") : F("Inactive"));
  content += "</fieldset></div>";

  content += "</div>";
  
  content += F("<script>setInterval(function(){fetch('/api/sensors').then(r=>r.text()).then(v=>{");
  content += F("var p=v.split('|');");
  content += F("document.getElementById('volt').innerText=p[0];");
  content += F("document.getElementById('curr').innerText=p[1];");
  content += F("document.getElementById('pwr').innerText=p[2];");
  content += F("document.getElementById('pf').innerText=p[3];");
  content += F("for(var i=0;i<4;i++){");
  content += F("var b=document.getElementById('btnRelay'+i);");
  content += F("if(b){var s=p[13+i]==='1';b.innerText=s?'ON':'OFF';");
  content += F("if(s)b.classList.remove('off');else b.classList.add('off');}}");
  content += F("var cl=document.getElementById('btnCL');");
  content += F("if(cl){var s=p[17]==='1';cl.innerText=s?'ON':'OFF';");
  content += F("if(s)cl.classList.remove('off');else cl.classList.add('off');}");
  content += F("document.getElementById('freeRam').innerText=p[18];");
  content += F("document.getElementById('wifiCon').innerText=p[19]; ");
  content += F("document.getElementById('wifiDis').innerText=p[20]; ");
  content += F("document.getElementById('mqttCon').innerText=p[21]; ");
  content += F("document.getElementById('mqttDis').innerText=p[22]; ");
  content += F("});},2000);</script>");

  content += "<a href='/config'><button class='secondary'>Configuration</button></a>";
  server.send(200, "text/html", renderPage("Status", content));
}

void handleToggle() {
  if (!checkAuth(appCfg.auth_root, appCfg.user_root, appCfg.pass_root)) return;
  String idArg = server.arg("id");
  if (idArg.length() > 0) {
    int id = idArg.toInt();
    setRelay(id, !appCfg.relay_state[id]);
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleToggleChildLock() {
  if (!checkAuth(appCfg.auth_root, appCfg.user_root, appCfg.pass_root)) return;
  appCfg.child_lock = !appCfg.child_lock;
  configDirty = true;
  lastChangeTime = millis();
  mqttPublishAll();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleSetPob() {
  if (!checkAuth(appCfg.auth_root, appCfg.user_root, appCfg.pass_root)) return;
  if (server.hasArg("val")) {
    appCfg.power_on_behavior = server.arg("val").toInt();
    configDirty = true;
    lastChangeTime = millis();
    mqttPublishAll();
  }
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleApiSensors() {
  if (appCfg.auth_root && !server.authenticate(appCfg.user_root, appCfg.pass_root)) return; // AJAX silent check
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%.1f|%.3f|%.1f|%.2f|%s|%d|%d|%.6f|%.6f|%.6f|%.2f|%.4f|%.2f|%d|%d|%d|%d|%d|%.1f|%lu|%lu|%lu|%lu",
    cse_voltage, cse_current, cse_power, cse_pf, 
    calStatusMsg.c_str(), calSamples, isCalibrating ? 1 : 0,
    appCfg.cal_voltage, appCfg.cal_current, appCfg.cal_power,
    cse_raw_v, cse_raw_i, cse_raw_p,
    appCfg.relay_state[0], appCfg.relay_state[1], appCfg.relay_state[2], appCfg.relay_state[3],
    appCfg.child_lock ? 1 : 0,
    (float)(ESP.getFreeHeap() / 1024.0),
    wifiConnectCount, wifiDisconnectCount,
    mqttConnectCount, mqttDisconnectCount
  );
  server.send(200, "text/plain", buffer);
}

void handleOpenHABGen() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  
  char safeId_buf[32];
  strlcpy(safeId_buf, mqttCfg.client_id, sizeof(safeId_buf));
  for (int i = 0; i < sizeof(safeId_buf) && safeId_buf[i] != '\0'; i++) {
      if (safeId_buf[i] == ' ' || safeId_buf[i] == '-') {
          safeId_buf[i] = '_';
      }
  }

  String things;
  things.reserve(1024);
  things = F("Thing mqtt:topic:"); things += safeId_buf; things += F(" \""); things += mqttCfg.mdns_hostname; things += F("\" (mqtt:broker:Broker) {\n");
  things += F("    Channels:\n");
  for(int i=0; i<RELAY_COUNT; i++) {
     things += F("        Type switch : relay"); things += i; things += F(" \"Socket "); things += (i+1); 
     things += F("\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/relay/"); things += i;
     things += F("\", commandTopic=\""); things += mqttCfg.topic; things += F("/relay/"); things += i; things += F("/set\", on=\"ON\", off=\"OFF\" ]\n");
  }
  things += F("        Type switch : child_lock \"Child Lock\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/child_lock\", commandTopic=\""); things += mqttCfg.topic; things += F("/child_lock/set\", on=\"ON\", off=\"OFF\" ]\n");
  things += F("        Type string : power_on \"Power On Behavior\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/power_on_behavior\", commandTopic=\""); things += mqttCfg.topic; things += F("/power_on_behavior/set\" ]\n");
  things += F("        Type number : voltage \"Voltage\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/voltage\" ]\n");
  things += F("        Type number : current \"Current\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/current\" ]\n");
  things += F("        Type number : power \"Power\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/power\" ]\n");
  things += F("        Type number : pf \"Power Factor\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/pf\" ]\n");
  things += F("        Type string : raw \"Raw Data\" [ stateTopic=\""); things += mqttCfg.topic; things += F("/raw\" ]\n");
  things += F("}\n");

  String items;
  items.reserve(1024);
  for(int i=0; i<RELAY_COUNT; i++) {
      items += F("Switch "); items += safeId_buf; items += F("_Relay"); items += i; items += F(" \"Socket "); items += (i+1); 
      items += F("\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":relay"); items += i; items += F("\" }\n");
  }
  items += F("Switch "); items += safeId_buf; items += F("_ChildLock \"Child Lock\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":child_lock\" }\n");
  items += F("String "); items += safeId_buf; items += F("_PowerOn \"Power On Behavior\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":power_on\" }\n");
  items += F("Number "); items += safeId_buf; items += F("_Voltage \"Voltage [%.1f V]\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":voltage\" }\n");
  items += F("Number "); items += safeId_buf; items += F("_Current \"Current [%.3f A]\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":current\" }\n");
  items += F("Number "); items += safeId_buf; items += F("_Power \"Power [%.1f W]\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":power\" }\n");
  items += F("Number "); items += safeId_buf; items += F("_PF \"Power Factor [%.2f]\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":pf\" }\n");
  items += F("String "); items += safeId_buf; items += F("_Raw \"Raw Data [%s]\" { channel=\"mqtt:topic:"); items += safeId_buf; items += F(":raw\" }\n");

  String html;
  html.reserve(things.length() + items.length() + 600);
  html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>OpenHAB Config</title>");
  html += F("<style>body{font-family:monospace;padding:20px;background:#f4f4f9;} textarea{width:100%;height:300px;background:#fff;border:1px solid #ccc;padding:10px;border-radius:4px;} h2{font-family:Arial;color:#333;} button{padding:10px 20px;background:#2196F3;color:white;border:none;border-radius:4px;cursor:pointer;}</style></head><body>");
  html += F("<h2>OpenHAB Things (mqtt.things)</h2><textarea readonly>"); html += things; html += F("</textarea>");
  html += F("<h2>OpenHAB Items (default.items)</h2><textarea readonly>"); html += items; html += F("</textarea>");
  html += F("<br><br><button onclick=\"window.location.href='/config'\">Back</button></body></html>");
  
  server.send(200, "text/html", html);
}

void handleESPHomeGen() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  
  char name_buf[32];
  strlcpy(name_buf, mqttCfg.mdns_hostname, sizeof(name_buf));
  for (int i = 0; i < sizeof(name_buf) && name_buf[i] != '\0'; i++) {
      name_buf[i] = tolower(name_buf[i]);
      if (name_buf[i] == ' ') {
          name_buf[i] = '_';
      }
  }

  String yaml;
  yaml.reserve(2048);
  yaml = F("esphome:\n  name: "); yaml += name_buf; yaml += F("\n  platform: ESP8266\n  board: esp01_1m\n\n");
  yaml += F("wifi:\n  ssid: \""); yaml += wifiCfg.ssid; yaml += F("\"\n  password: \""); yaml += wifiCfg.pass; yaml += F("\"\n\n");
  yaml += F("  ap:\n    ssid: \""); yaml += name_buf; yaml += F("_fallback\"\n    password: \"password\"\n\n");
  yaml += F("captive_portal:\n\nlogger:\n  baud_rate: 0\n\napi:\n\nota:\n\n");
  yaml += F("mqtt:\n  broker: "); yaml += mqttCfg.host; yaml += F("\n  port: "); yaml += mqttCfg.port; yaml += F("\n");
  yaml += F("  username: \""); yaml += mqttCfg.user; yaml += F("\"\n  password: \""); yaml += mqttCfg.pass; yaml += F("\"\n");
  yaml += F("  client_id: "); yaml += mqttCfg.client_id; yaml += F("\n\n");

  yaml += "uart:\n  rx_pin: GPIO3\n  tx_pin: GPIO1\n  baud_rate: 4800\n\n";
  yaml += "sensor:\n  - platform: cse7766\n    voltage:\n      name: \"Voltage\"\n    current:\n      name: \"Current\"\n    power:\n      name: \"Power\"\n";
  yaml += "    power_factor:\n      name: \"Power Factor\"\n\n";
  
  String restoreMode = (appCfg.power_on_behavior == 0) ? "ALWAYS_OFF" : (appCfg.power_on_behavior == 1 ? "ALWAYS_ON" : "RESTORE_DEFAULT_OFF");
  yaml += F("# Note: restore_mode set to "); yaml += restoreMode; yaml += F(" based on current config\n");

  yaml += "switch:\n";
  yaml += "  - platform: gpio\n    pin: GPIO14\n    name: \"Relay 1\"\n    id: relay1\n";
  yaml += "  - platform: gpio\n    pin: GPIO12\n    name: \"Relay 2\"\n    id: relay2\n";
  yaml += "  - platform: gpio\n    pin: GPIO13\n    name: \"Relay 3\"\n    id: relay3\n";
  yaml += "  - platform: gpio\n    pin:\n      number: GPIO5\n      inverted: true\n    name: \"Relay 4 (USB)\"\n    id: relay4\n\n";

  yaml += "binary_sensor:\n";
  yaml += "  - platform: gpio\n    pin: GPIO16\n    name: \"Button\"\n    on_press:\n      then:\n";
  yaml += "        - switch.toggle: relay1\n        - switch.toggle: relay2\n        - switch.toggle: relay3\n        - switch.toggle: relay4\n\n";

  yaml += "status_led:\n  pin:\n    number: GPIO2\n    inverted: true\n";

  String html;
  html.reserve(yaml.length() + 600);
  html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESPHome Config</title>");
  html += F("<style>body{font-family:monospace;padding:20px;background:#f4f4f9;} textarea{width:100%;height:500px;background:#fff;border:1px solid #ccc;padding:10px;border-radius:4px;} h2{font-family:Arial;color:#333;} button{padding:10px 20px;background:#2196F3;color:white;border:none;border-radius:4px;cursor:pointer;}</style></head><body>");
  html += F("<h2>ESPHome YAML (Migration)</h2><p>This code can be used to migrate the device to ESPHome firmware.</p><textarea readonly>");
  html += yaml; html += F("</textarea>");
  html += F("<br><br><button onclick=\"window.location.href='/config'\">Back</button></body></html>");
  
  server.send(200, "text/html", html);
}

// ... (The rest of the Configuration pages are similar to the previous project, minimally adapted)
void pageConfig() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String content;
  content.reserve(3000);
  content = "<h2>Configuration</h2>";
  
  // Dynamic testing script (AJAX)
  content += "<script>";
  content += "function testMqtt() {";
  content += "  var btn = document.getElementById('testBtn');";
  content += "  var res = document.getElementById('testResult');";
  content += "  btn.disabled = true; btn.innerHTML = 'Testing...';";
  content += "  res.innerHTML = '';";
  content += "  var formData = new FormData(document.getElementById('mqttForm'));";
  content += "  fetch('/api/test_mqtt', { method: 'POST', body: formData })";
  content += "    .then(response => response.text())";
  content += "    .then(data => {";
  content += "       res.innerHTML = data;";
  content += "       btn.disabled = false; btn.innerHTML = 'Test Connection';";
  content += "    }).catch(err => {";
  content += "       res.innerHTML = '<span style=\"color:red\">Server communication error</span>';";
  content += "       btn.disabled = false; btn.innerHTML = 'Test Connection';";
  content += "    });";
  content += "}";
  content += "</script>";

  content += "<div class='row'>"; // Start Row

  // --- LEFT COLUMN (MQTT) ---
  content += "<div class='col'>";
  content += "<fieldset><legend>MQTT Connection</legend>";
  content += "<form id='mqttForm' action='/save_connection' method='POST'>";
  content += F("Host:<br><input name='host' value='"); content += mqttCfg.host;
  content += F("'><br>Port:<br><input type='number' name='port' value='"); content += mqttCfg.port;
  content += F("'><br>Client ID:<br><input name='client_id' value='"); content += mqttCfg.client_id;
  content += F("'><br>User:<br><input name='user' value='"); content += mqttCfg.user;
  content += F("'><br>Pass:<br><input name='pass' type='");
  content += (appCfg.mask_mqtt ? F("password") : F("text"));
  content += F("' value='"); content += mqttCfg.pass; content += F("'><br>");
  
  content += "<button type='button' id='testBtn' onclick='testMqtt()' class='secondary'>Test Connection</button>";
  content += "<div id='testResult' class='result'></div><br>";
  content += "<button type='submit'>Save Connection</button>";
  content += "</form></fieldset>";

  // Additional Configs
  content += "<fieldset><legend>Additional Configs</legend>";
  content += "<form action='/save_topic' method='POST'>";
  content += F("Main Topic:<br><div style='display:flex;gap:5px;'><input name='topic' value='"); content += mqttCfg.topic; 
  content += F("'><button type='submit' style='width:auto;margin:5px 0;'>Save</button></div></form>");
  
  content += "<form action='/save_mdns' method='POST'>";
  content += F("mDNS Hostname:<br><div style='display:flex;gap:5px;'><input name='hostname' value='"); content += mqttCfg.mdns_hostname; 
  content += F("'><button type='submit' style='width:auto;margin:5px 0;'>Save</button></div></form>");

  content += "<form action='/save_interval' method='POST'>";
  content += F("MQTT Report Interval (1-300 sec):<br><div style='display:flex;gap:5px;'><input type='number' name='interval' min='1' max='300' value='"); content += mqttCfg.pub_interval; 
  content += F("'><button type='submit' style='width:auto;margin:5px 0;'>Save</button></div></form>");
  content += "</fieldset>";
  content += "</div>";

  // --- RIGHT COLUMN (Other) ---
  content += "<div class='col'>";
  
  // MQTT Control
  content += "<fieldset><legend>MQTT Control</legend>";
  content += "<form action='/save_mqtt_control' method='POST'>";
  content += F("Status: <b>");
  content += (mqttCfg.enabled ? F("<span style='color:green'>ENABLED</span>") : F("<span style='color:red'>DISABLED</span>"));
  content += F("</b><br><br>");
  content += F("<button type='submit' "); if(mqttCfg.enabled) content += F("class='danger' ");
  content += F("name='enabled' value='"); content += (mqttCfg.enabled ? '0' : '1'); content += F("'>");
  content += (mqttCfg.enabled ? F("Disable MQTT") : F("Enable MQTT")); content += F("</button>");
  content += "</form>";
  content += "<form action='/force_discovery' method='POST' style='margin-top:10px;'><button type='submit' class='secondary'>Resend HA Discovery</button></form>";
  content += F("<button onclick=\"window.location.href='/openhab'\" class='secondary' style='margin-top:10px;'>Generate OpenHAB Config</button>"); 
  content += F("<button onclick=\"window.location.href='/esphome'\" class='secondary' style='margin-top:10px;'>Generate ESPHome Config</button>");
  content += "</fieldset>";

  // Derived Topics
  content += "<fieldset><legend>Derived Topics</legend><small>";
  content += "<code>" + String(mqttCfg.topic) + "/relay/[0-3]</code> (State ON/OFF)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/relay/[0-3]/set</code> (Command)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/relay/all/set</code> (Global Command)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/voltage</code> (Voltage V)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/current</code> (Current A)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/power</code> (Power W)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/pf</code> (Power Factor)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/raw</code> (Raw values RawV/I/P)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/stats/wifi</code> (WiFi Counter)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/stats/mqtt</code> (MQTT Counter)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/stats/reset</code> (Reset Counters)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/child_lock</code> (Lock State)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/child_lock/set</code> (Lock Command)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/power_on_behavior</code> (State: OFF, ON, PREVIOUS)<br>";
  content += "<code>" + String(mqttCfg.topic) + "/power_on_behavior/set</code> (Command: 0, 1, 2 or OFF, ON, PREVIOUS)<br>";
  content += "</small></fieldset>";

  // Administration Links (Calibration, Update, Reset)
  content += "<fieldset><legend>Administration</legend>";
  content += "<a href='/security'><button class='secondary'>Security Settings</button></a>";
  content += "<a href='/calibration'><button class='secondary'>Energy Sensor Calibration</button></a>";
  content += "<form action='/update' method='GET' style='margin-top:10px;'><button type='submit' class='secondary'>Firmware Update (OTA)</button></form>";
  content += "<form action='/reset' method='POST' onsubmit='return confirm(\"Reset?\");' style='margin-top:10px;'><button class='danger'>Factory Reset</button></form>";
  content += "</fieldset>";
  
  content += "</div>"; // End Col Dreapta
  content += "</div>"; // End Row

  content += "<br><a href='/' style='display:block;text-align:center;'>&laquo; Back to Status</a>";
  
  server.send(200, "text/html", renderPage("Config", content));
}

void pageSecurity() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String content;
  content.reserve(3000);
  content = "<h2>Security Settings</h2>";
  content += "<form action='/save_security' method='POST'>";
  
  content += "<fieldset><legend>Status Page Authentication (Root)</legend>";
  content += "Enable: <input type='checkbox' name='auth_root' value='1' " + String(appCfg.auth_root ? "checked" : "") + " style='width:auto;'><br>";
  content += "Username:<br><input name='user_root' value='" + String(appCfg.user_root) + "'><br>";
  content += "Password:<br><input name='pass_root' type='" + String(appCfg.mask_auth_root ? "password" : "text") + "' value='" + String(appCfg.pass_root) + "'><br>";
  content += "</fieldset>";

  content += "<fieldset><legend>Config & Administration Authentication</legend>";
  content += "Enable: <input type='checkbox' name='auth_config' value='1' " + String(appCfg.auth_config ? "checked" : "") + " style='width:auto;'><br>";
  content += "Username:<br><input name='user_config' value='" + String(appCfg.user_config) + "'><br>";
  content += "Password:<br><input name='pass_config' type='" + String(appCfg.mask_auth_config ? "password" : "text") + "' value='" + String(appCfg.pass_config) + "'><br>";
  content += "</fieldset>";

  content += "<fieldset><legend>Passwords and Visibility</legend>";
  content += "OTA Password:<br><input name='ota_pass' value='" + String(appCfg.ota_pass) + "'><br>";
  content += "<label><input type='checkbox' name='mask_wifi' value='1' " + String(appCfg.mask_wifi ? "checked" : "") + "> Mask WiFi password in Setup</label><br>";
  content += "<label><input type='checkbox' name='mask_mqtt' value='1' " + String(appCfg.mask_mqtt ? "checked" : "") + "> Mask MQTT password in Config</label><br>";
  content += "<label><input type='checkbox' name='mask_auth_root' value='1' " + String(appCfg.mask_auth_root ? "checked" : "") + "> Mask Root password</label><br>";
  content += "<label><input type='checkbox' name='mask_auth_config' value='1' " + String(appCfg.mask_auth_config ? "checked" : "") + "> Mask Config password</label><br>";
  content += "</fieldset>";

  content += "<button type='submit'>Save Security Settings</button>";
  content += "</form>";
  content += "<br><a href='/config'><button class='secondary'>Back to Configuration</button></a>";
  
  server.send(200, "text/html", renderPage("Security", content));
}

void pageCalibration() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String content;
  content.reserve(3000);
  content = F("<h2>Energy Calibration</h2>");
  
  content += F("<div class='info' style='display:flex; justify-content:space-between; align-items:flex-start;'><div>");
  content += F("<b>Voltage:</b> <span id='volt'>"); content += String(cse_voltage, 1); content += F("</span> V<br>");
  content += F("<b>Current:</b> <span id='curr'>"); content += String(cse_current, 3); content += F("</span> A<br>");
  content += F("<b>Power:</b> <span id='pwr'>"); content += String(cse_power, 1); content += F("</span> W</div>");
  
  content += F("<div style='border-left:1px solid #ddd; padding-left:20px;'>");
  content += F("<b>Raw V:</b> <span id='raw_v'>"); content += String(appCfg.cal_voltage > 0 ? cse_voltage/appCfg.cal_voltage : 0, 2); content += F("</span><br>");
  content += F("<b>Raw I:</b> <span id='raw_i'>"); content += String(appCfg.cal_current > 0 ? cse_current/appCfg.cal_current : 0, 4); content += F("</span><br>");
  content += F("<b>Raw P:</b> <span id='raw_p'>"); content += String(appCfg.cal_power > 0 ? cse_power/appCfg.cal_power : 0, 2); content += F("</span></div>");

  content += F("<div style='border-left:1px solid #ddd; padding-left:20px;'>");
  content += F("<b>Voltage Factor:</b> <span id='f_volt'>"); content += String(appCfg.cal_voltage, 6); content += F("</span><br>");
  content += F("<b>Current Factor:</b> <span id='f_curr'>"); content += String(appCfg.cal_current, 6); content += F("</span><br>");
  content += F("<b>Power Factor:</b> <span id='f_pwr'>"); content += String(appCfg.cal_power, 6); content += F("</span></div></div>");
  
  content += F("<div id='calBox' class='info' style='display:none;'>");
  content += F("<b>Status:</b> <span id='calStatus'>"); content += calStatusMsg; content += F("</span> ");
  content += F("(<span id='calSamples'>"); content += calSamples; content += F("</span> samples collected)</div>");

  content += F("<script>setInterval(function(){fetch('/api/sensors').then(r=>r.text()).then(v=>{");
  content += F("var p=v.split('|');");
  content += F("document.getElementById('volt').innerText=p[0];");
  content += F("document.getElementById('curr').innerText=p[1];");
  content += F("document.getElementById('pwr').innerText=p[2];");
  content += F("if(p.length>12){");
  content += F("document.getElementById('raw_v').innerText=p[10];");
  content += F("document.getElementById('raw_i').innerText=p[11];");
  content += F("document.getElementById('raw_p').innerText=p[12];}");
  content += F("if(p.length>9){");
  content += F("document.getElementById('f_volt').innerText=p[7];");
  content += F("document.getElementById('f_curr').innerText=p[8];");
  content += F("document.getElementById('f_pwr').innerText=p[9];}");
  content += F("if(p.length>6){");
  content += F("document.getElementById('calStatus').innerText=p[4];");
  content += F("document.getElementById('calSamples').innerText=p[5];");
  content += F("var isCal=p[6]==='1';");
  content += F("var hasRes=p[4].indexOf('successful')!==-1;");
  content += F("var box=document.getElementById('calBox');");
  content += F("box.style.display=(isCal||hasRes)?'block':'none';");
  content += F("box.style.background=isCal?'#fff3cd':'#d4edda';}");
  content += F("});},1000);</script>");

  content += "<div class='row'><div class='col'>";
  
  content += F("<fieldset><legend>Auto Calibration</legend>");
  content += F("<p><small>Enter the real power (W). The system will use the set Voltage Factor as a fixed reference.</small></p>");
  content += F("<form action='/do_calibrate' method='POST'>");
  content += F("Real Power (W):<br><div style='display:flex;gap:5px;'><input name='target_p' placeholder='e.g., 60'><button type='submit' name='type' value='p' class='secondary' style='width:auto;margin:5px 0;'>Calibrate</button></div>");
  content += F("</form></fieldset>");
  content += "</div><div class='col'>";

  content += F("<fieldset><legend>Manual Factors</legend>");
  content += F("<form action='/reset_calibration' method='POST' onsubmit='return confirm(\"Reset factors to default values?\");'>");
  content += F("<button type='submit' class='danger'>Reset Calibration</button></form>");
  content += F("<a href='/manual_calibration'><button class='secondary' style='margin-top:10px;'>Advanced Settings</button></a>");
  content += F("</fieldset>");
  content += "</div></div>"; // End Row
  content += "<br><a href='/config'><button class='secondary'>Back to Configuration</button></a>";
  server.send(200, "text/html", renderPage("Calibration", content));
}

void pageManualCalibration() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String content;
  content.reserve(3000);
  content = F("<h2>Advanced Calibration Settings</h2>");
  
  content += F("<div class='info' style='display:flex; justify-content:space-between; align-items:flex-start;'><div>");
  content += F("<b>Voltage:</b> <span id='volt'>"); content += String(cse_voltage, 1); content += F("</span> V<br>");
  content += F("<b>Current:</b> <span id='curr'>"); content += String(cse_current, 3); content += F("</span> A<br>");
  content += F("<b>Power:</b> <span id='pwr'>"); content += String(cse_power, 1); content += F("</span> W</div>");
  content += F("<div style='border-left:1px solid #ddd; padding-left:20px;'>");
  content += F("<b>Raw V:</b> <span id='raw_v'>"); content += String(appCfg.cal_voltage > 0 ? cse_voltage/appCfg.cal_voltage : 0, 2); content += F("</span><br>");
  content += F("<b>Raw I:</b> <span id='raw_i'>"); content += String(appCfg.cal_current > 0 ? cse_current/appCfg.cal_current : 0, 4); content += F("</span><br>");
  content += F("<b>Raw P:</b> <span id='raw_p'>"); content += String(appCfg.cal_power > 0 ? cse_power/appCfg.cal_power : 0, 2); content += F("</span></div>");
  content += F("<div style='border-left:1px solid #ddd; padding-left:20px;'>");
  content += F("<b>Voltage Factor:</b> <span id='f_volt'>"); content += String(appCfg.cal_voltage, 6); content += F("</span><br>");
  content += F("<b>Current Factor:</b> <span id='f_curr'>"); content += String(appCfg.cal_current, 6); content += F("</span><br>");
  content += F("<b>Power Factor:</b> <span id='f_pwr'>"); content += String(appCfg.cal_power, 6); content += F("</span></div></div>");
  
  content += F("<div class='row'><div class='col'><fieldset><legend>Calibration via Target Values</legend>");
  content += F("<p><small>Calculate factors by providing real values.</small></p>");
  content += F("<form action='/do_calibrate' method='POST'>");
  content += F("Real Voltage (V):<br><div style='display:flex;gap:5px;'><input name='target_v' placeholder='e.g., 235'><button type='submit' name='type' value='v' class='secondary'>Set</button></div>");
  content += F("Real Current (A):<br><div style='display:flex;gap:5px;'><input name='target_c' placeholder='e.g., 0.5'><button type='submit' name='type' value='c' class='secondary'>Set</button></div>");
  content += F("Real Power (W):<br><div style='display:flex;gap:5px;'><input name='target_p_inst' placeholder='e.g., 100'><button type='submit' name='type' value='p_inst' class='secondary'>Set</button></div>");
  content += F("</form></fieldset></div>");

  content += F("<div class='col'><fieldset><legend>Manual Factors (Direct)</legend>");
  content += F("<p><small>Enter multiplication coefficients directly.</small></p>");
  content += F("<form action='/save_calibration' method='POST'>");
  content += F("Voltage Factor:<br><input name='cal_v' value='"); content += String(appCfg.cal_voltage, 6); content += F("'>");
  content += F("Current Factor:<br><input name='cal_c' value='"); content += String(appCfg.cal_current, 6); content += F("'>");
  content += F("Power Factor:<br><input name='cal_p' value='"); content += String(appCfg.cal_power, 6); content += F("'>");
  content += F("<button type='submit'>Save Factors</button></form></fieldset></div></div>");

  content += "<br><a href='/calibration'><button class='secondary'>Back</button></a>";
  
  content += F("<script>setInterval(function(){fetch('/api/sensors').then(r=>r.text()).then(v=>{");
  content += F("var p=v.split('|');");
  content += F("document.getElementById('volt').innerText=p[0];");
  content += F("document.getElementById('curr').innerText=p[1];");
  content += F("document.getElementById('pwr').innerText=p[2];");
  content += F("if(p.length>12){");
  content += F("document.getElementById('raw_v').innerText=p[10];");
  content += F("document.getElementById('raw_i').innerText=p[11];");
  content += F("document.getElementById('raw_p').innerText=p[12];}");
  content += F("if(p.length>9){");
  content += F("document.getElementById('f_volt').innerText=p[7];");
  content += F("document.getElementById('f_curr').innerText=p[8];");
  content += F("document.getElementById('f_pwr').innerText=p[9];}");
  content += F("});},1000);</script>");

  server.send(200, "text/html", renderPage("Manual Cal", content));
}

void handleDoCalibrate() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String type = server.arg("type");
  if (type == "v" && server.hasArg("target_v")) {
    float target = server.arg("target_v").toFloat();
    double rawV = cse_voltage / appCfg.cal_voltage;
    if (target > 50 && rawV > 10.0) appCfg.cal_voltage = (float)(target / rawV);
  } else if (type == "c" && server.hasArg("target_c")) {
    float target = server.arg("target_c").toFloat();
    double rawI = cse_current / appCfg.cal_current;
    if (target > 0.001 && rawI > 0.0001) appCfg.cal_current = (float)(target / rawI);
  } else if (type == "p_inst" && server.hasArg("target_p_inst")) {
    float target = server.arg("target_p_inst").toFloat();
    double rawP = cse_power / appCfg.cal_power;
    if (target > 0.1 && rawP > 0.1) appCfg.cal_power = (float)(target / rawP);
  } else if (type == "p" && server.hasArg("target_p")) {
    float target = server.arg("target_p").toFloat();
    if (target > 1.0) {
      calTargetP = target;
      sumRawV = 0; sumRawI = 0; sumRawP = 0; calSamples = 0;
      calStartTime = millis();
      isCalibrating = true;
      calStatusMsg = "Stabilizing load... Please wait.";
    }
  }
  saveAppConfig();
  // Explicit redirect back to calibration if Referer is not available
  server.sendHeader("Location", "/calibration", true);
  server.send(303);
}

void handleSaveSecurity() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  
  appCfg.auth_root = server.hasArg("auth_root");
  if (server.hasArg("user_root")) strlcpy(appCfg.user_root, server.arg("user_root").c_str(), sizeof(appCfg.user_root));
  if (server.hasArg("pass_root") && server.arg("pass_root").length() > 0) strlcpy(appCfg.pass_root, server.arg("pass_root").c_str(), sizeof(appCfg.pass_root));

  appCfg.auth_config = server.hasArg("auth_config");
  if (server.hasArg("user_config")) strlcpy(appCfg.user_config, server.arg("user_config").c_str(), sizeof(appCfg.user_config));
  if (server.hasArg("pass_config") && server.arg("pass_config").length() > 0) strlcpy(appCfg.pass_config, server.arg("pass_config").c_str(), sizeof(appCfg.pass_config));

  if (server.hasArg("ota_pass")) strlcpy(appCfg.ota_pass, server.arg("ota_pass").c_str(), sizeof(appCfg.ota_pass));
  appCfg.mask_wifi = server.hasArg("mask_wifi");
  appCfg.mask_mqtt = server.hasArg("mask_mqtt");
  appCfg.mask_auth_root = server.hasArg("mask_auth_root");
  appCfg.mask_auth_config = server.hasArg("mask_auth_config");

  saveAppConfig();
  server.sendHeader("Location", "/security");
  server.send(303);
}

void handleSaveConnection() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("host")) strlcpy(mqttCfg.host, server.arg("host").c_str(), sizeof(mqttCfg.host));
  if (server.hasArg("port")) mqttCfg.port = server.arg("port").toInt();
  if (server.hasArg("user")) strlcpy(mqttCfg.user, server.arg("user").c_str(), sizeof(mqttCfg.user));
  if (server.hasArg("pass")) strlcpy(mqttCfg.pass, server.arg("pass").c_str(), sizeof(mqttCfg.pass));
  if (server.hasArg("client_id")) strlcpy(mqttCfg.client_id, server.arg("client_id").c_str(), sizeof(mqttCfg.client_id));
  
  saveMqttConfig();
  mqtt.disconnect();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleSaveMdns() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("hostname")) {
    String newHost = server.arg("hostname");
    newHost.trim();
    if (newHost.length() > 0 && newHost.length() < 32) {
      strlcpy(mqttCfg.mdns_hostname, newHost.c_str(), sizeof(mqttCfg.mdns_hostname));
      saveMqttConfig();
    }
  }
  server.send(200, "text/html", "Saved! Rebooting... <script>setTimeout(function(){window.location.href='/config';}, 3000);</script>");
  delay(1000);
  ESP.restart();
}

void handleSaveTopic() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("topic")) strlcpy(mqttCfg.topic, server.arg("topic").c_str(), sizeof(mqttCfg.topic));
  saveMqttConfig();
  mqtt.disconnect();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleSaveInterval() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("interval")) {
    int val = server.arg("interval").toInt();
    if (val < 1) val = 1;
    if (val > 300) val = 300; // Limita extinsa la 5 minute
    mqttCfg.pub_interval = (uint16_t)val;
    saveMqttConfig();
  }
  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleSaveMqttControl() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("enabled")) {
    mqttCfg.enabled = (server.arg("enabled") == "1");
  }
  saveMqttConfig();
  if (!mqttCfg.enabled) mqtt.disconnect();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleForceDiscovery() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  sendDiscoveryMessages();
  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleResetCalibration() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  appCfg.cal_voltage = DEFAULT_CAL_V;
  appCfg.cal_current = DEFAULT_CAL_I;
  appCfg.cal_power = DEFAULT_CAL_P;
  saveAppConfig();
  server.sendHeader("Location", "/calibration");
  server.send(303);
}

void handleSaveCalibration() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  bool changed = false;
  auto validateAndSet = [&](String arg, float &target) {
    if (server.hasArg(arg)) {
      float val = server.arg(arg).toFloat();
      if (val > 0.000001f && val < 100.0f) { 
        target = val; 
        changed = true; 
      }
    }
  };

  validateAndSet("cal_v", appCfg.cal_voltage);
  validateAndSet("cal_c", appCfg.cal_current);
  validateAndSet("cal_p", appCfg.cal_power);

  if (changed) {
    saveAppConfig();
  }
  
  server.sendHeader(F("Location"), F("/calibration"));
  server.send(303);
}

void handleTestMqtt() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String t_host = server.arg("host");
  int t_port = server.arg("port").toInt();
  String t_user = server.arg("user");
  String t_pass = server.arg("pass");
  String t_client_id = server.arg("client_id");
  if (t_client_id.length() == 0) t_client_id = "nous_test";

  WiFiClient testEspClient;
  PubSubClient testClient(testEspClient);
  testClient.setServer(t_host.c_str(), t_port);
  bool res = false;
  
  if (t_user.length() > 0) res = testClient.connect(t_client_id.c_str(), t_user.c_str(), t_pass.c_str());
  else res = testClient.connect(t_client_id.c_str());
  
  testClient.disconnect();
  
  if (res) server.send(200, "text/plain", "<span style='color:green'>Connection SUCCESSFUL!</span>");
  else server.send(200, "text/plain", "<span style='color:red'>Connection FAILED!</span>");
}

void handleResetStats() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  if (server.hasArg("type")) {
    if (server.arg("type") == "wifi") wifiDisconnectCount = 0;
    if (server.arg("type") == "mqtt") mqttDisconnectCount = 0;
  }
  mqttPublishAll();
  server.sendHeader("Location", "/");
  server.send(303);
}

void cleanupUnusedFiles() {
  Dir dir = LittleFS.openDir("/");
  String filesToDelete = "";
  
  while (dir.next()) {
    String fileName = dir.fileName();
    if (fileName.length() == 0) continue;
    
    String cleanName = fileName;
    if (cleanName.startsWith("/")) cleanName = cleanName.substring(1);
    
    // Keep essential configuration files
    if (cleanName != "wifi.bin" && cleanName != "mqtt.bin" && cleanName != "app.bin") {
      filesToDelete += fileName + "\n";
    }
  }

  int p = 0;
  while (p < filesToDelete.length()) {
    int nextP = filesToDelete.indexOf('\n', p);
    if (nextP == -1) break;
    
    String fileName = filesToDelete.substring(p, nextP);
    p = nextP + 1;
    
    String path = fileName;
    if (!path.startsWith("/")) path = "/" + path;
    LittleFS.remove(path);
    yield();
  }
}

void pageSetup() {
  // Scan WiFi networks
  int n = WiFi.scanNetworks();
  
  String content;
  content.reserve(2048);
  content = F("<h2>WiFi Configuration</h2>");

  // Saved network notice
  if (strlen(wifiCfg.ssid) > 0) {
    content += F("<div class='info' style='background:#e7f3fe; border-left:6px solid #2196F3; padding:10px;'>");
    content += F("<strong>Notice:</strong> There is already a WiFi network saved in memory. Entering new data will overwrite the previous configuration.");
    content += F("</div><br>");
  }

  // List of scanned networks
  content += F("<fieldset style='margin-bottom:20px;'><legend>Available Networks (Click to select)</legend>");
  if (n == 0) {
    content += F("<p>No networks found. Reload the page to scan again.</p>");
  } else {
    content += F("<div style='max-height:200px; overflow-y:auto;'>");
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String color = (rssi > -60) ? "green" : (rssi > -80 ? "orange" : "red");
      // Escape potential quotes in SSID for JavaScript
      String escapedSsid = ssid; escapedSsid.replace("'", "\\'");
      
      content += "<div class='card' style='padding:8px; margin-bottom:5px; cursor:pointer; font-size:14px;' onclick=\"document.getElementsByName('ssid')[0].value='" + escapedSsid + "'\">";
      content += "<span><b>" + ssid + "</b></span>";
      content += "<span style='color:" + color + "'>" + String(rssi) + " dBm</span>";
      content += "</div>";
    }
    content += F("</div>");
  }
  content += F("</fieldset>");
  WiFi.scanDelete(); // Free memory used by scan

  content += F("<form action='/save_wifi' method='POST'>");
  content += F("SSID:<br><input name='ssid' placeholder='Network name' required autocapitalize='none' autocorrect='off'><br>");
  content += "Password:<br><input name='pass' type='" + String(appCfg.mask_wifi ? "password" : "text") + "' placeholder='Network password' autocapitalize='none' autocorrect='off'><br><br>";
  content += F("<label><input type='checkbox' name='clean' value='1'> Clean up unnecessary files (e.g. Tasmota)</label><br><br>");
  content += F("<button type='submit'>Save and Connect</button>");
  content += F("</form>");

  server.send(200, "text/html", renderPage("Setup", content));
}

void handleSaveWifi() {
  if (server.hasArg("ssid")) strlcpy(wifiCfg.ssid, server.arg("ssid").c_str(), sizeof(wifiCfg.ssid));
  if (server.hasArg("pass")) strlcpy(wifiCfg.pass, server.arg("pass").c_str(), sizeof(wifiCfg.pass));
  saveWifiConfig();
  if (server.hasArg("clean") && server.arg("clean") == "1") {
    cleanupUnusedFiles();
  }
  server.send(200, "text/html", "Restarting...");
  delay(1000);
  ESP.restart();
}

void handleReset() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  LittleFS.format();
  ESP.restart();
}

// Update handlers (Standard)
void handleUpdatePage() {
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  String content = "<h2>Update</h2><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><button>Upload</button></form>";
  server.send(200, "text/html", renderPage("Update", content));
}
void handleUpdateUpload() {
  // Verificăm autentificarea înainte de a procesa orice fragment (chunk) din upload
  if (appCfg.auth_config && !server.authenticate(appCfg.user_config, appCfg.pass_config)) {
    return; 
  }

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) { WiFiUDP::stopAll(); Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000); }
  else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if (upload.status == UPLOAD_FILE_END) Update.end(true);
}
void handleUpdateFinish() { 
  if (!checkAuth(appCfg.auth_config, appCfg.user_config, appCfg.pass_config)) return;
  server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK"); ESP.restart(); 
}


// ================= SETUP =================
void setup() {
  // ESP8285 Fix: Disable sleep for stability
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.persistent(false); // Disable automatic credential writing to flash by the SDK
  
  // Pin Configuration
  for(int i=0; i<RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    // Initialize state (OFF) accounting for inversion
    digitalWrite(RELAY_PINS[i], RELAY_INVERTED[i] ? HIGH : LOW);
  }
  pinMode(BTN_PIN, INPUT); // GPIO16 has no internal PULLUP, use INPUT (assume external pull-down/up)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED OFF

  // Stop Hardware Serial to free pins and avoid conflicts
  Serial.end();

  // Initialize Software Serial (RX=3, TX=1)
  cseSerial.begin(4800, SWSERIAL_8E1);
  // Force Pullup on RX (GPIO3) after initialization for stability
  pinMode(3, INPUT_PULLUP);
  delay(100);
  cseSerial.write(0x55); // Send sync

  loadConfig();

  // Apply Power-on behavior
  if (appCfg.power_on_behavior == 0) { // All OFF
    for(int i=0; i<RELAY_COUNT; i++) appCfg.relay_state[i] = false;
  } else if (appCfg.power_on_behavior == 1) { // All ON
    for(int i=0; i<RELAY_COUNT; i++) appCfg.relay_state[i] = true;
  }
  // If 2 (Previous), use appCfg.relay_state loaded from flash.

  // Initialize relay hardware
  for(int i=0; i<RELAY_COUNT; i++) {
    setRelay(i, appCfg.relay_state[i]);
  }

  wifiConnectHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) { wifiConnectCount++; });
  wifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) { wifiDisconnectCount++; });
  last_mqtt_report_time = millis(); // Initialize reporting timer

  if (!connectWiFi()) {
    setupMode = true;
    // AP_STA mode: Allows AP for config but also background scanning/connection
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("NOUS-Setup");
    
    ArduinoOTA.setHostname("NOUS-A5T-CONFIG-MODE");
    ArduinoOTA.setPassword(appCfg.ota_pass);
    ArduinoOTA.begin();
    
    server.on("/", pageSetup); // In AP mode, show initial setup page
    server.on("/save_wifi", handleSaveWifi);
  } else {
    setupMode = false;
    startNormalServices();
  }
  server.begin();
}

// ================= AUXILIARY HANDLERS =================
void handleButtons() {
  // --- Buton Digital (GPIO 16) ---
  static int lastBtnState = HIGH;
  static unsigned long btnPressTime = 0;
  int reading = digitalRead(BTN_PIN);

  if (reading == LOW && lastBtnState == HIGH) { // Press start
    btnPressTime = millis();
  }
  
  if (reading == HIGH && lastBtnState == LOW) { // Release
    unsigned long duration = millis() - btnPressTime;
    if (duration > 50 && duration < 5000) {
      // Short press: Toggle All (if not Child Lock)
      if (!appCfg.child_lock) toggleAll();
    } else if (duration > 10000) {
      // Long press (>10s): Factory Reset (if not Child Lock)
      if (!appCfg.child_lock) handleReset();
    }
  }
  lastBtnState = reading;

  // --- Butoane Analogice (ADC) ---
  static unsigned long lastAdcTime = 0;
  static int btnState = 0; // 0: None, 1: Btn1, 2: Btn2, 3: Btn3
  
  if (millis() - lastAdcTime > 100) {
    lastAdcTime = millis();
    int adcValue = analogRead(A0);

    int detectedBtn = 0;
    if (adcValue > ADC_BTN1_MIN && adcValue < ADC_BTN1_MAX) detectedBtn = 1;
    else if (adcValue > ADC_BTN2_MIN && adcValue < ADC_BTN2_MAX) detectedBtn = 2;
    else if (adcValue > ADC_BTN3_MIN && adcValue < ADC_BTN3_MAX) detectedBtn = 3;
    
    if (detectedBtn != 0 && btnState == 0) {
        // Button pressed
        btnState = detectedBtn;
        int relayIdx = detectedBtn - 1; // Btn1 -> Relay 0, etc.
        if (relayIdx >= 0 && relayIdx < 3) {
              if (!appCfg.child_lock) {
                setRelay(relayIdx, !appCfg.relay_state[relayIdx]);
                configDirty = true;
                lastChangeTime = millis();
              }
        }
    } else if (detectedBtn == 0) {
        // Button released
        btnState = 0;
    }
  }
}

void handlePeriodicUpdates() {
  // Periodic MQTT publication (Energy)
  static unsigned long lastMqttEnergy = 0;
  if (millis() - lastMqttEnergy > MQTT_PUB_INTERVAL) {
    lastMqttEnergy = millis();
    mqttPublishEnergy();
  }

  // Retry CSE7766: If no data received, send start command again
  static unsigned long lastCseRetry = 0;
  if (millis() - lastCsePacketTime > CSE_RETRY_INTERVAL && millis() - lastCseRetry > 1000) {
    lastCseRetry = millis();
    cseSerial.write(0x55);
  }

  // Blink LED in Setup Mode
  if (setupMode) {
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else {
    // LED Status (WiFi Connected) - ON (Inverted)
    digitalWrite(LED_PIN, LOW); 
  }

  // Periodic WiFi connection check (Auto-reconnect)
  static unsigned long lastWifiCheck = 0;
  static unsigned long lastDisconnectTime = 0;

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    lastDisconnectTime = 0;
  } else if (strlen(wifiCfg.ssid) > 0) {
    if (lastDisconnectTime == 0) lastDisconnectTime = millis();
  }

  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();

    if (status != WL_CONNECTED && strlen(wifiCfg.ssid) > 0) {
      // Try reconnecting only if not already in progress
      if (status != WL_DISCONNECTED) {
      WiFi.begin(wifiCfg.ssid, wifiCfg.pass);
      }

      // If more than 5 minutes have passed, activate AP mode for emergency
      if (!setupMode && lastDisconnectTime > 0 && (millis() - lastDisconnectTime > 300000)) { 
        setupMode = true;
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("NOUS-Setup");
        server.on("/", pageSetup);
        server.on("/save_wifi", handleSaveWifi);
      }
    }
  }

  // If in Setup Mode and just connected to saved WiFi
  if (setupMode && WiFi.status() == WL_CONNECTED) {
      setupMode = false;
      WiFi.mode(WIFI_STA); // Stop AP mode
      startNormalServices(); // Initialize monitoring and MQTT services
      digitalWrite(LED_PIN, LOW); // LED ON (Inverted)
  }

  // Hardware Overload Protection
  if (cse_power > MAX_POWER_LIMIT) { 
    for(int i=0; i<RELAY_COUNT; i++) setRelay(i, false);
    calStatusMsg = "ALARM: POWER LIMIT EXCEEDED!";
  }

  // Finish calibration after duration
  if (isCalibrating && (millis() - calStartTime > CAL_TOTAL_DURATION)) {
    if (calSamples > 5) {
      double avgRawV = sumRawV / calSamples;
      double avgRawI = sumRawI / calSamples;
      double avgRawP = sumRawP / calSamples;

      if (avgRawP > 0.1 && avgRawV > 10.0 && avgRawI > 0.001) {
        // 1. Calibrate Power (P) to match the real target load
        appCfg.cal_power = (float)(calTargetP / avgRawP);
        
        // 2. Adjust Current (I) relative to ALREADY CALIBRATED voltage
        // Target_I = Target_P / Actual_Calibrated_V
        double vCalibratActual = avgRawV * appCfg.cal_voltage;
        appCfg.cal_current = (float)((calTargetP / vCalibratActual) / avgRawI);

        calStatusMsg = "Calibration successful! P and I updated (Ref V: " + String(vCalibratActual, 1) + "V)";
      } else {
        calStatusMsg = "Error: Invalid values detected during sampling.";
      }
      saveAppConfig();
    }
    isCalibrating = false;
  }
}

// ================= LOOP =================
void loop() {
  server.handleClient();
  MDNS.update();
  ArduinoOTA.handle();
  if (!setupMode) {
    mqtt.loop();
    mqttConnect();
    
    // Monitorizare deconectari MQTT
    bool currentMqttState = mqtt.connected();
    if (lastMqttConnectedState && !currentMqttState) {
        mqttDisconnectCount++;
    }
    lastMqttConnectedState = currentMqttState;
  }

  // Read energy data and protection (always running)
  handleCSE7766();

  // Delayed save to LittleFS
  if (configDirty && millis() - lastChangeTime > CFG_SAVE_DELAY) {
    saveAppConfig();
    configDirty = false;
  }

  handleButtons();
  handlePeriodicUpdates();
}
