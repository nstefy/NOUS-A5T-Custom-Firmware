#pragma once

#include <LittleFS.h>
#include <SoftwareSerial.h>

#define DEVICE_RELAY_COUNT 4
#define DEVICE_MODEL       "Nous A5T"
#define DEFAULT_AP_SSID    "NOUS-Setup"
#define DEFAULT_AP_PASS    "noussetup"
#define FW_VERSION_NUMBER  "3.0.0"
#define FW_VERSION_FULL    DEVICE_MODEL " v." FW_VERSION_NUMBER

struct SensorSnapshot {
  float voltage = 0.0f;
  float current = 0.0f;
  float power   = 0.0f;
  float pf      = 0.0f;
  float energy  = 0.0f; // kWh accumulated
};

// Implementare hardware pentru NOUS A5T
// - Relays: GPIO14, GPIO12, GPIO13, GPIO5 (relay 4 inversat)
// - Buttons pe ADC (A0): 3 butoane fizice pentru relay 1..3
// - Buton digital GPIO16: short press = toggle all, long press >10s = factory reset event
// - LED status: GPIO2 (inverted)
// - CSE7766 pe SoftwareSerial RX=GPIO3, TX=GPIO1

class DeviceHardware {
public:
  String getSetupSsid() const {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s-%06X", DEFAULT_AP_SSID, (uint32_t)ESP.getChipId());
    return String(buf);
  }

