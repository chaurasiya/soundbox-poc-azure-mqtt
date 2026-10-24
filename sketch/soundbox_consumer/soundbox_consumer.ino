// =====================================================================
// RUPEECO SOUNDBOX CONSUMER  -  ESP32-S3 + A7670C 4G + Azure Event Grid MQTT
//
// Hardware: SH1106 OLED (I2C), micro SD (SPI), MAX98357A (I2S), A7670C (UART1), 3 touch buttons.
// Libraries: U8g2, ArduinoJson (v7), ESP_I2S / Preferences / SD (bundled with Arduino-ESP32 3.x).
//
// IDENTITY / PROVISIONING (one generic firmware for every unit):
//   * The device id is the modem IMEI, read at runtime with AT+CGSN. It is the MQTT client id and
//     username and appears in every topic.
//   * Per-device secrets (broker host/port, root CA, client cert + key, expected IMEI) are NOT compiled
//     in. scripts/onboard_device.py writes them into the NVS partition ("prov" namespace) of that unit.
//   * If the IMEI read from the modem differs from the provisioned one (cert flashed onto the wrong
//     unit), the device refuses to connect to Azure.
//
// Topics (<id> = modem IMEI):
//   soundbox/<id>/payments  live payment, announced immediately
//   soundbox/<id>/history   retained list of the last 3 payments, synced silently
//   soundbox/<id>/status    acks / heartbeat published by the device
//
// Transaction history (last 3), kept in flash and refreshed from the retained message:
//   Home screen, OK long-press (>= 800 ms)  -> replay last transaction
//   Home screen, OK double-press            -> speak last 3 transactions
//   Home screen, OK single press            -> menu (also has "Replay Last Txn" / "Last 3 Txns")
// Payments missed while the device was off are never announced on boot; they are only replayed on request.
//
// Design notes (why this differs from a naive AT-command sketch):
//  * ONE reader for all modem bytes (pumpModem). AT replies and MQTT pushes (+CMQTTRX*) are parsed by
//    length, so a payment that arrives mid-command or mid-announcement is queued, never dropped.
//  * Payments are parsed with ArduinoJson, not string searching.
//  * Auto-reconnect with back-off, periodic signal/clock refresh, and certs are uploaded to the modem
//    only when they change.
// =====================================================================
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <ArduinoJson.h>

// ---- Tunables -------------------------------------------------------
#define HISTORY_SIZE      3
#define LONG_PRESS_MS     800
#define DOUBLE_GAP_MS     350
#define RX_QUEUE_SIZE     8
#define MAX_AMOUNT        999999999L     // 99 crore; larger values are rejected, not mis-spoken
#define MODEM_DEBUG       1              // echo modem lines to the Serial Monitor

// =====================================================
// 1. SH1106 128x64 OLED DISPLAY (SCL=9, SDA=8)
// =====================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 9, /* data=*/ 8);

// =====================================================
// 2. MICRO SD CARD PINS (ESP32-S3 FSPI)
// =====================================================
#define SD_CS_PIN   10
#define SD_MOSI_PIN 11
#define SD_SCK_PIN  12
#define SD_MISO_PIN 13
SPIClass sdSPI(FSPI);
bool sdReady = false;

// =====================================================
// 3. MAX98357A I2S AUDIO PINS
// =====================================================
#define I2S_BCLK    15
#define I2S_LRC     16
#define I2S_DIN     17
I2SClass i2s;

// =====================================================
// 4. SIM A7670C 4G MODULE PINS (UART1)
// =====================================================
#define MODEM_RX    18  // Direct to A7670C TXD
#define MODEM_TX    7   // Via Voltage Divider (10k+10k) to A7670C RXD
#define MODEM_RST   6   // Optional Reset Pin
#define MODEM       Serial1

// =====================================================
// 5. THREE TOUCH BUTTONS (UP / OK / DOWN), active HIGH
// =====================================================
#define BTN_UP      1   // Scroll UP / Volume +
#define BTN_OK      2   // Open Menu / Select (OK)
#define BTN_DOWN    42  // Scroll DOWN / Volume -

// =====================================================
// 6. REAL ISO QR CODE (29x29) FOR: "upi://pay?pa=paytm.s3gpikd@pty"
// =====================================================
const uint32_t UPI_QR_29x29[29] = {
  0x1FC9927F, 0x10477B41, 0x175DD65D, 0x174EE65D, 0x17444E5D,
  0x10488B41, 0x1FD5557F, 0x001DDC00, 0x1DF668C4, 0x1AB6606D,
  0x10588CB1, 0x0B922C28, 0x0B5118C0, 0x1A2BB947, 0x0ADB715B,
  0x1921D9AA, 0x064E63E1, 0x08B668C5, 0x1148846B, 0x0E9A2B10,
  0x10F11BFB, 0x0017BF19, 0x1FD77757, 0x1051D913, 0x175279F2,
  0x17466E18, 0x1754839D, 0x105A29C2, 0x1FD52BCB
};

Preferences prefs;

// ---- Identity: provisioned secrets (NVS "prov") + IMEI read from the modem ----
String provHost, provCa, provCert, provKey, provImei;
uint16_t provPort = 8883;
bool provisioned = false;           // all provisioning fields present
String deviceImei;                  // from AT+CGSN
bool identityOk = false;            // provisioned IMEI == modem IMEI
String topicPay, topicHist, topicStatus;
String currentLang = "HIND";
bool isFirstSetupDone = false;
int volumeLevel = 100;

int menuCursor = 0;
int menuTopIndex = 0;
int langCursor = 0;
const int TOTAL_MENU_ITEMS = 7;
const char* menuItems[7] = {
  "1. Language",
  "2. Volume",
  "3. Replay Last Txn",
  "4. Last 3 Txns",
  "5. Reconnect Azure",
  "6. Power Off",
  "7. Exit Menu"
};

// 4G, Azure MQTT & Clock State Variables
bool modemOnline = false;
bool simInserted = false;
bool netRegistered = false;
bool azureConnected = false;
int signalCSQ = 0;
String netGen = "4G";
String operatorName = "Vi";
int rtcHour = 15, rtcMin = 45, rtcSec = 0;
int rtcDay = 21, rtcMonth = 10, rtcYear = 2026;
bool colonBlink = true;
unsigned long lastSecondTick = 0;
unsigned long lastSignalPoll = 0;
unsigned long lastClockSync = 0;
unsigned long nextAzureTry = 0;
unsigned long nextModemTry = 0;
unsigned long azureBackoff = 15000;
int atFails = 0;
int connFails = 0;

long lastAmount = 0;
long todayTotal = 0;
unsigned long paymentPopupTimer = 0;
int histSpeaking = -1;

enum SystemState {
  STATE_BOOT_WELCOME,
  STATE_FIRST_SETUP,
  STATE_4G_CONNECTING,
  STATE_READY_QR_CLOCK,
  STATE_PAYMENT_ALERT,
  STATE_HISTORY_VIEW,
  STATE_PHONE_MENU,
  STATE_LANG_SUBMENU,
  STATE_VOLUME_SUBMENU,
  STATE_POWERED_OFF
};
SystemState sysState = STATE_BOOT_WELCOME;

