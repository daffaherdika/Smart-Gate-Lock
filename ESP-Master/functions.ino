#include "globals.h"
#include "TOTP.h" 

// Global Variables untuk OTP
bool isWaitingForOTP = false;
String pendingUserID = "";
unsigned long otpStartTime = 0;
const unsigned long OTP_TIMEOUT = 30000; 

// ============ NETWORK ============
void setupWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 20) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nConnected!" : "\nFailed!");
}

void setupMQTT() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(4096);
  reconnectMQTT();
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.print("Connecting MQTT...");
  // KEMBALI KE ID UTAMA
  if (mqttClient.connect("esp32_kunci_pagar", mqtt_user, mqtt_password, 
                         "office/gate/status", 0, true, "offline")) {
    Serial.println("OK");
    haOnline = true;
    
    // KEMBALI KE TOPIK UTAMA
    mqttClient.subscribe("office/gate/command");
    mqttClient.subscribe("office/gate/user_management");
    mqttClient.subscribe("office/gate/schedule_config");
    mqttClient.subscribe("gate/logs/cmd");
    
    mqttClient.publish("office/gate/status", "online", true);
    
    delay(300);
    publishMQTTDiscovery();
    delay(300);
    publishDatabaseToHA();
    delay(300);
    publishLogBufferStatus();
    
    configTime(25200, 0, "pool.ntp.org");
    timeSynced = getLocalTime(&timeinfo, 10000);
  } else {
    Serial.printf("Failed, rc=%d\n", mqttClient.state());
    haOnline = false;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);

  if (topicStr == "office/gate/command") {
    if (msg.indexOf("open") >= 0) grantAccess("Home Assistant");
    else if (msg.indexOf("request_database") >= 0) publishDatabaseToHA();
    else if (msg.indexOf("backup_database") >= 0) publishDatabaseToHA();
    else if (msg.indexOf("cleanup_invalid") >= 0) {
      cleanupInvalidUsers();
      delay(200);
      publishDatabaseToHA();
    }
  }
  else if (topicStr == "office/gate/user_management") handleUserManagement(msg);
  else if (topicStr == "office/gate/schedule_config") handleScheduleConfig(msg);
  else if (topicStr == "gate/logs/cmd") {
    if (msg == "sync") publishLogBatchToHA();
    else if (msg == "clear") clearLogBuffer();
  }
}

// ============ TOTP HELPER FUNCTIONS ============
int base32Decode(String secret, uint8_t* buffer) {
  secret.toUpperCase();
  int bufferLen = 0;
  int val = 0;
  int valb = 0;
  
  for (int i = 0; i < secret.length(); i++) {
    char c = secret.charAt(i);
    int d = -1;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= '2' && c <= '7') d = c - '2' + 26;
    
    if (d >= 0) {
      val = (val << 5) | d;
      valb += 5;
      if (valb >= 8) {
        buffer[bufferLen++] = (byte)((val >> (valb - 8)) & 0xFF);
        valb -= 8;
      }
    }
  }
  return bufferLen;
}

// ============ TOTP HELPER FUNCTIONS ============
bool verifyTOTP(String secretKey, String inputCode) {
  // 1. Pastikan waktu sudah sinkron
  if (!timeSynced || !getLocalTime(&timeinfo)) return false;
  
  // 2. Decode Secret Key dari Base32 ke Byte
  uint8_t key[20]; 
  int keyLen = base32Decode(secretKey, key);
  
  // 3. Siapkan Object TOTP
  TOTP totp = TOTP(key, keyLen);
  long currentEpoch = mktime(&timeinfo); // Ambil waktu UTC sekarang
  
  // 4. Hitung 3 Kemungkinan Kode (Toleransi Waktu)
  String codeNow = String(totp.getCode(currentEpoch));
  String codePrev = String(totp.getCode(currentEpoch - 30));
  String codeNext = String(totp.getCode(currentEpoch + 30));
  
  // 5. Cek Kecocokan
  if (inputCode == codeNow || inputCode == codePrev || inputCode == codeNext) {
    return true;
  }
  return false;
}

