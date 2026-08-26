#include "motor_controller.h"
#include "mcp_server.h"
#include <esp_log.h>

#define TAG "MotorController"

MotorController::MotorController(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4)
    : in1_(in1), in2_(in2), in3_(in3), in4_(in4) {
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
        .gpio_num = in1_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ch1);
    
    ledc_channel_config_t ch2 = {
        .gpio_num = in2_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ch2);
    
    ledc_channel_config_t ch3 = {
        .gpio_num = in3_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ch3);
    
    ledc_channel_config_t ch4 = {
        .gpio_num = in4_,
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
    
    mcp.AddTool("self.motor.forward", "Robot tiến",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            Forward(p["speed"].value<int>());
            return "Tiến";
        });
        
    mcp.AddTool("self.motor.backward", "Robot lùi",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            Backward(p["speed"].value<int>());
            return "Lùi";
        });
        
    mcp.AddTool("self.motor.turn_left", "Rẽ trái",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            TurnLeft(p["speed"].value<int>());
            return "Rẽ trái";
        });
        
    mcp.AddTool("self.motor.turn_right", "Rẽ phải",
        PropertyList({Property("speed", kPropertyTypeInteger, 50, 0, 100)}),
        [this](const PropertyList& p) -> ReturnValue {
            TurnRight(p["speed"].value<int>());
            return "Rẽ phải";
        });
        
    mcp.AddTool("self.motor.stop", "Dừng",
        PropertyList(),
        [this](const PropertyList& p) -> ReturnValue {
            StopAll();
            return "Dừng";
        });
}