  void applyConfigDefaults(AppConfig& appCfg) {
    (void)appCfg;
    cfgChildLock = false;
    cfgPowerOnBehavior = 2;
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) cfgRelayState[i] = false;
    calV = DEFAULT_CAL_V;
    calI = DEFAULT_CAL_I;
    calP = DEFAULT_CAL_P;
  }

  void validateConfig(AppConfig& appCfg) {
    (void)appCfg;
    if (calV < MIN_CAL_FACTOR || calV > MAX_CAL_FACTOR) calV = DEFAULT_CAL_V;
    if (calI < MIN_CAL_FACTOR || calI > MAX_CAL_FACTOR) calI = DEFAULT_CAL_I;
    if (calP < MIN_CAL_FACTOR || calP > MAX_CAL_FACTOR) calP = DEFAULT_CAL_P;
    if (cfgPowerOnBehavior > 2) cfgPowerOnBehavior = 2;
  }

  void syncHardwareFromConfig(const AppConfig& appCfg) {
    (void)appCfg;
  }

  bool syncConfigFromHardware(AppConfig& appCfg) const {
    (void)appCfg;
    return false;
  }

  void applyPowerOnBehavior(AppConfig& appCfg) {
    (void)appCfg;
    if (cfgPowerOnBehavior == 0) {
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) cfgRelayState[i] = false;
    } else if (cfgPowerOnBehavior == 1) {
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) cfgRelayState[i] = true;
    }
  }

  void applyBootState(AppConfig& appCfg) {
    applyPowerOnBehavior(appCfg);
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      setRelay(i, cfgRelayState[i]);
    }
  }

  bool isInputLocked(const AppConfig& appCfg) const {
    (void)appCfg;
    return cfgChildLock;
  }

  void toggleInputLock(AppConfig& appCfg) {
    (void)appCfg;
    cfgChildLock = !cfgChildLock;
    markHwCfgDirty();
  }

  bool setPowerOnBehavior(AppConfig& appCfg, int pob) {
    (void)appCfg;
    if (pob < 0 || pob > 2) return false;
    if (cfgPowerOnBehavior == (uint8_t)pob) return true;
    cfgPowerOnBehavior = (uint8_t)pob;
    markHwCfgDirty();
    return true;
  }

  bool handleHttpToggle(AppConfig& appCfg, int id) {
    if (id < 0 || id >= DEVICE_RELAY_COUNT) return false;
    if (isInputLocked(appCfg)) return false;
    bool nextState = !cfgRelayState[id];
    if (!setRelay(id, nextState)) return false;
    cfgRelayState[id] = nextState;
    markHwCfgDirty();
    return true;
  }

  bool handleCoreMqttCommand(const String& sub, const String& msg, AppConfig& appCfg, bool& publishAll, int& publishIdx) {
    publishAll = false;
    publishIdx = -1;

    if (sub == "/relay/all/set") {
      bool s;
      if (!parseOnOff(msg, s)) return false;
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
        setRelay(i, s);
        cfgRelayState[i] = s;
      }
      markHwCfgDirty();
      publishAll = true;
      return true;
    }

    if (sub.startsWith("/relay/") && sub.endsWith("/set")) {
      String idxStr = sub.substring(7, sub.length() - 4);
      int idx = idxStr.toInt();
      bool s;
      if (idx < 0 || idx >= DEVICE_RELAY_COUNT) return false;
      if (!parseOnOff(msg, s)) return false;
      if (!setRelay(idx, s)) return false;
      cfgRelayState[idx] = s;
      markHwCfgDirty();
      publishIdx = idx;
      return true;
    }

    return false;
  }

  void publishCoreDiscovery(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const String& mac, const char* fwVersion) const {
    if (!mqttClient.connected()) return;

    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      String topic = "homeassistant/switch/" + mac + "/relay_" + String(i) + "/config";
      String payload;
      payload.reserve(512);
      payload += F("{\"name\":\"Relay "); payload += (i + 1);
      payload += F("\",\"uniq_id\":\""); payload += mac; payload += F("_relay_"); payload += i;
      payload += F("\",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/relay/"); payload += i;
      payload += F("\",\"cmd_t\":\""); payload += mqttCfg.topic; payload += F("/relay/"); payload += i; payload += F("/set\"");
      payload += F(",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");
      payload += F(",\"dev\":{\"ids\":[\""); payload += mac;
      payload += F("\"],\"name\":\"NOUS A5T\",\"mf\":\"NOUS\",\"mdl\":\"A5T\",\"sw\":\""); payload += fwVersion;
      payload += F("\",\"avty_t\":\""); payload += mqttCfg.topic; payload += F("/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"}}");
      mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }

    const char* sensorNames[]      = {"voltage", "current", "power", "pf", "energy"};
    const char* sensorUnits[]      = {"V", "A", "W", "", "kWh"};
    const char* sensorClasses[]    = {"voltage", "current", "power", "power_factor", "energy"};
    const char* sensorStateClass[] = {"", "", "measurement", "", "total_increasing"};

    for (int i = 0; i < 5; i++) {
      String topic = "homeassistant/sensor/" + mac + "/" + sensorNames[i] + "/config";
      String payload;
      payload.reserve(512);
      payload += F("{\"name\":\""); payload += sensorNames[i]; payload += F("\"");
      payload += F(",\"uniq_id\":\""); payload += mac; payload += "_"; payload += sensorNames[i]; payload += F("\"");
      payload += F(",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/sensors\"");
      payload += F(",\"val_tpl\":\"{{ value_json."); payload += sensorNames[i]; payload += F(" }}\"");
      if (strlen(sensorUnits[i]) > 0) {
        payload += F(",\"unit_of_meas\":\""); payload += sensorUnits[i]; payload += F("\"");
      }
      if (strlen(sensorClasses[i]) > 0) {
        payload += F(",\"dev_cla\":\""); payload += sensorClasses[i]; payload += F("\"");
      }
      if (strlen(sensorStateClass[i]) > 0) {
        payload += F(",\"stat_cla\":\""); payload += sensorStateClass[i]; payload += F("\"");
      }
      payload += F(",\"dev\":{\"ids\":[\""); payload += mac;
      payload += F("\"],\"name\":\"NOUS A5T\",\"sw\":\""); payload += fwVersion; payload += F("\",\"avty_t\":\""); payload += mqttCfg.topic; payload += F("/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"}}");
      mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }
  }

  void processRuntimeEvents(AppConfig& appCfg, wl_status_t wifiStatus, bool& requestFactoryReset, bool& stateChanged) {
    (void)appCfg;
    requestFactoryReset = false;
    stateChanged = false;

    if (consumeForceAllOffEvent()) {
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
        if (cfgRelayState[i]) stateChanged = true;
        cfgRelayState[i] = false;
        setRelay(i, false);
      }
    }

    if (consumeFactoryResetEvent() && allowFactoryReset(appCfg, wifiStatus)) {
      requestFactoryReset = true;
      return;
    }

    if (isInputLocked(appCfg)) return;

    if (consumeToggleAllEvent()) {
      bool anyOff = false;
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
        if (!cfgRelayState[i]) { anyOff = true; break; }
      }
      bool newState = anyOff;
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
        if (cfgRelayState[i] != newState) stateChanged = true;
        cfgRelayState[i] = newState;
        setRelay(i, newState);
      }
    }

    int idx = -1;
    while (consumeRelayToggleEvent(idx)) {
      if (idx >= 0 && idx < DEVICE_RELAY_COUNT) {
        bool nextState = !cfgRelayState[idx];
        cfgRelayState[idx] = nextState;
        setRelay(idx, nextState);
        stateChanged = true;
      }
      idx = -1;
    }

    if (stateChanged) markHwCfgDirty();
  }

  bool allowFactoryReset(const AppConfig& appCfg, wl_status_t wifiStatus) const {
    (void)appCfg;
    return !cfgChildLock && wifiStatus != WL_CONNECTED;
  }

  // Shared helper used by active hardware-rendered UI blocks
  static String htmlEscape(const String& input) {
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); i++) {
      char c = input[i];
      switch (c) {
        case '&': out += F("&amp;"); break;
        case '<': out += F("&lt;"); break;
        case '>': out += F("&gt;"); break;
        case '"': out += F("&quot;"); break;
        case '\'': out += F("&#39;"); break;
        default: out += c; break;
      }
    }
    return out;
  }

  void begin() {
    // Recomandat pentru stabilitate ESP8285
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.persistent(false);

    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      pinMode(RELAY_PINS[i], OUTPUT);
      relays[i] = false;
      writeRelayPin(i, false);
    }

    pinMode(BTN_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // OFF (inverted)

    // CSE7766 init
    Serial.end();
    cseSerial.begin(4800, SWSERIAL_8E1);
    pinMode(3, INPUT_PULLUP);
    delay(100);
    cseSerial.write(0x55);

    sensors.voltage = 0.0f;
    sensors.current = 0.0f;
    sensors.power = 0.0f;
    sensors.pf = 0.0f;
    sensors.energy = 0.0f;
    energyWh = 0.0;
    lastEnergyMs = 0;

    btnStable = 0;
    btnLastRaw = 0;
    lastDebounce = 0;
    pendingToggle = -1;

    lastBtnDigital = HIGH;
    btnDigitalPressTime = 0;

    pendingToggleAll = false;
    pendingFactoryReset = false;

    cseIdx = 0;
    lastCsePacketTime = 0;
    lastCseRetry = 0;
    lastCseByteMs = 0;

    ledConnected = false;
    ledBlinkState = false;
    lastLedBlink = 0;

    loadHwCfg();
  }

  void loop() {
    pollButtons();
    pollCse7766();
    retryCseIfNeeded();
    refreshStatusLed();
    flushHwCfgIfDirty();
  }

  bool setRelay(int idx, bool state) {
    if (idx < 0 || idx >= DEVICE_RELAY_COUNT) return false;
    relays[idx] = state;
    writeRelayPin(idx, state);
    return true;
  }

  bool getRelay(int idx) const {
    if (idx < 0 || idx >= DEVICE_RELAY_COUNT) return false;
    return relays[idx];
  }

  uint8_t getPowerOnBehavior() const { return cfgPowerOnBehavior; }

  SensorSnapshot readSensors() const {
    return sensors;
  }

  void setCalibrationFactors(float v, float i, float p) {
    bool changed = false;
    if (v >= MIN_CAL_FACTOR && v <= MAX_CAL_FACTOR && v != calV) { calV = v; changed = true; }
    if (i >= MIN_CAL_FACTOR && i <= MAX_CAL_FACTOR && i != calI) { calI = i; changed = true; }
    if (p >= MIN_CAL_FACTOR && p <= MAX_CAL_FACTOR && p != calP) { calP = p; changed = true; }
    if (changed) markHwCfgDirty();
  }

  float getCalVoltage() const { return calV; }
  float getCalCurrent() const { return calI; }
  float getCalPower() const { return calP; }

  void resetEnergy() {
    energyWh = 0.0;
    lastEnergyMs = 0;
    sensors.energy = 0.0f;
    markHwCfgDirty();
  }

  void resetCalibrationDefaults() {
    calV = 2.38f;
    calI = 0.93f;
    calP = 2.51f;
    calibrating = false;
    calibrationStatus = F("Calibration reset");
    markHwCfgDirty();
  }

  void startCalibration(float targetPowerW) {
    if (targetPowerW < 1.0f) targetPowerW = 1.0f;
    calTargetPower = targetPowerW;
    calSumRawV = 0.0;
    calSumRawI = 0.0;
    calSumRawP = 0.0;
    calSamples = 0;
    calStartMs = millis();
    calibrating = true;
    calibrationStatus = F("Load stabilizing...");
  }

  bool isCalibrating() const { return calibrating; }
  String getCalibrationStatus() const { return calibrationStatus; }

  float getRawVoltage() const { return (calV > 0.01f) ? (sensors.voltage / calV) : 0.0f; }
  float getRawCurrent() const { return (calI > 0.01f) ? (sensors.current / calI) : 0.0f; }
  float getRawPower() const { return (calP > 0.01f) ? (sensors.power / calP) : 0.0f; }

  void publishExtraState(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const AppConfig& appCfg) const {
    (void)appCfg;
    if (!mqttClient.connected()) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/status", mqttCfg.topic);
    mqttClient.publish(topic, "online", true);
    snprintf(topic, sizeof(topic), "%s/child_lock", mqttCfg.topic);
    mqttClient.publish(topic, cfgChildLock ? "ON" : "OFF", true);
    snprintf(topic, sizeof(topic), "%s/power_on_behavior", mqttCfg.topic);
    mqttClient.publish(topic,
                       cfgPowerOnBehavior == 0 ? "OFF" :
                       (cfgPowerOnBehavior == 1 ? "ON" : "PREVIOUS"),
                       true);
  }

  void publishRelayState(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const AppConfig& appCfg, int relayIdx) const {
    (void)appCfg;
    if (!mqttClient.connected()) return;
    if (relayIdx < 0 || relayIdx >= DEVICE_RELAY_COUNT) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/relay/%d", mqttCfg.topic, relayIdx);
    mqttClient.publish(topic, cfgRelayState[relayIdx] ? "ON" : "OFF", true);
  }

  void publishSensorsState(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const AppConfig& appCfg) const {
    if (!mqttClient.connected()) return;
    SensorSnapshot s = readSensors();

    char topic[96];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/sensors", mqttCfg.topic);
    snprintf(payload, sizeof(payload),
             "{\"voltage\":%.2f,\"current\":%.3f,\"power\":%.2f,\"pf\":%.3f,\"energy\":%.4f}",
             s.voltage, s.current, s.power, s.pf, s.energy);
    mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/voltage", mqttCfg.topic);
    snprintf(payload, sizeof(payload), "%.2f", s.voltage);
    mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/current", mqttCfg.topic);
    snprintf(payload, sizeof(payload), "%.3f", s.current);
    mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/power", mqttCfg.topic);
    snprintf(payload, sizeof(payload), "%.2f", s.power);
    mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/pf", mqttCfg.topic);
    snprintf(payload, sizeof(payload), "%.3f", s.pf);
    mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/energy", mqttCfg.topic);
    snprintf(payload, sizeof(payload), "%.4f", s.energy);
    mqttClient.publish(topic, payload, true);

    publishExtraState(mqttClient, mqttCfg, appCfg);
  }

  void publishAllState(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const AppConfig& appCfg) const {
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      publishRelayState(mqttClient, mqttCfg, appCfg, i);
    }
    publishSensorsState(mqttClient, mqttCfg, appCfg);
  }

  void publishExtraDiscovery(PubSubClient& mqttClient, const String& mac, const MqttConfig& mqttCfg) const {
    if (!mqttClient.connected()) return;

    {
      String topic = "homeassistant/switch/" + mac + "/child_lock/config";
      String payload;
      payload.reserve(420);
      payload += F("{\"name\":\"Child Lock\",\"uniq_id\":\""); payload += mac; payload += F("_child_lock\"");
      payload += F(",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/child_lock\"");
      payload += F(",\"cmd_t\":\""); payload += mqttCfg.topic; payload += F("/child_lock/set\"");
      payload += F(",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"icon\":\"mdi:lock\"");
      payload += F(",\"avty_t\":\""); payload += mqttCfg.topic; payload += F("/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"");
      payload += F(",\"dev\":{\"ids\":[\""); payload += mac; payload += F("\"],\"name\":\"NOUS A5T\"}}");
      mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }

    {
      String topic = "homeassistant/select/" + mac + "/power_on_behavior/config";
      String payload;
      payload.reserve(460);
      payload += F("{\"name\":\"Power On Behavior\",\"uniq_id\":\""); payload += mac; payload += F("_power_on\"");
      payload += F(",\"stat_t\":\""); payload += mqttCfg.topic; payload += F("/power_on_behavior\"");
      payload += F(",\"cmd_t\":\""); payload += mqttCfg.topic; payload += F("/power_on_behavior/set\"");
      payload += F(",\"options\":[\"OFF\",\"ON\",\"PREVIOUS\"],\"icon\":\"mdi:power-settings\"");
      payload += F(",\"avty_t\":\""); payload += mqttCfg.topic; payload += F("/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"");
      payload += F(",\"dev\":{\"ids\":[\""); payload += mac; payload += F("\"],\"name\":\"NOUS A5T\"}}");
      mqttClient.publish(topic.c_str(), payload.c_str(), true);
    }
  }

  void subscribeExtraTopics(PubSubClient& mqttClient, const MqttConfig& mqttCfg) const {
    if (!mqttClient.connected()) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/child_lock/set", mqttCfg.topic);
    mqttClient.subscribe(topic);
    snprintf(topic, sizeof(topic), "%s/power_on_behavior/set", mqttCfg.topic);
    mqttClient.subscribe(topic);
  }

  bool handleExtraMqttCommand(const String& sub, const String& msg, AppConfig& appCfg, bool& configChanged) {
    (void)appCfg;
    if (sub == "/child_lock/set") {
      cfgChildLock = (msg == "1" || msg.equalsIgnoreCase("ON") || msg.equalsIgnoreCase("TRUE"));
      configChanged = true;
      markHwCfgDirty();
      return true;
    }
    if (sub == "/power_on_behavior/set") {
      int targetPob = -1;
      if (msg.equalsIgnoreCase("OFF") || msg == "0") targetPob = 0;
      else if (msg.equalsIgnoreCase("ON") || msg == "1") targetPob = 1;
      else if (msg.equalsIgnoreCase("PREVIOUS") || msg.equalsIgnoreCase("RESTORE") || msg == "2") targetPob = 2;

      if (targetPob != -1 && setPowerOnBehavior(appCfg, targetPob)) {
        configChanged = true;
        return true;
      }
      return true;
    }
    return false;
  }

  String generateOpenHAB(const MqttConfig& mqttCfg) const {
    String out;
    out.reserve(8500);
    out += F("<div class='card'><h2>1. Things Configuration (nous_a5t.things)</h2><pre>");
    out += F("Thing mqtt:topic:nous_a5t \"NOUS A5T\" {\n");
    out += F("  Channels:\n");
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      out += F("    Type switch : relay"); out += i;
      out += F(" [ stateTopic=\""); out += mqttCfg.topic; out += F("/relay/"); out += i;
      out += F("\", commandTopic=\""); out += mqttCfg.topic; out += F("/relay/"); out += i;
      out += F("/set\", on=\"ON\", off=\"OFF\" ]\n");
    }
    out += F("    Type switch : child_lock [ stateTopic=\""); out += mqttCfg.topic; out += F("/child_lock\", commandTopic=\""); out += mqttCfg.topic; out += F("/child_lock/set\", on=\"ON\", off=\"OFF\" ]\n");
    out += F("    Type string : power_on [ stateTopic=\""); out += mqttCfg.topic; out += F("/power_on_behavior\", commandTopic=\""); out += mqttCfg.topic; out += F("/power_on_behavior/set\" ]\n");
    out += F("    Type number : voltage [ stateTopic=\""); out += mqttCfg.topic; out += F("/voltage\" ]\n");
    out += F("    Type number : current [ stateTopic=\""); out += mqttCfg.topic; out += F("/current\" ]\n");
    out += F("    Type number : power [ stateTopic=\""); out += mqttCfg.topic; out += F("/power\" ]\n");
    out += F("    Type number : pf [ stateTopic=\""); out += mqttCfg.topic; out += F("/pf\" ]\n");
    out += F("    Type number : energy [ stateTopic=\""); out += mqttCfg.topic; out += F("/energy\" ]\n");
    out += F("}\n</pre></div>");

    out += F("<div class='card'><h2>2. Items Configuration (nous_a5t.items)</h2><pre>");
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      out += F("Switch NOUS_A5T_Relay"); out += (i + 1); out += F(" \"Relay "); out += (i + 1); out += F("\" { channel=\"mqtt:topic:nous_a5t:relay"); out += i; out += F("\" }\n");
    }
    out += F("Switch NOUS_A5T_ChildLock \"Child Lock\" { channel=\"mqtt:topic:nous_a5t:child_lock\" }\n");
    out += F("String NOUS_A5T_PowerOnBehavior \"Power-On Behavior [%s]\" { channel=\"mqtt:topic:nous_a5t:power_on\" }\n");
    out += F("Number NOUS_A5T_Voltage \"Voltage [%.1f V]\" { channel=\"mqtt:topic:nous_a5t:voltage\" }\n");
    out += F("Number NOUS_A5T_Current \"Current [%.3f A]\" { channel=\"mqtt:topic:nous_a5t:current\" }\n");
    out += F("Number NOUS_A5T_Power \"Power [%.1f W]\" { channel=\"mqtt:topic:nous_a5t:power\" }\n");
    out += F("Number NOUS_A5T_PF \"Power Factor [%.3f]\" { channel=\"mqtt:topic:nous_a5t:pf\" }\n");
    out += F("Number NOUS_A5T_Energy \"Energy [%.3f kWh]\" { channel=\"mqtt:topic:nous_a5t:energy\" }\n");
    out += F("</pre></div>");

    out += F("<div class='card'><h2>3. Sitemap Configuration (nous_a5t.sitemap)</h2><pre>");
    out += F("sitemap nous_a5t label=\"NOUS A5T\" {\n");
    out += F("  Frame label=\"Relays\" {\n");
    out += F("    Switch item=NOUS_A5T_Relay1\n");
    out += F("    Switch item=NOUS_A5T_Relay2\n");
    out += F("    Switch item=NOUS_A5T_Relay3\n");
    out += F("    Switch item=NOUS_A5T_Relay4\n");
    out += F("  }\n");
    out += F("  Frame label=\"Controls\" {\n");
    out += F("    Switch item=NOUS_A5T_ChildLock\n");
    out += F("    Selection item=NOUS_A5T_PowerOnBehavior icon=\"settings\" mappings=[OFF=\"OFF\", ON=\"ON\", PREVIOUS=\"PREVIOUS\"]\n");
    out += F("  }\n");
    out += F("  Frame label=\"Sensors\" {\n");
    out += F("    Default item=NOUS_A5T_Voltage\n");
    out += F("    Default item=NOUS_A5T_Current\n");
    out += F("    Default item=NOUS_A5T_Power\n");
    out += F("    Default item=NOUS_A5T_PF\n");
    out += F("    Default item=NOUS_A5T_Energy\n");
    out += F("  }\n");
    out += F("}\n</pre></div>");
    return out;
  }

  String generateHomeAssistant(const MqttConfig& mqttCfg) const {
    String y;
    y.reserve(4200); // Increased reserve size to accommodate new select entity
    y += F("# Home Assistant MQTT manual example for NOUS A5T\n");
    y += F("# Base topic: "); y += mqttCfg.topic; y += F("\n\n");

    y += F("mqtt:\n");
    y += F("  switch:\n");
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      y += F("    - name: \"NOUS A5T Relay "); y += (i + 1); y += F("\"\n");
      y += F("      unique_id: \"nous_a5t_relay_"); y += i; y += F("\"\n");
      y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/relay/"); y += i; y += F("\"\n");
      y += F("      command_topic: \""); y += mqttCfg.topic; y += F("/relay/"); y += i; y += F("/set\"\n");
      y += F("      payload_on: \"ON\"\n");
      y += F("      payload_off: \"OFF\"\n");
      y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n");
      y += F("      payload_available: \"online\"\n");
      y += F("      payload_not_available: \"offline\"\n");
    }

    y += F("    - name: \"NOUS A5T Child Lock\"\n");
    y += F("      unique_id: \"nous_a5t_child_lock\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/child_lock\"\n");
    y += F("      command_topic: \""); y += mqttCfg.topic; y += F("/child_lock/set\"\n");
    y += F("      payload_on: \"ON\"\n");
    y += F("      payload_off: \"OFF\"\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n");
    y += F("      payload_available: \"online\"\n");
    y += F("      payload_not_available: \"offline\"\n\n");

    y += F("  select:\n");
    y += F("    - name: \"NOUS A5T Power On Behavior\"\n");
    y += F("      unique_id: \"nous_a5t_power_on_behavior\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/power_on_behavior\"\n");
    y += F("      command_topic: \""); y += mqttCfg.topic; y += F("/power_on_behavior/set\"\n");
    y += F("      options:\n");
    y += F("        - \"OFF\"\n");
    y += F("        - \"ON\"\n");
    y += F("        - \"PREVIOUS\"\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n");
    y += F("      payload_available: \"online\"\n");
    y += F("      payload_not_available: \"offline\"\n\n");


    y += F("  sensor:\n");
    y += F("    - name: \"NOUS A5T Voltage\"\n");
    y += F("      unique_id: \"nous_a5t_voltage\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/sensors\"\n");
    y += F("      value_template: \"{{ value_json.voltage }}\"\n");
    y += F("      unit_of_measurement: \"V\"\n");
    y += F("      device_class: voltage\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n\n");

    y += F("    - name: \"NOUS A5T Current\"\n");
    y += F("      unique_id: \"nous_a5t_current\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/sensors\"\n");
    y += F("      value_template: \"{{ value_json.current }}\"\n");
    y += F("      unit_of_measurement: \"A\"\n");
    y += F("      device_class: current\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n\n");

    y += F("    - name: \"NOUS A5T Power\"\n");
    y += F("      unique_id: \"nous_a5t_power\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/sensors\"\n");
    y += F("      value_template: \"{{ value_json.power }}\"\n");
    y += F("      unit_of_measurement: \"W\"\n");
    y += F("      device_class: power\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n\n");

    y += F("    - name: \"NOUS A5T PF\"\n");
    y += F("      unique_id: \"nous_a5t_pf\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/sensors\"\n");
    y += F("      value_template: \"{{ value_json.pf }}\"\n");
    y += F("      icon: mdi:flash\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n\n");

    y += F("    - name: \"NOUS A5T Energy\"\n");
    y += F("      unique_id: \"nous_a5t_energy\"\n");
    y += F("      state_topic: \""); y += mqttCfg.topic; y += F("/sensors\"\n");
    y += F("      value_template: \"{{ value_json.energy }}\"\n");
    y += F("      unit_of_measurement: \"kWh\"\n");
    y += F("      device_class: energy\n");
    y += F("      state_class: total_increasing\n");
    y += F("      availability_topic: \""); y += mqttCfg.topic; y += F("/status\"\n");

    return y;
  }

  String generateESPHome(const AppConfig& appCfg) const {
    (void)appCfg;
    String yaml;
    yaml.reserve(2800);
    yaml += F("esphome:\n  name: nous-a5t\n\nesp8266:\n  board: esp8285\n\n");
    yaml += F("switch:\n");
    yaml += F("  - platform: gpio\n    pin: GPIO14\n    name: \"Relay 1\"\n");
    yaml += F("  - platform: gpio\n    pin: GPIO12\n    name: \"Relay 2\"\n");
    yaml += F("  - platform: gpio\n    pin: GPIO13\n    name: \"Relay 3\"\n");
    yaml += F("  - platform: gpio\n    pin:\n      number: GPIO5\n      inverted: true\n    name: \"Relay 4 (USB)\"\n\n");
    yaml += F("# power_on_behavior: ");
    yaml += (cfgPowerOnBehavior == 0 ? F("ALWAYS_OFF") : (cfgPowerOnBehavior == 1 ? F("ALWAYS_ON") : F("RESTORE_DEFAULT_OFF")));
    yaml += F("\n");
    return yaml;
  }

  // ===== CALIBRATION PAGE RENDERERS (private - used only inside registerHardwareRoutes) =====
  String renderCalibrationPage(const AppConfig& appCfg) const {
    String c;
    c.reserve(4500);
    c += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
    c += F("<title>"); c += (appCfg.ui_lang == 0 ? F("Calibrare") : F("Calibration")); c += F("</title>");
    c += F("<style>");
    c += F("body{font-family:sans-serif;background:#f4f4f4;margin:20px;color:#333}");
    c += F(".container{max-width:900px;margin:0 auto}");
    c += F(".grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:20px}");
    c += F(".card{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}");
    c += F("h2{margin-top:0;font-size:1.1rem;color:#0d6efd;border-bottom:1px solid #eee;padding-bottom:8px}");
    c += F("p{margin:10px 0;font-size:14px}b{color:#555}");
    c += F("input{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}");
    c += F("button{width:100%;padding:10px;background:#0d6efd;color:#fff;border:none;border-radius:4px;cursor:pointer;font-weight:600;margin-top:10px}");
    c += F("button.secondary{background:#6c757d}button:hover{opacity:0.9}");
    c += F(".val-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}");
    c += F(".val-row input{flex:1;margin-right:10px;margin-top:0}");
    c += F(".val-row button{width:auto;margin-top:0;padding:8px 15px}");
    c += F("nav{margin-bottom:20px}nav a{text-decoration:none;color:#666;font-size:14px}");
    c += F("@media(max-width:700px){.grid{grid-template-columns:1fr}}");
    c += F("</style></head><body><div class='container'>");
    c += F("<nav><a href='/config'>&larr; "); c += (appCfg.ui_lang == 0 ? F("Înapoi") : F("Back")); c += F("</a></nav>");
    c += F("<h1>"); c += (appCfg.ui_lang == 0 ? F("Gestiune Calibrare") : F("Calibration Management")); c += F("</h1>");
    c += F("<div class='grid'>");
    c += F("<div class='card'><h2>"); c += (appCfg.ui_lang == 0 ? F("Valori Brute (Live)") : F("Raw Values (Live)")); c += F("</h2>");
    c += F("<p><b>V:</b> <span id='rv'>"); c += String(getRawVoltage(), 3); c += F("</span></p>");
    c += F(" <p><b>I:</b> <span id='ri'>"); c += String(getRawCurrent(), 5); c += F("</span></p>");
    c += F(" <p><b>P:</b> <span id='rp'>"); c += String(getRawPower(), 3); c += F("</span></p>");
    c += F("<p><small>"); c += (appCfg.ui_lang == 0 ? F("Status: ") : F("Status: ")); c += calibrationStatus; c += F("</small></p></div>");
    c += F("<div class='card'><h2>"); c += (appCfg.ui_lang == 0 ? F("Factori Activi") : F("Current Factors")); c += F("</h2>");
    c += F("<p><b>Factor V:</b> "); c += String(calV, 6); c += F("</p>");
    c += F("<p><b>Factor I:</b> "); c += String(calI, 6); c += F("</p>");
    c += F("<p><b>Factor P:</b> "); c += String(calP, 6); c += F("</p>");
    c += F("<form method='post' action='/reset_calibration' onsubmit=\"return confirm('Reset?')\"><button type='submit' class='secondary'>"); 
    c += (appCfg.ui_lang == 0 ? F("Resetează Factorii") : F("Reset Factors")); c += F("</button></form></div></div>");
    c += F("<div class='grid'><div class='card'><h2>"); c += (appCfg.ui_lang == 0 ? F("Calibrare Automată") : F("Auto Calibration")); c += F("</h2>");
    c += F("<p>"); c += (appCfg.ui_lang == 0 ? F("Conectați o sarcină cunoscută (ex. bec 60W).") : F("Connect a known load (e.g. 60W bulb).")); c += F("</p>");
    c += F("<form method='post' action='/do_calibrate'><label>"); c += (appCfg.ui_lang == 0 ? F("Putere Țintă (W):") : F("Target Power (W):")); 
    c += F("</label><input name='target' type='number' step='0.1' value='60'><button type='submit'>"); 
    c += (appCfg.ui_lang == 0 ? F("Start Calibrare") : F("Start Calibration")); c += F("</button></form></div>");
    c += F("<div class='card'><h2>"); c += (appCfg.ui_lang == 0 ? F("Calibrare Manuală") : F("Manual Calibration")); c += F("</h2>");
    auto renderRow = [&](const char* lbl, const char* nm, float val) {
      c += F("<form method='post' action='/save_calibration' class='val-row'><b>"); c += lbl; c += F(":</b>");
      c += F("<input name='"); c += nm; 
      c += F("' type='number' step='0.000001' min='0.001' max='100' value='"); 
      c += String(val, 6); 
      c += F("'>");
      c += F("<button type='submit'>"); c += (appCfg.ui_lang == 0 ? F("Salvează") : F("Save")); c += F("</button></form>");
    };
    renderRow("V", "cal_v", calV); renderRow("I", "cal_c", calI); renderRow("P", "cal_p", calP);
    c += F("</div></div>");
    c += F("<script>(function(){let ws=null;let rt=null;function connect(){try{ws=new WebSocket('ws://'+location.hostname+':81/');}catch(e){schedule();return;}");
    c += F("ws.onmessage=function(ev){if(!ev||!ev.data)return;try{let d=JSON.parse(ev.data);if(d.raw){");
    c += F("document.getElementById('rv').innerText=d.raw.v;document.getElementById('ri').innerText=d.raw.i;document.getElementById('rp').innerText=d.raw.p;");
    c += F("}}catch(e){}};ws.onclose=schedule;ws.onerror=function(){try{ws.close();}catch(e){}};}");
    c += F("function schedule(){if(rt)return;rt=setTimeout(function(){rt=null;connect();},1000);}connect();})();</script>");
    c += F("</div></body></html>");
    return c;
  }

  String getJsonStatus(const AppConfig& appCfg) const {
    SensorSnapshot s = readSensors();
    String json;
    json.reserve(400);
    json += F("\"relays\":[");
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      json += (getRelay(i) ? "true" : "false");
      if (i < DEVICE_RELAY_COUNT - 1) json += ",";
    }
    json += F("],\"sensors\":{\"v\":"); json += String(s.voltage, 1);
    json += F(",\"i\":"); json += String(s.current, 3);
    json += F(",\"p\":"); json += String(s.power, 1);
    json += F(",\"pf\":"); json += String(s.pf, 3);
    json += F(",\"e\":"); json += String(s.energy, 4);
    json += F("},\"raw\":{\"v\":"); json += String(getRawVoltage(), 3);
    json += F(",\"i\":"); json += String(getRawCurrent(), 5);
    json += F(",\"p\":"); json += String(getRawPower(), 3);
    json += F("},\"lock\":"); json += (isInputLocked(appCfg) ? "true" : "false");
    json += F(",\"pob\":"); json += getPowerOnBehavior();
    return json;
  }

  String renderMainPageScript(const AppConfig& appCfg) const {
    String s;
    s += F("window.hCmd=function(u,p){fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});};");
    s += F("function updateHardwareUI(d){");
    s += F("if(d.relays){for(let i=0;i<d.relays.length;i++){let b=document.getElementById('btn_r'+i);let st=document.getElementById('stat_r'+i);if(b&&st){b.style.background=d.relays[i]?'#198754':'#6c757d';st.innerText=d.relays[i]?'ON':'OFF';}}} ");
    s += F("if(d.sensors){document.getElementById('val_v').innerText=d.sensors.v+' V';document.getElementById('val_i').innerText=d.sensors.i+' A';document.getElementById('val_p').innerText=d.sensors.p+' W';document.getElementById('val_pf').innerText=d.sensors.pf;document.getElementById('val_e').innerText=d.sensors.e+' kWh';}");
    s += F("if(d.lock!==undefined){let b=document.getElementById('btn_lock');let st=document.getElementById('stat_lock');if(b&&st){b.style.background=d.lock?'#dc3545':'#198754';st.innerText=d.lock?'ON':'OFF';}}");
    s += F("if(d.pob!==undefined){for(let i=0;i<3;i++){let b=document.getElementById('btn_pob_'+i);if(b){b.style.background=(d.pob==i)?'#0d6efd':'#fff';b.style.color=(d.pob==i)?'#fff':'#333';}}}}");
    return s;
  }

  String renderManualCalibrationPage(const AppConfig& appCfg) const {
    return renderCalibrationPage(appCfg);
  }

  bool consumeRelayToggleEvent(int& relayIdxOut) {
    if (pendingToggle < 0) return false;
    relayIdxOut = pendingToggle;
    pendingToggle = -1;
    return true;
  }

  bool consumeToggleAllEvent() {
    if (!pendingToggleAll) return false;
    pendingToggleAll = false;
    return true;
  }

  bool consumeForceAllOffEvent() {
    if (!pendingForceAllOff) return false;
    pendingForceAllOff = false;
    return true;
  }

  bool consumeFactoryResetEvent() {
    if (!pendingFactoryReset) return false;
    pendingFactoryReset = false;
    return true;
  }

  void setStatusConnected(bool connected) {
    ledConnected = connected;
    if (connected) {
      digitalWrite(LED_PIN, LOW); // ON (inverted)
    }
  }

