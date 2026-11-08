#pragma once
void spi_guard_init(void);
void spi_guard_take(void);
void spi_guard_give(void);
void spi_guard_give_from_isr(void);
