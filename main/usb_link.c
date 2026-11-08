#include "usb_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

#include "app_state.h"
#include "files.h"
#include "rgb.h"
#include "sd_card.h"
#include "ui.h"

static const char *TAG = "usb";

#define TFR_MAGIC       0x31524654u
#define TFR_MAX_PAYLOAD 32768
#define TFR_HDR_SIZE    14
#define TFR_FLAG_ERROR  0x01

enum {
    REQ_HELLO = 0x01,
    REQ_INFO = 0x02,
    REQ_READ = 0x10,
    REQ_WRITE = 0x11,
    REQ_FLUSH = 0x12,
    REQ_HOST = 0x13,
    REQ_PING = 0x14,
    REQ_STATUS = 0x20,
    REQ_ECHO = 0x30,
    REQ_SCREEN = 0x40,
    REQ_SCREEN_CHUNK = 0x41,
    REQ_UI_PAGE = 0x42,
    REQ_SCREEN_END = 0x43,
    REQ_LOG = 0x21,
    RESP_HELLO = 0x81,
    RESP_INFO = 0x82,
    RESP_READ = 0x90,
    RESP_WRITE = 0x91,
    RESP_FLUSH = 0x92,
    RESP_HOST = 0x93,
    RESP_PING = 0x94,
    RESP_STATUS = 0xA0,
    RESP_ECHO = 0xB0,
    RESP_SCREEN = 0xC0,
    RESP_SCREEN_CHUNK = 0xC1,
    RESP_UI_PAGE = 0xC2,
    RESP_SCREEN_END = 0xC3,
    RESP_LOG = 0xA1,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint32_t len;
    uint16_t crc;
} tfr_hdr_t;

typedef struct __attribute__((packed)) {
    uint64_t lba;
    uint32_t count;
} tfr_rw_req_t;

typedef struct __attribute__((packed)) {
    uint32_t protocol;
    uint32_t sector_size;
    uint32_t max_transfer;
    uint32_t flags;
    uint64_t sectors;
} tfr_hello_t;

typedef struct __attribute__((packed)) {
    uint32_t present;
    uint32_t mounted;
    uint32_t host;
    uint64_t capacity;
    uint64_t total;
    uint64_t free_bytes;
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t mfg;
    uint32_t oem;
    uint32_t serial;
    uint32_t year;
    uint32_t month;
    char product[9];
    char fs[8];
    char label[16];
} tfr_info_t;

typedef struct __attribute__((packed)) {
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t read_ops;
    uint64_t write_ops;
    uint32_t errors;
    uint32_t host_active;
    uint32_t card_present;
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t tx_errors;
} tfr_status_t;

static SemaphoreHandle_t s_tx_lock;
static uint8_t s_payload[TFR_MAX_PAYLOAD] __attribute__((aligned(4)));
static uint8_t s_io_buf[16384] __attribute__((aligned(4)));
static bool s_host_active;
static int64_t s_last_host_us;
static int64_t s_disconnect_us;
static uint64_t s_read_ops, s_write_ops;
static uint32_t s_tx_errors;
static char s_log[2048];
static size_t s_log_len;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
RTC_DATA_ATTR static uint32_t s_boot_count;
RTC_DATA_ATTR static uint32_t s_last_cmd;
RTC_DATA_ATTR static uint32_t s_last_lba;

void usb_log(const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    taskENTER_CRITICAL(&s_log_mux);
    size_t space = sizeof(s_log) - s_log_len;
    size_t copy = (size_t)n < space ? (size_t)n : space;
    memcpy(s_log + s_log_len, tmp, copy);
    s_log_len += copy;
    taskEXIT_CRITICAL(&s_log_mux);
}

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool write_all(const void *src, size_t len)
{
    const uint8_t *p = src;
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (len) {
        int n = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(50));
        if (n > 0) {
            p += n;
            len -= n;
        } else if (esp_timer_get_time() > deadline) {
            return false;
        } else {
            vTaskDelay(1);
        }
    }
    return true;
}

