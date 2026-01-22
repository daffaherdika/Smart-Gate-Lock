#pragma once

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>
#include <vector>

// ============ CONFIGURATION ============
extern const char* ssid, *password, *mqtt_server, *mqtt_user, *mqtt_password;
extern const int mqtt_port;
extern const int RELAY_PIN, RS485_DE_RE_PIN, BUTTON_PIN;
extern const unsigned long RELAY_ON_DURATION_MS;
extern const int MAX_LOG_BUFFER;

// ============ DATA STRUCTURES ============
struct UserData {
  String id, name, role, expire, secret;
};

struct RoleSchedule {
  int start_hour, start_minute, end_hour, end_minute;
  String work_days;
};

struct AccessLog {
  String timestamp, userId, userName, role, status, reason, source;
};

// ============ GLOBAL VARIABLES ============
extern bool relayActive, timeSynced, lastButtonState;
extern unsigned long relayStartTime;
extern struct tm timeinfo;
extern WiFiClient espClient;
extern PubSubClient mqttClient;
extern std::vector<UserData> userDatabase;
extern std::vector<AccessLog> logBuffer;
extern RoleSchedule karyawanSchedule, officeBoySchedule, anakMagangSchedule;
extern bool haOnline;
extern unsigned long lastLogSyncAttempt;

// ============ NETWORK FUNCTIONS ============
void setupWiFi();
void setupMQTT();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishMQTTDiscovery();

// ============ USER MANAGEMENT ============
void handleUserManagement(String payload);
void publishDatabaseToHA();
void cleanupInvalidUsers();
UserData* findUserById(const String& id);

// ============ SCHEDULE MANAGEMENT ============
void handleScheduleConfig(String payload);
void loadScheduleConfigFromSPIFFS();
void saveScheduleConfigToSPIFFS();
bool isWorkDay(String workDaysConfig, int dayOfWeek);
bool isWithinTimeRange(int currentHour, int currentMinute, RoleSchedule schedule);

// ============ ACCESS CONTROL ============
void grantAccess(String source);
void denyAccess(String reason);
void checkRelay();

// ============ LOGGING SYSTEM ============
void addLogToBuffer(const AccessLog& log);
void saveLogBufferToSPIFFS();
void loadLogBufferFromSPIFFS();
void publishLogBatchToHA();
void clearLogBuffer();
void publishLogBufferStatus();
void checkAndSyncLogs();
void logAccessToHA(String userId, String userName, String role, String status, String reason, String source = "RFID");

// ============ COMMUNICATION ============
void sendRS485Message(String message);
void handleNanoMessages();
void publishCardTapEvent(String cardId);
void notifyUnknownCard(String cardId);
void processPbMessage(String msg);
void activateRelay();

// ============ STORAGE ============
void loadUserDatabaseFromSPIFFS();
void saveUserDatabaseToSPIFFS(const std::vector<UserData>& db);

// ============ UTILITIES ============
String getFormattedTime();