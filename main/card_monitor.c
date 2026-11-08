#include "card_monitor.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_state.h"
#include "board.h"
#include "files.h"
#include "rgb.h"
#include "sd_card.h"

static const char *TAG = "monitor";

static void publish(bool present)
{
    card_info_t info;
    memset(&info, 0, sizeof(info));
    if (present) {
        const card_info_t *p = sd_card_info();
        if (p) info = *p;
    }
    state_set_card(present ? &info : NULL, present, sd_card_is_mounted());
    if (present) {
        files_request_scan();
        rgb_set_state(RGB_CARD);
    } else {
        state_set_counts(0, 0);
        rgb_set_state(RGB_IDLE);
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    bool last_present = false;

    while (1) {
        if (state_host_active() || sd_card_host_active()) {
            vTaskDelay(pdMS_TO_TICKS(600));
            continue;
        }

        bool mounted = sd_card_is_mounted();
        if (mounted) {
            if (sd_card_check() != ESP_OK) {
                ESP_LOGW(TAG, "card removed");
                sd_card_unmount();
                publish(false);
                last_present = false;
                vTaskDelay(pdMS_TO_TICKS(800));
                continue;
            }
            if (!last_present) {
                publish(true);
                last_present = true;
            }
        } else {
            esp_err_t err = sd_card_mount();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "card mounted");
                publish(true);
                last_present = true;
            } else if (last_present) {
                publish(false);
                last_present = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void card_monitor_init(void)
{
    xTaskCreate(monitor_task, "card_mon", 4096, NULL, 3, NULL);
}
