#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

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

class LedController {
private:
    gpio_num_t led1_pin_, led2_pin_;
    LedAnimation anim_led1_ = {PATTERN_OFF, 5, false};
    LedAnimation anim_led2_ = {PATTERN_OFF, 5, false};
    uint32_t led_tick_ = 0;
    bool led_auto_mode_ = true;
    uint32_t led_timeout_ms_ = 0;
    uint32_t led_timeout_start_ = 0;
    const int DEFAULT_LED_DURATION_ = 30;
    TaskHandle_t led_task_handle_ = nullptr;
    
    int BreathEffect(uint32_t time_ms, int speed);
    int HeartbeatEffect(uint32_t time_ms);
    int WaveEffect(uint32_t time_ms, int speed, int led_index);
    int CometEffect(uint32_t time_ms, int speed, int led_index);
    int TwinkleEffect(uint32_t time_ms, int speed, int led_index);
    int PulseEffect(uint32_t time_ms, int speed);
    void ApplyLedEffect(int led_pin, LedAnimation anim);
    void SetLedTimeout(int duration_seconds);
    void UpdateLed();
    void UpdateLedByState();
    static void LedTask(void* arg);
    
public:
    LedController(gpio_num_t led1_pin, gpio_num_t led2_pin);
    ~LedController();
    void Initialize();
    void InitializeMcp();
    void SetLed1(bool on, int duration = 30);
    void SetLed2(bool on, int duration = 30);
    void SetBreath(int speed = 3, int duration = 30);
    void SetBlink(int speed = 5, int duration = 30);
    void SetHeartbeat(int duration = 30);
    void SetAutoMode();
    void OffAll();
};

#endif
