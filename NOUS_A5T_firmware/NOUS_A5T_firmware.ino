
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <ArduinoOTA.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ctype.h>
// ================= HARDWARE ABSTRACTION =================
#define DEVICE_HARDWARE_IMPL_HEADER "DeviceHardware_NousA5T.h"
#define CORE_VERSION         "v.1.0.1"

// ================= SETTINGS =================
#define CONFIG_MAGIC         0xA5A50006
#define WIFI_BLOB_MAGIC      0x57434647U
#define MQTT_BLOB_MAGIC      0x4D434647U
#define APP_BLOB_MAGIC       0x41434647U

#define CFG_SAVE_DELAY           3000UL
#define MQTT_RECONNECT_INTERVAL 10000UL
#define MQTT_PUB_INTERVAL_MS     1000UL
#define WIFI_CONNECT_TIMEOUT    15000UL
#define MQTT_CMD_MAX_LEN         128

// ================= DATA MODELS =================
struct WifiConfig {
  char ssid[64];
  char pass[64];
};

struct MqttConfig {
  char host[64];
  uint16_t port;
  char user[64];
  char pass[64];
  char client_id[64];
  char topic[64];
  bool enabled;
  char mdns_hostname[32];
  uint16_t pub_interval;
};

struct AppConfig {
  uint32_t magic;

  // Security & Auth (template infrastructure only)
  bool auth_config;
  char user_config[16];
  char pass_config[16];
  bool auth_root;
  char user_root[16];
  char pass_root[16];
  char setup_ap_pass[32];
  char ota_pass[32];
  bool mask_wifi;
  bool mask_mqtt;
  bool mask_auth_root;
  bool mask_auth_config;
  
  // UI
  uint8_t ui_lang;
};

struct WifiConfigBlob {
  uint32_t magic;
  WifiConfig cfg;
  uint32_t crc;
};

struct MqttConfigBlob {
  uint32_t magic;
  MqttConfig cfg;
  uint32_t crc;
};

struct AppConfigBlob {
  uint32_t magic;
  AppConfig cfg;
  uint32_t crc;
};

// Used by DeviceHardware_*.h for layered auth (network gate first)
bool checkNetworkGateForCurrentRequest(const AppConfig& appCfg);

// ================= HARDWARE ABSTRACTION =================
// Include identitatea dispozitivului inainte de orice definitie
#include DEVICE_HARDWARE_IMPL_HEADER

// FW_VERSION este acum compus din modelul definit in header
#define FW_VERSION FW_VERSION_FULL

// ================= GLOBALS =================
WifiConfig wifiCfg;
MqttConfig mqttCfg;
AppConfig appCfg;

DeviceHardware hw;
ESP8266WebServer server(80);
WebSocketsServer ws(81);
WiFiClient espClient;
PubSubClient mqtt(espClient);

bool setupMode = false;
bool configDirty = false;
unsigned long lastChangeTime = 0;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublish = 0;
bool lastMqttConnectedState = false;
bool requestFactoryReset = false;
bool pwrCycCleared = false;
unsigned long lastUiRealtimePush = 0;

unsigned long wifiConnectCount = 0;
unsigned long wifiDisconnectCount = 0;
unsigned long mqttConnectCount = 0;
unsigned long mqttDisconnectCount = 0;

IPAddress netAuthSessionIp;
unsigned long netAuthSessionUntil = 0;
const unsigned long NET_AUTH_SESSION_MS = 5UL * 60UL * 1000UL;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

// ================= FORWARD DECLARATIONS =================
void markConfigDirty();
void saveAllConfigsIfDirty();
void mqttEnsureConnected();
void wsBroadcastUpdate();
void wsBroadcastReload();
void setupRoutesNormal();
void setupRoutesSetup();

void pageStatus();
void pageConfig();
void pageSecurity();
void pageSetup();

void handleSaveConnection();
void handleSaveTopic();
void handleSaveMdns();
void handleSaveInterval();
void handleSaveMqttControl();
void handleSaveLang();
void handleSaveSecurity();
void handleReset();
void handleTestMqtt();

void handleUpdatePage();
void handleUpdateUpload();
void handleUpdateFinish();

bool checkAuth(bool required, const char* user, const char* pass);
bool checkAuthWithRealm(bool required, const char* user, const char* pass, const char* realm);
bool checkNetworkAccess();
bool checkConfigAccess();
bool isNetAuthSessionValid();
void rememberNetAuthSession();
String sanitizeLocalRedirect(const String& url);
bool isValidMdnsHostname(const String& host);

// ================= UTILS =================
uint32_t calcCrc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320U & (-(int32_t)(crc & 1U)));
    }
  }
  return ~crc;
}

bool isValidMdnsHostname(const String& host) {
  if (host.length() < 1 || host.length() > 31) return false;
  if (host[0] == '-' || host[host.length() - 1] == '-') return false;
  for (size_t i = 0; i < host.length(); i++) {
    unsigned char c = (unsigned char)host[i];
    if (!(isalnum(c) || c == '-')) return false;
  }
  return true;
}

String sanitizeLocalRedirect(const String& url) {
  if (url.length() == 0) return String("/");
  if (url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0) return String("/");

  String path = url;
  if (path.startsWith("http://") || path.startsWith("https://")) {
    int protoPos = path.indexOf("://");
    int slashPos = path.indexOf('/', protoPos + 3);
    if (slashPos < 0) return String("/");
    path = path.substring(slashPos);
  }

  if (path.startsWith("//")) return String("/");
  if (!path.startsWith("/")) return String("/");

  int frag = path.indexOf('#');
  if (frag >= 0) path = path.substring(0, frag);
  if (path.length() == 0) return String("/");
  return path;
}

