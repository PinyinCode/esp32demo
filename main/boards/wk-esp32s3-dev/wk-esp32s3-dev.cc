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
#include <driver/i2c.h>
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
#include <cstring>

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== AHT20 DEFINITIONS =====
#define AHT20_CMD_CALIBRATE    0xBE
#define AHT20_CMD_TRIGGER      0xAC
#define AHT20_CMD_SOFT_RESET   0xBA
#define AHT20_I2C_PORT         I2C_NUM_1

typedef struct {
    bool initialized;
    bool calibrated;
    float temperature;
    float humidity;
    uint32_t last_read_ms;
} aht20_handle_t;

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
    
    // ===== DISPLAY VARIABLES =====
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    Button volume_up_button_;
    Button volume_down_button_;
    SensorController* sensor_controller_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    int current_volume_ = 80;

    // ===== AHT20 VARIABLES =====
    aht20_handle_t* aht20_ = nullptr;
    const int SENSOR_READ_INTERVAL_MS = 2000;

    LedAnimation anim_led1_ = {PATTERN_OFF, 5, 255, 0, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, 255, 0, false};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;
    const int DEFAULT_LED_DURATION_ = 30;

    std::string current_emotion_ = "neutral";
    bool emotion_auto_mode_ = true;
    bool is_blinking_ = false;

    friend class SensorController;

    // ==========================================
    //  AHT20 FUNCTIONS
    // ==========================================
    
    esp_err_t aht20_write(uint8_t cmd, const uint8_t* data, size_t len) {
        i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
        i2c_master_start(cmd_handle);
        i2c_master_write_byte(cmd_handle, (AHT20_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd_handle, cmd, true);
        if (data && len > 0) {
            i2c_master_write(cmd_handle, data, len, true);
        }
        i2c_master_stop(cmd_handle);
        esp_err_t ret = i2c_master_cmd_begin(AHT20_I2C_PORT, cmd_handle, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd_handle);
        return ret;
    }

    esp_err_t aht20_read(uint8_t* data, size_t len) {
        i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
        i2c_master_start(cmd_handle);
        i2c_master_write_byte(cmd_handle, (AHT20_I2C_ADDR << 1) | I2C_MASTER_READ, true);
        if (len > 1) {
            for (int i = 0; i < len - 1; i++) {
                i2c_master_read_byte(cmd_handle, data + i, I2C_MASTER_ACK);
            }
        }
        i2c_master_read_byte(cmd_handle, data + len - 1, I2C_MASTER_LAST_NACK);
        i2c_master_stop(cmd_handle);
        esp_err_t ret = i2c_master_cmd_begin(AHT20_I2C_PORT, cmd_handle, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd_handle);
        return ret;
    }

    esp_err_t InitAHT20() {
#ifdef CONFIG_AHT20_ENABLED
        ESP_LOGI(TAG, "Initializing AHT20 sensor...");

        aht20_ = (aht20_handle_t*)calloc(1, sizeof(aht20_handle_t));
        if (!aht20_) {
            ESP_LOGE(TAG, "Failed to allocate AHT20 handle");
            return ESP_ERR_NO_MEM;
        }

        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = AHT20_SDA_PIN;
        conf.scl_io_num = AHT20_SCL_PIN;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        conf.clk_speed_hz = 100000;

        esp_err_t ret = i2c_param_config(AHT20_I2C_PORT, &conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2C");
            free(aht20_);
            aht20_ = nullptr;
            return ret;
        }

        ret = i2c_driver_install(AHT20_I2C_PORT, conf.mode, 0, 0, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install I2C driver");
            free(aht20_);
            aht20_ = nullptr;
            return ret;
        }

        ret = aht20_write(AHT20_CMD_SOFT_RESET, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset AHT20");
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t status;
        ret = aht20_read(&status, 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read AHT20 status");
            return ret;
        }

        if (status & 0x08) {
            aht20_->calibrated = true;
            ESP_LOGI(TAG, "AHT20 already calibrated");
        } else {
            uint8_t cal_cmd[2] = {0x08, 0x00};
            ret = aht20_write(AHT20_CMD_CALIBRATE, cal_cmd, 2);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to calibrate AHT20");
                return ret;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            
            ret = aht20_read(&status, 1);
            if (ret == ESP_OK && (status & 0x08)) {
                aht20_->calibrated = true;
                ESP_LOGI(TAG, "AHT20 calibrated successfully");
            } else {
                ESP_LOGW(TAG, "AHT20 calibration failed");
                aht20_->calibrated = false;
            }
        }

        aht20_->initialized = true;

        float temp, hum;
        ReadAHT20(&temp, &hum);
        aht20_->last_read_ms = esp_timer_get_time() / 1000;
        
        ESP_LOGI(TAG, "AHT20 initialized successfully");
        return ESP_OK;
#else
        ESP_LOGW(TAG, "AHT20 disabled in config");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    esp_err_t ReadAHT20(float* temperature, float* humidity) {
        if (!aht20_ || !aht20_->calibrated || !aht20_->initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t trigger_cmd[3] = {0x33, 0x00, 0x00};
        esp_err_t ret = aht20_write(AHT20_CMD_TRIGGER, trigger_cmd, 3);
        if (ret != ESP_OK) return ret;

        vTaskDelay(pdMS_TO_TICKS(80));

        uint8_t data[6];
        ret = aht20_read(data, 6);
        if (ret != ESP_OK) return ret;

        if (data[0] & 0x80) {
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t raw_humi = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((data[3] & 0xF0) >> 4);
        uint32_t raw_temp = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

        *humidity = (float)raw_humi * 100.0f / 1048576.0f;
        *temperature = (float)raw_temp * 200.0f / 1048576.0f - 50.0f;

        aht20_->temperature = *temperature;
        aht20_->humidity = *humidity;
        aht20_->last_read_ms = esp_timer_get_time() / 1000;

        return ESP_OK;
    }

    void UpdateSensorData() {
        if (!aht20_ || !aht20_->initialized) return;
        uint32_t now = esp_timer_get_time() / 1000;
        if (now - aht20_->last_read_ms < SENSOR_READ_INTERVAL_MS) {
            return;
        }

        float temp, hum;
        if (ReadAHT20(&temp, &hum) == ESP_OK) {
            ESP_LOGD(TAG, "Temp: %.2f°C, Humidity: %.2f%%", temp, hum);
        }
    }

    void InitializeAHT20Mcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.sensor.temperature", "Lấy nhiệt độ hiện tại", 
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (aht20_ && aht20_->calibrated && aht20_->initialized) {
                    float temp, hum;
                    if (ReadAHT20(&temp, &hum) == ESP_OK) {
                        char buffer[32];
                        snprintf(buffer, sizeof(buffer), "%.1f°C", temp);
                        return std::string(buffer);
                    }
                }
                return std::string("Không thể đọc nhiệt độ");
            });

        mcp.AddTool("self.sensor.humidity", "Lấy độ ẩm hiện tại", 
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (aht20_ && aht20_->calibrated && aht20_->initialized) {
                    float temp, hum;
                    if (ReadAHT20(&temp, &hum) == ESP_OK) {
                        char buffer[32];
                        snprintf(buffer, sizeof(buffer), "%.1f%%", hum);
                        return std::string(buffer);
                    }
                }
                return std::string("Không thể đọc độ ẩm");
            });

        mcp.AddTool("self.sensor.temp_humidity", "Lấy nhiệt độ và độ ẩm", 
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                if (aht20_ && aht20_->calibrated && aht20_->initialized) {
                    float temp, hum;
                    if (ReadAHT20(&temp, &hum) == ESP_OK) {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "Nhiệt độ: %.1f°C, Độ ẩm: %.1f%%", temp, hum);
                        return std::string(buffer);
                    }
                }
                return std::string("Không thể đọc cảm biến");
            });
    }

    // ==========================================
    //  HIỂN THỊ MẮT ĐỘNG
    // ==========================================
    void UpdateDisplayAnimation() {
        if (!display_) return;

        auto& app = Application::GetInstance();
        
        static uint32_t last_blink_time = 0;
        uint32_t now_ms = esp_timer_get_time() / 1000;
        if (!is_blinking_ && (now_ms - last_blink_time > 4000)) {
            is_blinking_ = true;
            last_blink_time = now_ms;
        } else if (is_blinking_ && (now_ms - last_blink_time > 150)) {
            is_blinking_ = false;
        }

        std::string eyes;
        if (is_blinking_) {
            eyes = "- -";
        } else if (current_emotion_ == "happy") {
            eyes = "^ ^";
        } else if (current_emotion_ == "sad") {
            eyes = "v v";
        } else if (app.GetDeviceState() == kDeviceStateListening) {
            eyes = "O O";
        } else if (app.GetDeviceState() == kDeviceStateSpeaking) {
            static int frame = 0;
            frame++;
            eyes = (frame % 4 == 0) ? "o o" : "O O";
        } else {
            eyes = "O O";
        }

        display_->SetEmotion(eyes.c_str());
        display_->SetStatus(GetStatusText().c_str());
    }

    std::string GetStatusText() {
        auto& app = Application::GetInstance();
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle: return "San sang";
            case kDeviceStateConnecting: return "Dang ket noi...";
            case kDeviceStateListening: return "Dang nghe...";
            case kDeviceStateSpeaking: return "Dang noi...";
            default: return "San sang";
        }
    }

    void ShowEmotionDisplay(const std::string& emotion) {
        current_emotion_ = emotion;
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
        
        gpio_set_level((gpio_num_t)led_pin, brightness > 50 ? 1 : 0);
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
        if (emotion == current_emotion_ && emotion_auto_mode_ == false) return;
        current_emotion_ = emotion;
        emotion_auto_mode_ = false;
        
        ESP_LOGI(TAG, "Emotion: %s", emotion.c_str());
        
        ShowEmotionDisplay(emotion);
        
        if (emotion == "happy") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 5, 255, 0, true};
            anim_led2_ = {PATTERN_BREATH, 5, 255, 0, true};
            SetLedTimeout(5);
            SetLeftMotor(40); SetRightMotor(40);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(-30); SetRightMotor(-30);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "sad") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 1, 255, 0, true};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false};
            SetLedTimeout(5);
            SetLeftMotor(-20); SetRightMotor(-20);
            vTaskDelay(pdMS_TO_TICKS(1000));
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "angry") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BLINK_FAST, 15, 255, 0, true};
            anim_led2_ = {PATTERN_BLINK_FAST, 15, 255, 0, true};
            SetLedTimeout(3);
            SetLeftMotor(60); SetRightMotor(60);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "scared") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BLINK_FAST, 25, 255, 0, true};
            anim_led2_ = {PATTERN_BLINK_FAST, 25, 255, 0, true};
            SetLedTimeout(3);
            SetLeftMotor(-70); SetRightMotor(-70);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0); SetRightMotor(0);
            for (int i = 0; i < 5; i++) {
                SetLeftMotor(-30); SetRightMotor(30);
                vTaskDelay(pdMS_TO_TICKS(100));
                SetLeftMotor(30); SetRightMotor(-30);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "love") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_HEARTBEAT, 5, 255, 0, true};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false};
            SetLedTimeout(5);
            for (int i = 0; i < 3; i++) {
                SetLeftMotor(-20); SetRightMotor(20);
                vTaskDelay(pdMS_TO_TICKS(400));
                SetLeftMotor(20); SetRightMotor(-20);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "neutral") {
            led_auto_mode_ = true;
            led_timeout_ms_ = 0;
            emotion_auto_mode_ = true;
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "thinking") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 2, 255, 0, true};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false};
            SetLedTimeout(10);
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "listening") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 3, 255, 0, true};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false};
            SetLeftMotor(-15); SetRightMotor(15);
            vTaskDelay(pdMS_TO_TICKS(500));
            SetLeftMotor(0); SetRightMotor(0);
        }
        else if (emotion == "speaking") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 4, 255, 0, true};
            anim_led2_ = {PATTERN_BREATH, 4, 255, 0, true};
            SetLeftMotor(15); SetRightMotor(15);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(-10); SetRightMotor(-10);
            vTaskDelay(pdMS_TO_TICKS(300));
            SetLeftMotor(0); SetRightMotor(0);
        }
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
        led_tick_ += 50;
        
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
                    anim_led1_.pattern = PATTERN_BREATH; anim_led1_.speed = 3; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_OFF; anim_led2_.active = false;
                    break;
                case kDeviceStateConnecting:
                    anim_led1_.pattern = PATTERN_PULSE; anim_led1_.speed = 8; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_PULSE; anim_led2_.speed = 8; anim_led2_.active = true;
                    break;
                case kDeviceStateListening:
                    anim_led1_.pattern = PATTERN_BLINK_FAST; anim_led1_.speed = 10; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_WAVE; anim_led2_.speed = 7; anim_led2_.active = true;
                    break;
                case kDeviceStateSpeaking:
                    anim_led1_.pattern = PATTERN_HEARTBEAT; anim_led1_.speed = 5; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_BREATH; anim_led2_.speed = 4; anim_led2_.active = true;
                    break;
                default:
                    anim_led1_.pattern = PATTERN_BLINK_FAST; anim_led1_.speed = 12; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_BLINK_FAST; anim_led2_.speed = 12; anim_led2_.active = true;
                    break;
            }
        }
        
        ApplyLedEffect(LED_1, anim_led1_);
        ApplyLedEffect(LED_2, anim_led2_);
        
        UpdateSensorData();
    }

    static void LedCreativeTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        while (1) {
            board->UpdateLedCreative();
            board->UpdateDisplayAnimation();
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
            .gpio_num = DRV8833_IN1, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0
        };
        ledc_channel_config(&ch1);
        
        ledc_channel_config_t ch2 = {
            .gpio_num = DRV8833_IN2, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0
        };
        ledc_channel_config(&ch2);
        
        ledc_channel_config_t ch3 = {
            .gpio_num = DRV8833_IN3, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_2, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0
        };
        ledc_channel_config(&ch3);
        
        ledc_channel_config_t ch4 = {
            .gpio_num = DRV8833_IN4, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_3, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0
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

    bool ReadMotionDetected() {
        return gpio_get_level(PIR_MOTION_SENSOR_PIN) == 1;
    }

    // ===== ULTRASONIC =====
    void InitializeUltrasonic() {
        ESP_LOGI(TAG, "Initialize Ultrasonic Sensor");
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

    // ===== MCP: MOTOR =====
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.motor.forward", "Robot tiến", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(speed);
                return "Tiến " + std::to_string(speed) + "%";
            });
        mcp.AddTool("self.motor.backward", "Robot lùi", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(-speed);
                return "Lùi " + std::to_string(speed) + "%";
            });
        mcp.AddTool("self.motor.turn_left", "Rẽ trái", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed);
                SetRightMotor(speed);
                return "Rẽ trái";
            });
        mcp.AddTool("self.motor.turn_right", "Rẽ phải", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed);
                SetRightMotor(-speed);
                return "Rẽ phải";
            });
        mcp.AddTool("self.motor.stop", "Dừng", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetLeftMotor(0);
                SetRightMotor(0);
                return "Dừng";
            });
    }

    // ===== MCP: AUDIO =====
    void InitializeVolumeMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.audio.volume_set", "Đặt âm lượng", PropertyList({Property("volume", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = p["volume"].value<int>();
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Âm lượng: " + std::to_string(current_volume_) + "%";
            });
    }

    // ===== MCP: LED =====
    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.led.on", "Bật LED", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                gpio_set_level(LED_1, 1);
                gpio_set_level(LED_2, 1);
                return "LED bật";
            });
        mcp.AddTool("self.led.off", "Tắt LED", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                gpio_set_level(LED_1, 0);
                gpio_set_level(LED_2, 0);
                return "LED tắt";
            });
        mcp.AddTool("self.led.auto", "LED tự động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                return "OK";
            });
    }

    // ===== MCP: EMOTION =====
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
        mcp.AddTool("self.emotion.auto", "Tự động", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                emotion_auto_mode_ = true;
                led_auto_mode_ = true;
                return "OK";
            });
    }

    // ===== MCP: BATTERY =====
    void InitializeBatteryMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.battery.level", "Mức pin", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                int adc_value = 0;
                adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value);
                return std::to_string((adc_value * 100) / 4095) + "%";
            });
    }

    // ===== MCP: SENSOR =====
    void InitializeSensorMcp() {
        sensor_controller_ = new SensorController(this);
        
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.sensor.motion_detected", "Kiểm tra chuyển động",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                return ReadMotionDetected() ? "Có chuyển động" : "Không có chuyển động";
            });
    }

    // ===== DISPLAY =====
    void InitDisplay() {
#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = DISPLAY_SDA_PIN;
        conf.scl_io_num = DISPLAY_SCL_PIN;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        conf.clk_speed_hz = 400000;
        
        ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
        ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0));
        
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(I2C_NUM_0, &io_config, &panel_io_));

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
#endif
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
        InitializeLedGpio();
        InitializeLedMcp();
        InitializeEmotionMcp();
        InitializeVolumeMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        // ===== KHỞI TẠO AHT20 =====
        esp_err_t ret = InitAHT20();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 initialization failed, sensor will be disabled");
        }

        // ===== MCP TOOL CHO AHT20 =====
        InitializeAHT20Mcp();

        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 8192, this, 5, nullptr);

        InitDisplay();
        if (display_) {
            ShowEmotionDisplay("neutral");
        }

        InitializeButtons();
        InitializeTools();
        
        InitializeSensorMcp();
        
        audio_codec_ = GetAudioCodec();
        if (audio_codec_) {
            audio_codec_->SetOutputVolume(current_volume_);
        }
    }

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
}

DECLARE_BOARD(WkEsp32s3Dev);
