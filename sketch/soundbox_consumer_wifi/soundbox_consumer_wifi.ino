// Soundbox consumer: connects to Azure Event Grid MQTT with a client certificate,
// subscribes to soundbox/<DEVICE_ID>/payments and speaks the amount from SD card clips.
// It also keeps the last 3 transactions (filled from the retained message on
// soundbox/<DEVICE_ID>/history) so the repeat button can replay them:
//   short press = repeat last payment, long press = last 3 payments (newest first).
// Payments missed while the device was off are NOT announced on boot, only on button press.
//
// Board:     ESP32 (Arduino-ESP32 core 2.x or 3.x)
// Libraries: PubSubClient (Nick O'Leary), ArduinoJson (v7), ESP32-audioI2S (schreibfaul1)
// Run scripts/make-secrets.sh <deviceId> first to create secrets.h.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <Audio.h>
#include "secrets.h"

// ---- Edit for your network and PCB ---------------------------------------
const char WIFI_SSID[] = "your-wifi";
const char WIFI_PASS[] = "your-password";

#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23
#define I2S_BCLK 26
#define I2S_LRC  25
#define I2S_DOUT 22
#define VOLUME   18          // 0..21

#define BTN_PIN      4       // repeat button to GND (internal pull-up, active low)
#define LONG_PRESS_MS 800
#define HISTORY_SIZE 3

#define AUDIO_DIR "/audio"   // clips: received, rupees, notx, 0..19, 20,30..90, hundred, thousand, lakh
#define AUDIO_EXT ".wav"     // .mp3 also works with ESP32-audioI2S
// ---------------------------------------------------------------------------

WiFiClientSecure tls;
PubSubClient mqtt(tls);
Audio audio;

String topicPayments, topicStatus, topicHistory;

struct Txn { String id; long rupees; };
Txn history[HISTORY_SIZE];            // newest first
int historyCount = 0;

// ---- Clip queue: audio.connecttoFS is one file at a time, so we chain clips ----
static const int MAX_CLIPS = 48;      // 3 announcements x ~12 clips
String clips[MAX_CLIPS];
int clipHead = 0, clipTail = 0;

void queueClip(const String &name) {
  if (clipTail < MAX_CLIPS) clips[clipTail++] = String(AUDIO_DIR) + "/" + name + AUDIO_EXT;
}

void queueBelow100(int n) {                 // 1..99
  if (n < 20) { queueClip(String(n)); return; }
  queueClip(String((n / 10) * 10));
  if (n % 10) queueClip(String(n % 10));
}

void queueNumber(long n) {                  // Indian grouping, up to 99,99,999
  if (n >= 100000) { queueBelow100(n / 100000); queueClip("lakh"); n %= 100000; }
  if (n >= 1000)   { queueBelow100(n / 1000);   queueClip("thousand"); n %= 1000; }
  if (n >= 100)    { queueBelow100(n / 100);    queueClip("hundred"); n %= 100; }
  if (n > 0) queueBelow100(n);
}

void startAnnouncement() {                  // a new request interrupts any pending announcement
  clipHead = clipTail = 0;
  audio.stopSong();
}

void queuePayment(long rupees) {
  queueClip("received");
  if (rupees == 0) queueClip("0"); else queueNumber(rupees);
  queueClip("rupees");
}

void announceHistory(int count) {
  startAnnouncement();
  if (historyCount == 0) { queueClip("notx"); return; }
  for (int i = 0; i < count && i < historyCount; i++) queuePayment(history[i].rupees);
}

void addToHistory(const String &id, long rupees) {
  for (int i = 0; i < historyCount; i++)    // already known (e.g. live message then retained copy)
    if (id.length() && history[i].id == id) return;
  int n = historyCount < HISTORY_SIZE ? historyCount : HISTORY_SIZE - 1;
  for (int i = n; i > 0; i--) history[i] = history[i - 1];
  history[0] = {id, rupees};
  if (historyCount < HISTORY_SIZE) historyCount++;
}

