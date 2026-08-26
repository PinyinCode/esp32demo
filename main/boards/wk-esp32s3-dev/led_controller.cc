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
        case PATTERN_TWINKLE: brightness = TwinkleEffect(time, anim.speed
