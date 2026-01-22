#include "globals.h"

// ============ CONFIGURATION ============
const char* ssid = "Jayantara-Guest_Ext";
const char* password = "tamujalin";
const char* mqtt_server = "172.17.2.19";
const int mqtt_port = 1883;
const char* mqtt_user = "kuncipagar";
const char* mqtt_password = "242424";

// Hardware Pins
const int RELAY_PIN = 27;
const int RS485_DE_RE_PIN = 25;
const unsigned long RELAY_ON_DURATION_MS = 5000;

// ============ GLOBAL VARIABLES ============
bool relayActive = false;
unsigned long relayStartTime = 0;
bool timeSynced = false;
struct tm timeinfo;
bool haOnline = false;
unsigned long lastLogSyncAttempt = 0;
const int MAX_LOG_BUFFER = 300;

// Objects
WiFiClient espClient;
PubSubClient mqttClient(espClient);
std::vector<UserData> userDatabase;
std::vector<AccessLog> logBuffer;

// Schedules
RoleSchedule karyawanSchedule = {7, 0, 19, 0, "Senin - Jumat"};
RoleSchedule officeBoySchedule = {6, 0, 20, 0, "Senin - Sabtu"};
RoleSchedule anakMagangSchedule = {7, 0, 17, 0, "Senin - Jumat"};

// Serial Communication
HardwareSerial PushButtonSerial(1);
String serial2Buffer = "";
String pbBuffer = "";
const int PB_RX = 14, PB_TX = 12;

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  PushButtonSerial.begin(9600, SERIAL_8N1, PB_RX, PB_TX);
  
  // Setup Hardware
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);

  Serial.println("ESP32 Gate Controller Starting...");
  
  // Init Storage
  if (SPIFFS.begin(true)) {
    loadUserDatabaseFromSPIFFS();
    loadScheduleConfigFromSPIFFS();
    loadLogBufferFromSPIFFS();
    Serial.printf("Loaded: %d users, %d logs\n", userDatabase.size(), logBuffer.size());
  }

  // Setup Network
  setupWiFi();
  Serial.println("IP: " + WiFi.localIP().toString());
  setupMQTT();
  
  // Sync Time
  configTime(25200, 0, "pool.ntp.org");
  timeSynced = getLocalTime(&timeinfo);
  Serial.println(timeSynced ? "Time synced" : "Time sync failed");
  
  Serial.println("Setup complete");
}

// ============ MAIN LOOP ============
void loop() {
  // MQTT Connection
  static unsigned long lastReconnect = 0;
  if (!mqttClient.connected()) {
    haOnline = false;
    if (millis() - lastReconnect > 5000) {
      lastReconnect = millis();
      reconnectMQTT();
    }
  } else {
    haOnline = true;
    mqttClient.loop();
  }
  
  // Core Functions
  handleNanoMessages();
  checkRelay();
  checkAndSyncLogs();
  
  // Push Button Serial
  while (PushButtonSerial.available()) {
    char c = PushButtonSerial.read();
    if (c == '\n') {
      processPbMessage(pbBuffer);
      pbBuffer = "";
    } else {
      pbBuffer += c;
    }
  }
  
  yield();
  delay(10);
}