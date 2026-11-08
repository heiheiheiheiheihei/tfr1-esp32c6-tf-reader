#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_EVT_NEXT 0x01
#define UI_EVT_RESCAN 0x02

void ui_init(void);
void ui_post_event(uint32_t evt);
void ui_mark_dirty(void);
void ui_request_screen(void);
void ui_request_page(int page);
bool ui_screen_ready(void);
const uint8_t *ui_screen_data(uint32_t *len);
void ui_screen_release(void);
