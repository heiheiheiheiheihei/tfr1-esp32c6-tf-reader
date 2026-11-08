#pragma once

#include <stdbool.h>
#include "lvgl.h"

#define DISPLAY_BUF_LINES 40

void display_init(void);
void display_set_backlight(uint8_t percent);
void display_lvgl_init(void);
lv_disp_t *display_get_disp(void);
