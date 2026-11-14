#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HardwareSerial.h>

// --- Pin Definitions for 4G Module ---
#define MODEM_RX    16
#define MODEM_TX    17
#define MODEM_BAUD  115200

// --- Configuration ---
const char* APN = "hutch3g";
// Using your verified Webhook.site endpoint
const char* SERVER_URL = "https://webhook.site/6db5d1b5-f42a-4bbd-b01b-05e7fabd8e3e"; 

// --- Unified ESP-NOW Data Payload (Matches Door Lock & Laser Nodes) ---
typedef struct struct_message {
  char eventCode; // 'G', 'D', 'M' (Door Lock) | 'L', 'C' (Laser ESP32)
} struct_message;

struct_message incomingData;

// --- State Variables for Asynchronous 4G Alerting ---
volatile bool alertPending = false;
char pendingEventCode = '\0';
char pendingSenderMac[18] = "";

// ---------- ESP-NOW Receive Callback (Arduino Core v3.x) ----------
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len != sizeof(incomingData)) {
    Serial.print("[WARN] Unexpected packet size: ");
    Serial.println(len);
    return;
  }

  memcpy(&incomingData, data, sizeof(incomingData));

  // Format sender MAC address string
  snprintf(pendingSenderMac, sizeof(pendingSenderMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
           recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);

  pendingEventCode = incomingData.eventCode;
  alertPending = true; // Safely flag main loop to send 4G HTTP alert
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize 4G Modem Serial (Serial2 on ESP32-S3)
  Serial2.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX, MODEM_TX);
  Serial.println("Waiting for modem to boot...");
  delay(3000);

  // Initialize Wi-Fi in Station Mode with brownout-safe TX power
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  Serial.print("Central Gateway MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW initialization failed! Restarting...");
    delay(1000);
    ESP.restart();
  }

  // Register receive callback
  esp_now_register_recv_cb(OnDataRecv);
  
  // Initialize 4G GPRS Bearer
  initModemBearer();
  
  Serial.println("=== Central ESP32-S3 Security Gateway Ready & Armed ===");
}

void loop() {
  // Process pending security events outside the ISR/callback to protect Wi-Fi stack
  if (alertPending) {
    alertPending = false; // Clear flag

    Serial.printf("\n[EVENT] Processing alert code '%c' from node %s\n", pendingEventCode, pendingSenderMac);
    
    // Map event code to descriptive text
    String eventDescription = "UNKNOWN";
    if (pendingEventCode == 'G') eventDescription = "Access Granted";
    else if (pendingEventCode == 'D') eventDescription = "Access Denied";
    else if (pendingEventCode == 'M') eventDescription = "Motion Detected";
    else if (pendingEventCode == 'L') eventDescription = "Laser Tripwire Broken";
    else if (pendingEventCode == 'C') eventDescription = "Laser Beam Restored";

    // Build JSON Payload
    String jsonPayload = "{\"event_code\":\"" + String(pendingEventCode) + 
                         "\",\"description\":\"" + eventDescription + 
                         "\",\"node_mac\":\"" + String(pendingSenderMac) + 
                         "\",\"timestamp\":" + String(millis()) + "}";

    // Transmit via 4G HTTP POST
    sendHTTPPost(jsonPayload);
  }
}

// ---------- 4G Modem Helper Functions ----------

void sendCmd(const char* cmd) {
  Serial.print(">> ");
  Serial.println(cmd);
  Serial2.print(cmd);
  Serial2.print("\r\n");
}

bool expect(const char* expected, unsigned long timeoutMs) {
  unsigned long start = millis();
  String buf;
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      Serial.write(c);
      buf += c;
      if (buf.indexOf(expected) >= 0) return true;
      if (buf.length() > 1024) buf.remove(0, buf.length() - 256);
    }
    delay(5);
  }
  return false;
}

void drain(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    while (Serial2.available()) Serial.write(Serial2.read());
    delay(5);
  }
}

void initModemBearer() {
  Serial.println("=== Initializing 4G GPRS Bearer ===");
  sendCmd("AT");
  expect("OK", 2000);

  sendCmd("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
  expect("OK", 2000);

  String apnCmd = String("AT+SAPBR=3,1,\"APN\",\"") + APN + "\"";
  sendCmd(apnCmd.c_str());
  expect("OK", 2000);

  sendCmd("AT+SAPBR=1,1");
  if (!expect("OK", 15000)) Serial.println("[WARN] Bearer open slow or failed");

  sendCmd("AT+SAPBR=2,1");
  expect("OK", 2000);
  drain(1000);
}

void sendHTTPPost(String payload) {
  Serial.println("[HTTP] Starting HTTP session...");
  
  sendCmd("AT+HTTPINIT");
  if (!expect("OK", 3000)) {
    sendCmd("AT+HTTPTERM");
    expect("OK", 2000);
    sendCmd("AT+HTTPINIT");
    expect("OK", 3000);
  }

  sendCmd("AT+HTTPPARA=\"CID\",1");
  expect("OK", 3000);

  String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + SERVER_URL + "\"";
  sendCmd(urlCmd.c_str());
  expect("OK", 3000);

  sendCmd("AT+HTTPPARA=\"CONTENT\",\"application/json\"");
  expect("OK", 3000);

  int len = payload.length();
  char dataCmd[64];
  snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%d,30000", len);
  sendCmd(dataCmd);

  if (expect("DOWNLOAD", 5000)) {
    Serial2.print(payload); // Send exact JSON payload bytes
    
    if (expect("OK", 10000)) {
      Serial.println("[HTTP] Data accepted. Executing POST...");
      sendCmd("AT+HTTPACTION=1"); // 1 = POST

      if (expect("+HTTPACTION:", 30000)) {
        drain(1000);
        sendCmd("AT+HTTPREAD");
        drain(3000);
      } else {
        Serial.println("[ERROR] HTTPACTION timeout.");
      }
    } else {
      Serial.println("[ERROR] Modem rejected JSON data.");
    }
  } else {
    Serial.println("[ERROR] Never received DOWNLOAD prompt.");
  }

  sendCmd("AT+HTTPTERM");
  expect("OK", 2000);
  Serial.println("[HTTP] Session closed. Gateway listening for ESP-NOW events...\n");
}