void checkOTPTimeout() {
  if (isWaitingForOTP && (millis() - otpStartTime > OTP_TIMEOUT)) {
    isWaitingForOTP = false;
    Serial.println("OTP Timeout!");
    sendRS485Message("CMD:BEEP_ERROR");
    logAccessToHA(pendingUserID, "Unknown", "unknown", "denied", "OTP Timeout", "RFID");
    pendingUserID = "";
  }
}

// ============ USER MANAGEMENT ============
void handleUserManagement(String payload) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload)) return;
  String action = doc["action"].as<String>();
  bool changed = false;
  
  if (action == "add") {
    UserData newUser = {
      doc["user"]["id"].as<String>(),
      doc["user"]["name"].as<String>(),
      doc["user"]["role"].as<String>(),
      doc["user"]["expire"].as<String>(),
      doc["user"]["secret"].as<String>() 
    };
    
    if (newUser.id == "" || newUser.id == "unknown") return;
    UserData* existing = findUserById(newUser.id);
    if (existing) *existing = newUser;
    else userDatabase.push_back(newUser);
    changed = true;
  }
  else if (action == "update") {
    UserData* user = findUserById(doc["user"]["id"].as<String>());
    if (user) {
      user->name = doc["user"]["name"].as<String>();
      user->role = doc["user"]["role"].as<String>();
      user->expire = doc["user"]["expire"].as<String>();
      if (doc["user"].containsKey("secret")) {
         user->secret = doc["user"]["secret"].as<String>();
      }
      changed = true;
    }
  }
  else if (action == "delete") {
    String userId = doc["user_id"].as<String>();
    userId.trim();
    for (auto it = userDatabase.begin(); it != userDatabase.end(); ++it) {
      if (it->id == userId) {
        userDatabase.erase(it);
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    saveUserDatabaseToSPIFFS(userDatabase);
    delay(200);
    publishDatabaseToHA();
  }
}

void publishDatabaseToHA() {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(10000);
  doc["total_users"] = userDatabase.size();
  doc["timestamp"] = millis();
  doc["last_sync"] = getFormattedTime();
  
  JsonArray users = doc.createNestedArray("users");
  for (const auto& u : userDatabase) {
    JsonObject user = users.createNestedObject();
    user["id"] = u.id;
    user["name"] = u.name;
    user["role"] = u.role;
    user["expire"] = u.expire;
    user["secret"] = u.secret;
  }

  String output;
  serializeJson(doc, output);
  mqttClient.publish("office/gate/user_database", output.c_str(), false);
  mqttClient.publish("office/gate/database_size", String(userDatabase.size()).c_str(), true);
}

void cleanupInvalidUsers() {
  int removed = 0;
  for (auto it = userDatabase.begin(); it != userDatabase.end(); ) {
    if (it->id == "unknown" || it->id == "") {
      it = userDatabase.erase(it);
      removed++;
    } else {
      ++it;
    }
  }
  if (removed > 0) {
    saveUserDatabaseToSPIFFS(userDatabase);
    Serial.printf("Removed %d invalid users\n", removed);
  }
}

UserData* findUserById(const String& id) {
  for (auto& user : userDatabase) {
    if (user.id == id) return &user;
  }
  return nullptr;
}

// ============ STORAGE ============
void loadUserDatabaseFromSPIFFS() {
  userDatabase.clear();
  if (!SPIFFS.exists("/userdb.json")) return;
  File file = SPIFFS.open("/userdb.json", "r");
  if (!file) return;
  DynamicJsonDocument doc(10000);
  if (!deserializeJson(doc, file)) {
    JsonArray users = doc["users"];
    for (JsonVariant user : users) {
      userDatabase.push_back({
        user["id"].as<String>(),
        user["name"].as<String>(),
        user["role"].as<String>(),
        user["expire"].as<String>(),
        user["secret"].as<String>()
      });
    }
  }
  file.close();
}

void saveUserDatabaseToSPIFFS(const std::vector<UserData>& db) {
  File file = SPIFFS.open("/userdb.json", "w");
  if (!file) return;
  DynamicJsonDocument doc(10000);
  JsonArray users = doc.createNestedArray("users");
  for (const auto& u : db) {
    JsonObject user = users.createNestedObject();
    user["id"] = u.id;
    user["name"] = u.name;
    user["role"] = u.role;
    user["expire"] = u.expire;
    user["secret"] = u.secret;
  }
  serializeJson(doc, file);
  file.close();
}

// ============ LOGGING SYSTEM ============
void addLogToBuffer(const AccessLog& log) {
  logBuffer.push_back(log);
  if (logBuffer.size() > MAX_LOG_BUFFER) {
    logBuffer.erase(logBuffer.begin());
  }
  static unsigned long lastSave = 0;
  if (millis() - lastSave > 5000) {
    saveLogBufferToSPIFFS();
    lastSave = millis();
  }
  publishLogBufferStatus();
}

void saveLogBufferToSPIFFS() {
  File file = SPIFFS.open("/logbuffer.json", "w");
  if (!file) return;
  DynamicJsonDocument doc(8192);
  JsonArray logs = doc.createNestedArray("logs");
  int startIndex = (logBuffer.size() > 50) ? (logBuffer.size() - 50) : 0;
  for (size_t i = startIndex; i < logBuffer.size(); i++) {
    JsonObject logObj = logs.createNestedObject();
    logObj["timestamp"] = logBuffer[i].timestamp;
    logObj["user_id"] = logBuffer[i].userId;
    logObj["user_name"] = logBuffer[i].userName;
    logObj["role"] = logBuffer[i].role;
    logObj["status"] = logBuffer[i].status;
    logObj["reason"] = logBuffer[i].reason;
    logObj["source"] = logBuffer[i].source;
    yield();
  }
  serializeJson(doc, file);
  file.close();
  yield();
}

void loadLogBufferFromSPIFFS() {
  logBuffer.clear();
  if (!SPIFFS.exists("/logbuffer.json")) return;
  File file = SPIFFS.open("/logbuffer.json", "r");
  if (!file) return;
  DynamicJsonDocument doc(8192);
  if (!deserializeJson(doc, file)) {
    JsonArray logs = doc["logs"];
    for (JsonVariant log : logs) {
      logBuffer.push_back({
        log["timestamp"].as<String>(),
        log["user_id"].as<String>(),
        log["user_name"].as<String>(),
        log["role"].as<String>(),
        log["status"].as<String>(),
        log["reason"].as<String>(),
        log["source"].as<String>()
      });
      yield();
    }
  }
  file.close();
  Serial.printf("Loaded %d logs\n", logBuffer.size());
}

void publishLogBatchToHA() {
  if (!mqttClient.connected() || logBuffer.empty()) return;
  const int MAX_BATCH_SIZE = 10;
  int batchCount = 0;
  while (!logBuffer.empty() && batchCount < 5) {
    int batchSize = min((int)logBuffer.size(), MAX_BATCH_SIZE);
    DynamicJsonDocument doc(8192);
    doc["count"] = batchSize;
    doc["timestamp"] = getFormattedTime();
    JsonArray logs = doc.createNestedArray("logs");
    for (int i = 0; i < batchSize; i++) {
      JsonObject logObj = logs.createNestedObject();
      logObj["timestamp"] = logBuffer[i].timestamp;
      logObj["user_id"] = logBuffer[i].userId;
      logObj["user_name"] = logBuffer[i].userName;
      logObj["role"] = logBuffer[i].role;
      logObj["status"] = logBuffer[i].status;
      logObj["reason"] = logBuffer[i].reason;
      logObj["source"] = logBuffer[i].source;
    }
    String output;
    serializeJson(doc, output);
    if (mqttClient.publish("gate/logs/batch", output.c_str(), false)) {
      logBuffer.erase(logBuffer.begin(), logBuffer.begin() + batchSize);
      saveLogBufferToSPIFFS();
      yield();
      delay(100);
    } else {
      break;
    }
    batchCount++;
  }
  publishLogBufferStatus();
}

void clearLogBuffer() {
  logBuffer.clear();
  if (SPIFFS.exists("/logbuffer.json")) SPIFFS.remove("/logbuffer.json");
  yield();
  publishLogBufferStatus();
}

void publishLogBufferStatus() {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(512);
  doc["buffer_count"] = logBuffer.size();
  doc["max_buffer"] = MAX_LOG_BUFFER;
  doc["timestamp"] = getFormattedTime();
  doc["remaining"] = MAX_LOG_BUFFER - logBuffer.size();
  String output;
  serializeJson(doc, output);
  mqttClient.publish("gate/logs/status", output.c_str(), true);
}

void checkAndSyncLogs() {
  if (haOnline && mqttClient.connected() && !logBuffer.empty()) {
    if (millis() - lastLogSyncAttempt >= 300000) {
      lastLogSyncAttempt = millis();
      publishLogBatchToHA();
    }
  }
}

// ============ SCHEDULE ============
void loadScheduleConfigFromSPIFFS() {
  if (!SPIFFS.exists("/schedule.json")) return;
  File file = SPIFFS.open("/schedule.json", "r");
  if (!file) return;
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, file);
  file.close();
  if (doc.containsKey("karyawan")) {
    karyawanSchedule = {doc["karyawan"]["start_hour"], doc["karyawan"]["start_minute"], doc["karyawan"]["end_hour"], doc["karyawan"]["end_minute"], doc["karyawan"]["work_days"].as<String>()};
  }
  if (doc.containsKey("office_boy")) {
    officeBoySchedule = {doc["office_boy"]["start_hour"], doc["office_boy"]["start_minute"], doc["office_boy"]["end_hour"], doc["office_boy"]["end_minute"], doc["office_boy"]["work_days"].as<String>()};
  }
  if (doc.containsKey("anak_magang")) {
    anakMagangSchedule = {doc["anak_magang"]["start_hour"], doc["anak_magang"]["start_minute"], doc["anak_magang"]["end_hour"], doc["anak_magang"]["end_minute"], doc["anak_magang"]["work_days"].as<String>()};
  }
}

