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
    bool active;
};

class WkEsp32s3Dev : public WifiBoard {
private:
    Button boot_button_;
    OledDisplay* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Button volume_up_button_;
    Button volume_down_button_;
    SensorController* sensor_controller_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    int current_volume_ = 80;

    // LED
    LedAnimation anim_led1_ = {PATTERN_OFF, 5, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, false};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;
    const int DEFAULT_LED_DURATION_ = 30;

    // Face
    std::string current_face_ = "";
    bool face_auto_mode_ = true;

    friend class SensorController;

    // ===== FACE FUNCTIONS =====
    void DrawEye(int cx, int cy, int r, bool blink = false) {
        if (!display_) return;
        if (blink) {
            display_->DrawLine(cx - r, cy, cx + r, cy, 1);
        } else {
            display_->DrawCircle(cx, cy, r, 1);
            display_->DrawCircle(cx, cy, r / 2, 1);
        }
    }

    void DrawMouth(int cx, int cy, int w, int h, bool smile = true) {
        if (!display_) return;
        if (smile) {
            for (int x = -w/2; x <= w/2; x++) {
                int y = -(h * x * x) / (w * w / 4) + h;
                display_->DrawPixel(cx + x, cy + y, 1);
            }
        } else {
            for (int x = -w/2; x <= w/2; x++) {
                int y = (h * x * x) / (w * w / 4);
                display_->DrawPixel(cx + x, cy + y, 1);
            }
        }
    }

    void DrawEyebrow(int cx, int cy, int w, int h, bool raised = true) {
        if (!display_) return;
        if (raised) {
            for (int x = -w/2; x <= w/2; x++) {
                int y = -(h * x * x) / (w * w / 4);
                display_->DrawPixel(cx + x, cy + y, 1);
            }
        } else {
            for (int x = -w/2; x <= w/2; x++) {
                int y = (h * x * x) / (w * w / 4);
                display_->DrawPixel(cx + x, cy + y, 1);
            }
        }
    }