bool checkAuth(bool required, const char* user, const char* pass) {
  if (!required) return true;
  if (!server.authenticate(user, pass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

bool checkAuthWithRealm(bool required, const char* user, const char* pass, const char* realm) {
  if (!required) return true;
  if (!server.authenticate(user, pass)) {
    server.requestAuthentication(BASIC_AUTH, realm);
    return false;
  }
  return true;
}

bool isNetAuthSessionValid() {
  if (netAuthSessionUntil == 0) return false;
  if (server.client().remoteIP() != netAuthSessionIp) return false;
  return (long)(netAuthSessionUntil - millis()) > 0;
}

void rememberNetAuthSession() {
  netAuthSessionIp = server.client().remoteIP();
  netAuthSessionUntil = millis() + NET_AUTH_SESSION_MS;
}

bool checkNetworkGateForCurrentRequest(const AppConfig& appCfg) {
  if (!appCfg.auth_root) return true;
  if (isNetAuthSessionValid()) return true;
  if (server.authenticate(appCfg.user_root, appCfg.pass_root)) {
    rememberNetAuthSession();
    return true;
  }
  server.requestAuthentication(BASIC_AUTH, "Network Access");
  return false;
}

bool checkNetworkAccess() {
  return checkNetworkGateForCurrentRequest(appCfg);
}

bool checkConfigAccess() {
  if (!checkNetworkAccess()) return false;
  return checkAuthWithRealm(appCfg.auth_config, appCfg.user_config, appCfg.pass_config, "Config Access");
}

void defaultConfigs() {
  memset(&wifiCfg, 0, sizeof(wifiCfg));
  memset(&mqttCfg, 0, sizeof(mqttCfg));
  memset(&appCfg, 0, sizeof(appCfg));

  String defaultHost = hw.getDefaultHostname();

  mqttCfg.port = 1883;
  mqttCfg.enabled = false;
  mqttCfg.pub_interval = 1;
  strlcpy(mqttCfg.client_id, defaultHost.c_str(), sizeof(mqttCfg.client_id));
  strlcpy(mqttCfg.topic, defaultHost.c_str(), sizeof(mqttCfg.topic));
  strlcpy(mqttCfg.mdns_hostname, defaultHost.c_str(), sizeof(mqttCfg.mdns_hostname));

  appCfg.magic = CONFIG_MAGIC;
  appCfg.ui_lang = 1;
  strlcpy(appCfg.user_root, "admin", sizeof(appCfg.user_root));
  strlcpy(appCfg.pass_root, "admin", sizeof(appCfg.pass_root));
  strlcpy(appCfg.user_config, "admin", sizeof(appCfg.user_config));
  strlcpy(appCfg.pass_config, "admin", sizeof(appCfg.pass_config));
  strlcpy(appCfg.setup_ap_pass, DEFAULT_AP_PASS, sizeof(appCfg.setup_ap_pass));
  strlcpy(appCfg.ota_pass, "", sizeof(appCfg.ota_pass));
  hw.applyConfigDefaults(appCfg);
}

void saveWifiConfig() {
  File f = LittleFS.open("/wifi.bin.tmp", "w");
  if (!f) return;
  WifiConfigBlob blob;
  blob.magic = WIFI_BLOB_MAGIC;
  blob.cfg = wifiCfg;
  blob.crc = calcCrc32((const uint8_t*)&blob.cfg, sizeof(WifiConfig));
  size_t written = f.write((uint8_t*)&blob, sizeof(blob));
  f.close();
  if (written == sizeof(blob)) {
    LittleFS.remove("/wifi.bin");
    LittleFS.rename("/wifi.bin.tmp", "/wifi.bin");
  }
}

void saveMqttConfig() {
  File f = LittleFS.open("/mqtt.bin.tmp", "w");
  if (!f) return;
  MqttConfigBlob blob;
  blob.magic = MQTT_BLOB_MAGIC;
  blob.cfg = mqttCfg;
  blob.crc = calcCrc32((const uint8_t*)&blob.cfg, sizeof(MqttConfig));
  size_t written = f.write((uint8_t*)&blob, sizeof(blob));
  f.close();
  if (written == sizeof(blob)) {
    LittleFS.remove("/mqtt.bin");
    LittleFS.rename("/mqtt.bin.tmp", "/mqtt.bin");
  }
}

void saveAppConfig() {
  File f = LittleFS.open("/app.bin.tmp", "w");
  if (!f) return;
  AppConfigBlob blob;
  blob.magic = APP_BLOB_MAGIC;
  blob.cfg = appCfg;
  blob.crc = calcCrc32((const uint8_t*)&blob.cfg, sizeof(AppConfig));
  size_t written = f.write((uint8_t*)&blob, sizeof(blob));
  f.close();
  if (written == sizeof(blob)) {
    LittleFS.remove("/app.bin");
    LittleFS.rename("/app.bin.tmp", "/app.bin");
  }
}

void loadConfigs() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }

  defaultConfigs();

  if (LittleFS.exists("/wifi.bin")) {
    File f = LittleFS.open("/wifi.bin", "r");
    if (f && f.size() == sizeof(WifiConfigBlob)) {
      WifiConfigBlob blob;
      if (f.read((uint8_t*)&blob, sizeof(blob)) == sizeof(blob) &&
          blob.magic == WIFI_BLOB_MAGIC &&
          blob.crc == calcCrc32((const uint8_t*)&blob.cfg, sizeof(WifiConfig))) {
        wifiCfg = blob.cfg;
      }
    }
    f.close();
  }

  if (LittleFS.exists("/mqtt.bin")) {
    File f = LittleFS.open("/mqtt.bin", "r");
    if (f && f.size() == sizeof(MqttConfigBlob)) {
      MqttConfigBlob blob;
      if (f.read((uint8_t*)&blob, sizeof(blob)) == sizeof(blob) &&
          blob.magic == MQTT_BLOB_MAGIC &&
          blob.crc == calcCrc32((const uint8_t*)&blob.cfg, sizeof(MqttConfig))) {
        mqttCfg = blob.cfg;
      }
    }
    f.close();
  }

  if (LittleFS.exists("/app.bin")) {
    File f = LittleFS.open("/app.bin", "r");
    if (f && f.size() == sizeof(AppConfigBlob)) {
      AppConfigBlob blob;
      if (f.read((uint8_t*)&blob, sizeof(blob)) == sizeof(blob) &&
          blob.magic == APP_BLOB_MAGIC &&
          blob.crc == calcCrc32((const uint8_t*)&blob.cfg, sizeof(AppConfig)) &&
          blob.cfg.magic == CONFIG_MAGIC) {
        appCfg = blob.cfg;
      }
    }
    f.close();
  }

  // Sanitizare: Dacă valorile sunt goale sau invalide, forțăm formatul dinamic bazat pe Chip ID
  String defHost = hw.getDefaultHostname();
  if (strlen(mqttCfg.mdns_hostname) == 0 || !isValidMdnsHostname(String(mqttCfg.mdns_hostname))) {
    strlcpy(mqttCfg.mdns_hostname, defHost.c_str(), sizeof(mqttCfg.mdns_hostname));
  }
  if (strlen(mqttCfg.topic) == 0) {
    strlcpy(mqttCfg.topic, defHost.c_str(), sizeof(mqttCfg.topic));
  }
  if (strlen(mqttCfg.client_id) == 0) {
    strlcpy(mqttCfg.client_id, defHost.c_str(), sizeof(mqttCfg.client_id));
  }

  if (strlen(appCfg.setup_ap_pass) < 8 || strlen(appCfg.setup_ap_pass) > 31) {
    strlcpy(appCfg.setup_ap_pass, DEFAULT_AP_PASS, sizeof(appCfg.setup_ap_pass));
  }
  if (mqttCfg.pub_interval < 1) mqttCfg.pub_interval = 1;
  hw.validateConfig(appCfg);
}

void markConfigDirty() {
  configDirty = true;
  lastChangeTime = millis();
}

void saveAllConfigsIfDirty() {
  if (!configDirty) return;
  if (millis() - lastChangeTime < CFG_SAVE_DELAY) return;
  saveWifiConfig();
  saveMqttConfig();
  saveAppConfig();
  configDirty = false;
}

// ================= MQTT INFRASTRUCTURE (TEMPLATE ONLY) =================
// Rolul template-ului: gestiune conexiune, routare comenzi către hardware
// Hardware-ul: publish senzori, releu, discovery, comenzi specifice

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length > MQTT_CMD_MAX_LEN) return;

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  String topicStr = topic;
  String base = String(mqttCfg.topic);
  
  // Rutare comenzi către hardware
  if (topicStr.startsWith(base + "/")) {
    String sub = topicStr.substring(base.length());
    hw.handleMqttCommand(sub, msg, appCfg, mqtt, mqttCfg);
    if (sub.endsWith("/set")) {
      wsBroadcastUpdate();
    }
    return;
  }
}

void wsBroadcastReload() {
  ws.broadcastTXT("reload");
}

