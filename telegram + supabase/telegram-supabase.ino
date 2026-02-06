#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ===== WiFi =====
const char* ssid = "ET0731-IOTS";
const char* password = "iotset0731";

// ===== Supabase Edge Function =====
const char* functionUrl = "https://qjdlnjknaycjooxmkfsp.supabase.co/functions/v1/auth-door";
const char* deviceKey   = "secret123";

// If your Edge Function still has verify_jwt ON, you need a valid JWT here.
// If you disable verify_jwt, you can remove these two headers entirely.
const char* supabaseAnonKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFqZGxuamtuYXljam9veG1rZnNwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3Njk2Njk1MDksImV4cCI6MjA4NTI0NTUwOX0.7J5PGodycnykvlEK7eLYH4-ZM05WlznEKFdllopfZB0";

// ===== Telegram =====
const char* TG_BOT_TOKEN = "8360452283:AAEDPAUH9-i36U8ftEj6-1GaYQT4gmaNN-k";
const char* TG_CHAT_ID   = "6070646400"; // e.g. "123456789"

// ---- Helpers ----
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
  WiFiClientSecure client;
  client.setInsecure(); // simplest; for production, pin cert/root CA

  HTTPClient https;

  String url = String("https://api.telegram.org/bot") + TG_BOT_TOKEN +
               "/sendMessage?chat_id=" + TG_CHAT_ID +
               "&text=" + urlEncode(message);

  Serial.println("[TG] Sending...");
  // Serial.println(url); // uncomment if you want to see full URL (leaks token in serial)

  https.begin(client, url);
  int code = https.GET();

  Serial.print("[TG] HTTP Code: ");
  Serial.println(code);

  String resp = https.getString();
  Serial.println("[TG] Response:");
  Serial.println(resp);

  https.end();
  return (code == 200);
}

void connectWiFi() {
  Serial.println("\n[WiFi] Connecting...");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++tries > 40) {
      Serial.println("\n[WiFi] Failed!");
      return;
    }
  }
  Serial.println("\n[WiFi] Connected!");
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());
}

void callEdgeFunction() {
  Serial.println("\n========== CALLING EDGE FUNCTION ==========");

  // ✅ Put the UID in a variable so we can reuse it everywhere
  String rfidUid = "RFID002";

  HTTPClient http;
  http.begin(functionUrl);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", deviceKey);

  // If verify_jwt is ON, these are required
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseAnonKey);

  // ✅ Build payload using the variable UID
  String payload = String("{\"method\":\"rfid\",\"rfid_uid\":\"") + rfidUid + "\"}";

  Serial.println("[HTTP] Payload:");
  Serial.println(payload);

  int httpCode = http.POST(payload);

  Serial.print("[HTTP] Response Code: ");
  Serial.println(httpCode);

  // 1) HTTP / network errors
  if (httpCode <= 0) {
    String err = "[ALERT] Edge call failed. httpCode=" + String(httpCode) +
                 " err=" + http.errorToString(httpCode);
    Serial.println(err);
    sendTelegram(err);
    http.end();
    return;
  }

  String response = http.getString();
  Serial.println("[HTTP] Raw Response:");
  Serial.println(response);
  http.end();

  // 2) HTTP error codes (401/403/500 etc)
  if (httpCode >= 400) {
    String msg = "[ALERT] Edge HTTP error " + String(httpCode) + "\n" + response;
    sendTelegram(msg);
    return;
  }

  // 3) Parse JSON and alert on access granted/denied
  StaticJsonDocument<256> doc;
  DeserializationError e = deserializeJson(doc, response);
  if (e) {
    String msg = "[ALERT] JSON parse failed: " + String(e.c_str()) + "\n" + response;
    sendTelegram(msg);
    return;
  }

  bool access = doc["access"] | false;
  const char* method = doc["method"] | "unknown";

  if (access) {
    String msg = String("[GRANTED] Access granted ✅") +
                 "\nDevice=" + deviceKey +
                 "\nMethod=" + method +
                 "\nUID=" + rfidUid;
    Serial.println(msg);
    sendTelegram(msg);   // ✅ send telegram when granted
  } else {
    String msg = String("[DENIED] Access denied ❌") +
                 "\nDevice=" + deviceKey +
                 "\nMethod=" + method +
                 "\nUID=" + rfidUid;
    Serial.println(msg);
    sendTelegram(msg);   // ✅ send telegram when denied
  }
}


void setup() {
  Serial.begin(115200);
  delay(1000);
  connectWiFi();

  // Optional: test telegram first
  // sendTelegram("ESP32 online ✅");

  callEdgeFunction();
}

void loop() {}
