#include "wk_esp32s3_dev.h"

esp_err_t WkEsp32s3Dev::_http_event_handler(esp_http_client_event_t *evt) {
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

std::string WkEsp32s3Dev::HttpGetRequest(const std::string& endpoint) {
    std::string url = std::string(SERVER_BASE_URL) + endpoint + "/" + device_chipid_str_;
    std::string response_data = "";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = _http_event_handler;
    config.user_data = &response_data;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    std::string result = "";
    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        result = response_data;
    }
    esp_http_client_cleanup(client);
    return result;
}

WkEsp32s3Dev::WkEsp32s3Dev() :
    boot_button_(BOOT_BUTTON_GPIO),
#if CONFIG_TOUCH_SENSOR_ENABLED
    touch_button_((gpio_num_t)CONFIG_TOUCH_SENSOR_GPIO),
#else
    touch_button_(TOUCH_BUTTON_GPIO),
#endif
    volume_up_button_(VOLUME_UP_BUTTON_GPIO),
    volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

    InitDisplay();

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint64_t chipid = 0;
    for (int i = 0; i < 6; i++) {
        chipid |= ((uint64_t)mac[i] << (8 * (5 - i)));
    }
    char chipid_str[20];
    snprintf(chipid_str, sizeof(chipid_str), "%012llX", (unsigned long long)chipid);
    device_chipid_str_ = std::string(chipid_str);

    InitializeSystemInfoMcp();

    xTaskCreate(SecurityCheckTask, "security_check_task", 4096, this, 3, nullptr);

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

    InitializeBankSpeakerMcp();
    xTaskCreate(BankNotificationTask, "bank_notification_task", 4096, this, 4, &bank_task_handle_);

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

void WkEsp32s3Dev::InitializeButtons() {
    boot_button_.OnClick([this]() {
        auto& app = Application::GetInstance();
        if (!sys_kernel_secured_) return;
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    });

#if CONFIG_TOUCH_SENSOR_ENABLED
    touch_button_.OnClick([this]() {
        auto& app = Application::GetInstance();
        if (!sys_kernel_secured_) return;
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    });
#endif
}

void WkEsp32s3Dev::InitializeTools() {}

Led* WkEsp32s3Dev::GetLed() {
    static SingleLed led(LED_1);
    return &led;
}

AudioCodec* WkEsp32s3Dev::GetAudioCodec() {
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

Display* WkEsp32s3Dev::GetDisplay() {
    return display_;
}

SensorController::SensorController(WkEsp32s3Dev* board) {}

DECLARE_BOARD(WkEsp32s3Dev);
