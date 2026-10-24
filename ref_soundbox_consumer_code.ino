#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include <esp_sleep.h>

// =====================================================
// 0. AZURE EVENT GRID MQTT CONFIGURATION & CERTIFICATES
// =====================================================
#define AZURE_MQTT_HOST   "rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net"
#define AZURE_MQTT_PORT   8883
#define AZURE_CLIENT_ID   "sb-device-001"
#define AZURE_USERNAME    "sb-device-001"
#define AZURE_SUB_TOPIC   "soundbox/sb-device-001/payments"
#define AZURE_PUB_TOPIC   "soundbox/sb-device-001/status"

// Server Trust: DigiCert Global Root G3
static const char AZURE_ROOT_CA[] PROGMEM =
"-----BEGIN CERTIFICATE-----\r\n"
"MIICPzCCAcWgAwIBAgIQBVVWvPJepDU1w6QP1atFcjAKBggqhkjOPQQDAzBhMQsw\r\n"
"CQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cu\r\n"
"ZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMzAe\r\n"
"Fw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVTMRUw\r\n"
"EwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20x\r\n"
"IDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEczMHYwEAYHKoZIzj0CAQYF\r\n"
"K4EEACIDYgAE3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FG\r\n"
"fp4tn+6OYwwX7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPO\r\n"
"Z9wj/wMco+I+o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAd\r\n"
"BgNVHQ4EFgQUs9tIpPmhxdiuNkHMEWNpYim8S8YwCgYIKoZIzj0EAwMDaAAwZQIx\r\n"
"AK288mw/EkrRLTnDCgmXc/SINoyIJ7vmiI1Qhadj+Z4y3maTD/HMsQmP3Wyr+mt/\r\n"
"oAIwOWZbwmSNuJ5Q3KjVSaLtx9zRSX8XAbjIho9OjIgrqJqpisXRAL34VOKa5Vt8\r\n"
"sycX\r\n"
"-----END CERTIFICATE-----\r\n";

// Device Client Certificate (sb-device-001.pem)
static const char AZURE_CLIENT_CERT[] PROGMEM =
"-----BEGIN CERTIFICATE-----\r\n"
"MIIBhTCCASugAwIBAgIUXDckIEkW7zJo6eczV1V/4kowFZYwCgYIKoZIzj0EAwIw\r\n"
"GDEWMBQGA1UEAwwNc2ItZGV2aWNlLTAwMTAeFw0yNjEwMjEwODA5MjdaFw0yNzEw\r\n"
"MjEwODA5MjdaMBgxFjAUBgNVBAMMDXNiLWRldmljZS0wMDEwWTATBgcqhkjOPQIB\r\n"
"BggqhkjOPQMBBwNCAAR/iEH/5SB/CcYcshFy03xFbhAFm621xdf0q95duN2oKoLB\r\n"
"1X5/gUeAUFejlWGP/5yzxNVA0cdGF+VqxqSdOe6+o1MwUTAdBgNVHQ4EFgQU8fZV\r\n"
"0jMcLFCnvxFXEMWNhZTIGk4wHwYDVR0jBBgwFoAU8fZV0jMcLFCnvxFXEMWNhZTI\r\n"
"Gk4wDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNIADBFAiEAsBTdnTULXY4e\r\n"
"Ma40COsyx1C/mf4eu/LibMqqP4rozN4CIEAjsj0582yHGvjhnn7/sbq3bdEUY6O+\r\n"
"GRL91zOcAP6G\r\n"
"-----END CERTIFICATE-----\r\n";

