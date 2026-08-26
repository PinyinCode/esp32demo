#include "led_controller.h"
#include "application.h"
#include "mcp_server.h"
#include <esp_log.h>

#define TAG "LedController"

LedController::LedController() {}
LedController::~LedController() {
    if (led_task_handle_) vTaskDelete(led_task_handle_);
}

void LedController::Initialize() {
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
    xTaskCreate(LedTask, "led_task", 4096, this, 5, &led_task_handle_);
}

int LedController::BreathEffect(uint32_t time_ms, int speed) {
    float period = 2000.0f / speed;
    float phase = (time_ms % (int)period) / period * 2 * 3.14159f;
    return (int)((sin(phase) + 1) / 2 * 255);
}

int LedController::HeartbeatEffect(uint32_t time_ms) {
    uint32_t cycle = time_ms % 1000;
    if (cycle < 100) return 255;
    else if (cycle < 200) return 50;
    else if (cycle < 300) return 255;
    else if (cycle < 400) return 50;
    else return 0;
}

int LedController::WaveEffect(uint32_t time_ms, int speed, int led_index) {
    float period = 1500.0f / speed;
    float phase = (time_ms % (int)period) / period * 2 * 3.14159f;
    float phase_offset = led_index == 0 ? 0 : 3.14159f;
    return (int)((sin(phase + phase_offset) + 1) / 2 * 255);
}

int LedController::CometEffect(uint32_t time_ms, int speed, int led_index) {
    int cycle_time = 3000 / speed;
    int pos = (time_ms % cycle_time) * 255 / cycle_time;
    int brightness = 0;
    if (pos > 200) brightness = 255;
    else if (pos > 150) brightness = (pos - 150) * 5;
    return led_index == 0 ? brightness : brightness / 2;
}

int LedController::TwinkleEffect(uint32_t time_ms, int speed, int led_index) {
    uint32_t seed = (time_ms / (200 / speed)) + led_index * 1000;
    uint32_t random = (seed * 1103515245 + 12345) & 0x7fffffff;
    return (random % 256) > 200 ? 255 : (random % 256) > 100 ? 128 : 0;
}

int LedController::PulseEffect(uint32_t time_ms, int speed) {
    int pulse_width = 200 / speed;
    uint32_t cycle = time_ms % (pulse_width * 4);
    if (cycle < pulse_width) return (cycle * 255) / pulse_width;
    else if (cycle < pulse_width * 2) return 255 - ((cycle - pulse_width) * 255 / pulse_width);
    else return 0;
}

void LedController::ApplyLedEffect(int led_pin, LedAnimation anim) {
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
        case PATTERN_PULSE: brightness = PulseEffect(time, anim.speed); break;
        case PATTERN_TWINKLE: brightness = TwinkleEffect(time, anim.speed, led_pin == LED_1 ? 0 : 1); break;
        default: brightness = 0; break;
    }
    gpio_set_level((gpio_num_t)led_pin, brightness > 50 ? 1 : 0);
}

void LedController::SetLedTimeout(int duration_seconds) {
    if (duration_seconds <= 0) led_timeout_ms_ = 0;
    else {
        led_timeout_ms_ = duration_seconds * 1000;
        led_timeout_start_ = led_tick_;
    }
}

void LedController::UpdateLedByState() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    switch (state) {
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

void LedController::UpdateLed() {
    led_tick_ += 50;
    if (led_timeout_ms_ > 0 && led_tick_ - led_timeout_start_ >= led_timeout_ms_) {
        led_timeout_ms_ = 0;
        led_auto_mode_ = true;
    }
    if (led_auto_mode_) UpdateLedByState();
    ApplyLedEffect(LED_1, anim_led1_);
    ApplyLedEffect(LED_2, anim_led2_);
}

void LedController::LedTask(void* arg) {
    auto* controller = static_cast<LedController*>(arg);
    while (1) {
        controller->UpdateLed();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void LedController::SetLed1(bool on, int duration) {
    led_auto_mode_ = false;
    anim_led1_.active = false;
    gpio_set_level(LED_1, on ? 1 : 0);
    SetLedTimeout(duration);
}

void LedController::SetLed2(bool on, int duration) {
    led_auto_mode_ = false;
    anim_led2_.active = false;
    gpio_set_level(LED_2, on ? 1 : 0);
    SetLedTimeout(duration);
}

void LedController::SetBreath(int speed, int duration) {
    led_auto_mode_ = false;
    anim_led1_ = {PATTERN_BREATH, speed, true};
    anim_led2_ = {PATTERN_OFF, 0, false};
    SetLedTimeout(duration);
}

void LedController::SetBlink(int speed, int duration) {
    led_auto_mode_ = false;
    anim_led1_ = {PATTERN_BLINK_FAST, speed, true};
    anim_led2_ = {PATTERN_BLINK_FAST, speed, true};
    SetLedTimeout(duration);
}

void LedController::SetHeartbeat(int duration) {
    led_auto_mode_ = false;
    anim_led1_ = {PATTERN_HEARTBEAT, 5, true};
    anim_led2_ = {PATTERN_OFF, 0, false};
    SetLedTimeout(duration);
}

void LedController::SetAutoMode() {
    led_auto_mode_ = true;
    led_timeout_ms_ = 0;
}

void LedController::OffAll() {
    led_auto_mode_ = false;
    led_timeout_ms_ = 0;
    anim_led1_ = {PATTERN_OFF, 0, false};
    anim_led2_ = {PATTERN_OFF, 0, false};
    gpio_set_level(LED_1, 0);
    gpio_set_level(LED_2, 0);
}

void LedController::InitializeMcp() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.led.on", "Bật LED 1",
        PropertyList({Property("duration", kPropertyTypeInteger, 30, -1, 3600)}),
        [this](const PropertyList& p) -> ReturnValue {
            SetLed1(true, p["duration"].value<int>());
            return "LED 1 bật";
        });
    mcp.AddTool("self.led.off", "Tắt LED 1", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetLed1(false, 0);
            return "LED 1 tắt";
        });
    mcp.AddTool("self.led.breath", "LED thở",
        PropertyList({Property("speed", kPropertyTypeInteger, 3, 1, 10),
                      Property("duration", kPropertyTypeInteger, 30, -1, 3600)}),
        [this](const PropertyList& p) -> ReturnValue {
            SetBreath(p["speed"].value<int>(), p["duration"].value<int>());
            return "LED thở";
        });
    mcp.AddTool("self.led.blink", "LED nhấp nháy",
        PropertyList({Property("speed", kPropertyTypeInteger, 5, 1, 20),
                      Property("duration", kPropertyTypeInteger, 30, -1, 3600)}),
        [this](const PropertyList& p) -> ReturnValue {
            SetBlink(p["speed"].value<int>(), p["duration"].value<int>());
            return "LED nhấp nháy";
        });
    mcp.AddTool("self.led.heartbeat", "LED nhịp tim",
        PropertyList({Property("duration", kPropertyTypeInteger, 30, -1, 3600)}),
        [this](const PropertyList& p) -> ReturnValue {
            SetHeartbeat(p["duration"].value<int>());
            return "LED nhịp tim";
        });
    mcp.AddTool("self.led.auto", "LED tự động", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetAutoMode();
            return "LED tự động";
        });
    mcp.AddTool("self.led.off_all", "Tắt tất cả LED", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            OffAll();
            return "Đã tắt hết";
        });
}