void saveScheduleConfigToSPIFFS() {
  File file = SPIFFS.open("/schedule.json", "w");
  if (!file) return;
  DynamicJsonDocument doc(1024);
  JsonObject k = doc.createNestedObject("karyawan");
  k["start_hour"] = karyawanSchedule.start_hour; k["start_minute"] = karyawanSchedule.start_minute; k["end_hour"] = karyawanSchedule.end_hour; k["end_minute"] = karyawanSchedule.end_minute; k["work_days"] = karyawanSchedule.work_days;
  JsonObject o = doc.createNestedObject("office_boy");
  o["start_hour"] = officeBoySchedule.start_hour; o["start_minute"] = officeBoySchedule.start_minute; o["end_hour"] = officeBoySchedule.end_hour; o["end_minute"] = officeBoySchedule.end_minute; o["work_days"] = officeBoySchedule.work_days;
  JsonObject a = doc.createNestedObject("anak_magang");
  a["start_hour"] = anakMagangSchedule.start_hour; a["start_minute"] = anakMagangSchedule.start_minute; a["end_hour"] = anakMagangSchedule.end_hour; a["end_minute"] = anakMagangSchedule.end_minute; a["work_days"] = anakMagangSchedule.work_days;
  serializeJson(doc, file);
  file.close();
}

