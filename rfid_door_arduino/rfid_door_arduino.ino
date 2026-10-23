/*
 * RFID door lock - 2 dau doc RC522 (VAO / RA) tren ESP32-S3, Arduino IDE
 *   - Xac thuc the theo danh sach luu trong NVS (Preferences = "EEPROM" cua ESP32)
 *   - Servo MG996R keo/day chot qua banh rang - thanh rang
 *   - Buzzer bao the dung / sai
 *   - Pin < 20% -> thu chot (mo cua) de khong nhot nguoi ben trong
 *   - Web quan tri: them/xoa the, xem lich su checkin/checkout, mo cua tu xa
 *   - (tuy chon) POST tung su kien len server ngoai (LOG_SERVER_URL trong config.h)
 *
 * Board: ESP32S3 Dev Module, core esp32 >= 3.0 (da thu voi 3.3.x)
 * Thu vien ngoai duy nhat: "MFRC522" by GithubCommunity (miguelbalboa)
 * Moi cau hinh (WiFi, chan, xung servo, nguong pin...) nam trong config.h
 *
 * So do noi:
 *   RC522 (x2)   ESP32-S3
 *   3.3V   ->    3V3   (KHONG cap 5V)
 *   GND    ->    GND
 *   MOSI   ->    GPIO11  (chung)
 *   SCK    ->    GPIO12  (chung)
 *   MISO   ->    GPIO13  (chung)
 *   SDA    ->    GPIO10  (dau doc NGOAI -> VAO)
 *   SDA    ->    GPIO9   (dau doc TRONG -> RA)
 *   RST    ->    GPIO17  (dau doc VAO)  - PIN_RST_OUT, dat -1 trong config.h neu noi cung 3.3V
 *   RST    ->    GPIO21  (dau doc RA)   - PIN_RST_IN
 *
 *   MG996R: do -> VBAT 6V (4xAA, KHONG lay tu DevKit), nau -> GND, cam -> R 330R -> GPIO4
 *           + tu 1000uF/10V + 100nF sat dau cam servo
 *   Buzzer: GPIO5 -> R 1k -> Gate AO3400A (100k xuong GND) ; buzzer + 1N4148 song song
 *   LED:    GPIO16 -> 220R -> LED xanh -> GND ; GPIO47 -> 270R -> LED do -> GND
 *   Nut Exit: GPIO6 -> nut -> GND  (10k keo len 3V3 + 100nF xuong GND)
 *   MPR121: SDA GPIO8, SCL GPIO18 (4.7k keo len), IRQ GPIO7 (10k keo len), ADDR -> GND
 *   Pin:    VBAT -> R1 1M -> GPIO1 -> R2 470k -> GND, tu 100nF GPIO1-GND (BAT_DIVIDER = 3.128)
 *   Chan du phong / chan cam: xem cuoi muc chan trong config.h
 *
 * Lenh Serial Monitor (115200, ket thuc bang Enter):
 *   v            in phien ban chip 2 dau doc (kiem tra day)
 *   r            khoi tao lai 2 dau doc
 *   spi          chan doan bus SPI: tach loi nguon / day / chap MISO / toc do
 *   d            bat/tat log chi tiet
 *   lock|unlock  dong/mo chot
 *   servo <us>   xuat xung tuy y (500..2500) de can chinh vi tri chot
 *   bat          in dien ap / % pin
 *   cards        liet ke the
 *   add <uid> [ten] / del <uid>   them / xoa the
 *   rid          bat/tat CHE DO DOC ID THE: ca 2 dau doc chi doc va in UID (khong mo cua),
 *                tu tat sau 60s khong quet. Tren web: nut "Che do doc the"
 *   enroll [ten] bat/tat CHE DO DANG KY 1 CHAM: quet the moi -> tu luu voi [ten] (khong mo cua),
 *                tu tat sau khi them xong / 60s. Tren web: nut "Dang ky the moi"
 *   ip           in dia chi IP
 *   wifi         trang thai WiFi: song, IP, thoi gian mat mang, ly do, so su kien cho gui
 *   wifi scan    quet mang 2.4 GHz xung quanh (tim nguyen nhan khong vao duoc WiFi)
 *   wifi off|on  gia lap mat mang / ket noi lai (test gui bu su kien)
 */

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include "driver/gpio.h"
#include "config.h"
#include "web_page.h"

// ---------------- Tham so doc the ----------------
const uint32_t FIELD_SETTLE_MS  = 10;   // cho the nhan du nang luong sau khi bat anten (ISO14443 >= 5ms; 10ms cho the/anten yeu)
const uint32_t CARD_GONE_MS     = 1000; // khong thay the trong khoang nay = da rut the
const uint32_t CROSS_READ_MS    = 800;  // dau doc ben kia vua thay cung the -> bo qua

// ---------------- Kieu du lieu ----------------
struct Reader {
  MFRC522     rfid;
  const char *name;      // "VAO" / "RA"
  const char *type;      // "checkin" / "checkout" (gui len web)
  String      lastUid;
  uint32_t    lastSeenMs;
  bool        ok;
  int8_t      rstPin;    // chan RST rieng (-1 = RST noi cung len 3V3)
  // Khong giao RST cho thu vien: PCD_Init() cua MFRC522 se doi chan RST thanh INPUT (tha noi)
  // neu dang HIGH -> tu dieu khien RST trong hardReset().
  Reader(uint8_t cs, int8_t rst, const char *n, const char *t)
    : rfid(cs, MFRC522::UNUSED_PIN), name(n), type(t), lastSeenMs(0), ok(false), rstPin(rst) {}
};

// Khai bao o day (truoc moi ham) de prototype Arduino tu sinh nhin thay kieu nay
enum ReaderState { RS_OK, RS_NO_REPLY, RS_GARBAGE, RS_RESET };

struct CardEntry {
  char uid[32];          // "DE:AD:BE:EF" (toi da 10 byte -> 29 ky tu)
  char name[24];
};

struct LogEntry {
  time_t   ts;           // epoch (0 neu chua dong bo NTP)
  uint32_t upSec;        // thoi gian tu luc khoi dong, dung khi chua co NTP
  char     type[10];     // checkin / checkout / system
  char     uid[32];
  char     name[24];
  char     result[12];   // granted / denied / lowbat / ...
  uint32_t offlineSec;   // chi su kien wifi_up: so giay vua mat mang (0 = khong ap dung)
  bool     sent;         // da gui len LOG_SERVER_URL chua
};

Reader readers[2] = {
  Reader(PIN_CS_OUT, PIN_RST_OUT, "VAO", "checkin"),
  Reader(PIN_CS_IN,  PIN_RST_IN,  "RA",  "checkout"),
};

WebServer   server(80);
Preferences prefs;

CardEntry cards[MAX_CARDS];
int       cardCount = 0;

LogEntry  logs[LOG_SIZE];
int       logHead = 0;     // vi tri ghi tiep theo
int       logCount = 0;

bool      verbose = false;

// Theo doi WiFi (ghi trong callback su kien -> chi doc/in trong loop)
volatile bool     wifiUp = false;
volatile bool     wifiChanged = false;
volatile uint8_t  wifiLastReason = 0;   // ly do mat ket noi gan nhat (wifi_err_reason_t)
volatile uint32_t wifiDownAtMs = 0;     // millis() luc mat mang (0 = chua tung mat)
volatile uint32_t wifiUpAtMs = 0;       // millis() luc co IP gan nhat
volatile uint16_t wifiDropCount = 0;
bool      wifiManualOff = false;        // lenh "wifi off" (gia lap mat mang)