void wsBroadcastUpdate() {
  String json;
  json.reserve(600);
  json += F("{\"heap\":"); json += ESP.getFreeHeap();
  json += F(",\"uptime\":"); json += (millis() / 1000);
  json += F(",\"wifi\":\""); json += (WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected"));
  json += F("\",\"mqtt\":\""); json += (mqtt.connected() ? F("Connected") : F("Disconnected"));
  json += F("\",\"rssi\":"); json += WiFi.RSSI();
  json += F(","); json += hw.getJsonStatus(appCfg);
  json += F("}");
  ws.broadcastTXT(json);
}

void mqttSubscribeTopics() {
  if (!mqtt.connected()) return;
  
  // Template subscribe la topicul base doar pentru comenzi
  String subTopic = String(mqttCfg.topic) + "/#";
  mqtt.subscribe(subTopic.c_str());
}

bool mqttConnect() {
  if (!mqttCfg.enabled || strlen(mqttCfg.host) == 0) return false;

  // Asigurăm că PubSubClient folosește cele mai recente setări din config
  mqtt.setServer(mqttCfg.host, mqttCfg.port);

  String lwtTopic = String(mqttCfg.topic) + "/status";
  bool ok;
  if (strlen(mqttCfg.user) > 0) {
    ok = mqtt.connect(mqttCfg.client_id, mqttCfg.user, mqttCfg.pass,
                      lwtTopic.c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(mqttCfg.client_id,
                      lwtTopic.c_str(), 0, true, "offline");
  }

  if (ok) {
    mqttConnectCount++;
    mqttSubscribeTopics();
    // Hardware publichiază discovery și status
      hw.onMqttConnected(mqtt, mqttCfg, appCfg, FW_VERSION);
  }

  return ok;
}

void mqttEnsureConnected() {
  if (!mqttCfg.enabled) return;
  if (mqtt.connected()) return;

  if (millis() - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL) return;
  lastMqttReconnectAttempt = millis();
  mqttConnect();
}

// ================= WEB =================
void sendRedirect(const String& url) {
  server.sendHeader("Location", sanitizeLocalRedirect(url), true);
  server.send(303, "text/plain", "Redirect");
}

// ================= UI HELPERS =================
String getCommonCss() {
  String css;
  css.reserve(1200);
  css += F("<style>*{box-sizing:border-box}body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
           ".card{background:white;padding:16px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);margin-bottom:12px;overflow:hidden;word-break:break-all}"
           ".mini-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;align-items:start}"
           ".mini-card h2{font-size:16px;margin-bottom:8px}.mini-card p{font-size:13px;margin:6px 0}h2{color:#333;margin-top:0}p{margin:8px 0;color:#666}"
           ".status-ok{color:#28a745;font-weight:bold}.status-err{color:#dc3545;font-weight:bold}nav{margin-bottom:20px}"
           "nav a{display:inline-block;padding:10px 15px;background:#007bff;color:white;text-decoration:none;border-radius:4px;margin-right:5px}nav a:hover{background:#0056b3}"
           "form{display:flex;flex-direction:column}label{margin-top:10px;font-weight:bold;color:#333;display:block}"
           "input,select{width:100%;padding:8px;margin-top:5px;border:1px solid #ddd;border-radius:4px}"
           "button{width:100%;margin-top:15px;padding:10px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer}button:hover{background:#0056b3}"
           "code,pre{word-break:break-all;white-space:pre-wrap;display:block;font-size:12px}.config-grid, .grid2{display:grid;grid-template-columns:1fr;gap:20px;align-items:start}"
           "@media(min-width:700px){.config-grid, .grid2{grid-template-columns:1fr 1fr}}");
  css += F("</style>");
  return css;
}

String getCommonHeader(const String& title, const String& fromUrl) {
  String h;
  h.reserve(600);
  h += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'><h1 style='margin:0'>");
  h += title;
  h += F("</h1><div style='display:flex;gap:5px'><a href='/save_lang?lang=0&from=");
  h += fromUrl; h += F("' style='text-decoration:none;padding:5px 12px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;");
  h += (appCfg.ui_lang == 0 ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  h += F("'>RO</a><a href='/save_lang?lang=1&from="); h += fromUrl; h += F("' style='text-decoration:none;padding:5px 12px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;");
  h += (appCfg.ui_lang == 1 ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  h += F("'>EN</a>");
  h += F("</div></div>");
  return h;
}

String getCommonNav() {
  String n;
  n.reserve(400);
  n += F("<nav>");
  n += F("<a href='/'>"); n += (appCfg.ui_lang == 0 ? F("Status") : F("Status"));
  n += F("</a><a href='/config'>"); n += (appCfg.ui_lang == 0 ? F("Config") : F("Config"));
  n += F("</a><a href='/security'>"); n += (appCfg.ui_lang == 0 ? F("Securitate") : F("Security"));
  n += F("</a><a href='/update'>"); n += (appCfg.ui_lang == 0 ? F("Actualizare OTA") : F("OTA Update"));
  n += F("</a></nav>");
  return n;
}

void pageStatus() {
  if (!checkNetworkAccess()) return;

  String html;
  html.reserve(3200);

  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Status</title>");
  html += getCommonCss();
  html += F("</head><body>");

  html += getCommonHeader(appCfg.ui_lang == 0 ? F("Status Dispozitiv") : F("Device Status"), F("/"));
  html += getCommonNav();

  // Hardware controls first (requested)
  html += hw.renderMainPageFrame(FW_VERSION, WiFi.localIP().toString(), WiFi.RSSI(), mqtt.connected(), appCfg);

  html += F("<div class='mini-grid'>");
  // Device Info Card
  html += F("<div class='card mini-card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Informații Dispozitiv") : F("Device Information")); html += F("</h2>");
  html += F("<p><b>Firmware:</b> "); html += FW_VERSION; html += F("</p>");
  html += F("<p><b>Core S:</b> "); html += CORE_VERSION; html += F("</p>");
  html += F("<p><b>MAC Address:</b> "); html += WiFi.macAddress(); html += F("</p>");
  html += F("<p><b>Chip ID:</b> "); html += String(ESP.getChipId(), HEX); html += F("</p>");
  html += F("<p><b>"); html += (appCfg.ui_lang == 0 ? F("RAM Liber:") : F("Free RAM:")); html += F("</b> <span id='val_heap'>"); html += (ESP.getFreeHeap() / 1024); html += F("</span> KB</p>");
  html += F("<p><b>"); html += (appCfg.ui_lang == 0 ? F("Timp funcționare:") : F("Uptime:")); html += F("</b> <span id='val_uptime'>");
  unsigned long upS = millis() / 1000;
  unsigned long upD = upS / 86400;
  unsigned long upH = (upS % 86400) / 3600;
  unsigned long upM = (upS % 3600) / 60;
  if (upD > 0) { html += upD; html += (appCfg.ui_lang == 0 ? F("z ") : F("d ")); }
  if (upH > 0 || upD > 0) { html += upH; html += F("h "); }
  html += upM; html += F("m</span></p>");
  html += F("</div>");
  // Network Card
  html += F("<div class='card mini-card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Stare Rețea") : F("Network Status")); html += F("</h2>");
  
  bool connected = (WiFi.status() == WL_CONNECTED);
  html += F("<p id='wifi_stat' class='"); html += (connected ? F("status-ok") : F("status-err")); html += F("'>");
  html += (connected ? (appCfg.ui_lang == 0 ? F("WiFi: Conectat ✓") : F("WiFi: Connected ✓")) : (appCfg.ui_lang == 0 ? F("WiFi: Deconectat ✗") : F("WiFi: Disconnected ✗")));
  html += F("</p>");
  
  html += F("<p><b>SSID:</b> "); html += DeviceHardware::htmlEscape(WiFi.SSID()); html += F("</p>");
  html += F("<p><b>IP Address:</b> "); html += WiFi.localIP().toString(); html += F("</p>");
  html += F("<p><b>Signal Strength (RSSI):</b> <span id='val_rssi'>"); html += WiFi.RSSI(); html += F("</span> dBm</p>");

  html += F("<p><b>WiFi Connections:</b> "); html += wifiConnectCount; html += F("</p>");
  html += F("<p><b>WiFi Disconnections:</b> "); html += wifiDisconnectCount; html += F("</p>");

  html += F("</div>");
  // MQTT Card
  html += F("<div class='card mini-card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Stare MQTT") : F("MQTT Status")); html += F("</h2>");
  
  bool mConnected = mqtt.connected();
  html += F("<p id='mqtt_stat' class='"); html += (mConnected ? F("status-ok") : F("status-err")); html += F("'>");
  if (mqttCfg.enabled) {
    html += (mConnected ? (appCfg.ui_lang == 0 ? F("MQTT: Conectat ✓") : F("MQTT: Connected ✓")) : (appCfg.ui_lang == 0 ? F("MQTT: Deconectat ✗") : F("MQTT: Disconnected ✗")));
    html += F("</p><p><b>Broker:</b> "); html += DeviceHardware::htmlEscape(mqttCfg.host); html += F(":"); html += mqttCfg.port; html += F("</p>");
    html += F("<p><b>Topic:</b> "); html += DeviceHardware::htmlEscape(mqttCfg.topic); html += F("</p>");
    html += F("<p><b>MQTT Connections:</b> "); html += mqttConnectCount; html += F("</p>");
    html += F("<p><b>MQTT Disconnections:</b> "); html += mqttDisconnectCount; html += F("</p>");
    } else {
      html += (appCfg.ui_lang == 0 ? F("MQTT: Dezactivat") : F("MQTT: Disabled"));
      html += F("</p>");
  }
  html += F("</div>");
  html += F("</div>");

  html += F("<script>");
  html += F("(function(){");
  html += F("const lang="); html += appCfg.ui_lang; html += F(";");
  html += F("let ws=null;let rt=null;");
  html += F("function connect(){");
  html += F("try{ws=new WebSocket('ws://'+location.hostname+':81/');}catch(e){schedule();return;}");
  html += F("ws.onmessage=function(ev){");
  html += F("if(!ev||!ev.data)return;");
  html += F("if(ev.data==='reload'){location.reload();return;}");
  html += F("try{let d=JSON.parse(ev.data);");
  html += F("if(d.heap!==undefined)document.getElementById('val_heap').innerText=Math.round(d.heap/1024);");
  html += F("if(d.uptime!==undefined){let s=d.uptime,dy=Math.floor(s/86400),hr=Math.floor((s%86400)/3600),mn=Math.floor((s%3600)/60);document.getElementById('val_uptime').innerText=(dy>0?dy+(lang==0?'z ':'d '):'')+(hr>0||dy>0?hr+'h ':'')+mn+'m';}");
  html += F("if(d.rssi!==undefined)document.getElementById('val_rssi').innerText=d.rssi;");
  html += F("if(d.wifi){let w=document.getElementById('wifi_stat');let s=(d.wifi==='Connected');w.innerText='WiFi: '+(lang==0?(s?'Conectat':'Deconectat'):(s?'Connected':'Disconnected'))+(s?' ✓':' ✗');w.className=(s?'status-ok':'status-err');}");
  html += F("if(d.mqtt){let m=document.getElementById('mqtt_stat');let s=(d.mqtt==='Connected');m.innerText='MQTT: '+(lang==0?(s?'Conectat':'Deconectat'):(s?'Connected':'Disconnected'))+(s?' ✓':' ✗');m.className=(s?'status-ok':'status-err');}");
  html += F("if(typeof updateHardwareUI==='function')updateHardwareUI(d);");
  html += F("}catch(e){}};");
  html += F("ws.onclose=schedule;ws.onerror=function(){try{ws.close();}catch(e){}};}");
  html += F("function schedule(){if(rt)return;rt=setTimeout(function(){rt=null;connect();},1000);}");
  html += F("connect();");
  html += F("})();");
  html += hw.renderMainPageScript(appCfg);
  html += F("</script>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void pageConfig() {
  if (!checkConfigAccess()) return;

  String html;
  html.reserve(4500);

  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Configuration</title>");
  html += getCommonCss();
  html += F("</head><body>");

  html += getCommonHeader(appCfg.ui_lang == 0 ? F("Configurări") : F("Configuration"), F("/config"));
  html += getCommonNav();

  if (server.hasArg("saved")) {
    html += F("<div id='svmsg' style='position:fixed;top:20px;left:50%;transform:translateX(-50%);background:#28a745;color:white;padding:12px 25px;border-radius:30px;z-index:10000;box-shadow:0 4px 12px rgba(0,0,0,0.2);font-weight:bold;'>");
    html += (appCfg.ui_lang == 0 ? F("Setări salvate!") : F("Settings saved!"));
    html += F("</div><script>setTimeout(function(){var m=document.getElementById('svmsg');if(m)m.style.display='none';},5000);</script>");
  }

  // Two-column layout for config page
  html += F("<div class='config-grid'>");
  html += F("<div>");
  // MQTT Settings
  html += F("<div class='card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Setări MQTT") : F("MQTT Settings")); html += F("</h2>");
  html += F("<form method='post' action='/save_connection'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Gazdă MQTT:") : F("MQTT Host:")); html += F("</label>");
  html += F("<input type='text' name='host' value='"); html += DeviceHardware::htmlEscape(mqttCfg.host); html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Port MQTT:") : F("MQTT Port:")); html += F("</label>");
  html += F("<input type='number' name='port' value='"); html += mqttCfg.port; html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Client ID MQTT:") : F("MQTT Client ID:")); html += F("</label>");
  html += F("<input type='text' name='client_id' value='"); html += DeviceHardware::htmlEscape(mqttCfg.client_id); html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Utilizator MQTT:") : F("MQTT User:")); html += F("</label>");
  html += F("<input type='text' name='user' value='"); html += DeviceHardware::htmlEscape(mqttCfg.user); html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Parolă MQTT:") : F("MQTT Password:")); html += F("</label>");
  html += F("<input type='password' name='mqtpass' value='"); html += DeviceHardware::htmlEscape(mqttCfg.pass); html += F("'>");
  html += F("<button type='button' onclick='testMqtt()' style='background:#6c757d'>"); html += (appCfg.ui_lang == 0 ? F("Test conexiune MQTT") : F("Test MQTT connection")); html += F("</button>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează MQTT") : F("Save MQTT")); html += F("</button>");
  html += F("</form>");
  html += F("</div>");

  // Configurari suplimentare (sub setari MQTT)
  html += F("<div class='card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Setări Suplimentare") : F("Additional Settings")); html += F("</h2>");

  html += F("<form method='post' action='/save_topic'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Topic principal:") : F("Base Topic:")); html += F("</label>");
  html += F("<input type='text' name='topic' value='"); html += DeviceHardware::htmlEscape(mqttCfg.topic); html += F("'>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează Topic") : F("Save Topic")); html += F("</button>");
  html += F("</form>");

  html += F("<form method='post' action='/save_mdns'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Nume gazdă mDNS:") : F("mDNS hostname:")); html += F("</label>");
  html += F("<input type='text' name='mdns' value='"); html += DeviceHardware::htmlEscape(mqttCfg.mdns_hostname); html += F("'>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează mDNS") : F("Save mDNS")); html += F("</button>");
  html += F("</form>");

  html += F("<form method='post' action='/save_interval'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Interval raportare (sec):") : F("Reporting Interval (sec):")); html += F("</label>");
  html += F("<input type='number' min='1' max='3600' name='pub_interval' value='"); html += mqttCfg.pub_interval; html += F("'>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează Interval") : F("Save Interval")); html += F("</button>");
  html += F("</form>");

  html += F("<form method='post' action='/save_mqtt_control'>");
  html += F("<input type='hidden' name='enabled' value='"); html += (mqttCfg.enabled ? F("0") : F("1")); html += F("'>");
  html += F("<button type='submit' style='background:");
  html += (mqttCfg.enabled ? F("#dc3545") : F("#28a745"));
  html += F("'>");
  html += (mqttCfg.enabled ? (appCfg.ui_lang == 0 ? F("Dezactivează MQTT") : F("Disable MQTT")) : (appCfg.ui_lang == 0 ? F("Activează MQTT") : F("Enable MQTT")));
  html += F("</button>");
  html += F("</form>");
  html += F("</div>"); // Închide 'Additional Settings'

  // Danger Zone - Mutat aici din header
  html += F("<div class='card' style='border:1px solid #dc3545'>");
  html += F("<h2 style='color:#dc3545'>"); html += (appCfg.ui_lang == 0 ? F("Zonă Periculoasă") : F("Danger Zone")); html += F("</h2>");
  html += F("<button type='button' style='background:#dc3545;color:#fff' onclick='factoryReset()'>"); 
  html += (appCfg.ui_lang == 0 ? F("Resetare din Fabrică") : F("Factory Reset")); html += F("</button>");
  html += F("</div>");

  html += F("</div>"); // Închide coloana din stânga

  // Right column - additional non-hardware config (kept in INO)
  html += F("<div>");

  // Right column frame generated in hardware header
  html += hw.renderConfigRightFrame(appCfg, mqttCfg);
  html += F("</div>");

  html += F("</div>");

  html += F("<script>");
  html += F("function testMqtt(){const l="); html += appCfg.ui_lang;
  html += F(";const b=document.querySelector('button[onclick=\"testMqtt()\"]');");
  html += F("const ot=b.innerText;b.innerText=(l==0?'Se testează...':'Testing...');b.disabled=true;");
  html += F("const f=document.querySelector('form[action=\"/save_connection\"]');");
  html += F("const d=new URLSearchParams(new FormData(f));");
  html += F("fetch('/api/test_mqtt',{method:'POST',body:d}).then(r=>r.ok?r.json():Promise.reject()).then(d=>{");
  html += F("alert((d.connected?(l==0?'Conectat la ':'Connected to '):(l==0?'Eroare conexiune la ':'Failed to connect to '))+d.host);");
  html += F("}).catch(e=>alert(l==0?'Eroare rețea sau timeout':'Network error or timeout'))");
  html += F(".finally(()=>{b.innerText=ot;b.disabled=false;});}");
  html += F("function factoryReset(){const l="); html += appCfg.ui_lang;
  html += F(";if(confirm(l==0?'Ești sigur că vrei să ștergi toate setările?':'Are you sure you want to delete all settings?'))location.href='/factory_reset';}");
  html += F("</script>");

  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void pageSecurity() {
  if (!checkConfigAccess()) return;

  String html;
  html.reserve(4000);

  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Security</title>");
  html += getCommonCss();
  html += F("</head><body>");

  html += getCommonHeader(appCfg.ui_lang == 0 ? F("Securitate") : F("Security"), F("/security"));
  html += getCommonNav();

  if (server.hasArg("saved")) {
    html += F("<div id='svmsg' style='position:fixed;top:20px;left:50%;transform:translateX(-50%);background:#28a745;color:white;padding:12px 25px;border-radius:30px;z-index:10000;box-shadow:0 4px 12px rgba(0,0,0,0.2);font-weight:bold;'>");
    html += (appCfg.ui_lang == 0 ? F("Setări salvate!") : F("Settings saved!"));
    html += F("</div><script>setTimeout(function(){var m=document.getElementById('svmsg');if(m)m.style.display='none';},5000);</script>");
  }

  html += F("<div class='grid2'>");

  html += F("<div class='card'>");
  html += F("<form method='post' action='/save_security'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Acces Rețea") : F("Network Access")); html += F("</h2>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Activează Autentificare Rețea:") : F("Enable Network Auth:")); html += F("</label>");
  
  html += F("<div style='display:flex;gap:5px;margin-top:5px;margin-bottom:5px'>");
  html += F("<a href='/save_security?auth_root=0' style='text-decoration:none;padding:7px 15px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;flex:1;text-align:center;");
  html += (!appCfg.auth_root ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  html += F("'>OFF</a>");
  html += F("<a href='/save_security?auth_root=1' style='text-decoration:none;padding:7px 15px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;flex:1;text-align:center;");
  html += (appCfg.auth_root ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  html += F("'>ON</a></div>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Nume Utilizator Rețea:") : F("Network Username:")); html += F("</label>");
  html += F("<input type='text' name='user_root' value='"); html += DeviceHardware::htmlEscape(appCfg.user_root); html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Parolă Rețea:") : F("Network Password:")); html += F("</label>");
  html += F("<input type='password' name='pass_root' value='"); html += DeviceHardware::htmlEscape(appCfg.pass_root); html += F("'>");
  html += F("<small>"); html += (appCfg.ui_lang == 0 ? F("Protejează toate paginile (status/config/securitate/actualizare).") : F("Protects all pages (status/config/security/update).")); html += F("</small>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează Acces Rețea") : F("Save Net Access")); html += F("</button>");
  html += F("</form>");
  html += F("</div>");

  html += F("<div class='card'>");
  html += F("<form method='post' action='/save_security'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Acces Configurare") : F("Config Access")); html += F("</h2>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Activează Autentificare Configurare:") : F("Enable Config Auth:")); html += F("</label>");

  html += F("<div style='display:flex;gap:5px;margin-top:5px;margin-bottom:5px'>");
  html += F("<a href='/save_security?auth_config=0' style='text-decoration:none;padding:7px 15px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;flex:1;text-align:center;");
  html += (!appCfg.auth_config ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  html += F("'>OFF</a>");
  html += F("<a href='/save_security?auth_config=1' style='text-decoration:none;padding:7px 15px;border:1px solid #007bff;border-radius:4px;font-size:12px;font-weight:bold;flex:1;text-align:center;");
  html += (appCfg.auth_config ? F("background:#007bff;color:#fff") : F("background:#fff;color:#007bff"));
  html += F("'>ON</a></div>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Nume Utilizator Configurare:") : F("Config Username:")); html += F("</label>");
  html += F("<input type='text' name='user_config' value='"); html += DeviceHardware::htmlEscape(appCfg.user_config); html += F("'>");
  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Parolă Configurare:") : F("Config Password:")); html += F("</label>");
  html += F("<input type='password' name='pass_config' value='"); html += DeviceHardware::htmlEscape(appCfg.pass_config); html += F("'>");
  html += F("<small>"); html += (appCfg.ui_lang == 0 ? F("Adaugă o a doua poartă pentru rutele de config/securitate/actualizare.") : F("Adds second gate for config/security/update/routes.")); html += F("</small>");
  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează Acces Config") : F("Save Config Access")); html += F("</button>");
  html += F("</form>");
  html += F("</div>");

  html += F("</div>");

  html += F("<div class='card'>");
  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Mod OTA & Configurare") : F("OTA & Setup Mode")); html += F("</h2>");

  // OTA Password Form (Horizontal)
  html += F("<form method='post' action='/save_security' style='flex-direction:row;align-items:flex-end;gap:10px;margin-bottom:15px'>");
  html += F("<div style='flex:1'><label>"); html += (appCfg.ui_lang == 0 ? F("Parolă OTA:") : F("OTA Password:")); html += F("</label>");
  html += F("<input type='password' name='ota_pass' value='"); html += DeviceHardware::htmlEscape(appCfg.ota_pass); html += F("'></div>");
  html += F("<button type='submit' style='margin-top:0;width:auto;white-space:nowrap'>"); html += (appCfg.ui_lang == 0 ? F("Salvează") : F("Save")); html += F("</button>");
  html += F("</form>");

  // Setup AP Password Form (Horizontal)
  html += F("<form method='post' action='/save_security' style='flex-direction:row;align-items:flex-end;gap:10px'>");
  html += F("<div style='flex:1'><label>"); html += (appCfg.ui_lang == 0 ? F("Parolă AP Configurare:") : F("Setup AP Password:")); html += F("</label>");
  html += F("<input type='password' name='setup_ap_pass' value='"); html += DeviceHardware::htmlEscape(appCfg.setup_ap_pass); html += F("'></div>");
  html += F("<button type='submit' style='margin-top:0;width:auto;white-space:nowrap'>"); html += (appCfg.ui_lang == 0 ? F("Salvează") : F("Save")); html += F("</button>");
  html += F("</form>");

  html += F("<small>"); 
  html += (appCfg.ui_lang == 0 ? F("Utilizată în modul AP de configurare (") : F("Used in setup mode AP ("));
  html += hw.getSetupSsid();
  html += F("). Min 8 caractere.</small>");
  html += F("</div>");

  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void pageSetup() {
  String html;
  html.reserve(3000);
  String defaultHost = hw.getDefaultHostname();

  int wifiCount = WiFi.scanNetworks();

  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Setup</title>");
  html += getCommonCss();
  html += F("</head><body>");

  html += F("<div class='card'>");
  html += getCommonHeader(appCfg.ui_lang == 0 ? F("Configurare Inițială") : F("Initial Setup"), F("/"));

  html += F("<h2>"); html += (appCfg.ui_lang == 0 ? F("Conectare WiFi și MQTT") : F("Connect to WiFi and MQTT")); html += F("</h2>");
  html += F("<form method='post' action='/setup_save'>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("SSID WiFi:") : F("WiFi SSID:")); html += F("</label>");
  html += F("<input type='text' name='ssid' list='ssid_list' placeholder='"); html += (appCfg.ui_lang == 0 ? F("Selectează sau scrie SSID") : F("Select or type WiFi SSID")); html += F("'>");
  html += F("<datalist id='ssid_list'>");
  if (wifiCount > 0) {
    for (int i = 0; i < wifiCount; i++) {
      String ssid_val = WiFi.SSID(i);
      if (ssid_val.length() == 0) continue;
      html += F("<option value='"); html += DeviceHardware::htmlEscape(ssid_val); html += F("'>");
    }
    html += F("</datalist>");
    html += F("<small style='color:#666'>"); html += (appCfg.ui_lang == 0 ? F("Rețele detectate: ") : F("Networks detected: ")); html += wifiCount; html += F("</small>");
  } else {
    html += F("</datalist>");
    html += F("<small style='color:#666'>"); html += (appCfg.ui_lang == 0 ? F("Nu s-au detectat rețele acum. Introdu manual SSID.") : F("No networks detected now. Enter SSID manually.")); html += F("</small>");
  }

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Parolă WiFi:") : F("WiFi Password:")); html += F("</label>");
  html += F("<input type='password' name='pass'>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Gazdă Broker MQTT:") : F("MQTT Broker Host:")); html += F("</label>");
  html += F("<input type='text' name='host' placeholder='"); html += (appCfg.ui_lang == 0 ? F("ex: mqtt.local sau 192.168.1.100") : F("mqtt.local or 192.168.1.100")); html += F("'>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Port Broker MQTT:") : F("MQTT Broker Port:")); html += F("</label>");
  html += F("<input type='number' name='port' value='1883'>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Topic Principal MQTT:") : F("MQTT Topic Base:")); html += F("</label>");
  html += F("<input type='text' name='topic' value='"); html += defaultHost; html += F("'>");

  html += F("<label>"); html += (appCfg.ui_lang == 0 ? F("Activează MQTT:") : F("Enable MQTT:")); html += F("</label>");
  html += F("<select name='enabled'><option value='0'>No</option><option value='1' selected>Yes</option></select>");

  html += F("<button type='submit'>"); html += (appCfg.ui_lang == 0 ? F("Salvează și Repornește") : F("Save & Reboot")); html += F("</button>");
  html += F("</form>");
  html += F("</div>");
  html += F("</body></html>");

  server.send(200, "text/html", html);
}

