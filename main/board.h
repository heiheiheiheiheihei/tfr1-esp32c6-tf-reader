#pragma once

#include "driver/gpio.h"

#define LCD_H_RES 172
#define LCD_V_RES 320
#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define PIN_LCD_MOSI 6
#define PIN_LCD_SCLK 7
#define PIN_LCD_MISO 5
#define PIN_LCD_CS 14
#define PIN_LCD_DC 15
#define PIN_LCD_RST 21
#define PIN_LCD_BL 22

#define PIN_SD_CS 4

#define PIN_RGB 8
#define PIN_BOOT 9

#define SD_MOUNT_POINT "/sdcard"
