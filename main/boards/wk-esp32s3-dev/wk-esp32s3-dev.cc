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
#include <nvs_flash.h>
#include <nvs.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <cstring>
#include <esp_http_client.h>
#include <esp_mac.h>
#include <cJSON.h>

#define TAG "WkEsp32s3Dev"

class WkEsp32s3Dev;

// ===== AHT20 DEFINITIONS =====
#define AHT20_CMD_CALIBRATE    0xBE
#define AHT20_CMD_TRIGGER      0xAC
#define AHT20_CMD_SOFT_RESET   0xBA
#define AHT20_I2C_PORT         I2C_NUM_1

// ===== TOF I2C DEFINITIONS =====
#define TOF_I2C_PORT           I2C_NUM_0
#define TOF_I2C_ADDR           0x29

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
    Button touch_button_;
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

    // ===== BẢN QUYỀN (LICENSE) VARIABLES =====
    bool is_license_valid_ = false;

    // ===== AHT20 VARIABLES =====
    aht20_handle_t* aht20_ = nullptr;
    const int SENSOR_READ_INTERVAL_MS = 2000;

    // ===== HỒNG NGOẠI (IR) & NVS STORAGE VARIABLES =====
    std::string last_captured_ir_code_ = "Chưa có";
    bool ir_initialized_ = false;
    uint32_t ir_raw_intervals[150];
    int ir_raw_len = 0;
    bool is_learning_mode = false;
    std::string active_learning_device_ = "";

    LedAnimation anim_led1_ = {PATTERN_OFF, 5, 255, 0, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, 255, 0, false};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;

    std::string current_emotion_ = "neutral";
    bool emotion_auto_mode_ = true;
    bool is_blinking_ = false;

    friend class SensorController;

    // ==========================================
    //  HỆ THỐNG KIỂM TRA BẢN QUYỀN TỪ SERVER API
    // ==========================================
    static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
        switch(evt->event_id) {
            case HTTP_EVENT_ON_DATA:
                if (evt->user_data) {
                    std::string* output = (std::string*)evt->user_data;
                    output->append((char*)evt->data, evt->data_len);
                }
                break;
            default:
                break;
        }
        return ESP_OK;
    }

    void CheckDeviceLicense() {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        std::string url = "https://license-rqbk.onrender.com/api/check-license?mac=" + std::string(mac_str);
        ESP_LOGI(TAG, "Đang kiểm tra bản quyền với URL: %s", url.c_str());

        std::string response_data = "";
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.event_handler = _http_event_handler;
        config.user_data = &response_data;
        config.timeout_ms = 5000;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200) {
                cJSON *root = cJSON_Parse(response_data.c_str());
                if (root) {
                    cJSON *status = cJSON_GetObjectItem(root, "status");
                    if (status && cJSON_IsString(status)) {
                        if (strcmp(status->valuestring, "active") == 0) {
                            is_license_valid_ = true;
                            ESP_LOGI(TAG, ">>> BẢN QUYỀN HỢP LỆ HOẶC ĐÃ TỰ ĐỘNG CẤP TRIAL 30 NGÀY!");
                        } else {
                            is_license_valid_ = false;
                            ESP_LOGW(TAG, ">>> BẢN QUYỀN ĐÃ HẾT HẠN HOẶC KHÔNG TỒN TẠI!");
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        } else {
            ESP_LOGE(TAG, "Không thể kết nối Server bản quyền, chạy offline tạm thời: %s", esp_err_to_name(err));
            is_license_valid_ = true; 
        }
        esp_http_client_cleanup(client);
    }

    // ==========================================
    //  HỒNG NGOẠI (IR) & HỌC LỆNH (NVS STORAGE)
    // ==========================================
    std::string sanitizeKey(const std::string& input) {
        std::string result = input;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        for (char &c : result) {
            if (c == ' ' || c == '-') c = '_';
        }
        if (result.length() > 15) {
            result = result.substr(0, 15);
        }
        return result;
    }

    bool saveIRCodeToNVS(const std::string& deviceName, const uint8_t* data, size_t length) {
        std::string key = sanitizeKey(deviceName);
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Lỗi mở NVS: %s", esp_err_to_name(err));
            return false;
        }

        err = nvs_set_blob(nvs_handle, key.c_str(), data, length);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Đã lưu thành công mã IR cho khóa: %s", key.c_str());
        }
        nvs_close(nvs_handle);
        return err == ESP_OK;
    }

    bool playIRCodeFromNVS(const std::string& deviceName) {
        std::string key = sanitizeKey(deviceName);
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Không thể mở NVS để đọc: %s", esp_err_to_name(err));
            return false;
        }

        size_t required_size = 0;
        err = nvs_get_blob(nvs_handle, key.c_str(), NULL, &required_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Không tìm thấy mã IR đã lưu cho thiết bị: %s", key.c_str());
            nvs_close(nvs_handle);
            return false;
        }

        uint8_t* ir_data = new uint8_t[required_size];
        err = nvs_get_blob(nvs_handle, key.c_str(), ir_data, &required_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Đang phát lệnh IR từ NVS cho: %s (Kích thước: %d)", key.c_str(), required_size);
            SendCustomIrSignal(ir_data, required_size);
        }

        delete[] ir_data;
        nvs_close(nvs_handle);
        return err == ESP_OK;
    }

    void InitializeInfrared() {
        ESP_LOGI(TAG, "Initializing Custom Infrared (IR) module with Learning feature...");

        gpio_config_t io_conf_tx = {};
        io_conf_tx.intr_type = GPIO_INTR_DISABLE;
        io_conf_tx.mode = GPIO_MODE_OUTPUT;
        io_conf_tx.pin_bit_mask = (1ULL << IR_TRANSMITTER_GPIO);
        io_conf_tx.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf_tx.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf_tx);
        gpio_set_level(IR_TRANSMITTER_GPIO, 0);

        gpio_config_t io_conf_rx = {};
        io_conf_rx.intr_type = GPIO_INTR_DISABLE;
        io_conf_rx.mode = GPIO_MODE_INPUT;
        io_conf_rx.pin_bit_mask = (1ULL << IR_RECEIVER_GPIO);
        io_conf_rx.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf_rx);

        ir_initialized_ = true;
        ESP_LOGI(TAG, "Custom IR Initialized. TX Pin: %d, RX Pin: %d", IR_TRANSMITTER_GPIO, IR_RECEIVER_GPIO);
    }

    void StartLearningIr(const std::string& targetName) {
        is_learning_mode = true;
        active_learning_device_ = targetName;
        ir_raw_len = 0;
        ESP_LOGI(TAG, "--- ĐANG BẬT CHẾ ĐỘ HỌC LỆNH CHO: %s --- Hãy bấm remote!", targetName.c_str());
    }

    void SendCustomIrSignal(const uint8_t* data, size_t len) {
        if (!ir_initialized_) return;
        ESP_LOGI(TAG, "Sending custom IR signal, length: %d", len);
    }

    bool ReceiveCustomIrSignal(uint8_t* buffer, size_t max_len, size_t* out_len) {
        if (!ir_initialized_) return false;

        if (is_learning_mode) {
            int level = gpio_get_level(IR_RECEIVER_GPIO);
            if (level == 0) {
                uint32_t start_time = (uint32_t)esp_timer_get_time();
                int count = 0;
                uint32_t last_edge = start_time;
                int last_level = 0;
                
                while (count < 150) {
                    int current_level = gpio_get_level(IR_RECEIVER_GPIO);
                    if (current_level != last_level) {
                        uint32_t now = (uint32_t)esp_timer_get_time();
                        ir_raw_intervals[count++] = now - last_edge;
                        last_edge = now;
                        last_level = current_level;
                    }
                    if ((esp_timer_get_time() - last_edge) > 50000) break; 
                }
                
                if (count > 10) {
                    ir_raw_len = count;
                    is_learning_mode = false;
                    saveIRCodeToNVS(active_learning_device_, (uint8_t*)ir_raw_intervals, ir_raw_len * sizeof(uint32_t));
                    last_captured_ir_code_ = active_learning_device_ + " (Xung: " + std::to_string(ir_raw_len) + ")";
                    ESP_LOGI(TAG, ">>> HỌC VÀ LƯU XONG CHO: %s", active_learning_device_.c_str());
                    active_learning_device_ = "";
                    return true;
                }
            }
        }
        return false;
    }

    static void IrTask(void* arg) {
        auto* board = static_cast<WkEsp32s3Dev*>(arg);
        uint8_t rx_buf[64];
        size_t rx_len = 0;
        while (1) {
            board->ReceiveCustomIrSignal(rx_buf, sizeof(rx_buf), &rx_len);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeInfraredMcp() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("self.ir.learn", "Học lệnh hồng ngoại cho một nút bất kỳ", 
            PropertyList({Property("name", kPropertyTypeString, "quat_so_1")}),
            [this](const PropertyList& p) -> ReturnValue {
                std::string name = p["name"].value<std::string>();
                StartLearningIr(name);
                return "Đang chờ học lệnh cho: " + name + ". Hãy bấm remote ngay!";
            });

        mcp.AddTool("self.fan.control", "Điều khiển quạt nhiều số hoặc tắt", 
            PropertyList({Property("action", kPropertyTypeString, "so_1")}), 
            [this](const PropertyList& p) -> ReturnValue {
                std::string action = p["action"].value<std::string>();
                std::string key = "fan_" + action;
                playIRCodeFromNVS(key);
                return "Đã thực hiện lệnh quạt: " + action;
            });

        mcp.AddTool("self.tv.control", "Bật tắt hoặc chỉnh âm lượng TV", 
            PropertyList({Property("command", kPropertyTypeString, "nguon")}), 
            [this](const PropertyList& p) -> ReturnValue {
                std::string cmd = p["command"].value<std::string>();
                std::string key = "tv_" + cmd;
                playIRCodeFromNVS(key);
                return "Đã gửi lệnh TV: " + cmd;
            });
    }

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
        aht20_ = (aht20_handle_t*)calloc(1, sizeof(aht20_handle_t));
        if (!aht20_) return ESP_ERR_NO_MEM;

        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = AHT20_SDA_PIN;
        conf.scl_io_num = AHT20_SCL_PIN;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;

        i2c_param_config(AHT20_I2C_PORT, &conf);
        i2c_driver_install(AHT20_I2C_PORT, conf.mode, 0, 0, 0);

        aht20_write(AHT20_CMD_SOFT_RESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t status;
        aht20_read(&status, 1);
        if (status & 0x08) {
            aht20_->calibrated = true;
        } else {
            uint8_t cal_cmd[2] = {0x08, 0x00};
            aht20_write(AHT20_CMD_CALIBRATE, cal_cmd, 2);
            vTaskDelay(pdMS_TO_TICKS(300));
            aht20_->calibrated = true;
        }

        aht20_->initialized = true;
        float temp, hum;
        ReadAHT20(&temp, &hum);
        aht20_->last_read_ms = esp_timer_get_time() / 1000;
        return ESP_OK;
    }

    esp_err_t ReadAHT20(float* temperature, float* humidity) {
        if (!aht20_ || !aht20_->calibrated || !aht20_->initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        uint8_t trigger_cmd[3] = {0x33, 0x00, 0x00};
        if (aht20_write(AHT20_CMD_TRIGGER, trigger_cmd, 3) != ESP_OK) return ESP_ERR_INVALID_STATE;

        vTaskDelay(pdMS_TO_TICKS(80));

        uint8_t data[6];
        if (aht20_read(data, 6) != ESP_OK) return ESP_ERR_INVALID_STATE;
        if (data[0] & 0x80) return ESP_ERR_INVALID_STATE;

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
        if (now - aht20_->last_read_ms < SENSOR_READ_INTERVAL_MS) return;

        float temp, hum;
        ReadAHT20(&temp, &hum);
    }

    void InitializeAHT20Mcp() {
        auto& mcp = McpServer::GetInstance();
        
        mcp.AddTool("self.sensor.temperature", "Lấy nhiệt độ hiện tại", PropertyList(),
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

        mcp.AddTool("self.sensor.humidity", "Lấy độ ẩm hiện tại", PropertyList(),
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

        mcp.AddTool("self.sensor.temp_humidity", "Lấy nhiệt độ và độ ẩm", PropertyList(),
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
    //  HIỂN THỊ MẮT ĐỘNG & LED SÁNG TẠO
    // ==========================================
    void UpdateDisplayAnimation() {
        if (!display_) return;

        if (!is_license_valid_) {
            display_->SetEmotion("X X");
            display_->SetStatus("Het han ban quyen");
            return;
        }

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
        if (is_blinking_) eyes = "- -";
        else if (current_emotion_ == "happy") eyes = "^ ^";
        else if (current_emotion_ == "sad") eyes = "v v";
        else if (app.GetDeviceState() == kDeviceStateListening) eyes = "O O";
        else if (app.GetDeviceState() == kDeviceStateSpeaking) {
            static int frame = 0;
            frame++;
            eyes = (frame % 4 == 0) ? "o o" : "O O";
        } else eyes = "O O";

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
        if (cycle < pulse_width) return (cycle * 255) / pulse_width;
        else if (cycle < pulse_width * 2) return 255 - ((cycle - pulse_width) * 255 / pulse_width);
        else return 0;
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
        if (duration_seconds <= 0) led_timeout_ms_ = 0;
        else {
            led_timeout_ms_ = duration_seconds * 1000;
            led_timeout_start_ = led_tick_;
        }
    }

    void ExecuteEmotion(const std::string& emotion) {
        if (emotion == current_emotion_ && emotion_auto_mode_ == false) return;
        current_emotion_ = emotion;
        emotion_auto_mode_ = false;
        
        ShowEmotionDisplay(emotion);
        
        if (emotion == "happy") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 5, 255, 0, true};
            anim_led2_ = {PATTERN_BREATH, 5, 255, 0, true};
            SetLedTimeout(5);
        } else if (emotion == "sad") {
            led_auto_mode_ = false;
            anim_led1_ = {PATTERN_BREATH, 1, 255, 0, true};
            anim_led2_ = {PATTERN_OFF, 0, 0, 0, false};
            SetLedTimeout(5);
        } else if (emotion == "neutral") {
            led_auto_mode_ = true;
            led_timeout_ms_ = 0;
            emotion_auto_mode_ = true;
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
        if (led_tick_ % 500 == 0) UpdateEmotionByState();
        
        if (led_timeout_ms_ > 0) {
            if (led_tick_ - led_timeout_start_ >= led_timeout_ms_) {
                led_timeout_ms_ = 0;
                led_auto_mode_ = true;
                emotion_auto_mode_ = true;
            }
        }
        
        if (led_auto_mode_) {
            auto& app = Application::GetInstance();
            switch (app.GetDeviceState()) {
                case kDeviceStateIdle:
                    anim_led1_.pattern = PATTERN_BREATH; anim_led1_.speed = 3; anim_led1_.active = true;
                    anim_led2_.pattern = PATTERN_OFF; anim_led2_.active = false;
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

    // ==========================================
    //  ĐỘNG CƠ DRV8833 & TOF & PIN
    // ==========================================
    void InitializeMotor() {
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 1000,
            .clk_cfg = LEDC_AUTO_CLK
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

    void InitializeUltrasonic() {
        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = ULTRASONIC_SDA_PIN;
        conf.scl_io_num = ULTRASONIC_SCL_PIN;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        i2c_param_config(TOF_I2C_PORT, &conf);
        i2c_driver_install(TOF_I2C_PORT, conf.mode, 0, 0, 0);
    }

    float ReadUltrasonicDistanceCm() {
        uint8_t reg_addr = 0x1E; 
        uint8_t data[2] = {0};
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (TOF_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg_addr, true);
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (TOF_I2C_ADDR << 1) | I2C_MASTER_READ, true);
        i2c_master_read(cmd, data, 2, I2C_MASTER_LAST_NACK);
        i2c_master_stop(cmd);
        
        esp_err_t ret = i2c_master_cmd_begin(TOF_I2C_PORT, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        if (ret != ESP_OK) return -1.0f;
        return (float)((data[0] << 8) | data[1]) / 10.0f;
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
        adc_oneshot_new_unit(&init_config, &adc_handle_);
        adc_oneshot_chan_cfg_t config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        adc_oneshot_config_channel(adc_handle_, POWER_ADC_CHANNEL, &config);
    }

    // ==========================================
    //  MCP TOOLS
    // ==========================================
    void InitializeMotorMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.motor.forward", "Robot tiến", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                if (!is_license_valid_) return "Thiết bị bị khóa bản quyền";
                int speed = p["speed"].value<int>();
                SetLeftMotor(speed); SetRightMotor(speed);
                return "Tiến " + std::to_string(speed) + "%";
            });
        mcp.AddTool("self.motor.backward", "Robot lùi", PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                if (!is_license_valid_) return "Thiết bị bị khóa bản quyền";
                int speed = p["speed"].value<int>();
                SetLeftMotor(-speed); SetRightMotor(-speed);
                return "Lùi " + std::to_string(speed) + "%";
            });
        mcp.AddTool("self.motor.stop", "Dừng", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                SetLeftMotor(0); SetRightMotor(0);
                return "Dừng";
            });
    }

    void InitializeVolumeMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.audio.volume_set", "Đặt âm lượng", PropertyList({Property("volume", kPropertyTypeInteger, 80, 0, 100)}),
            [this](const PropertyList& p) -> ReturnValue {
                current_volume_ = p["volume"].value<int>();
                if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
                return "Âm lượng: " + std::to_string(current_volume_) + "%";
            });
    }

    void InitializeLedMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.led.on", "Bật LED", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = false;
                gpio_set_level(LED_1, 1); gpio_set_level(LED_2, 1);
                return "LED bật";
            });
        mcp.AddTool("self.led.off", "Tắt LED", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                led_auto_mode_ = true;
                gpio_set_level(LED_1, 0); gpio_set_level(LED_2, 0);
                return "LED tắt";
            });
    }

    void InitializeEmotionMcp() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.emotion.set", "Đặt cảm xúc", PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
            [this](const PropertyList& p) -> ReturnValue {
                ExecuteEmotion(p["emotion"].value<std::string>());
                return "OK";
            });
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
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.sensor.distance", "Lấy khoảng cách vật cản phía trước", PropertyList(),
            [this](const PropertyList& p) -> ReturnValue {
                float dist = ReadUltrasonicDistanceCm();
                if (dist < 0) return std::string("Không thể đọc cảm biến khoảng cách");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "Khoảng cách là %.1f cm", dist);
                return std::string(buffer);
            });
    }

    void InitDisplay() {
#if CONFIG_WK_ESP32S3_DEV_DISPLAY_OLED
        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = DISPLAY_SDA_PIN;
        conf.scl_io_num = DISPLAY_SCL_PIN;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        
        i2c_param_config(I2C_NUM_0, &conf);
        i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
        
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        esp_lcd_new_panel_io_i2c(I2C_NUM_0, &io_config, &panel_io_);

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_disp_on_off(panel_, true);

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#endif
    }

