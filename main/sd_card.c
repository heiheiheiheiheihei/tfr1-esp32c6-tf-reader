#include "sd_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board.h"
#include "spi_guard.h"

static const char *TAG = "sd";
static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static sdmmc_card_t *s_raw_card;
static sdspi_dev_handle_t s_raw_handle = -1;
static card_info_t s_info;
static bool s_mounted;
static bool s_host_mode;

static void info_from_card(sdmmc_card_t *card, bool mounted)
{
    s_info.present = true;
    s_info.mounted = mounted;
    s_info.sector_size = card->csd.sector_size;
    s_info.sector_count = card->csd.capacity;
    s_info.capacity_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    s_info.cid_mfg = (uint8_t)card->cid.mfg_id;
    s_info.cid_oem_hi = (uint8_t)(card->cid.oem_id >> 8);
    s_info.cid_oem_lo = (uint8_t)(card->cid.oem_id & 0xff);
    s_info.cid_serial = (uint32_t)card->cid.serial;
    s_info.cid_year = 2000 + ((card->cid.date >> 4) & 0xff);
    s_info.cid_month = card->cid.date & 0x0f;
    snprintf(s_info.product, sizeof(s_info.product), "%.8s", card->cid.name);
    if (s_info.fs_type[0] == 0) {
        snprintf(s_info.fs_type, sizeof(s_info.fs_type), "FAT");
    }
}

static void fill_fs_info(void)
{
    if (!s_card) return;
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free) == ESP_OK) {
        s_info.total_bytes = total;
        s_info.free_bytes = free;
    } else {
        s_info.total_bytes = s_info.capacity_bytes;
        s_info.free_bytes = 0;
    }
}

static esp_err_t mount_locked(void)
{
    if (s_host_mode) return ESP_ERR_INVALID_STATE;
    if (s_mounted) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = LCD_HOST;

    ESP_LOGI(TAG, "mounting TF card");
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        s_info.present = false;
        s_info.mounted = false;
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    info_from_card(s_card, true);
    fill_fs_info();
    ESP_LOGI(TAG, "card: %s %u MB, %u sectors x %u B, fs total=%llu free=%llu",
             s_info.product,
             (unsigned)(s_info.capacity_bytes / (1024 * 1024)),
             (unsigned)s_info.sector_count,
             (unsigned)s_info.sector_size,
             (unsigned long long)s_info.total_bytes,
             (unsigned long long)s_info.free_bytes);
    return ESP_OK;
}

static void unmount_locked(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    s_info.mounted = false;
}

static esp_err_t raw_open_locked(void)
{
    if (s_raw_card) return ESP_OK;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = LCD_HOST;

    sdspi_dev_handle_t handle;
    esp_err_t err = sdspi_host_init_device(&slot_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        sdspi_host_remove_device(handle);
        return ESP_ERR_NO_MEM;
    }

    host.slot = handle;
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init: %s", esp_err_to_name(err));
        free(card);
        sdspi_host_remove_device(handle);
        return err;
    }

    s_raw_card = card;
    s_raw_handle = handle;
    info_from_card(card, false);
    ESP_LOGI(TAG, "raw host mode ready: %u sectors", (unsigned)s_info.sector_count);
    return ESP_OK;
}

static void raw_close_locked(void)
{
    if (!s_raw_card) return;
    if (s_raw_handle >= 0) {
        sdspi_host_remove_device(s_raw_handle);
        s_raw_handle = -1;
    }
    free(s_raw_card);
    s_raw_card = NULL;
}

static void card_lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

esp_err_t sd_card_mount(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mount_locked();
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}

void sd_card_unmount(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unmount_locked();
    xSemaphoreGive(s_lock);
    spi_guard_give();
}

bool sd_card_is_mounted(void)
{
    card_lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_mounted;
    xSemaphoreGive(s_lock);
    return v;
}

void sd_card_lock(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void sd_card_unlock(void)
{
    xSemaphoreGive(s_lock);
    spi_guard_give();
}

esp_err_t sd_card_check(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_mounted && s_card) {
        err = sdmmc_get_status(s_card);
    }
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}

const card_info_t *sd_card_info(void)
{
    return &s_info;
}

sdmmc_card_t *sd_card_handle(void)
{
    return s_card;
}

esp_err_t sd_card_host_begin(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_host_mode) {
        xSemaphoreGive(s_lock);
        spi_guard_give();
        return ESP_OK;
    }

    unmount_locked();
    esp_err_t err = raw_open_locked();
    if (err == ESP_OK) {
        s_host_mode = true;
    } else {
        mount_locked();
    }
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}

esp_err_t sd_card_host_end(void)
{
    card_lock_init();
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_host_mode) {
        xSemaphoreGive(s_lock);
        spi_guard_give();
        return ESP_OK;
    }
    raw_close_locked();
    s_host_mode = false;
    esp_err_t err = mount_locked();
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}

bool sd_card_host_active(void)
{
    card_lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_host_mode;
    xSemaphoreGive(s_lock);
    return v;
}

esp_err_t sd_card_read(uint32_t lba, void *dst, uint32_t count)
{
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_host_mode && s_raw_card) {
        err = sdmmc_read_sectors(s_raw_card, dst, lba, count);
    }
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}

esp_err_t sd_card_write(uint32_t lba, const void *src, uint32_t count)
{
    spi_guard_take();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_host_mode && s_raw_card) {
        err = sdmmc_write_sectors(s_raw_card, src, lba, count);
    }
    xSemaphoreGive(s_lock);
    spi_guard_give();
    return err;
}
