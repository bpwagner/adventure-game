#pragma once

// Freenove FNK0103 4.0-inch ST7796 display
#define USER_SETUP_LOADED
#define ST7796_DRIVER
#define TFT_WIDTH 320
#define TFT_HEIGHT 480

#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1
#define TFT_BL 27
#define TFT_BACKLIGHT_ON HIGH
#define USE_HSPI_PORT
#define SPI_FREQUENCY 20000000

#define LOAD_GLCD
