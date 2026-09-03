#include "wk_esp32s3_dev.h"
#include "mcp_server.h"
#include <esp_log.h>
#include <esp_https_ota.h>
#include <cjson/cJSON.h>

#define TAG "WkEsp32s3DevSecurity"

void WkEsp32s3Dev::InitSystemKernelSecurity() {
    InitSystemKernelSecurityCore();
}

void WkEsp32s3Dev::InitSystemKernelSecurityCore() {
    std::string response = HttpGetRequest("/api/device/verify");
    if (!response.empty()) {
        cJSON *root = cJSON_Parse(response.c_str());
        if (root) {
            cJSON *status = cJSON_GetObjectItem(root, "status");
            cJSON *exp = cJSON_GetObjectItem(root, "expiration");
            if (status && cJSON_IsBool(status)) {
                sys_kernel_secured_ = status->valueint;
            }
            if (exp && cJSON_IsString(exp)) {
                license_expiration_ = std::string(exp->valuestring);
            }
            cJSON_Delete(root);
        }
    }
}

void WkEsp32s3Dev::CheckAndPerformOta() {
    std::string response = HttpGetRequest("/api/device/ota-check");
    if (!response.empty()) {
        cJSON *root = cJSON_Parse(response.c_str());
        if (root) {
            cJSON *update_available = cJSON_GetObjectItem(root, "update_available");
            cJSON *url = cJSON_GetObjectItem(root, "url");
            if (update_available && cJSON_IsTrue(update_available) && url && cJSON_IsString(url)) {
                ESP_LOGI(TAG, "Phát hiện bản cập nhật OTA mới, đang tiến hành tải về...");
                esp_http_client_config_t ota_config = {};
                ota_config.url = url->valuestring;
                ota_config.skip_cert_common_name_check = true;
                
                esp_https_ota_config_t https_ota_config = {
                    .http_config = &ota_config,
                };
                esp_https_ota(&https_ota_config);
            }
            cJSON_Delete(root);
        }
    }
}

void WkEsp32s3Dev::DailyLicenseCheckTask(void* arg) {
    WkEsp32s3Dev* board = (WkEsp32s3Dev*)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(86400000)); // Kiểm tra mỗi 24 giờ
        board->InitSystemKernelSecurityCore();
        board->CheckAndPerformOta();
    }
}

void WkEsp32s3Dev::SecurityCheckTask(void* arg) {
    WkEsp32s3Dev* board = (WkEsp32s3Dev*)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    board->InitSystemKernelSecurity();
    if (board->sys_kernel_secured_) {
        xTaskCreate(DailyLicenseCheckTask, "daily_license_task", 4096, board, 2, nullptr);
    }
    vTaskDelete(NULL);
}

void WkEsp32s3Dev::InitializeSystemInfoMcp() {
    // Sửa thành tham chiếu & và bỏ kiểm tra nullptr
    auto& mcp_server = McpServer::GetInstance();

    // Dùng dấu chấm (.) thay vì toán tử ->
    mcp_server.AddTool(
        "self.system.get_info", 
        "Lấy thông tin chip ID và hạn sử dụng hệ thống", 
        PropertyList{}, 
        [this](const PropertyList& args) -> ReturnValue {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "chip_id", device_chipid_str_.c_str());
            cJSON_AddBoolToObject(root, "secured", sys_kernel_secured_);
            cJSON_AddStringToObject(root, "expiration", license_expiration_.c_str());
            char* json_str = cJSON_PrintUnformatted(root);
            std::string result_str(json_str);
            free(json_str);
            cJSON_Delete(root);
            return result_str;
        }
    );
}