    void ShowFace(const std::string& emotion) {
        if (!display_) return;
        if (emotion == current_face_) return;
        current_face_ = emotion;
        display_->Clear();
        
        if (emotion == "neutral") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            display_->DrawLine(49, 45, 79, 45, 1);
        }
        else if (emotion == "happy") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            DrawMouth(64, 45, 30, 10, true);
            DrawEyebrow(40, 18, 16, 3, true);
            DrawEyebrow(88, 18, 16, 3, true);
        }
        else if (emotion == "sad") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            DrawMouth(64, 50, 30, 10, false);
            DrawEyebrow(40, 18, 16, 3, false);
            DrawEyebrow(88, 18, 16, 3, false);
        }
        else if (emotion == "angry") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            DrawMouth(64, 50, 20, 5, false);
            DrawEyebrow(40, 15, 16, 5, false);
            DrawEyebrow(88, 15, 16, 5, false);
        }
        else if (emotion == "surprised") {
            DrawEye(40, 30, 12);
            DrawEye(88, 30, 12);
            display_->DrawCircle(64, 45, 8, 1);
            DrawEyebrow(40, 15, 16, 3, true);
            DrawEyebrow(88, 15, 16, 3, true);
        }
        else if (emotion == "sleeping") {
            DrawEye(40, 30, 8, true);
            DrawEye(88, 30, 8, true);
            DrawMouth(64, 45, 15, 3, true);
        }
        else if (emotion == "thinking") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            display_->DrawCircle(40, 26, 4, 1);
            display_->DrawCircle(88, 26, 4, 1);
            DrawMouth(64, 45, 20, 5, true);
            DrawEyebrow(40, 18, 16, 3, true);
            DrawEyebrow(88, 18, 16, 3, false);
        }
        else if (emotion == "listening") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            display_->DrawCircle(36, 30, 4, 1);
            display_->DrawCircle(84, 30, 4, 1);
            display_->DrawCircle(64, 45, 10, 1);
            DrawEyebrow(40, 18, 16, 3, true);
            DrawEyebrow(88, 18, 16, 3, true);
        }
        else if (emotion == "speaking") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            for (int i = 0; i < 6; i++) {
                display_->DrawCircle(64, 45, 5 + i, 1);
            }
        }
        else if (emotion == "love") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            DrawMouth(64, 45, 35, 12, true);
            display_->DrawCircle(30, 50, 3, 1);
            display_->DrawCircle(98, 50, 3, 1);
        }
        else if (emotion == "confused") {
            DrawEye(40, 30, 8);
            DrawEye(88, 30, 8);
            display_->DrawLine(49, 45, 64, 42, 1);
            display_->DrawLine(64, 42, 79, 45, 1);
            DrawEyebrow(40, 18, 16, 3, true);
            DrawEyebrow(88, 18, 16, 3, false);
        }
        display_->Update();
    }

    void UpdateFaceByState() {
        if (!face_auto_mode_) return;
        auto& app = Application::GetInstance();
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle: ShowFace("neutral"); break;
            case kDeviceStateConnecting: ShowFace("thinking"); break;
            case kDeviceStateListening: ShowFace("listening"); break;
            case kDeviceStateSpeaking: ShowFace("speaking"); break;
            default: ShowFace("neutral"); break;
        }
    }

    // ===== LED EFFECTS =====
    int BreathEffect(uint32_t t, int s) {
        float period = 2000.0f / s;
        float phase = (t % (int)period) / period * 2 * 3.14159f;
        return (int)((sin(phase) + 1) / 2 * 255);
    }

    int HeartbeatEffect(uint32_t t) {
        uint32_t c = t % 1000;
        if (c < 100) return 255;
        else if (c < 200) return 50;
        else if (c < 300) return 255;
        else if (c < 400) return 50;
        else return 0;
    }

    int WaveEffect(uint32_t t, int s, int idx) {
        float period = 1500.0f / s;
        float phase = (t % (int)period) / period * 2 * 3.14159f;
        float offset = idx == 0 ? 0 : 3.14159f;
        return (int)((sin(phase + offset) + 1) / 2 * 255);
    }

    int PulseEffect(uint32_t t, int s) {
        int pw = 200 / s;
        uint32_t c = t % (pw * 4);
        if (c < pw) return (c * 255) / pw;
        else if (c < pw * 2) return 255 - ((c - pw) * 255 / pw);
        else return 0;
    }

    void ApplyLed(int pin, LedAnimation anim) {
        if (!anim.active) { gpio_set_level((gpio_num_t)pin, 0); return; }
        int b = 0;
        switch (anim.pattern) {
            case PATTERN_BREATH: b = BreathEffect(led_tick_, anim.speed); break;
            case PATTERN_BLINK_FAST: b = (led_tick_ % (100 / anim.speed)) < 50 ? 255 : 0; break;
            case PATTERN_HEARTBEAT: b = HeartbeatEffect(led_tick_); break;
            case PATTERN_WAVE: b = WaveEffect(led_tick_, anim.speed, pin == LED_1 ? 0 : 1); break;
            case PATTERN_PULSE: b = PulseEffect(led_tick_, anim.speed); break;
            default: b = 0; break;
        }
        gpio_set_level((gpio_num_t)pin, b > 50 ? 1 : 0);
    }

    void UpdateLed() {
        led_tick_ += 50;
        if (led_tick_ % 500 == 0) UpdateFaceByState();
        if (led_timeout_ms_ > 0 && led_tick_ - led_timeout_start_ >= led_timeout_ms_) {
            led_timeout_ms_ = 0; led_auto_mode_ = true;
        }
        if (led_auto_mode_) {
            auto& app = Application::GetInstance();
            switch (app.GetDeviceState()) {
                case kDeviceStateIdle:
                    anim_led1_ = {PATTERN_BREATH, 3, true};
                    anim_led2_ = {PATTERN_OFF, 0, false};
                    break;
                case kDeviceStateConnecting:
                    anim_led1_ = {PATTERN_PULSE, 8, true};
                    anim_led2_ = {PATTERN_PULSE, 8, true};
                    break;
                case kDeviceStateListening:
                    anim_led1_ = {PATTERN_BLINK_FAST, 10, true};
                    anim_led2_ = {PATTERN_WAVE, 7, true};
                    break;
                case kDeviceStateSpeaking:
                    anim_led1_ = {PATTERN_HEARTBEAT, 5, true};
                    anim_led2_ = {PATTERN_BREATH, 4, true};
                    break;
                default:
                    anim_led1_ = {PATTERN_BLINK_FAST, 12, true};
                    anim_led2_ = {PATTERN_BLINK_FAST, 12, true};
                    break;
            }
        }
        ApplyLed(LED_1, anim_led1_);
        ApplyLed(LED_2, anim_led2_);
    }

    static void LedTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        while (1) { board->UpdateLed(); vTaskDelay(pdMS_TO_TICKS(50)); }
    }

    // ===== MOTOR =====
    void InitializeMotor() {
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0, .freq_hz = 1000, .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&timer);
        
        ledc_channel_config_t ch1 = {.gpio_num = DRV8833_IN1, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&ch1);
        ledc_channel_config_t ch2 = {.gpio_num = DRV8833_IN2, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&ch2);
        ledc_channel_config_t ch3 = {.gpio_num = DRV8833_IN3, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&ch3);
        ledc_channel_config_t ch4 = {.gpio_num = DRV8833_IN4, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_3, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&ch4);
    }

    void SetMotor(int ch_a, int ch_b, int speed) {
        speed = std::max(-100, std::min(100, speed));
        if (speed > 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a, (speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b);
        } else if (speed < 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b, (-speed * 1023) / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_a);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch_b);
        }
    }

    void SetLeftMotor(int speed) { SetMotor(LEDC_CHANNEL_0, LEDC_CHANNEL_1, speed); }
    void SetRightMotor(int speed) { SetMotor(LEDC_CHANNEL_2, LEDC_CHANNEL_3, speed); }
    void Forward(int s) { SetLeftMotor(s); SetRightMotor(s); }
    void Backward(int s) { SetLeftMotor(-s); SetRightMotor(-s); }
    void TurnLeft(int s) { SetLeftMotor(-s); SetRightMotor(s); }
    void TurnRight(int s) { SetLeftMotor(s); SetRightMotor(-s); }
    void StopMotors() { SetLeftMotor(0); SetRightMotor(0); }

    // ===== EMOTION =====
    void ExecuteEmotion(const std::string& e) {
        ShowFace(e);
        if (e == "happy") {
            anim_led1_ = {PATTERN_BREATH, 5, true};
            Forward(40); vTaskDelay(pdMS_TO_TICKS(300));
            Backward(30); vTaskDelay(pdMS_TO_TICKS(300));
            StopMotors();
        }
        else if (e == "sad") {
            anim_led1_ = {PATTERN_BREATH, 1, true};
            Backward(20); vTaskDelay(pdMS_TO_TICKS(1000));
            StopMotors();
        }
        else if (e == "angry") {
            anim_led1_ = {PATTERN_BLINK_FAST, 15, true};
            Forward(60); vTaskDelay(pdMS_TO_TICKS(500));
            StopMotors();
        }
        else if (e == "scared") {
            anim_led1_ = {PATTERN_BLINK_FAST, 25, true};
            Backward(70); vTaskDelay(pdMS_TO_TICKS(500));
            StopMotors();
        }
        else if (e == "love") {
            anim_led1_ = {PATTERN_HEARTBEAT, 5, true};
            for (int i = 0; i < 3; i++) {
                TurnLeft(20); vTaskDelay(pdMS_TO_TICKS(400));
                TurnRight(20); vTaskDelay(pdMS_TO_TICKS(400));
            }
            StopMotors();
        }
        else if (e == "neutral") {
            led_auto_mode_ = true;
            StopMotors();
        }
    }

    // ===== INIT =====
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
        adc_oneshot_unit_init_cfg_t init_config = {.unit_id = POWER_ADC_UNIT, .ulp_mode = ADC_ULP_MODE_DISABLE};
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));
        adc_oneshot_chan_cfg_t config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_ADC_CHANNEL, &config));
    }

    // ===== MCP =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.motor.forward", "Tiến", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue { Forward(p["speed"].value<int>()); return "Tiến"; });
        mcp.AddTool("self.motor.backward", "Lùi", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue { Backward(p["speed"].value<int>()); return "Lùi"; });
        mcp.AddTool("self.motor.turn_left", "Rẽ trái", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue { TurnLeft(p["speed"].value<int>()); return "Rẽ trái"; });
        mcp.AddTool("self.motor.turn_right", "Rẽ phải", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue { TurnRight(p["speed"].value<int>()); return "Rẽ phải"; });
        mcp.AddTool("self.motor.stop", "Dừng", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { StopMotors(); return "Dừng"; });
    }

    void InitializeVolumeMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.audio.volume_set", "Đặt âm lượng", PropertyList({Property("volume", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = p["volume"].value<int>();
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Âm lượng: " + std::to_string(current_volume_) + "%";
            });
        mcp.AddTool("self.audio.mute", "Tắt tiếng", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { if (audio_codec_) audio_codec_->SetOutputVolume(0); return "Tắt tiếng"; });
        mcp.AddTool("self.audio.unmute", "Bật tiếng", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_); return "Bật tiếng"; });
    }

    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.led.on", "Bật LED 1", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { led_auto_mode_ = false; gpio_set_level(LED_1, 1); return "LED 1 bật"; });
        mcp.AddTool("self.led.off", "Tắt LED 1", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { led_auto_mode_ = true; gpio_set_level(LED_1, 0); return "Tắt"; });
        mcp.AddTool("self.led.breath", "LED thở", PropertyList({Property("speed", kPropertyTypeInteger, 3, 1, 10)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                anim_led1_ = {PATTERN_BREATH, p["speed"].value<int>(), true};
                return "LED thở";
            });
        mcp.AddTool("self.led.blink", "LED nhấp nháy", PropertyList({Property("speed", kPropertyTypeInteger, 5, 1, 20)}),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                anim_led1_ = {PATTERN_BLINK_FAST, p["speed"].value<int>(), true};
                anim_led2_ = {PATTERN_BLINK_FAST, p["speed"].value<int>(), true};
                return "LED nhấp nháy";
            });
        mcp.AddTool("self.led.auto", "LED tự động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { led_auto_mode_ = true; return "OK"; });
        mcp.AddTool("self.led.off_all", "Tắt hết", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                anim_led1_ = {PATTERN_OFF, 0, false};
                anim_led2_ = {PATTERN_OFF, 0, false};
                gpio_set_level(LED_1, 0); gpio_set_level(LED_2, 0);
                return "Tắt hết";
            });
    }

    void InitializeFaceMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.face.set", "Đặt biểu cảm", PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
            [this](const PropertyList& p) -> ReturnValue {
                face_auto_mode_ = false;
                ShowFace(p["emotion"].value<std::string>());
                return "OK";
            });
        mcp.AddTool("self.face.happy", "Mặt vui", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { face_auto_mode_ = false; ShowFace("happy"); return "Vui!"; });
        mcp.AddTool("self.face.sad", "Mặt buồn", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { face_auto_mode_ = false; ShowFace("sad"); return "Buồn"; });
        mcp.AddTool("self.face.auto", "Tự động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { face_auto_mode_ = true; UpdateFaceByState(); return "OK"; });
    }

    void InitializeEmotionMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.emotion.set", "Đặt cảm xúc", PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
            [this](const PropertyList& p) -> ReturnValue {
                ExecuteEmotion(p["emotion"].value<std::string>());
                return "OK";
            });
        mcp.AddTool("self.emotion.happy", "Vui vẻ", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { ExecuteEmotion("happy"); return "Vui!"; });
        mcp.AddTool("self.emotion.sad", "Buồn", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { ExecuteEmotion("sad"); return "Buồn"; });
        mcp.AddTool("self.emotion.angry", "Giận dữ", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { ExecuteEmotion("angry"); return "Giận!"; });
        mcp.AddTool("self.emotion.scared", "Sợ hãi", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { ExecuteEmotion("scared"); return "Sợ!"; });
        mcp.AddTool("self.emotion.love", "Yêu thương", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue { ExecuteEmotion("love"); return "Yêu!"; });
    }

    void InitializeBatteryMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.battery.level", "Mức pin", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                int adc_value = 0;
                adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value);
                return std::to_string((adc_value * 100) / 4095) + "%";
            });
    }

    void InitializeSensorMcp() {
        sensor_controller_ = new SensorController(this);
    }

