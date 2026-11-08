#include "spi_guard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_guard;

void spi_guard_init(void)
{
    if (!s_guard) {
        s_guard = xSemaphoreCreateBinary();
        xSemaphoreGive(s_guard);
    }
}

void spi_guard_take(void)
{
    if (s_guard) xSemaphoreTake(s_guard, portMAX_DELAY);
}

void spi_guard_give(void)
{
    if (s_guard) xSemaphoreGive(s_guard);
}

void spi_guard_give_from_isr(void)
{
    if (s_guard) {
        BaseType_t hp = pdFALSE;
        xSemaphoreGiveFromISR(s_guard, &hp);
        if (hp) portYIELD_FROM_ISR(hp);
    }
}
