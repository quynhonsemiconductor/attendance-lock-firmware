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
#define REG_T_RELOAD_MSB 0x2C // TReloadReg high byte (datasheet table 20)
#define REG_T_RELOAD_LSB 0x2D
#define REG_VERSION 0x37

#define CMD_IDLE 0x00
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

#define IRQ_RX 0x20
#define IRQ_TIMER 0x01
#define ERR_FRAME 0x13 // BufferOfl|ParityErr|ProtocolErr — bad frame, retry next poll
#define ERR_CRC 0x04   // CRCErr — bad CRC on a CRC-on exchange (select), retry next poll
#define ERR_COLL 0x08  // CollErr — two cards answered; CollReg holds the first clash
#define COLL_POS_NOT_VALID 0x20
#define COLL_POS_MASK 0x1F

#define PICC_REQA 0x26
#define PICC_ANTICOLL_CL1 0x93
#define PICC_SELECT_CL1 0x93
#define PICC_HALT 0x50
#define PICC_CASCADE_TAG 0x88

static const char *TAG = "rc522";

static spi_device_handle_t s_spi;

// One SPI frame = one CS assertion. Without DMA the host moves at most its
// 64-byte hardware buffer per transaction. Polling transmit: a frame is a
// few µs at 4 MHz, far less than the interrupt + task switch of
// spi_device_transmit, a cost that adds up at the 10 MHz DFS floor.
#define SPI_FRAME_MAX 64
#define ADDR_W(reg) ((uint8_t)(((reg) << 1) & 0x7E))
#define ADDR_R(reg) ((uint8_t)(ADDR_W(reg) | 0x80))

static void spi_frame(const rc522_conf_t *conf, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = { .length = len * 8, .tx_buffer = tx, .rx_buffer = rx };
    gpio_set_level(conf->cs_gpio, 0);
    (void)spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(conf->cs_gpio, 1);
}

// The MFRC522 SPI protocol: byte0 = reg<<1 | R/W (0x80 = read), MSB first.
// A write frame is one address then data bytes that ALL land in that one
// register (datasheet §8.1.2.2) — a burst only makes sense for FIFODataReg.
static void write_burst(const rc522_conf_t *conf, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t tx[SPI_FRAME_MAX];
    while (len > 0) {
        size_t chunk = len < SPI_FRAME_MAX - 1 ? len : SPI_FRAME_MAX - 1;
        tx[0] = ADDR_W(reg);
        memcpy(&tx[1], data, chunk);
        spi_frame(conf, tx, NULL, chunk + 1);
        data += chunk;
        len -= chunk;
    }
}

static void write_reg(const rc522_conf_t *conf, uint8_t reg, uint8_t val)
{
    write_burst(conf, reg, &val, 1);
}

// A read frame may name a different register in every byte: MOSI carries
// the next address while MISO returns the previous one's value, closed by
// a 0x00 (datasheet §8.1.2.1). n reads cost one frame instead of n.
static void read_regs(const rc522_conf_t *conf, const uint8_t *regs, uint8_t *vals, size_t n)
{
    uint8_t tx[SPI_FRAME_MAX];
    uint8_t rx[SPI_FRAME_MAX];
    while (n > 0) {
        size_t chunk = n < SPI_FRAME_MAX - 1 ? n : SPI_FRAME_MAX - 1;
        for (size_t i = 0; i < chunk; i++) {
            tx[i] = ADDR_R(regs[i]);
        }
        tx[chunk] = 0x00;
        spi_frame(conf, tx, rx, chunk + 1);
        memcpy(vals, &rx[1], chunk);
        regs += chunk;
        vals += chunk;
        n -= chunk;
    }
}

static uint8_t read_reg(const rc522_conf_t *conf, uint8_t reg)
{
    uint8_t val;
    read_regs(conf, &reg, &val, 1);
    return val;
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
    static const uint8_t rmw_regs[2] = { REG_RF_CFG, REG_TX_CONTROL };
    uint8_t rmw[2];
    read_regs(conf, rmw_regs, rmw, 2);
    write_reg(conf, REG_RF_CFG, (uint8_t)((rmw[0] & 0x08) | 0x70)); // RxGain_max
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(rmw[1] & ~0x03));
}

