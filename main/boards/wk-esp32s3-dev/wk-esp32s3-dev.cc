#include "wifi_board.h"
#include "max98357a_codec.h"
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
#include <freertos/queue.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <vector>
#include <map>

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== ENUMS & STRUCTS =====
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
    PATTERN_RAINBOW,
    PATTERN_FIRE,
    PATTERN_COUNT
};

struct LedAnimation {
    LedPattern pattern;
    int speed;
    int brightness;
    uint32_t start_time;
    bool active;
    uint8_t hue; // For rainbow effect
};

// ===== PWM LED CONTROLLER =====
class PwmLedController {
private:
    ledc_channel_t channel_;
    ledc_timer_t timer_;
    gpio_num_t gpio_;
    int current_duty_ = 0;
    bool initialized_ = false;
    
public:
    PwmLedController() = default;
    
    void Init(gpio_num_t gpio, ledc_channel_t channel, ledc_timer_t timer) {
        gpio_ = gpio;
        channel_ = channel;
        timer_ = timer;
        
        // Configure PWM timer if not already configured
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_12_BIT, // 12-bit for smoother PWM
            .timer_num = timer,
            .freq_hz = 5000, // Higher frequency to avoid flickering
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        
        // Configure PWM channel
        ledc_channel_config_t ledc_channel = {
            .gpio_num = gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = timer,
            .duty = 0,
            .hpoint = 0,
            .flags = {.output_invert = 0}
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        
        initialized_ = true;
    }
    
    void SetBrightness(int brightness) {
        if (!initialized_) return;
        brightness = std::max(0, std::min(100, brightness));
        current_duty_ = (brightness * 4095) / 100; // 12-bit
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, current_duty_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
    }
    
    void SetDutyRaw(uint32_t duty) {
        if (!initialized_) return;
        current_duty_ = std::min(duty, (uint32_t)4095);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, current_duty_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
    }
    
    void Off() {
        if (!initialized_) return;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
        current_duty_ = 0;
    }
    
    int GetCurrentDuty() const { return current_duty_; }
};

// ===== SENSOR CONTROLLER =====
class SensorController {
private:
    WkEsp32s3Dev* board_;
    TaskHandle_t sensor_task_handle_ = nullptr;
    bool pir_enabled_ = true;
    uint32_t last_motion_time_ = 0;
    int motion_count_ = 0;
    
