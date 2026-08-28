#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

// ===== CẤU HÌNH ÂM THANH =====
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true
#define AUDIO_I2S_METHOD_SIMPLEX

// ===== MIC INMP441 (I2S) =====
#ifdef AUDIO_I2S_METHOD_SIMPLEX
    #define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
    #define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
    #define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
    
    #define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
    #define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
    #define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16
#endif

// ===== MÀN HÌNH OLED SSD1306 (I2C) =====
#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// ===== CẢM BIẾN AHT20 =====
// Sử dụng GPIO_NUM_* thay vì CONFIG_* để tránh lỗi ép kiểu
#define AHT20_SDA_PIN   GPIO_NUM_17
#define AHT20_SCL_PIN   GPIO_NUM_18
#define AHT20_I2C_ADDR  0x38

// ===== CẢM BIẾN KHOẢNG CÁCH =====
#define ULTRASONIC_SCL_PIN GPIO_NUM_39
#define ULTRASONIC_SDA_PIN GPIO_NUM_40

// ===== CẢM BIẾN PIR =====
#define PIR_MOTION_SENSOR_PIN GPIO_NUM_3

// ===== ĐỘNG CƠ DRV8833 =====
#define DRV8833_IN1 GPIO_NUM_1
#define DRV8833_IN2 GPIO_NUM_2
#define DRV8833_IN3 GPIO_NUM_21
#define DRV8833_IN4 GPIO_NUM_8

// ===== LED =====
#define LED_1 GPIO_NUM_10
#define LED_2 GPIO_NUM_11

// ===== MICROSD =====
#define SD_SPI_MISO_PIN GPIO_NUM_12
#define SD_SPI_MOSI_PIN GPIO_NUM_13
#define SD_SPI_SCK_PIN  GPIO_NUM_14
#define SD_SPI_CS_PIN   GPIO_NUM_NC

// ===== SẠC PIN =====
#define POWER_CHARGE_DETECT_PIN GPIO_NUM_NC
#define POWER_ADC_UNIT          ADC_UNIT_1
#define POWER_ADC_CHANNEL       ADC_CHANNEL_2

// ===== NÚT BOOT =====
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// ===== KHÔNG DÙNG =====
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_NC
#define LAMP_GPIO               GPIO_NUM_NC
#define BUILTIN_LED_GPIO        GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