static void antenna_on(const rc522_conf_t *conf)
{
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(read_reg(conf, REG_TX_CONTROL) | 0x03));
}

static void antenna_off(const rc522_conf_t *conf)
{
    write_reg(conf, REG_TX_CONTROL, (uint8_t)(read_reg(conf, REG_TX_CONTROL) & ~0x03));
}

typedef enum {
    XFER_NONE = 0,  // no answer, bad frame or bad CRC: "no card this poll"
    XFER_OK,        // clean reply in rx
    XFER_COLLISION, // reply in rx up to the clash; CollReg says where
} xfer_t;

// Transceive with the timer guarding "no card answered" (25 ms) and a
// ~36 ms software bound behind it in case the IRQ flags never arrive.
// CRC off; callers needing CRC set TxMode/RxMode first. bit_framing carries
// RxAlign (bits 6..4) + TxLastBits (bits 2..0); with RxAlign set, rx[0]
// keeps its low RxAlign bits (the caller's known bits) and takes the rest
// from the reply. *rx_len gets the bytes read. Timeout and overrun are both
// "no card this poll", never a reader fault.
static xfer_t transceive(const rc522_conf_t *conf, const uint8_t *tx, size_t tx_len,
                         uint8_t bit_framing, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    *rx_len = 0;
    write_reg(conf, REG_COM_IRQ, 0x7F); // clear every IRQ flag
    write_reg(conf, REG_FIFO_LEVEL, 0x80); // flush FIFO
    write_burst(conf, REG_FIFO_DATA, tx, tx_len); // whole frame in one SPI burst
    write_reg(conf, REG_COMMAND, CMD_TRANSCEIVE);
    // StartSend is BitFramingReg bit 7, not a CommandReg bit: without it
    // Transceive waits forever and no REQA ever leaves the antenna.
    write_reg(conf, REG_BIT_FRAMING, (uint8_t)(bit_framing | 0x80));

    // ComIrq + Error in one frame per spin.
    static const uint8_t spin_regs[2] = { REG_COM_IRQ, REG_ERROR };
    uint8_t spin[2];
    int64_t deadline_us = esp_timer_get_time() + 36000;
    bool got_rx = false;
    while (esp_timer_get_time() < deadline_us) {
        read_regs(conf, spin_regs, spin, 2);
        if (spin[0] & IRQ_RX) {
            got_rx = true;
            break;
        }
        if (spin[0] & IRQ_TIMER) {
            return XFER_NONE;
        }
        if (spin[1] & ERR_FRAME) {
            return XFER_NONE;
        }
    }
    if (!got_rx) {
        return XFER_NONE; // software bound hit: the FIFO still holds our own TX bytes
    }
    // RxIRq fires on a damaged frame too: the error flags decide. CRCErr
    // latches only on CRC-on exchanges (select/halt). Error + FIFO level in
    // one frame, then the whole reply in one more.
    static const uint8_t done_regs[2] = { REG_ERROR, REG_FIFO_LEVEL };
    uint8_t done[2];
    read_regs(conf, done_regs, done, 2);
    uint8_t err = done[0];
    if (err & (ERR_FRAME | ERR_CRC)) {
        return XFER_NONE;
    }
    uint8_t n = done[1];
    if (n > rx_cap) {
        n = (uint8_t)rx_cap;
    }
    uint8_t fifo_regs[SPI_FRAME_MAX - 1];
    uint8_t fifo[SPI_FRAME_MAX - 1];
    if (n > sizeof(fifo)) {
        n = sizeof(fifo);
    }
    memset(fifo_regs, REG_FIFO_DATA, n);
    read_regs(conf, fifo_regs, fifo, n);
    uint8_t rx_align = (uint8_t)((bit_framing >> 4) & 0x07);
    if (n > 0 && rx_align != 0) {
        uint8_t mask = (uint8_t)(0xFF << rx_align);
        fifo[0] = (uint8_t)((rx[0] & ~mask) | (fifo[0] & mask));
    }
    memcpy(rx, fifo, n);
    *rx_len = n;
    return (err & ERR_COLL) ? XFER_COLLISION : XFER_OK;
}

