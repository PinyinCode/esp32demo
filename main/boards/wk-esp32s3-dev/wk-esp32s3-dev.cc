#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include <wifi_station.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif
#if defined(LCD_TYPE_NV3030B_SERIAL)
#include "esp_lcd_nv3030b.h"
#endif
#if defined(LCD_TYPE_ILI9486_SERIAL)
#include "esp_lcd_ili9486.h"
#endif
#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0, (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C, 0x04, 0x12, 0x14, 0x1f}, 14, 0},
    {0xf1, (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D, 0x0C, 0x1A, 0x14, 0x1E}, 14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== SENSOR CONTROLLER =====
class SensorController {
public:
    SensorController(WkEsp32s3Dev* board);
};

class WkEsp32s3Dev : public WifiBoard {
private:
    Button boot_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Button volume_up_button_;
    Button volume_down_button_;
    SensorController* sensor_controller_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;

    friend class SensorController;

    // ===== ĐÈN CHỚP THEO TRẠNG THÁI AI =====
    void UpdateLampByState() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        
        switch (state) {
            case kDeviceStateListening:
                gpio_set_level(LAMP_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LAMP_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case kDeviceStateSpeaking:
                gpio_set_level(LAMP_GPIO, 1);
                break;
            case kDeviceStateIdle:
                gpio_set_level(LAMP_GPIO, 0);
                break;
            default:
                break;
        }
    }

    static void LampStateTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        while (1) {
            board->UpdateLampByState();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // ===== MOTOR =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor (2 Servos)");
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&timer);

        ledc_channel_config_t ch1 = {
            .gpio_num = GPIO_NUM_5,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        ledc_channel_config(&ch1);

        ledc_channel_config_t ch2 = {
            .gpio_num = GPIO_NUM_4,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        ledc_channel_config(&ch2);
    }

    // ===== PIR =====
    void InitializePirSensor() {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << GPIO_NUM_3,
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&io_conf);
    }

    // ===== ULTRASONIC =====
    void InitializeUltrasonic() {
        gpio_config_t trig_conf = {
            .pin_bit_mask = 1ULL << GPIO_NUM_18,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&trig_conf);

        gpio_config_t echo_conf = {
            .pin_bit_mask = 1ULL << GPIO_NUM_17,
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&echo_conf);
    }

    // ===== LAMP GPIO =====
    void InitializeLampGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << LAMP_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        gpio_set_level(LAMP_GPIO, 0);
    }

    // ===== ADC (PIN) =====
    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = POWER_ADC_UNIT,
        };
        adc_oneshot_new_unit(&init_config, &adc_handle_);

        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(adc_handle_, POWER_ADC_CHANNEL, &config);
    }

    // ===== MCP: MOTOR =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.motor.servo1_move", "Servo 1 (0-180)",
            PropertyList({Property("angle", kPropertyTypeInteger, 90, 0, 180)}),
            [](const PropertyList& p) -> ReturnValue {
                int angle = p["angle"].value<int>();
                uint32_t duty = (angle * 1023 / 180) + 26;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                return true;
            });
        mcp.AddTool("self.motor.servo2_move", "Servo 2 (0-180)",
            PropertyList({Property("angle", kPropertyTypeInteger, 90, 0, 180)}),
            [](const PropertyList& p) -> ReturnValue {
                int angle = p["angle"].value<int>();
                uint32_t duty = (angle * 1023 / 180) + 26;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                return true;
            });
        mcp.AddTool("self.motor.servo_reset", "Reset servo về 90 độ",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                uint32_t duty = (90 * 1023 / 180) + 26;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                return true;
            });
    }

    // ===== MCP: LAMP =====
    void InitializeLampMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.lamp.on", "Bật đèn",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LAMP_GPIO, 1);
                return "Đã bật đèn";
            });
        mcp.AddTool("self.lamp.off", "Tắt đèn",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LAMP_GPIO, 0);
                return "Đã tắt đèn";
            });
        mcp.AddTool("self.lamp.toggle", "Bật/tắt đèn",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                static bool state = false;
                state = !state;
                gpio_set_level(LAMP_GPIO, state ? 1 : 0);
                return state ? "Đèn đang bật" : "Đèn đang tắt";
            });
        mcp.AddTool("self.lamp.status", "Trạng thái đèn",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                return gpio_get_level(LAMP_GPIO) ? "Đèn đang bật" : "Đèn đang tắt";
            });
    }

    // ===== MCP: BATTERY =====
    void InitializeBatteryMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.battery.level", "Mức pin (%)",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                int adc_value = 0;
                adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value);
                int level = (adc_value * 100) / 4095;
                char result[32];
                snprintf(result, sizeof(result), "%d%%", level);
                return std::string(result);
            });
        mcp.AddTool("self.battery.charging", "Kiểm tra đang sạc",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                return gpio_get_level((gpio_num_t)POWER_CHARGE_DETECT_PIN) == 1 ? "Đang sạc" : "Không sạc";
            });
    }

    // ===== MCP: SENSOR =====
    void InitializeSensorMcp() {
        sensor_controller_ = new SensorController(this);
    }