void renderUI(const char* stepMsg = "");
void pumpModem();

const char* getDayName(int d, int m, int y) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
  return days[dow];
}

// =====================================================
// TRANSACTION HISTORY (newest first, persisted in flash)
// =====================================================
struct Txn { String id; long amount; uint32_t ts; };
Txn history[HISTORY_SIZE];
int historyCount = 0;

void saveHistory() {
  prefs.putUChar("hn", historyCount);
  for (int i = 0; i < HISTORY_SIZE; i++) {
    char k[8];
    snprintf(k, sizeof k, "h%di", i); prefs.putString(k, i < historyCount ? history[i].id : String(""));
    snprintf(k, sizeof k, "h%da", i); prefs.putLong(k, i < historyCount ? history[i].amount : 0);
    snprintf(k, sizeof k, "h%dt", i); prefs.putULong(k, i < historyCount ? history[i].ts : 0);
  }
}

void loadHistory() {
  int n = prefs.getUChar("hn", 0);
  if (n > HISTORY_SIZE) n = HISTORY_SIZE;
  historyCount = 0;
  for (int i = 0; i < n; i++) {
    char k[8];
    snprintf(k, sizeof k, "h%da", i); long amt = prefs.getLong(k, 0);
    if (amt <= 0) continue;
    history[historyCount].amount = amt;
    snprintf(k, sizeof k, "h%di", i); history[historyCount].id = prefs.getString(k, "");
    snprintf(k, sizeof k, "h%dt", i); history[historyCount].ts = prefs.getULong(k, 0);
    historyCount++;
  }
}

// Union of what we have and what we are given, de-duplicated by txn id, newest (largest ts) first.
void mergeHistory(const Txn* in, int n) {
  Txn cand[HISTORY_SIZE * 2];
  int c = 0;
  for (int i = 0; i < historyCount && c < HISTORY_SIZE * 2; i++) cand[c++] = history[i];
  for (int i = 0; i < n && c < HISTORY_SIZE * 2; i++) {
    bool dup = false;
    for (int j = 0; j < c; j++) {
      if (in[i].id.length() ? cand[j].id == in[i].id
                            : (cand[j].amount == in[i].amount && cand[j].ts == in[i].ts)) { dup = true; break; }
    }
    if (!dup) cand[c++] = in[i];
  }
  for (int i = 1; i < c; i++) {                       // stable insertion sort, ts descending
    Txn t = cand[i];
    int j = i - 1;
    while (j >= 0 && cand[j].ts < t.ts) { cand[j + 1] = cand[j]; j--; }
    cand[j + 1] = t;
  }
  historyCount = c < HISTORY_SIZE ? c : HISTORY_SIZE;
  for (int i = 0; i < historyCount; i++) history[i] = cand[i];
  saveHistory();
}

void printHistory() {
  Serial.printf("History (%d):\n", historyCount);
  for (int i = 0; i < historyCount; i++)
    Serial.printf("  %d. Rs.%ld  id=%s ts=%lu\n", i + 1, history[i].amount, history[i].id.c_str(), (unsigned long)history[i].ts);
}

// Ids of live payments already announced this boot (guards QoS 1 redelivery after reconnects).
String announcedIds[8];
int announcedNext = 0;
bool alreadyAnnounced(const String& id) {
  if (!id.length()) return false;
  for (int i = 0; i < 8; i++) if (announcedIds[i] == id) return true;
  return false;
}
void markAnnounced(const String& id) {
  if (!id.length()) return;
  announcedIds[announcedNext] = id;
  announcedNext = (announcedNext + 1) % 8;
}

// =====================================================
// WAV AUDIO PLAYER WITH DIGITAL VOLUME CONTROL
// =====================================================
struct WavInfo { uint32_t rate; uint16_t channels; uint32_t dataLen; };

// Walks the RIFF chunks, so extra chunks (LIST, etc.) before "data" are handled. 16-bit PCM only.
bool parseWav(File &f, WavInfo &w) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return false;
  bool haveFmt = false;
  uint16_t fmtType = 0, bits = 0;
  for (int guard = 0; guard < 24; guard++) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) return false;
    uint32_t size = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (!memcmp(ch, "fmt ", 4) && size >= 16) {
      uint8_t fm[16];
      if (f.read(fm, 16) != 16) return false;
      fmtType = fm[0] | (fm[1] << 8);
      w.channels = fm[2] | (fm[3] << 8);
      w.rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
      bits = fm[14] | (fm[15] << 8);
      haveFmt = true;
      uint32_t rest = size - 16 + (size & 1);
      if (rest) f.seek(f.position() + rest);
    } else if (!memcmp(ch, "data", 4)) {
      w.dataLen = size;
      return haveFmt && fmtType == 1 && bits == 16 && (w.channels == 1 || w.channels == 2);
    } else {
      f.seek(f.position() + size + (size & 1));
    }
  }
  return false;
}

void playClip(String langFolder, String fileBaseName) {
  if (!sdReady) return;

  String path = "/" + langFolder + "/" + fileBaseName + ".wav";
  File wavFile = SD.open(path, FILE_READ);
  if (!wavFile) {
    Serial.printf("Missing file: %s\n", path.c_str());
    return;
  }

  WavInfo w;
  if (!parseWav(wavFile, w)) {
    Serial.printf("Unsupported/corrupt WAV (need 16-bit PCM): %s\n", path.c_str());
    wavFile.close();
    return;
  }

  uint32_t rate = (w.rate >= 8000 && w.rate <= 48000) ? w.rate : 48000;
  i2s_slot_mode_t slotMode = (w.channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

  static uint32_t currentRate = 0;
  static i2s_slot_mode_t currentSlot = I2S_SLOT_MODE_MONO;

  if (rate != currentRate || slotMode != currentSlot) {
    i2s.end();
    i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN, -1, -1);
    i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, slotMode);
    currentRate = rate;
    currentSlot = slotMode;
  }

  static int16_t sampleBuffer[1024];
  uint32_t remaining = w.dataLen;
  const uint32_t frameBytes = 2 * w.channels;

  while (remaining > 0) {
    size_t want = remaining < sizeof(sampleBuffer) ? remaining : sizeof(sampleBuffer);
    size_t bytesRead = wavFile.read((uint8_t*)sampleBuffer, want);
    bytesRead -= bytesRead % frameBytes;
    if (bytesRead == 0) break;
    remaining -= bytesRead;

    if (volumeLevel < 100) {
      int numSamples = bytesRead / 2;
      for (int i = 0; i < numSamples; i++) {
        sampleBuffer[i] = (int16_t)(((int32_t)sampleBuffer[i] * volumeLevel) / 100);
      }
    }
    i2s.write((uint8_t*)sampleBuffer, bytesRead);
    pumpModem();                       // keep draining the modem so no push is lost while speaking
  }
  wavFile.close();
}

// =====================================================
// DRAWING
// =====================================================
void drawFullLeftQR() {
  u8g2.setDrawColor(1);
  for (int row = 0; row < 29; row++) {
    uint32_t rowBits = UPI_QR_29x29[row];
    for (int col = 0; col < 29; col++) {
      if ((rowBits >> (28 - col)) & 1) {
        u8g2.drawBox(1 + (col * 2), 3 + (row * 2), 2, 2);
      }
    }
  }
}

