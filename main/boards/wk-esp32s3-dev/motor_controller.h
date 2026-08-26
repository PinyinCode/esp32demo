#ifndef MOTOR_CONTROLLER_H
#define MOTOR_CONTROLLER_H

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <algorithm>
#include <esp_log.h>

class MotorController {
private:
    gpio_num_t in1_, in2_, in3_, in4_;
    
    void InitializeMotor();
    
public:
    MotorController(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4);
    ~MotorController();
    
    void SetLeftMotor(int speed);
    void SetRightMotor(int speed);
    void StopAll();
    void Forward(int speed);
    void Backward(int speed);
    void TurnLeft(int speed);
    void TurnRight(int speed);
    
    void InitializeMcp();
};

#endif