// Trang thai khoa
bool      doorLocked = true;
uint32_t  relockAtMs = 0;       // 0 = khong hen gio khoa lai
uint32_t  servoOffAtMs = 0;     // 0 = khong can ngat xung
uint32_t  lastServoMoveMs = 0;

// Pin
float     batV = 0;
int       batPct = 100;
bool      lowBattery = false;
int       lowBatCounter = 0;
uint32_t  lastBatReadMs = 0;

// The vua quet gan nhat (de them nhanh tren web)
String    lastScanUid;
String    lastScanReader;       // "VAO" / "RA"
uint32_t  lastScanMs = 0;
uint32_t  scanSeq = 0;          // tang moi lan quet -> web biet co lan quet moi (ke ca cung 1 the)

// Che do doc ID the: ca 2 dau doc van quet, nhung chi in UID - KHONG mo cua, KHONG ghi denied
const uint32_t READ_MODE_TIMEOUT_MS = 60000;
bool      readMode = false;
uint32_t  readModeUntilMs = 0;

// Che do dang ky 1 cham: the chua dang ky quet vao -> tu luu voi ten enrollName (KHONG mo cua)
bool      enrollMode = false;
String    enrollName;
uint32_t  enrollUntilMs = 0;
String    enrollMsg;            // ket qua lan dang ky gan nhat (hien tren web)
uint32_t  enrollSeq = 0;        // tang moi khi co ket qua moi

// Gui server ngoai
uint32_t  nextSendTryMs = 0;

// =====================================================================
//  Buzzer (khong chan - chay theo mau on/off)
// =====================================================================
const uint16_t BEEP_GRANTED[] = {150, 0};
const uint16_t BEEP_DENIED[]  = {80, 80, 80, 80, 80, 0};
const uint16_t BEEP_LOWBAT[]  = {400, 200, 400, 0};
const uint16_t BEEP_ADDED[]   = {60, 60, 60, 0};
const uint16_t BEEP_READ[]    = {40, 0};

// =====================================================================
//  LED xanh / do - di kem moi am bao (goi trong beep())
//   the dung / Exit / mo tu xa : xanh sang suot thoi gian cua mo
//   the sai / loi              : do sang LED_DENIED_MS
//   the moi da dang ky / doc ID: xanh nhay ngan
//   che do doc the / dang ky   : xanh nhay cham (dang cho quet)
//   pin yeu (cua mo san)       : do chop moi 2s
// =====================================================================
uint32_t  ledGreenUntilMs = 0;
uint32_t  ledRedUntilMs = 0;

