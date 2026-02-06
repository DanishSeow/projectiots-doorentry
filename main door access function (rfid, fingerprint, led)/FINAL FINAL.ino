#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// ================== CONFIG ==================
// WiFi
const char* ssid = "PX";
const char* password = "FreeWifi";

// Supabase Edge Function
const char* edgeFunctionUrl  = "https://qjdlnjknaycjooxmkfsp.supabase.co/functions/v1/auth-door";
const char* supabaseAnonKey  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFqZGxuamtuYXljam9veG1rZnNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njk2Njk1MDksImV4cCI6MjA4NTI0NTUwOX0.7J5PGodycnykvlEK7eLYH4-ZM05WlznEKFdllopfZB0";   // needed if verify_jwt is ON
const char* deviceKey        = "hospitalkey";               // devices.secret

// Telegram
const char* TG_BOT_TOKEN = "8360452283:AAEDPAUH9-i36U8ftEj6-1GaYQT4gmaNN-k";
const char* TG_CHAT_ID   = "6070646400";

// Hardware pins
#define LED_DENIED  4
#define LED_GRANTED 21
#define SERVO_PIN   18

// RFID pins
#define SS_PIN   16
#define RST_PIN  2
#define SCK_PIN  23
#define MISO_PIN 22
#define MOSI_PIN 5

// Fingerprint UART2 pins
#define FP_RX 17
#define FP_TX 19

// ================== GLOBALS ==================
Servo doorServo;

MFRC522 myRFID(SS_PIN, RST_PIN);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

// offline whitelist (fingerprints)
int trustedFingerprints[] = {};
int fpCount = 1;

// enroll control
bool enrollMode = false;
int enrollID = 0;

// ================== TELEGRAM HELPERS ==================
String urlEncode(const String &s) {
  String out;
  char c;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      out += hex;
    }
  }
  return out;
}

bool sendTelegram(const String& message) {
  // Avoid crashing if WiFi not connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TG] WiFi not connected, skip Telegram.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = String("https://api.telegram.org/bot") + TG_BOT_TOKEN +
               "/sendMessage?chat_id=" + TG_CHAT_ID +
               "&text=" + urlEncode(message);

  Serial.println("[TG] Sending...");
  int code = -999;
  if (https.begin(client, url)) {
    code = https.GET();
    Serial.print("[TG] HTTP Code: ");
    Serial.println(code);
    Serial.println("[TG] Response:");
    Serial.println(https.getString());
    https.end();
  } else {
    Serial.println("[TG] begin() failed");
  }
  return (code == 200);
}

// ================== MENU / SERIAL ==================
void printMenu() {
  Serial.println("\n=== DOOR SYSTEM ===");
  Serial.println("scan | enroll X | delete | status | test");
}

void handleSerialCommands(); // forward decl
void deleteFingerprint();
void enrollFingerprint();

// ================== LED / SERVO ==================
void blinkDenied(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_DENIED, HIGH);
    delay(200);
    digitalWrite(LED_DENIED, LOW);
    delay(200);
  }
}

void blinkGranted(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_GRANTED, HIGH);
    delay(200);
    digitalWrite(LED_GRANTED, LOW);
    delay(200);
  }
}

// simple open-close
void triggerDoor() {
  Serial.println("🚪 DOOR UNLOCK!");
  doorServo.write(180);
  delay(800);
  doorServo.write(0);
  delay(800);
  Serial.println("🔒 DOOR LOCK!");
}

// ================== WIFI ==================
void connectWiFi() {
  Serial.println("\n[WiFi] Connecting...");
  WiFi.begin(ssid, password);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++tries > 40) {
      Serial.println("\n[WiFi] FAILED");
      return;
    }
  }
  Serial.println("\n[WiFi] Connected!");
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());
}

// ================== OFFLINE AUTH ==================
void offlineAuth(const String& method, const String& value) {
  Serial.println("🔌 OFFLINE AUTH MODE");

  if (method == "fingerprint") {
    int fid = value.toInt();
    for (int i = 0; i < fpCount; i++) {
      if (fid == trustedFingerprints[i]) {
        Serial.println("✅ OFFLINE FP GRANTED: " + value);
        blinkGranted(4);
        triggerDoor();
        sendTelegram("[OFFLINE GRANTED ✅]\nDevice=" + String(deviceKey) +
                     "\nMethod=fingerprint\nID=" + value);
        return;
      }
    }
  }

  Serial.println("❌ OFFLINE DENIED");
  blinkDenied(4);
  sendTelegram("[OFFLINE DENIED ❌]\nDevice=" + String(deviceKey) +
               "\nMethod=" + method + "\nValue=" + value);
}

