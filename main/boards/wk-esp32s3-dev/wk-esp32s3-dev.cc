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
    uint8_t hue;
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
        
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_12_BIT,
            .timer_num = timer,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        
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
        current_duty_ = (brightness * 4095) / 100;
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
            if (now - last_motion_time_ > 2000) {
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
    int current_brightness_ = 30;

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
    const int LED_UPDATE_INTERVAL_MS = 20;

    // Emotion state
    std::string current_emotion_ = "neutral";
    bool emotion_auto_mode_ = true;
    uint32_t emotion_lock_until_ = 0;
    
    // Battery monitoring
    int battery_level_ = 100;
    uint32_t last_battery_read_ = 0;
    const int BATTERY_READ_INTERVAL_MS = 10000;

    friend class SensorController;

    // ===== DISPLAY FUNCTIONS =====
    void ShowEmotionDisplay(const std::string& emotion) {
        if (!display_) return;
        
        if (emotion == "happy") {
            display_->SetEmotion("😊");
            display_->SetStatus("Vui vẻ!");
        } else if (emotion == "sad") {
            display_->SetEmotion("😢");
            display_->SetStatus("Buồn...");
        } else if (emotion == "angry") {
            display_->SetEmotion("😠");
            display_->SetStatus("Giận dữ!");
        } else if (emotion == "surprised") {
            display_->SetEmotion("😮");
            display_->SetStatus("Ngạc nhiên!");
        } else if (emotion == "scared") {
            display_->SetEmotion("😨");
            display_->SetStatus("Sợ hãi!");
        } else if (emotion == "sleeping") {
            display_->SetEmotion("😴");
            display_->SetStatus("Đang ngủ...");
        } else if (emotion == "thinking") {
            display_->SetEmotion("🤔");
            display_->SetStatus("Đang suy nghĩ...");
        } else if (emotion == "listening") {
            display_->SetEmotion("👂");
            display_->SetStatus("Đang lắng nghe...");
        } else if (emotion == "speaking") {
            display_->SetEmotion("🗣️");
            display_->SetStatus("Đang nói...");
        } else if (emotion == "love") {
            display_->SetEmotion("😍");
            display_->SetStatus("Yêu thương!");
        } else if (emotion == "confused") {
            display_->SetEmotion("😕");
            display_->SetStatus("Bối rối?");
        } else {
            display_->SetEmotion("😐");
            display_->SetStatus("Sẵn sàng");
        }
    }

    // ===== LED EFFECTS =====
    int BreathEffect(uint32_t time_ms, int speed, int brightness) {
        float period = 3000.0f / std::max(1, speed);
        float phase = fmod(time_ms, period) / period * 2 * M_PI;
        float value = (sin(phase) + 1) / 2;
        value = pow(value, 1.5);
        return (int)(value * brightness * 2.55);
    }

    int HeartbeatEffect(uint32_t time_ms, int brightness) {
        uint32_t cycle = time_ms % 1200;
        float intensity = 0;
        
        if (cycle < 100) {
            intensity = (float)cycle / 100;
        } else if (cycle < 200) {
            intensity = 1.0 - (float)(cycle - 100) / 100;
        } else if (cycle < 300) {
            intensity = (float)(cycle - 200) / 100 * 0.7;
        } else if (cycle < 400) {
            intensity = 0.7 - (float)(cycle - 300) / 100 * 0.7;
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
        float s = 1.0, v = 1.0;
        float c = v * s;
        float x = c * (1 - fabs(fmod(hue / 60.0, 2) - 1));
        float r, g, b;
        
        if (hue < 60) { r = c; g = x; b = 0; }
        else if (hue < 120) { r = x; g = c; b = 0; }
        else if (hue < 180) { r = 0; g = c; b = x; }
        else if (hue < 240) { r = 0; g = x; b = c; }
        else if (hue < 300) { r = x; g = 0; b = c; }
        else { r = c; g = 0; b = x; }
        
        int intensity = (int)((r + g + b) / 3 * brightness * 2.55);
        return intensity;
    }

    int FireEffect(uint32_t time_ms, int speed, int led_index, int brightness) {
        uint32_t seed = time_ms / 50 + led_index * 1000;
        uint32_t random1 = (seed * 1103515245 + 12345) & 0x7fffffff;
        uint32_t random2 = ((seed + 50) * 1103515245 + 12345) & 0x7fffffff;
        
        int flicker = (random1 % 300) + (random2 % 700);
        flicker = std::min(1000, flicker);
        
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
        
        led.SetDutyRaw(std::min(4095, brightness * 4));
    }

    void SetLedTimeout(int duration_seconds) {
        if (duration_seconds <= 0) {
            led_timeout_ms_ = 0;
        } else {
            led_timeout_ms_ = duration_seconds * 1000;
            led_timeout_start_ = led_tick_;
        }
    }

    // ===== EMOTION EXECUTION =====
    void ExecuteEmotion(const std::string& emotion) {
        if (emotion == current_emotion_ && !emotion_auto_mode_) return;
        if (esp_timer_get_time() / 1000 < emotion_lock_until_) return;
        
        current_emotion_ = emotion;
        emotion_auto_mode_ = false;
        
        ESP_LOGI(TAG, "Emotion: %s", emotion.c_str());
        
        ShowEmotionDisplay(emotion);
        
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
        } else if (emotion == "sad") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 1, 30, 0, true, 240};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLedTimeout(5);
            SetLeftMotor(-20);
            SetRightMotor(-20);
            vTaskDelay(pdMS_TO_TICKS(1000));
            SetLeftMotor(0);
            SetRightMotor(0);
        } else if (emotion == "angry") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_FIRE, 15, 100, 0, true, 0};
            anim_led2_ = {PATTERN_FIRE, 15, 100, 0, true, 0};
            SetLedTimeout(3);
            SetLeftMotor(60);
            SetRightMotor(60);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0);
            SetRightMotor(0);
        } else if (emotion == "scared") {
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
        } else if (emotion == "love") {
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
        } else if (emotion == "confused") {
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
        } else if (emotion == "neutral") {
            led_auto_mode_ = true;
            led_timeout_ms_ = 0;
            emotion_auto_mode_ = true;
            SetLeftMotor(0);
            SetRightMotor(0);
        } else if (emotion == "thinking") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_PULSE, 2, 60, 0, true, 0};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLedTimeout(10);
            SetLeftMotor(0);
            SetRightMotor(0);
        } else if (emotion == "listening") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 3, 70, 0, true, 0};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false, 0};
            SetLeftMotor(-15);
            SetRightMotor(15);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0);
            SetRightMotor(0);
        } else if (emotion == "speaking") {
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
                ESP_LOGI(TAG, "LED timeout, back to auto mode");
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

    // ===== MOTOR CONTROL =====
    void InitializeMotor() {
        ESP_LOGI(TAG, "Initialize Motor DRV8833 with PWM");
        
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

    // ===== SENSOR INITIALIZATION =====
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

    void InitializeLedPwm() {
        ESP_LOGI(TAG, "Initialize LED with PWM");
        led1_controller_.Init((gpio_num_t)LED_1, LEDC_CHANNEL_0, LEDC_TIMER_0);
        led2_controller_.Init((gpio_num_t)LED_2, LEDC_CHANNEL_1, LEDC_TIMER_0);
        led1_controller_.Off();
        led2_controller_.Off();
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

    int ReadBatteryLevel() {
        if (!adc_handle_) return -1;
        
        int adc_value = 0;
        adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value);
        
        battery_level_ = std::max(0, std::min(100, (adc_value - 2500) * 100 / (3300 - 2500)));
        
        if (battery_level_ < 10) {
            led_power_save_ = true;
            ESP_LOGW(TAG, "Low battery! Entering power save mode");
        }
        
        return battery_level_;
    }

    // ===== MCP INITIALIZATION =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.forward", "Robot moves forward", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(speed);
                return "Moving forward at " + std::to_string(speed) + "%";
            });
            
        mcp.AddTool("self.motor.backward", "Robot moves backward", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(-speed);
                return "Moving backward at " + std::to_string(speed) + "%";
            });
            
        mcp.AddTool("self.motor.turn_left", "Turn left", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(speed);
                return "Turning left";
            });
            
        mcp.AddTool("self.motor.turn_right", "Turn right", 
            PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(-speed);
                return "Turning right";
            });
            
        mcp.AddTool("self.motor.stop", "Stop motors", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetLeftMotor(0);
                SetRightMotor(0);
                return "Motors stopped";
            });
    }

    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.set_brightness", "Set LED brightness", 
            PropertyList({Property("brightness", kPropertyTypeInteger, 30, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_brightness_ = p["brightness"].value<int>();
                anim_led1_.brightness = current_brightness_;
                anim_led2_.brightness = current_brightness_;
                return "LED brightness: " + std::to_string(current_brightness_) + "%";
            });
            
        mcp.AddTool("self.led.on", "Turn on LEDs", 
            PropertyList({Property("brightness", kPropertyTypeInteger, 100, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                int brightness = p["brightness"].value<int>();
                led1_controller_.SetBrightness(brightness);
                led2_controller_.SetBrightness(brightness);
                return "LEDs on at " + std::to_string(brightness) + "%";
            });
            
        mcp.AddTool("self.led.off", "Turn off LEDs", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                led1_controller_.Off();
                led2_controller_.Off();
                return "LEDs off";
            });
    }

    void InitializeEmotionMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.emotion.set", "Set emotion", 
            PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
            [this](const PropertyList& p) -> ReturnValue {
                ExecuteEmotion(p["emotion"].value<std::string>());
                return "OK";
            });
            
        mcp.AddTool("self.emotion.happy", "Happy emotion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { 
                ExecuteEmotion("happy"); 
                return "Happy!"; 
            });
            
        mcp.AddTool("self.emotion.sad", "Sad emotion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { 
                ExecuteEmotion("sad"); 
                return "Sad"; 
            });
            
        mcp.AddTool("self.emotion.angry", "Angry emotion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { 
                ExecuteEmotion("angry"); 
                return "Angry!"; 
            });
            
        mcp.AddTool("self.emotion.scared", "Scared emotion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { 
                ExecuteEmotion("scared"); 
                return "Scared!"; 
            });
            
        mcp.AddTool("self.emotion.love", "Love emotion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { 
                ExecuteEmotion("love"); 
                return "Love!"; 
            });
            
        mcp.AddTool("self.emotion.auto", "Auto emotion mode", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                emotion_auto_mode_ = true;
                led_auto_mode_ = true;
                return "Auto mode";
            });
    }

    void InitializeVolumeMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.audio.volume_set", "Set volume", 
            PropertyList({Property("volume", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = p["volume"].value<int>();
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Volume: " + std::to_string(current_volume_) + "%";
            });
    }

    void InitializeBatteryMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.battery.level", "Battery level", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                int level = ReadBatteryLevel();
                return std::to_string(level) + "%";
            });
            
        mcp.AddTool("self.system.status", "System status", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                std::string status = "Battery: " + std::to_string(battery_level_) + "%\n";
                status += "Emotion: " + current_emotion_ + "\n";
                status += "LED auto: " + std::string(led_auto_mode_ ? "ON" : "OFF") + "\n";
                status += "Power save: " + std::string(led_power_save_ ? "ON" : "OFF");
                return status;
            });
    }

    void InitializeSensorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.sensor.motion_detected", "Check motion", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                return ReadMotionDetected() ? "Motion detected" : "No motion";
            });
            
        mcp.AddTool("self.sensor.motion_stats", "Motion statistics", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (!sensor_controller_) return "Sensor not available";
                return "Detection count: " + std::to_string(sensor_controller_->GetMotionCount());
            });
    }

    // ===== DISPLAY INITIALIZATION =====
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
        
        volume_up_button_.OnClick([this]() {
            current_volume_ = std::min(100, current_volume_ + 10);
            if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
            ESP_LOGI(TAG, "Volume: %d%%", current_volume_);
        });
        
        volume_down_button_.OnClick([this]() {
            current_volume_ = std::max(0, current_volume_ - 10);
            if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
            ESP_LOGI(TAG, "Volume: %d%%", current_volume_);
        });
    }

    void InitializeTools() {
        // Initialize any additional tools here
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
        InitializeLedMcp();
        InitializeEmotionMcp();
        InitializeVolumeMcp();
        InitializeAdc();
        InitializeBatteryMcp();
        
        // Initialize sensor controller after everything else
        sensor_controller_ = new SensorController(this);
        InitializeSensorMcp();

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
        
        ESP_LOGI(TAG, "WkEsp32s3Dev initialized successfully");
    }

    bool ReadMotionDetected() {
        return gpio_get_level(PIR_MOTION_SENSOR_PIN) == 1;
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

DECLARE_BOARD(WkEsp32s3Dev);
