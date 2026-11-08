#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sd_card.h"

#define STATS_HISTORY 32

typedef struct {
    bool card_present;
    bool fs_mounted;
    bool host_active;
    card_info_t info;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t file_count;
    uint32_t dir_count;
    uint32_t errors;
    uint64_t session_read;
    uint64_t session_write;
    float read_mbps;
    float write_mbps;
    uint8_t hist_read[STATS_HISTORY];
    uint8_t hist_write[STATS_HISTORY];
    uint8_t hist_index;
    uint32_t uptime_s;
    char last_error[40];
} app_state_t;

void state_init(void);
void state_snapshot(app_state_t *out);
void state_set_card(const card_info_t *info, bool present, bool fs_mounted);
void state_set_counts(uint32_t files, uint32_t dirs);
void state_set_host(bool active);
void state_set_error(const char *msg);
void state_add_read(uint64_t bytes);
void state_add_write(uint64_t bytes);
void state_stats_tick(void);
bool state_host_active(void);
