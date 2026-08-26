#ifndef EMOTION_CONTROLLER_H
#define EMOTION_CONTROLLER_H

#include "face_display.h"
#include "motor_controller.h"
#include "led_controller.h"
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class EmotionController {
private:
    FaceDisplay* face_display_ = nullptr;
    MotorController* motor_controller_ = nullptr;
    LedController* led_controller_ = nullptr;
    std::string current_emotion_ = "neutral";
    bool auto_mode_ = true;
    TaskHandle_t emotion_task_handle_ = nullptr;
    
    void ExecuteEmotion(const std::string& emotion);
    static void EmotionTask(void* arg);
    void UpdateByState();
    
public:
    EmotionController(FaceDisplay* face, MotorController* motor, LedController* led);
    ~EmotionController();
    void SetEmotion(const std::string& emotion);
    void SetAutoMode(bool auto_mode);
    void InitializeMcp();
};

#endif
