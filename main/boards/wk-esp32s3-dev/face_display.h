#ifndef FACE_DISPLAY_H
#define FACE_DISPLAY_H

#include "display/oled_display.h"
#include <string>

class FaceDisplay {
private:
    Display* display_ = nullptr;
    std::string current_emotion_ = "";
    
    void DrawEye(int center_x, int center_y, int radius, bool blink = false);
    void DrawMouth(int center_x, int center_y, int width, int height, bool smile = true);
    void DrawEyebrow(int center_x, int center_y, int width, int height, bool raised = true);
    void DrawFace(const std::string& emotion);

public:
    FaceDisplay(Display* display);
    ~FaceDisplay();
    
    void ShowNeutral();
    void ShowHappy();
    void ShowSad();
    void ShowAngry();
    void ShowSurprised();
    void ShowSleeping();
    void ShowThinking();
    void ShowListening();
    void ShowSpeaking();
    void ShowBlink();
    void ShowWinking();
    void ShowLove();
    void ShowConfused();
    void Clear();
    
    void SetEmotion(const std::string& emotion);
    std::string GetCurrentEmotion() { return current_emotion_; }
};

#endif