public:
    WkEsp32s3Dev() : boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        InitializePirSensor();
        InitializeLedGpio();
        InitializeAdc();

#ifdef CONFIG_BOARD_WK_HAVE_MOTOR
        InitializeMotor();
        InitializeMotorMcp();
#endif

        InitializeLedMcp();
        InitializeVolumeMcp();
        InitializeBatteryMcp();
        InitializeSensorMcp();

        xTaskCreate(LedTask, "led_task", 4096, this, 5, nullptr);

#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeFaceMcp();
        InitializeEmotionMcp();
        ShowFace("neutral");
#endif

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        audio_codec_ = GetAudioCodec();
        if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
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

    virtual Led* GetLed() override {
        static SingleLed led(LED_1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Max98357aCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_BCLK,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_LRCK,
            (gpio_num_t)AUDIO_I2S_SPK_GPIO_DOUT,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_SCK,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_WS,
            (gpio_num_t)AUDIO_I2S_MIC_GPIO_DIN
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
};

// ===== SENSOR CONTROLLER =====
SensorController::SensorController(WkEsp32s3Dev* board) {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.sensor.motion_detected", "Kiểm tra chuyển động",
        PropertyList(),
        [board](const PropertyList& p) -> ReturnValue {
            return board->ReadMotionDetected() ? "Có chuyển động" : "Không";
        });
}

DECLARE_BOARD(WkEsp32s3Dev);
