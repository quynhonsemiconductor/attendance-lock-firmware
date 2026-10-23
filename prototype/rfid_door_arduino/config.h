#pragma once
// =====================================================================
//  CAU HINH - sua file nay truoc khi nap
// =====================================================================

// ---------------- WiFi + mat khau (KHONG con gia tri mac dinh) ----------------
// WIFI_SSID, WIFI_PASS, WEB_USER, WEB_PASS, AP_PASS deu nam trong secrets.h (KHONG commit).
// Lan dau: copy secrets.example.h -> secrets.h roi dien thong tin that, roi XOA
// dong AP_PASS_PLACEHOLDER trong secrets.h. Thieu secrets.h thi build that bai.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Thieu secrets.h - copy secrets.example.h thanh secrets.h roi dien thong tin that"
#endif

#if !defined(WEB_USER) || !defined(WEB_PASS) || !defined(AP_PASS)
#error "secrets.h thieu WEB_USER / WEB_PASS / AP_PASS"
#endif

// WEB_PASS qua ngan khong duoc phep compile (khoa web quan tri la lop phong thu duy nhat cua prototype).
static_assert(sizeof(WEB_PASS) - 1 >= 12, "WEB_PASS qua ngan - can >= 12 ky tu");

// AP du phong CHI bat khi AP_PASS la mat khau that (da xoa AP_PASS_PLACEHOLDER trong secrets.h).
#if defined(AP_PASS_PLACEHOLDER)
#warning "AP_PASS van la placeholder -> AP du phong se KHONG duoc bat"
#define AP_FALLBACK_ENABLED 0
#else
#define AP_FALLBACK_ENABLED 1
#endif

#define WIFI_TIMEOUT_MS 10000           // khong vao duoc WiFi sau 10s -> bat them AP du phong
#define AP_SSID         "RFID-DOOR"     // AP du phong: vao http://192.168.4.1
#define HOSTNAME        "rfid-door"     // http://rfid-door.local (mDNS)

// ---------------- Gui su kien len server ngoai (tuy chon) ----------------
// De trong "" neu chi dung web tren ESP32. Vd: "http://192.168.1.10:3000/api/events"
// Firmware POST JSON: {"device","time","ts","type","uid","name","result"}
#define LOG_SERVER_URL  ""
#define DEVICE_ID       "door-01"
#define TZ_INFO         "ICT-7"         // UTC+7 Viet Nam
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"

// ---------------- Chan RC522 (SPI chung) ----------------
#define PIN_SPI_SCK   12
#define PIN_SPI_MISO  13
#define PIN_SPI_MOSI  11
#define PIN_CS_OUT    10   // dau doc mat NGOAI -> VAO  (checkin)
#define PIN_CS_IN      9   // dau doc mat TRONG -> RA   (checkout)
// RST (NRSTPD) rieng tung dau doc -> reset cung khi bi treo. LOW = ngu/reset, HIGH = chay.
// Dat -1 neu van noi RST cung len 3.3V (van chay binh thuong, chi mat kha nang reset cung).
#define PIN_RST_OUT   17   // RST dau doc VAO
#define PIN_RST_IN    21   // RST dau doc RA

// ---------------- Servo MG996R ----------------
#define PIN_SERVO         4
#define SERVO_LOCK_US     1000   // xung tai vi tri CHOT VAO NGAM (khoa) - can chinh bang lenh "servo <us>"
#define SERVO_UNLOCK_US   2000   // xung tai vi tri THU CHOT (m
#define SERVO_MOVE_MS     700    // thoi gian giu xung de servo chay het hanh trinh
#define SERVO_RELEASE     true   // true: ngat xung sau khi chay xong (tiet kiem pin, het rung)
                                 // false: giu luc lien tuc (neu chot bi day nguoc duoc)
#define UNLOCK_HOLD_MS    5000   // the dung -> mo bao lau roi tu khoa lai

// ---------------- Buzzer (active buzzer, tin hieu digital) ----------------
#define PIN_BUZZER          5
#define BUZZER_ACTIVE_HIGH  true

