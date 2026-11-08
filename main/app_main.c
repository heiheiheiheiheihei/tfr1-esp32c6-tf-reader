#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

#include "app_state.h"
#include "board.h"
#include "button.h"
#include "card_monitor.h"
#include "display/display.h"
#include "files.h"
#include "rgb.h"
#include "sd_card.h"
#include "spi_guard.h"
#include "ui.h"
#include "usb_link.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_LOGI(TAG, "boot");
    state_init();
    spi_guard_init();
    display_init();
    display_lvgl_init();
    rgb_init();
    ui_init();
    files_init();
    card_monitor_init();
    button_init();
    usb_link_start();
    ESP_LOGI(TAG, "system ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        lv_timer_handler();
    }
}