void handleScheduleConfig(String payload) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload)) return;
  if (doc.containsKey("karyawan")) karyawanSchedule = {doc["karyawan"]["start_hour"], doc["karyawan"]["start_minute"], doc["karyawan"]["end_hour"], doc["karyawan"]["end_minute"], doc["karyawan"]["work_days"].as<String>()};
  if (doc.containsKey("office_boy")) officeBoySchedule = {doc["office_boy"]["start_hour"], doc["office_boy"]["start_minute"], doc["office_boy"]["end_hour"], doc["office_boy"]["end_minute"], doc["office_boy"]["work_days"].as<String>()};
  if (doc.containsKey("anak_magang")) anakMagangSchedule = {doc["anak_magang"]["start_hour"], doc["anak_magang"]["start_minute"], doc["anak_magang"]["end_hour"], doc["anak_magang"]["end_minute"], doc["anak_magang"]["work_days"].as<String>()};
  saveScheduleConfigToSPIFFS();
}

bool isWorkDay(String workDaysConfig, int dayOfWeek) {
  if (workDaysConfig == "Setiap Hari" || workDaysConfig == "Senin - Minggu") return true;
  if (workDaysConfig == "Senin - Jumat") return (dayOfWeek >= 1 && dayOfWeek <= 5);
  if (workDaysConfig == "Senin - Sabtu") return (dayOfWeek >= 1 && dayOfWeek <= 6);
  return false;
}

