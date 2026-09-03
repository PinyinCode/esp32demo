#include "wk_esp32s3_dev.h"
#include "mcp_server.h"
#include <esp_log.h>

#define TAG "WkEsp32s3Dev"

void WkEsp32s3Dev::BankNotificationTask(void* arg) {
    WkEsp32s3Dev* board = (WkEsp32s3Dev*)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Kiểm tra giao dịch mỗi 10 giây
        if (board->bank_speaker_enabled_ && WifiStation::GetInstance().IsConnected()) {
            std::string res = board->HttpGetRequest(API_BANK_STATS);
            if (!res.empty()) {
                cJSON *root = cJSON_Parse(res.c_str());
                if (root) {
                    cJSON *notify = cJSON_GetObjectItem(root, "has_new");
                    if (notify && cJSON_IsTrue(notify)) {
                        ESP_LOGI(TAG, "Phát hiện giao dịch ngân hàng mới!");
                        // Thực hiện các hành động phát âm thanh / thông báo ở đây
                    }
                    cJSON_Delete(root);
                }
            }
        }
    }
}

void WkEsp32s3Dev::InitializeBankSpeakerMcp() {
    // Sửa thành tham chiếu (&) và không cần check nullptr
    auto& mcp_server = McpServer::GetInstance();

    // Khai báo property cho tool MCP nếu cần nhận tham số "enabled"
    PropertyList properties;
    properties.push_back({"enabled", "boolean", "Bật hoặc tắt loa thông báo", true});

    // Sử dụng dấu chấm (.) thay vì ->
    mcp_server.AddTool(
        "self.bank.set_speaker", 
        "Bật hoặc tắt loa thông báo ngân hàng", 
        properties, 
        [this](const PropertyList& args) -> ReturnValue {
            auto enabled_it = args.find("enabled");
            if (enabled_it != args.end()) {
                bank_speaker_enabled_ = std::get<bool>(enabled_it->second);
                return std::string("{\"status\": \"success\"}");
            }
            return std::string("{\"status\": \"error\", \"message\": \"Missing parameter\"}");
        }
    );
}
