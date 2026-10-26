// MFRC522 over SPI (ESP-IDF v6): init/UID-only poll/hard power-down.
// Register map and command flow ported from the bench-proven prototype
// firmware (prototype/rfid_door_arduino, PR#1) — which itself runs the
// miguelbalboa MFRC522 sequence on this exact hardware: hard NRSTPD reset,
// timer regs, TxASK force-100% ASK, RxGain_max, REQA + anticoll + select,
// antenna only on during the scan turn. UID-only: no MIFARE auth (§2.5).
#include "reader/rc522.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Register addresses (datasheet table 20; page = addr>>4).
#define REG_COMMAND 0x01
#define REG_COM_IRQ 0x04
#define REG_ERROR 0x06
#define REG_FIFO_DATA 0x09
#define REG_FIFO_LEVEL 0x0A
#define REG_BIT_FRAMING 0x0D
#define REG_COLL 0x0E
#define REG_MODE 0x11
#define REG_TX_MODE 0x12
#define REG_RX_MODE 0x13
#define REG_TX_CONTROL 0x14
#define REG_TX_ASK 0x15
#define REG_RF_CFG 0x26
#define REG_T_MODE 0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_LSB 0x2C
#define REG_T_RELOAD_MSB 0x2D
#define REG_VERSION 0x37

#define CMD_IDLE 0x00
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

#define IRQ_RX 0x20
#define IRQ_TIMER 0x01
#define ERR_FRAME 0x13 // BufferOfl|ParityErr|ProtocolErr — bad frame, retry next poll

#define PICC_REQA 0x26
#define PICC_ANTICOLL_CL1 0x93
#define PICC_SELECT_CL1 0x93
#define PICC_HALT 0x50
#define PICC_CASCADE_TAG 0x88

static const char *TAG = "rc522";

static spi_device_handle_t s_spi;

// The MFRC522 SPI protocol: byte0 = reg<<1 | R/W (0x80 = read), MSB first.
static void write_reg(const rc522_conf_t *conf, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    gpio_set_level(conf->cs_gpio, 0);
    (void)spi_device_transmit(s_spi, &t);
    gpio_set_level(conf->cs_gpio, 1);
}

static uint8_t read_reg(const rc522_conf_t *conf, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    gpio_set_level(conf->cs_gpio, 0);
    (void)spi_device_transmit(s_spi, &t);
    gpio_set_level(conf->cs_gpio, 1);
    return rx[1];
}

static void write_fifo(const rc522_conf_t *conf, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        write_reg(conf, REG_FIFO_DATA, data[i]);
    }
}

// The prototype's init recipe (its PCD_Init): soft reset, then the timer /
// ASK / mode registers that make REQA work. TPrescaler 0xA9 + TMode 0x80 +
// TReload 0x03E8 gives the 25 ms transceive timeout the timer IRQ reports.
static void chip_init(const rc522_conf_t *conf)
{
    write_reg(conf, REG_COMMAND, CMD_SOFT_RESET);
    for (int i = 0; i < 20 && (read_reg(conf, REG_COMMAND) & 0x10); i++) {
        vTaskDelay(1); // PowerDown bit clears when the reset finishes
    }
    write_reg(conf, REG_T_MODE, 0x80);
    write_reg(conf, REG_T_PRESCALER, 0xA9);
    write_reg(conf, REG_T_RELOAD_LSB, 0xE8); // 0x03E8 ticks of 25 µs = 25 ms
    write_reg(conf, REG_T_RELOAD_MSB, 0x03);
    write_reg(conf, REG_TX_ASK, 0x40);
    write_reg(conf, REG_MODE, 0x3D);
    write_reg(conf, REG_RF_CFG,
              (uint8_t)((read_reg(conf, REG_RF_CFG) & 0x08) | 0x70)); // RxGain_max
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(read_reg(conf, REG_TX_CONTROL) & ~0x03));
}

static void antenna_on(const rc522_conf_t *conf)
{
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(read_reg(conf, REG_TX_CONTROL) | 0x03));
}

static void antenna_off(const rc522_conf_t *conf)
{
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(read_reg(conf, REG_TX_CONTROL) & ~0x03));
}

// Transceive with the timer guarding "no card answered" (25 ms) and a
// ~36 ms software bound behind it in case the IRQ flags never arrive.
// CRC off; callers
// needing CRC set TxMode/RxMode first. Returns reply length or 0 on
// timeout/overrun (both are "no card this poll", not a reader fault).
static size_t transceive(const rc522_conf_t *conf, const uint8_t *tx, size_t tx_len,
                         uint8_t bit_framing, uint8_t *rx, size_t rx_cap)
{
    write_reg(conf, REG_COM_IRQ, 0x7F); // clear every IRQ flag
    write_reg(conf, REG_FIFO_LEVEL, 0x80); // flush FIFO
    write_fifo(conf, tx, tx_len);
    write_reg(conf, REG_BIT_FRAMING, bit_framing);
    write_reg(conf, REG_COMMAND, CMD_TRANSCEIVE); // then StartSend below
    write_reg(conf, REG_COMMAND, (uint8_t)(CMD_TRANSCEIVE | 0x80));

    int64_t deadline_us = esp_timer_get_time() + 36000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t irq = read_reg(conf, REG_COM_IRQ);
        if (irq & IRQ_RX) {
            break;
        }
        if (irq & IRQ_TIMER) {
            return 0;
        }
        if (read_reg(conf, REG_ERROR) & ERR_FRAME) {
            return 0;
        }
    }
    uint8_t n = read_reg(conf, REG_FIFO_LEVEL);
    if (n > rx_cap) {
        n = (uint8_t)rx_cap;
    }
    for (uint8_t i = 0; i < n; i++) {
        rx[i] = read_reg(conf, REG_FIFO_DATA);
    }
    return n;
}

