// Minimal MFRC522 over SPI (ESP-IDF v6): init/UID-only poll/hard power-down.
// Register map per the MFRC522 datasheet; command flow mirrors the bench
// prototype (hard reset, antenna control, REQA + anticoll 106 kbps).
#include "reader/rc522.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

// Register pages (address<<1 | 0x80 = write in the MSB-first protocol).
#define REG_COMMAND 0x01
#define REG_COM_IRQ 0x04
#define REG_DIV_IRQ 0x05
#define REG_FIFO_LEVEL 0x0A
#define REG_FIFO_DATA 0x09
#define REG_BIT_FRAMING 0x11
#define REG_MODE 0x13
#define REG_TX_MODE 0x12
#define REG_RX_MODE 0x13
#define REG_TX_CONTROL 0x14
#define REG_VERSION 0x37

#define CMD_IDLE 0x00
#define CMD_MEM 0x01
#define CMD_SOFT_RESET 0x0F
#define PCD_RECALIBRATE 0x0F

#define PICC_REQA 0x26
#define PICC_ANTICOLL_CL1 0x93

static const char *TAG = "rc522";

static spi_device_handle_t s_spi;

static void rcs_select(const rc522_conf_t *conf)
{
    gpio_set_level(conf->cs_gpio, 0);
}

static void rcs_deselect(const rc522_conf_t *conf)
{
    gpio_set_level(conf->cs_gpio, 1);
}

static void write_reg(const rc522_conf_t *conf, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    rcs_select(conf);
    spi_device_transmit(s_spi, &t);
    rcs_deselect(conf);
}

static uint8_t read_reg(const rc522_conf_t *conf, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), 0 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    rcs_select(conf);
    spi_device_transmit(s_spi, &t);
    rcs_deselect(conf);
    return rx[1];
}

static void set_bit_mask(const rc522_conf_t *conf, uint8_t reg, uint8_t mask)
{
    write_reg(conf, reg, read_reg(conf, reg) | mask);
}

static void clear_bit_mask(const rc522_conf_t *conf, uint8_t reg, uint8_t mask)
{
    write_reg(conf, reg, read_reg(conf, reg) & (uint8_t)~mask);
}

static void hard_reset(const rc522_conf_t *conf)
{
    if (conf->rst_gpio < 0) {
        return; // RST tied to 3V3: soft reset only
    }
    gpio_set_level(conf->rst_gpio, 0);
    gpio_set_level(conf->rst_gpio, 1);
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
    // RC522 max 10 MHz; one transaction per register access.
    return spi_bus_initialize((spi_host_device_t)spi_host, &bus, SPI_DMA_DISABLED) == ESP_OK;
}

bool rc522_init(const rc522_conf_t *conf)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << conf->cs_gpio) | (conf->rst_gpio >= 0 ? (1ULL << conf->rst_gpio) : 0),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    rcs_deselect(conf);
    if (conf->rst_gpio >= 0) {
        gpio_set_level(conf->rst_gpio, 1);
    }

    hard_reset(conf);
    write_reg(conf, REG_COMMAND, CMD_SOFT_RESET);
    int guard = 0;
    while ((read_reg(conf, REG_COMMAND) & 0x30) && guard++ < 50) {
        // soft reset clears itself; bounded wait, no blocking primitives
    }

    if (conf->rst_gpio >= 0) {
        hard_reset(conf); // prototype's robustness: full hard reset per reader
        write_reg(conf, REG_COMMAND, CMD_SOFT_RESET);
    }

    uint8_t version = read_reg(conf, REG_VERSION);
    // 0x91/0x92 = MFRC522 v1/v2, 0x88/0x98 = clones. Anything else = dead.
    if (version != 0x91 && version != 0x92 && version != 0x88 && version != 0x98) {
        ESP_LOGW(TAG, "reader cs=%d: bad VersionReg 0x%02x — reader offline", conf->cs_gpio,
                 version);
        return false;
    }

    write_reg(conf, REG_TX_MODE, 0x00);
    write_reg(conf, REG_MODE, 0x3F); // CRC off for anticoll, TX CRC only for auth (unused)
    set_bit_mask(conf, REG_TX_CONTROL, 0x03); // antenna on
    return true;
}

// Transceive `tx` and collect up to rx_cap bytes; false on timeout.
static bool transceive(const rc522_conf_t *conf, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                       size_t *rx_len)
{
    write_reg(conf, REG_COM_IRQ, 0x7F);  // clear all IRQ bits
    write_reg(conf, REG_FIFO_LEVEL, 0x80); // flush FIFO
    for (size_t i = 0; i < tx_len; i++) {
        write_reg(conf, REG_FIFO_DATA, tx[i]);
    }
    write_reg(conf, REG_COMMAND, CMD_IDLE);
    write_reg(conf, REG_BIT_FRAMING, tx_len == 1 ? 0x07 : 0x00); // 7 bits for REQA
    write_reg(conf, REG_COMMAND, 0x0C); // Transceive

    for (int i = 0; i < 25; i++) { // ~25 ms bounded: no card = timeout
        uint8_t irq = read_reg(conf, REG_COM_IRQ);
        if (irq & 0x01) { // TimerIRq: no answer
            return false;
        }
        if (irq & 0x20) { // RxIRq
            break;
        }
        // busy-wait bounded by the loop; the poll cadence makes this rare
    }
    write_reg(conf, REG_BIT_FRAMING, 0x80); // StopAlign... (clear StartSend)
    uint8_t fifo = read_reg(conf, REG_FIFO_LEVEL);
    if (fifo > *rx_len) {
        fifo = (uint8_t)*rx_len;
    }
    for (uint8_t i = 0; i < fifo; i++) {
        rx[i] = read_reg(conf, REG_FIFO_DATA);
    }
    *rx_len = fifo;
    return fifo > 0;
}

bool rc522_poll(const rc522_conf_t *conf, uint8_t *uid, uint8_t *uid_len)
{
    uint8_t atqa[2];
    size_t atqa_len = sizeof(atqa);
    uint8_t reqa = PICC_REQA;
    if (!transceive(conf, &reqa, 1, atqa, &atqa_len)) {
        return false; // no card in the field
    }

    uint8_t tx[2] = { PICC_ANTICOLL_CL1, 0x20 };
    uint8_t reply[7];
    size_t reply_len = sizeof(reply);
    if (!transceive(conf, tx, sizeof(tx), reply, &reply_len) || reply_len != 5) {
        return false; // collision or bad frame: try again next poll
    }
    // BCC = UID0^UID1^UID2^UID3 (4-byte UID case); 7/10-byte UIDs cascade —
    // the attendance deployment uses 4-byte classic cards; longer cascades
    // are read as the first level only (recorded, not authenticated).
    if (reply[4] != (uint8_t)(reply[0] ^ reply[1] ^ reply[2] ^ reply[3])) {
        return false;
    }
    if (*uid_len < 4) {
        return false;
    }
    memcpy(uid, reply, 4);
    *uid_len = 4;
    return true;
}

void rc522_power_down(const rc522_conf_t *conf)
{
    clear_bit_mask(conf, REG_TX_CONTROL, 0x03); // antenna off
    write_reg(conf, REG_COMMAND, 0x30);          // PowerDown command
    if (conf->rst_gpio >= 0) {
        gpio_set_level(conf->rst_gpio, 0); // hard power-down (RST low)
    }
}
