#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
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
#include <esp_adc/adc_oneshot.h>   // ← SỬA: dùng adc_oneshot
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;  // ← SỬA: dùng adc_oneshot_unit_handle_t

    friend class SensorController;

    // ===== LED TRẠNG THÁI =====
    void UpdateLedByState() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        
        switch (state) {
            case kDeviceStateListening:
                gpio_set_level(LED_1, 1);
                gpio_set_level(LED_2, 0);
                break;
            case kDeviceStateSpeaking:
                gpio_set_level(LED_1, 0);
                gpio_set_level(LED_2, 1);
                break;
            case kDeviceStateIdle:
                gpio_set_level(LED_1, 0);
                gpio_set_level(LED_2, 0);
                break;
            default:
                break;
        }
    }

    static void LedStateTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        while (1) {
            board->UpdateLedByState();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // ===== ĐỘNG CƠ DRV8833 =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833");
        
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << DRV8833_IN1) | (1ULL << DRV8833_IN2) | 
                            (1ULL << DRV8833_IN3) | (1ULL << DRV8833_IN4),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        
        gpio_set_level(DRV8833_IN1, 0);
        gpio_set_level(DRV8833_IN2, 0);
        gpio_set_level(DRV8833_IN3, 0);
        gpio_set_level(DRV8833_IN4, 0);
    }

    // ===== PIR =====
    void InitializePirSensor() {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << PIR_MOTION_SENSOR_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // ===== CẢM BIẾN KHOẢNG CÁCH I2C =====
    void InitializeUltrasonic() {
        ESP_LOGI(TAG, "Initialize Ultrasonic Sensor (I2C: SCL=39, SDA=40)");
    }

    // ===== LED GPIO =====
    void InitializeLedGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << LED_1) | (1ULL << LED_2),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(LED_1, 0);
        gpio_set_level(LED_2, 0);
    }

    // ===== ADC (PIN) - SỬA DÙNG ADC ONESHOT =====
    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = POWER_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_ADC_CHANNEL, &config));
    }

    // ===== MCP: ĐỘNG CƠ =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.left", "Điều khiển động cơ trái",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed > 0) {
                    gpio_set_level(DRV8833_IN1, 1);
                    gpio_set_level(DRV8833_IN2, 0);
                } else if (speed < 0) {
                    gpio_set_level(DRV8833_IN1, 0);
                    gpio_set_level(DRV8833_IN2, 1);
                } else {
                    gpio_set_level(DRV8833_IN1, 0);
                    gpio_set_level(DRV8833_IN2, 0);
                }
                return true;
            });
            
        mcp.AddTool("self.motor.right", "Điều khiển động cơ phải",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed > 0) {
                    gpio_set_level(DRV8833_IN3, 1);
                    gpio_set_level(DRV8833_IN4, 0);
                } else if (speed < 0) {
                    gpio_set_level(DRV8833_IN3, 0);
                    gpio_set_level(DRV8833_IN4, 1);
                } else {
                    gpio_set_level(DRV8833_IN3, 0);
                    gpio_set_level(DRV8833_IN4, 0);
                }
                return true;
            });
            
        mcp.AddTool("self.motor.stop", "Dừng tất cả động cơ",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(DRV8833_IN1, 0);
                gpio_set_level(DRV8833_IN2, 0);
                gpio_set_level(DRV8833_IN3, 0);
                gpio_set_level(DRV8833_IN4, 0);
                return "Đã dừng động cơ";
            });
            
        mcp.AddTool("self.motor.forward", "Robot tiến về phía trước",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(DRV8833_IN1, 1);
                gpio_set_level(DRV8833_IN2, 0);
                gpio_set_level(DRV8833_IN3, 1);
                gpio_set_level(DRV8833_IN4, 0);
                return "Robot đang tiến";
            });
            
        mcp.AddTool("self.motor.backward", "Robot lùi về phía sau",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(DRV8833_IN1, 0);
                gpio_set_level(DRV8833_IN2, 1);
                gpio_set_level(DRV8833_IN3, 0);
                gpio_set_level(DRV8833_IN4, 1);
                return "Robot đang lùi";
            });
            
        mcp.AddTool("self.motor.turn_left", "Robot rẽ trái",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(DRV8833_IN1, 0);
                gpio_set_level(DRV8833_IN2, 1);
                gpio_set_level(DRV8833_IN3, 1);
                gpio_set_level(DRV8833_IN4, 0);
                return "Robot đang rẽ trái";
            });
            
        mcp.AddTool("self.motor.turn_right", "Robot rẽ phải",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(DRV8833_IN1, 1);
                gpio_set_level(DRV8833_IN2, 0);
                gpio_set_level(DRV8833_IN3, 0);
                gpio_set_level(DRV8833_IN4, 1);
                return "Robot đang rẽ phải";
            });
    }

    // ===== MCP: LED =====
    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.on", "Bật đèn LED 1",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_1, 1);
                return "Đã bật LED 1";
            });
            
        mcp.AddTool("self.led.off", "Tắt đèn LED 1",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_1, 0);
                return "Đã tắt LED 1";
            });
            
        mcp.AddTool("self.led.toggle", "Bật/tắt LED 1",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                static bool state = false;
                state = !state;
                gpio_set_level(LED_1, state ? 1 : 0);
                return state ? "LED 1 đang bật" : "LED 1 đang tắt";
            });
            
        mcp.AddTool("self.led2.on", "Bật đèn LED 2",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_2, 1);
                return "Đã bật LED 2";
            });
            
        mcp.AddTool("self.led2.off", "Tắt đèn LED 2",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_2, 0);
                return "Đã tắt LED 2";
            });
            
        mcp.AddTool("self.led2.toggle", "Bật/tắt LED 2",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                static bool state = false;
                state = !state;
                gpio_set_level(LED_2, state ? 1 : 0);
                return state ? "LED 2 đang bật" : "LED 2 đang tắt";
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

#ifdef CONFIG_BOARD_WK_HAVE_MOTOR
        InitializeMotor();
        InitializeMotorMcp();
#endif

        InitializePirSensor();
        InitializeUltrasonic();
        InitializeSensorMcp();
        InitializeLedGpio();
        InitializeLedMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        xTaskCreate(LedStateTask, "led_state", 2048, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
#endif

        InitializeButtons();
        InitializeTools();
    }

    bool ReadMotionDetected() {
        return gpio_get_level(PIR_MOTION_SENSOR_PIN) == 1;
    }

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

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
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
        // Giữ nguyên tools
    }

    virtual Led* GetLed() override {
        static SingleLed led(LED_1);
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
}

DECLARE_BOARD(WkEsp32s3Dev);