// Device Private Key (sb-device-001.key)
static const char AZURE_CLIENT_KEY[] PROGMEM =
"-----BEGIN EC PRIVATE KEY-----\r\n"
"MDECAQEEIJ59ji7Ra7ZWvSJlr3FnvpdFjJ5/pdYR9F/a1vjDNS65oAoGCCqGSM49\r\n"
"AwEH\r\n"
"-----END EC PRIVATE KEY-----\r\n";

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
// 3. MAX98357A I2S AUDIO PINS (Verified Working)
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
// 5. THREE TOUCH BUTTONS (UP / OK / DOWN)
// =====================================================
#define BTN_UP      1   // Top Button: Scroll UP / Volume +
#define BTN_OK      2   // Middle Button: Open Menu / Select (OK)
#define BTN_DOWN    42  // Bottom Button: Scroll DOWN / Volume -

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
String currentLang = "HIND";
bool isFirstSetupDone = false;
int volumeLevel = 100;

int menuCursor = 0;
int menuTopIndex = 0;
int langCursor = 0;
const int TOTAL_MENU_ITEMS = 6;
const char* menuItems[6] = {
  "1. Language",
  "2. Volume",
  "3. Replay Last Txn",
  "4. Reconnect Azure",
  "5. Power Off",
  "6. Exit Menu"
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

// Transaction State & Anti-Duplicate Cache
long lastAmount = 0;
long todayTotal = 0;
String lastTxnId = "";
unsigned long paymentPopupTimer = 0;
String modemRxBuffer = "";

enum SystemState {
  STATE_BOOT_WELCOME,
  STATE_FIRST_SETUP,
  STATE_4G_CONNECTING,
  STATE_READY_QR_CLOCK,
  STATE_PAYMENT_ALERT,
  STATE_PHONE_MENU,
  STATE_LANG_SUBMENU,
  STATE_VOLUME_SUBMENU,
  STATE_POWERED_OFF
};

SystemState sysState = STATE_BOOT_WELCOME;

struct WavHeader {
  char riff[4];
  uint32_t overallSize;
  char wave[4];
  char fmtChunkMarker[4];
  uint32_t lengthOfFmt;
  uint16_t formatType;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t byterate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
};

const char* getDayName(int d, int m, int y) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
  return days[dow];
}

// =====================================================
// WAV AUDIO PLAYER WITH DIGITAL VOLUME CONTROL
// =====================================================
void playClip(String langFolder, String fileBaseName) {
  if (!sdReady) return;

  String path = "/" + langFolder + "/" + fileBaseName + ".wav";
  File wavFile = SD.open(path, FILE_READ);
  if (!wavFile) {
    Serial.printf("⚠️ Missing File: %s\n", path.c_str());
    return;
  }

  WavHeader header;
  wavFile.read((uint8_t*)&header, sizeof(WavHeader));

  uint32_t rate = (header.sampleRate >= 8000 && header.sampleRate <= 48000) ? header.sampleRate : 48000;
  i2s_slot_mode_t slotMode = (header.channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

  static uint32_t currentRate = 0;
  static i2s_slot_mode_t currentSlot = I2S_SLOT_MODE_MONO;

  if (rate != currentRate || slotMode != currentSlot) {
    i2s.end();
    i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN, -1, -1);
    i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, slotMode);
    currentRate = rate;
    currentSlot = slotMode;
  }

  wavFile.seek(44);
  int16_t sampleBuffer[1024];
  size_t bytesRead = 0;

  while ((bytesRead = wavFile.read((uint8_t*)sampleBuffer, sizeof(sampleBuffer))) > 0) {
    if (volumeLevel < 100) {
      int numSamples = bytesRead / 2;
      for (int i = 0; i < numSamples; i++) {
        sampleBuffer[i] = (int16_t)(((int32_t)sampleBuffer[i] * volumeLevel) / 100);
      }
    }
    i2s.write((uint8_t*)sampleBuffer, bytesRead);
  }
  wavFile.close();
}

// =====================================================
// DRAW 2X FULL-HEIGHT REAL QR CODE (58x58 PIXELS)
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