    static void SensorTask(void* arg) {
        auto* controller = static_cast<SensorController*>(arg);
        while (1) {
            controller->UpdateSensors();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    
    void UpdateSensors() {
        if (pir_enabled_ && board_->ReadMotionDetected()) {
            uint32_t now = esp_timer_get_time() / 1000;
            if (now - last_motion_time_ > 2000) { // Debounce 2s
                motion_count_++;
                last_motion_time_ = now;
                ESP_LOGI(TAG, "Motion detected! Count: %d", motion_count_);
            }
        }
    }
    
public:
    SensorController(WkEsp32s3Dev* board) : board_(board) {
        xTaskCreate(SensorTask, "sensor_task", 3072, this, 5, &sensor_task_handle_);
    }
    
    int GetMotionCount() const { return motion_count_; }
    void ResetMotionCount() { motion_count_ = 0; }
    void EnablePir(bool enable) { pir_enabled_ = enable; }
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
    AudioCodec* audio_codec_ = nullptr;
    
    // PWM LED controllers
    PwmLedController led1_controller_;
    PwmLedController led2_controller_;
    
    // Motor PWM
    PwmLedController motor_in1_, motor_in2_, motor_in3_, motor_in4_;
    
    int current_volume_ = 80;
    int current_brightness_ = 30; // Default brightness 30%

    // LED animation variables
    LedAnimation anim_led1_ = {PATTERN_BREATH, 5, 100, 0, true, 0};
    LedAnimation anim_led2_ = {PATTERN_OFF, 0, 100, 0, false, 120};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    bool led_power_save_ = false;
    
    // LED timeout
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;
    const int DEFAULT_LED_DURATION_ = 30;
    const int LED_UPDATE_INTERVAL_MS = 20; // 50Hz update rate

    // Emotion state
    std::string current_emotion_ = "neutral";
    bool emotion_auto_mode_ = true;
    uint32_t emotion_lock_until_ = 0;
    
    // Battery monitoring
    int battery_level_ = 100;
    uint32_t last_battery_read_ = 0;
    const int BATTERY_READ_INTERVAL_MS = 10000;
    
    // Motion state
    bool motion_active_ = false;
    uint32_t motion_timeout_ = 0;

    friend class SensorController;

    // ===== ENHANCED DISPLAY =====
    void ShowEmotionDisplay(const std::string& emotion) {
        if (!display_) return;
        
        static const std::map<std::string, std::pair<std::string, std::string>> emotion_map = {
            {"happy", {"😊", "Vui vẻ!"}},
            {"sad", {"😢", "Buồn..."}},
            {"angry", {"😠", "Giận dữ!"}},
            {"surprised", {"😮", "Ngạc nhiên!"}},
            {"scared", {"😨", "Sợ hãi!"}},
            {"sleeping", {"😴", "Đang ngủ..."}},
            {"thinking", {"🤔", "Đang suy nghĩ..."}},
            {"listening", {"👂", "Đang lắng nghe..."}},
            {"speaking", {"🗣️", "Đang nói..."}},
            {"love", {"😍", "Yêu thương!"}},
            {"confused", {"😕", "Bối rối?"}},
            {"neutral", {"😐", "Sẵn sàng"}}
        };
        
        auto it = emotion_map.find(emotion);
        if (it != emotion_map.end()) {
            display_->SetEmotion(it->second.first);
            display_->SetStatus(it->second.second);
        }
    }

    // ===== ENHANCED LED EFFECTS =====
    int BreathEffect(uint32_t time_ms, int speed, int brightness) {
        float period = 3000.0f / std::max(1, speed);
        float phase = fmod(time_ms, period) / period * 2 * M_PI;
        // Smooth breathing using sine with gamma correction
        float value = (sin(phase) + 1) / 2;
        value = pow(value, 1.5); // Gamma correction for smoother transition
        return (int)(value * brightness * 2.55);
    }

    int HeartbeatEffect(uint32_t time_ms, int brightness) {
        uint32_t cycle = time_ms % 1200;
        float intensity = 0;
        
        if (cycle < 100) {
            intensity = (float)cycle / 100; // First beat rise
        } else if (cycle < 200) {
            intensity = 1.0 - (float)(cycle - 100) / 100; // First beat fall
        } else if (cycle < 300) {
            intensity = (float)(cycle - 200) / 100 * 0.7; // Second beat rise
        } else if (cycle < 400) {
            intensity = 0.7 - (float)(cycle - 300) / 100 * 0.7; // Second beat fall
        } else {
            intensity = 0;
        }
        
        return (int)(intensity * brightness * 2.55);
    }

    int WaveEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        float period = 2000.0f / std::max(1, speed);
        float phase = fmod(time_ms, period) / period * 2 * M_PI;
        float phase_offset = led_index == 0 ? 0 : M_PI;
        float value = (sin(phase + phase_offset) + 1) / 2;
        value = pow(value, 1.2);
        return (int)(value * brightness * 2.55);
    }

    int CometEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        int cycle_time = 4000 / std::max(1, speed);
        int pos = (time_ms % cycle_time) * 1000 / cycle_time;
        int intensity = 0;
        
        if (pos > 800) {
            intensity = 1000;
        } else if (pos > 600) {
            intensity = (pos - 600) * 5;
        }
        
        int value = led_index == 0 ? intensity : intensity / 2;
        return (int)(value * brightness * 2.55 / 1000);
    }

    int TwinkleEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        uint32_t seed = (time_ms / std::max(1, (200 / speed))) + led_index * 1000;
        uint32_t random = (seed * 1103515245 + 12345) & 0x7fffffff;
        int intensity;
        