public:
    WkEsp32s3Dev() :
        boot_button_(BOOT_BUTTON_GPIO),
#if CONFIG_TOUCH_SENSOR_ENABLED
        touch_button_((gpio_num_t)CONFIG_TOUCH_SENSOR_GPIO),
#else
        touch_button_(TOUCH_BUTTON_GPIO),
#endif
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        InitDisplay();

        // ** GỌI KIỂM TRA BẢN QUYỀN VÀ TỰ CẤP TRIAL NGAY KHI KHỞI ĐỘNG **
        CheckDeviceLicense();

#ifdef CONFIG_BOARD_WK_HAVE_MOTOR
        InitializeMotor();
        InitializeMotorMcp();
#endif

        InitializeUltrasonic();
        InitializeLedGpio();
        InitializeLedMcp();
        InitializeEmotionMcp();
        InitializeVolumeMcp();
        InitializeAdc();
        InitializeBatteryMcp();

        InitAHT20();
        InitializeAHT20Mcp();

        InitializeInfrared();
        InitializeInfraredMcp();

        anim_led1_.active = true;
        anim_led2_.active = true;
        xTaskCreate(LedCreativeTask, "led_creative", 8192, this, 5, nullptr);
        xTaskCreate(IrTask, "ir_task", 4096, this, 4, nullptr);

        if (display_) ShowEmotionDisplay("neutral");

        InitializeButtons();
        InitializeTools();
        InitializeSensorMcp();
        
        audio_codec_ = GetAudioCodec();
        if (audio_codec_) audio_codec_->SetOutputVolume(current_volume_);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (!is_license_valid_) {
                ESP_LOGW(TAG, "Thiết bị bị khóa do hết hạn bản quyền!");
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_TOUCH_SENSOR_ENABLED
        touch_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (!is_license_valid_) {
                ESP_LOGW(TAG, "Thiết bị bị khóa do hết hạn bản quyền!");
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
#endif
    }

    void InitializeTools() {}

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

SensorController::SensorController(WkEsp32s3Dev* board) {}

DECLARE_BOARD(WkEsp32s3Dev);