void handleSaveConnection() {
  if (!checkConfigAccess()) return;

  bool wifiChanged = false;
  if (server.hasArg("ssid")) {
    if (strcmp(wifiCfg.ssid, server.arg("ssid").c_str()) != 0) wifiChanged = true;
    strlcpy(wifiCfg.ssid, server.arg("ssid").c_str(), sizeof(wifiCfg.ssid));
  }
  if (server.hasArg("pass")) strlcpy(wifiCfg.pass, server.arg("pass").c_str(), sizeof(wifiCfg.pass));
  if (server.hasArg("host")) strlcpy(mqttCfg.host, server.arg("host").c_str(), sizeof(mqttCfg.host));
  if (server.hasArg("port")) mqttCfg.port = (uint16_t)server.arg("port").toInt();
  if (server.hasArg("user")) strlcpy(mqttCfg.user, server.arg("user").c_str(), sizeof(mqttCfg.user));
  if (server.hasArg("client_id")) strlcpy(mqttCfg.client_id, server.arg("client_id").c_str(), sizeof(mqttCfg.client_id));
  if (server.hasArg("mqtpass")) strlcpy(mqttCfg.pass, server.arg("mqtpass").c_str(), sizeof(mqttCfg.pass));
  if (server.hasArg("topic")) strlcpy(mqttCfg.topic, server.arg("topic").c_str(), sizeof(mqttCfg.topic));
  if (server.hasArg("enabled")) mqttCfg.enabled = (server.arg("enabled") == "1");

  saveWifiConfig();
  saveMqttConfig();
  configDirty = false; // Resetam flag-ul pentru a evita scrierea dubla in loop

  if (wifiChanged) {
    server.send(200, "text/plain", "WiFi Changed. Rebooting to connect to new network...");
    delay(1000);
    ESP.restart();
  } else {
    mqtt.disconnect(); // Fortam reconectarea MQTT cu noile setari (host/user/pass)
    lastMqttReconnectAttempt = 0;
    sendRedirect("/config?saved=1");
  }
}

