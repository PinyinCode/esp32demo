#ifndef MOTOR_CONTROLLER_H
#define MOTOR_CONTROLLER_H

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <algorithm>

class MotorController {
private:
    void InitializeMotor();
    
public:
    MotorController();
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
