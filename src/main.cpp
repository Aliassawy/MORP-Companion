#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "display.h"

// ---------- Settings ----------
const char* ssid = "omar Assawy_EXT";
const char* password = "aly@omar@yahya2013";
const char* websocket_server = "192.168.100.81";
const int websocket_port = 8000;

const int IR_SENSOR_1 = 35;
const int IR_SENSOR_2 = 34;
const int STATUS_LED  = 33;
const int LDR_PIN     = 32;                 // ADC1 pin, works with Wi-Fi on
const unsigned long LIGHT_SEND_MS = 5000;

WebSocketsClient webSocket;
unsigned long ledOffAt = 0;
unsigned long nextLightSend = 0;
bool lastIr1State = HIGH;
bool lastIr2State = HIGH;

// ---------- Light sensor ----------
int readLight() {
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(LDR_PIN);  // average to smooth noise
  return (int)(sum / 16);
}

// ---------- WebSocket ----------
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from Laptop Brain!");
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] Connected to Laptop Brain!");
      webSocket.sendTXT("{\"type\": \"handshake\", \"device\": \"MORP_SINGLE_HEADER\"}");
      nextLightSend = 0;  // send a light reading right away
      break;
    case WStype_TEXT: {
      Serial.printf("[WS Message From Laptop]: %s\n", payload);
      JsonDocument doc;
      if (deserializeJson(doc, (const char*)payload, length)) break;
      const char* t = doc["type"] | "";
      Mood m = displayParseMood(doc["mood"] | "normal");
      unsigned long ms = (unsigned long)(doc["seconds"] | 4) * 1000;

      if (!strcmp(t, "face"))              displayMood(m, ms);
      else if (!strcmp(t, "say"))          displaySay(m, doc["text"] | "");
      else if (!strcmp(t, "text"))         displayText(doc["text"] | "", ms);
      else if (!strcmp(t, "weather"))      displayWeather(5000);
      else if (!strcmp(t, "clock"))        displayClock(ms);
      else if (!strcmp(t, "sleep_after"))  displaySetSleepAfter(ms);
      break;
    }
    default:
      break;
  }
}

// ---------- Sensors ----------
void trigger(const char* sensor, Mood m, unsigned long ms) {
  displayMood(m, ms);
  digitalWrite(STATUS_LED, HIGH);
  ledOffAt = millis() + 150;
  String msg = String("{\"type\": \"sensor_trigger\", \"sensor\": \"") + sensor + "\"}";
  webSocket.sendTXT(msg);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(IR_SENSOR_1, INPUT);
  pinMode(IR_SENSOR_2, INPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(LDR_PIN, INPUT);

  displayBegin();
  displayMessage("Starting...");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  displayMessage("WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected!");
  Serial.println(WiFi.localIP());

  // Clock: Cairo time (EET, with Egypt's daylight saving), synced from the internet
  configTzTime("EET-2EEST,M4.5.5/0,M10.5.4/24", "pool.ntp.org", "time.google.com");

  displayMessage("Connected!");
  delay(800);

  ArduinoOTA.setHostname("morp-esp32");
  ArduinoOTA.begin();

  webSocket.begin(websocket_server, websocket_port, "/ws");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  ArduinoOTA.handle();
  webSocket.loop();

  bool ir1State = digitalRead(IR_SENSOR_1);
  bool ir2State = digitalRead(IR_SENSOR_2);
  if (ir1State == LOW && lastIr1State == HIGH) trigger("IR1_Poke", MOOD_SURPRISED, 2000);
  if (ir2State == LOW && lastIr2State == HIGH) trigger("IR2_Pet", MOOD_HAPPY, 3000);
  lastIr1State = ir1State;
  lastIr2State = ir2State;

  if (ledOffAt && millis() >= ledOffAt) {
    digitalWrite(STATUS_LED, LOW);
    ledOffAt = 0;
  }

  // Send the room's light level to the server every 5 seconds
  if (webSocket.isConnected() && millis() >= nextLightSend) {
    nextLightSend = millis() + LIGHT_SEND_MS;
    String msg = String("{\"type\": \"light\", \"raw\": ") + readLight() + "}";
    webSocket.sendTXT(msg);
  }

  displayUpdate();
}