#include "rgb.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#include "board.h"

static const char *TAG = "rgb";
static led_strip_handle_t s_strip;
static volatile rgb_state_t s_state = RGB_IDLE;
static volatile int s_pulse_until;
static volatile bool s_pulse_write;

static void apply(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void rgb_set_state(rgb_state_t state)
{
    s_state = state;
}

void rgb_pulse(bool write)
{
    s_pulse_until = 8;
    s_pulse_write = write;
}

static void rgb_task(void *arg)
{
    (void)arg;
    float phase = 0;
    while (1) {
        phase += 0.14f;
        float breath = 0.55f + 0.45f * (0.5f + 0.5f * sinf(phase));
        if (s_pulse_until > 0) {
            s_pulse_until--;
            if (s_pulse_write) apply(180, 90, 0);
            else apply(0, 150, 210);
        } else {
            switch (s_state) {
            case RGB_CARD:
                apply(0, (uint8_t)(16 * breath), (uint8_t)(52 * breath));
                break;
            case RGB_HOST:
                apply(0, (uint8_t)(58 * breath), (uint8_t)(38 * breath));
                break;
            case RGB_ERROR:
                apply((uint8_t)(110 * breath), 0, 8);
                break;
            case RGB_IDLE:
            default:
                apply(0, (uint8_t)(6 * breath), (uint8_t)(18 * breath));
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(55));
    }
}

void rgb_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_RGB,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed: %s", esp_err_to_name(err));
        s_strip = NULL;
    }
    xTaskCreate(rgb_task, "rgb", 3072, NULL, 1, NULL);
}