void handleSaveTopic() {
  if (!checkConfigAccess()) return;
  if (!server.hasArg("topic")) {
    server.send(400, "text/plain", "Missing topic");
    return;
  }
  strlcpy(mqttCfg.topic, server.arg("topic").c_str(), sizeof(mqttCfg.topic));
  markConfigDirty();
  
  mqtt.disconnect(); // Reconectare pentru a aplica noul topic de subscriere
  lastMqttReconnectAttempt = 0;
  sendRedirect("/config?saved=1");
}

void handleSaveMdns() {
  if (!checkConfigAccess()) return;
  if (!server.hasArg("mdns")) {
    server.send(400, "text/plain", "Missing mdns");
    return;
  }
  String h = server.arg("mdns");
  h.toLowerCase();
  if (!isValidMdnsHostname(h)) {
    server.send(400, "text/plain", "Invalid mdns hostname");
    return;
  }
  strlcpy(mqttCfg.mdns_hostname, h.c_str(), sizeof(mqttCfg.mdns_hostname));
  markConfigDirty();

  MDNS.begin(mqttCfg.mdns_hostname);
  ArduinoOTA.setHostname(mqttCfg.mdns_hostname);

  sendRedirect("/config?saved=1");
}

void handleSaveInterval() {
  if (!checkConfigAccess()) return;
  if (!server.hasArg("pub_interval")) {
    server.send(400, "text/plain", "Missing pub_interval");
    return;
  }
  int v = server.arg("pub_interval").toInt();
  if (v < 1) v = 1;
  if (v > 3600) v = 3600;
  mqttCfg.pub_interval = (uint16_t)v;
  markConfigDirty();
  sendRedirect("/config?saved=1");
}

