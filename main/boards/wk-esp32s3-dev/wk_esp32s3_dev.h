#pragma once

#include "wifi_board.h"
#include "max98357a_codec.h"
#include "display/lcd_display.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "project_config.h"
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
#include <esp_https_ota.h>
#include <esp_mac.h>
#include <cJSON.h>

#define TAG "WkEsp32s3Dev"


class WkEsp32s3Dev;

#define AHT20_CMD_CALIBRATE    0xBE
#define AHT20_CMD_TRIGGER      0xAC
#define AHT20_CMD_SOFT_RESET   0xBA
#define AHT20_I2C_PORT         I2C_NUM_1

#define TOF_I2C_PORT           I2C_NUM_0
#define TOF_I2C_ADDR           0x29

typedef struct {
    bool initialized;
    bool calibrated;
    float temperature;
    float humidity;
    uint32_t last_read_ms;
} aht20_handle_t;

class SensorController {
public:
    SensorController(WkEsp32s3Dev* board);
};

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
    
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    Button volume_up_button_;
    Button volume_down_button_;
    SensorController* sensor_controller_ = nullptr;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    int current_volume_ = 80;

    bool sys_kernel_secured_ = true;
    std::string device_chipid_str_ = "000000000000";
    std::string license_expiration_ = "Không xác định";

    bool bank_speaker_enabled_ = true; 
    TaskHandle_t bank_task_handle_ = nullptr;

    aht20_handle_t* aht20_ = nullptr;
    const int SENSOR_READ_INTERVAL_MS = 2000;

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

public:
    WkEsp32s3Dev();

    // Các hàm chia tách theo module
    static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
    std::string HttpGetRequest(const std::string& endpoint);

    // security_manager.cc
    void InitSystemKernelSecurity();
    void InitSystemKernelSecurityCore();
    void CheckAndPerformOta();
    static void DailyLicenseCheckTask(void* arg);
    static void SecurityCheckTask(void* arg);
    void InitializeSystemInfoMcp();

    // bank_manager.cc
    static void BankNotificationTask(void* arg);
    void InitializeBankSpeakerMcp();

    // ir_manager.cc
    std::string sanitizeKey(const std::string& input);
    bool saveIRCodeToNVS(const std::string& deviceName, const uint8_t* data, size_t length);
    bool playIRCodeFromNVS(const std::string& deviceName);
    void InitializeInfrared();
    void StartLearningIr(const std::string& targetName);
    void SendCustomIrSignal(const uint8_t* data, size_t len);
    bool ReceiveCustomIrSignal(uint8_t* buffer, size_t max_len, size_t* out_len);
    static void IrTask(void* arg);
    void InitializeInfraredMcp();

    // sensor_manager.cc
    esp_err_t aht20_write(uint8_t cmd, const uint8_t* data, size_t len);
    esp_err_t aht20_read(uint8_t* data, size_t len);
    esp_err_t InitAHT20();
    esp_err_t ReadAHT20(float* temperature, float* humidity);
    void UpdateSensorData();
    void InitializeAHT20Mcp();
    void InitializeUltrasonic();
    float ReadUltrasonicDistanceCm();
    void InitializeAdc();
    void InitializeBatteryMcp();
    void InitializeSensorMcp();

    // peripheral_manager.cc
    void UpdateDisplayAnimation();
    std::string GetStatusText();
    void ShowEmotionDisplay(const std::string& emotion);
    int BreathEffect(uint32_t time_ms, int speed);
    int HeartbeatEffect(uint32_t time_ms);
    int WaveEffect(uint32_t time_ms, int speed, int led_index);
    int CometEffect(uint32_t time_ms, int speed, int led_index);
    int TwinkleEffect(uint32_t time_ms, int speed, int led_index);
    int PulseEffect(uint32_t time_ms, int speed, int led_index);
    void ApplyLedEffect(int led_pin, LedAnimation anim);
    void SetLedTimeout(int duration_seconds);
    void ExecuteEmotion(const std::string& emotion);
    void UpdateEmotionByState();
    void UpdateLedCreative();
    static void LedCreativeTask(void* arg);
    void InitializeMotor();
    void SetLeftMotor(int speed);
    void SetRightMotor(int speed);
    void InitializeLedGpio();
    void InitializeMotorMcp();
    void InitializeVolumeMcp();
    void InitializeLedMcp();
    void InitializeEmotionMcp();
    void InitDisplay();

    void InitializeButtons();
    void InitializeTools();

    virtual Led* GetLed() override;
    virtual AudioCodec* GetAudioCodec() override;
    virtual Display* GetDisplay() override;
};
