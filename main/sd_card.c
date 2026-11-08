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
#include "usb_link.h"

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
    if (mounted) {
        snprintf(s_info.fs_type, sizeof(s_info.fs_type), "FAT");
    }
    s_info.fault = false;
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

/* Read-only data-path self-test.
 *
 * Three widely separated sectors are compared.  A healthy card returns
 * different content (or blank).  Counterfeit / defective cards whose
 * controller ignores the address bits return the very same block for every
 * LBA -- the failure mode that leaves sdmmc_card_init() happy while FATFS
 * cannot mount and nothing else reports why.
 *
 * Static buffer: the whole SD path runs under s_lock, and the card monitor
 * task only has a 4 KiB stack. */
static bool data_path_aliased(sdmmc_card_t *card)
{
    uint32_t n = card->csd.capacity;
    if (n < 64) return false;
    static uint8_t blk[3][512];
    const uint32_t lbas[3] = { 0, n / 2, n - 16 };

    for (int i = 0; i < 3; i++) {
        if (sdmmc_read_sectors(card, blk[i], lbas[i], 1) != ESP_OK) return false;
    }
    if (memcmp(blk[0], blk[1], 512) != 0 || memcmp(blk[0], blk[2], 512) != 0) {
        return false;
    }
    /* all-0x00 / all-0xFF is a legitimately blank card, not aliasing */
    for (int i = 0; i < 512; i++) {
        if (blk[0][i] != 0x00 && blk[0][i] != 0xFF) return true;
    }
    return false;
}

/* Re-open the card at block level, without FATFS.
 *
 * esp_vfs_fat_sdspi_mount() releases the card and the sdspi slot when the
 * filesystem cannot be mounted, so presence, CID/CSD and the data-path check
 * have to be recovered here. */
static esp_err_t probe_locked(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = LCD_HOST;

    sdspi_dev_handle_t handle;
    esp_err_t err = sdspi_host_init_device(&slot_config, &handle);
    if (err != ESP_OK) return err;

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        sdspi_host_remove_device(handle);
        return ESP_ERR_NO_MEM;
    }

    host.slot = handle;
    err = sdmmc_card_init(&host, card);
    if (err == ESP_OK) {
        info_from_card(card, false);
        s_info.fault = data_path_aliased(card);
        snprintf(s_info.fs_type, sizeof(s_info.fs_type), "RAW");
    }
    free(card);
    sdspi_host_remove_device(handle);
    return err;
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
        s_info.fault = false;
        s_info.fs_type[0] = 0;
        s_info.total_bytes = 0;
        s_info.free_bytes = 0;
        /* FATFS rejects exFAT/NTFS/unformatted media with ESP_FAIL after the
         * card answered fine, so presence is decided by a block-level probe
         * instead of by the mount result. */
        esp_err_t perr = probe_locked();
        if (perr == ESP_OK) {
            usb_log("mount failed %s; raw card %u MB%s", esp_err_to_name(err),
                    (unsigned)(s_info.capacity_bytes / (1024 * 1024)),
                    s_info.fault ? " (FAULT: all LBAs alias)" : "");
        } else {
            usb_log("no card: mount %s, probe %s", esp_err_to_name(err),
                    esp_err_to_name(perr));
        }
        ESP_LOGW(TAG, "mount failed: %s (probe %s)", esp_err_to_name(err),
                 esp_err_to_name(perr));
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

bool sd_card_is_present(void)
{
    card_lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_info.present;
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