void handleSaveMqttControl() {
  if (!checkConfigAccess()) return;
  mqttCfg.enabled = server.arg("enabled") == "1";
  markConfigDirty();

  if (!mqttCfg.enabled) {
    mqtt.disconnect();
  } else {
    lastMqttReconnectAttempt = 0; // Trigger reconectare imediata
  }

  sendRedirect("/config?saved=1");
}

void handleTestMqtt() {
  if (!checkConfigAccess()) return;
  
  // Verificăm dacă suntem conectați la WiFi, altfel testul va eșua garantat
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json", "{\"connected\":false,\"host\":\"WiFi Not Connected\"}");
    return;
  }

  String tHost = server.arg("host");
  uint16_t tPort = (uint16_t)server.arg("port").toInt();
  if (tPort == 0) tPort = 1883;
  String tUser = server.arg("user");
  String tPass = server.arg("mqtpass");

  if (tHost.length() == 0) {
    server.send(200, "application/json", "{\"connected\":false,\"error\":\"No host\"}");
    return;
  }

  // Folosim instante locale pentru a nu afecta conexiunea curenta
  WiFiClient testClient;
  PubSubClient testMqtt(testClient);
  
  // Scurtam timeout-ul pentru a returna eroarea rapid si a nu bloca WDT
  testClient.setTimeout(3000); 
  testMqtt.setServer(tHost.c_str(), tPort);

  // Generam un ID unic pentru test ca sa nu intram in conflict cu sesiunea activa
  String testId = "Test-" + String(ESP.getChipId(), HEX);

  bool success = false;
  if (tUser.length() > 0) {
    success = testMqtt.connect(testId.c_str(), tUser.c_str(), tPass.c_str());
  } else {
    success = testMqtt.connect(testId.c_str());
  }

  int mState = testMqtt.state();
  if (success) testMqtt.disconnect();

  StaticJsonDocument<128> doc;
  doc["connected"] = success;
  doc["host"] = tHost;
  if (!success) doc["state"] = mState;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSaveLang() {
  if (!setupMode) {
    if (!checkConfigAccess()) return;
  }
  int lang = server.arg("lang").toInt();
  appCfg.ui_lang = (lang == 1) ? 1 : 0;
  markConfigDirty();
  String target = setupMode ? "/" : "/config";
  if (server.hasArg("from")) {
    target = server.arg("from");
  }
  sendRedirect(target);
}