        if ((random % 256) > 220) {
            intensity = 1000;
        } else if ((random % 256) > 180) {
            intensity = 500;
        } else if ((random % 256) > 140) {
            intensity = 200;
        } else {
            intensity = 0;
        }
        
        return (int)(intensity * brightness * 2.55 / 1000);
    }

    int PulseEffect(uint32_t time_ms, int speed, int brightness) {
        int pulse_width = std::max(1, 300 / speed);
        uint32_t cycle = time_ms % (pulse_width * 4);
        int intensity;
        
        if (cycle < pulse_width) {
            intensity = (cycle * 1000) / pulse_width;
        } else if (cycle < pulse_width * 2) {
            intensity = 1000 - ((cycle - pulse_width) * 1000 / pulse_width);
        } else {
            intensity = 0;
        }
        
        return (int)(intensity * brightness * 2.55 / 1000);
    }

    int RainbowEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        float hue = fmod(time_ms * speed / 10.0 + led_index * 60, 360);
        
        // Convert HSV to RGB
        float s = 1.0, v = 1.0;
        float c = v * s;
        float x = c * (1 - fabs(fmod(hue / 60.0, 2) - 1));
        float m = v - c;
        float r, g, b;
        
        if (hue < 60) { r = c; g = x; b = 0; }
        else if (hue < 120) { r = x; g = c; b = 0; }
        else if (hue < 180) { r = 0; g = c; b = x; }
        else if (hue < 240) { r = 0; g = x; b = c; }
        else if (hue < 300) { r = x; g = 0; b = c; }
        else { r = c; g = 0; b = x; }
        