private:
  struct HwCfgBlob {
    uint32_t magic;
    uint16_t version;
    bool relay_state[DEVICE_RELAY_COUNT];
    float cal_voltage;
    float cal_current;
    float cal_power;
    bool child_lock;
    uint8_t power_on_behavior;
    double energy_wh;
    uint32_t crc;
  };

  static const uint32_t HWCFG_MAGIC = 0x48435731U; // "HCW1"
  static const uint16_t HWCFG_VERSION = 1;
  static const unsigned long HWCFG_SAVE_DELAY = 3000UL;

  uint32_t hwCfgCrc32(const uint8_t* data, size_t len) const {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];
      for (uint8_t b = 0; b < 8; b++) {
        crc = (crc >> 1) ^ (0xEDB88320U & (-(int32_t)(crc & 1U)));
      }
    }
    return ~crc;
  }

  void markHwCfgDirty() {
    hwCfgDirty = true;
    hwCfgDirtySince = millis();
  }

  void saveHwCfgNow() {
    HwCfgBlob blob = {};
    blob.magic = HWCFG_MAGIC;
    blob.version = HWCFG_VERSION;
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) blob.relay_state[i] = cfgRelayState[i];
    blob.cal_voltage = calV;
    blob.cal_current = calI;
    blob.cal_power = calP;
    blob.child_lock = cfgChildLock;
    blob.power_on_behavior = cfgPowerOnBehavior;
    blob.energy_wh = energyWh;
    blob.crc = hwCfgCrc32((const uint8_t*)&blob, sizeof(HwCfgBlob) - sizeof(blob.crc));

    File f = LittleFS.open("/hwcfg.bin.tmp", "w");
    if (!f) return;
    size_t written = f.write((const uint8_t*)&blob, sizeof(blob));
    f.close();
    if (written == sizeof(blob)) {
      LittleFS.remove("/hwcfg.bin");
      LittleFS.rename("/hwcfg.bin.tmp", "/hwcfg.bin");
      hwCfgDirty = false;
    }
  }

  void flushHwCfgIfDirty() {
    if (!hwCfgDirty) return;
    if (millis() - hwCfgDirtySince < HWCFG_SAVE_DELAY) return;
    saveHwCfgNow();
  }

  void loadHwCfg() {
    if (!LittleFS.exists("/hwcfg.bin")) return;
    File f = LittleFS.open("/hwcfg.bin", "r");
    if (!f || f.size() != sizeof(HwCfgBlob)) {
      if (f) f.close();
      return;
    }

    HwCfgBlob blob;
    size_t r = f.read((uint8_t*)&blob, sizeof(blob));
    f.close();
    if (r != sizeof(blob)) return;
    if (blob.magic != HWCFG_MAGIC || blob.version != HWCFG_VERSION) return;

    uint32_t crc = hwCfgCrc32((const uint8_t*)&blob, sizeof(HwCfgBlob) - sizeof(blob.crc));
    if (crc != blob.crc) return;

    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) cfgRelayState[i] = blob.relay_state[i];
    calV = (blob.cal_voltage > 0.01f) ? blob.cal_voltage : DEFAULT_CAL_V;
    calI = (blob.cal_current > 0.01f) ? blob.cal_current : DEFAULT_CAL_I;
    calP = (blob.cal_power > 0.01f) ? blob.cal_power : DEFAULT_CAL_P;
    cfgChildLock = blob.child_lock;
    cfgPowerOnBehavior = (blob.power_on_behavior <= 2) ? blob.power_on_behavior : 2;
    energyWh = (blob.energy_wh >= 0.0) ? blob.energy_wh : 0.0;
    sensors.energy = (float)(energyWh / 1000.0);
  }

  // ===== CALIBRATION ACTION HANDLERS (private - used only inside registerHardwareRoutes) =====
  void handleStartCalibration(AppConfig& appCfg, float targetPowerW) {
    if (targetPowerW < 1.0f) targetPowerW = 1.0f;
    startCalibration(targetPowerW);
  }

  void handleResetCalibration(AppConfig& appCfg) {
    resetCalibrationDefaults();
    syncConfigFromHardware(appCfg);
  }

  void handleSaveCalibration(AppConfig& appCfg, float v, float i, float p) {
    if (v > 0.01f && i > 0.01f && p > 0.01f) {
      setCalibrationFactors(v, i, p);
      syncConfigFromHardware(appCfg);
    }
  }

  bool checkHardwareAuth(ESP8266WebServer& server, const AppConfig& appCfg) const {
    if (!checkNetworkGateForCurrentRequest(appCfg)) return false;
    if (!appCfg.auth_config) return true;
    if (!server.authenticate(appCfg.user_config, appCfg.pass_config)) {
      server.requestAuthentication(BASIC_AUTH, "Config Access");
      return false;
    }
    return true;
  }

  bool parseOnOff(const String& value, bool& outState) const {
    if (value.equalsIgnoreCase("ON") || value == "1" || value.equalsIgnoreCase("true")) {
      outState = true;
      return true;
    }
    if (value.equalsIgnoreCase("OFF") || value == "0" || value.equalsIgnoreCase("false")) {
      outState = false;
      return true;
    }
    return false;
  }

  static const int RELAY_PINS[DEVICE_RELAY_COUNT];
  static const bool RELAY_INVERTED[DEVICE_RELAY_COUNT];
  static const int BTN_PIN = 16;
  static const int LED_PIN = 2;

  static constexpr float DEFAULT_CAL_V = 2.38f;
  static constexpr float DEFAULT_CAL_I = 0.93f;
  static constexpr float DEFAULT_CAL_P = 2.51f;
  static constexpr float MIN_CAL_FACTOR = 0.001f;
  static constexpr float MAX_CAL_FACTOR = 100.0f;
  static const unsigned long CAL_STABILIZE_DELAY = 1000UL;
  static const unsigned long CAL_TOTAL_DURATION = 15000UL;
  static constexpr float P_OFFSET = 4.6f;
  static constexpr float P_DEADBAND = 0.4f;
  static constexpr float I_NOISE_MIN = 0.03f;
  static constexpr float EMA_ALPHA = 0.10f;
  static constexpr float MAX_POWER_LIMIT = 3680.0f;

  static const int ADC_BTN1_MIN = 720;
  static const int ADC_BTN1_MAX = 770;
  static const int ADC_BTN2_MIN = 450;
  static const int ADC_BTN2_MAX = 500;
  static const int ADC_BTN3_MIN = 200;
  static const int ADC_BTN3_MAX = 250;

  bool relays[DEVICE_RELAY_COUNT] = {false};
  bool cfgRelayState[DEVICE_RELAY_COUNT] = {false};
  bool cfgChildLock = false;
  uint8_t cfgPowerOnBehavior = 2;
  bool hwCfgDirty = false;
  unsigned long hwCfgDirtySince = 0;
  SensorSnapshot sensors;
  double energyWh = 0.0;       // accumulated Wh
  unsigned long lastEnergyMs = 0; // last integration timestamp
  float calV = DEFAULT_CAL_V;
  float calI = DEFAULT_CAL_I;
  float calP = DEFAULT_CAL_P;

  SoftwareSerial cseSerial = SoftwareSerial(3, 1);
  uint8_t cseBuff[32] = {0};
  int cseIdx = 0;
  unsigned long lastCsePacketTime = 0;
  unsigned long lastCseRetry = 0;
  unsigned long lastCseByteMs = 0;
  double emaPower = 0.0;

  int btnStable = 0;
  int btnLastRaw = 0;
  unsigned long lastDebounce = 0;
  int pendingToggle = -1;
  bool pendingToggleAll = false;
  bool pendingForceAllOff = false;
  bool pendingFactoryReset = false;
  int lastBtnDigital = HIGH;
  unsigned long btnDigitalPressTime = 0;

  bool ledConnected = false;
  bool ledBlinkState = false;
  unsigned long lastLedBlink = 0;

  bool calibrating = false;
  unsigned long calStartMs = 0;
  float calTargetPower = 0.0f;
  double calSumRawV = 0.0;
  double calSumRawI = 0.0;
  double calSumRawP = 0.0;
  int calSamples = 0;
  String calibrationStatus = "Ready";

  void writeRelayPin(int idx, bool state) {
    bool pinState = RELAY_INVERTED[idx] ? !state : state;
    digitalWrite(RELAY_PINS[idx], pinState ? HIGH : LOW);
  }

  int decodeButtonFromAdc(int adc) const {
    if (adc >= ADC_BTN1_MIN && adc <= ADC_BTN1_MAX) return 1;
    if (adc >= ADC_BTN2_MIN && adc <= ADC_BTN2_MAX) return 2;
    if (adc >= ADC_BTN3_MIN && adc <= ADC_BTN3_MAX) return 3;
    return 0;
  }

  void pollButtons() {
    // Buton digital GPIO16
    int reading = digitalRead(BTN_PIN);
    if (reading == LOW && lastBtnDigital == HIGH) {
      btnDigitalPressTime = millis();
    }
    if (reading == HIGH && lastBtnDigital == LOW) {
      unsigned long duration = millis() - btnDigitalPressTime;
      if (duration > 50 && duration < 5000) {
        pendingToggleAll = true;
      } else if (duration > 10000) {
        pendingFactoryReset = true;
      }
    }
    lastBtnDigital = reading;

    // Butoane analogice A0
    static unsigned long lastAdcPoll = 0;
    if (millis() - lastAdcPoll < 100) return;
    lastAdcPoll = millis();

    int adc = analogRead(A0);
    int raw = decodeButtonFromAdc(adc);

    if (raw != btnLastRaw) {
      btnLastRaw = raw;
      lastDebounce = millis();
      return;
    }

    if (millis() - lastDebounce < 50) return;
    if (raw == btnStable) return;

    btnStable = raw;

    // Detectam doar frontul de apasare
    if (btnStable >= 1 && btnStable <= 3) {
      int relayIdx = btnStable - 1;
      if (relayIdx >= 0 && relayIdx < DEVICE_RELAY_COUNT) {
        pendingToggle = relayIdx;
      }
    }
  }

  void refreshStatusLed() {
    if (ledConnected) {
      digitalWrite(LED_PIN, LOW); // ON
      return;
    }

    if (millis() - lastLedBlink >= 500) {
      lastLedBlink = millis();
      ledBlinkState = !ledBlinkState;
      digitalWrite(LED_PIN, ledBlinkState ? LOW : HIGH);
    }
  }

  void retryCseIfNeeded() {
    if (millis() - lastCsePacketTime > 3000 && millis() - lastCseRetry > 1000) {
      lastCseRetry = millis();
      cseSerial.write(0x55);
    }
  }

  void ingestPowerSample(double rawP, double p, double current) {
    if (rawP > 5000.0 || p > 10000.0) return;

    float val = (float)p - P_OFFSET;
    if (val < 0.0f) val = 0.0f;
    if (val < P_DEADBAND) val = 0.0f;
    if (current < I_NOISE_MIN) val = 0.0f;

    emaPower = emaPower + (double)EMA_ALPHA * ((double)val - emaPower);
  }

  void pollCse7766() {
    if (cseIdx > 0 && millis() - lastCseByteMs > 50) {
      cseIdx = 0;
    }

    while (cseSerial.available() > 0) {
      uint8_t c = cseSerial.read();
      lastCseByteMs = millis();

      if (cseIdx == 0 && c != 0x55) continue;
      if (cseIdx == 1 && c != 0x5A) {
        if (c == 0x55) cseIdx = 1;
        else cseIdx = 0;
        continue;
      }

      if (cseIdx < (int)sizeof(cseBuff)) cseBuff[cseIdx++] = c;
      else cseIdx = 0;

      if (cseIdx < 24) continue;

      uint8_t sum = 0;
      for (int i = 2; i < 23; i++) sum += cseBuff[i];
      if (sum != cseBuff[23]) {
        cseIdx = 0;
        continue;
      }

      unsigned long vCoeff = ((unsigned long)cseBuff[2] << 16) | ((unsigned long)cseBuff[3] << 8) | cseBuff[4];
      unsigned long vCycle = ((unsigned long)cseBuff[5] << 16) | ((unsigned long)cseBuff[6] << 8) | cseBuff[7];
      unsigned long iCoeff = ((unsigned long)cseBuff[8] << 16) | ((unsigned long)cseBuff[9] << 8) | cseBuff[10];
      unsigned long iCycle = ((unsigned long)cseBuff[11] << 16) | ((unsigned long)cseBuff[12] << 8) | cseBuff[13];
      unsigned long pCoeff = ((unsigned long)cseBuff[14] << 16) | ((unsigned long)cseBuff[15] << 8) | cseBuff[16];
      unsigned long pCycle = ((unsigned long)cseBuff[17] << 16) | ((unsigned long)cseBuff[18] << 8) | cseBuff[19];

      double rawV = (vCycle > 0) ? (double)vCoeff / vCycle : 0.0;
      double rawI = (iCycle > 0) ? (double)iCoeff / iCycle : 0.0;
      double rawP = (pCycle > 0) ? (double)pCoeff / pCycle : 0.0;

      double voltage = rawV * calV;
      double current = rawI * calI;
      double powerInstant = rawP * calP;

      if (rawP > 5000.0 || powerInstant > 10000.0) {
        cseIdx = 0;
        return;
      }

      ingestPowerSample(rawP, powerInstant, current);
      double power = emaPower;

      if (current < 0.05f) current *= 0.25f;
      double pf = 0.0;

      if (power < 0.1) {
        power = 0.0;
        current = 0.0;
        pf = 0.0;
      } else {
        double apparent = voltage * current;
        if (apparent > 0.5) {
          pf = power / apparent;
          if (pf > 1.0) pf = 1.0;
          else if (pf < 0.0) pf = 0.0;
        } else {
          pf = 1.0;
        }
      }

      // Protectie la suprasarcina: request OFF all
      if (powerInstant > MAX_POWER_LIMIT) {
        pendingForceAllOff = true;
        calibrationStatus = F("FAULT: POWER LIMIT EXCEEDED!");
      }

      if (calibrating && (millis() - calStartMs > CAL_STABILIZE_DELAY) && rawV > 0.0 && rawI > 0.0 && rawP > 0.0) {
        calSumRawV += rawV;
        calSumRawI += rawI;
        calSumRawP += rawP;
        calSamples++;
      }

      sensors.voltage = (float)voltage;
      sensors.current = (float)current;
      sensors.power   = (float)power;
      sensors.pf      = (float)pf;

      // Accumulate energy (Wh) from power measurement
      unsigned long now = millis();
      if (lastEnergyMs > 0 && power > 0.0) {
        double dtHours = (double)(now - lastEnergyMs) / 3600000.0;
        energyWh += power * dtHours;
      }
      lastEnergyMs = now;
      sensors.energy = (float)(energyWh / 1000.0); // expose as kWh

      lastCsePacketTime = millis();
      cseIdx = 0;
    }

    if (calibrating && (millis() - calStartMs > CAL_TOTAL_DURATION)) {
      if (calSamples > 10) {
        double avgRawV = calSumRawV / calSamples;
        double avgRawI = calSumRawI / calSamples;
        double avgRawP = calSumRawP / calSamples;

        if (avgRawP > 0.001 && avgRawI > 0.0001 && avgRawV > 0.1) {
          calP = (float)(calTargetPower / avgRawP);
          double vCalibrated = avgRawV * calV;
          calI = (float)((calTargetPower / vCalibrated) / avgRawI);
          calibrationStatus = F("Calibration successful");
        } else {
          calibrationStatus = F("Calibration failed: weak signal");
        }
      } else {
        calibrationStatus = F("Calibration failed: not enough samples");
      }
      calibrating = false;
    }
  }