void handleSaveSecurity() {
  if (!checkConfigAccess()) return;

  if (server.hasArg("auth_root")) appCfg.auth_root = server.arg("auth_root") == "1";
  if (server.hasArg("auth_config")) appCfg.auth_config = server.arg("auth_config") == "1";

  if (server.hasArg("user_root")) strlcpy(appCfg.user_root, server.arg("user_root").c_str(), sizeof(appCfg.user_root));
  if (server.hasArg("pass_root")) strlcpy(appCfg.pass_root, server.arg("pass_root").c_str(), sizeof(appCfg.pass_root));
  if (server.hasArg("user_config")) strlcpy(appCfg.user_config, server.arg("user_config").c_str(), sizeof(appCfg.user_config));
  if (server.hasArg("pass_config")) strlcpy(appCfg.pass_config, server.arg("pass_config").c_str(), sizeof(appCfg.pass_config));
  if (server.hasArg("setup_ap_pass")) {
    String s = server.arg("setup_ap_pass");
    if (s.length() >= 8 && s.length() <= 31) {
      strlcpy(appCfg.setup_ap_pass, s.c_str(), sizeof(appCfg.setup_ap_pass));
    }
  }
  
  if (server.hasArg("ota_pass")) {
    String ota = server.arg("ota_pass");
    ota.trim(); // Elimina spatii accidentale
    strlcpy(appCfg.ota_pass, ota.c_str(), sizeof(appCfg.ota_pass));
    
    if (ota.length() > 0) {
      ArduinoOTA.setPassword(appCfg.ota_pass);
    }
  }

  markConfigDirty();
  sendRedirect("/security?saved=1");
}

void handleReset() {
  if (!checkConfigAccess()) return;
  server.send(200, "text/plain", "Rebooting...");
  delay(300);
  ESP.restart();
}

void handleUpdatePage() {
  if (!checkConfigAccess()) return;
  uint32_t maxS = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  String html;
  html.reserve(4200);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>"); html += (appCfg.ui_lang == 0 ? F("Actualizare") : F("Update")); html += F("</title>");
  html += getCommonCss();
  html += F("<style>.file-area{position:relative;margin-bottom:25px}.file-area input[type=file]{position:absolute;width:100%;height:100%;opacity:0;cursor:pointer;left:0;top:0}.file-dummy{padding:15px;background:#f8f9fa;border:2px dashed #cbd5e0;border-radius:8px;font-size:14px;color:#718096;transition:all .2s}.file-area:hover .file-dummy{background:#edf2f7;border-color:#a0aec0}.prg-c{display:none;margin-top:10px}.p-bar-bg{background:#edf2f7;border-radius:8px;height:14px;overflow:hidden;margin-bottom:10px}.p-bar{background:#007bff;height:100%;width:0%;transition:width .3s}</style>");
  html += F("</head><body>");

  html += getCommonHeader(appCfg.ui_lang == 0 ? F("Actualizare Firmware") : F("Firmware Update"), F("/update"));
  html += getCommonNav();

  html += F("<div class='card'>");
  html += F("<p id='msg' style='margin-bottom:10px'>"); html += (appCfg.ui_lang == 0 ? F("Selectați fișierul .bin.") : F("Select the .bin file.")); html += F("</p>");
  
  html += F("<div style='margin-bottom:15px; font-size:13px; color:#4a5568'>");
  html += F("<b>Version:</b> "); html += FW_VERSION; html += F(" | <b>Chip ID:</b> "); html += String(ESP.getChipId(), HEX);
  html += F("</div>");

  // Informații spațiu disponibil
  html += F("<div style='background:#fff5f5;border:1px solid #feb2b2;padding:10px;border-radius:6px;margin-bottom:20px;font-size:13px;color:#c53030'>");
  html += (appCfg.ui_lang == 0 ? F("<b>Spațiu disponibil:</b> ") : F("<b>Available space:</b> "));
  html += (maxS / 1024); html += F(" KB<br>");
  html += (appCfg.ui_lang == 0 ? F("Fișierul nu trebuie să depășească această limită.") : F("The file must not exceed this limit."));
  html += F("</div>");

  html += F("<form id='uf' onsubmit='event.preventDefault();upload(this);'>");
  html += F("<div class='file-area' id='fa'><div class='file-dummy' id='fn'>");
  html += (appCfg.ui_lang == 0 ? F("Apasă pentru a alege fișierul") : F("Click to choose file"));
  html += F("</div><input type='file' name='update' accept='.bin' required onchange=\"document.getElementById('fn').innerText=this.files[0].name;document.getElementById('fn').style.borderStyle='solid';document.getElementById('fn').style.color='#2d3748'\"></div>");
  html += F("<button type='submit' id='ub'>");
  html += (appCfg.ui_lang == 0 ? F("Verifică și Încarcă") : F("Verify and Upload"));
  html += F("</button></form>");
  html += F("<div id='pc' class='prg-c'><div class='p-bar-bg'><div id='pb' class='p-bar'></div></div><p id='pt' style='font-weight:600;color:#2d3748'>0%</p></div>");
  html += F("<a href='/config' id='bk' style='display:block;margin-top:20px;text-align:center;color:#007bff;text-decoration:none'>&larr; "); html += (appCfg.ui_lang == 0 ? F("Înapoi la Configurări") : F("Back to Settings")); html += F("</a>");
  html += F("<script>");
  html += F("var maxS="); html += maxS; html += F(";var lang="); html += appCfg.ui_lang; html += F(";");
  html += F("function upload(f){var file=f.update.files[0];if(!file)return;");
  // Verificare integritate client-side (extensie și mărime)
  html += F("if(!file.name.toLowerCase().endsWith('.bin')){alert(lang==0?'Eroare: Doar fișiere .bin sunt permise.':'Error: Only .bin files are allowed.');return;}");
  html += F("if(file.size > maxS){alert((lang==0?'Fișierul este prea mare! Maxim: ':'File too large! Max: ')+Math.round(maxS/1024)+' KB');return;}");
  
  html += F("var d=new FormData(f);var x=new XMLHttpRequest();");
  html += F("x.upload.addEventListener('progress',function(e){if(e.lengthComputable){var p=Math.round((e.loaded/e.total)*100);document.getElementById('pb').style.width=p+'%';document.getElementById('pt').innerText=p+'%';}});");
  html += F("x.onload=function(){if(x.status===200){document.getElementById('pt').innerText='");
  html += (appCfg.ui_lang == 0 ? F("SUCCES! Repornire...") : F("SUCCESS! Rebooting..."));
  html += F("';setTimeout(function(){window.location='/';},4500);}else{alert('Error: '+x.responseText);location.reload();}};");
  html += F("x.open('POST','/update');x.send(d);");
  html += F("document.getElementById('pc').style.display='block';document.getElementById('fa').style.display='none';document.getElementById('ub').style.display='none';document.getElementById('bk').style.display='none';");
  html += F("document.getElementById('msg').innerText='");
  html += (appCfg.ui_lang == 0 ? F("Actualizarea este în curs. Vă rugăm să nu închideți pagina.") : F("Update in progress. Please do not close this page."));
  html += F("';}");
  html += F("</script>");
  html += F("</div></body></html>");
  server.send(200, "text/html", html);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    WiFiUDP::stopAll();
    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      Update.printError(Serial);
    }
  }
}

void handleUpdateFinish() {
  if (!checkConfigAccess()) return;

  if (Update.hasError()) {
    server.send(500, "text/plain", "OTA failed");
  } else {
    server.send(200, "text/plain", "OTA success. Rebooting...");
    delay(250);
    ESP.restart();
  }
}