static bool send_frame(uint8_t type, uint8_t flags, uint16_t seq,
                       const void *payload, uint32_t len)
{
    if (len > TFR_MAX_PAYLOAD) return false;
    uint8_t hdrbuf[TFR_HDR_SIZE];
    tfr_hdr_t *h = (tfr_hdr_t *)hdrbuf;
    h->magic = TFR_MAGIC;
    h->type = type;
    h->flags = flags;
    h->seq = seq;
    h->len = len;
    h->crc = 0;

    uint16_t crc = crc16_update(0xFFFF, (const uint8_t *)&h->type, 8);
    if (len) crc = crc16_update(crc, (const uint8_t *)payload, len);
    h->crc = crc;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    bool ok = write_all(hdrbuf, sizeof(hdrbuf));
    if (ok && len) ok = write_all(payload, len);
    xSemaphoreGive(s_tx_lock);
    if (ok) {
        if (usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200)) != ESP_OK) {
            ok = false;
        }
    }
    if (!ok) {
        s_tx_errors++;
        usb_log("TX fail type=%02x len=%lu", type, (unsigned long)len);
    }
    return ok;
}

static void send_error(uint8_t type, uint16_t seq, int32_t code)
{
    send_frame((uint8_t)(type | 0x80), TFR_FLAG_ERROR, seq, &code, sizeof(code));
}

static int read_exact(uint8_t *dst, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < len) {
        int n = usb_serial_jtag_read_bytes(dst + got, len - got, pdMS_TO_TICKS(20));
        if (n > 0) {
            got += n;
        } else if (esp_timer_get_time() > deadline) {
            break;
        }
    }
    return (int)got;
}

static bool read_frame(tfr_hdr_t *h, uint8_t *payload)
{
    uint8_t magic[4];
    while (1) {
        int n = read_exact(magic, 4, 100);
        if (n != 4) return false;
        if (memcmp(magic, "TFR1", 4) == 0) break;
    }
    uint8_t rest[TFR_HDR_SIZE - 4];
    if (read_exact(rest, sizeof(rest), 200) != sizeof(rest)) return false;
    memcpy((uint8_t *)h + 4, rest, sizeof(rest));
    if (h->len > TFR_MAX_PAYLOAD) return false;
    if (h->len && read_exact(payload, h->len, 500) != (int)h->len) return false;

    uint16_t crc = crc16_update(0xFFFF, (const uint8_t *)&h->type, 8);
    if (h->len) crc = crc16_update(crc, payload, h->len);
    if (crc != h->crc) {
        usb_log("CRC type=%02x len=%lu", h->type, (unsigned long)h->len);
        return false;
    }
    return true;
}

static void host_release(void)
{
    if (!s_host_active) return;
    sd_card_host_end();
    s_host_active = false;
    state_set_host(false);
    const card_info_t *info = sd_card_info();
    state_set_card(info, sd_card_is_mounted(), sd_card_is_mounted());
    files_request_scan();
    rgb_set_state(sd_card_is_mounted() ? RGB_CARD : RGB_IDLE);
    usb_log("host released");
}

static void host_begin(uint16_t seq)
{
    esp_err_t err = sd_card_host_begin();
    if (err != ESP_OK) {
        usb_log("host begin failed %s", esp_err_to_name(err));
        send_error(RESP_HOST, seq, err);
        return;
    }
    s_host_active = true;
    s_last_host_us = esp_timer_get_time();
    state_set_host(true);
    state_set_card(sd_card_info(), true, false);
    rgb_set_state(RGB_HOST);
    uint8_t ok = 1;
    send_frame(RESP_HOST, 0, seq, &ok, 1);
    usb_log("host attached");
}

