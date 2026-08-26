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
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== SENSOR CONTROLLER =====
class SensorController {
public:
    SensorController(WkEsp32s3Dev* board);
};

// ===== LED PATTERNS =====
enum LedPattern {
    PATTERN_OFF = 0,
    PATTERN_BREATH,
    PATTERN_BLINK_FAST,
    PATTERN_BLINK_SLOW,
    PATTERN_HEARTBEAT,
    PATTERN_WAVE,
    PATTERN_COMET,
    PATTERN_PULSE,
    PATTERN_TWINKLE,
};

struct LedAnimation {
    LedPattern pattern;
    int speed;
    int brightness;
    uint32_t start_time;
    bool active;
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

    // LED animation variables
    LedAnimation anim_led1_ = {PATTERN_OFF, 5, 255, 0, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, 255, 0, false};
    uint32_t led_tick_ = 0;

    // PWM variables for motor speed control
    ledc_channel_t motor_pwm_channel_left = LEDC_CHANNEL_0;
    ledc_channel_t motor_pwm_channel_right = LEDC_CHANNEL_1;
    ledc_timer_t motor_pwm_timer = LEDC_TIMER_0;
    uint32_t motor_pwm_freq_hz = 5000;  // 5kHz PWM frequency
    uint8_t motor_pwm_resolution = LEDC_TIMER_8_BIT;  // 8-bit resolution (0-255)
    int current_left_speed = 0;
    int current_right_speed = 0;

    friend class SensorController;

    // ===== LED EFFECT FUNCTIONS =====
    int BreathEffect(uint32_t time_ms, int speed) {
        float period = 2000.0f / speed;
        float phase = (time_ms % (int)period) / period * 2 * 3.14159f;
        return (int)((sin(phase) + 1) / 2 * 255);
    }

    int HeartbeatEffect(uint32_t time_ms) {
        uint32_t cycle = time_ms % 1000;
        if (cycle < 100) return 255;
        else if (cycle < 200) return 50;
        else if (cycle < 300) return 255;
        else if (cycle < 400) return 50;
        else return 0;
    }

    int WaveEffect(uint32_t time_ms, int speed, int led_index) {
        float period = 1500.0f / speed;
        float phase = (time_ms % (int)period) / period * 2 * 3.14159f;
        float phase_offset = led_index == 0 ? 0 : 3.14159f;
        return (int)((sin(phase + phase_offset) + 1) / 2 * 255);
    }

    int CometEffect(uint32_t time_ms, int speed, int led_index) {
        int cycle_time = 3000 / speed;
        int pos = (time_ms % cycle_time) * 255 / cycle_time;
        int brightness = 0;
        if (pos > 200) brightness = 255;
        else if (pos > 150) brightness = (pos - 150) * 5;
        return led_index == 0 ? brightness : brightness / 2;
    }

    int TwinkleEffect(uint32_t time_ms, int speed, int led_index) {
        uint32_t seed = (time_ms / (200 / speed)) + led_index * 1000;
        uint32_t random = (seed * 1103515245 + 12345) & 0x7fffffff;
        return (random % 256) > 200 ? 255 : (random % 256) > 100 ? 128 : 0;
    }

    int PulseEffect(uint32_t time_ms, int speed, int led_index) {
        int pulse_width = 200 / speed;
        uint32_t cycle = time_ms % (pulse_width * 4);
        if (cycle < pulse_width) {
            return (cycle * 255) / pulse_width;
        } else if (cycle < pulse_width * 2) {
            return 255 - ((cycle - pulse_width) * 255 / pulse_width);
        } else {
            return 0;
        }
    }

