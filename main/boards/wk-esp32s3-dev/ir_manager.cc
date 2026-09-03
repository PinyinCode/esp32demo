#include "wk_esp32s3_dev.h"
#include "mcp_server.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cctype>

#define TAG "WkEsp32s3DevIR"

std::string WkEsp32s3Dev::sanitizeKey(const std::string& input) {
    std::string output = "";
    for (char c : input) {
        if (isalnum(c) || c == '_') {
            output += tolower(c);
        } else {
            output += '_';
        }
    }
    if (output.length() > 15) {
        output = output.substr(0, 15);
    }
    return output;
}

bool WkEsp32s3Dev::saveIRCodeToNVS(const std::string& deviceName, const uint8_t* data, size_t length) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    std::string key = sanitizeKey(deviceName);
    esp_err_t err = nvs_set_blob(handle, key.c_str(), data, length);
    nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool WkEsp32s3Dev::playIRCodeFromNVS(const std::string& deviceName) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
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
    ESP_LOGI(TAG, "Khoi tạo phân cứng Hồng ngoại thành công.");
}

void WkEsp32s3Dev::StartLearningIr(const std::string& targetName) {
    is_learning_mode = true;
    active_learning_device_ = targetName;
    ESP_LOGI(TAG, "Bắt đầu chế độ học lệnh IR cho thiết bị: %s", targetName.c_str());
}

void WkEsp32s3Dev::SendCustomIrSignal(const uint8_t* data, size_t len) {
    // Thực hiện logic phần cứng để phát tín hiệu IR ở đây
    ESP_LOGI(TAG, "Đang phát tín hiệu IR với độ dài: %u bytes", (unsigned int)len);
}

bool WkEsp32s3Dev::ReceiveCustomIrSignal(uint8_t* buffer, size_t max_len, size_t* out_len) {
    // Logic nhận tín hiệu IR (trả về true khi nhận được lệnh từ điều khiển)
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
                ESP_LOGI(TAG, "Đã học và lưu thành công lệnh IR cho thiết bị.");
            }
        }
    }
}

void WkEsp32s3Dev::InitializeInfraredMcp() {
    // Lấy instance của McpServer dưới dạng tham chiếu (Không dùng con trỏ, không check nullptr)
    auto& mcp_server = McpServer::GetInstance();

    PropertyList properties;
    properties.push_back({"device", "string", "Tên thiết bị hồng ngoại cần phát tín hiệu", true});

    // Sử dụng dấu chấm (.) thay vì toán tử ->
    mcp_server.AddTool(
        "self.ir.send", 
        "Phát tín hiệu hồng ngoại đã học", 
        properties, 
        [this](const PropertyList& args) -> ReturnValue {
            auto device_it = args.find("device");
            if (device_it != args.end()) {
                std::string device_name = std::get<std::string>(device_it->second);
                bool success = playIRCodeFromNVS(device_name);
                return success ? std::string("{\"status\": \"success\"}") : std::string("{\"status\": \"not_found\"}");
            }
            return std::string("{\"status\": \"error\", \"message\": \"Missing device parameter\"}");
        }
    );
}