// =====================================================
// DRAW RIGHT-SIDE DASHBOARD
// =====================================================
void drawRightSideDashboard() {
  u8g2.drawLine(61, 0, 61, 63);

  // 1. TOP ROW: 5 Signal Bars + Operator (Vi 4G / 2G)
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

  // 2. BIG DIGITAL CLOCK
  u8g2.setFont(u8g2_font_fub14_tn);
  char timeStr[10];
  sprintf(timeStr, "%02d%c%02d", rtcHour, colonBlink ? ':' : ' ', rtcMin);
  u8g2.drawStr(65, 28, timeStr);

  // 3. DAY & DATE ROW
  u8g2.setFont(u8g2_font_6x10_tf);
  char dateLine[16];
  sprintf(dateLine, "%s %02d/%02d", getDayName(rtcDay, rtcMonth, rtcYear), rtcDay, rtcMonth);
  u8g2.drawStr(66, 39, dateLine);

  u8g2.drawLine(61, 41, 127, 41);

  // 4. STATUS ROW: Shows Azure Cloud Status or Errors
  u8g2.setFont(u8g2_font_6x10_tf);
  if (!sdReady) {
    u8g2.drawStr(65, 50, "SD ERROR!");
  } else if (!modemOnline) {
    u8g2.drawStr(65, 50, "4G ERROR!");
  } else if (!simInserted) {
    u8g2.drawStr(65, 50, "SIM ERROR!");
  } else {
    if ((rtcSec % 6) < 3) {
      u8g2.drawStr(64, 50, "SCAN & PAY");
    } else {
      u8g2.drawStr(64, 50, azureConnected ? "AZURE: OK" : "AZURE:WAIT");
    }
  }

  // 5. BOTTOM INVERTED BRAND BAR
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

// =====================================================
// MASTER UI RENDERER
// =====================================================
void renderUI(const char* stepMsg = "") {
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

void announcePayment(long amount) {
  lastAmount = amount;
  todayTotal += amount;
  sysState = STATE_PAYMENT_ALERT;
  paymentPopupTimer = millis();
  renderUI();

  Serial.printf("💰 ANNOUNCING PAYMENT [%s]: Rs. %ld\n", currentLang.c_str(), amount);

  playClip(currentLang, "ding");
  delay(50);
  playClip(currentLang, "prefix");
  speakNumber(amount, currentLang);
  playClip(currentLang, "rupaye");
  playClip(currentLang, "prapt_thx");

  paymentPopupTimer = millis();
}

// =====================================================
// SIM A7670C AT COMMAND & AZURE MQTT ENGINE
// =====================================================
String sendATCommand(const char* cmd, unsigned long timeoutMs = 1200) {
  while (MODEM.available()) MODEM.read();
  Serial.printf(">> AT: %s\n", cmd);
  MODEM.println(cmd);
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (MODEM.available()) {
      char c = MODEM.read();
      resp += c;
    }
    if (resp.indexOf("OK\r\n") != -1 || resp.indexOf("ERROR") != -1) break;
  }
  resp.trim();
  if (resp.length() > 0) Serial.printf("<< %s\n", resp.c_str());
  return resp;
}

// Upload Embedded PROGMEM X.509 Certificate to A7670C Modem for Azure mTLS
bool uploadCertToModem(const char* modemCertName, const char* certData) {
  size_t len = strlen_P(certData);
  // Delete old copy if present
  String delCmd = String("AT+CCERTDELE=\"") + modemCertName + "\"";
  sendATCommand(delCmd.c_str(), 500);

  while (MODEM.available()) MODEM.read();
  String cmd = String("AT+CCERTDOWN=\"") + modemCertName + "\"," + String(len);
  Serial.printf(">> AT: %s\n", cmd.c_str());
  MODEM.println(cmd);

  // Wait for '>' prompt from A7670C
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 2000) {
    if (MODEM.available()) {
      char c = MODEM.read();
      if (c == '>') { gotPrompt = true; break; }
    }
  }
  if (!gotPrompt) {
    Serial.printf("⚠️ Failed to get '>' prompt for %s\n", modemCertName);
    return false;
  }

  // Write certificate bytes
  MODEM.write((const uint8_t*)certData, len);

  String resp = "";
  start = millis();
  while (millis() - start < 2000) {
    while (MODEM.available()) resp += (char)MODEM.read();
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
  }
  resp.trim();
  Serial.printf("<< Cert %s: %s\n", modemCertName, resp.c_str());
  return (resp.indexOf("OK") != -1);
}