public:
  // UI block for main page (relay buttons + power-on-behavior)
  // Called by template pageStatus(); all HTML for hardware controls stays in this header.
  String renderMainPageFrame(const char* /*fwVersion*/, const String& /*ip*/, int /*rssi*/, bool /*mqttConnected*/, const AppConfig& appCfg) const {
    (void)appCfg;
    String h;
    h.reserve(2400);

    h += F("<div class='card'>");
    h += F("<h2>"); h += (appCfg.ui_lang == 0 ? F("Controale Hardware") : F("Hardware Controls")); h += F("</h2>");

    h += F("<div style='display:grid;grid-template-columns:repeat(5,1fr);gap:8px;margin:10px 0 14px 0;'>");
    for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
      bool on = cfgRelayState[i];
      h += F("<button id='btn_r"); h += i; h += F("' type='button' onclick='hCmd(\"/toggle\",\"id="); h += i; h += F("\")' style='width:100%;padding:12px;border:0;border-radius:6px;font-weight:600;color:#fff;background:");
      h += (on ? F("#198754") : F("#6c757d"));
      h += F("'>");
      if (i < 3) {
        h += (appCfg.ui_lang == 0 ? F("Priză ") : F("Socket "));
        h += (i + 1);
      } else {
        h += F("USB");
      }
      h += F("<br><small id='stat_r"); h += i; h += F("'>"); h += (on ? F("ON") : F("OFF")); h += F("</small>");
      h += F("</button>");
    }
    h += F("<button type='button' onclick='hCmd(\"/toggle_all\",\"\")' style='width:100%;padding:12px;border:0;border-radius:6px;background:#0d6efd;color:#fff;font-weight:600'>ALL</button>");
    h += F("</div>");

    h += F("<div style='display:grid;grid-template-columns:1fr 1.5fr;gap:8px;margin:0 0 12px 0'>");
    
    // Child Lock
    h += F("<button id='btn_lock' type='button' onclick='hCmd(\"/toggle_child_lock\",\"\")' style='width:100%;padding:10px;border:0;border-radius:6px;font-weight:600;color:#fff;background:");
    h += (cfgChildLock ? F("#dc3545") : F("#198754"));
    h += F("'>Lock: <span id='stat_lock'>"); h += (cfgChildLock ? F("ON") : F("OFF")); h += F("</span></button>");

    // Power-on Behavior (Direct Buttons)
    h += F("<div style='display:flex;gap:4px;background:#eee;padding:4px;border-radius:6px;align-items:center'>");
    const char* pobLabels[] = {"OFF", "ON", "PREV"};
    for (int i = 0; i < 3; i++) {
      h += F("<button id='btn_pob_"); h += i; h += F("' type='button' onclick='hCmd(\"/set_pob\",\"pob="); h += i; h += F("\")' style='flex:1;padding:8px 0;border:0;border-radius:4px;font-size:11px;font-weight:bold;cursor:pointer;");
      if (cfgPowerOnBehavior == i) {
        h += F("background:#0d6efd;color:#fff;");
      } else {
        h += F("background:#fff;color:#333;");
      }
      h += F("'>"); h += pobLabels[i]; h += F("</button>");
    }
    h += F("</div></div>");

    // Sensors + energy card
    SensorSnapshot s = readSensors();
    h += F("<div style='margin-top:10px'>");
    h += F("<h2 style='margin-top:0'>"); h += (appCfg.ui_lang == 0 ? F("Senzori Consum") : F("Power Sensors")); h += F("</h2>");
    h += F("<div style='display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:10px'>");
    h += F("<div style='text-align:center;padding:8px;background:#f8f9fa;border-radius:6px'><small style='color:#666'>"); h += (appCfg.ui_lang == 0 ? F("Tensiune") : F("Voltage")); h += F("</small><br><b id='val_v'>"); h += String(s.voltage,1); h += F(" V</b></div>");
    h += F("<div style='text-align:center;padding:8px;background:#f8f9fa;border-radius:6px'><small style='color:#666'>"); h += (appCfg.ui_lang == 0 ? F("Curent") : F("Current")); h += F("</small><br><b id='val_i'>"); h += String(s.current,3); h += F(" A</b></div>");
    h += F("<div style='text-align:center;padding:8px;background:#f8f9fa;border-radius:6px'><small style='color:#666'>"); h += (appCfg.ui_lang == 0 ? F("Putere") : F("Power")); h += F("</small><br><b id='val_p'>"); h += String(s.power,1); h += F(" W</b></div>");
    h += F("<div style='text-align:center;padding:8px;background:#f8f9fa;border-radius:6px'><small style='color:#666'>"); h += (appCfg.ui_lang == 0 ? F("Factor putere") : F("Power Factor")); h += F("</small><br><b id='val_pf'>"); h += String(s.pf,3); h += F("</b></div>");
    h += F("<div style='text-align:center;padding:8px;background:#e8f5e9;border-radius:6px'><small style='color:#666'>"); h += (appCfg.ui_lang == 0 ? F("Energie") : F("Energy")); h += F("</small><br><b id='val_e' style='font-size:0.9em'>"); h += String(s.energy,4); h += F(" kWh</b></div>");
    h += F("<button type='button' onclick='if(confirm(\"Reset?\"))hCmd(\"/reset_energy\",\"\")' style='width:100%;height:100%;padding:4px 0;border:0;border-radius:6px;background:#dc3545;color:#fff;font-weight:600;font-size:11px;cursor:pointer'>"); h += (appCfg.ui_lang == 0 ? F("RESET<br>ENERG.") : F("RESET<br>ENERGY")); h += F("</button>");
    h += F("</div>"); // Inchide grid-ul de senzori
    h += F("</div>"); // Inchide containerul margin-top:10px

    h += F("</div>");
    return h;
  }

  // Right-side frame for configuration page (all hardware UI HTML stays here)
  // Contains only: MQTT control, derived topics, sensors calibration
  String renderConfigRightFrame(const AppConfig& appCfg, const MqttConfig& mqttCfg) const {
    String h;
    h.reserve(2800);

    h += F("<div class='card'>");
    h += F("<h2 style='margin-top:0'>"); h += (appCfg.ui_lang == 0 ? F("Control MQTT") : F("MQTT Control")); h += F("</h2>");
    h += F("<p style='margin:6px 0'><b>"); h += (appCfg.ui_lang == 0 ? F("Status:") : F("Status:")); h += F("</b> ");
    h += (mqttCfg.enabled ? (appCfg.ui_lang == 0 ? F("Activat") : F("Enabled")) : (appCfg.ui_lang == 0 ? F("Dezactivat") : F("Disabled")));
    h += F("</p>");
    h += F("<p style='margin:6px 0'><b>"); h += (appCfg.ui_lang == 0 ? F("Broker:") : F("Broker:")); h += F("</b> ");
    h += htmlEscape(String(mqttCfg.host));
    h += F(":");
    h += mqttCfg.port;
    h += F("</p>");
    h += F("<p style='margin:6px 0'><b>"); h += (appCfg.ui_lang == 0 ? F("Topic Principal:") : F("Base Topic:")); h += F("</b> ");
    h += htmlEscape(String(mqttCfg.topic));
    h += F("</p>");
    h += F("</div>");

    h += F("<div class='card'>");
    h += F("<h2 style='margin-top:0'>"); h += (appCfg.ui_lang == 0 ? F("Integrări") : F("Integrations")); h += F("</h2>");
    h += F("<a href='/ha' target='_top' style='display:block;text-align:center;padding:9px;border:1px solid #ccc;border-radius:6px;background:#f6f6f6;color:#222;text-decoration:none;margin:6px 0'>"); h += (appCfg.ui_lang == 0 ? F("Config Home Assistant") : F("Home Assistant Config")); h += F("</a>");
    h += F("<a href='/openhab' target='_top' style='display:block;text-align:center;padding:9px;border:1px solid #ccc;border-radius:6px;background:#f6f6f6;color:#222;text-decoration:none;margin:6px 0'>"); h += (appCfg.ui_lang == 0 ? F("Config openHAB") : F("openHAB Config")); h += F("</a>");
    h += F("<a href='/esphome' target='_top' style='display:block;text-align:center;padding:9px;border:1px solid #ccc;border-radius:6px;background:#f6f6f6;color:#222;text-decoration:none;margin:6px 0'>"); h += (appCfg.ui_lang == 0 ? F("Config ESPHome") : F("ESPHome Config")); h += F("</a>");
    h += F("</div>");



    h += F("<div class='card'>");
    h += F("<h2 style='margin-top:0'>"); h += (appCfg.ui_lang == 0 ? F("Topicuri Derivate") : F("Derived Topics")); h += F("</h2>");
    String base = String(mqttCfg.topic);
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/relay/[0-3]</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/relay/[0-3]/set</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/relay/all/set</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/sensors <small style='color:#888'>{json}</small></code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/voltage</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/current</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/power</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/pf</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/energy</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/child_lock</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/child_lock/set</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/power_on_behavior</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/power_on_behavior/set</code>");
    h += F("<code style='padding:6px;background:#f3f3f3;border-radius:6px;margin:4px 0'>"); h += base; h += F("/status</code>");
    h += F("</div>");

    h += F("<div class='card'>");
    h += F("<h2 style='margin-top:0'>"); h += (appCfg.ui_lang == 0 ? F("Calibrare Senzori") : F("Sensor Calibration")); h += F("</h2>");
    h += F("<a href='/calibration' target='_top' style='display:block;text-align:center;padding:9px;border:1px solid #ccc;border-radius:6px;background:#f6f6f6;color:#222;text-decoration:none;margin:6px 0'>"); h += (appCfg.ui_lang == 0 ? F("Gestiune Calibrare") : F("Calibration Management")); h += F("</a>");
    h += F("<p style='margin:8px 0 0 0;color:#666;font-size:12px'>"); h += (appCfg.ui_lang == 0 ? F("Configurare factori (Auto/Manual)") : F("Configure factors (Auto/Manual)")); h += F("</p>");
    h += F("</div>");

    return h;
  }

  // Called from .ino mqttConnect() after successful MQTT connection
  void onMqttConnected(PubSubClient& mqttClient, const MqttConfig& mqttCfg, const AppConfig& appCfg, const char* fwVersion) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    publishCoreDiscovery(mqttClient, mqttCfg, mac, fwVersion);
    publishExtraDiscovery(mqttClient, mac, mqttCfg);
    publishAllState(mqttClient, mqttCfg, appCfg);
  }

  // Called from .ino mqttCallback() to route incoming MQTT commands to hardware
  void handleMqttCommand(const String& sub, const String& msg, AppConfig& appCfg, PubSubClient& mqttClient, const MqttConfig& mqttCfg) {
    bool publishAll = false;
    int publishIdx = -1;
    if (handleCoreMqttCommand(sub, msg, appCfg, publishAll, publishIdx)) {
      if (publishAll || publishIdx >= 0) publishAllState(mqttClient, mqttCfg, appCfg);
      return;
    }
    bool configChanged = false;
    if (handleExtraMqttCommand(sub, msg, appCfg, configChanged) && configChanged) {
      publishExtraState(mqttClient, mqttCfg, appCfg);
    }
  }

  // Register all hardware-specific HTTP routes.
  // Called once from setupRoutesNormal() in the .ino - all hardware logic stays here.
  void registerHardwareRoutes(ESP8266WebServer& server, AppConfig& appCfg, PubSubClient& mqttClient, const MqttConfig& mqttCfg, void (*markDirty)()) {
    server.on("/toggle", HTTP_POST, [this, &server, &appCfg, &mqttClient, &mqttCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      if (!server.hasArg("id")) { server.send(400, "text/plain", "Missing id"); return; }
      String idArg = server.arg("id");
      for (size_t i = 0; i < idArg.length(); i++) {
        char c = idArg[i];
        if (c < '0' || c > '9') { server.send(400, "text/plain", "Invalid id"); return; }
      }
      int id = idArg.toInt();
      if (id < 0 || id >= DEVICE_RELAY_COUNT) { server.send(400, "text/plain", "Invalid id"); return; }
      if (handleHttpToggle(appCfg, id)) {
        if (mqttClient.connected()) publishAllState(mqttClient, mqttCfg, appCfg);
        markDirty();
      }
      server.send(200, "text/plain", "OK");
    });

    server.on("/toggle_all", HTTP_POST, [this, &server, &appCfg, &mqttClient, &mqttCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      bool anyOff = false;
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) if (!cfgRelayState[i]) { anyOff = true; break; }
      bool newState = anyOff;
      for (int i = 0; i < DEVICE_RELAY_COUNT; i++) {
        cfgRelayState[i] = newState;
        setRelay(i, newState);
      }
      markHwCfgDirty();
      if (mqttClient.connected()) publishAllState(mqttClient, mqttCfg, appCfg);
      markDirty();
      server.send(200, "text/plain", "OK");
    });

    server.on("/toggle_child_lock", HTTP_POST, [this, &server, &appCfg, &mqttClient, &mqttCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      toggleInputLock(appCfg);
      if (mqttClient.connected()) publishExtraState(mqttClient, mqttCfg, appCfg);
      markDirty();
      server.send(200, "text/plain", "OK");
    });

    server.on("/set_pob", HTTP_POST, [this, &server, &appCfg, &mqttClient, &mqttCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      if (server.hasArg("pob")) {
        String pobArg = server.arg("pob");
        if (pobArg != "0" && pobArg != "1" && pobArg != "2") {
          server.send(400, "text/plain", "Invalid pob");
          return;
        }
        if (setPowerOnBehavior(appCfg, pobArg.toInt())) {
          if (mqttClient.connected()) publishExtraState(mqttClient, mqttCfg, appCfg);
          markDirty();
        }
      }
      server.send(200, "text/plain", "OK");
    });

    server.on("/calibration", HTTP_GET, [this, &server, &appCfg]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      server.send(200, "text/html", renderCalibrationPage(appCfg));
    });

    server.on("/manual_calibration", HTTP_GET, [this, &server, &appCfg]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      server.send(200, "text/html", renderManualCalibrationPage(appCfg));
    });

    server.on("/do_calibrate", HTTP_POST, [this, &server, &appCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      float target = server.hasArg("target") ? server.arg("target").toFloat() : 60.0f;
      handleStartCalibration(appCfg, target);
      markDirty();
      server.sendHeader("Location", "/calibration", true);
      server.send(303, "text/plain", "Redirect");
    });

    server.on("/reset_calibration", HTTP_POST, [this, &server, &appCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      handleResetCalibration(appCfg);
      markDirty();
      server.sendHeader("Location", "/calibration", true);
      server.send(303, "text/plain", "Redirect");
    });

    server.on("/save_calibration", HTTP_POST, [this, &server, &appCfg, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      float v = server.hasArg("cal_v") ? server.arg("cal_v").toFloat() : calV;
      float ci = server.hasArg("cal_c") ? server.arg("cal_c").toFloat() : calI;
      float p = server.hasArg("cal_p") ? server.arg("cal_p").toFloat() : calP;
      handleSaveCalibration(appCfg, v, ci, p);
      markDirty();
      server.sendHeader("Location", "/calibration", true);
      server.send(303, "text/plain", "Redirect");
    });

    server.on("/reset_energy", HTTP_POST, [this, &server, &appCfg, &mqttCfg, &mqttClient, markDirty]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      resetEnergy();
      if (mqttClient.connected()) publishSensorsState(mqttClient, mqttCfg, appCfg);
      server.send(200, "text/plain", "OK");
    });

    server.on("/ha", HTTP_GET, [this, &server, &appCfg, &mqttCfg]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      String body = generateHomeAssistant(mqttCfg);
      server.send(200, "text/plain", body);
    });

    server.on("/openhab", HTTP_GET, [this, &server, &appCfg, &mqttCfg]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      String h;
      h.reserve(9500);
      h += F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
      h += F("<title>OpenHAB Config</title><style>");
      h += F("body{font-family:sans-serif;background:#f4f4f4;margin:20px;color:#333}");
      h += F(".card{background:#fff;border-radius:8px;padding:20px;margin-bottom:20px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}");
      h += F("h2{margin-top:0;color:#0d6efd;font-size:1.1rem;border-bottom:1px solid #eee;padding-bottom:8px}");
      h += F("pre{background:#f8f9fa;padding:12px;border:1px solid #ddd;border-radius:4px;overflow-x:auto;font-size:13px;white-space:pre-wrap;word-break:break-all}");
      h += F("</style></head><body>");
      h += F("<a href='/config' style='text-decoration:none;color:#666;font-size:14px'>&larr; Inapoi la configurari</a>");
      h += F("<h1 style='font-size:1.5rem;margin:15px 0'>Configurare OpenHAB</h1>");
      h += generateOpenHAB(mqttCfg);
      h += F("</body></html>");
      server.send(200, "text/html", h);
    });

    server.on("/esphome", HTTP_GET, [this, &server, &appCfg]() {
      if (!checkHardwareAuth(server, appCfg)) return;
      String body = generateESPHome(appCfg);
      server.send(200, "text/plain", body);
    });
  }
};

const int DeviceHardware::RELAY_PINS[DEVICE_RELAY_COUNT] = {14, 12, 13, 5};
const bool DeviceHardware::RELAY_INVERTED[DEVICE_RELAY_COUNT] = {false, false, false, true};