void ledWrite(uint8_t pin, bool on) {
  digitalWrite(pin, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

void ledFeedback(const uint16_t *pattern) {
  uint32_t now = millis();
  if (pattern == BEEP_GRANTED)      { ledGreenUntilMs = now + UNLOCK_HOLD_MS; ledRedUntilMs = 0; }
  else if (pattern == BEEP_ADDED)   { ledGreenUntilMs = now + 1000; ledRedUntilMs = 0; }
  else if (pattern == BEEP_READ)    { ledGreenUntilMs = now + 200; }
  else if (pattern == BEEP_DENIED)  { ledRedUntilMs = now + LED_DENIED_MS; ledGreenUntilMs = 0; }
  else if (pattern == BEEP_LOWBAT)  { ledRedUntilMs = now + 1500; }
}

void updateLeds() {
  uint32_t now = millis();
  bool green = (int32_t)(ledGreenUntilMs - now) > 0;
  bool red   = (int32_t)(ledRedUntilMs - now) > 0;
  if (!green && !red) {
    if (lowBattery)                   red = (now % 2000) < 100;     // chop do: pin yeu
    else if (readMode || enrollMode)  green = (now % 1000) < 500;   // nhay xanh: dang cho quet the
  }
  ledWrite(PIN_LED_GREEN, green);
  ledWrite(PIN_LED_RED, red);
}

const uint16_t *beepPattern = nullptr;
int       beepStep = 0;
uint32_t  beepStepAtMs = 0;

void buzzerWrite(bool on) {
  digitalWrite(PIN_BUZZER, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW);
}

void beep(const uint16_t *pattern) {
  beepPattern = pattern;
  beepStep = 0;
  beepStepAtMs = millis();
  buzzerWrite(true);
  ledFeedback(pattern);
}

void updateBuzzer() {
  if (!beepPattern) return;
  if (millis() - beepStepAtMs < beepPattern[beepStep]) return;
  beepStep++;
  beepStepAtMs = millis();
  if (beepPattern[beepStep] == 0) {   // het mau
    buzzerWrite(false);
    beepPattern = nullptr;
    return;
  }
  buzzerWrite(beepStep % 2 == 0);     // buoc chan = keu, buoc le = im
}

// =====================================================================
//  Ban phim cam ung MPR121 (I2C) - hien chi in phim cham ra Serial de kiem tra phan cung.
//  Chua dung de mo cua (nhap ma PIN se lam o buoc sau).
// =====================================================================
bool mprOk = false;
uint16_t mprLastTouched = 0;

void mprWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPR121_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

int mprRead(uint8_t reg) {           // -1 neu khong co thiet bi tra loi
  Wire.beginTransmission(MPR121_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)MPR121_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool mprInit() {
  mprWrite(0x80, 0x63);              // soft reset
  delay(2);
  if (mprRead(0x5D) != 0x24) return false;   // CONFIG2 = 0x24 sau reset -> dung la MPR121
  mprWrite(0x5E, 0x00);              // ECR: dung do de cau hinh
  for (uint8_t i = 0; i < 12; i++) {
    mprWrite(0x41 + i * 2, 12);      // nguong cham
    mprWrite(0x42 + i * 2, 6);       // nguong nha
  }
  // Bo loc baseline (gia tri khuyen nghi cua NXP AN3944)
  const uint8_t filt[][2] = {{0x2B, 0x01}, {0x2C, 0x01}, {0x2D, 0x0E}, {0x2E, 0x00},
                             {0x2F, 0x01}, {0x30, 0x05}, {0x31, 0x01}, {0x32, 0x00},
                             {0x33, 0x00}, {0x34, 0x00}, {0x35, 0x00}};
  for (auto &f : filt) mprWrite(f[0], f[1]);
  mprWrite(0x5B, 0x00);              // debounce
  mprWrite(0x5C, 0x10);              // CONFIG1: dong sac 16uA
  mprWrite(0x5D, 0x20);              // CONFIG2: 0.5us, chu ky lay mau 1ms
  // Tu dong hieu chinh cho nguon 3.3V
  mprWrite(0x7D, 200); mprWrite(0x7E, 130); mprWrite(0x7F, 180);
  mprWrite(0x7B, 0x0B);
  mprWrite(0x5E, 0x8C);              // ECR: chay, bat 12 dien cuc, giu baseline
  return true;
}

void setupKeypad() {
  if (!MPR121_ENABLE) return;
  pinMode(PIN_MPR121_IRQ, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  mprOk = mprInit();
  Serial.printf("[KEY] MPR121 (SDA=%d SCL=%d IRQ=%d): %s\n", PIN_I2C_SDA, PIN_I2C_SCL, PIN_MPR121_IRQ,
                mprOk ? "OK" : "KHONG TIM THAY - bo qua ban phim");
}

void updateKeypad() {
  if (!mprOk || digitalRead(PIN_MPR121_IRQ) == HIGH) return;   // IRQ = LOW khi co thay doi
  int lo = mprRead(0x00), hi = mprRead(0x01);
  if (lo < 0 || hi < 0) return;
  uint16_t touched = ((hi << 8) | lo) & 0x0FFF;
  uint16_t pressed = touched & ~mprLastTouched;                 // chi lay phim vua cham
  mprLastTouched = touched;
  for (uint8_t i = 0; i < 12; i++) {
    if (pressed & (1 << i)) {
      Serial.printf("[KEY] cham phim '%c' (ELE%d)\n", MPR121_KEYMAP[i], i);
      beep(BEEP_READ);
    }
  }
}

// =====================================================================
//  Servo MG996R (LEDC 50Hz, 14 bit)
// =====================================================================
const uint32_t SERVO_FREQ = 50;
const uint8_t  SERVO_RES  = 14;
#if ESP_ARDUINO_VERSION_MAJOR < 3
const uint8_t  SERVO_CH   = 0;
#endif

void servoInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_SERVO, SERVO_FREQ, SERVO_RES);
#else
  ledcSetup(SERVO_CH, SERVO_FREQ, SERVO_RES);
  ledcAttachPin(PIN_SERVO, SERVO_CH);
#endif
}

void servoWriteDuty(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_SERVO, duty);
#else
  ledcWrite(SERVO_CH, duty);
#endif
}

// Xuat xung us (500..2500). Neu SERVO_RELEASE, tu ngat xung sau SERVO_MOVE_MS.
void servoWriteUs(uint32_t us) {
  us = constrain(us, 500, 2500);
  servoWriteDuty(us * ((1UL << SERVO_RES) - 1) / (1000000UL / SERVO_FREQ));
  lastServoMoveMs = millis();
  servoOffAtMs = SERVO_RELEASE ? (millis() + SERVO_MOVE_MS) | 1 : 0;   // |1 de khong bao gio = 0
}

void updateServo() {
  if (servoOffAtMs && (int32_t)(millis() - servoOffAtMs) >= 0) {
    servoWriteDuty(0);   // khong xung -> servo tha long, khong tieu dong
    servoOffAtMs = 0;
  }
}

// =====================================================================
//  Thoi gian
// =====================================================================
bool timeValid() { return time(nullptr) > 1700000000; }

String formatTime(time_t ts, uint32_t upSec) {
  if (ts > 1700000000) {
    struct tm t;
    localtime_r(&ts, &t);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
  }
  return "uptime+" + String(upSec) + "s";
}

// =====================================================================
//  Log + gui server ngoai
// =====================================================================
void addLog(const char *type, const String &uid, const char *name, const char *result) {
  addLogAt(type, uid, name, result, millis(), 0);
}

// Ghi su kien xay ra tai thoi diem atMs (millis) - dung cho moc mat / co mang lai,
// vi su kien WiFi duoc xu ly trong loop sau khi da xay ra.
void addLogAt(const char *type, const String &uid, const char *name, const char *result,
              uint32_t atMs, uint32_t offlineSec) {
  LogEntry &e = logs[logHead];
  uint32_t agoSec = (millis() - atMs) / 1000;
  e.ts = timeValid() ? time(nullptr) - (time_t)agoSec : 0;
  e.upSec = atMs / 1000;
  e.offlineSec = offlineSec;
  strlcpy(e.type, type, sizeof(e.type));
  strlcpy(e.uid, uid.c_str(), sizeof(e.uid));
  strlcpy(e.name, name, sizeof(e.name));
  strlcpy(e.result, result, sizeof(e.result));
  e.sent = (strlen(LOG_SERVER_URL) == 0);   // khong co server -> coi nhu da gui
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;

  Serial.printf("[LOG] %s | %-8s | %-14s | %-12s | %s\n",
                formatTime(e.ts, e.upSec).c_str(), type, uid.c_str(), name, result);
}

// Chi so cua log thu i tinh tu cu nhat (i = 0..logCount-1)
int logIndex(int i) { return (logHead - logCount + i + LOG_SIZE) % LOG_SIZE; }

String logToJson(const LogEntry &e) {
  String s = "{\"device\":\"" DEVICE_ID "\",\"time\":\"" + formatTime(e.ts, e.upSec) +
             "\",\"ts\":" + String((uint32_t)e.ts) +
             ",\"type\":\"" + e.type + "\",\"uid\":\"" + e.uid +
             "\",\"name\":\"" + e.name + "\",\"result\":\"" + e.result + "\"";
  if (e.offlineSec) {   // wifi_up: kem luc mat mang + luc co mang lai de web hien ro khoang mat mang
    time_t lostTs = e.ts ? e.ts - (time_t)e.offlineSec : 0;
    s += ",\"lost_at\":\"" + formatTime(lostTs, e.upSec - e.offlineSec) +
         "\",\"lost_ts\":" + String((uint32_t)lostTs) +
         ",\"connected_at\":\"" + formatTime(e.ts, e.upSec) +
         "\",\"offline_sec\":" + String(e.offlineSec);
  }
  s += "}";
  return s;
}

// Gui toi da 1 su kien moi vong loop, cu nhat truoc; loi thi thu lai sau 10s.
void updateUploader() {
  if (strlen(LOG_SERVER_URL) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  // Vua co mang lai: cho NTP bu gio that cho su kien luc mat mang (toi da 30s) roi moi gui
  if (!timeValid() && millis() - wifiUpAtMs < 30000) return;
  if ((int32_t)(millis() - nextSendTryMs) < 0) return;

  for (int i = 0; i < logCount; i++) {
    LogEntry &e = logs[logIndex(i)];
    if (e.sent) continue;

    HTTPClient http;
    http.setConnectTimeout(1500);   // server tat -> khong treo vong quet the qua lau
    http.setTimeout(1500);
    http.begin(LOG_SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(logToJson(e));
    http.end();

    if (code >= 200 && code < 300) {
      e.sent = true;
    } else {
      if (verbose) Serial.printf("  [dbg] gui server loi: %d\n", code);
      nextSendTryMs = millis() + 10000;
    }
    return;
  }
}

// =====================================================================
//  Danh sach the (NVS)
// =====================================================================
// Chuan hoa UID: "deadbeef" / "DE AD BE EF" / "de:ad:be:ef" -> "DE:AD:BE:EF". Tra "" neu sai.
String normalizeUid(String in) {
  String hex;
  in.toUpperCase();
  for (char c : in) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex += c;
    else if (c != ':' && c != ' ' && c != '-') return "";
  }
  int n = hex.length() / 2;
  if (hex.length() % 2 || (n != 4 && n != 7 && n != 10)) return "";
  String out;
  for (int i = 0; i < n; i++) {
    out += hex.substring(i * 2, i * 2 + 2);
    if (i < n - 1) out += ':';
  }
  return out;
}

// Ten: bo ky tu pha JSON/HTML va ky tu phan cach luu tru ( , ; " \ < > )
String sanitizeName(const String &in) {
  String out;
  for (char c : in) {
    if ((uint8_t)c < 0x20 || strchr(",;\"\\<>", c)) continue;
    out += c;
  }
  out.trim();
  // Cat theo byte nhung khong cat giua 1 ky tu UTF-8 (tieng Viet co dau = 2-3 byte)
  unsigned int maxLen = sizeof(CardEntry::name) - 1;
  if (out.length() > maxLen) {
    while (maxLen > 0 && ((uint8_t)out[maxLen] & 0xC0) == 0x80) maxLen--;
    out = out.substring(0, maxLen);
  }
  return out;
}

// Luu dang "UID,ten;UID,ten;..." trong 1 key NVS. Tra false neu ghi loi (vd vuot ~4000 byte)
bool saveCards() {
  String s;
  for (int i = 0; i < cardCount; i++) {
    s += cards[i].uid; s += ','; s += cards[i].name; s += ';';
  }
  return prefs.putString("cards", s) == s.length();
}

void loadCards() {
  String s = prefs.getString("cards", "");
  cardCount = 0;
  int start = 0;
  while (start < (int)s.length() && cardCount < MAX_CARDS) {
    int semi = s.indexOf(';', start);
    if (semi < 0) break;
    String item = s.substring(start, semi);
    int comma = item.indexOf(',');
    if (comma > 0) {
      strlcpy(cards[cardCount].uid, item.substring(0, comma).c_str(), sizeof(CardEntry::uid));
      strlcpy(cards[cardCount].name, item.substring(comma + 1).c_str(), sizeof(CardEntry::name));
      cardCount++;
    }
    start = semi + 1;
  }
  Serial.printf("Da nap %d the tu NVS\n", cardCount);
}

int findCard(const String &uid) {
  for (int i = 0; i < cardCount; i++)
    if (uid.equals(cards[i].uid)) return i;
  return -1;
}

// Tra ve "" neu thanh cong, nguoc lai la thong bao loi
String addCard(const String &rawUid, const String &rawName) {
  String uid = normalizeUid(rawUid);
  if (uid.isEmpty()) return "UID khong hop le (4/7/10 byte hex)";
  if (findCard(uid) >= 0) return "The da ton tai";
  if (cardCount >= MAX_CARDS) return "Danh sach day";
  String name = sanitizeName(rawName);
  if (name.isEmpty()) name = "Khong ten";
  strlcpy(cards[cardCount].uid, uid.c_str(), sizeof(CardEntry::uid));
  strlcpy(cards[cardCount].name, name.c_str(), sizeof(CardEntry::name));
  cardCount++;
  if (!saveCards()) {
    cardCount--;
    return "Loi ghi NVS (bo nho day?)";
  }
  addLog("system", uid, name.c_str(), "card_added");
  beep(BEEP_ADDED);
  return "";
}

String deleteCard(const String &rawUid) {
  String uid = normalizeUid(rawUid);
  int idx = findCard(uid);
  if (idx < 0) return "Khong tim thay the";
  String name = cards[idx].name;
  for (int i = idx; i < cardCount - 1; i++) cards[i] = cards[i + 1];
  cardCount--;
  if (!saveCards()) Serial.println("[NVS] Loi ghi khi xoa the");
  addLog("system", uid, name.c_str(), "card_deleted");
  return "";
}

// =====================================================================
//  Khoa cua
// =====================================================================
void lockDoor() {
  if (lowBattery) return;              // pin yeu: KHONG cho khoa
  servoWriteUs(SERVO_LOCK_US);
  doorLocked = true;
  relockAtMs = 0;
  Serial.println("[DOOR] KHOA");
}

// holdMs = 0: mo va giu mo (khong tu khoa lai)
void unlockDoor(uint32_t holdMs) {
  servoWriteUs(SERVO_UNLOCK_US);
  doorLocked = false;
  relockAtMs = holdMs ? (millis() + holdMs) | 1 : 0;
  Serial.println("[DOOR] MO");
}

void updateDoor() {
  if (relockAtMs && (int32_t)(millis() - relockAtMs) >= 0) {
    relockAtMs = 0;
    lockDoor();
  }
}

// ---------------- Nut Exit ----------------
// Luon hoat dong (ke ca dang o che do rid / dang ky the) - day la loi thoat an toan.
// Nhan giu cung chi mo 1 lan; moi lan nhan gia han lai thoi gian mo.
void updateExitButton() {
  static bool     stable = HIGH, lastRaw = HIGH;
  static uint32_t changedMs = 0;
  bool raw = digitalRead(PIN_EXIT_BTN);
  if (raw != lastRaw) { lastRaw = raw; changedMs = millis(); }
  if (millis() - changedMs < EXIT_DEBOUNCE_MS || raw == stable) return;

  stable = raw;
  if (stable == LOW) {                 // vua nhan xuong
    addLog("checkout", "", "EXIT", "exit_button");
    beep(BEEP_GRANTED);
    if (!lowBattery) unlockDoor(UNLOCK_HOLD_MS);   // pin yeu thi cua da mo san
  }
}

// =====================================================================
//  Pin
// =====================================================================
float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);   // da hieu chinh eFuse
  return (sum / 16.0f) / 1000.0f * BAT_DIVIDER;
}