void drawRightSideDashboard() {
  u8g2.drawLine(61, 0, 61, 63);

  int bars = 0;
  if (modemOnline && simInserted && signalCSQ > 0 && signalCSQ != 99) {
    if (signalCSQ >= 22)      bars = 5;
    else if (signalCSQ >= 16) bars = 4;
    else if (signalCSQ >= 11) bars = 3;
    else if (signalCSQ >= 5)  bars = 2;
    else                      bars = 1;
  }

  for (int b = 0; b < 5; b++) {
    int bh = (b + 1) * 2 - 1;
    int bx = 64 + (b * 3);
    int by = 10 - bh;
    if (b < bars) u8g2.drawBox(bx, by, 2, bh);
    else { u8g2.drawPixel(bx, 9); u8g2.drawPixel(bx + 1, 9); }
  }

  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.setCursor(81, 10);
  if (!modemOnline)      u8g2.print("No 4G");
  else if (!simInserted) u8g2.print("NoSIM");
  else {
    u8g2.print(operatorName);
    u8g2.print(" ");
    u8g2.print(netGen);
  }

  u8g2.setFont(u8g2_font_fub14_tn);
  char timeStr[10];
  sprintf(timeStr, "%02d%c%02d", rtcHour, colonBlink ? ':' : ' ', rtcMin);
  u8g2.drawStr(65, 28, timeStr);

  u8g2.setFont(u8g2_font_6x10_tf);
  char dateLine[16];
  sprintf(dateLine, "%s %02d/%02d", getDayName(rtcDay, rtcMonth, rtcYear), rtcDay, rtcMonth);
  u8g2.drawStr(66, 39, dateLine);

  u8g2.drawLine(61, 41, 127, 41);

  u8g2.setFont(u8g2_font_6x10_tf);
  if (!sdReady) {
    u8g2.drawStr(65, 50, "SD ERROR!");
  } else if (!modemOnline) {
    u8g2.drawStr(65, 50, "4G ERROR!");
  } else if (!simInserted) {
    u8g2.drawStr(65, 50, "SIM ERROR!");
  } else if (!provisioned) {
    u8g2.drawStr(65, 50, "NOT PROV.");
  } else if (!identityOk) {
    u8g2.drawStr(65, 50, "IMEI ERR!");
  } else {
    if ((rtcSec % 6) < 3) {
      u8g2.drawStr(64, 50, "SCAN & PAY");
    } else {
      u8g2.drawStr(64, 50, azureConnected ? "AZURE: OK" : "AZURE:WAIT");
    }
  }

  u8g2.drawBox(62, 52, 66, 12);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  if ((rtcSec % 6) < 3) u8g2.drawStr(74, 61, "RUPEECO");
  else                  u8g2.drawStr(80, 61, "PAWAN");
  u8g2.setDrawColor(1);
}

void drawBootErrorPopup(const char* title, const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_helvB08_tf);
  u8g2.drawStr(8, 11, title);
  u8g2.setDrawColor(1);

  u8g2.drawRFrame(2, 17, 124, 45, 3);
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(8, 34, line1);
  u8g2.setFont(u8g2_font_helvB08_tf);
  u8g2.drawStr(8, 52, line2);
  u8g2.sendBuffer();
}