bool isWithinTimeRange(int currentHour, int currentMinute, RoleSchedule schedule) {
  int current = currentHour * 60 + currentMinute;
  int start = schedule.start_hour * 60 + schedule.start_minute;
  int end = schedule.end_hour * 60 + schedule.end_minute;
  return (current >= start && current < end);
}

// ============ ACCESS CONTROL ============
void grantAccess(String source) {
  digitalWrite(RELAY_PIN, HIGH);
  relayStartTime = millis();
  relayActive = true;
  sendRS485Message("CMD:GRANT," + source);
  if (mqttClient.connected()) mqttClient.publish("office/gate/relay_state", "ON");
}

void denyAccess(String reason) {
  sendRS485Message("CMD:DENY," + reason);
}

void checkRelay() {
  if (relayActive && (millis() - relayStartTime >= RELAY_ON_DURATION_MS)) {
    digitalWrite(RELAY_PIN, LOW);
    relayActive = false;
    if (mqttClient.connected()) mqttClient.publish("office/gate/relay_state", "OFF");
  }
}

// ============ MAIN LOGIC (NANO MESSAGES) ============
void sendRS485Message(String message) {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  Serial2.println(message);
  Serial2.flush();
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

void handleNanoMessages() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      String msg = String(serial2Buffer);
      msg.trim();
      serial2Buffer = "";
      
      if (msg == "CMD:UNLOCK_CODE") {
        grantAccess("Kode Keypad");
        logAccessToHA("KEYPAD", "Keypad Code", "keypad", "granted", "Valid code", "Keypad");
      }
      else if (msg.startsWith("OTP:")) {
        String code = msg.substring(4);
        if (isWaitingForOTP) {
          UserData* user = findUserById(pendingUserID);
          if (user && verifyTOTP(user->secret, code)) {
            Serial.println("OTP Valid!");
            isWaitingForOTP = false;
            grantAccess(user->name);
            logAccessToHA(pendingUserID, user->name, user->role, "granted", "Valid OTP", "RFID+OTP");
          } else {
            Serial.println("OTP Invalid!");
            sendRS485Message("CMD:DENY,Salah Kode");
          }
        }
      }
      else if (msg.startsWith("NEW_UID:")) {
        String uid = msg.substring(8);
        if (mqttClient.connected()) {
          String payload = "{\"card_id_for_registration\": \"" + uid + "\"}";
          mqttClient.publish("office/gate/registration", payload.c_str());
        }
      }
      else if (msg.startsWith("UID:")) {
        String uid = msg.substring(4);
        publishCardTapEvent(uid);
        
        if (isWaitingForOTP) {
          isWaitingForOTP = false;
          Serial.println("OTP Cancelled by new tap");
        }

        UserData* user = findUserById(uid);
        if (!user) {
          denyAccess("Kartu Tdk Daftar");
          logAccessToHA(uid, "Unknown", "unknown", "denied", "Not registered", "RFID");
          notifyUnknownCard(uid);
          return;
        }

        if (user->expire != "null" && user->expire != "") {
          struct tm expire_tm = {0};
          int year, month, day;
          if (sscanf(user->expire.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
            expire_tm.tm_year = year - 1900;
            expire_tm.tm_mon = month - 1;
            expire_tm.tm_mday = day;
            expire_tm.tm_hour = 23;
            expire_tm.tm_min = 59;
            expire_tm.tm_sec = 59;
            if (mktime(&expire_tm) < mktime(&timeinfo)) {
              denyAccess("Kartu Kadaluarsa");
              logAccessToHA(uid, user->name, user->role, "denied", "Card expired", "RFID");
              return;
            }
          }
        }

        RoleSchedule schedule;
        if (user->role == "karyawan") schedule = karyawanSchedule;
        else if (user->role == "office_boy") schedule = officeBoySchedule;
        else if (user->role == "anak_magang") schedule = anakMagangSchedule;
        else {
          denyAccess("Role Invalid");
          logAccessToHA(uid, user->name, user->role, "denied", "Invalid role", "RFID");
          return;
        }
        
        if (!isWorkDay(schedule.work_days, timeinfo.tm_wday) || 
            !isWithinTimeRange(timeinfo.tm_hour, timeinfo.tm_min, schedule)) {
          denyAccess("Akses Ditolak");
          logAccessToHA(uid, user->name, user->role, "denied", "Schedule/Time", "RFID");
          return;
        }

        if (user->secret != "" && user->secret != "null") {
          Serial.println("User needs OTP. Waiting...");
          isWaitingForOTP = true;
          pendingUserID = uid;
          otpStartTime = millis();
          sendRS485Message("CMD:ENTER_OTP"); 
        } else {
          grantAccess(user->name);
          logAccessToHA(uid, user->name, user->role, "granted", "Valid", "RFID");
        }
      }
    } else if (c != '\r') {
      serial2Buffer += c;
    }
  }
}

