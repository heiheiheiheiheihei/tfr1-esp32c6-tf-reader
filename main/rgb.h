#pragma once

#include <stdbool.h>

typedef enum {
    RGB_IDLE = 0,
    RGB_CARD,
    RGB_HOST,
    RGB_ERROR,
} rgb_state_t;

void rgb_init(void);
void rgb_set_state(rgb_state_t state);
void rgb_pulse(bool write);
