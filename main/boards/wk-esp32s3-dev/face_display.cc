#include "face_display.h"
#include <esp_log.h>

#define TAG "FaceDisplay"

FaceDisplay::FaceDisplay(Display* display) : display_(display) {
    ESP_LOGI(TAG, "FaceDisplay initialized");
}

FaceDisplay::~FaceDisplay() {}

void FaceDisplay::DrawEye(int center_x, int center_y, int radius, bool blink) {
    if (!display_) return;
    if (blink) {
        display_->DrawLine(center_x - radius, center_y, center_x + radius, center_y, 1);
    } else {
        display_->DrawCircle(center_x, center_y, radius, 1);
        display_->DrawCircle(center_x, center_y, radius / 2, 1);
    }
}

void FaceDisplay::DrawMouth(int center_x, int center_y, int width, int height, bool smile) {
    if (!display_) return;
    if (smile) {
        for (int x = -width/2; x <= width/2; x++) {
            int y = -(height * x * x) / (width * width / 4) + height;
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    } else {
        for (int x = -width/2; x <= width/2; x++) {
            int y = (height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    }
}

void FaceDisplay::DrawEyebrow(int center_x, int center_y, int width, int height, bool raised) {
    if (!display_) return;
    if (raised) {
        for (int x = -width/2; x <= width/2; x++) {
            int y = -(height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    } else {
        for (int x = -width/2; x <= width/2; x++) {
            int y = (height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    }
}

void FaceDisplay::DrawFace(const std::string& emotion) {
    if (!display_) return;
    display_->Clear();
    
    if (emotion == "neutral") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        display_->DrawLine(49, 45, 79, 45, 1);
    }
    else if (emotion == "happy") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        DrawMouth(64, 45, 30, 10, true);
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, true);
    }
    else if (emotion == "sad") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        DrawMouth(64, 50, 30, 10, false);
        DrawEyebrow(40, 18, 16, 3, false);
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else if (emotion == "angry") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        DrawMouth(64, 50, 20, 5, false);
        DrawEyebrow(40, 15, 16, 5, false);
        DrawEyebrow(88, 15, 16, 5, false);
    }
    else if (emotion == "surprised") {
        DrawEye(40, 30, 12);
        DrawEye(88, 30, 12);
        display_->DrawCircle(64, 45, 8, 1);
        DrawEyebrow(40, 15, 16, 3, true);
        DrawEyebrow(88, 15, 16, 3, true);
    }
    else if (emotion == "sleeping") {
        DrawEye(40, 30, 8, true);
        DrawEye(88, 30, 8, true);
        DrawMouth(64, 45, 15, 3, true);
    }
    else if (emotion == "thinking") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        display_->DrawCircle(40, 26, 4, 1);
        display_->DrawCircle(88, 26, 4, 1);
        DrawMouth(64, 45, 20, 5, true);
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else if (emotion == "listening") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        display_->DrawCircle(36, 30, 4, 1);
        display_->DrawCircle(84, 30, 4, 1);
        display_->DrawCircle(64, 45, 10, 1);
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, true);
    }
    else if (emotion == "speaking") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        for (int i = 0; i < 6; i++) {
            display_->DrawCircle(64, 45, 5 + i, 1);
        }
    }
    else if (emotion == "love") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        DrawMouth(64, 45, 35, 12, true);
        display_->DrawCircle(30, 50, 3, 1);
        display_->DrawCircle(98, 50, 3, 1);
    }
    else if (emotion == "confused") {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        display_->DrawLine(49, 45, 64, 42, 1);
        display_->DrawLine(64, 42, 79, 45, 1);
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else {
        DrawEye(40, 30, 8);
        DrawEye(88, 30, 8);
        display_->DrawLine(49, 45, 79, 45, 1);
    }
    
    display_->Update();
}

void FaceDisplay::ShowNeutral() { SetEmotion("neutral"); }
void FaceDisplay::ShowHappy() { SetEmotion("happy"); }
void FaceDisplay::ShowSad() { SetEmotion("sad"); }
void FaceDisplay::ShowAngry() { SetEmotion("angry"); }
void FaceDisplay::ShowSurprised() { SetEmotion("surprised"); }
void FaceDisplay::ShowSleeping() { SetEmotion("sleeping"); }
void FaceDisplay::ShowThinking() { SetEmotion("thinking"); }
void FaceDisplay::ShowListening() { SetEmotion("listening"); }
void FaceDisplay::ShowSpeaking() { SetEmotion("speaking"); }
void FaceDisplay::ShowBlink() { SetEmotion("blink"); }
void FaceDisplay::ShowWinking() { SetEmotion("winking"); }
void FaceDisplay::ShowLove() { SetEmotion("love"); }
void FaceDisplay::ShowConfused() { SetEmotion("confused"); }

void FaceDisplay::SetEmotion(const std::string& emotion) {
    if (emotion == current_emotion_) return;
    current_emotion_ = emotion;
    DrawFace(emotion);
    ESP_LOGI(TAG, "Emotion: %s", emotion.c_str());
}

void FaceDisplay::Clear() {
    if (!display_) return;
    display_->Clear();
    display_->Update();
    current_emotion_ = "";
}
