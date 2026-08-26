#include "face_display.h"
#include <esp_log.h>

#define TAG "FaceDisplay"

FaceDisplay::FaceDisplay(Display* display) : display_(display) {
    ESP_LOGI(TAG, "FaceDisplay initialized");
}

FaceDisplay::~FaceDisplay() {
}

void FaceDisplay::DrawEye(int center_x, int center_y, int radius, bool blink) {
    if (!display_) return;
    
    if (blink) {
        // Mắt nhắm - vẽ đường thẳng
        display_->DrawLine(center_x - radius, center_y, 
                          center_x + radius, center_y, 1);
    } else {
        // Mắt mở - vẽ hình tròn
        display_->DrawCircle(center_x, center_y, radius, 1);
        // Vẽ con ngươi
        display_->DrawCircle(center_x, center_y, radius / 2, 1);
    }
}

void FaceDisplay::DrawMouth(int center_x, int center_y, int width, int height, bool smile) {
    if (!display_) return;
    
    if (smile) {
        // Miệng cười - vẽ vòng cung lên
        for (int x = -width/2; x <= width/2; x++) {
            int y = -(height * x * x) / (width * width / 4) + height;
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    } else {
        // Miệng buồn - vẽ vòng cung xuống
        for (int x = -width/2; x <= width/2; x++) {
            int y = (height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    }
}

void FaceDisplay::DrawEyebrow(int center_x, int center_y, int width, int height, bool raised) {
    if (!display_) return;
    
    if (raised) {
        // Lông mày nhướng lên
        for (int x = -width/2; x <= width/2; x++) {
            int y = -(height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    } else {
        // Lông mày cau lại
        for (int x = -width/2; x <= width/2; x++) {
            int y = (height * x * x) / (width * width / 4);
            display_->DrawPixel(center_x + x, center_y + y, 1);
        }
    }
}

void FaceDisplay::DrawFace(const std::string& emotion) {
    if (!display_) return;
    
    // Xóa màn hình
    display_->Clear();
    
    if (emotion == "neutral") {
        // Mặt trung tính
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        display_->DrawLine(49, 45, 79, 45, 1);  // Miệng thẳng
    }
    else if (emotion == "happy") {
        // Mặt vui vẻ
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        DrawMouth(64, 45, 30, 10, true);  // Miệng cười
        DrawEyebrow(40, 18, 16, 3, true);  // Lông mày nhướng
        DrawEyebrow(88, 18, 16, 3, true);
    }
    else if (emotion == "sad") {
        // Mặt buồn
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        DrawMouth(64, 50, 30, 10, false);  // Miệng buồn
        DrawEyebrow(40, 18, 16, 3, false);  // Lông mày cau
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else if (emotion == "angry") {
        // Mặt giận dữ
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        DrawMouth(64, 50, 20, 5, false);  // Miệng cau
        DrawEyebrow(40, 15, 16, 5, false);  // Lông mày cau mạnh
        DrawEyebrow(88, 15, 16, 5, false);
    }
    else if (emotion == "surprised") {
        // Mặt ngạc nhiên
        DrawEye(40, 30, 12, false);  // Mắt to
        DrawEye(88, 30, 12, false);
        display_->DrawCircle(64, 45, 8, 1);  // Miệng tròn
        DrawEyebrow(40, 15, 16, 3, true);  // Lông mày nhướng cao
        DrawEyebrow(88, 15, 16, 3, true);
    }
    else if (emotion == "sleeping") {
        // Mặt ngủ
        DrawEye(40, 30, 8, true);  // Mắt nhắm
        DrawEye(88, 30, 8, true);
        DrawMouth(64, 45, 15, 3, true);  // Miệng nhỏ
    }
    else if (emotion == "thinking") {
        // Mặt suy nghĩ
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        // Mắt nhìn lên
        display_->DrawCircle(40, 26, 4, 1);
        display_->DrawCircle(88, 26, 4, 1);
        DrawMouth(64, 45, 20, 5, true);  // Miệng nhỏ
        DrawEyebrow(40, 18, 16, 3, true);  // Một bên nhướng
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else if (emotion == "listening") {
        // Mặt lắng nghe
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        // Con ngươi nhìn sang trái
        display_->DrawCircle(36, 30, 4, 1);
        display_->DrawCircle(84, 30, 4, 1);
        display_->DrawCircle(64, 45, 10, 1);  // Miệng chữ O
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, true);
    }
    else if (emotion == "speaking") {
        // Mặt nói chuyện
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        // Miệng mở - vẽ oval
        for (int i = 0; i < 6; i++) {
            display_->DrawCircle(64, 45, 5 + i, 1);
        }
    }
    else if (emotion == "blink") {
        // Mặt chớp mắt
        DrawEye(40, 30, 8, true);  // Cả hai mắt nhắm
        DrawEye(88, 30, 8, true);
        DrawMouth(64, 45, 20, 5, true);
    }
    else if (emotion == "winking") {
        // Mặt nháy mắt
        DrawEye(40, 30, 8, true);  // Mắt trái nhắm
        DrawEye(88, 30, 8, false);  // Mắt phải mở
        DrawMouth(64, 45, 25, 8, true);
    }
    else if (emotion == "love") {
        // Mặt yêu thương
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        // Miệng cười lớn
        DrawMouth(64, 45, 35, 12, true);
        // Vẽ trái tim nhỏ ở má
        display_->DrawCircle(30, 50, 3, 1);
        display_->DrawCircle(98, 50, 3, 1);
    }
    else if (emotion == "confused") {
        // Mặt bối rối
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        // Miệng lượn sóng
        display_->DrawLine(49, 45, 64, 42, 1);
        display_->DrawLine(64, 42, 79, 45, 1);
        // Một bên lông mày nhướng
        DrawEyebrow(40, 18, 16, 3, true);
        DrawEyebrow(88, 18, 16, 3, false);
    }
    else {
        // Mặc định - mặt trung tính
        DrawEye(40, 30, 8, false);
        DrawEye(88, 30, 8, false);
        display_->DrawLine(49, 45, 79, 45, 1);
    }
    
    // Cập nhật màn hình
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
    if (emotion == current_emotion_) {
        return;  // Không cần vẽ lại nếu cùng cảm xúc
    }
    
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