void publishCardTapEvent(String cardId) {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(512);
  doc["card_id"] = cardId;
  doc["timestamp"] = getFormattedTime();
  UserData* user = findUserById(cardId);
  doc["status"] = user ? "registered" : "unknown";
  doc["user_name"] = user ? user->name : "Unknown Card";
  doc["role"] = user ? user->role : "none";
  String output; serializeJson(doc, output);
  mqttClient.publish("office/gate/card_tap", output.c_str(), false);
}

void notifyUnknownCard(String cardId) {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(256);
  doc["card_id"] = cardId;
  doc["message"] = "Kartu tidak terdaftar!";
  doc["timestamp"] = getFormattedTime();
  String output; serializeJson(doc, output);
  mqttClient.publish("office/gate/unknown_card", output.c_str(), false);
}

void logAccessToHA(String userId, String userName, String role, String status, String reason, String source) {
  AccessLog log = {getFormattedTime(), userId, userName, role, status, reason, source};
  addLogToBuffer(log);
  if (mqttClient.connected()) {
    DynamicJsonDocument doc(512);
    doc["user_id"] = userId; doc["user_name"] = userName; doc["role"] = role;
    doc["status"] = status; doc["reason"] = reason; doc["source"] = source;
    doc["timestamp"] = log.timestamp;
    String output; serializeJson(doc, output);
    mqttClient.publish("office/gate/access_log", output.c_str());
  }
}

void processPbMessage(String msg) {
  if (msg.indexOf("\"cmd\":\"open_req\"") != -1) {
     grantAccess("Tombol Manual");
     logAccessToHA("PB_ESP32", "Push Button", "external", "granted", "Button pressed", "PushButton");
  }
}

void activateRelay() { grantAccess("Internal"); }

