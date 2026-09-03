#include "wk_esp32s3_dev.h"

std::string WkEsp32s3Dev::sanitizeKey(const std::string& input) {
    std::string output = "";
    for (char c : input) {
        if (isalnum(c) || c == '_') output += tolower(c);
        else output += '_';
    }
    if (output.length() > 15) output = output.substr(0, 15);
    return output;
}

bool WkEsp32s3Dev::saveIRCodeToNVS(const std::string& deviceName, const uint8_t* data, size_t length) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) != ESP_OK) return false;
    std::string key = sanitizeKey(deviceName);
    esp_err_t err = nvs_set_blob(handle, key.c_str(), data, length);
    nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool WkEsp32s3Dev::playIRCodeFromNVS(const std::string& deviceName) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READONLY, &handle) != ESP_OK) return false;
    std::string key = sanitizeKey(deviceName);
    size_t required_size = 0;
    if (nvs_get_blob(handle, key.c_str(), NULL, &required_size) != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    uint8_t* ir_data = new uint8_t[required_size];
    if (nvs_get_blob(handle, key.c_str(), ir_data, &required_size) == ESP_OK) {
        SendCustomIrSignal(ir_data, required_size);
        delete[] ir_data;
        nvs_close(handle);
        return true;
    }
    delete[] ir_data;
    nvs_close(handle);
    return false;
}

void WkEsp32s3Dev::InitializeInfrared() {
    ir_initialized_ = true;
}

void WkEsp32s3Dev::StartLearningIr(const std::string& targetName) {
    is_learning_mode = true;
    active_learning_device_ = targetName;
}

void WkEsp32s3Dev::SendCustomIrSignal(const uint8_t* data, size_t len) {
    // Gửi tín hiệu IR thực tế qua phần cứng
}

bool WkEsp32s3Dev::ReceiveCustomIrSignal(uint8_t* buffer, size_t max_len, size_t* out_len) {
    return false;
}

void WkEsp32s3Dev::IrTask(void* arg) {
    WkEsp32s3Dev* board = (WkEsp32s3Dev*)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (board->is_learning_mode) {
            uint8_t dummy_buf[64];
            size_t out_len = 0;
            if (board->ReceiveCustomIrSignal(dummy_buf, sizeof(dummy_buf), &out_len)) {
                board->saveIRCodeToNVS(board->active_learning_device_, dummy_buf, out_len);
                board->is_learning_mode = false;
            }
        }
    }
}

void WkEsp32s3Dev::InitializeInfraredMcp() {
    McpServer::GetInstance().AddTool(
        "self.ir.send", 
        "Phát tín hiệu hồng ngoại đã học", 
        PropertyList{}, // <--- Thêm tham số properties để đủ 4 tham số
        [this](const PropertyList& args, std::string& result) { // <--- Đổi kiểu callback sang PropertyList&
            auto device_it = args.find("device");
            if (device_it != args.end()) {
                std::string device_name = std::get<std::string>(device_it->second);
                bool success = playIRCodeFromNVS(device_name);
                result = success ? "{\"status\": \"success\"}" : "{\"status\": \"not_found\"}";
                return success;
            }
            result = "{\"status\": \"error\"}";
            return false;
        }
    );
}