int voltageToPercent(float v) {
  float p = (v - BAT_V_EMPTY) / (BAT_V_FULL - BAT_V_EMPTY) * 100.0f;
  return constrain((int)p, 0, 100);
}

void updateBattery(bool force) {
  if (!BAT_MONITOR_ENABLE) return;
  if (!force && millis() - lastBatReadMs < 2000) return;
  // Servo dang chay keo sut ap -> bo qua lan doc nay
  if (!force && millis() - lastServoMoveMs < SERVO_MOVE_MS + 500) return;
  lastBatReadMs = millis();

  float v = readBatteryVoltage();
  batV = (batV == 0 || force) ? v : batV * 0.7f + v * 0.3f;   // loc nhieu
  batPct = voltageToPercent(batV);

  if (!lowBattery) {
    lowBatCounter = (batPct < BAT_LOW_PCT) ? lowBatCounter + 1 : 0;
    if (lowBatCounter >= BAT_LOW_CONFIRM || (force && batPct < BAT_LOW_PCT)) {
      lowBattery = true;
      Serial.printf("[BAT] PIN YEU %.2fV (%d%%) -> THU CHOT, giu cua mo\n", batV, batPct);
      unlockDoor(0);                  // mo vinh vien den khi sac lai
      addLog("system", "", "", "lowbat_open");
      beep(BEEP_LOWBAT);
    }
  } else if (batPct > BAT_RECOVER_PCT) {
    lowBattery = false;
    lowBatCounter = 0;
    Serial.printf("[BAT] Pin da hoi %.2fV (%d%%) -> khoa lai binh thuong\n", batV, batPct);
    addLog("system", "", "", "bat_ok");
    lockDoor();
  }
}

// =====================================================================
//  RC522
// =====================================================================
String uidToString(const MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += '0';
    s += String(uid.uidByte[i], HEX);
    if (i < uid.size - 1) s += ':';
  }
  s.toUpperCase();
  return s;
}

// Kiem tra day noi: 0x91/0x92 = chip NXP that, 0x88/0x12/0xB2 = ban clone (van dung duoc)
// 0x00 hoac 0xFF = KHONG giao tiep duoc -> kiem tra lai day, nguon 3.3V, chan CS
bool knownVersion(byte v) {
  return v == 0x88 || v == 0x90 || v == 0x91 || v == 0x92 || v == 0x12 || v == 0xB2;
}

// Trang thai 1 dau doc: doc VersionReg + doc lai TPrescalerReg (PCD_Init ghi 0xA9).
// Doc lai dung 0xA9 = SPI doc/ghi chuan -> tin duoc, ke ca chip clone co ma la.
ReaderState readerState(Reader &r, byte &v) {
  v = r.rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (v == 0x00 || v == 0xFF) return RS_NO_REPLY;
  if (r.rfid.PCD_ReadRegister(MFRC522::TPrescalerReg) == 0xA9) return RS_OK;
  return knownVersion(v) ? RS_RESET : RS_GARBAGE;
}