static void handle_frame(const tfr_hdr_t *h, const uint8_t *payload)
{
    switch (h->type) {
    case REQ_HELLO: {
        tfr_hello_t r = {
            .protocol = 1,
            .sector_size = 512,
            .max_transfer = 8192,
            .flags = 0,
            .sectors = 0,
        };
        if (sd_card_is_mounted() || sd_card_host_active()) {
            const card_info_t *i = sd_card_info();
            r.sectors = i->sector_count;
            r.sector_size = i->sector_size;
        }
        send_frame(RESP_HELLO, 0, h->seq, &r, sizeof(r));
        break;
    }
    case REQ_INFO: {
        app_state_t st;
        state_snapshot(&st);
        tfr_info_t r;
        memset(&r, 0, sizeof(r));
        r.present = st.card_present;
        r.mounted = st.fs_mounted;
        r.host = st.host_active;
        r.capacity = st.info.capacity_bytes;
        r.total = st.total_bytes;
        r.free_bytes = st.free_bytes;
        r.sector_size = st.info.sector_size;
        r.sector_count = st.info.sector_count;
        r.mfg = st.info.cid_mfg;
        r.oem = ((uint32_t)st.info.cid_oem_hi << 8) | st.info.cid_oem_lo;
        r.serial = st.info.cid_serial;
        r.year = st.info.cid_year;
        r.month = st.info.cid_month;
        memcpy(r.product, st.info.product, 8);
        memcpy(r.fs, st.info.fs_type[0] ? st.info.fs_type : "FAT", 3);
        send_frame(RESP_INFO, 0, h->seq, &r, sizeof(r));
        break;
    }
    case REQ_READ: {
        if (h->len < sizeof(tfr_rw_req_t)) { send_error(RESP_READ, h->seq, -1); break; }
        tfr_rw_req_t req;
        memcpy(&req, payload, sizeof(req));
        uint32_t nbytes = req.count * 512;
        if (req.count == 0 || nbytes > sizeof(s_io_buf)) {
            send_error(RESP_READ, h->seq, -2);
            break;
        }
        s_last_cmd = 1;
        s_last_lba = (uint32_t)req.lba;
        esp_err_t err = sd_card_read((uint32_t)req.lba, s_io_buf, req.count);
        if (err != ESP_OK) {
            state_set_error("read");
            send_error(RESP_READ, h->seq, err);
            break;
        }
        s_read_ops++;
        state_add_read(nbytes);
        s_last_host_us = esp_timer_get_time();
        send_frame(RESP_READ, 0, h->seq, s_io_buf, nbytes);
        break;
    }
    case REQ_WRITE: {
        if (h->len < sizeof(tfr_rw_req_t)) { send_error(RESP_WRITE, h->seq, -1); break; }
        tfr_rw_req_t req;
        memcpy(&req, payload, sizeof(req));
        uint32_t nbytes = req.count * 512;
        if (req.count == 0 || nbytes > sizeof(s_io_buf) || h->len < sizeof(req) + nbytes) {
            send_error(RESP_WRITE, h->seq, -2);
            break;
        }
        memcpy(s_io_buf, payload + sizeof(req), nbytes);
        esp_err_t err = sd_card_write((uint32_t)req.lba, s_io_buf, req.count);
        if (err != ESP_OK) {
            state_set_error("write");
            send_error(RESP_WRITE, h->seq, err);
            break;
        }
        s_write_ops++;
        state_add_write(nbytes);
        s_last_host_us = esp_timer_get_time();
        uint8_t ok = 1;
        send_frame(RESP_WRITE, 0, h->seq, &ok, 1);
        break;
    }
    case REQ_FLUSH: {
        uint8_t ok = 1;
        send_frame(RESP_FLUSH, 0, h->seq, &ok, 1);
        break;
    }
    case REQ_HOST: {
        if (h->len < 1) { send_error(RESP_HOST, h->seq, -1); break; }
        if (payload[0]) host_begin(h->seq);
        else {
            host_release();
            uint8_t ok = 1;
            send_frame(RESP_HOST, 0, h->seq, &ok, 1);
        }
        break;
    }
    case REQ_PING: {
        s_last_host_us = esp_timer_get_time();
        uint8_t ok = 1;
        send_frame(RESP_PING, 0, h->seq, &ok, 1);
        break;
    }
    case REQ_STATUS: {
        tfr_status_t r = {
            .read_bytes = 0,
            .write_bytes = 0,
            .read_ops = s_read_ops,
            .write_ops = s_write_ops,
            .errors = 0,
            .host_active = s_host_active,
            .card_present = sd_card_is_mounted() || sd_card_host_active(),
            .free_heap = esp_get_free_heap_size(),
            .min_heap = esp_get_minimum_free_heap_size(),
            .tx_errors = s_tx_errors,
        };
        app_state_t st;
        state_snapshot(&st);
        r.read_bytes = st.session_read;
        r.write_bytes = st.session_write;
        r.errors = st.errors;
        send_frame(RESP_STATUS, 0, h->seq, &r, sizeof(r));
        break;
    }
    case REQ_ECHO: {
        send_frame(RESP_ECHO, 0, h->seq, payload, h->len);
        break;
    }
    case REQ_SCREEN: {
        ui_request_screen();
        for (int i = 0; i < 200 && !ui_screen_ready(); i++) vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t len = 0;
        const uint8_t *data = ui_screen_data(&len);
        if (!data || len == 0) { send_error(RESP_SCREEN, h->seq, -4); break; }
        uint32_t rh[4] = {172, 320, len, 1};
        send_frame(RESP_SCREEN, 0, h->seq, rh, sizeof(rh));
        break;
    }
    case REQ_SCREEN_CHUNK: {
        if (h->len < 8) { send_error(RESP_SCREEN_CHUNK, h->seq, -1); break; }
        uint32_t off, n;
        memcpy(&off, payload, 4);
        memcpy(&n, payload + 4, 4);
        uint32_t len = 0;
        const uint8_t *data = ui_screen_data(&len);
        if (!data || off > len || n > len - off || n > TFR_MAX_PAYLOAD) {
            send_error(RESP_SCREEN_CHUNK, h->seq, -2);
            break;
        }
        send_frame(RESP_SCREEN_CHUNK, 0, h->seq, data + off, n);
        break;
    }
    case REQ_SCREEN_END: {
        ui_screen_release();
        uint8_t ok = 1;
        send_frame(RESP_SCREEN_END, 0, h->seq, &ok, 1);
        break;
    }
    case REQ_UI_PAGE: {
        if (h->len < 1 || payload[0] > 2) { send_error(RESP_UI_PAGE, h->seq, -1); break; }
        ui_request_page(payload[0]);
        uint8_t ok = 1;
        send_frame(RESP_UI_PAGE, 0, h->seq, &ok, 1);
        break;
    }
    case REQ_LOG: {
        taskENTER_CRITICAL(&s_log_mux);
        uint32_t n = (uint32_t)s_log_len;
        taskEXIT_CRITICAL(&s_log_mux);
        send_frame(RESP_LOG, 0, h->seq, s_log, n);
        break;
    }
    default:
        send_error(h->type, h->seq, -3);
        break;
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    tfr_hdr_t h;
    while (1) {
        if (read_frame(&h, s_payload)) {
            handle_frame(&h, s_payload);
        }
        if (s_host_active) {
            int64_t now = esp_timer_get_time();
            if (!usb_serial_jtag_is_connected()) {
                if (s_disconnect_us == 0) s_disconnect_us = now;
                else if (now - s_disconnect_us > 3000000) host_release();
            } else {
                s_disconnect_us = 0;
            }
            if (now - s_last_host_us > 8000000) {
                usb_log("host keepalive timeout");
                host_release();
            }
        } else {
            s_disconnect_us = 0;
        }
    }
}

void usb_link_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 32768,
        .rx_buffer_size = 16384,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb driver install failed: %s", esp_err_to_name(err));
        return;
    }
    xTaskCreate(usb_task, "usb_link", 8192, NULL, 6, NULL);
    s_boot_count++;
    usb_log("usb ready boot=%lu rst=%d last=%lu/%lu", (unsigned long)s_boot_count, (int)esp_reset_reason(), (unsigned long)s_last_cmd, (unsigned long)s_last_lba);
}