// ISO 14443-3 anticollision + SELECT at cascade level 1 — miguelbalboa's
// PICC_Select loop, CL1 only. buf = SEL, NVB, UID0..3, BCC. On a clash the
// bit at CollPos is taken as 1 and the loop asks again with one more known
// bit, so two cards in one wallet still yield one of them per poll instead
// of neither. uid_cl1 gets the 4 CL1 bytes on success.
static bool select_cl1(const rc522_conf_t *conf, uint8_t *uid_cl1)
{
    uint8_t buf[9] = { PICC_ANTICOLL_CL1 };
    int known_bits = 0;
    bool bcc_received = false; // false when bit 32 itself was a clash
    // ValuesAfterColl = 0: bits after a clash read back as 0, CollReg valid.
    write_reg(conf, REG_COLL, (uint8_t)(read_reg(conf, REG_COLL) & 0x7F));
    for (int round = 0; round <= 32; round++) {
        size_t n = 0;
        if (known_bits >= 32) {
            uint8_t bcc = (uint8_t)(buf[2] ^ buf[3] ^ buf[4] ^ buf[5]);
            if (bcc_received && buf[6] != bcc) {
                return false; // corrupt anticoll reply: try again next poll
            }
            buf[6] = bcc;
            if (buf[2] == PICC_CASCADE_TAG) {
                ESP_LOGI(TAG, "cs=%d: 7/10-byte UID (cascade) not supported — card skipped",
                         conf->cs_gpio);
                return false;
            }
            uint8_t sak[3];
            buf[1] = 0x70; // NVB: all 40 bits, this is SELECT
            write_reg(conf, REG_TX_MODE, 0x80); // CRC on for select + halt
            write_reg(conf, REG_RX_MODE, 0x80);
            if (transceive(conf, buf, 7, 0x00, sak, sizeof(sak), &n) != XFER_OK || n == 0) {
                return false; // card answered anticoll but failed select — gone or flaky
            }
            memcpy(uid_cl1, &buf[2], 4);
            return true;
        }
        uint8_t tx_last_bits = (uint8_t)(known_bits % 8);
        uint8_t index = (uint8_t)(2 + known_bits / 8); // first byte the card fills
        buf[1] = (uint8_t)((index << 4) | tx_last_bits); // NVB: bytes + bits we send
        size_t tx_len = index + (tx_last_bits ? 1 : 0);
        write_reg(conf, REG_TX_MODE, 0x00); // CRC off for anticoll
        write_reg(conf, REG_RX_MODE, 0x00);
        xfer_t r = transceive(conf, buf, tx_len, (uint8_t)((tx_last_bits << 4) | tx_last_bits),
                              &buf[index], sizeof(buf) - index, &n);
        if (r == XFER_NONE) {
            return false;
        }
        if (r == XFER_OK) {
            known_bits = 32; // whole CL1 UID + BCC in buf: select next round
            bcc_received = true;
            continue;
        }
        uint8_t coll = read_reg(conf, REG_COLL);
        if (coll & COLL_POS_NOT_VALID) {
            return false;
        }
        int pos = coll & COLL_POS_MASK;
        if (pos == 0) {
            pos = 32; // 0 means the 32nd bit
        }
        if (pos <= known_bits) {
            return false; // no progress: give up this poll
        }
        known_bits = pos;
        int bit = (known_bits - 1) % 8;
        buf[1 + known_bits / 8 + ((known_bits % 8) ? 1 : 0)] |= (uint8_t)(1u << bit);
    }
    return false;
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

// CS high (chip off the shared bus) and RST low (hard power-down). Levels
// are set before the pins turn into outputs so neither glitches. The pins
// are taken out of the sleep-time GPIO switch (CONFIG_PM_SLP_DISABLE_GPIO):
// otherwise every tickless light sleep floats them — RST mid-wake and the
// CS of an idle reader — and the "powered down between polls" state is
// not actually held.
void rc522_pins_init(const rc522_conf_t *conf)
{
    uint64_t mask = 1ULL << conf->cs_gpio;
    gpio_set_level(conf->cs_gpio, 1);
    if (conf->rst_gpio >= 0) {
        mask |= 1ULL << conf->rst_gpio;
        gpio_set_level(conf->rst_gpio, 0);
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    (void)gpio_config(&io);
    (void)gpio_sleep_sel_dis(conf->cs_gpio);
    if (conf->rst_gpio >= 0) {
        (void)gpio_sleep_sel_dis(conf->rst_gpio);
    }
}

// Boot-time bring-up: pins (CS high, RST low), then the wake poll's hard
// reset (RST rising edge), init, health check. The RST pin doubles as the
// power-down control (NRSTPD): low = power-down between polls.
bool rc522_init(const rc522_conf_t *conf)
{
    rc522_pins_init(conf);
    return rc522_wake_poll(conf, NULL, NULL) != RC522_FAULT;
}

// The prototype's health check, run on every wake: a live chip re-reads
// TPrescaler as the 0xA9 init wrote it; 0x00/0xFF on VersionReg = no reply.
bool rc522_healthy(const rc522_conf_t *conf)
{
    static const uint8_t regs[2] = { REG_VERSION, REG_T_PRESCALER };
    uint8_t v[2];
    read_regs(conf, regs, v, 2);
    if (v[0] == 0x00 || v[0] == 0xFF) {
        return false; // nothing on the bus
    }
    return v[1] == 0xA9;
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
    // Polling transmit takes no APB pm lock by itself: unheld, DFS can drop
    // APB to 10 MHz (or switch it mid-frame) under the SPI clock. Holding
    // the bus holds that lock — only for the SPI bursts, released across
    // both sleeps so light sleep between them is untouched.
    (void)spi_device_acquire_bus(s_spi, portMAX_DELAY);
    chip_init(conf);
    if (!rc522_healthy(conf)) {
        uint8_t v = read_reg(conf, REG_VERSION);
        spi_device_release_bus(s_spi);
        ESP_LOGW(TAG, "reader cs=%d did not init (VersionReg 0x%02x)", conf->cs_gpio, v);
        return RC522_FAULT;
    }

    antenna_on(conf);
    spi_device_release_bus(s_spi);
    vTaskDelay(pdMS_TO_TICKS(10)); // field settle (ISO 14443 ≥ 5 ms; prototype 10 ms)
    (void)spi_device_acquire_bus(s_spi, portMAX_DELAY);

    rc522_poll_result_t result = RC522_NONE;
    uint8_t reqa = PICC_REQA;
    uint8_t atqa[2];
    size_t n = 0;
    write_reg(conf, REG_TX_MODE, 0x00); // CRC off for REQIDL + anticoll
    write_reg(conf, REG_RX_MODE, 0x00);
    // Several cards answer REQA at once and their ATQAs clash: that is
    // still "a card is here" (miguelbalboa's PICC_IsNewCardPresent).
    xfer_t r = transceive(conf, &reqa, 1, 0x07, atqa, sizeof(atqa), &n);
    if (r == XFER_NONE || (r == XFER_OK && n != sizeof(atqa))) {
        goto out; // no card in the field — the common case
    }
    uint8_t uid_cl1[4];
    if (!select_cl1(conf, uid_cl1)) {
        goto out; // bad frame, cascade UID or card gone: try again next poll
    }
    uint8_t halt[2] = { PICC_HALT, 0x00 };
    uint8_t ignored[3];
    (void)transceive(conf, halt, sizeof(halt), 0x00, ignored, sizeof(ignored), &n);
    if (uid != NULL && uid_len != NULL && *uid_len >= 4) {
        memcpy(uid, uid_cl1, 4);
        *uid_len = 4;
    }
    result = RC522_CARD;
out:
    antenna_off(conf);
    spi_device_release_bus(s_spi);
    return result;
}

void rc522_power_down(const rc522_conf_t *conf)
{
    gpio_set_level(conf->rst_gpio, 0); // NRSTPD low = hard power-down
}
