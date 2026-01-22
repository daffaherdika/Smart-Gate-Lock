#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PN532.h>
#include <SoftwareSerial.h>
#include <Keypad.h>

// Pin Hardware 
#define LCD_I2C_ADDRESS 0x27
#define RS485_RX_PIN 3
#define RS485_TX_PIN 2
#define RS485_DE_RE_PIN 4

// Pin Keypad 
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[ROWS] = {12, 11, 10, 9};
byte colPins[COLS] = {8, 7, 6};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Komunikasi & Perangkat
SoftwareSerial rs485Serial(RS485_RX_PIN, RS485_TX_PIN);
Adafruit_PN532 nfc(13, 5); // GANTI PIN SPI JIKA BERBEDA
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);

// Variabel Global 
bool regMode = false;
bool otpMode = false; // Mode menunggu input OTP
char rs485Buf[128];
int rs485Idx = 0;
bool rs485NewLine = false;
String inputCode = "";
const String UNLOCK_CODE = "2424"; // Kode Darurat/Master

void setup() {
  Serial.begin(9600);
  Wire.begin();
  rs485Serial.begin(9600);
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcd.print("Menghubungkan...");

  nfc.begin();
  if (!nfc.getFirmwareVersion()) {
    lcd.clear();
    lcd.print("PN532 Error!");
    while(1);
  }
  nfc.SAMConfig();
  lcd.clear();
  showLCD("Tempelkan Kartu", "");
}

void loop() {
  char key = keypad.getKey();
  if (key) {
    handleKeyInput(key);
  }

  readRs485Line();
  handleRs485Msg();

  // Hanya scan kartu jika TIDAK sedang menunggu OTP
  if (!otpMode) {
    if (regMode) {
      scanNewCard();
    } else {
      scanAccess();
    }
  }
}

void handleKeyInput(char k) {
  if (k == '*') { // Reset input
    inputCode = "";
    if(otpMode) showLCD("Input OTP:", inputCode);
    else showLCD("Input Pin", inputCode);
    
  } else if (k == '#') { // Kirim input
    if (otpMode) {
      // [MODE OTP]: Kirim kode ke ESP32 untuk diverifikasi
      showLCD("Verifikasi OTP..", "");
      sendToMaster("OTP:" + inputCode);
      inputCode = "";
      // Jangan matikan otpMode dulu, tunggu balasan ESP32 (GRANT/DENY)
      
    } else {
      // [MODE BIASA]: Cek kode master lokal
      if (inputCode == UNLOCK_CODE) {
        showLCD("Unlock Pagar!", "");
        sendToMaster("CMD:UNLOCK_CODE");
        delay(2000);
      } else {
        showLCD("Kode Salah!", "");
        delay(1000);
      }
      inputCode = "";
      showLCD("Tempelkan Kartu", "");
    }
    
  } else if (k >= '0' && k <= '9') { // Input angka
    if (inputCode.length() == 0) { 
      lcd.clear();
      lcd.setCursor(0, 0);
      if(otpMode) lcd.print("Input OTP:");
      else lcd.print("Input Pin:");
    }
    
    if(inputCode.length() < 6) { // Batasi max digit
        inputCode += k;
        lcd.setCursor(0, 1);
        lcd.print(inputCode);
    }
  }
}

void readRs485Line() {
  while (rs485Serial.available()) {
    char c = rs485Serial.read();
    if (c == '\n') {
      if (rs485Idx > 0) {
        rs485NewLine = true;
        rs485Buf[rs485Idx] = '\0';
      }
      rs485Idx = 0;
    } else if (c != '\r' && rs485Idx < 127) {
      rs485Buf[rs485Idx++] = c;
    }
  }
}

void handleRs485Msg() {
  if (rs485NewLine) {
    String msg = String(rs485Buf);
    msg.trim();
    rs485NewLine = false;

    if (msg.startsWith("CMD:GRANT,")) {
      String name = msg.substring(10);
      showLCD("Selamat Datang", name);
      otpMode = false; // Reset mode OTP
      delay(4000);
      showLCD("Tempelkan Kartu", "");
      
    } else if (msg.startsWith("CMD:DENY,")) {
      String reason = msg.substring(9);
      showLCD("Akses Ditolak!", reason);
      // Jika ditolak karena salah kode OTP, user harus tap ulang kartu (keamanan)
      otpMode = false; 
      delay(4000);
      showLCD("Tempelkan Kartu", "");
      
    } else if (msg == "CMD:ENTER_OTP") {
      // [PERINTAH BARU DARI ESP32]
      otpMode = true;
      inputCode = "";
      showLCD("Butuh Verifikasi", "Input OTP Google");
      // Bunyi beep pendek (jika ada buzzer) bisa ditambahkan di sini
      
    } else if (msg.startsWith("CMD:SUCCESS,")) {
      String m = msg.substring(12);
      showLCD("Berhasil!", m);
      delay(4000);
      regMode = false;
      showLCD("Tempelkan Kartu", "");
      
    } else if (msg == "CMD:WAIT_FOR_NEW_CARD") {
      regMode = true;
      showLCD("Registrasi Baru", "Tempelkan Kartu");
    }
  }
}

String getUID() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLen;
  // Timeout dipersingkat agar tidak nge-lag keypad
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);
  
  if (success) {
    String uidStr = "";
    for (uint8_t i = 0; i < uidLen; i++) {
      if (uid[i] < 0x10) uidStr += "0";
      uidStr += String(uid[i], HEX);
    }
    uidStr.toUpperCase();
    return uidStr;
  }
  return "";
}

void scanAccess() {
  String uid = getUID();
  if (uid != "") {
    showLCD("Memverifikasi...", "");
    sendToMaster("UID:" + uid);
    // Jeda sedikit untuk memberi waktu ESP32 menjawab (apakah butuh OTP atau Grant)
    delay(500); 
  }
}

void scanNewCard() {
  String uid = getUID();
  if (uid != "") {
    showLCD("Mengirim UID...", "");
    sendToMaster("NEW_UID:" + uid);
    regMode = false;
  }
}

void showLCD(String l1, String l2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

void sendToMaster(String msg) {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  rs485Serial.println(msg);
  rs485Serial.flush();
  digitalWrite(RS485_DE_RE_PIN, LOW);
}