const char *readerStateText(ReaderState s) {
  switch (s) {
    case RS_OK:       return "OK";
    case RS_NO_REPLY: return "KHONG TRA LOI - kiem tra nguon 3V3/GND, day SDA(CS), MISO";
    case RS_GARBAGE:  return "DU LIEU SPI RAC - bus nhieu / chip kia tranh MISO / tiep xuc kem SCK-MOSI-MISO";
    default:          return "BI RESET (mat cau hinh) - sut ap / RST chap chon";
  }
}

bool checkReader(Reader &r) {
  byte v;
  ReaderState s = readerState(r, v);
  r.ok = (s == RS_OK);
  Serial.printf("[%s] VersionReg = 0x%02X -> %s%s\n", r.name, v, readerStateText(s),
                r.ok && !knownVersion(v) ? " (ma chip la)" : "");
  return r.ok;
}

// Reset cung qua chan NRSTPD: LOW = power-down/reset, HIGH = chay. Thoat power-down
// -> chip tu reset, cho thach anh khoi dong (~38us theo datasheet, cho 50ms cho chac).
// Day la cach DUY NHAT cuu RC522 khi bi treo ma soft reset qua SPI khong vao duoc.
void hardReset(Reader &r) {
  if (r.rstPin < 0) return;
  pinMode(r.rstPin, OUTPUT);
  digitalWrite(r.rstPin, LOW);
  delay(2);
  digitalWrite(r.rstPin, HIGH);
  delay(50);
}

void initReader(Reader &r) {
  hardReset(r);
  r.rfid.PCD_Init();
  r.rfid.PCD_SetAntennaGain(MFRC522::RxGain_max); // xuyen kinh can gain cao; giam neu doc cheo nhieu
  r.rfid.PCD_AntennaOff();                         // tat, chi bat khi den luot quet
}

void initReaders() {
  // Keo CS cua CA 2 dau doc len HIGH truoc khi khoi tao bat ky con nao.
  // Neu khong, luc init dau doc VAO thi CS cua dau doc RA (GPIO9) dang tha noi ->
  // ca 2 cung tra loi tren MISO -> doc sai thanh ghi, cau hinh hong.
  pinMode(PIN_CS_OUT, OUTPUT); digitalWrite(PIN_CS_OUT, HIGH);
  pinMode(PIN_CS_IN,  OUTPUT); digitalWrite(PIN_CS_IN,  HIGH);
  for (auto &r : readers)        // RST xac dinh muc HIGH ngay tu dau, khong de tha noi
    if (r.rstPin >= 0) { pinMode(r.rstPin, OUTPUT); digitalWrite(r.rstPin, HIGH); }
  delay(5);
  for (auto &r : readers) {
    initReader(r);
    checkReader(r);
  }
}

// ---------------- Chan doan bus SPI (lenh "spi") ----------------
// Doc VersionReg truc tiep, lan luot bat dien tro keo LEN / keo XUONG tren MISO:
//  - Ket qua doi theo dien tro keo (0xFF khi keo len, 0x00 khi keo xuong) -> RC522 KHONG tra loi
//    (MISO tha noi): chua cap nguon, RST thap/tha noi, sai day SCK/MOSI/CS, hoac dut day MISO.
//  - Ket qua giu nguyen 0xFF/0x00 du keo nguoc lai -> MISO bi chap len 3V3 / xuong GND.
//  - Ra gia tri hop le o 500 kHz nhung loi o 4 MHz -> day qua dai / tiep xuc kem.
byte rawReadVersion(uint8_t cs, uint32_t hz) {
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  SPI.transfer(0x80 | MFRC522::VersionReg);   // bit7 = doc; dia chi trong thu vien da dich trai 1
  byte v = SPI.transfer(0x00);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
  return v;
}

void spiDiag() {
  Serial.println("=== CHAN DOAN SPI (MISO=GPIO13, SCK=12, MOSI=11) ===");
  digitalWrite(PIN_CS_OUT, HIGH);
  digitalWrite(PIN_CS_IN, HIGH);
  const gpio_num_t miso = (gpio_num_t)PIN_SPI_MISO;
  byte res[2][2][2];   // [dau doc][keo len/xuong][500k/4M]
  for (int pull = 0; pull < 2; pull++) {
    gpio_set_pull_mode(miso, pull == 0 ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    delay(2);
    Serial.printf("MISO nghi (khong chon dau doc), keo %s: muc %d\n",
                  pull == 0 ? "LEN  " : "XUONG", digitalRead(PIN_SPI_MISO));
    for (int i = 0; i < 2; i++) {
      uint8_t cs = (i == 0) ? PIN_CS_OUT : PIN_CS_IN;
      res[i][pull][0] = rawReadVersion(cs, 500000);
      res[i][pull][1] = rawReadVersion(cs, 4000000);
    }
  }
  // Ai dang giu MISO khi khong chon? Cho tung chip ngu bang RST (power-down -> MISO tha noi)
  if (PIN_RST_OUT >= 0 && PIN_RST_IN >= 0) {
    gpio_set_pull_mode(miso, GPIO_PULLUP_ONLY);
    const char *lbl[] = {"ca 2 chip NGU (RST=LOW)", "chi VAO chay      ", "chi RA chay       ", "ca 2 chip chay    "};
    int lvl[4];
    for (int m = 0; m < 4; m++) {
      digitalWrite(PIN_RST_OUT, (m == 1 || m == 3) ? HIGH : LOW);
      digitalWrite(PIN_RST_IN,  (m == 2 || m == 3) ? HIGH : LOW);
      delay(60);
      lvl[m] = digitalRead(PIN_SPI_MISO);
      Serial.printf("MISO nghi, keo LEN, %s: muc %d\n", lbl[m], lvl[m]);
    }
    if (lvl[0] == 0)
      Serial.println("   -> Ca 2 chip ngu ma MISO van LOW: MISO chap GND / noi nham day, HOAC module CHUA CO NGUON 3V3 (diode ghim MISO xuong)");
    else {
      if (lvl[1] == 0) Serial.println("   -> Chip VAO giu MISO khi khong duoc chon: day SDA cua VAO chua toi GPIO10");
      if (lvl[2] == 0) Serial.println("   -> Chip RA giu MISO khi khong duoc chon: day SDA cua RA chua toi GPIO9");
      if (lvl[1] && lvl[2]) Serial.println("   -> Khong chip nao giu bus khi nghi");
    }
  }
  gpio_set_pull_mode(miso, GPIO_FLOATING);

  for (int i = 0; i < 2; i++) {
    Reader &r = readers[i];
    Serial.printf("[%s] CS=GPIO%d RST=%s | keo LEN: 500k=0x%02X 4M=0x%02X | keo XUONG: 500k=0x%02X 4M=0x%02X\n",
                  r.name, i == 0 ? PIN_CS_OUT : PIN_CS_IN,
                  r.rstPin >= 0 ? ("GPIO" + String(r.rstPin)).c_str() : "3V3",
                  res[i][0][0], res[i][0][1], res[i][1][0], res[i][1][1]);
    if (knownVersion(res[i][0][1]) && knownVersion(res[i][1][1]))
      Serial.println("   -> OK: chip tra loi binh thuong");
    else if (knownVersion(res[i][0][0]) || knownVersion(res[i][1][0]))
      Serial.println("   -> Chi chay o toc do thap: day qua dai / tiep xuc kem / breadboard -> rut ngan, ep chat day");
    else if (res[i][0][0] == 0xFF && res[i][1][0] == 0x00) {
      Serial.println("   -> RC522 KHONG tra loi (MISO tha noi): kiem tra 3V3+GND tai module, RST=3.3V, day SCK/MOSI/SDA, day MISO");
      if (res[i][0][1] == res[i][1][1] && res[i][0][1] != 0x00 && res[i][0][1] != 0xFF)
        Serial.printf("      (gia tri 0x%02X o 4MHz la NHIEU xuyen tu SCK sang day MISO dang ho, khong phai chip)\n", res[i][0][1]);
    }
    else if (res[i][1][0] == 0xFF)
      Serial.println("   -> MISO bi keo cung len 3V3 (chap / noi nham MISO vao 3V3 hoac chan khac)");
    else if (res[i][0][0] == 0x00)
      Serial.println("   -> MISO bi keo cung xuong GND (chap / noi nham, hoac 1 module khac giu bus)");
    else
      Serial.println("   -> Ket qua la - chup lai log gui ky thuat");
  }
  initReaders();   // tra bus ve trang thai binh thuong
}

// Moi 5s kiem tra 2 dau doc. RC522 hay bi treo/reset khi servo chay (sut ap, nhieu tren 3.3V)
// hoac long day: khi do thanh ghi ve mac dinh va dau doc "im lang" -> tu khoi tao lai.
void checkReadersHealth() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 5000) return;
  lastMs = millis();

  for (auto &r : readers) {
    byte v;
    ReaderState s = readerState(r, v);
    if (s == RS_OK) {
      if (!r.ok) Serial.printf("[%s] Dau doc da hoat dong lai (0x%02X)\n", r.name, v);
      r.ok = true;
      continue;
    }
    // Chi in khi vua chuyen tu OK -> loi, tranh lap log moi 5s
    if (r.ok) Serial.printf("[%s] VersionReg=0x%02X -> %s -> khoi tao lai\n", r.name, v, readerStateText(s));
    initReader(r);
    r.ok = (readerState(r, v) == RS_OK);   // phai doc lai dung cau hinh moi coi la song
  }
}