// Publish Status / ACK back to soundbox/sb-device-001/status
void publishAzureStatus(const String& jsonPayload) {
  if (!azureConnected) return;
  String topic = AZURE_PUB_TOPIC;

  // 1. Set Topic
  String topCmd = String("AT+CMQTTTOPIC=0,") + String(topic.length());
  MODEM.println(topCmd);
  delay(150);
  MODEM.print(topic);
  delay(150);
  while (MODEM.available()) MODEM.read();

  // 2. Set Payload
  String payCmd = String("AT+CMQTTPAYLOAD=0,") + String(jsonPayload.length());
  MODEM.println(payCmd);
  delay(150);
  MODEM.print(jsonPayload);
  delay(150);
  while (MODEM.available()) MODEM.read();

  // 3. Publish with QoS 1
  sendATCommand("AT+CMQTTPUB=0,1,60", 1500);
}

// Connect SIM A7670C to Azure Event Grid MQTT Broker
bool connectAzureMQTT() {
  renderUI("Starting Azure MQTT");
  Serial.println("☁️ Connecting to Azure Event Grid MQTT Broker...");

  // 1. Activate 4G PDP Context
  sendATCommand("AT+CGACT=1,1", 2500);

  // 2. Upload Root CA, Client Cert & Client Key directly from Flash (PROGMEM) to A7670C
  renderUI("Loading mTLS Certs");
  uploadCertToModem("root_ca.pem", AZURE_ROOT_CA);
  uploadCertToModem("client_cert.pem", AZURE_CLIENT_CERT);
  uploadCertToModem("client_key.pem", AZURE_CLIENT_KEY);

  // 3. Configure SSL Context 0 for Azure Event Grid mTLS
  sendATCommand("AT+CSSLCFG=\"sslversion\",0,4", 800);                    // TLS 1.2
  sendATCommand("AT+CSSLCFG=\"authmode\",0,2", 800);                      // Verify Server + Client Cert (mTLS)
  sendATCommand("AT+CSSLCFG=\"cacert\",0,\"root_ca.pem\"", 800);          // DigiCert Global Root G3
  sendATCommand("AT+CSSLCFG=\"clientcert\",0,\"client_cert.pem\"", 800);  // Fixed space typo!
  sendATCommand("AT+CSSLCFG=\"clientkey\",0,\"client_key.pem\"", 800);    // EC P-256 Private Key
  sendATCommand("AT+CSSLCFG=\"enableSNI\",0,1", 800);                     // Required by Azure Event Grid

  // 4. Start A7670C Hardware MQTT Service
  sendATCommand("AT+CMQTTDISC=0,60", 800);
  sendATCommand("AT+CMQTTREL=0", 800);
  sendATCommand("AT+CMQTTSTOP", 800);
  sendATCommand("AT+CMQTTSTART", 2500);

  // 5. Acquire Client Index 0 with SSL Enabled (,1)
  String accqCmd = String("AT+CMQTTACCQ=0,\"") + AZURE_CLIENT_ID + "\",1";
  sendATCommand(accqCmd.c_str(), 1000);
  sendATCommand("AT+CMQTTSSLCFG=0,0", 800);

  // 6. Connect to Azure Event Grid MQTT Broker on Port 8883
  renderUI("Auth Azure Cloud..");
  String connCmd = String("AT+CMQTTCONNECT=0,\"tcp://") + AZURE_MQTT_HOST + ":" + String(AZURE_MQTT_PORT) + "\",60,1,\"" + AZURE_USERNAME + "\"";
  String r = sendATCommand(connCmd.c_str(), 6000);

  // Wait up to 6 seconds for +CMQTTCONNECT: 0,0
  unsigned long waitStart = millis();
  while (millis() - waitStart < 6000 && r.indexOf("+CMQTTCONNECT: 0,0") == -1) {
    while (MODEM.available()) {
      char c = MODEM.read();
      Serial.write(c);
      r += c;
    }
    delay(50);
  }

  if (r.indexOf("+CMQTTCONNECT: 0,0") != -1) {
    Serial.println("\n✅ Connected to Azure Event Grid MQTT Broker!");

    // 7. Subscribe to Isolated Payment Topic: "soundbox/sb-device-001/payments"
    String topic = AZURE_SUB_TOPIC;
    String subCmd = String("AT+CMQTTSUB=0,") + String(topic.length()) + ",1";
    MODEM.println(subCmd);
    delay(300);
    MODEM.print(topic);
    delay(1200);
    while (MODEM.available()) Serial.write(MODEM.read());

    azureConnected = true;
    playClip(currentLang, "mqtt_con");

    // Send Online Heartbeat to soundbox/sb-device-001/status
    publishAzureStatus("{\"device\":\"sb-device-001\",\"status\":\"ONLINE\"}");
    return true;
  }

  Serial.println("\n⚠️ Azure MQTT connection failed! Response: " + r);
  azureConnected = false;
  return false;
}

