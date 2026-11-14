#include <WiFi.h>
#include <esp_now.h>

// Calibrated ESP32 Dev Module Pinout
const int LASER_1_PIN = 14;
const int LASER_2_PIN = 32;
const int LASER_3_PIN = 21;
const int LDR_PIN     = 5;  // LM393 OUT: LOW = laser detected, HIGH = beam broken
const int BUZZER_PIN  = 4;

// Central ESP32-S3 Receiver MAC Address (68:EE:8F:4C:38:94)
uint8_t centralS3MAC[] = {0x68, 0xEE, 0x8F, 0x4C, 0x38, 0x94};

// Unified ESP-NOW Data Payload (matches Door Lock ESP32-S3)
typedef struct struct_message {
  char eventCode; // 'L' = Laser Trip, 'C' = Laser Clear
} struct_message;

struct_message eventData;
esp_now_peer_info_t peerInfo;

int lastStableState = -1;
int lastRawState = -1;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY_MS = 40;

void sendLaserEvent(char code) {
  eventData.eventCode = code;
  esp_err_t result = esp_now_send(centralS3MAC, (uint8_t *)&eventData, sizeof(eventData));
  Serial.print("[ESP-NOW] Laser Event '");
  Serial.print(code);
  Serial.print("' -> Central S3: ");
  Serial.println(result == ESP_OK ? "OK" : "ERR");
}

void setup() {
  Serial.begin(115200);

  pinMode(LASER_1_PIN, OUTPUT);
  pinMode(LASER_2_PIN, OUTPUT);
  pinMode(LASER_3_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LDR_PIN, INPUT);

  digitalWrite(LASER_1_PIN, HIGH);
  digitalWrite(LASER_2_PIN, HIGH);
  digitalWrite(LASER_3_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed! Restarting...");
    delay(1000);
    ESP.restart();
  }

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, centralS3MAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add Central ESP32-S3 peer");
    return;
  }

  Serial.println("=== Laser Tripwire Transmitter -> Central S3 Ready ===");
}

void loop() {
  int rawReading = digitalRead(LDR_PIN);

  if (rawReading != lastRawState) {
    lastDebounceTime = millis();
    lastRawState = rawReading;
  }

  if ((millis() - lastDebounceTime) >= DEBOUNCE_DELAY_MS) {
    if (rawReading != lastStableState) {
      lastStableState = rawReading;

      if (lastStableState == HIGH) {
        Serial.println("[ALERT] Beam Broken -> Sending 'L'");
        tone(BUZZER_PIN, 2000, 300);
        sendLaserEvent('L');
      } else {
        Serial.println("[OK] Beam Restored -> Sending 'C'");
        sendLaserEvent('C');
      }
    }
  }
}