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
#include <driver/i2s_std.h>

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== SENSOR CONTROLLER =====
class SensorController {
public:
    SensorController(WkEsp32s3Dev* board);
};

// ===== SIMPLE AUDIO CODEC (tự viết, không phụ thuộc thư viện ngoài) =====
class SimpleAudioCodec : public AudioCodec {
public:
    SimpleAudioCodec(int input_sample_rate, int output_sample_rate,
                     gpio_num_t spk_bclk, gpio_num_t spk_lrck, gpio_num_t spk_dout,
                     gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
        : AudioCodec(input_sample_rate, output_sample_rate) {
        
        ESP_LOGI("SimpleAudioCodec", "Initializing audio codec");
        ESP_LOGI("SimpleAudioCodec", "SPK: BCLK=%d, LRCK=%d, DOUT=%d", spk_bclk, spk_lrck, spk_dout);
        ESP_LOGI("SimpleAudioCodec", "MIC: SCK=%d, WS=%d, DIN=%d", mic_sck, mic_ws, mic_din);
        
        spk_bclk_ = spk_bclk;
        spk_lrck_ = spk_lrck;
        spk_dout_ = spk_dout;
        mic_sck_ = mic_sck;
        mic_ws_ = mic_ws;
        mic_din_ = mic_din;
        
        initialized_ = false;
    }
    
    virtual bool Start() override {
        if (initialized_) return true;
        
        ESP_LOGI("SimpleAudioCodec", "Starting audio codec");
        
        // Cấu hình I2S cho loa (MAX98357A)
        i2s_std_config_t spk_config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(output_sample_rate_),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = spk_bclk_,
                .ws = spk_lrck_,
                .dout = spk_dout_,
                .din = GPIO_NUM_NC,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        
        // Cấu hình I2S cho mic (INMP441)
        i2s_std_config_t mic_config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(input_sample_rate_),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = GPIO_NUM_NC,
                .bclk = mic_sck_,
                .ws = mic_ws_,
                .dout = GPIO_NUM_NC,
                .din = mic_din_,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        
        // Khởi tạo I2S cho loa
        esp_err_t ret = i2s_channel_alloc_std(&spk_config, &spk_tx_handle_, &spk_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE("SimpleAudioCodec", "Failed to allocate speaker I2S: %d", ret);
            return false;
        }
        
        ret = i2s_channel_enable(spk_tx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE("SimpleAudioCodec", "Failed to enable speaker I2S: %d", ret);
            return false;
        }
        
        // Khởi tạo I2S cho mic
        ret = i2s_channel_alloc_std(&mic_config, &mic_tx_handle_, &mic_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE("SimpleAudioCodec", "Failed to allocate mic I2S: %d", ret);
            return false;
        }
        
        ret = i2s_channel_enable(mic_rx_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE("SimpleAudioCodec", "Failed to enable mic I2S: %d", ret);
            return false;
        }
        
        initialized_ = true;
        ESP_LOGI("SimpleAudioCodec", "Audio codec started successfully");
        return true;
    }
    
    virtual bool Stop() override {
        if (!initialized_) return true;
        
        ESP_LOGI("SimpleAudioCodec", "Stopping audio codec");
        
        if (spk_tx_handle_) {
            i2s_channel_disable(spk_tx_handle_);
            i2s_del_channel(spk_tx_handle_);
            spk_tx_handle_ = nullptr;
        }
        
        if (mic_rx_handle_) {
            i2s_channel_disable(mic_rx_handle_);
            i2s_del_channel(mic_rx_handle_);
            mic_rx_handle_ = nullptr;
        }
        
        initialized_ = false;
        return true;
    }
    
    virtual bool Read(int16_t* data, int samples) override {
        if (!initialized_ || !mic_rx_handle_) return false;
        
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(mic_rx_handle_, data, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(100));
        return (ret == ESP_OK && bytes_read == samples * sizeof(int16_t));
    }
    
    virtual bool Write(int16_t* data, int samples) override {
        if (!initialized_ || !spk_tx_handle_) return false;
        
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(spk_tx_handle_, data, samples * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(100));
        return (ret == ESP_OK && bytes_written == samples * sizeof(int16_t));
    }
    
    virtual int GetInputSampleRate() override {
        return input_sample_rate_;
    }
    
    virtual int GetOutputSampleRate() override {
        return output_sample_rate_;
    }
    
    virtual AudioCodecType GetType() override {
        return kAudioCodecTypeGeneric;
    }

private:
    gpio_num_t spk_bclk_;
    gpio_num_t spk_lrck_;
    gpio_num_t spk_dout_;
    gpio_num_t mic_sck_;
    gpio_num_t mic_ws_;
    gpio_num_t mic_din_;
    
    i2s_chan_handle_t spk_tx_handle_ = nullptr;
    i2s_chan_handle_t spk_rx_handle_ = nullptr;
    i2s_chan_handle_t mic_tx_handle_ = nullptr;
    i2s_chan_handle_t mic_rx_handle_ = nullptr;
    
    bool initialized_ = false;
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

    // ===== ĐỘNG CƠ DRV8833 với PWM =====
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

    // ===== MCP: MOTOR với PWM =====
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

    // ===== MCP: LED =====
    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.led.on", "Bật LED 1",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_1, 1);
                return "Đã bật LED 1";
            });
            
        mcp.AddTool("self.led.off", "Tắt LED 1",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_1, 0);
                return "Đã tắt LED 1";
            });
            
        mcp.AddTool("self.led2.on", "Bật LED 2",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_2, 1);
                return "Đã bật LED 2";
            });
            
        mcp.AddTool("self.led2.off", "Tắt LED 2",
            PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                gpio_set_level(LED_2, 0);
                return "Đã tắt LED 2";
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
    }

    virtual Led* GetLed() override {
        static SingleLed led(LED_1);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static SimpleAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,    // BCLK loa
            AUDIO_I2S_SPK_GPIO_LRCK,    // LRCK loa
            AUDIO_I2S_SPK_GPIO_DOUT,    // DOUT loa
            AUDIO_I2S_MIC_GPIO_SCK,     // SCK mic
            AUDIO_I2S_MIC_GPIO_WS,      // WS mic
            AUDIO_I2S_MIC_GPIO_DIN      // DIN mic
        );
#else
        static SimpleAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN
        );
#endif
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