public:
    WkEsp32s3Dev() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

#ifdef CONFIG_BOARD_HAVE_MOTOR_CONTROL
        InitializeMotor();
        InitializeMotorMcp();
#endif

        InitializePirSensor();
        InitializeUltrasonic();
        InitializeSensorMcp();
        InitializeLampGpio();
        InitializeLampMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        // ===== TASK ĐÈN CHỚP THEO TRẠNG THÁI =====
        xTaskCreate(LampStateTask, "lamp_state", 2048, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
#elif CONFIG_WK_ESP32S3_DEV_DISPLAY_LCD
        InitializeSpi();
        InitializeLcdDisplay();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
#endif
        InitializeButtons();
        InitializeTools();
    }

    // ===== ĐỌC CẢM BIẾN =====
    bool ReadMotionDetected() {
        return gpio_get_level(GPIO_NUM_3) == 1;
    }

    float ReadDistanceCm() {
        gpio_set_level(GPIO_NUM_18, 0);
        esp_rom_delay_us(2);
        gpio_set_level(GPIO_NUM_18, 1);
        esp_rom_delay_us(10);
        gpio_set_level(GPIO_NUM_18, 0);

        int timeout = 100000;
        while (gpio_get_level(GPIO_NUM_17) == 0 && timeout > 0) timeout--;
        int64_t start = esp_timer_get_time();

        timeout = 100000;
        while (gpio_get_level(GPIO_NUM_17) == 1 && timeout > 0) timeout--;
        int64_t end = esp_timer_get_time();

        float duration = (end - start) / 1000000.0f;
        return duration * 34300.0f / 2.0f;
    }

    // ===== OLED =====
#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }
#elif CONFIG_WK_ESP32S3_DEV_DISPLAY_LCD
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

    #if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io_, &panel_config, &panel_));
    #elif defined(LCD_TYPE_ILI9486_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9486(panel_io_, &panel_config, &panel_));
    #elif defined(LCD_TYPE_NV3030B_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_nv3030b(panel_io_, &panel_config, &panel_));
    #elif defined(LCD_TYPE_GC9A01_SERIAL)
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
        panel_config.vendor_config = &gc9107_vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_, &panel_config, &panel_));
    #else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
    #endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
#endif

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
             AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_RIGHT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_LEFT);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

// ===== SENSOR CONTROLLER IMPLEMENTATION =====
SensorController::SensorController(WkEsp32s3Dev* board) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.sensor.motion_detected", "Kiểm tra chuyển động",
        PropertyList(),
        [board](const PropertyList& p) -> ReturnValue {
            return board->ReadMotionDetected() ? "Có chuyển động" : "Không có chuyển động";
        });

    mcp.AddTool("self.sensor.distance", "Đọc khoảng cách (cm)",
        PropertyList(),
        [board](const PropertyList& p) -> ReturnValue {
            char result[32];
            snprintf(result, sizeof(result), "%.1f cm", board->ReadDistanceCm());
            return std::string(result);
        });

    mcp.AddTool("self.sensor.obstacle_check", "Kiểm tra vật cản",
        PropertyList({Property("threshold_cm", kPropertyTypeInteger, 30, 5, 200)}),
        [board](const PropertyList& p) -> ReturnValue {
            float distance = board->ReadDistanceCm();
            int threshold = p["threshold_cm"].value<int>();
            return distance < threshold ? "Có vật cản" : "Không có vật cản";
        });
}

DECLARE_BOARD(WkEsp32s3Dev);
