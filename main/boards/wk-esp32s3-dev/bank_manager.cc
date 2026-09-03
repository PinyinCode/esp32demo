#include "wk_esp32s3_dev.h"

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
    McpServer::GetInstance().AddTool(
        "self.bank.set_speaker", 
        "Bật hoặc tắt loa thông báo ngân hàng", 
        PropertyList{}, 
        [this](const PropertyList& args, std::string& result) { // <--- Sử dụng const PropertyList& args
            auto enabled_it = args.find("enabled");
            if (enabled_it != args.end()) {
                bank_speaker_enabled_ = std::get<bool>(enabled_it->second);
                result = "{\"status\": \"success\"}";
                return true;
            }
            result = "{\"status\": \"error\", \"message\": \"Missing parameter\"}";
            return false;
        }
    );
}