// ================== SUPABASE EDGE CALL ==================
void callAuthDoor(const String& method, const String& value) {
  Serial.println("\n========== AUTH REQUEST ==========");
  Serial.println("Method: " + method + "  Value: " + value);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, edgeFunctionUrl)) {
    Serial.println("[HTTP] begin() failed");
    sendTelegram("[ERROR] HTTP begin() failed\nDevice=" + String(deviceKey));
    offlineAuth(method, value);
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", deviceKey);

  // If verify_jwt is ON:
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseAnonKey);

  String payload;
  if (method == "rfid") {
    payload = String("{\"method\":\"rfid\",\"rfid_uid\":\"") + value + "\"}";
  } else if (method == "fingerprint") {
    payload = String("{\"method\":\"fingerprint\",\"fingerprint_id\":") + value + "}";
  } else {
    payload = String("{\"method\":\"") + method + "\",\"value\":\"" + value + "\"}";
  }

  Serial.println("[HTTP] Payload:");
  Serial.println(payload);

  int httpCode = http.POST(payload);
  Serial.print("[HTTP] Code: ");
  Serial.println(httpCode);

  // Network / TLS / connection error
  if (httpCode <= 0) {
    String msg = String("[ERROR] Edge call failed\nCode=") + httpCode +
                 "\nErr=" + http.errorToString(httpCode) +
                 "\nDevice=" + deviceKey +
                 "\nMethod=" + method +
                 "\nValue=" + value;
    Serial.println(msg);
    sendTelegram(msg);
    http.end();
    offlineAuth(method, value);
    return;
  }

  String response = http.getString();
  http.end();

  Serial.println("[HTTP] Response:");
  Serial.println(response);

  // HTTP error code from server (401/403/500 etc)
  if (httpCode >= 400) {
    String msg = String("[ERROR] Edge HTTP ") + httpCode +
                 "\n" + response +
                 "\nDevice=" + deviceKey +
                 "\nMethod=" + method +
                 "\nValue=" + value;
    sendTelegram(msg);
    offlineAuth(method, value);
    return;
  }

  // Parse JSON
  StaticJsonDocument<256> doc;
  DeserializationError e = deserializeJson(doc, response);
  if (e) {
    String msg = String("[ERROR] JSON parse failed: ") + e.c_str() +
                 "\nResp=" + response;
    Serial.println(msg);
    sendTelegram(msg);
    offlineAuth(method, value);
    return;
  }

  bool access = doc["access"] | false;
  const char* m = doc["method"] | "unknown";

  if (access) {
    Serial.println("✅ SUPABASE GRANTED");
    blinkGranted(4);
    triggerDoor();

    String msg = String("[GRANTED ✅]\nDevice=") + deviceKey +
                 "\nMethod=" + m +
                 "\nValue=" + value;
    sendTelegram(msg);
  } else {
    Serial.println("❌ SUPABASE DENIED");
    blinkDenied(4);

    String msg = String("[DENIED ❌]\nDevice=") + deviceKey +
                 "\nMethod=" + m +
                 "\nValue=" + value;
    sendTelegram(msg);

    // fallback offline if you want
    offlineAuth(method, value);
  }
}

// ================== FINGERPRINT ==================
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;

  return finger.fingerID;
}

void enrollFingerprint() {
  Serial.println("✋ Enroll #" + String(enrollID));
  int p = -1;

  Serial.println("1) Place finger...");
  while (p != FINGERPRINT_OK) p = finger.getImage();

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("❌ image2Tz(1) failed");
    sendTelegram("[FP ENROLL ❌]\nStep=image2Tz(1)\nID=" + String(enrollID));
    enrollMode = false;
    return;
  }

  Serial.println("Remove finger...");
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);

  Serial.println("2) Place same finger again...");
  p = -1;
  while (p != FINGERPRINT_OK) p = finger.getImage();

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("❌ image2Tz(2) failed");
    sendTelegram("[FP ENROLL ❌]\nStep=image2Tz(2)\nID=" + String(enrollID));
    enrollMode = false;
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("❌ createModel failed");
    sendTelegram("[FP ENROLL ❌]\nStep=createModel\nID=" + String(enrollID));
    enrollMode = false;
    return;
  }

  p = finger.storeModel(enrollID);
  if (p == FINGERPRINT_OK) {
    Serial.println("✅ Enroll success! Stored ID=" + String(enrollID));

    // Add to offline whitelist (max 10)
    if (fpCount < 10) {
      trustedFingerprints[fpCount++] = enrollID;
      Serial.println("➕ Added to offline whitelist");
    }

    sendTelegram("[FP ENROLL ✅]\nDevice=" + String(deviceKey) +
                 "\nID=" + String(enrollID));
  } else {
    Serial.println("❌ storeModel failed: " + String(p));
    sendTelegram("[FP ENROLL ❌]\nStep=storeModel\nID=" + String(enrollID) +
                 "\nErr=" + String(p));
  }

  enrollMode = false;
}