void renderUI(const char* stepMsg) {
  u8g2.clearBuffer();

  if (sysState == STATE_BOOT_WELCOME) {
    u8g2.drawRFrame(0, 0, 128, 64, 4);
    u8g2.drawRFrame(3, 3, 122, 58, 2);
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(22, 22, "RUPEECO");
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(14, 38, "WELCOME PAWAN");
    u8g2.drawLine(14, 42, 114, 42);
    u8g2.drawStr(12, 55, "Azure 4G Soundbox");
  }
  else if (sysState == STATE_FIRST_SETUP || sysState == STATE_LANG_SUBMENU) {
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(10, 10, "SELECT LANGUAGE (OK)");
    u8g2.setDrawColor(1);

    if (langCursor == 0) {
      u8g2.drawRBox(4, 18, 120, 20, 3);
      u8g2.setDrawColor(0);
      u8g2.drawStr(12, 32, "> 1. HINDI (Default)");
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawRFrame(4, 18, 120, 20, 3);
      u8g2.drawStr(12, 32, "  1. HINDI (Default)");
    }

    if (langCursor == 1) {
      u8g2.drawRBox(4, 42, 120, 20, 3);
      u8g2.setDrawColor(0);
      u8g2.drawStr(12, 56, "> 2. ENGLISH");
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawRFrame(4, 42, 120, 20, 3);
      u8g2.drawStr(12, 56, "  2. ENGLISH");
    }
  }
  else if (sysState == STATE_PHONE_MENU) {
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(18, 10, "SYSTEM MENU (OK)");
    u8g2.setDrawColor(1);

    for (int i = 0; i < 3; i++) {
      int itemIdx = menuTopIndex + i;
      if (itemIdx >= TOTAL_MENU_ITEMS) break;
      int yBox = 15 + (i * 16);
      if (itemIdx == menuCursor) {
        u8g2.drawRBox(2, yBox, 118, 15, 2);
        u8g2.setDrawColor(0);
        u8g2.drawStr(6, yBox + 11, menuItems[itemIdx]);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(6, yBox + 11, menuItems[itemIdx]);
      }
    }
    u8g2.drawFrame(123, 15, 4, 47);
    int thumbY = 16 + (menuCursor * 38) / (TOTAL_MENU_ITEMS - 1);
    u8g2.drawBox(124, thumbY, 2, 7);
  }
  else if (sysState == STATE_VOLUME_SUBMENU) {
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(14, 10, "VOLUME CONTROL (OK)");
    u8g2.setDrawColor(1);

    u8g2.setFont(u8g2_font_ncenB10_tr);
    char volStr[20];
    sprintf(volStr, "Volume: %d%%", volumeLevel);
    u8g2.drawStr(18, 30, volStr);

    u8g2.drawRFrame(10, 36, 108, 12, 3);
    int fillW = (volumeLevel * 104) / 100;
    if (fillW > 0) u8g2.drawBox(12, 38, fillW, 8);

    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(6, 61, "UP:(+)   OK:Save   DN:(-)");
  }
  else if (sysState == STATE_4G_CONNECTING) {
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(8, 10, "AZURE 4G CONNECTING");
    u8g2.setDrawColor(1);

    u8g2.drawRFrame(4, 18, 120, 26, 3);
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(8, 35);
    u8g2.print(stepMsg);

    u8g2.drawFrame(14, 51, 100, 7);
    int barW = (millis() / 25) % 96;
    u8g2.drawBox(16, 53, barW, 3);
  }
  else if (sysState == STATE_READY_QR_CLOCK) {
    drawFullLeftQR();
    drawRightSideDashboard();
  }
  else if (sysState == STATE_PAYMENT_ALERT) {
    u8g2.drawBox(0, 0, 128, 14);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(14, 11, "RUPEECO RECEIVED");
    u8g2.setDrawColor(1);

    u8g2.drawRFrame(2, 16, 124, 34, 3);
    u8g2.setFont(u8g2_font_fub17_tf);
    char amtStr[20];
    sprintf(amtStr, "Rs.%ld", lastAmount);
    int xPos = (128 - (strlen(amtStr) * 11)) / 2;
    u8g2.setCursor(xPos < 4 ? 4 : xPos, 42);
    u8g2.print(amtStr);

    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(14, 62, currentLang == "HIND" ? "Prapt Huye - Safal" : "Payment Successful");
  }
  else if (sysState == STATE_HISTORY_VIEW) {
    u8g2.drawBox(0, 0, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(10, 10, "LAST 3 TRANSACTIONS");
    u8g2.setDrawColor(1);

    for (int i = 0; i < HISTORY_SIZE; i++) {
      int yBox = 15 + (i * 16);
      char row[32];
      if (i < historyCount) snprintf(row, sizeof row, "%d. Rs.%ld", i + 1, history[i].amount);
      else                  snprintf(row, sizeof row, "%d. --", i + 1);
      u8g2.setFont(u8g2_font_ncenB08_tr);
      if (i == histSpeaking) {
        u8g2.drawRBox(2, yBox, 124, 15, 2);
        u8g2.setDrawColor(0);
        u8g2.drawStr(8, yBox + 11, row);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(8, yBox + 11, row);
      }
    }
  }
  else if (sysState == STATE_POWERED_OFF) {
    u8g2.drawRFrame(4, 10, 120, 44, 4);
    u8g2.setFont(u8g2_font_helvB08_tf);
    u8g2.drawStr(14, 28, "RUPEECO SOUNDBOX");
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(18, 44, "POWERING OFF...");
  }

  u8g2.sendBuffer();
}

// =====================================================
// OK-BUTTON GESTURES (home screen): click / double click / long press
// =====================================================
bool okDown = false, okLongFired = true;
uint32_t okPressAt = 0, okReleaseAt = 0;
int okClicks = 0;

void okResync() {                      // forget a button that is still held from a previous screen
  okClicks = 0;
  okDown = (digitalRead(BTN_OK) == HIGH);
  okLongFired = true;
}

// Returns 0 = nothing, 1 = click, 2 = double click, 3 = long press.
int pollOk() {
  bool p = (digitalRead(BTN_OK) == HIGH);
  uint32_t now = millis();
  if (p && !okDown) {
    okDown = true; okPressAt = now; okLongFired = false;
  } else if (p && okDown && !okLongFired && now - okPressAt >= LONG_PRESS_MS) {
    okLongFired = true; okClicks = 0;
    return 3;
  } else if (!p && okDown) {
    okDown = false;
    if (!okLongFired && now - okPressAt >= 30) { okClicks++; okReleaseAt = now; }
  }
  if (!okDown && okClicks > 0 && now - okReleaseAt >= DOUBLE_GAP_MS) {
    int c = okClicks; okClicks = 0;
    return c >= 2 ? 2 : 1;
  }
  return 0;
}

void goHome() {
  sysState = STATE_READY_QR_CLOCK;
  okResync();
  renderUI();
}

// =====================================================
// INDIAN NUMBER-TO-VOICE ENGINE
// =====================================================
void speakNumber(long n, String lang) {
  if (n == 0) { playClip(lang, "num0"); return; }
  if (n >= 10000000) { speakNumber(n / 10000000, lang); playClip(lang, "crore"); n %= 10000000; }
  if (n >= 100000)   { speakNumber(n / 100000, lang);   playClip(lang, "lakh");  n %= 100000; }
  if (n >= 1000)     { speakNumber(n / 1000, lang);     playClip(lang, "hazaar");n %= 1000; }
  if (n >= 100)      { speakNumber(n / 100, lang);      playClip(lang, "sau");   n %= 100; }
  if (n > 0)         { playClip(lang, "num" + String(n)); }
}

void announcePayment(long amount, bool isReplay) {
  lastAmount = amount;
  if (!isReplay) todayTotal += amount;
  sysState = STATE_PAYMENT_ALERT;
  paymentPopupTimer = millis();
  renderUI();

  Serial.printf("%s [%s]: Rs. %ld\n", isReplay ? "REPLAY" : "ANNOUNCING PAYMENT", currentLang.c_str(), amount);

  playClip(currentLang, "ding");
  delay(50);
  playClip(currentLang, "prefix");
  speakNumber(amount, currentLang);
  playClip(currentLang, "rupaye");
  playClip(currentLang, "prapt_thx");

  paymentPopupTimer = millis();
}

void replayLast() {
  if (historyCount == 0) { goHome(); playClip(currentLang, "no_txn"); return; }
  playClip(currentLang, "replay");
  announcePayment(history[0].amount, true);
}

void replayLast3() {
  if (historyCount == 0) { goHome(); playClip(currentLang, "no_txn"); return; }
  sysState = STATE_HISTORY_VIEW;
  histSpeaking = -1;
  renderUI();
  playClip(currentLang, "replay");
  for (int i = 0; i < historyCount; i++) {
    histSpeaking = i;
    renderUI();
    speakNumber(history[i].amount, currentLang);
    playClip(currentLang, "rupaye");
    delay(300);
  }
  histSpeaking = -1;
  renderUI();
  paymentPopupTimer = millis();
}

// =====================================================
// MODEM READER: the ONLY place that reads Serial1.
// Parses AT replies and MQTT pushes by length, so nothing is lost or misread.
//   +CMQTTRXSTART: 0,<topicLen>,<payloadLen>
//   +CMQTTRXTOPIC: 0,<len>\r\n<topic bytes>
//   +CMQTTRXPAYLOAD: 0,<len>\r\n<payload bytes>
//   +CMQTTRXEND: 0
// =====================================================
struct RxMsg { bool hist; String payload; };
RxMsg rxq[RX_QUEUE_SIZE];
int rxHead = 0, rxCount = 0;

String mLine;
bool mRaw = false;
int mRawKind = 0;                       // 1 = topic, 2 = payload
uint32_t mRawLeft = 0;
String mTopic, mPayload;
String mCapture;
bool mCapturing = false, mPrompt = false, mFinalOk = false, mFinalErr = false;
int mConnRes = -100, mSubRes = -100, mPubRes = -100;

void rxPush(bool hist, const String& payload) {
  if (rxCount == RX_QUEUE_SIZE) { rxHead = (rxHead + 1) % RX_QUEUE_SIZE; rxCount--; Serial.println("RX queue full, dropped oldest"); }
  int tail = (rxHead + rxCount) % RX_QUEUE_SIZE;
  rxq[tail].hist = hist;
  rxq[tail].payload = payload;
  rxCount++;
}

long csvInt(const String& line, int idx) {          // idx-th comma separated integer after ':'
  int p = line.indexOf(':');
  if (p < 0) return -1;
  String rest = line.substring(p + 1);
  for (int i = 0; i < idx; i++) {
    int c = rest.indexOf(',');
    if (c < 0) return -1;
    rest = rest.substring(c + 1);
  }
  return rest.toInt();
}

void handleModemLine(const String& line) {
#if MODEM_DEBUG
  Serial.printf("<< %s\n", line.c_str());
#endif
  if (line.startsWith("+CMQTTRXSTART:"))   { mTopic = ""; mPayload = ""; return; }
  if (line.startsWith("+CMQTTRXTOPIC:"))   { long n = csvInt(line, 1); if (n > 0) { mRaw = true; mRawKind = 1; mRawLeft = n; } return; }
  if (line.startsWith("+CMQTTRXPAYLOAD:")) { long n = csvInt(line, 1); if (n > 0) { mRaw = true; mRawKind = 2; mRawLeft = n; } return; }
  if (line.startsWith("+CMQTTRXEND:")) {
    if (mTopic == topicHist)      rxPush(true, mPayload);
    else if (mTopic == topicPay)  rxPush(false, mPayload);
    else Serial.printf("Ignoring message on unexpected topic '%s'\n", mTopic.c_str());
    mTopic = ""; mPayload = "";
    return;
  }
  if (line.startsWith("+CMQTTCONNLOST:") || line.startsWith("+CMQTTNONET")) { azureConnected = false; return; }
  if (line.startsWith("+CMQTTCONNECT: "))  { mConnRes = (int)csvInt(line, 1); return; }
  if (line.startsWith("+CMQTTSUB: "))      { mSubRes = (int)csvInt(line, 1); return; }
  if (line.startsWith("+CMQTTPUB: "))      { mPubRes = (int)csvInt(line, 1); return; }

  if (mCapturing) {
    if (line == "OK") mFinalOk = true;
    else if (line.indexOf("ERROR") >= 0) mFinalErr = true;
    mCapture += line + "\n";
  }
}

void pumpModem() {
  while (MODEM.available()) {
    char c = (char)MODEM.read();
    if (mRaw) {
      String &dst = (mRawKind == 1) ? mTopic : mPayload;
      if (dst.length() < 1100) dst += c;
      if (--mRawLeft == 0) mRaw = false;
      continue;
    }
    if (c == '\n') {
      mLine.trim();
      if (mLine.length()) handleModemLine(mLine);
      mLine = "";
    } else if (c != '\r') {
      mLine += c;
      if (mLine == ">") { mPrompt = true; mLine = ""; }       // data prompt carries no newline
      else if (mLine.length() > 600) mLine = "";
    }
  }
}

bool atOk = false;

String sendAT(const char* cmd, unsigned long timeoutMs = 1200) {
  pumpModem();                                     // process anything already pending, never discard it
  mCapture = ""; mCapturing = true; mFinalOk = mFinalErr = false;
  Serial.printf(">> AT: %s\n", cmd);
  MODEM.println(cmd);
  unsigned long start = millis();
  while (millis() - start < timeoutMs && !mFinalOk && !mFinalErr) { pumpModem(); delay(1); }
  mCapturing = false;
  atOk = mFinalOk;
  String r = mCapture;
  r.trim();
  return r;
}

// Command that answers with a '>' prompt, then takes raw data, then OK (CCERTDOWN, CMQTTTOPIC/PAYLOAD/SUB).
bool sendWithPrompt(const String& cmd, const char* data, size_t len, unsigned long promptMs = 3000, unsigned long okMs = 4000) {
  pumpModem();
  mPrompt = false; mCapture = ""; mCapturing = true; mFinalOk = mFinalErr = false;
  Serial.printf(">> AT: %s\n", cmd.c_str());
  MODEM.println(cmd);
  unsigned long start = millis();
  while (millis() - start < promptMs && !mPrompt && !mFinalErr) { pumpModem(); delay(1); }
  if (!mPrompt) { mCapturing = false; Serial.println("No '>' prompt"); return false; }
  MODEM.write((const uint8_t*)data, len);
  start = millis();
  while (millis() - start < okMs && !mFinalOk && !mFinalErr) { pumpModem(); delay(1); }
  mCapturing = false;
  return mFinalOk;
}

bool waitResult(int &var, unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms && var == -100) { pumpModem(); delay(5); }
  return var != -100;
}