    void ApplyLedEffect(int led_pin, LedAnimation anim) {
        if (!anim.active) return;
        
        int brightness = 0;
        uint32_t time = led_tick_;
        
        switch (anim.pattern) {
            case PATTERN_OFF:
                brightness = 0;
                break;
            case PATTERN_BREATH:
                brightness = BreathEffect(time, anim.speed);
                break;
            case PATTERN_BLINK_FAST:
                brightness = (time % (100 / anim.speed)) < 50 ? 255 : 0;
                break;
            case PATTERN_BLINK_SLOW:
                brightness = (time % (500 / anim.speed)) < 250 ? 255 : 0;
                break;
            case PATTERN_HEARTBEAT:
                brightness = HeartbeatEffect(time);
                break;
            case PATTERN_WAVE:
                brightness = WaveEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1);
                break;
            case PATTERN_COMET:
                brightness = CometEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1);
                break;
            case PATTERN_PULSE:
                brightness = PulseEffect(time, anim.speed, 0);
                break;
            case PATTERN_TWINKLE:
                brightness = TwinkleEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1);
                break;
            default:
                brightness = 0;
                break;
        }
        
        if (brightness > 200) {
            gpio_set_level((gpio_num_t)led_pin, 1);
        } else if (brightness > 50) {
            gpio_set_level((gpio_num_t)led_pin, 1);
        } else {
            gpio_set_level((gpio_num_t)led_pin, 0);
        }
    }

    void UpdateLedCreative() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    led_tick_ += 50;
    
    switch (state) {
        case kDeviceStateIdle:
            // 🌙 IDLE: LED 1 thở chậm, LED 2 tắt
            anim_led1_.pattern = PATTERN_BREATH;
            anim_led1_.speed = 3;
            anim_led2_.pattern = PATTERN_OFF;
            break;
            
        case kDeviceStateConnecting:
            // 📡 CONNECTING: cả 2 LED xung nhịp
            anim_led1_.pattern = PATTERN_PULSE;
            anim_led1_.speed = 8;
            anim_led2_.pattern = PATTERN_PULSE;
            anim_led2_.speed = 8;
            break;
            
        case kDeviceStateListening:
            // 🎤 LISTENING: LED 1 nhấp nháy nhanh, LED 2 sóng
            anim_led1_.pattern = PATTERN_BLINK_FAST;
            anim_led1_.speed = 10;
            anim_led2_.pattern = PATTERN_WAVE;
            anim_led2_.speed = 7;
            break;
            
        case kDeviceStateSpeaking:
            // 💬 SPEAKING: LED 1 nhịp tim, LED 2 thở
            anim_led1_.pattern = PATTERN_HEARTBEAT;
            anim_led1_.speed = 5;
            anim_led2_.pattern = PATTERN_BREATH;
            anim_led2_.speed = 4;
            break;
            
        case kDeviceStateStarting:
            // 🚀 STARTING: LED 1 + LED 2 nhấp nháy chậm
            anim_led1_.pattern = PATTERN_BLINK_SLOW;
            anim_led1_.speed = 5;
            anim_led2_.pattern = PATTERN_BLINK_SLOW;
            anim_led2_.speed = 5;
            break;
            
        default:
            // ❌ UNKNOWN: cả 2 LED nhấp nháy nhanh (báo lỗi)
            anim_led1_.pattern = PATTERN_BLINK_FAST;
            anim_led1_.speed = 12;
            anim_led2_.pattern = PATTERN_BLINK_FAST;
            anim_led2_.speed = 12;
            break;
    }
    
    ApplyLedEffect(LED_1, anim_led1_);
    ApplyLedEffect(LED_2, anim_led2_);
}
    static void LedCreativeTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        while (1) {
            board->UpdateLedCreative();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // ===== PWM MOTOR CONTROL FUNCTIONS =====
    void SetMotorSpeed(int left_speed, int right_speed) {
        // Clamp speeds to -255 to 255
        left_speed = std::max(-255, std::min(255, left_speed));
        right_speed = std::max(-255, std::min(255, right_speed));
        
        current_left_speed = left_speed;
        current_right_speed = right_speed;
        
        // Control left motor with PWM
        if (left_speed > 0) {
            gpio_set_level(DRV8833_IN1, 1);
            gpio_set_level(DRV8833_IN2, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left, left_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left);
        } else if (left_speed < 0) {
            gpio_set_level(DRV8833_IN1, 0);
            gpio_set_level(DRV8833_IN2, 1);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left, -left_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left);
        } else {
            gpio_set_level(DRV8833_IN1, 0);
            gpio_set_level(DRV8833_IN2, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_left);
        }
        
        // Control right motor with PWM
        if (right_speed > 0) {
            gpio_set_level(DRV8833_IN3, 1);
            gpio_set_level(DRV8833_IN4, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right, right_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right);
        } else if (right_speed < 0) {
            gpio_set_level(DRV8833_IN3, 0);
            gpio_set_level(DRV8833_IN4, 1);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right, -right_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right);
        } else {
            gpio_set_level(DRV8833_IN3, 0);
            gpio_set_level(DRV8833_IN4, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_pwm_channel_right);
        }
    }

    // ===== ĐỘNG CƠ DRV8833 =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833 with PWM");
        
        // Configure GPIO pins
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << DRV8833_IN1) | (1ULL << DRV8833_IN2) | 
                            (1ULL << DRV8833_IN3) | (1ULL << DRV8833_IN4),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        
        // Initialize PWM for motor speed control
        ledc_timer_config_t timer_conf = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = motor_pwm_resolution,
            .timer_num = motor_pwm_timer,
            .freq_hz = motor_pwm_freq_hz,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));
        
        // Left motor PWM channel (using IN1)
        ledc_channel_config_t channel_conf_left = {
            .gpio_num = DRV8833_IN1,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motor_pwm_channel_left,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = motor_pwm_timer,
            .duty = 0,
            .hpoint = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_conf_left));
        
        // Right motor PWM channel (using IN3)
        ledc_channel_config_t channel_conf_right = {
            .gpio_num = DRV8833_IN3,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motor_pwm_channel_right,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = motor_pwm_timer,
            .duty = 0,
            .hpoint = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_conf_right));
        
        // Initial state: motors stopped
        gpio_set_level(DRV8833_IN1, 0);
        gpio_set_level(DRV8833_IN2, 0);
        gpio_set_level(DRV8833_IN3, 0);
        gpio_set_level(DRV8833_IN4, 0);
        SetMotorSpeed(0, 0);
        
        ESP_LOGI(TAG, "Motor PWM initialized: freq=%dHz, resolution=%d-bit", 
                 motor_pwm_freq_hz, motor_pwm_resolution);
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

    // ===== ADC (PIN) =====
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
        
        mcp.AddTool("self.motor.left", "Điều khiển động cơ trái với tốc độ (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, current_right_speed);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ trái: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.right", "Điều khiển động cơ phải với tốc độ (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(current_left_speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ phải: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.stop", "Dừng tất cả động cơ",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetMotorSpeed(0, 0);
                return "Đã dừng động cơ";
            });
            
        mcp.AddTool("self.motor.forward", "Robot tiến về phía trước với tốc độ (speed: 1-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, 1, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed < 1) speed = 200;  // Default speed
                SetMotorSpeed(speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang tiến với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.backward", "Robot lùi về phía sau với tốc độ (speed: 1-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, 1, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed < 1) speed = 200;
                SetMotorSpeed(-speed, -speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang lùi với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.turn_left", "Robot rẽ trái với tốc độ (speed: 1-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, 1, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed < 1) speed = 200;
                SetMotorSpeed(-speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang rẽ trái với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.turn_right", "Robot rẽ phải với tốc độ (speed: 1-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, 1, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                if (speed < 1) speed = 200;
                SetMotorSpeed(speed, -speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang rẽ phải với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.speed", "Đặt tốc độ cho cả 2 động cơ cùng lúc (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ: %d", speed);
                return std::string(result);
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

        // Start creative LED task (replaces old LedStateTask)
        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 4096, this, 5, nullptr);

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
        // Keep tools
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
