#include "wifi_board.h"
#include "codecs/wk_audio_codec.h"  // Thay đổi: dùng codec mới
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
#include <algorithm>

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

    // Audio variables
    int current_volume_ = 85;
    AudioCodec* audio_codec_ = nullptr;

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
                anim_led1_.pattern = PATTERN_BREATH;
                anim_led1_.speed = 3;
                anim_led2_.pattern = PATTERN_OFF;
                break;
            case kDeviceStateConnecting:
                anim_led1_.pattern = PATTERN_PULSE;
                anim_led1_.speed = 8;
                anim_led2_.pattern = PATTERN_PULSE;
                anim_led2_.speed = 8;
                break;
            case kDeviceStateListening:
                anim_led1_.pattern = PATTERN_BLINK_FAST;
                anim_led1_.speed = 10;
                anim_led2_.pattern = PATTERN_WAVE;
                anim_led2_.speed = 7;
                break;
            case kDeviceStateSpeaking:
                anim_led1_.pattern = PATTERN_HEARTBEAT;
                anim_led1_.speed = 5;
                anim_led2_.pattern = PATTERN_BREATH;
                anim_led2_.speed = 4;
                break;
            case kDeviceStateStarting:
                anim_led1_.pattern = PATTERN_BLINK_SLOW;
                anim_led1_.speed = 5;
                anim_led2_.pattern = PATTERN_BLINK_SLOW;
                anim_led2_.speed = 5;
                break;
            default:
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

    // ===== AUDIO SETUP =====
    void InitializeAudio() {
        ESP_LOGI(TAG, "========== AUDIO INITIALIZATION ==========");
        ESP_LOGI(TAG, "Audio input sample rate: %d", AUDIO_INPUT_SAMPLE_RATE);
        ESP_LOGI(TAG, "Audio output sample rate: %d", AUDIO_OUTPUT_SAMPLE_RATE);
        
        audio_codec_ = GetAudioCodec();
        if (audio_codec_) {
            ESP_LOGI(TAG, "Audio codec created successfully");
            
            audio_codec_->SetOutputVolume(current_volume_);
            ESP_LOGI(TAG, "Set volume to: %d%%", current_volume_);
            
            audio_codec_->SetOutputMute(false);
            ESP_LOGI(TAG, "Audio unmuted");
            
            audio_codec_->EnableInput(true);
            audio_codec_->EnableOutput(true);
            ESP_LOGI(TAG, "Input and output enabled");
            
            // Start the codec
            audio_codec_->Start();
            ESP_LOGI(TAG, "Audio codec started");
        } else {
            ESP_LOGE(TAG, "Failed to create audio codec!");
        }
        
        auto& app = Application::GetInstance();
        app.SetVolume(current_volume_);
        
        ESP_LOGI(TAG, "Audio initialization complete. Volume: %d%%", current_volume_);
        ESP_LOGI(TAG, "I2S pins:");
        ESP_LOGI(TAG, "  SPK: BCLK=%d, LRCK=%d, DOUT=%d", 
                 AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT);
        ESP_LOGI(TAG, "  MIC: SCK=%d, WS=%d, DIN=%d", 
                 AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        ESP_LOGI(TAG, "===========================================");
    }

    // ===== ĐỘNG CƠ DRV8833 với PWM =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833 with PWM");
        
        // Configure PWM for motor speed control
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&timer);
        
        // Left motor PWM channels (IN1 and IN2)
        ledc_channel_config_t ch1 = {
            .gpio_num = DRV8833_IN1,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        };
        ledc_channel_config(&ch1);
        
        ledc_channel_config_t ch2 = {
            .gpio_num = DRV8833_IN2,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        };
        ledc_channel_config(&ch2);
        
        // Right motor PWM channels (IN3 and IN4)
        ledc_channel_config_t ch3 = {
            .gpio_num = DRV8833_IN3,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_2,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        };
        ledc_channel_config(&ch3);
        
        ledc_channel_config_t ch4 = {
            .gpio_num = DRV8833_IN4,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_3,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        };
        ledc_channel_config(&ch4);
        
        // Initial state: motors stopped
        SetMotorSpeed(0, 0);
        
        ESP_LOGI(TAG, "Motor PWM initialized with 5kHz, 8-bit resolution");
    }
    
    void SetMotorSpeed(int left_speed, int right_speed) {
        // Clamp speeds to -255 to 255
        left_speed = std::max(-255, std::min(255, left_speed));
        right_speed = std::max(-255, std::min(255, right_speed));
        
        // Control left motor with PWM
        if (left_speed > 0) {
            gpio_set_level(DRV8833_IN1, 1);
            gpio_set_level(DRV8833_IN2, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, left_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        } else if (left_speed < 0) {
            gpio_set_level(DRV8833_IN1, 0);
            gpio_set_level(DRV8833_IN2, 1);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, -left_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        } else {
            gpio_set_level(DRV8833_IN1, 0);
            gpio_set_level(DRV8833_IN2, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }
        
        // Control right motor with PWM
        if (right_speed > 0) {
            gpio_set_level(DRV8833_IN3, 1);
            gpio_set_level(DRV8833_IN4, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, right_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        } else if (right_speed < 0) {
            gpio_set_level(DRV8833_IN3, 0);
            gpio_set_level(DRV8833_IN4, 1);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, -right_speed);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        } else {
            gpio_set_level(DRV8833_IN3, 0);
            gpio_set_level(DRV8833_IN4, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        }
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
        ESP_LOGI(TAG, "PIR sensor initialized on GPIO %d", PIR_MOTION_SENSOR_PIN);
    }

    // ===== CẢM BIẾN KHOẢNG CÁCH I2C =====
    void InitializeUltrasonic() {
        ESP_LOGI(TAG, "Initialize Ultrasonic Sensor (I2C: SCL=%d, SDA=%d)", 
                 ULTRASONIC_SCL_PIN, ULTRASONIC_SDA_PIN);
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
        ESP_LOGI(TAG, "LEDs initialized: LED1=%d, LED2=%d", LED_1, LED_2);
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
        ESP_LOGI(TAG, "ADC initialized for battery monitoring");
    }

    // ===== MCP: AUDIO =====
    void InitializeAudioMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.audio.volume", "Đặt âm lượng (0-100)",
            PropertyList({Property("level", kPropertyTypeInteger, 85, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int level = p["level"].value<int>();
                level = std::max(0, std::min(100, level));
                current_volume_ = level;
                
                if (audio_codec_) {
                    audio_codec_->SetOutputVolume(level);
                }
                auto& app = Application::GetInstance();
                app.SetVolume(level);
                
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt âm lượng: %d%%", level);
                return std::string(result);
            });
            
        mcp.AddTool("self.audio.volume_up", "Tăng âm lượng lên 10%%",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = std::min(100, current_volume_ + 10);
                if (audio_codec_) {
                    audio_codec_->SetOutputVolume(current_volume_);
                }
                auto& app = Application::GetInstance();
                app.SetVolume(current_volume_);
                
                char result[64];
                snprintf(result, sizeof(result), "Âm lượng: %d%%", current_volume_);
                return std::string(result);
            });
            
        mcp.AddTool("self.audio.volume_down", "Giảm âm lượng xuống 10%%",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = std::max(0, current_volume_ - 10);
                if (audio_codec_) {
                    audio_codec_->SetOutputVolume(current_volume_);
                }
                auto& app = Application::GetInstance();
                app.SetVolume(current_volume_);
                
                char result[64];
                snprintf(result, sizeof(result), "Âm lượng: %d%%", current_volume_);
                return std::string(result);
            });
            
        mcp.AddTool("self.audio.mute", "Tắt âm thanh",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (audio_codec_) {
                    audio_codec_->SetOutputMute(true);
                }
                auto& app = Application::GetInstance();
                app.SetVolume(0);
                return "Đã tắt âm thanh";
            });
            
        mcp.AddTool("self.audio.unmute", "Bật âm thanh",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (audio_codec_) {
                    audio_codec_->SetOutputMute(false);
                    audio_codec_->SetOutputVolume(current_volume_ > 0 ? current_volume_ : 85);
                }
                auto& app = Application::GetInstance();
                app.SetVolume(current_volume_ > 0 ? current_volume_ : 85);
                return "Đã bật âm thanh";
            });
            
        mcp.AddTool("self.audio.status", "Trạng thái âm lượng",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                char result[128];
                snprintf(result, sizeof(result), 
                    "Âm lượng: %d%% | Codec: WkAudioCodec", 
                    current_volume_);
                return std::string(result);
            });
    }

    // ===== MCP: ĐỘNG CƠ với PWM =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.left", "Điều khiển động cơ trái với tốc độ (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, 0);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ trái: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.right", "Điều khiển động cơ phải với tốc độ (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(0, speed);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ phải: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.speed", "Đặt tốc độ cho cả 2 động cơ (speed: -255 đến 255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -255, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Đã đặt tốc độ động cơ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.stop", "Dừng tất cả động cơ",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetMotorSpeed(0, 0);
                return "Đã dừng động cơ";
            });
            
        mcp.AddTool("self.motor.forward", "Robot tiến về phía trước (speed: 0-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 200, 0, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang tiến với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.backward", "Robot lùi về phía sau (speed: 0-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 200, 0, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = -p["speed"].value<int>();
                SetMotorSpeed(speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang lùi với tốc độ: %d", -speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.turn_left", "Robot rẽ trái (speed: 0-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 200, 0, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(-speed, speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang rẽ trái với tốc độ: %d", speed);
                return std::string(result);
            });
            
        mcp.AddTool("self.motor.turn_right", "Robot rẽ phải (speed: 0-255)",
            PropertyList({Property("speed", kPropertyTypeInteger, 200, 0, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetMotorSpeed(speed, -speed);
                char result[64];
                snprintf(result, sizeof(result), "Robot đang rẽ phải với tốc độ: %d", speed);
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

        ESP_LOGI(TAG, "=== WK ESP32S3 Dev Board Initializing ===");
        
        // Initialize audio first with default volume
        InitializeAudio();
        InitializeAudioMcp();

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

        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 4096, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
#endif

        InitializeButtons();
        InitializeTools();
        
        ESP_LOGI(TAG, "=== Board initialization complete ===");
        ESP_LOGI(TAG, "Audio volume: %d%%", current_volume_);
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
        ESP_LOGI(TAG, "Display I2C initialized: SDA=%d, SCL=%d", DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
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
        ESP_LOGI(TAG, "SSD1306 Display initialized: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
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
        ESP_LOGI(TAG, "Buttons initialized");
    }

    void InitializeTools() {
        // Keep tools
    }

    virtual Led* GetLed() override {
        static SingleLed led(LED_1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Sử dụng WkAudioCodec thay vì NoAudioCodec
        static WkAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,    // 16000
            AUDIO_OUTPUT_SAMPLE_RATE,   // 24000
            AUDIO_I2S_SPK_GPIO_BCLK,    // 15 - BCLK cho loa
            AUDIO_I2S_SPK_GPIO_LRCK,    // 16 - LRCK cho loa
            AUDIO_I2S_SPK_GPIO_DOUT,    // 7  - DOUT cho loa
            I2S_STD_SLOT_RIGHT,         // MAX98357A dùng Right Channel
            AUDIO_I2S_MIC_GPIO_SCK,     // 5  - SCK cho mic
            AUDIO_I2S_MIC_GPIO_WS,      // 4  - WS cho mic
            AUDIO_I2S_MIC_GPIO_DIN,     // 6  - DIN cho mic
            I2S_STD_SLOT_LEFT           // INMP441 dùng Left Channel
        );
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
