#include "wifi_board.h"
#include "max98357a_codec.h"
#include "face_display.h"  // THÊM: Include face display
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
#include <string>

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
    FaceDisplay* face_display_ = nullptr;  // THÊM: Face display
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Button volume_up_button_;
    Button volume_down_button_;
    SensorController* sensor_controller_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    int current_volume_ = 80;

    // LED animation variables
    LedAnimation anim_led1_ = {PATTERN_OFF, 5, 255, 0, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, 255, 0, false};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    bool face_auto_mode_ = true;  // THÊM: Face auto mode
    
    // LED timeout
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;
    const int DEFAULT_LED_DURATION_ = 30;

    friend class SensorController;

    // ===== FACE UPDATE =====
    void UpdateFaceByState() {
        if (!face_display_ || !face_auto_mode_) return;
        
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        
        switch (state) {
            case kDeviceStateIdle:
                face_display_->ShowNeutral();
                break;
            case kDeviceStateConnecting:
                face_display_->ShowThinking();
                break;
            case kDeviceStateListening:
                face_display_->ShowListening();
                break;
            case kDeviceStateSpeaking:
                face_display_->ShowSpeaking();
                break;
            case kDeviceStateStarting:
                face_display_->ShowBlink();
                break;
            default:
                face_display_->ShowNeutral();
                break;
        }
    }

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
        if (!anim.active) {
            gpio_set_level((gpio_num_t)led_pin, 0);
            return;
        }
        
        int brightness = 0;
        uint32_t time = led_tick_;
        
        switch (anim.pattern) {
            case PATTERN_OFF: brightness = 0; break;
            case PATTERN_BREATH: brightness = BreathEffect(time, anim.speed); break;
            case PATTERN_BLINK_FAST: brightness = (time % (100 / anim.speed)) < 50 ? 255 : 0; break;
            case PATTERN_BLINK_SLOW: brightness = (time % (500 / anim.speed)) < 250 ? 255 : 0; break;
            case PATTERN_HEARTBEAT: brightness = HeartbeatEffect(time); break;
            case PATTERN_WAVE: brightness = WaveEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1); break;
            case PATTERN_COMET: brightness = CometEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1); break;
            case PATTERN_PULSE: brightness = PulseEffect(time, anim.speed, 0); break;
            case PATTERN_TWINKLE: brightness = TwinkleEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1); break;
            default: brightness = 0; break;
        }
        
        if (brightness > 200) {
            gpio_set_level((gpio_num_t)led_pin, 1);
        } else if (brightness > 50) {
            gpio_set_level((gpio_num_t)led_pin, 1);
        } else {
            gpio_set_level((gpio_num_t)led_pin, 0);
        }
    }

    void SetLedTimeout(int duration_seconds) {
        if (duration_seconds <= 0) {
            led_timeout_ms_ = 0;
        } else {
            led_timeout_ms_ = duration_seconds * 1000;
            led_timeout_start_ = led_tick_;
        }
    }

    void UpdateLedCreative() {
        led_tick_ += 50;
        
        // Cập nhật khuôn mặt mỗi 500ms
        if (led_tick_ % 500 == 0) {
            UpdateFaceByState();
        }
        
        if (led_timeout_ms_ > 0) {
            if (led_tick_ - led_timeout_start_ >= led_timeout_ms_) {
                led_timeout_ms_ = 0;
                led_auto_mode_ = true;
                anim_led1_.active = true;
                anim_led2_.active = true;
                ESP_LOGI(TAG, "LED timeout, trở về chế độ tự động");
            }
        }
        
        if (led_auto_mode_) {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            
            switch (state) {
                case kDeviceStateIdle:
                    anim_led1_.pattern = PATTERN_BREATH;
                    anim_led1_.speed = 3;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_OFF;
                    anim_led2_.active = false;
                    break;
                case kDeviceStateConnecting:
                    anim_led1_.pattern = PATTERN_PULSE;
                    anim_led1_.speed = 8;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_PULSE;
                    anim_led2_.speed = 8;
                    anim_led2_.active = true;
                    break;
                case kDeviceStateListening:
                    anim_led1_.pattern = PATTERN_BLINK_FAST;
                    anim_led1_.speed = 10;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_WAVE;
                    anim_led2_.speed = 7;
                    anim_led2_.active = true;
                    break;
                case kDeviceStateSpeaking:
                    anim_led1_.pattern = PATTERN_HEARTBEAT;
                    anim_led1_.speed = 5;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_BREATH;
                    anim_led2_.speed = 4;
                    anim_led2_.active = true;
                    break;
                case kDeviceStateStarting:
                    anim_led1_.pattern = PATTERN_BLINK_SLOW;
                    anim_led1_.speed = 5;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_BLINK_SLOW;
                    anim_led2_.speed = 5;
                    anim_led2_.active = true;
                    break;
                default:
                    anim_led1_.pattern = PATTERN_BLINK_FAST;
                    anim_led1_.speed = 12;
                    anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_BLINK_FAST;
                    anim_led2_.speed = 12;
                    anim_led2_.active = true;
                    break;
            }
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

    // ===== ĐỘNG CƠ DRV8833 =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833 with PWM");
        
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&timer);
        
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
    }
    
    void SetLeftMotor(int speed) {
        speed = std::max(-100, std::min(100, speed));
        
        if (speed > 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        } else if (speed < 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (-speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }
    }
    
    void SetRightMotor(int speed) {
        speed = std::max(-100, std::min(100, speed));
        
        if (speed > 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, (speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        } else if (speed < 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, (-speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        }
    }

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

    void InitializeUltrasonic() {
        ESP_LOGI(TAG, "Initialize Ultrasonic Sensor");
    }

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

    // ===== MCP: MOTOR =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.left", "Điều khiển động cơ trái (speed: -100 đến 100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -100, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                return "Motor trái: " + std::to_string(speed) + "%";
            });
            
        mcp.AddTool("self.motor.right", "Điều khiển động cơ phải (speed: -100 đến 100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 0, -100, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetRightMotor(speed);
                return "Motor phải: " + std::to_string(speed) + "%";
            });
            
        mcp.AddTool("self.motor.stop", "Dừng tất cả động cơ",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetLeftMotor(0);
                SetRightMotor(0);
                return "Đã dừng động cơ";
            });
            
        mcp.AddTool("self.motor.forward", "Robot tiến (speed: 0-100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(speed);
                return "Robot tiến với tốc độ " + std::to_string(speed) + "%";
            });
            
        mcp.AddTool("self.motor.backward", "Robot lùi (speed: 0-100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = -p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(speed);
                return "Robot lùi với tốc độ " + std::to_string(-speed) + "%";
            });
            
        mcp.AddTool("self.motor.turn_left", "Robot rẽ trái (speed: 0-100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(speed);
                return "Robot rẽ trái";
            });
            
        mcp.AddTool("self.motor.turn_right", "Robot rẽ phải (speed: 0-100)",
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(-speed);
                return "Robot rẽ phải";
            });
    }

    // ===== MCP: AUDIO =====
    void InitializeVolumeMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.audio.volume_set", "Đặt âm lượng (0-100)",
            PropertyList({Property("volume", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int volume = p["volume"].value<int>();
                current_volume_ = std::max(0, std::min(100, volume));
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Đã đặt âm lượng: " + std::to_string(current_volume_) + "%";
            });
            
        mcp.AddTool("self.audio.volume_up", "Tăng âm lượng lên 10%",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = std::min(100, current_volume_ + 10);
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Âm lượng: " + std::to_string(current_volume_) + "%";
            });
            
        mcp.AddTool("self.audio.volume_down", "Giảm âm lượng xuống 10%",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = std::max(0, current_volume_ - 10);
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Âm lượng: " + std::to_string(current_volume_) + "%";
            });
            
        mcp.AddTool("self.audio.mute", "Tắt tiếng",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (audio_codec_) audio_codec_->SetOutputVolume(0);
                return "Đã tắt tiếng";
            });
            
        mcp.AddTool("self.audio.unmute", "Bật tiếng",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Đã bật tiếng, âm lượng: " + std::to_string(current_volume_) + "%";
            });
    }

    // ===== MCP: FACE =====
    void InitializeFaceMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.face.set", "Đặt biểu cảm khuôn mặt",
            PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
            [this](const PropertyList& p) -> ReturnValue {
                std::string emotion = p["emotion"].value<std::string>();
                face_auto_mode_ = false;
                if (face_display_) face_display_->SetEmotion(emotion);
                return "Đã hiển thị: " + emotion;
            });
            
        mcp.AddTool("self.face.happy", "Hiển thị mặt vui",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                face_auto_mode_ = false;
                if (face_display_) face_display_->ShowHappy();
                return "Đang vui vẻ!";
            });
            
        mcp.AddTool("self.face.sad", "Hiển thị mặt buồn",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                face_auto_mode_ = false;
                if (face_display_) face_display_->ShowSad();
                return "Đang buồn...";
            });
            
        mcp.AddTool("self.face.auto", "Khuôn mặt tự động theo trạng thái AI",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                face_auto_mode_ = true;
                UpdateFaceByState();
                return "Đã chuyển sang chế độ tự động";
            });
    }

    // ===== MCP: LED BASIC =====
    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.on", "Bật LED 1 (duration: -1=vĩnh viễn, 0=mặc định 30s)",
            PropertyList({Property("duration", kPropertyTypeInteger, DEFAULT_LED_DURATION_, -1, 3600)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int duration = p["duration"].value<int>();
                anim_led1_.active = false;
                
                if (duration == -1) {
                    SetLedTimeout(0);
                    gpio_set_level(LED_1, 1);
                    return "LED 1 bật vĩnh viễn";
                } else if (duration == 0) {
                    duration = DEFAULT_LED_DURATION_;
                }
                
                gpio_set_level(LED_1, 1);
                SetLedTimeout(duration);
                return "LED 1 bật trong " + std::to_string(duration) + " giây";
            });
            
        mcp.AddTool("self.led.off", "Tắt LED 1",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                led_timeout_ms_ = 0;
                gpio_set_level(LED_1, 0);
                return "Đã tắt LED 1";
            });
            
        mcp.AddTool("self.led2.on", "Bật LED 2 (duration: -1=vĩnh viễn, 0=mặc định 30s)",
            PropertyList({Property("duration", kPropertyTypeInteger, DEFAULT_LED_DURATION_, -1, 3600)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int duration = p["duration"].value<int>();
                anim_led2_.active = false;
                
                if (duration == -1) {
                    SetLedTimeout(0);
                    gpio_set_level(LED_2, 1);
                    return "LED 2 bật vĩnh viễn";
                } else if (duration == 0) {
                    duration = DEFAULT_LED_DURATION_;
                }
                
                gpio_set_level(LED_2, 1);
                SetLedTimeout(duration);
                return "LED 2 bật trong " + std::to_string(duration) + " giây";
            });
            
        mcp.AddTool("self.led2.off", "Tắt LED 2",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_2, 0);
                return "Đã tắt LED 2";
            });
    }

    // ===== MCP: LED EFFECTS =====
    void InitializeLedEffectsMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.breath", "LED thở",
            PropertyList({
                Property("speed", kPropertyTypeInteger, 3, 1, 10),
                Property("duration", kPropertyTypeInteger, DEFAULT_LED_DURATION_, -1, 3600)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int speed = p["speed"].value<int>();
                int duration = p["duration"].value<int>();
                
                if (duration == -1) SetLedTimeout(0);
                else if (duration == 0) { duration = DEFAULT_LED_DURATION_; SetLedTimeout(duration); }
                else SetLedTimeout(duration);
                
                anim_led1_.pattern = PATTERN_BREATH;
                anim_led1_.speed = speed;
                anim_led1_.active = true;
                anim_led2_.pattern = PATTERN_OFF;
                anim_led2_.active = false;
                
                return duration == -1 ? "LED thở vĩnh viễn" : "LED thở trong " + std::to_string(duration) + " giây";
            });
            
        mcp.AddTool("self.led.blink", "LED nhấp nháy",
            PropertyList({
                Property("speed", kPropertyTypeInteger, 5, 1, 20),
                Property("duration", kPropertyTypeInteger, DEFAULT_LED_DURATION_, -1, 3600)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int speed = p["speed"].value<int>();
                int duration = p["duration"].value<int>();
                
                if (duration == -1) SetLedTimeout(0);
                else if (duration == 0) { duration = DEFAULT_LED_DURATION_; SetLedTimeout(duration); }
                else SetLedTimeout(duration);
                
                anim_led1_.pattern = PATTERN_BLINK_FAST;
                anim_led1_.speed = speed;
                anim_led1_.active = true;
                anim_led2_.pattern = PATTERN_BLINK_FAST;
                anim_led2_.speed = speed;
                anim_led2_.active = true;
                
                return duration == -1 ? "LED nhấp nháy vĩnh viễn" : "LED nhấp nháy trong " + std::to_string(duration) + " giây";
            });
            
        mcp.AddTool("self.led.heartbeat", "LED nhịp tim",
            PropertyList({
                Property("duration", kPropertyTypeInteger, DEFAULT_LED_DURATION_, -1, 3600)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int duration = p["duration"].value<int>();
                
                if (duration == -1) SetLedTimeout(0);
                else if (duration == 0) { duration = DEFAULT_LED_DURATION_; SetLedTimeout(duration); }
                else SetLedTimeout(duration);
                
                anim_led1_.pattern = PATTERN_HEARTBEAT;
                anim_led1_.active = true;
                anim_led2_.pattern = PATTERN_OFF;
                anim_led2_.active = false;
                
                return duration == -1 ? "LED nhịp tim vĩnh viễn" : "LED nhịp tim trong " + std::to_string(duration) + " giây";
            });
            
        mcp.AddTool("self.led.auto", "LED tự động theo trạng thái AI",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                led_timeout_ms_ = 0;
                anim_led1_.active = true;
                anim_led2_.active = true;
                return "LED đã chuyển sang chế độ tự động";
            });
            
        mcp.AddTool("self.led.off_all", "Tắt tất cả LED",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                led_timeout_ms_ = 0;
                anim_led1_.pattern = PATTERN_OFF;
                anim_led1_.active = false;
                anim_led2_.pattern = PATTERN_OFF;
                anim_led2_.active = false;
                gpio_set_level(LED_1, 0);
                gpio_set_level(LED_2, 0);
                return "Đã tắt tất cả LED";
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
        InitializeLedEffectsMcp();
        InitializeVolumeMcp();
        InitializeFaceMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 4096, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        face_display_ = new FaceDisplay(display_);
        face_display_->ShowNeutral();
#endif

        InitializeButtons();
        InitializeTools();
        
        audio_codec_ = GetAudioCodec();
        if (audio_codec_) {
            audio_codec_->SetOutputVolume(current_volume_);
        }
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
    }

    virtual Led* GetLed() override {
        static SingleLed led(LED_1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Max98357aCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_BCLK,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_LRCK,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_DOUT,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_SCK,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_WS,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_DIN
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

// ===== SENSOR CONTROLLER =====
SensorController::SensorController(WkEsp32s3Dev* board) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.sensor.motion_detected", "Kiểm tra chuyển động",
        PropertyList(),
        [board](const PropertyList& p) -> ReturnValue {
            return board->ReadMotionDetected() ? "Có chuyển động" : "Không có chuyển động";
        });
}

DECLARE_BOARD(WkEsp32s3Dev);
