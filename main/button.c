#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "board.h"
#include "ui.h"

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int stable = 1;
    int raw_last = 1;
    int debounce = 0;
    TickType_t pressed_at = 0;
    bool long_fired = false;

    while (1) {
        int raw = gpio_get_level(PIN_BOOT);
        if (raw != raw_last) {
            raw_last = raw;
            debounce = 0;
        } else if (debounce < 3) {
            debounce++;
        }
        if (debounce == 3 && stable != raw) {
            stable = raw;
            if (stable == 0) {
                pressed_at = xTaskGetTickCount();
                long_fired = false;
            } else {
                TickType_t held = xTaskGetTickCount() - pressed_at;
                if (!long_fired && held < pdMS_TO_TICKS(900)) {
                    ui_post_event(UI_EVT_NEXT);
                }
            }
        }
        if (stable == 0 && !long_fired &&
            (xTaskGetTickCount() - pressed_at) > pdMS_TO_TICKS(900)) {
            long_fired = true;
            ui_post_event(UI_EVT_RESCAN);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void button_init(void)
{
    xTaskCreate(button_task, "button", 2048, NULL, 2, NULL);
}