static uint32_t fnv(uint32_t h, const char* s) { while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
uint32_t certFingerprint() {
  uint32_t h = 2166136261u;
  h = fnv(h, provCa.c_str()); h = fnv(h, provCert.c_str()); h = fnv(h, provKey.c_str());
  return h;
}

bool uploadCertToModem(const char* name, const char* pem) {
  sendAT((String("AT+CCERTDELE=\"") + name + "\"").c_str(), 600);
  bool ok = sendWithPrompt(String("AT+CCERTDOWN=\"") + name + "\"," + String(strlen(pem)), pem, strlen(pem), 2000, 3000);
  Serial.printf("Cert %s: %s\n", name, ok ? "OK" : "FAILED");
  return ok;
}

// ---- Identity ----
void loadProvisioning() {
  Preferences prov;
  provisioned = false;
  if (!prov.begin("prov", true)) { Serial.println("No provisioning data in NVS"); return; }
  provHost = prov.getString("host", "");
  provPort = prov.getUShort("port", 8883);
  provImei = prov.getString("imei", "");
  provCa   = prov.getString("ca", "");
  provCert = prov.getString("cert", "");
  provKey  = prov.getString("key", "");
  prov.end();
  provisioned = provHost.length() && provImei.length() == 15 &&
                provCa.length() > 100 && provCert.length() > 100 && provKey.length() > 50;
  Serial.printf("Provisioning: %s (host=%s port=%u imei=%s)\n", provisioned ? "OK" : "INCOMPLETE",
                provHost.c_str(), provPort, provImei.c_str());
}

// IMEI = 15 digits in the AT+CGSN reply.
bool readModemImei() {
  String r = sendAT("AT+CGSN", 1000);
  int run = 0;
  for (unsigned i = 0; i < r.length(); i++) {
    if (isDigit(r[i])) {
      if (++run == 15) { deviceImei = r.substring(i - 14, i + 1); break; }
    } else run = 0;
  }
  if (deviceImei.length() != 15) { Serial.println("Could not read IMEI"); return false; }
  Serial.printf("Modem IMEI: %s\n", deviceImei.c_str());
  return true;
}

void updateIdentity() {
  identityOk = false;
  if (deviceImei.length() != 15) return;
  topicPay = "soundbox/" + deviceImei + "/payments";
  topicHist = "soundbox/" + deviceImei + "/history";
  topicStatus = "soundbox/" + deviceImei + "/status";
  if (!provisioned) return;
  identityOk = (provImei == deviceImei);
  if (!identityOk) Serial.printf("IMEI MISMATCH: provisioned %s, modem %s\n", provImei.c_str(), deviceImei.c_str());
}

// Publish a status / ack JSON to soundbox/<id>/status (QoS 1).
bool publishStatus(const String& json) {
  if (!azureConnected) return false;
  String topic = topicStatus;
  if (!sendWithPrompt(String("AT+CMQTTTOPIC=0,") + String((unsigned)topic.length()), topic.c_str(), topic.length())) return false;
  if (!sendWithPrompt(String("AT+CMQTTPAYLOAD=0,") + String((unsigned)json.length()), json.c_str(), json.length())) return false;
  mPubRes = -100;
  sendAT("AT+CMQTTPUB=0,1,60", 2000);
  if (!atOk) return false;
  return waitResult(mPubRes, 8000) && mPubRes == 0;
}

bool subscribeTopic(const char* topic) {
  mSubRes = -100;
  if (!sendWithPrompt(String("AT+CMQTTSUB=0,") + String((unsigned)strlen(topic)) + ",1", topic, strlen(topic))) return false;
  return waitResult(mSubRes, 6000) && mSubRes == 0;
}

void failAzure(const char* why) {
  Serial.printf("Azure MQTT failed: %s\n", why);
  azureConnected = false;
  sendAT("AT+CMQTTDISC=0,10", 1000);
  sendAT("AT+CMQTTREL=0", 800);
  if (++connFails >= 2) { prefs.remove("certfp"); Serial.println("Will re-upload certs next try"); }
  nextAzureTry = millis() + azureBackoff;
  azureBackoff = azureBackoff * 2 > 120000 ? 120000 : azureBackoff * 2;
}

// Connect the modem's MQTT client to Azure Event Grid (mutual TLS) and subscribe.
bool connectAzureMQTT(bool quiet) {
  azureConnected = false;
  if (!provisioned || !identityOk) {              // nothing to retry until the unit is provisioned / IMEI verified
    Serial.println(!provisioned ? "Not provisioned (run scripts/onboard_device.py)" : "IMEI mismatch: refusing to connect");
    nextAzureTry = millis() + 60000;
    return false;
  }
  if (!quiet) renderUI("Starting Azure MQTT");
  Serial.println("Connecting to Azure Event Grid MQTT...");

  sendAT("AT+CGACT=1,1", 2500);

  // Certs go to the modem's own filesystem; only re-upload when they changed or a connect kept failing.
  uint32_t fp = certFingerprint();
  if (prefs.getUInt("certfp", 0) != fp) {
    if (!quiet) renderUI("Loading mTLS Certs");
    bool ok = uploadCertToModem("root_ca.pem", provCa.c_str());
    ok = uploadCertToModem("client_cert.pem", provCert.c_str()) && ok;
    ok = uploadCertToModem("client_key.pem", provKey.c_str()) && ok;
    if (ok) prefs.putUInt("certfp", fp);
  }

  sendAT("AT+CSSLCFG=\"sslversion\",0,4", 800);                    // TLS 1.2
  sendAT("AT+CSSLCFG=\"authmode\",0,2", 800);                      // verify server + present client cert
  sendAT("AT+CSSLCFG=\"cacert\",0,\"root_ca.pem\"", 800);
  sendAT("AT+CSSLCFG=\"clientcert\",0,\"client_cert.pem\"", 800);
  sendAT("AT+CSSLCFG=\"clientkey\",0,\"client_key.pem\"", 800);
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1", 800);                     // required by Event Grid
  sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", 800);               // modem clock may be wrong right after boot

  sendAT("AT+CMQTTDISC=0,10", 800);
  sendAT("AT+CMQTTREL=0", 800);
  sendAT("AT+CMQTTSTOP", 1500);
  sendAT("AT+CMQTTSTART", 2500);

  sendAT((String("AT+CMQTTACCQ=0,\"") + deviceImei + "\",1").c_str(), 1000);
  sendAT("AT+CMQTTSSLCFG=0,0", 800);

  if (!quiet) renderUI("Auth Azure Cloud..");
  String base = String("AT+CMQTTCONNECT=0,\"tcp://") + provHost + ":" + String(provPort) + "\",60,1,\"" + deviceImei + "\"";
  mConnRes = -100;
  sendAT(base.c_str(), 6000);
  if (!atOk) {                                    // some firmware wants a password argument even when empty
    mConnRes = -100;
    sendAT((base + ",\"\"").c_str(), 6000);
  }
  if (!atOk && mConnRes == -100) { failAzure("CMQTTCONNECT rejected"); return false; }
  if (!waitResult(mConnRes, 20000)) { failAzure("connect timeout"); return false; }
  if (mConnRes != 0) { char b[32]; snprintf(b, sizeof b, "connect error %d", mConnRes); failAzure(b); return false; }

  Serial.println("Connected to Azure Event Grid MQTT");
  if (!subscribeTopic(topicPay.c_str())) { failAzure("subscribe to payments denied"); return false; }
  if (!subscribeTopic(topicHist.c_str())) Serial.println("History subscribe failed (history feature limited to local data)");

  azureConnected = true;
  connFails = 0;
  azureBackoff = 15000;
  if (!quiet) playClip(currentLang, "mqtt_con");
  publishStatus(String("{\"device\":\"") + deviceImei + "\",\"status\":\"ONLINE\"}");
  return true;
}

// =====================================================
// MESSAGE HANDLING (runs from loop(), never from inside audio playback)
// =====================================================
void handleHistoryMessage(const String& payload) {
  // Retained list, newest first: {"txns":[{"txnId":"..","amount":150.0,"ts":1700000000}, ...]}
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { Serial.println("Bad history JSON"); return; }
  Txn in[HISTORY_SIZE];
  int n = 0;
  for (JsonObject t : doc["txns"].as<JsonArray>()) {
    if (n >= HISTORY_SIZE) break;
    double a = t["amount"] | 0.0;
    if (!(a >= 1.0) || a > MAX_AMOUNT) continue;
    in[n].id = String((const char*)(t["txnId"] | ""));
    in[n].amount = (long)(a + 1e-6);
    in[n].ts = t["ts"] | 0u;
    n++;
  }
  if (n) mergeHistory(in, n);
  Serial.printf("History synced (%d received, %d stored)\n", n, historyCount);
}

void handlePaymentMessage(const String& payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { Serial.println("Bad payment JSON"); return; }

  const char* status = doc["status"] | "SUCCESS";
  if (strcasecmp(status, "SUCCESS") != 0) { Serial.printf("Ignoring payment with status %s\n", status); return; }

  double a = doc["amount"] | (doc["amt"] | 0.0);
  if (!(a >= 1.0) || a > MAX_AMOUNT) { Serial.printf("Rejected amount %.2f\n", a); return; }
  long rupees = (long)(a + 1e-6);                 // paise are not spoken

  String id = String((const char*)(doc["txnId"] | (doc["txn"] | "")));
  if (alreadyAnnounced(id)) { Serial.printf("Duplicate %s ignored\n", id.c_str()); return; }
  markAnnounced(id);

  uint32_t ts = doc["ts"] | 0u;
  if (ts == 0) ts = historyCount ? history[0].ts + 1 : 1;
  Txn t = { id, rupees, ts };
  mergeHistory(&t, 1);

  announcePayment(rupees, false);

  String ack = String("{\"device\":\"") + deviceImei + "\",\"txnId\":\"" + id + "\",\"amount\":" + String(rupees) + ",\"ack\":\"ANNOUNCED\"}";
  publishStatus(ack);
}

void processRxQueue() {
  while (rxCount > 0) {
    RxMsg m = rxq[rxHead];
    rxHead = (rxHead + 1) % RX_QUEUE_SIZE;
    rxCount--;
    if (m.hist) handleHistoryMessage(m.payload);
    else        handlePaymentMessage(m.payload);
  }
}

// =====================================================
// MODEM STATUS / BRING-UP
// =====================================================
void updateSignalAndNetType() {
  String r = sendAT("AT", 500);
  if (!atOk) {
    if (++atFails >= 2) { modemOnline = false; signalCSQ = 0; azureConnected = false; }
    return;
  }
  atFails = 0;
  modemOnline = true;

  r = sendAT("AT+CSQ", 500);
  int p = r.indexOf("+CSQ: ");
  if (p != -1) {
    signalCSQ = r.substring(p + 6, r.indexOf(",", p)).toInt();
    if (signalCSQ == 99) signalCSQ = 0;
  }

  r = sendAT("AT+COPS?", 600);
  if (r.indexOf("Vi") != -1 || r.indexOf("Vodafone") != -1 || r.indexOf("IDEA") != -1) operatorName = "Vi";
  else if (r.indexOf("Jio") != -1 || r.indexOf("JIO") != -1 || r.indexOf("4058") != -1) operatorName = "JIO";
  else if (r.indexOf("Airtel") != -1 || r.indexOf("AIRTEL") != -1) operatorName = "Airtel";
  else if (r.indexOf("BSNL") != -1) operatorName = "BSNL";

  if (r.indexOf(",7") != -1)      netGen = "4G";
  else if (r.indexOf(",2") != -1) netGen = "3G";
  else if (r.indexOf(",0") != -1) netGen = "2G";
  else                            netGen = "4G";
}

void syncModemClock() {
  String r = sendAT("AT+CCLK?", 800);
  int idx = r.indexOf("+CCLK: \"");
  if (idx != -1 && r.length() >= (unsigned)(idx + 25)) {
    String dt = r.substring(idx + 8, idx + 25);
    int y = 2000 + dt.substring(0, 2).toInt();
    if (y < 2024) return;                          // modem has not got network time yet; keep ours
    rtcYear  = y;
    rtcMonth = dt.substring(3, 5).toInt();
    rtcDay   = dt.substring(6, 8).toInt();
    rtcHour  = dt.substring(9, 11).toInt();
    rtcMin   = dt.substring(12, 14).toInt();
    rtcSec   = dt.substring(15, 17).toInt();
  }
}

// quiet = background retry: no screen changes, no voice prompts.
void bringUpModem(bool quiet) {
  if (!quiet) {
    sysState = STATE_4G_CONNECTING;
    renderUI("Starting 4G Modem..");
    playClip(currentLang, "net_start");
    renderUI("Checking A7670C...");
  }
  sendAT("AT", 1000);
  if (atOk) {
    modemOnline = true;
    atFails = 0;
    sendAT("ATE0", 500);
    sendAT("AT+CTZU=1", 500);
    if (readModemImei()) updateIdentity();

    if (!quiet) renderUI("Checking SIM Card..");
    String r = sendAT("AT+CPIN?", 1200);
    if (r.indexOf("READY") != -1) {
      simInserted = true;
      if (!quiet) renderUI("Scanning Tower..");
      netRegistered = false;
      for (int i = 0; i < 6; i++) {
        updateSignalAndNetType();
        r = sendAT("AT+CEREG?", 600);
        if (r.indexOf(",1") != -1 || r.indexOf(",5") != -1) { netRegistered = true; break; }
        delay(400);
      }

      syncModemClock();
      lastClockSync = millis();
      if (!quiet) { renderUI("4G Connected!"); playClip(currentLang, "net_con"); }

      azureBackoff = 15000;
      connectAzureMQTT(quiet);
      if (!quiet) playClip(currentLang, "dev_rdy");
    } else {
      simInserted = false;
      if (!quiet) {
        drawBootErrorPopup("! SIM CARD ERROR !", "SIM Not Detected", "Insert Active 4G SIM");
        playClip(currentLang, "sim_err");
        delay(2000);
      }
    }
  } else {
    modemOnline = false;
    simInserted = false;
    signalCSQ = 0;
    if (!quiet) {
      drawBootErrorPopup("! 4G MODULE ERROR !", "4G Not Connected!", "Check 4V & RX/TX Pin");
      playClip(currentLang, "net_dcon");
      delay(2000);
      playClip(currentLang, "dev_rdy");
    }
  }

  if (!quiet) goHome();
}

void changeVolume(int delta) {
  volumeLevel += delta;
  if (volumeLevel >= 100) {
    volumeLevel = 100;
    prefs.putInt("vol", volumeLevel);
    renderUI();
    playClip(currentLang, "max_vol");
  } else if (volumeLevel <= 10) {
    volumeLevel = 10;
    prefs.putInt("vol", volumeLevel);
    renderUI();
    playClip(currentLang, "min_vol");
  } else {
    prefs.putInt("vol", volumeLevel);
    renderUI();
    playClip(currentLang, delta > 0 ? "vol_up" : "vol_down");
  }
}

void triggerPowerOff() {
  sysState = STATE_POWERED_OFF;
  renderUI();
  playClip(currentLang, "power_off");
  playClip(currentLang, "off_tone");

  sendAT("AT+CMQTTDISC=0,10", 1000);
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setPowerSave(1);
  esp_sleep_enable_ext1_wakeup(1ULL << BTN_UP, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  MODEM.setRxBufferSize(4096);                      // must precede begin(); holds pushes while audio plays
  MODEM.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(MODEM_RST, OUTPUT);
  digitalWrite(MODEM_RST, HIGH);

  delay(500);
  Serial.println("\n=======================================================");
  Serial.println("   RUPEECO AZURE EVENT GRID MQTT 4G SOUNDBOX");
  Serial.println("=======================================================");

  prefs.begin("rupeeco", false);
  isFirstSetupDone = prefs.getBool("setup_done", false);
  currentLang = prefs.getString("lang", "HIND");
  volumeLevel = prefs.getInt("vol", 100);
  langCursor = (currentLang == "HIND") ? 0 : 1;
  loadHistory();
  printHistory();
  loadProvisioning();

  u8g2.begin();
  u8g2.setContrast(255);
  sysState = STATE_BOOT_WELCOME;
  renderUI();

  pinMode(BTN_UP, INPUT_PULLDOWN);
  pinMode(BTN_OK, INPUT_PULLDOWN);
  pinMode(BTN_DOWN, INPUT_PULLDOWN);
  okResync();

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN, -1, -1);
  i2s.begin(I2S_MODE_STD, 48000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);

  pinMode(SD_MISO_PIN, INPUT_PULLUP);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (SD.begin(SD_CS_PIN, sdSPI, 4000000)) {
    sdReady = true;
    playClip(currentLang, "boot_tone");
    playClip(currentLang, "power_on");
  } else {
    sdReady = false;
    drawBootErrorPopup("! SD CARD ERROR !", "SD Card Not Found!", "Check Card & Wiring");
    delay(2500);
  }

  if (!isFirstSetupDone) {
    sysState = STATE_FIRST_SETUP;
    langCursor = 0;
    renderUI();
    playClip("HIND", "first_setup");
  } else {
    bringUpModem(false);
  }

  lastSecondTick = millis();
  lastSignalPoll = millis();
  lastClockSync = millis();
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  unsigned long now = millis();

  pumpModem();
  processRxQueue();                                  // announce any payment that arrived

  if (sysState == STATE_PAYMENT_ALERT && (now - paymentPopupTimer > 4000)) goHome();
  if (sysState == STATE_HISTORY_VIEW && histSpeaking < 0 && (now - paymentPopupTimer > 3500)) goHome();

  if (now - lastSecondTick >= 1000) {
    lastSecondTick = now;
    colonBlink = !colonBlink;
    rtcSec++;
    if (rtcSec >= 60) { rtcSec = 0; rtcMin++; }
    if (rtcMin >= 60) { rtcMin = 0; rtcHour = (rtcHour + 1) % 24; }

    if (!sdReady && SD.begin(SD_CS_PIN, sdSPI, 4000000)) sdReady = true;
    if (sysState == STATE_READY_QR_CLOCK) renderUI();
  }

  // Serial Monitor: M menu | ID identity | AZURE reconnect | RESET | H history | L1 / L3 replay | <number> simulate payment
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      if (cmd.equalsIgnoreCase("M")) {
        menuCursor = 0; menuTopIndex = 0;
        sysState = STATE_PHONE_MENU;
        renderUI();
        playClip(currentLang, "menu_open");
      } else if (cmd.equalsIgnoreCase("AZURE")) {
        azureBackoff = 15000;
        connectAzureMQTT(false);
      } else if (cmd.equalsIgnoreCase("RESET")) {
        prefs.clear();
        ESP.restart();
      } else if (cmd.equalsIgnoreCase("ID")) {
        Serial.printf("IMEI=%s provisioned=%d identityOk=%d topic=%s\n", deviceImei.c_str(), provisioned, identityOk, topicPay.c_str());
      } else if (cmd.equalsIgnoreCase("H")) {
        printHistory();
      } else if (cmd.equalsIgnoreCase("L1")) {
        replayLast();
      } else if (cmd.equalsIgnoreCase("L3")) {
        replayLast3();
      } else if (cmd.toInt() > 0) {
        long amt = cmd.toInt();
        Txn t = { String("SIM") + String(millis()), amt, historyCount ? history[0].ts + 1 : 1 };
        mergeHistory(&t, 1);
        announcePayment(amt, false);
      }
    }
  }

  // Buttons
  if (sysState == STATE_FIRST_SETUP || sysState == STATE_LANG_SUBMENU) {
    if (digitalRead(BTN_UP) == HIGH || digitalRead(BTN_DOWN) == HIGH) {
      langCursor = (langCursor == 0) ? 1 : 0;
      renderUI();
      playClip(langCursor == 0 ? "HIND" : "ENG", "Beep");
      delay(250);
    } else if (digitalRead(BTN_OK) == HIGH) {
      bool wasFirstSetup = (sysState == STATE_FIRST_SETUP);
      currentLang = (langCursor == 0) ? "HIND" : "ENG";
      prefs.putString("lang", currentLang);
      prefs.putBool("setup_done", true);
      isFirstSetupDone = true;
      playClip(currentLang, "Beep");
      playClip(currentLang, currentLang == "HIND" ? "lang_hi" : "lang_en");
      if (wasFirstSetup) bringUpModem(false);
      else { sysState = STATE_PHONE_MENU; renderUI(); }
      delay(300);
    }
  }
  else if (sysState == STATE_PHONE_MENU) {
    if (digitalRead(BTN_UP) == HIGH) {
      menuCursor = (menuCursor - 1 + TOTAL_MENU_ITEMS) % TOTAL_MENU_ITEMS;
      if (menuCursor < menuTopIndex) menuTopIndex = menuCursor;
      if (menuCursor == TOTAL_MENU_ITEMS - 1) menuTopIndex = TOTAL_MENU_ITEMS - 3;
      renderUI(); playClip(currentLang, "Beep"); delay(220);
    } else if (digitalRead(BTN_DOWN) == HIGH) {
      menuCursor = (menuCursor + 1) % TOTAL_MENU_ITEMS;
      if (menuCursor == 0) menuTopIndex = 0;
      if (menuCursor >= menuTopIndex + 3) menuTopIndex = menuCursor - 2;
      renderUI(); playClip(currentLang, "Beep"); delay(220);
    } else if (digitalRead(BTN_OK) == HIGH) {
      playClip(currentLang, "Beep"); delay(250);
      if (menuCursor == 0)      { langCursor = (currentLang == "HIND") ? 0 : 1; sysState = STATE_LANG_SUBMENU; renderUI(); playClip(currentLang, "lang_menu"); }
      else if (menuCursor == 1) { sysState = STATE_VOLUME_SUBMENU; renderUI(); playClip(currentLang, "menu_vol"); }
      else if (menuCursor == 2) { goHome(); replayLast(); }
      else if (menuCursor == 3) { goHome(); replayLast3(); }
      else if (menuCursor == 4) { bringUpModem(false); }
      else if (menuCursor == 5) { triggerPowerOff(); }
      else if (menuCursor == 6) { playClip(currentLang, "menu_exit"); goHome(); }
    }
  }
  else if (sysState == STATE_VOLUME_SUBMENU) {
    if (digitalRead(BTN_UP) == HIGH) { changeVolume(+15); delay(250); }
    else if (digitalRead(BTN_DOWN) == HIGH) { changeVolume(-15); delay(250); }
    else if (digitalRead(BTN_OK) == HIGH) { playClip(currentLang, "Beep"); sysState = STATE_PHONE_MENU; renderUI(); delay(300); }
  }
  else if (sysState == STATE_HISTORY_VIEW) {
    if (histSpeaking < 0 && (digitalRead(BTN_UP) == HIGH || digitalRead(BTN_OK) == HIGH || digitalRead(BTN_DOWN) == HIGH)) {
      delay(250);
      goHome();
    }
  }
  else if (sysState == STATE_READY_QR_CLOCK || sysState == STATE_PAYMENT_ALERT) {
    if (digitalRead(BTN_UP) == HIGH) { changeVolume(+15); delay(250); }
    else if (digitalRead(BTN_DOWN) == HIGH) { changeVolume(-15); delay(250); }
    else {
      int g = pollOk();
      if (g == 1) {                                   // click: menu
        playClip(currentLang, "Beep");
        menuCursor = 0; menuTopIndex = 0;
        sysState = STATE_PHONE_MENU;
        renderUI();
        playClip(currentLang, "menu_open");
        delay(300);
      } else if (g == 2) {                            // double click: last 3
        replayLast3();
      } else if (g == 3) {                            // long press: last transaction
        replayLast();
      }
    }
  }

  // Background upkeep, only from the idle home screen so it never interrupts the user.
  if (sysState == STATE_READY_QR_CLOCK) {
    if (now - lastSignalPoll > 30000) { lastSignalPoll = now; updateSignalAndNetType(); }
    if (modemOnline && simInserted && now - lastClockSync > 600000) { lastClockSync = now; syncModemClock(); }
    if (modemOnline && simInserted && provisioned && identityOk && !azureConnected && now >= nextAzureTry) {
      connectAzureMQTT(true);
    } else if ((!modemOnline || !simInserted) && now >= nextModemTry) {
      nextModemTry = now + 60000;
      bringUpModem(true);
    }
  }

  delay(20);
}
