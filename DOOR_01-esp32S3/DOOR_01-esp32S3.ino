#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <esp_now.h>
#include <WiFi.h>

// --- Pin Definitions ---
// RFID Pins (SPI Bus)
#define RST_PIN         3
#define SS_PIN          13 
#define SCK_PIN         12
#define MOSI_PIN        11
#define MISO_PIN        10

// Fingerprint UART Pins (HardwareSerial 1)
#define FP_RX_PIN       7 
#define FP_TX_PIN       6 

// RGB LED Pins
#define RED_PIN         41
#define GREEN_PIN       42
#define BLUE_PIN        40

// Sensor & Control Pins
#define DOOR_SENSOR_PIN 21
#define PIR_SENSOR_PIN  17 
#define RELAY_PIN       18  // 5V Relay Signal Pin driving 12V Solenoid
#define BUZZER_PIN      20  // Local Buzzer Pin

// --- Relay Logic Definitions ---
#define RELAY_ON        LOW   // Active signal (unlock)
#define RELAY_OFF       HIGH  // Inactive signal (lock)

// --- TARGET ESP32 MAC ADDRESSES ---
uint8_t secondaryReceiverMAC[] = {0x68, 0x09, 0x47, 0x53, 0xBF, 0xD8};
uint8_t centralS3MAC[]         = {0x68, 0xEE, 0x8F, 0x4C, 0x38, 0x94};

// Unified ESP-NOW Data Payload (shared across Door Lock & Laser ESP32)
typedef struct struct_message {
  char eventCode; // 'G' = Granted, 'D' = Denied, 'M' = Motion, 'L' = Laser Trip, 'C' = Laser Clear
} struct_message;

struct_message eventData;

// --- PRESET AUTHORIZED RFID CARD ---
byte authorizedUID[4] = {0x5B, 0x71, 0x2D, 0x07};

// --- State Variables ---
bool lastMotionState = LOW;

// --- Class Instances ---
MFRC522 mfrc522(SS_PIN, RST_PIN);
HardwareSerial fpSerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);

void setLED(bool r, bool g, bool b) {
  digitalWrite(RED_PIN, r);
  digitalWrite(GREEN_PIN, g);
  digitalWrite(BLUE_PIN, b);
}

bool registerPeer(const uint8_t *macAddr) {
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, macAddr, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  return (esp_now_add_peer(&peerInfo) == ESP_OK);
}

// Wireless Event Transmitter via ESP-NOW to BOTH receivers
void sendBuzzerEvent(char eventCode) {
  eventData.eventCode = eventCode;

  esp_err_t res1 = esp_now_send(secondaryReceiverMAC, (uint8_t *) &eventData, sizeof(eventData));
  esp_err_t res2 = esp_now_send(centralS3MAC, (uint8_t *) &eventData, sizeof(eventData));

  Serial.print("[ESP-NOW] Event '");
  Serial.print(eventCode);
  Serial.print("' -> Secondary: ");
  Serial.print(res1 == ESP_OK ? "OK" : "ERR");
  Serial.print(" | Central S3: ");
  Serial.println(res2 == ESP_OK ? "OK" : "ERR");
}

void grantAccess(String method) {
  Serial.println("Access Granted via " + method + "!");
  
  setLED(LOW, HIGH, LOW); 
  digitalWrite(RELAY_PIN, RELAY_ON);  
  sendBuzzerEvent('G');               
  
  digitalWrite(BUZZER_PIN, HIGH);
  delay(500);
  digitalWrite(BUZZER_PIN, LOW);
  
  delay(2500); // Remaining delay before locking (3000ms total)                     
  
  digitalWrite(RELAY_PIN, RELAY_OFF); 
  Serial.println("Locking...");
}

void denyAccess(String method) {
  Serial.println("Access Denied via " + method + "!");
  sendBuzzerEvent('D'); 
  
  for (int i = 0; i < 3; i++) {
    setLED(HIGH, LOW, LOW);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    setLED(LOW, LOW, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

bool connectSensor(uint32_t baud) {
  fpSerial.begin(baud, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN); 
  delay(100);
  return finger.verifyPassword();
}

int checkFingerprint() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return 0;

  return finger.fingerID;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // 1. Set initial Relay state before enabling output mode
  digitalWrite(RELAY_PIN, RELAY_OFF); 
  pinMode(RELAY_PIN, OUTPUT);

  // 1b. Initialize Local Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 

  // 2. Initialize Wi-Fi in Station Mode for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // 3. Register BOTH Peer ESP32 Boards
  if (registerPeer(secondaryReceiverMAC)) {
    Serial.println("[ESP-NOW] Secondary Receiver (68:09:47:53:BF:D8) added.");
  } else {
    Serial.println("[ESP-NOW] Failed to add Secondary Receiver.");
  }

  if (registerPeer(centralS3MAC)) {
    Serial.println("[ESP-NOW] Central ESP32-S3 (68:EE:8F:4C:38:94) added.");
  } else {
    Serial.println("[ESP-NOW] Failed to add Central ESP32-S3.");
  }

  // 4. Configure GPIO Pins
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
  pinMode(PIR_SENSOR_PIN, INPUT);

  // 5. Initialize MFRC522 RFID Reader
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  
  // 6. Initialize Optical Fingerprint Sensor
  Serial.println("Initializing Fingerprint Sensor...");
  bool fpFound = connectSensor(57600);
  if (!fpFound) {
    fpSerial.end();
    fpFound = connectSensor(9600);
  }

  if (fpFound) {
    Serial.println("-> Fingerprint Sensor Connected Successfully!");
  } else {
    Serial.println("-> ERROR: Fingerprint Sensor not found! Check RX/TX wiring.");
  }

  setLED(HIGH, LOW, LOW);
  Serial.println("ESP32-S3 Door Lock Ready.");
}

void loop() {
  // 1. Motion Detection Processing
  bool currentMotionState = digitalRead(PIR_SENSOR_PIN);
  if (currentMotionState == HIGH && lastMotionState == LOW) {
    Serial.println("Motion detected!");
    sendBuzzerEvent('M'); 
    
    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);
    
    lastMotionState = HIGH;
  } else if (currentMotionState == LOW && lastMotionState == HIGH) {
    lastMotionState = LOW;
  }

  // 2. Status LED State Machine
  if (digitalRead(DOOR_SENSOR_PIN) == HIGH) { 
    setLED(LOW, LOW, HIGH);   // Blue: Door open
  } else if (currentMotionState == HIGH) {
    setLED(HIGH, LOW, HIGH);  // Magenta: Motion detected
  } else {
    setLED(HIGH, LOW, LOW);   // Red: Standby / Locked
  }

  // 3. Fingerprint Sensor Processing
  int fingerResult = checkFingerprint();
  if (fingerResult > 0) {
    grantAccess("Fingerprint ID: " + String(fingerResult));
    return;
  } else if (fingerResult == 0) {
    denyAccess("Fingerprint");
    return;
  }

  // 4. RFID Card Processing
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    bool isAuthorized = true;
    for (byte i = 0; i < 4; i++) {
      if (mfrc522.uid.uidByte[i] != authorizedUID[i]) {
        isAuthorized = false;
        break;
      }
    }

    if (isAuthorized) {
      grantAccess("RFID Card");
    } else {
      denyAccess("RFID Card");
    }
    
    mfrc522.PICC_HaltA();
  }
}