void setupRoutesNormal() {
  server.on("/", pageStatus);
  server.on("/config", pageConfig);
  server.on("/security", pageSecurity);

  server.on("/save_connection", HTTP_POST, handleSaveConnection);
  server.on("/save_topic", HTTP_POST, handleSaveTopic);
  server.on("/save_mdns", HTTP_POST, handleSaveMdns);
  server.on("/save_interval", HTTP_POST, handleSaveInterval);
  server.on("/save_mqtt_control", HTTP_POST, handleSaveMqttControl);
  server.on("/save_lang", handleSaveLang);
  server.on("/save_security", handleSaveSecurity);
  server.on("/api/test_mqtt", handleTestMqtt);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/factory_reset", handleFactoryReset);

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);

  // Rute hardware (calibrare, etc.) - inregistrate direct din headerul de hardware
    hw.registerHardwareRoutes(server, appCfg, mqtt, mqttCfg, markConfigDirty);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
}

void setupRoutesSetup() {
  server.on("/", pageSetup);
  server.on("/save_lang", handleSaveLang);
  server.on("/setup_save", HTTP_POST, []() {
    if (server.hasArg("ssid")) strlcpy(wifiCfg.ssid, server.arg("ssid").c_str(), sizeof(wifiCfg.ssid));
    if (server.hasArg("pass")) strlcpy(wifiCfg.pass, server.arg("pass").c_str(), sizeof(wifiCfg.pass));
    if (server.hasArg("host")) strlcpy(mqttCfg.host, server.arg("host").c_str(), sizeof(mqttCfg.host));
    if (server.hasArg("port")) mqttCfg.port = (uint16_t)server.arg("port").toInt();
    if (server.hasArg("topic")) strlcpy(mqttCfg.topic, server.arg("topic").c_str(), sizeof(mqttCfg.topic));
    mqttCfg.enabled = server.arg("enabled") == "1";

    saveWifiConfig();
    saveMqttConfig();
    saveAppConfig();

    server.send(200, "text/plain", "Saved. Rebooting...");
    delay(300);
    ESP.restart();
  });

  server.onNotFound([]() {
    sendRedirect("/");
  });
}

void handlePowerCycleReset() {
  uint8_t count = 0;
  if (LittleFS.exists("/pwr_cyc.bin")) {
    File f = LittleFS.open("/pwr_cyc.bin", "r");
    if (f) {
      f.read(&count, 1);
      f.close();
    }
  }

  count++;

  if (count >= 5) {
    LittleFS.remove("/pwr_cyc.bin");
    LittleFS.remove("/wifi.bin");
    LittleFS.remove("/mqtt.bin");
    LittleFS.remove("/app.bin");
    LittleFS.remove("/hwcfg.bin");
    ESP.restart();
  } else {
    File f = LittleFS.open("/pwr_cyc.bin", "w");
    if (f) {
      f.write(&count, 1);
      f.close();
    }
  }
}

void handleFactoryReset() {
  if (!checkConfigAccess()) return;
  
  String h = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>");
  h += F("body{font-family:sans-serif;background:#f4f7f9;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0}");
  h += F(".card{background:#fff;padding:30px;border-radius:12px;box-shadow:0 8px 16px rgba(0,0,0,0.1);text-align:center;max-width:400px}");
  h += F("h2{color:#dc3545}p{color:#666;line-height:1.5}b{color:#333}</style></head><body><div class='card'><h2>");
  h += (appCfg.ui_lang == 0 ? F("Resetare Finalizată") : F("Reset Complete"));
  h += F("</h2><p>");
  h += (appCfg.ui_lang == 0 ? F("Dispozitivul a fost resetat la valorile din fabrică.<br>Conectați-vă la WiFi: <b>") : F("Device has been reset to factory defaults.<br>Please connect to WiFi: <b>"));
  h += hw.getSetupSsid();
  h += F("</b> pentru configurare.</p></div></body></html>");

  server.send(200, "text/html", h);
  delay(1000);
  requestFactoryReset = true; // Setăm flag-ul pentru loop()
}

// ================= SETUP & LOOP =================
void startSetupApMode() {
  setupMode = true;
  WiFi.mode(WIFI_AP_STA);
  const char* setupPass = (strlen(appCfg.setup_ap_pass) >= 8) ? appCfg.setup_ap_pass : DEFAULT_AP_PASS;
  WiFi.softAP(hw.getSetupSsid().c_str(), setupPass);
  setupRoutesSetup();
  server.begin();
}

void startNormalMode() {
  setupMode = false;
  setupRoutesNormal();
  server.begin();
  ws.begin();

  MDNS.begin(mqttCfg.mdns_hostname);
  MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(mqttCfg.mdns_hostname);
  if (appCfg.ota_pass[0] != '\0') {
    ArduinoOTA.setPassword(appCfg.ota_pass);
  }
  ArduinoOTA.begin();

  mqtt.setServer(mqttCfg.host, mqttCfg.port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
}

bool connectWiFiStation() {
  if (strlen(wifiCfg.ssid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiCfg.ssid, wifiCfg.pass);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT) {
    delay(150);
  }

  return WiFi.status() == WL_CONNECTED;
}

void setupEvents() {
  wifiConnectHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) {
    wifiConnectCount++;
    hw.setStatusConnected(true);
  });

  wifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected&) {
    wifiDisconnectCount++;
    hw.setStatusConnected(false);
  });
}

void setup() {
  Serial.begin(115200);
  delay(20);

  loadConfigs();
  setupEvents();

  hw.begin();
  hw.syncHardwareFromConfig(appCfg);
  hw.applyBootState(appCfg);

  handlePowerCycleReset();

  if (connectWiFiStation()) {
    startNormalMode();
  } else {
    startSetupApMode();
  }
}

void loop() {
  server.handleClient();
  if (!setupMode) ws.loop();
  
  if (!setupMode) {
    ArduinoOTA.handle();
    mqttEnsureConnected();
    mqtt.loop();

    bool mqttNowConnected = mqtt.connected();
    if (mqttNowConnected != lastMqttConnectedState) {
      lastMqttConnectedState = mqttNowConnected;
      wsBroadcastUpdate(); // Actualizare live în loc de refresh pagină
    }

    if (!pwrCycCleared && millis() > 3000) {
      if (LittleFS.exists("/pwr_cyc.bin")) LittleFS.remove("/pwr_cyc.bin");
      pwrCycCleared = true;
    }

    // UI live refresh (status cards + sensors)
    if (millis() - lastUiRealtimePush >= 2000UL) {
      lastUiRealtimePush = millis();
      wsBroadcastUpdate();
    }

    if (mqttCfg.enabled && mqtt.connected()) {
      unsigned long publishIntervalMs = (unsigned long)mqttCfg.pub_interval * 1000UL;
      if (publishIntervalMs < MQTT_PUB_INTERVAL_MS) publishIntervalMs = MQTT_PUB_INTERVAL_MS;
      if (millis() - lastMqttPublish >= publishIntervalMs) {
        hw.publishAllState(mqtt, mqttCfg, appCfg);
        lastMqttPublish = millis();
      }
    }
  }

  hw.loop();
  
    bool hwResetRequested = false;
    bool hwStateChanged = false;
    hw.processRuntimeEvents(appCfg, WiFi.status(), hwResetRequested, hwStateChanged);
    if (hwResetRequested) requestFactoryReset = true;

    if (hwStateChanged) {
      if (!setupMode && mqttCfg.enabled && mqtt.connected()) {
        hw.publishAllState(mqtt, mqttCfg, appCfg);
        lastMqttPublish = millis();
      }
      if (!setupMode) wsBroadcastUpdate();
      markConfigDirty();
    }
  
  if (requestFactoryReset) {
    LittleFS.remove("/wifi.bin");
    LittleFS.remove("/mqtt.bin");
    LittleFS.remove("/app.bin");
    LittleFS.remove("/hwcfg.bin");
    delay(500);
    ESP.restart();
    return;
  }

  saveAllConfigsIfDirty();
}