// Parse Instant Incoming MQTT Push from Azure (+CMQTTRXPayload)
void checkIncomingAzurePush() {
  while (MODEM.available()) {
    char c = MODEM.read();
    Serial.write(c); // Echo live modem push to Serial Monitor
    modemRxBuffer += c;

    // Prevent buffer overflow
    if (modemRxBuffer.length() > 1024) {
      modemRxBuffer = modemRxBuffer.substring(512);
    }

    // Detect if modem dropped MQTT connection
    if (modemRxBuffer.indexOf("+CMQTTCONNLOST:") != -1 || modemRxBuffer.indexOf("+CMQTTNONET") != -1) {
      azureConnected = false;
      modemRxBuffer = "";
    }

    // When A7670C finishes receiving an MQTT push packet (+CMQTTRXEND: 0)
    if (modemRxBuffer.indexOf("+CMQTTRXEND:") != -1) {
      Serial.println("\n⚡ INSTANT AZURE MQTT PUSH RECEIVED!");

      // Extract JSON payload e.g. {"txnId":"...","amount":500.00} or {"amt":250}
      int amtPos = modemRxBuffer.indexOf("\"amount\":");
      if (amtPos == -1) amtPos = modemRxBuffer.indexOf("\"amt\":");

      if (amtPos != -1) {
        int colon = modemRxBuffer.indexOf(":", amtPos);
        String numPart = modemRxBuffer.substring(colon + 1);
        numPart.trim();
        long amt = (long)numPart.toFloat();

        // Check Duplicate Transaction ID ("txnId" or "txn")
        String txnId = "";
        int txnPos = modemRxBuffer.indexOf("\"txnId\":");
        if (txnPos == -1) txnPos = modemRxBuffer.indexOf("\"txn\":");
        if (txnPos != -1) {
          int firstQuote = modemRxBuffer.indexOf("\"", modemRxBuffer.indexOf(":", txnPos));
          if (firstQuote != -1) {
            int endQuote = modemRxBuffer.indexOf("\"", firstQuote + 1);
            if (endQuote != -1) txnId = modemRxBuffer.substring(firstQuote + 1, endQuote);
          }
        }

        if (amt > 0 && (txnId == "" || txnId != lastTxnId)) {
          lastTxnId = txnId;
          announcePayment(amt);

          // Send instant ACK to soundbox/sb-device-001/status
          String ackJson = String("{\"device\":\"sb-device-001\",\"txnId\":\"") + txnId + "\",\"amount\":" + String(amt) + ",\"ack\":\"ANNOUNCED\"}";
          publishAzureStatus(ackJson);
        }
      }
      modemRxBuffer = ""; // Clear buffer after handling
    }
  }
}