void publishMQTTDiscovery() {
  if (!mqttClient.connected()) return;
  
  Serial.println("Publishing MQTT Discovery...");
  DynamicJsonDocument doc(1024);
  String output;
  JsonObject dev;
  
  auto addDevice = [&]() {
    dev = doc.createNestedObject("device");
    dev["identifiers"].add("esp32_gate_controller");
    dev["name"] = "Gate Controller";
    dev["manufacturer"] = "Custom";
    dev["model"] = "ESP32-WROOM";
  };
  
 
  // 1. User DB
  doc.clear(); doc["name"] = "Gate User Database"; doc["state_topic"] = "office/gate/user_database"; doc["unique_id"] = "esp32_gate_user_database"; doc["value_template"] = "{{ value_json.total_users | default(0) }}"; doc["json_attributes_topic"] = "office/gate/user_database"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/sensor/esp32_gate/user_database/config", output.c_str(), true); delay(50);
  
  // 2. Last Access
  doc.clear(); doc["name"] = "Gate Last Access"; doc["state_topic"] = "office/gate/access_log"; doc["unique_id"] = "esp32_gate_last_access"; doc["value_template"] = "{{ value_json.user_name | default('N/A') }}"; doc["icon"] = "mdi:account-clock"; doc["json_attributes_topic"] = "office/gate/access_log"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/sensor/esp32_gate/last_access/config", output.c_str(), true); delay(50);
  
  // 3. Status
  doc.clear(); doc["name"] = "Gate ESP32 Status"; doc["state_topic"] = "office/gate/status"; doc["unique_id"] = "esp32_gate_status"; doc["icon"] = "mdi:lan-connect"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/sensor/esp32_gate/status/config", output.c_str(), true); delay(50);
  
  // 4. Card Tap
  doc.clear(); doc["name"] = "Gate Last Card Tap"; doc["state_topic"] = "office/gate/card_tap"; doc["unique_id"] = "esp32_gate_card_tap"; doc["value_template"] = "{{ value_json.card_id }}"; doc["icon"] = "mdi:card-account-details"; doc["json_attributes_topic"] = "office/gate/card_tap"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/sensor/esp32_gate/card_tap/config", output.c_str(), true); delay(50);
  
  // 5. Log Buffer
  doc.clear(); doc["name"] = "Gate Log Buffer Status"; doc["state_topic"] = "gate/logs/status"; doc["unique_id"] = "esp32_gate_log_buffer"; doc["value_template"] = "{{ value_json.buffer_count | default(0) }}"; doc["icon"] = "mdi:database"; doc["json_attributes_topic"] = "gate/logs/status"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/sensor/esp32_gate/log_buffer/config", output.c_str(), true); delay(50);
  
  // 6. Switch
  doc.clear(); doc["name"] = "Gate Lock"; doc["command_topic"] = "office/gate/command"; doc["state_topic"] = "office/gate/relay_state"; doc["payload_on"] = "open"; doc["state_on"] = "ON"; doc["state_off"] = "OFF"; doc["unique_id"] = "esp32_gate_lock_switch"; doc["icon"] = "mdi:gate"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/switch/esp32_gate/lock/config", output.c_str(), true); delay(50);
  
  // 7. Binary Sensor
  doc.clear(); doc["name"] = "Gate Relay State"; doc["state_topic"] = "office/gate/relay_state"; doc["payload_on"] = "ON"; doc["payload_off"] = "OFF"; doc["unique_id"] = "esp32_gate_relay_state"; doc["device_class"] = "lock"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/binary_sensor/esp32_gate/relay_state/config", output.c_str(), true); delay(50);
  
  // 8. Button
  doc.clear(); doc["name"] = "Gate Sync Database"; doc["command_topic"] = "office/gate/command"; doc["payload_press"] = "request_database"; doc["unique_id"] = "esp32_gate_sync_button"; doc["icon"] = "mdi:sync"; addDevice(); output = ""; serializeJson(doc, output); mqttClient.publish("homeassistant/button/esp32_gate/sync_database/config", output.c_str(), true); delay(50);
  
  Serial.println("MQTT Discovery complete");
}

String getFormattedTime() {
  if (!timeSynced || !getLocalTime(&timeinfo)) return "Unknown";
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStr);
}