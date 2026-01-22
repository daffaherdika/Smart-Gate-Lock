#include <HardwareSerial.h>

// Gunakan UART1 untuk komunikasi ke Kontrol Box
HardwareSerial CtrlBoxSerial(1);

// Definisi Pin
const int BUTTON_PIN = 25;
const int PB_TX_PIN  = 12;
const int PB_RX_PIN  = 14;

// Variabel Debounce
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool lastButtonState = HIGH;
bool currentButtonState = HIGH;

void setup() {
  // 1. Inisialisasi komunikasi ke Kontrol Box (PENTING: Jangan dihapus)
  // RX=14, TX=12, Baud=9600
  CtrlBoxSerial.begin(9600, SERIAL_8N1, PB_RX_PIN, PB_TX_PIN);

  // 2. Inisialisasi Tombol
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Set status awal agar sinkron
  lastButtonState = digitalRead(BUTTON_PIN);
  currentButtonState = lastButtonState;
}

void loop() {
  int reading = digitalRead(BUTTON_PIN);

  // Jika input berubah (karena ditekan atau noise), reset timer debounce
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  // Cek apakah waktu debounce sudah terlewati
  if ((millis() - lastDebounceTime) > debounceDelay) {
    
    // Jika status tombol benar-benar berubah
    if (reading != currentButtonState) {
      currentButtonState = reading;

      // Aksi HANYA jika tombol DITEKAN (Active LOW)
      if (currentButtonState == LOW) {
        // Format pesan JSON sesuai protokol sistem kamu
        String message = "{\"id\":\"PB_ESP32\",\"cmd\":\"open_req\",\"ts\":" + String(millis()) + "}\n";
        
        // Kirim ke Kontrol Box
        CtrlBoxSerial.print(message);
      }
    }
  }

  // Simpan pembacaan terakhir untuk loop selanjutnya
  lastButtonState = reading;
  
  // Delay kecil untuk menjaga stabilitas CPU
  delay(10);
}