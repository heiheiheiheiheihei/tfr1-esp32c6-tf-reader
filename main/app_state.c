#include "app_state.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static SemaphoreHandle_t s_lock;
static app_state_t s_state;
static uint64_t s_last_read;
static uint64_t s_last_write;
static int64_t s_last_tick_us;
static uint64_t s_pending_read;
static uint64_t s_pending_write;
static int s_hist_pos;

void state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_last_tick_us = esp_timer_get_time();
}

void state_snapshot(app_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void state_set_card(const card_info_t *info, bool present, bool fs_mounted)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.card_present = present;
    s_state.fs_mounted = fs_mounted;
    if (info) {
        s_state.info = *info;
        s_state.total_bytes = info->total_bytes;
        s_state.free_bytes = info->free_bytes;
    } else {
        memset(&s_state.info, 0, sizeof(s_state.info));
        s_state.total_bytes = 0;
        s_state.free_bytes = 0;
    }
    xSemaphoreGive(s_lock);
}

void state_set_counts(uint32_t files, uint32_t dirs)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.file_count = files;
    s_state.dir_count = dirs;
    xSemaphoreGive(s_lock);
}

void state_set_host(bool active)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.host_active = active;
    xSemaphoreGive(s_lock);
}

void state_set_error(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.errors++;
    snprintf(s_state.last_error, sizeof(s_state.last_error), "%s", msg ? msg : "");
    xSemaphoreGive(s_lock);
}

void state_add_read(uint64_t bytes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending_read += bytes;
    s_state.session_read += bytes;
    xSemaphoreGive(s_lock);
}

void state_add_write(uint64_t bytes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending_write += bytes;
    s_state.session_write += bytes;
    xSemaphoreGive(s_lock);
}

void state_stats_tick(void)
{
    int64_t now = esp_timer_get_time();
    double dt = (now - s_last_tick_us) / 1000000.0;
    if (dt <= 0.001) return;
    s_last_tick_us = now;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t rd = s_pending_read;
    uint64_t wr = s_pending_write;
    s_pending_read = 0;
    s_pending_write = 0;
    s_last_read += rd;
    s_last_write += wr;

    float rmb = (float)(rd / dt / 1000000.0);
    float wmb = (float)(wr / dt / 1000000.0);
    s_state.read_mbps = rmb;
    s_state.write_mbps = wmb;
    s_state.uptime_s = (uint32_t)(now / 1000000);

    int r = (int)(rmb * 50.0f);
    int w = (int)(wmb * 50.0f);
    if (r > 100) r = 100;
    if (w > 100) w = 100;
    if (r < 0) r = 0;
    if (w < 0) w = 0;
    s_state.hist_read[s_hist_pos] = (uint8_t)r;
    s_state.hist_write[s_hist_pos] = (uint8_t)w;
    s_hist_pos = (s_hist_pos + 1) % STATS_HISTORY;
    s_state.hist_index = s_hist_pos;
    xSemaphoreGive(s_lock);
}

bool state_host_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_state.host_active;
    xSemaphoreGive(s_lock);
    return v;
}
