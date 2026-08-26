#include "emotion_controller.h"
#include "application.h"
#include "mcp_server.h"
#include <esp_log.h>

#define TAG "EmotionController"

EmotionController::EmotionController(FaceDisplay* face, MotorController* motor, LedController* led)
    : face_display_(face), motor_controller_(motor), led_controller_(led) {
    ESP_LOGI(TAG, "Emotion Controller initialized");
    xTaskCreate(EmotionTask, "emotion_task", 4096, this, 5, &emotion_task_handle_);
}

EmotionController::~EmotionController() {
    if (emotion_task_handle_) {
        vTaskDelete(emotion_task_handle_);
    }
}

void EmotionController::ExecuteEmotion(const std::string& emotion) {
    if (face_display_) face_display_->SetEmotion(emotion);
    
    if (emotion == "happy") {
        if (led_controller_) led_controller_->SetBreath(5, 5);
        if (motor_controller_) {
            motor_controller_->Forward(40);
            vTaskDelay(pdMS_TO_TICKS(300));
            motor_controller_->Backward(30);
            vTaskDelay(pdMS_TO_TICKS(300));
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "sad") {
        if (led_controller_) led_controller_->SetBreath(1, 5);
        if (motor_controller_) {
            motor_controller_->Backward(20);
            vTaskDelay(pdMS_TO_TICKS(1000));
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "angry") {
        if (led_controller_) led_controller_->SetBlink(15, 3);
        if (motor_controller_) {
            motor_controller_->Forward(60);
            vTaskDelay(pdMS_TO_TICKS(500));
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "scared") {
        if (led_controller_) led_controller_->SetBlink(25, 3);
        if (motor_controller_) {
            motor_controller_->Backward(70);
            vTaskDelay(pdMS_TO_TICKS(500));
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "love") {
        if (led_controller_) led_controller_->SetHeartbeat(5);
        if (motor_controller_) {
            for (int i = 0; i < 3; i++) {
                motor_controller_->TurnLeft(20);
                vTaskDelay(pdMS_TO_TICKS(400));
                motor_controller_->TurnRight(20);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "listening") {
        if (led_controller_) led_controller_->SetBreath(3, 0);
        if (motor_controller_) {
            motor_controller_->TurnLeft(15);
            vTaskDelay(pdMS_TO_TICKS(500));
            motor_controller_->StopAll();
        }
    }
    else if (emotion == "speaking") {
        if (led_controller_) led_controller_->SetBreath(4, 0);
    }
    else if (emotion == "sleeping") {
        if (led_controller_) led_controller_->OffAll();
        if (motor_controller_) motor_controller_->StopAll();
    }
    else if (emotion == "neutral") {
        if (led_controller_) led_controller_->SetAutoMode();
        if (motor_controller_) motor_controller_->StopAll();
    }
}

void EmotionController::SetEmotion(const std::string& emotion) {
    current_emotion_ = emotion;
    auto_mode_ = false;
    ExecuteEmotion(emotion);
}

void EmotionController::SetAutoMode(bool auto_mode) {
    auto_mode_ = auto_mode;
    if (auto_mode) UpdateByState();
}

void EmotionController::UpdateByState() {
    if (!auto_mode_) return;
    
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    std::string new_emotion = "neutral";
    
    switch (state) {
        case kDeviceStateIdle: new_emotion = "neutral"; break;
        case kDeviceStateConnecting: new_emotion = "thinking"; break;
        case kDeviceStateListening: new_emotion = "listening"; break;
        case kDeviceStateSpeaking: new_emotion = "speaking"; break;
        default: new_emotion = "neutral"; break;
    }
    
    if (new_emotion != current_emotion_) {
        current_emotion_ = new_emotion;
        ExecuteEmotion(new_emotion);
    }
}

void EmotionController::EmotionTask(void* arg) {
    auto* controller = static_cast<EmotionController*>(arg);
    while (1) {
        controller->UpdateByState();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void EmotionController::InitializeMcp() {
    auto& mcp = McpServer::GetInstance();
    
    mcp.AddTool("self.emotion.set", "Đặt cảm xúc",
        PropertyList({Property("emotion", kPropertyTypeString, "neutral")}),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion(p["emotion"].value<std::string>());
            return "OK";
        });
        
    mcp.AddTool("self.emotion.happy", "Vui vẻ", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion("happy");
            return "Vui!";
        });
        
    mcp.AddTool("self.emotion.sad", "Buồn", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion("sad");
            return "Buồn...";
        });
        
    mcp.AddTool("self.emotion.angry", "Giận dữ", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion("angry");
            return "Giận!";
        });
        
    mcp.AddTool("self.emotion.scared", "Sợ hãi", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion("scared");
            return "Sợ!";
        });
        
    mcp.AddTool("self.emotion.love", "Yêu thương", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetEmotion("love");
            return "Yêu!";
        });
        
    mcp.AddTool("self.emotion.auto", "Tự động", PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            SetAutoMode(true);
            return "OK";
        });
}