void deleteFingerprint() {
  Serial.println("🗑️ Delete mode");
  Serial.println("Enter ID to delete (1-127) or 0 to cancel:");

  while (Serial.available() == 0) delay(50);
  int id = Serial.parseInt();

  if (id == 0) {
    Serial.println("Cancelled");
    return;
  }
  if (id < 1 || id > 127) {
    Serial.println("❌ ID must be 1-127");
    return;
  }

  Serial.print("Deleting FP #"); Serial.println(id);

  int p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("✅ FP deleted from sensor!");

    // remove from offline whitelist
    for (int i = 0; i < fpCount; i++) {
      if (trustedFingerprints[i] == id) {
        trustedFingerprints[i] = trustedFingerprints[fpCount - 1];
        fpCount--;
        Serial.println("➖ Removed from offline whitelist");
        break;
      }
    }

    sendTelegram("[FP DELETE ✅]\nDevice=" + String(deviceKey) +
                 "\nID=" + String(id));
  } else {
    Serial.println("❌ Delete failed: " + String(p));
    sendTelegram("[FP DELETE ❌]\nDevice=" + String(deviceKey) +
                 "\nID=" + String(id) +
                 "\nErr=" + String(p));
  }
}

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "scan") {
    enrollMode = false;
    Serial.println("👁️ SCAN MODE");
  }
  else if (cmd.startsWith("enroll ")) {
    enrollID = cmd.substring(7).toInt();
    if (enrollID >= 1 && enrollID <= 127) {
      enrollMode = true;
      Serial.println("✋ ENROLL MODE for ID=" + String(enrollID));
    } else {
      Serial.println("❌ ID must be 1-127");
    }
  }
  else if (cmd == "delete") {
    deleteFingerprint();
  }
  else if (cmd == "status") {
    finger.getTemplateCount();
    Serial.print("Templates stored: ");
    Serial.println(finger.templateCount);
  }
  else if (cmd == "test") {
    callAuthDoor("fingerprint", "1");
  }

  printMenu();
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_DENIED, OUTPUT);
  pinMode(LED_GRANTED, OUTPUT);
  digitalWrite(LED_DENIED, LOW);
  digitalWrite(LED_GRANTED, LOW);

  // RFID SPI init
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  pinMode(SS_PIN, OUTPUT);
  digitalWrite(SS_PIN, HIGH);
  myRFID.PCD_Init();
  Serial.println("✓ RFID Initialized");

  // Servo init
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  Serial.println("✓ Servo Initialized");

  // WiFi
  connectWiFi();

  // Fingerprint init
  fingerSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint Initialized");
  } else {
    Serial.println("✗ Fingerprint NOT found");
  }

  sendTelegram("ESP32 Door online ✅\nDevice=" + String(deviceKey));
  Serial.println("✅ READY! Scan RFID / Fingerprint");
  printMenu();
}

void loop() {
  handleSerialCommands();

  // Enroll flow
  if (enrollMode) {
    enrollFingerprint();
    delay(200);
    return;
  }

  // Fingerprint scan
  int fid = getFingerprintID();
  if (fid > 0) {
    Serial.print("👆 FP ID: ");
    Serial.println(fid);
    callAuthDoor("fingerprint", String(fid));
    delay(2000);
    return;
  }

  // RFID scan
  if (myRFID.PICC_IsNewCardPresent() && myRFID.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < myRFID.uid.size; i++) {
      if (myRFID.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(myRFID.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    Serial.println("🏷️ RFID UID: " + uid);
    callAuthDoor("rfid", uid);

    myRFID.PICC_HaltA();
    myRFID.PCD_StopCrypto1();

    delay(1200);
  }

  delay(50);
}