void updateSignalAndNetType() {
  String r = sendATCommand("AT", 500);
  if (r.indexOf("OK") == -1) { modemOnline = false; signalCSQ = 0; return; }
  modemOnline = true;

  r = sendATCommand("AT+CSQ", 500);
  int p = r.indexOf("+CSQ: ");
  if (p != -1) {
    signalCSQ = r.substring(p + 6, r.indexOf(",", p)).toInt();
    if (signalCSQ == 99) signalCSQ = 0;
  }

  r = sendATCommand("AT+COPS?", 600);
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
  String r = sendATCommand("AT+CCLK?", 800);
  int idx = r.indexOf("+CCLK: \"");
  if (idx != -1 && r.length() >= idx + 25) {
    String dt = r.substring(idx + 8, idx + 25);
    rtcYear  = 2000 + dt.substring(0, 2).toInt();
    rtcMonth = dt.substring(3, 5).toInt();
    rtcDay   = dt.substring(6, 8).toInt();
    rtcHour  = dt.substring(9, 11).toInt();
    rtcMin   = dt.substring(12, 14).toInt();
    rtcSec   = dt.substring(15, 17).toInt();
  }
}

void connect4GModemFlow() {
  sysState = STATE_4G_CONNECTING;
  renderUI("Starting 4G Modem..");
  playClip(currentLang, "net_start");

  renderUI("Checking A7670C...");
  String r = sendATCommand("AT", 1000);
  if (r.indexOf("OK") != -1) {
    modemOnline = true;
    sendATCommand("ATE0", 500);
    sendATCommand("AT+CTZU=1", 500);

    renderUI("Checking SIM Card..");
    r = sendATCommand("AT+CPIN?", 1200);
    if (r.indexOf("READY") != -1) {
      simInserted = true;
      renderUI("Scanning Tower..");
      for (int i = 0; i < 6; i++) {
        updateSignalAndNetType();
        r = sendATCommand("AT+CEREG?", 600);
        if (r.indexOf(",1") != -1 || r.indexOf(",5") != -1) {
          netRegistered = true;
          break;
        }
        delay(400);
      }

      syncModemClock();
      renderUI("4G Connected!");
      playClip(currentLang, "net_con");

      // Connect to Azure Event Grid MQTT Broker
      connectAzureMQTT();
      playClip(currentLang, "dev_rdy");
    } else {
      simInserted = false;
      drawBootErrorPopup("! SIM CARD ERROR !", "SIM Not Detected", "Insert Active 4G SIM");
      playClip(currentLang, "sim_err");
      delay(2000);
    }
  } else {
    modemOnline = false;
    simInserted = false;
    signalCSQ = 0;
    drawBootErrorPopup("! 4G MODULE ERROR !", "4G Not Connected!", "Check 4V & RX/TX Pin");
    playClip(currentLang, "net_dcon");
    delay(2000);
    playClip(currentLang, "dev_rdy");
  }

  sysState = STATE_READY_QR_CLOCK;
  renderUI();
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
  MODEM.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(MODEM_RST, OUTPUT);
  digitalWrite(MODEM_RST, HIGH);

  delay(500);
  Serial.println("\n=======================================================");
  Serial.println("   🚀 RUPEECO AZURE EVENT GRID MQTT 4G SOUNDBOX");
  Serial.println("=======================================================");

  prefs.begin("rupeeco", false);
  isFirstSetupDone = prefs.getBool("setup_done", false);
  currentLang = prefs.getString("lang", "HIND");
  volumeLevel = prefs.getInt("vol", 100);
  langCursor = (currentLang == "HIND") ? 0 : 1;

  u8g2.begin();
  u8g2.setContrast(255);
  sysState = STATE_BOOT_WELCOME;
  renderUI();

  pinMode(BTN_UP, INPUT_PULLDOWN);
  pinMode(BTN_OK, INPUT_PULLDOWN);
  pinMode(BTN_DOWN, INPUT_PULLDOWN);

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
    connect4GModemFlow();
  }

  lastSecondTick = millis();
  lastSignalPoll = millis();
}