// Quet 1 dau doc (bat anten -> doc -> tat anten).
// Chi 1 anten phat tai 1 thoi diem -> giam nhieu cheo giua 2 mat kinh.
bool scanOnce(Reader &r, String &uidOut) {
  if (!r.ok) return false;
  r.rfid.PCD_AntennaOn();
  delay(FIELD_SETTLE_MS);

  bool found = false;
  if (r.rfid.PICC_IsNewCardPresent() && r.rfid.PICC_ReadCardSerial()) {
    uidOut = uidToString(r.rfid.uid);
    found = true;
    if (verbose) {
      MFRC522::PICC_Type t = r.rfid.PICC_GetType(r.rfid.uid.sak);
      Serial.printf("  [dbg] %s thay %s (", r.name, uidOut.c_str());
      Serial.print(r.rfid.PICC_GetTypeName(t));   // tra ve chuoi F(), khong dung %s duoc
      Serial.println(")");
    }
    r.rfid.PICC_HaltA();
  }

  r.rfid.PCD_AntennaOff();
  return found;
}

// ---------------- Che do doc ID the ----------------
void setEnrollMode(bool on, const String &name = "");

void setReadMode(bool on) {
  if (on && enrollMode) setEnrollMode(false);   // 2 che do loai tru nhau
  readMode = on;
  readModeUntilMs = millis() + READ_MODE_TIMEOUT_MS;
  Serial.println(on ? "[RID] BAT che do doc ID the - quet the vao 1 trong 2 dau doc (cua KHONG mo)"
                    : "[RID] TAT che do doc ID the - tro lai kiem soat ra vao");
}

void setEnrollMode(bool on, const String &name) {
  if (on && readMode) setReadMode(false);
  enrollMode = on;
  enrollUntilMs = millis() + READ_MODE_TIMEOUT_MS;
  if (on) {
    enrollName = sanitizeName(name);
    if (enrollName.isEmpty()) enrollName = "Khong ten";
    Serial.printf("[ENR] BAT dang ky the cho \"%s\" - quet the moi (cua KHONG mo)\n", enrollName.c_str());
  } else {
    Serial.println("[ENR] TAT che do dang ky the");
  }
}

void setEnrollResult(const String &m) {
  enrollMsg = m;
  enrollSeq++;
  Serial.println("[ENR] " + m);
}

void updateReadMode() {
  if (readMode && (int32_t)(millis() - readModeUntilMs) >= 0) {
    Serial.println("[RID] Het 60s khong quet -> tu tat");
    setReadMode(false);
  }
  if (enrollMode && (int32_t)(millis() - enrollUntilMs) >= 0) {
    setEnrollMode(false);
    setEnrollResult("Het thoi gian cho quet the");
  }
}

void printReadId(Reader &r, const String &uid, int idx) {
  MFRC522::PICC_Type t = r.rfid.PICC_GetType(r.rfid.uid.sak);
  Serial.printf("[RID] %-3s | UID %s | %d byte | ", r.name, uid.c_str(), r.rfid.uid.size);
  Serial.print(r.rfid.PICC_GetTypeName(t));
  if (idx >= 0) Serial.printf(" | DA DANG KY: %s\n", cards[idx].name);
  else Serial.printf(" | CHUA DANG KY -> go: add %s <ten>\n", uid.c_str());
}

// ---------------- Xu ly su kien the ----------------
void onCardEvent(Reader &r, const String &uid) {
  lastScanUid = uid;
  lastScanReader = r.name;
  lastScanMs = millis();
  scanSeq++;

  int idx = findCard(uid);
  if (enrollMode) {                     // dang ky 1 cham, khong mo cua
    if (idx >= 0) {                     // the da co -> bao loi, van cho the khac
      beep(BEEP_DENIED);
      enrollUntilMs = millis() + READ_MODE_TIMEOUT_MS;
      setEnrollResult("The " + uid + " da dang ky: " + cards[idx].name);
      return;
    }
    String err = addCard(uid, enrollName);   // tu luu NVS, ghi log, keu BEEP_ADDED
    String name = enrollName;
    setEnrollMode(false);
    if (err.isEmpty()) {
      setEnrollResult("Da them " + uid + " (" + name + ")");
    } else {
      beep(BEEP_DENIED);
      setEnrollResult("Loi: " + err);
    }
    return;
  }
  if (readMode) {                       // chi doc ID, khong mo cua / khong ghi log ra vao
    printReadId(r, uid, idx);
    beep(BEEP_READ);
    readModeUntilMs = millis() + READ_MODE_TIMEOUT_MS;   // con quet thi con giu che do
    return;
  }
  if (idx >= 0) {
    addLog(r.type, uid, cards[idx].name, "granted");
    beep(BEEP_GRANTED);
    if (!lowBattery) unlockDoor(UNLOCK_HOLD_MS);   // pin yeu thi cua da mo san
  } else {
    addLog(r.type, uid, "", "denied");
    beep(BEEP_DENIED);
  }
}

void handleDetection(int idx, const String &uid) {
  Reader &me = readers[idx];
  Reader &other = readers[1 - idx];
  uint32_t now = millis();

  // 1) Doc cheo: dau doc ben kia dang "giu" the nay -> bo qua
  if (other.lastUid == uid && now - other.lastSeenMs < CROSS_READ_MS) {
    if (verbose) Serial.printf("  [dbg] bo qua doc cheo o %s\n", me.name);
    return;
  }

  // 2) The van dang ap vao (chua rut ra) -> khong bao lai
  bool sameCardStillThere = (me.lastUid == uid) && (now - me.lastSeenMs < CARD_GONE_MS);
  me.lastUid = uid;
  me.lastSeenMs = now;
  if (sameCardStillThere) return;

  onCardEvent(me, uid);
}