bool rc522_bus_init(int spi_host, int sck, int miso, int mosi)
{
    if (s_spi != NULL) {
        return true; // shared bus already up
    }
    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    // 4 MHz: the bench rate — 10 MHz worked on short leads, the deployment
    // harness runs longer ones (prototype diagnostics flag MISO noise).
    esp_err_t err = spi_bus_initialize((spi_host_device_t)spi_host, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return false;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4000000,
        .mode = 0, // SPI mode 0, MSB first (MFRC522 datasheet §8.1.2)
        .spics_io_num = -1, // CS driven by hand: one CS per reader
        .queue_size = 1,
    };
    return spi_bus_add_device((spi_host_device_t)spi_host, &dev, &s_spi) == ESP_OK;
}

// Boot-time bring-up: pins high (CS high keeps the chip off the bus), hard
// reset, init, health check, powered down. The RST pin doubles as the
// power-down control (NRSTPD): low = power-down between polls.
bool rc522_init(const rc522_conf_t *conf)
{
    uint64_t mask = 1ULL << conf->cs_gpio;
    if (conf->rst_gpio >= 0) {
        mask |= 1ULL << conf->rst_gpio;
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    (void)gpio_config(&io);
    gpio_set_level(conf->cs_gpio, 1);
    if (conf->rst_gpio >= 0) {
        gpio_set_level(conf->rst_gpio, 1);
    }
    return rc522_wake_poll(conf, NULL, NULL) != RC522_FAULT;
}

// The prototype's health check, run on every wake: a live chip re-reads
// TPrescaler as the 0xA9 init wrote it; 0x00/0xFF on VersionReg = no reply.
bool rc522_healthy(const rc522_conf_t *conf)
{
    uint8_t v = read_reg(conf, REG_VERSION);
    if (v == 0x00 || v == 0xFF) {
        return false; // nothing on the bus
    }
    return read_reg(conf, REG_T_PRESCALER) == 0xA9;
}

// One poll cycle (battery/wake decisions 2026-10-25): wake from hard
// power-down — which self-resets the chip, so the FULL init re-runs every
// poll — antenna on, settle, REQIDL + anticoll + select + halt, antenna
// off, power down again. 4-byte UIDs only: a cascade tag (0x88) in the CL1
// UID means a 7/10-byte card — rejected until CL2 is implemented.
rc522_poll_result_t rc522_wake_poll(const rc522_conf_t *conf, uint8_t *uid, uint8_t *uid_len)
{
    if (conf->rst_gpio < 0) {
        ESP_LOGE(TAG, "no RST pin: power-down wake impossible");
        return RC522_FAULT;
    }
    gpio_set_level(conf->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); // crystal restart after power-down (datasheet §9.3.2.6, prototype uses 50 ms)
    chip_init(conf);
    if (!rc522_healthy(conf)) {
        uint8_t v = read_reg(conf, REG_VERSION);
        ESP_LOGW(TAG, "reader cs=%d did not init (VersionReg 0x%02x)", conf->cs_gpio, v);
        return RC522_FAULT;
    }

    antenna_on(conf);
    vTaskDelay(pdMS_TO_TICKS(10)); // field settle (ISO 14443 ≥ 5 ms; prototype 10 ms)

    rc522_poll_result_t result = RC522_NONE;
    uint8_t reqa = PICC_REQA;
    uint8_t atqa[2];
    write_reg(conf, REG_TX_MODE, 0x00); // CRC off for REQIDL + anticoll
    write_reg(conf, REG_RX_MODE, 0x00);
    if (transceive(conf, &reqa, 1, 0x07, atqa, sizeof(atqa)) != sizeof(atqa)) {
        goto out; // no card in the field — the common case
    }
    uint8_t reply[5];
    uint8_t anticoll[2] = { PICC_ANTICOLL_CL1, 0x20 };
    if (transceive(conf, anticoll, sizeof(anticoll), 0x00, reply, sizeof(reply)) != 5) {
        goto out; // collision or bad frame: try again next poll
    }
    uint8_t bcc = (uint8_t)(reply[0] ^ reply[1] ^ reply[2] ^ reply[3]);
    if (reply[4] != bcc || reply[0] == PICC_CASCADE_TAG) {
        if (reply[4] == bcc && reply[0] == PICC_CASCADE_TAG) {
            ESP_LOGI(TAG, "cs=%d: 7/10-byte UID (cascade) not supported — card skipped",
                     conf->cs_gpio);
        }
        goto out;
    }
    uint8_t select[7] = { PICC_SELECT_CL1, 0x70, reply[0], reply[1], reply[2], reply[3], bcc };
    uint8_t sak[3];
    write_reg(conf, REG_TX_MODE, 0x80); // CRC on for select + halt
    write_reg(conf, REG_RX_MODE, 0x80);
    size_t sak_len = transceive(conf, select, sizeof(select), 0x00, sak, sizeof(sak));
    if (sak_len == 0) {
        goto out; // card answered anticoll but failed select — gone or flaky
    }
    uint8_t halt[2] = { PICC_HALT, 0x00 };
    (void)transceive(conf, halt, sizeof(halt), 0x00, sak, sizeof(sak));
    if (uid != NULL && uid_len != NULL && *uid_len >= 4) {
        memcpy(uid, reply, 4);
        *uid_len = 4;
    }
    result = RC522_CARD;
out:
    antenna_off(conf);
    return result;
}

void rc522_power_down(const rc522_conf_t *conf)
{
    gpio_set_level(conf->rst_gpio, 0); // NRSTPD low = hard power-down
}
