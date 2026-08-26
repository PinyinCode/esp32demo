#include "motor_controller.h"
#include "config.h"  // THÊM: Include config.h
#include "mcp_server.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "MotorController"

MotorController::MotorController() {
    InitializeMotor();
    ESP_LOGI(TAG, "Motor Controller initialized");
}

MotorController::~MotorController() {
    StopAll();
}

void MotorController::InitializeMotor() {
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

void MotorController::SetLeftMotor(int speed) {
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

void MotorController::SetRightMotor(int speed) {
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

void MotorController::StopAll() {
    SetLeftMotor(0);
    SetRightMotor(0);
}

void MotorController::Forward(int speed) {
    SetLeftMotor(speed);
    SetRightMotor(speed);
}

void MotorController::Backward(int speed) {
    SetLeftMotor(-speed);
    SetRightMotor(-speed);
}

void MotorController::TurnLeft(int speed) {
    SetLeftMotor(-speed);
    SetRightMotor(speed);
}

void MotorController::TurnRight(int speed) {
    SetLeftMotor(speed);
    SetRightMotor(-speed);
}

void MotorController::InitializeMcp() {
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
            StopAll();
            return "Đã dừng động cơ";
        });
        
    mcp.AddTool("self.motor.forward", "Robot tiến (speed: 0-100)",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            int speed = p["speed"].value<int>();
            Forward(speed);
            return "Robot tiến với tốc độ " + std::to_string(speed) + "%";
        });
        
    mcp.AddTool("self.motor.backward", "Robot lùi (speed: 0-100)",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            int speed = p["speed"].value<int>();
            Backward(speed);
            return "Robot lùi với tốc độ " + std::to_string(speed) + "%";
        });
        
    mcp.AddTool("self.motor.turn_left", "Robot rẽ trái (speed: 0-100)",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            int speed = p["speed"].value<int>();
            TurnLeft(speed);
            return "Robot rẽ trái";
        });
        
    mcp.AddTool("self.motor.turn_right", "Robot rẽ phải (speed: 0-100)",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            int speed = p["speed"].value<int>();
            TurnRight(speed);
            return "Robot rẽ phải";
        });
}