// =====================================================================
//  Web server
// =====================================================================
bool requireAuth() {
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                              : WiFi.softAPIP().toString() + " (AP)";
  int idx = lastScanUid.isEmpty() ? -1 : findCard(lastScanUid);
  String s = "{\"locked\":" + String(doorLocked ? "true" : "false") +
             ",\"lowBat\":" + String(lowBattery ? "true" : "false") +
             ",\"batEnabled\":" + String(BAT_MONITOR_ENABLE ? "true" : "false") +
             ",\"batV\":" + String(batV, 2) +
             ",\"batPct\":" + String(batPct) +
             ",\"time\":\"" + formatTime(timeValid() ? time(nullptr) : 0, millis() / 1000) +
             "\",\"ip\":\"" + ip +
             "\",\"lastUid\":\"" + lastScanUid +
             "\",\"lastReader\":\"" + lastScanReader +
             "\",\"lastName\":\"" + (idx >= 0 ? String(cards[idx].name) : String("")) +
             "\",\"lastAgo\":" + String(lastScanUid.isEmpty() ? 0 : (millis() - lastScanMs) / 1000) +
             ",\"scanSeq\":" + String(scanSeq) +
             ",\"readMode\":" + String(readMode ? "true" : "false") +
             ",\"readLeft\":" + String(readMode ? (int32_t)(readModeUntilMs - millis()) / 1000 : 0) +
             ",\"enroll\":" + String(enrollMode ? "true" : "false") +
             ",\"enrollName\":\"" + enrollName +
             "\",\"enrollLeft\":" + String(enrollMode ? (int32_t)(enrollUntilMs - millis()) / 1000 : 0) +
             ",\"enrollMsg\":\"" + enrollMsg +
             "\",\"enrollSeq\":" + String(enrollSeq) + "}";
  server.send(200, "application/json", s);
}

void handleReadMode() {
  if (!requireAuth()) return;
  setReadMode(server.arg("on") == "1");
  server.send(200, "text/plain", "OK");
}

void handleEnroll() {
  if (!requireAuth()) return;
  bool on = server.arg("on") == "1";
  if (on && cardCount >= MAX_CARDS) {
    server.send(400, "text/plain; charset=utf-8", "Danh sach day");
    return;
  }
  setEnrollMode(on, server.arg("name"));
  server.send(200, "text/plain", "OK");
}

void handleCardsGet() {
  if (!requireAuth()) return;
  String s = "[";
  for (int i = 0; i < cardCount; i++) {
    if (i) s += ',';
    s += "{\"uid\":\"" + String(cards[i].uid) + "\",\"name\":\"" + String(cards[i].name) + "\"}";
  }
  s += "]";
  server.send(200, "application/json", s);
}

void handleCardsAdd() {
  if (!requireAuth()) return;
  String err = addCard(server.arg("uid"), server.arg("name"));
  if (err.isEmpty()) server.send(200, "text/plain", "OK");
  else server.send(400, "text/plain; charset=utf-8", err);
}

void handleCardsDelete() {
  if (!requireAuth()) return;
  String err = deleteCard(server.arg("uid"));
  if (err.isEmpty()) server.send(200, "text/plain", "OK");
  else server.send(404, "text/plain; charset=utf-8", err);
}

void handleLogs() {
  if (!requireAuth()) return;
  String s = "[";
  for (int i = logCount - 1; i >= 0; i--) {     // moi nhat truoc
    if (i != logCount - 1) s += ',';
    s += logToJson(logs[logIndex(i)]);
  }
  s += "]";
  server.send(200, "application/json", s);
}

void handleUnlock() {
  if (!requireAuth()) return;
  addLog("system", "", "web", "remote_open");
  beep(BEEP_GRANTED);
  if (!lowBattery) unlockDoor(UNLOCK_HOLD_MS);
  server.send(200, "text/plain", "OK");
}

void setupWeb() {
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/cards",   HTTP_GET,  handleCardsGet);
  server.on("/api/cards",   HTTP_POST, handleCardsAdd);
  server.on("/api/delete",  HTTP_POST, handleCardsDelete);
  server.on("/api/logs",    HTTP_GET,  handleLogs);
  server.on("/api/unlock",  HTTP_POST, handleUnlock);
  server.on("/api/readmode", HTTP_POST, handleReadMode);
  server.on("/api/enroll",  HTTP_POST, handleEnroll);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ---------------- Theo doi WiFi ----------------
void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiUp = true;
    wifiUpAtMs = millis();
    wifiChanged = true;
  } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiLastReason = info.wifi_sta_disconnected.reason;
    if (wifiUp) {                 // su kien nay lap lai moi lan thu ket noi -> chi tinh lan dau
      wifiUp = false;
      wifiDownAtMs = millis();
      wifiDropCount++;
      wifiChanged = true;
    }
  }
}

// Su kien tao luc chua co gio that (khoi dong khi mat mang) chi co upSec.
// Khi NTP dong bo xong: gio that = bay gio - (uptime hien tai - upSec cua su kien).
void backfillLogTimes() {
  time_t now = time(nullptr);
  uint32_t up = millis() / 1000;
  int n = 0;
  for (int i = 0; i < logCount; i++) {
    LogEntry &e = logs[logIndex(i)];
    if (e.ts == 0) { e.ts = now - (time_t)(up - e.upSec); n++; }
  }
  Serial.printf("[TIME] Da dong bo gio: %s", formatTime(now, up).c_str());
  if (n) Serial.printf(" -> bo sung gio that cho %d su kien luc mat mang", n);
  Serial.println();
}

int pendingLogCount() {
  int n = 0;
  for (int i = 0; i < logCount; i++) if (!logs[logIndex(i)].sent) n++;
  return n;
}

void updateWiFiMonitor() {
  static bool timeSynced = false;
  if (!timeSynced && timeValid()) { timeSynced = true; backfillLogTimes(); }

  if (!wifiChanged) return;
  wifiChanged = false;
  if (wifiUp) {
    // Lan dau co mang sau khi khoi dong: khong co khoang mat mang de bao
    uint32_t offSec = wifiDownAtMs ? (wifiUpAtMs - wifiDownAtMs) / 1000 : 0;
    char note[24] = "";
    if (offSec) snprintf(note, sizeof(note), "mat mang %lus", (unsigned long)offSec);
    Serial.printf("[WIFI] Co mang, IP %s%s%s, con %d su kien cho gui\n",
                  WiFi.localIP().toString().c_str(), offSec ? " - " : "", note, pendingLogCount());
    addLogAt("system", "", note, "wifi_up", wifiUpAtMs, offSec);
  } else {
    Serial.printf("[WIFI] MAT KET NOI (ly do %u: %s)\n", wifiLastReason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)wifiLastReason));
    addLogAt("system", "", "", "wifi_down", wifiDownAtMs, 0);   // moc thoi gian mat mang
  }
}

const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default:                      return "khac";
  }
}