// =====================================================
// MAIN LOOP
// =====================================================
void loop() {
  unsigned long now = millis();

  // 0. Listen continuously for Instant Azure MQTT Push from 4G Modem!
  checkIncomingAzurePush();

  // 1. Auto-return from Payment Popup after 4 seconds
  if (sysState == STATE_PAYMENT_ALERT && (now - paymentPopupTimer > 4000)) {
    sysState = STATE_READY_QR_CLOCK;
    renderUI();
  }

  // 2. Live 1-Second Clock Tick
  if (now - lastSecondTick >= 1000) {
    lastSecondTick = now;
    colonBlink = !colonBlink;
    rtcSec++;
    if (rtcSec >= 60) { rtcSec = 0; rtcMin++; }
    if (rtcMin >= 60) { rtcMin = 0; rtcHour = (rtcHour + 1) % 24; }

    if (!sdReady && SD.begin(SD_CS_PIN, sdSPI, 4000000)) sdReady = true;
    if (sysState == STATE_READY_QR_CLOCK) renderUI();
  }

  // 3. Serial Monitor Input Handler (Local & JSON Simulation)
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
        connectAzureMQTT();
      } else if (cmd.equalsIgnoreCase("RESET")) {
        prefs.clear();
        ESP.restart();
      } else if (cmd.toInt() > 0) {
        announcePayment(cmd.toInt());
      }
    }
  }

  // 4. Button Controls (Language / Menu / Volume / Home)
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
      if (wasFirstSetup) connect4GModemFlow();
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
      if (menuCursor == 0) { langCursor = (currentLang == "HIND") ? 0 : 1; sysState = STATE_LANG_SUBMENU; renderUI(); playClip(currentLang, "lang_menu"); }
      else if (menuCursor == 1) { sysState = STATE_VOLUME_SUBMENU; renderUI(); playClip(currentLang, "menu_vol"); }
      else if (menuCursor == 2) { sysState = STATE_READY_QR_CLOCK; if (lastAmount > 0) { playClip(currentLang, "replay"); announcePayment(lastAmount); } else { renderUI(); playClip(currentLang, "no_txn"); } }
      else if (menuCursor == 3) { connect4GModemFlow(); }
      else if (menuCursor == 4) { triggerPowerOff(); }
      else if (menuCursor == 5) { playClip(currentLang, "menu_exit"); sysState = STATE_READY_QR_CLOCK; renderUI(); }
    }
  }
  else if (sysState == STATE_VOLUME_SUBMENU) {
    if (digitalRead(BTN_UP) == HIGH) { changeVolume(+15); delay(250); }
    else if (digitalRead(BTN_DOWN) == HIGH) { changeVolume(-15); delay(250); }
    else if (digitalRead(BTN_OK) == HIGH) { playClip(currentLang, "Beep"); sysState = STATE_PHONE_MENU; renderUI(); delay(300); }
  }
  else if (sysState == STATE_READY_QR_CLOCK || sysState == STATE_PAYMENT_ALERT) {
    if (digitalRead(BTN_UP) == HIGH) { changeVolume(+15); delay(250); }
    else if (digitalRead(BTN_DOWN) == HIGH) { changeVolume(-15); delay(250); }
    else if (digitalRead(BTN_OK) == HIGH) {
      playClip(currentLang, "Beep");
      menuCursor = 0; menuTopIndex = 0;
      sysState = STATE_PHONE_MENU;
      renderUI();
      playClip(currentLang, "menu_open");
      delay(300);
    }
  }

  delay(20);
}