void pumpAudio() {
  audio.loop();
  if (!audio.isRunning() && clipHead < clipTail) {
    const String &path = clips[clipHead++];
    if (!audio.connecttoFS(SD, path.c_str())) Serial.printf("missing clip %s\n", path.c_str());
  }
}

// ---- MQTT -----------------------------------------------------------------
void onHistory(byte *payload, unsigned int len) {
  // Retained message: {"txns":[{"txnId":"..","amount":150.0}, ...]} newest first. Silent, authoritative.
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) { Serial.println("bad history JSON"); return; }
  historyCount = 0;
  for (JsonObject t : doc["txns"].as<JsonArray>()) {
    if (historyCount >= HISTORY_SIZE) break;
    history[historyCount++] = {String(t["txnId"] | ""), (long)(t["amount"] | 0.0)};
  }
  Serial.printf("history synced: %d transaction(s)\n", historyCount);
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  if (topicHistory == topic) { onHistory(payload, len); return; }

  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) { Serial.println("bad JSON"); return; }

  String txn = doc["txnId"] | "";
  if (String(doc["status"] | "SUCCESS") != "SUCCESS") return;
  for (int i = 0; i < historyCount; i++)
    if (txn.length() && history[i].id == txn) return;  // QoS 1 duplicate delivery
  long rupees = (long)(doc["amount"] | 0.0);           // paise are not spoken
  Serial.printf("payment %s: %ld\n", txn.c_str(), rupees);

  addToHistory(txn, rupees);
  startAnnouncement();
  queuePayment(rupees);

  JsonDocument ack;
  ack["txnId"] = txn;
  ack["event"] = "played";
  String out;
  serializeJson(ack, out);
  mqtt.publish(topicStatus.c_str(), out.c_str());
}

// Short press = last payment, long press = last 3. Decided on release.
void pollButton() {
  static bool down = false;
  static uint32_t since = 0;
  bool pressed = digitalRead(BTN_PIN) == LOW;
  uint32_t now = millis();
  if (pressed && !down && now - since > 40) { down = true; since = now; }          // debounce
  else if (!pressed && down && now - since > 40) {
    down = false;
    announceHistory(now - since >= LONG_PRESS_MS ? HISTORY_SIZE : 1);
    since = now;
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print('.'); }
  Serial.printf("\nWiFi %s\n", WiFi.localIP().toString().c_str());
}

void syncClock() {                                      // TLS cert validation needs real time
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  time_t now = 0;
  while ((now = time(nullptr)) < 1700000000) { delay(300); }
}

void connectMqtt() {
  while (!mqtt.connected()) {
    connectWifi();
    Serial.print("MQTT connect... ");
    // Event Grid: username must equal the client's authenticationName; no password (cert auth).
    if (mqtt.connect(DEVICE_ID, DEVICE_ID, nullptr)) {
      Serial.println("ok");
      mqtt.subscribe(topicPayments.c_str(), 1);
      mqtt.subscribe(topicHistory.c_str(), 1);     // delivers the retained history right away
    } else {
      Serial.printf("failed rc=%d\n", mqtt.state());
      for (int i = 0; i < 50; i++) { pumpAudio(); delay(100); }   // retry in 5 s, keep audio alive
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BTN_PIN, INPUT_PULLUP);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) Serial.println("SD mount failed");
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(VOLUME);

  connectWifi();
  syncClock();

  tls.setCACert(ROOT_CA);
  tls.setCertificate(DEVICE_CERT);
  tls.setPrivateKey(DEVICE_KEY);

  topicPayments = String("soundbox/") + DEVICE_ID + "/payments";
  topicStatus   = String("soundbox/") + DEVICE_ID + "/status";
  topicHistory  = String("soundbox/") + DEVICE_ID + "/history";
  mqtt.setServer(MQTT_HOST, 8883);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMessage);
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();
  pollButton();
  pumpAudio();
}