// Lenh "wifi [scan|off|on]"
void wifiCommand(const String &arg) {
  if (arg == "scan") {
    Serial.println("Dang quet (ESP32 chi thay mang 2.4 GHz)...");
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      Serial.printf("  %-28s kenh %2d  %4d dBm  %s%s\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
                    WiFi.RSSI(i), authName(WiFi.encryptionType(i)),
                    WiFi.SSID(i) == WIFI_SSID ? "   <- WIFI_SSID" : "");
    Serial.printf("Tong %d mang. Khong thay \"%s\" -> mang dang phat 5 GHz / an / sai ten\n", n, WIFI_SSID);
    WiFi.scanDelete();
    return;
  }
  if (arg == "off") {             // gia lap mat mang: ngat va khong tu ket noi lai
    wifiManualOff = true;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    Serial.println("[WIFI] Da ngat (gia lap mat mang). Go \"wifi on\" de ket noi lai");
    return;
  }
  if (arg == "on") {
    wifiManualOff = false;
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WIFI] Dang ket noi lai \"%s\"...\n", WIFI_SSID);
    return;
  }

  bool up = WiFi.status() == WL_CONNECTED;
  Serial.println("=== WIFI ===");
  Serial.printf("Trang thai : %s\n", up ? "CO MANG" : wifiManualOff ? "NGAT THU CONG (wifi off)"
                                       : wifiDownAtMs ? "MAT MANG" : "CHUA KET NOI LAN NAO");
  Serial.printf("SSID       : \"%s\"\n", WIFI_SSID);
  if (up) {
    Serial.printf("IP         : %s  (gateway %s)\n", WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());
    Serial.printf("Song       : %d dBm, kenh %d  (> -70 dBm la on)\n", WiFi.RSSI(), WiFi.channel());
    Serial.printf("Da co mang : %lus\n", (unsigned long)((millis() - wifiUpAtMs) / 1000));
  } else {
    if (wifiDownAtMs) Serial.printf("Da mat     : %lus\n", (unsigned long)((millis() - wifiDownAtMs) / 1000));
    if (wifiLastReason)
      Serial.printf("Ly do      : %u (%s)\n", wifiLastReason,
                    WiFi.disconnectReasonName((wifi_err_reason_t)wifiLastReason));
  }
  Serial.printf("So lan mat : %u\n", wifiDropCount);
  if (WiFi.getMode() & WIFI_AP)
    Serial.printf("AP du phong: \"%s\" %s, %d thiet bi\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum());
  Serial.printf("Gio        : %s%s\n", formatTime(timeValid() ? time(nullptr) : 0, millis() / 1000).c_str(),
                timeValid() ? "" : "  (CHUA dong bo NTP)");
  Serial.printf("Hang cho   : %d/%d su kien chua gui -> %s\n", pendingLogCount(), logCount,
                strlen(LOG_SERVER_URL) ? LOG_SERVER_URL : "(chua cau hinh LOG_SERVER_URL)");
  Serial.println("  wifi scan | wifi off | wifi on");
}

void setupWiFi() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Ket noi WiFi \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    // Van tiep tuc thu STA o nen, dong thoi mo AP de vao web cau hinh
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("Khong vao duoc WiFi -> AP \"%s\", IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
  }
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// =====================================================================
//  Lenh Serial
// =====================================================================
void runCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();   // go "SPI" (Caps Lock) van chay

  if (cmd == "v") { for (auto &r : readers) checkReader(r); }
  else if (cmd == "r") { Serial.println("Khoi tao lai..."); initReaders(); }
  else if (cmd == "spi") { spiDiag(); }
  else if (cmd == "d") { verbose = !verbose; Serial.printf("Log chi tiet: %s\n", verbose ? "BAT" : "TAT"); }
  else if (cmd == "lock") { lockDoor(); }
  else if (cmd == "unlock") { unlockDoor(UNLOCK_HOLD_MS); }
  else if (cmd == "servo") { servoWriteUs(arg.toInt()); Serial.printf("Servo %ld us\n", arg.toInt()); }
  else if (cmd == "bat") {
    float v = BAT_MONITOR_ENABLE ? readBatteryVoltage() : 0;
    Serial.printf("Pin: tuc thoi %.2fV, loc %.2fV, %d%%, lowBat=%d\n", v, batV, batPct, lowBattery);
  }
  else if (cmd == "cards") {
    for (int i = 0; i < cardCount; i++) Serial.printf("  %2d. %-30s %s\n", i + 1, cards[i].uid, cards[i].name);
    Serial.printf("Tong: %d\n", cardCount);
  }
  else if (cmd == "add") {
    int sp2 = arg.indexOf(' ');
    String uid = sp2 < 0 ? arg : arg.substring(0, sp2);
    String name = sp2 < 0 ? "" : arg.substring(sp2 + 1);
    String err = addCard(uid, name);
    Serial.println(err.isEmpty() ? "Da them" : err);
  }
  else if (cmd == "del") {
    String err = deleteCard(arg);
    Serial.println(err.isEmpty() ? "Da xoa" : err);
  }
  else if (cmd == "rid") { setReadMode(!readMode); }
  else if (cmd == "enroll") { setEnrollMode(!enrollMode, arg); }
  else if (cmd == "wifi") { arg.toLowerCase(); wifiCommand(arg); }
  else if (cmd == "ip") {
    Serial.printf("STA: %s  AP: %s\n", WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
  }
  else {
    // In nguyen van + ma hex de thay ky tu la (Unikey/Telex, ky tu an...)
    Serial.printf("Lenh khong ro: \"%s\" (hex:", line.c_str());
    for (size_t i = 0; i < line.length(); i++) Serial.printf(" %02X", (uint8_t)line[i]);
    Serial.println(")");
    Serial.println("  Lenh: v r spi d lock unlock servo bat cards add del rid enroll ip");
  }
}

void handleSerialCommand() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { runCommand(buf); buf = ""; }
    else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F && buf.length() < 80) buf += c;   // bo ky tu dieu khien / ngoai ASCII
  }
}

// =====================================================================
//  Arduino
// =====================================================================
void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  buzzerWrite(false);
  pinMode(PIN_EXIT_BTN, INPUT_PULLUP);   // co them 10k ngoai thi van de pull-up noi
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  ledWrite(PIN_LED_GREEN, true);         // thu den luc khoi dong
  ledWrite(PIN_LED_RED, true);

  Serial.begin(115200);
  delay(1500); // cho Serial Monitor ket noi (USB CDC)
  Serial.println("\n=== RFID door - 2 dau doc RC522 ===");

  prefs.begin("door", false);
  loadCards();

  servoInit();
  if (BAT_MONITOR_ENABLE) {
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);   // do toi ~3.1V tai chan ADC
    updateBattery(true);                               // pin yeu ngay luc khoi dong -> mo luon
    Serial.printf("Pin: %.2fV (%d%%)\n", batV, batPct);
  }
  if (!lowBattery) lockDoor();

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  initReaders();
  setupKeypad();

  setupWiFi();
  setupWeb();
  addLog("system", "", "", "boot");

  Serial.printf("San sang. Web: http://%s.local  (user: %s)\n", HOSTNAME, WEB_USER);
}

void loop() {
  handleSerialCommand();
  server.handleClient();

  for (int i = 0; i < 2; i++) {
    String uid;
    if (scanOnce(readers[i], uid)) {
      handleDetection(i, uid);
    }
  }
  checkReadersHealth();

  updateKeypad();
  updateBuzzer();
  updateLeds();
  updateServo();
  updateExitButton();
  updateDoor();
  updateReadMode();
  updateBattery(false);
  updateWiFiMonitor();
  updateUploader();
  delay(10);
}