        // Simple average for single LED
        int intensity = (int)((r + g + b) / 3 * brightness * 2.55);
        return intensity;
    }

    int FireEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        uint32_t seed = time_ms / 50 + led_index * 1000;
        uint32_t random1 = (seed * 1103515245 + 12345) & 0x7fffffff;
        uint32_t random2 = ((seed + 50) * 1103515245 + 12345) & 0x7fffffff;
        
        int flicker = (random1 % 300) + (random2 % 700);
        flicker = std::min(1000, flicker);
        
        // Add slow variation
        float slow_var = (sin(time_ms / 1000.0) + 1) / 2;
        int intensity = (int)(flicker * (0.7 + 0.3 * slow_var));
        
        return (int)(intensity * brightness * 2.55 / 1000);
    }

    void ApplyLedEffect(PwmLedController& led, LedAnimation& anim, int led_index) {
        if (!anim.active || led_power_save_) {
            led.Off();
            return;
        }
        
        int brightness = 0;
        uint32_t time = led_tick_;
        
        switch (anim.pattern) {
            case PATTERN_OFF: 
                brightness = 0; 
                break;
            case PATTERN_BREATH: 
                brightness = BreathEffect(time, anim.speed, anim.brightness); 
                break;
            case PATTERN_BLINK_FAST: 
                brightness = (time % (100 / std::max(1, anim.speed))) < 50 ? anim.brightness * 2.55 : 0; 
                break;
            case PATTERN_BLINK_SLOW: 
                brightness = (time % (500 / std::max(1, anim.speed))) < 250 ? anim.brightness * 2.55 : 0; 
                break;
            case PATTERN_HEARTBEAT: 
                brightness = HeartbeatEffect(time, anim.brightness); 
                break;
            case PATTERN_WAVE: 
                brightness = WaveEffect(time, anim.speed, led_index, anim.brightness); 
                break;
            case PATTERN_COMET: 
                brightness = CometEffect(time, anim.speed, led_index, anim.brightness); 
                break;
            case PATTERN_PULSE: 
                brightness = PulseEffect(time, anim.speed, anim.brightness); 
                break;
            case PATTERN_TWINKLE: 
                brightness = TwinkleEffect(time, anim.speed, led_index, anim.brightness); 
                break;
            case PATTERN_RAINBOW:
                brightness = RainbowEffect(time, anim.speed, led_index, anim.brightness);
                break;
            case PATTERN_FIRE:
                brightness = FireEffect(time, anim.speed, led_index, anim.brightness);
                break;
            default: 
                brightness = 0; 
                break;
        }
        
        led.SetDutyRaw(std::min(4095, brightness * 4)); // Scale to 12-bit
    }

    void SetLedTimeout(int duration_seconds) {
        if (duration_seconds <= 0) {
            led_timeout_ms_ = 0;
        } else {
            led_timeout_ms_ = duration_seconds * 1000;
            led_timeout_start_ = led_tick_;
        }
    }

    // ===== ENHANCED EMOTION EXECUTION =====
    void ExecuteEmotion(const std::string& emotion) {
        if (emotion == current_emotion_ && !emotion_auto_mode_) return;
        if (esp_timer_get_time() / 1000 < emotion_lock_until_) return;
        
        current_emotion_ = emotion;
        emotion_auto_mode_ = false;
        
        ESP_LOGI(TAG, "Emotion: %s", emotion.c_str());
        
        // Hiển thị emoji + text trên OLED
        ShowEmotionDisplay(emotion);
        
        // Điều khiển LED + Motor theo cảm xúc
        if (emotion == "happy") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 5, 80, 0, true, 0};
            anim_led2_ = {PATTERN_BREATH, 5, 80, 0, true, 120};
            SetLedTimeout(5);
            SetLeftMotor(40);
            SetRightMotor(40);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(-30);
            SetRightMotor(-30);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "sad") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 1, 30, 0, true, 240};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLedTimeout(5);
            SetLeftMotor(-20);
            SetRightMotor(-20);
            vTaskDelay(pdMS_TO_TICKS(1000));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "angry") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_FIRE, 15, 100, 0, true, 0};
            anim_led2_ = {PATTERN_FIRE, 15, 100, 0, true, 0};
            SetLedTimeout(3);
            SetLeftMotor(60);
            SetRightMotor(60);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "scared") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BLINK_FAST, 25, 100, 0, true, 0};
            anim_led2_ = {PATTERN_BLINK_FAST, 25, 100, 0, true, 0};
            SetLedTimeout(3);
            SetLeftMotor(-70);
            SetRightMotor(-70);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0);
            SetRightMotor(0);
            for (int i = 0; i < 5; i++) {
                SetLeftMotor(-30);
                SetRightMotor(30);
                vTaskDelay(pdMS_TO_TICKS(100));
                SetLeftMotor(30);
                SetRightMotor(-30);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "love") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_HEARTBEAT, 5, 80, 0, true, 0};
            anim_led2_ = {PATTERN_RAINBOW, 3, 40, 0, true, 120};
            SetLedTimeout(5);
            for (int i = 0; i < 3; i++) {
                SetLeftMotor(-20);
                SetRightMotor(20);
                vTaskDelay(pdMS_TO_TICKS(400));
                SetLeftMotor(20);
                SetRightMotor(-20);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "confused") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_TWINKLE, 3, 50, 0, true, 60};
            anim_led2_ = {PATTERN_TWINKLE, 5, 50, 0, true, 180};
            SetLedTimeout(3);
            SetLeftMotor(-30);
            SetRightMotor(30);
            vTaskDelay(pdMS_TO_TICKS(400));
            SetLeftMotor(30);
            SetRightMotor(-30);
            vTaskDelay(pdMS_TO_TICKS(400));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "neutral") {
            led_auto_mode_ = true;
            led_timeout_ms_ = 0;
            emotion_auto_mode_ = true;
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "thinking") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_PULSE, 2, 60, 0, true, 0};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLedTimeout(10);
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "listening") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 3, 70, 0, true, 0};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLeftMotor(-15);
            SetRightMotor(15);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        else if (emotion == "speaking") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_WAVE, 4, 90, 0, true, 0};
            anim_led2_ = {PATTERN_WAVE, 4, 90, 0, true, 180};
            SetLeftMotor(15);
            SetRightMotor(15);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(-10);
            SetRightMotor(-10);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(0);
            SetRightMotor(0);
        }
        
        // Lock emotion for 1 second to prevent rapid switching
        emotion_lock_until_ = esp_timer_get_time() / 1000 + 1000;
    }

    void UpdateEmotionByState() {
        if (!emotion_auto_mode_) return;
        auto& app = Application::GetInstance();
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle: ExecuteEmotion("neutral"); break;
            case kDeviceStateConnecting: ExecuteEmotion("thinking"); break;
            case kDeviceStateListening: ExecuteEmotion("listening"); break;
            case kDeviceStateSpeaking: ExecuteEmotion("speaking"); break;
            default: ExecuteEmotion("neutral"); break;
        }
    }

    void UpdateLedCreative() {
        led_tick_ += LED_UPDATE_INTERVAL_MS;
        
        if (led_tick_ % 500 == 0) {
            UpdateEmotionByState();
        }
        
        if (led_timeout_ms_ > 0) {
            if (led_tick_ - led_timeout_start_ >= led_timeout_ms_) {
                led_timeout_ms_ = 0;
                led_auto_mode_ = true;
                emotion_auto_mode_ = true;
                ESP_LOGI(TAG, "LED timeout, trở về chế độ tự động");
            }
        }
        
        if (led_auto_mode_) {
            auto& app = Application::GetInstance();
            switch (app.GetDeviceState()) {
                case kDeviceStateIdle:
                    anim_led1_ = {PATTERN_BREATH, 3, current_brightness_/2, 0, true, 0};
                    anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
                    break;
                case kDeviceStateConnecting:
                    anim_led1_ = {PATTERN_PULSE, 8, current_brightness_, 0, true, 0};
                    anim_led2_ = {PATTERN_PULSE, 8, current_brightness_, 0, true, 180};
                    break;
                case kDeviceStateListening:
                    anim_led1_ = {PATTERN_BLINK_FAST, 10, current_brightness_, 0, true, 0};
                    anim_led2_ = {PATTERN_WAVE, 7, current_brightness_, 0, true, 120};
                    break;
                case kDeviceStateSpeaking:
                    anim_led1_ = {PATTERN_HEARTBEAT, 5, current_brightness_, 0, true, 0};
                    anim_led2_ = {PATTERN_BREATH, 4, current_brightness_/2, 0, true, 180};
                    break;
                default:
                    anim_led1_ = {PATTERN_BLINK_FAST, 12, current_brightness_, 0, true, 0};
                    anim_led2_ = {PATTERN_BLINK_FAST, 12, current_brightness_, 0, true, 0};
                    break;
            }
        }
        
        ApplyLedEffect(led1_controller_, anim_led1_, 0);
        ApplyLedEffect(led2_controller_, anim_led2_, 1);
        
        // Battery monitoring
        uint32_t now = esp_timer_get_time() / 1000;
        if (now - last_battery_read_ >= BATTERY_READ_INTERVAL_MS) {
            ReadBatteryLevel();
            last_battery_read_ = now;
        }
    }

    static void LedCreativeTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        TickType_t last_wake_time = xTaskGetTickCount();
        const TickType_t interval = pdMS_TO_TICKS(board->LED_UPDATE_INTERVAL_MS);
        
        while (1) {
            board->UpdateLedCreative();
            vTaskDelayUntil(&last_wake_time, interval);
        }
    }

    // ===== ENHANCED MOTOR CONTROL =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833 with PWM");
        
        // Initialize motor PWM channels
        motor_in1_.Init((gpio_num_t)DRV8833_IN1, LEDC_CHANNEL_4, LEDC_TIMER_1);
        motor_in2_.Init((gpio_num_t)DRV8833_IN2, LEDC_CHANNEL_5, LEDC_TIMER_1);
        motor_in3_.Init((gpio_num_t)DRV8833_IN3, LEDC_CHANNEL_6, LEDC_TIMER_1);
        motor_in4_.Init((gpio_num_t)DRV8833_IN4, LEDC_CHANNEL_7, LEDC_TIMER_1);
    }
    
    void SetLeftMotor(int speed) {
        speed = std::max(-100, std::min(100, speed));
        
        if (speed > 0) {
            motor_in1_.SetBrightness(speed);
            motor_in2_.Off();
        } else if (speed < 0) {
            motor_in1_.Off();
            motor_in2_.SetBrightness(-speed);
        } else {
            motor_in1_.Off();
            motor_in2_.Off();
        }
    }
    
    void SetRightMotor(int speed) {
        speed = std::max(-100, std::min(100, speed));
        
        if (speed > 0) {
            motor_in3_.SetBrightness(speed);
            motor_in4_.Off();
        } else if (speed < 0) {
            motor_in3_.Off();
            motor_in4_.SetBrightness(-speed);
        } else {
            motor_in3_.Off();
            motor_in4_.Off();
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
    }

    // ===== ULTRASONIC =====
    void InitializeUltrasonic() {
        ESP_LOGI(TAG, "Initialize Ultrasonic Sensor");
        // TODO: Implement HC-SR04 ultrasonic sensor
    }

    // ===== LED GPIO (now using PWM) =====
    void InitializeLedPwm() {
        ESP_LOGI(TAG, "Initialize LED with PWM");
        led1_controller_.Init((gpio_num_t)LED_1, LEDC_CHANNEL_0, LEDC_TIMER_0);
        led2_controller_.Init((gpio_num_t)LED_2, LEDC_CHANNEL_1, LEDC_TIMER_0);
        led1_controller_.Off();
        led2_controller_.Off();
    }

    // ===== ADC =====
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

    int ReadBatteryLevel() {
        if (!adc_handle_) return -1;
        
        int adc_value = 0;
        adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value);
        
        // Map ADC value to battery percentage (adjust based on your battery)
        // Assuming 3.7V LiPo with voltage divider
        battery_level_ = std::max(0, std::min(100, (adc_value - 2500) * 100 / (3300 - 2500)));
        
        if (battery_level_ < 10) {
            led_power_save_ = true; // Enable power saving mode
            ESP_LOGW(TAG, "Low battery! Entering power save mode");
        }
        
        return battery_level_;
    }

    // ===== ENHANCED MCP TOOLS =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.forward", "Robot tiến", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100), 
                         Property("duration", kPropertyTypeInteger, 0, 0, 10000)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                int duration = p["duration"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(speed);
                
                if (duration > 0) {
                    vTaskDelay(pdMS_TO_TICKS(duration));
                    SetLeftMotor(0);
                    SetRightMotor(0);
                }
                
                return "Tiến " + std::to_string(speed) + "%" + 
                       (duration > 0 ? " trong " + std::to_string(duration) + "ms" : "");
            });
            
        mcp.AddTool("self.motor.backward", "Robot lùi", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100),
                         Property("duration", kPropertyTypeInteger, 0, 0, 10000)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                int duration = p["duration"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(-speed);
                
                if (duration > 0) {
                    vTaskDelay(pdMS_TO_TICKS(duration));
                    SetLeftMotor(0);
                    SetRightMotor(0);
                }
                
                return "Lùi " + std::to_string(speed) + "%";
            });
            
        // ... (similar enhancements for other motor commands)
        
        mcp.AddTool("self.motor.stop", "Dừng động cơ", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetLeftMotor(0);
                SetRightMotor(0);
                return "Đã dừng động cơ";
            });
    }

    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.set_brightness", "Đặt độ sáng LED", 
            PropertyList({Property("brightness", kPropertyTypeInteger, 30, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_brightness_ = p["brightness"].value<int>();
                anim_led1_.brightness = current_brightness_;
                anim_led2_.brightness = current_brightness_;
                return "Độ sáng LED: " + std::to_string(current_brightness_) + "%";
            });
            
        mcp.AddTool("self.led.set_pattern", "Đặt pattern LED", 
            PropertyList({Property("pattern", kPropertyTypeString, "breath"),
                         Property("led", kPropertyTypeInteger, 0, 0, 1)}),
            [this](const PropertyList& p) -> ReturnValue {
                std::string pattern_name = p["pattern"].value<std::string>();
                int led_index = p["led"].value<int>();
                
                static const std::map<std::string, LedPattern> pattern_map = {
                    {"off", PATTERN_OFF},
                    {"breath", PATTERN_BREATH},
                    {"blink_fast", PATTERN_BLINK_FAST},
                    {"blink_slow", PATTERN_BLINK_SLOW},
                    {"heartbeat", PATTERN_HEARTBEAT},
                    {"wave", PATTERN_WAVE},
                    {"comet", PATTERN_COMET},
                    {"pulse", PATTERN_PULSE},
                    {"twinkle", PATTERN_TWINKLE},
                    {"rainbow", PATTERN_RAINBOW},
                    {"fire", PATTERN_FIRE}
                };
                
                auto it = pattern_map.find(pattern_name);
                if (it == pattern_map.end()) {
                    return "Pattern không hợp lệ";
                }
                
                led_auto_mode_ = false;
                if (led_index == 0) {
                    anim_led1_.pattern = it->second;
                    anim_led1_.active = (it->second != PATTERN_OFF);
                } else {
                    anim_led2_.pattern = it->second;
                    anim_led2_.active = (it->second != PATTERN_OFF);
                }
                
                return "Đã đặt pattern: " + pattern_name;
            });
            
        mcp.AddTool("self.led.on", "Bật LED", 
            PropertyList({Property("brightness", kPropertyTypeInteger, 100, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int brightness = p["brightness"].value<int>();
                led1_controller_.SetBrightness(brightness);
                led2_controller_.SetBrightness(brightness);
                return "LED bật với độ sáng " + std::to_string(brightness) + "%";
            });
            
        mcp.AddTool("self.led.off", "Tắt LED", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                led1_controller_.Off();
                led2_controller_.Off();
                return "LED tắt";
            });
    }

    void InitializeSensorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.sensor.motion_detected", "Kiểm tra chuyển động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                return ReadMotionDetected() ? "Có chuyển động" : "Không có chuyển động";
            });
            
        mcp.AddTool("self.sensor.motion_stats", "Thống kê chuyển động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (!sensor_controller_) return "Sensor không khả dụng";
                return "Số lần phát hiện: " + std::to_string(sensor_controller_->GetMotionCount());
            });
    }

    void InitializeBatteryMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.battery.level", "Mức pin", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                int level = ReadBatteryLevel();
                return std::to_string(level) + "%";
            });
            
        mcp.AddTool("self.system.status", "Trạng thái hệ thống", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                std::string status = "Pin: " + std::to_string(battery_level_) + "%\n";
                status += "Cảm xúc: " + current_emotion_ + "\n";
                status += "LED auto: " + std::string(led_auto_mode_ ? "ON" : "OFF") + "\n";
                status += "Power save: " + std::string(led_power_save_ ? "ON" : "OFF");
                return status;
            });
    }

public:
    WkEsp32s3Dev() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        // Initialize LED with PWM
        InitializeLedPwm();

#ifdef CONFIG_BOARD_WK_HAVE_MOTOR
        InitializeMotor();
        InitializeMotorMcp();
#endif

        InitializePirSensor();
        InitializeUltrasonic();
        InitializeSensorMcp();
        InitializeLedMcp();
        InitializeEmotionMcp();
        InitializeVolumeMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        // Start LED animation task
        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 4096, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        ShowEmotionDisplay("neutral");
#endif

        InitializeButtons();
        InitializeTools();
        
        // Initialize audio codec
        audio_codec_ = GetAudioCodec();
        if (audio_codec_) {
            audio_codec_->SetOutputVolume(current_volume_);
        }
        
        // Initial battery read
        ReadBatteryLevel();
    }

    bool ReadMotionDetected() {
        return gpio_get_level(PIR_MOTION_SENSOR_PIN) == 1;
    }

    // ... (rest of the display initialization remains the same)
};

// ===== SENSOR CONTROLLER IMPLEMENTATION =====
// (already implemented above in the class definition)

DECLARE_BOARD(WkEsp32s3Dev);
