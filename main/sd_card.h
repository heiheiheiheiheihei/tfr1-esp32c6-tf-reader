#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

typedef struct {
    bool present;
    bool mounted;
    uint64_t capacity_bytes;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t sector_size;
    uint32_t sector_count;
    uint8_t cid_mfg;
    uint8_t cid_oem_hi;
    uint8_t cid_oem_lo;
    uint16_t cid_year;
    uint8_t cid_month;
    uint32_t cid_serial;
    char product[9];
    char fs_type[8];
    char label[16];
    /* Block-level data path looks unusable (e.g. every LBA aliases to one block). */
    bool fault;
} card_info_t;

esp_err_t sd_card_mount(void);
void sd_card_unmount(void);
bool sd_card_is_mounted(void);
/* Card answers at block level, even when FATFS cannot mount it. */
bool sd_card_is_present(void);
esp_err_t sd_card_check(void);
void sd_card_lock(void);
void sd_card_unlock(void);
const card_info_t *sd_card_info(void);
sdmmc_card_t *sd_card_handle(void);

esp_err_t sd_card_host_begin(void);
esp_err_t sd_card_host_end(void);
bool sd_card_host_active(void);
esp_err_t sd_card_read(uint32_t lba, void *dst, uint32_t count);
esp_err_t sd_card_write(uint32_t lba, const void *src, uint32_t count);