// ---------------- Nut Exit (mat trong, ra khong can the) ----------------
// GPIO6 -> nut -> GND, dien tro 10k keo len 3V3 + tu 100nF xuong GND (chong doi).
// GPIO6 la RTC GPIO -> sau nay dung duoc de danh thuc tu deep sleep.
#define PIN_EXIT_BTN        6
#define EXIT_DEBOUNCE_MS    40

// ---------------- LED bao hieu (board NGOAI, qua cap DIL16) ----------------
// GPIO -> R 220R -> LED xanh la -> GND ; GPIO -> R 270R -> LED do -> GND
// Module ESP32 ma co chu "V" cuoi (flash Octal 1.8V) thi GPIO47 chi ra 1.8V -> doi LED do sang GPIO40.
#define PIN_LED_GREEN       16
#define PIN_LED_RED         47
#define LED_ACTIVE_HIGH     true
#define LED_DENIED_MS       1000   // the sai -> LED do sang bao lau

// ---------------- Ban phim cam ung MPR121 (board NGOAI, I2C dia chi 0x5A) ----------------
// SDA/SCL keo len 3V3 bang 4.7k, IRQ keo len 3V3 bang 10k. IRQ = LOW khi co thay doi cham.
// GPIO7 la RTC GPIO -> cham phim danh thuc duoc ESP32 tu deep sleep (giai doan chay pin).
#define MPR121_ENABLE       true   // khong gan MPR121 thi firmware tu bo qua (in thong bao luc khoi dong)
#define PIN_I2C_SDA         8
#define PIN_I2C_SCL         18
#define PIN_MPR121_IRQ      7
#define MPR121_ADDR         0x5A   // chan ADDR noi GND
// Ky tu cua tung dien cuc ELE0..ELE11 - sua theo cach ve phim tren PCB
#define MPR121_KEYMAP       "123456789*0#"

// ---------------- Chan du phong (CHUA dung trong firmware) ----------------
//  GPIO2   cong tac chong thao (board NGOAI, chan 16 cap DIL16)
//  GPIO14  MOSFET cat nguon RC522 (giai doan chay pin)
//  GPIO15  MOSFET cat nguon servo (giai doan chay pin)
//  GPIO39  IRQ RC522 VAO (tuy chon - RC522 khong danh thuc duoc khi co the)
//  GPIO42  IRQ RC522 RA  (tuy chon)
//  GPIO40  cong tac tu bao cua dong ; GPIO41 cong tac hanh trinh chot
// KHONG DUNG: 0, 3, 45, 46 (strapping) | 35-37 (PSRAM Octal module R8) | 19, 20 (USB) | 43, 44 (UART/COM)
//             38 hoac 48 (LED RGB tren DevKitC-1: v1.1 = GPIO38, v1.0 = GPIO48)

// ---------------- Pin (battery) ----------------
#define BAT_MONITOR_ENABLE  false   // false khi test tren ban chua noi cau chia ap (chan ADC tha noi se doc rac)
#define PIN_BAT_ADC         1      // ADC1 (GPIO1..10). KHONG dung ADC2 vi xung dot WiFi
// Cau chia ap: VBAT (sau cong tac, truoc diode SS34) -> R1 1M 1% -> GPIO1 -> R2 470k 1% -> GND, tu 100nF GPIO1-GND
#define BAT_DIVIDER         3.128f // (1M + 470k) / 470k. Hieu chinh: x (V dong ho / V lenh "bat")
#define BAT_V_FULL          6.40f  // 4 x AA moi (1.6V/vien)
#define BAT_V_EMPTY         4.40f  // 4 x AA can (1.1V/vien) -> 20% = 4.8V = muc thap nhat MG996R con du luc
#define BAT_LOW_PCT         20     // < 20% -> THU CHOT (mo cua) de khong nhot nguoi
#define BAT_RECOVER_PCT     25     // > 25% (da sac) -> tro lai che do binh thuong (tre de tranh dao dong)
#define BAT_LOW_CONFIRM     3      // phai doc duoi nguong lien tiep N lan (moi lan 2s) moi kich hoat

// ---------------- Khac ----------------
#define MAX_CARDS     80
#define LOG_SIZE      100          // so su kien giu trong RAM de hien thi tren web
