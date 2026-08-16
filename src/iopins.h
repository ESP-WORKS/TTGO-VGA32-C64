#ifndef IOPINS_H
#define IOPINS_H

// IDF 5.x nao vaza mais esses headers via esp_system.h, e iopins.h faz cast
// para gpio_num_t / usa ADC2_CHANNEL_*. Precisa incluir explicitamente.
#include "driver/gpio.h"
#include "driver/adc.h"

// ILI9341
//#define PIN_NUM_CS      (gpio_num_t)15
//#define PIN_NUM_CLK     (gpio_num_t)14
//#define PIN_NUM_MISO    (gpio_num_t)12
//#define PIN_NUM_MOSI    (gpio_num_t)13
//#define PIN_NUM_DC      (gpio_num_t)16

#define TPIN_NUM_CS        (gpio_num_t)32
#define TPIN_NUM_IRQ       (gpio_num_t)33


#define PIN_NUM_CS        (gpio_num_t)17
#define PIN_NUM_CLK       (gpio_num_t)18
#define PIN_NUM_MISO      (gpio_num_t)19
#define PIN_NUM_MOSI      (gpio_num_t)23
#define PIN_NUM_DC        (gpio_num_t)21

// SD card SPI
// --- Cartao SD da TTGO VGA32 v1.4 ---
#define SPIN_NUM_CS      (gpio_num_t)13
#define SPIN_NUM_CLK     (gpio_num_t)14
#define SPIN_NUM_MISO    (gpio_num_t)2    // LilyGO=2, ROBGO/Olimex=35
#define SPIN_NUM_MOSI    (gpio_num_t)12

// --- Saida VGA (TTGO VGA32 v1.4) ---
// ATENCAO: GPIO 4/5 eram o I2C do teclado e o 15 era o CS do SD no
// hardware original do MCUME. Na VGA32 sao pinos de video.
#define VGA_PIN_R1       22
#define VGA_PIN_R0       21
#define VGA_PIN_G1       19
#define VGA_PIN_G0       18
#define VGA_PIN_B1       5
#define VGA_PIN_B0       4
#define VGA_PIN_HSYNC    23
#define VGA_PIN_VSYNC    15


// I2C keyboard
#define I2C_SCL_IO        (gpio_num_t)5 
#define I2C_SDA_IO        (gpio_num_t)4 


// Analog joystick (primary) for JOY2 and 5 extra buttons
#define PIN_JOY2_A1X     ADC2_CHANNEL_7 // 27 //ADC1_CHANNEL_0
#define PIN_JOY2_A2Y     ADC2_CHANNEL_2 // 2  //ADC1_CHANNEL_3
#define PIN_JOY2_BTN    32

#define PIN_KEY_USER1   35
#define PIN_KEY_USER2   34   // <-- reused as LINK_PIN_RX when HAS_TDISPLAY_LINK
#define PIN_KEY_USER3   39
#define PIN_KEY_USER4   36

// TTGO T-Display controller bridge (UART2)
// GPIO34 is input-only: fine for RX, unusable for TX.
// GPIO26 is DAC2. AudioPlaySystem was moved to DAC1 (GPIO25) to free it.
#define LINK_PIN_RX     (gpio_num_t)34
#define LINK_PIN_TX     (gpio_num_t)26
/*
#define PIN_KEY_ESCAPE  23
*/

// Second joystick
/*
#define PIN_JOY1_BTN     30
#define PIN_JOY1_1       16
#define PIN_JOY1_2       17
#define PIN_JOY1_3       18
#define PIN_JOY1_4       19
*/

#endif




