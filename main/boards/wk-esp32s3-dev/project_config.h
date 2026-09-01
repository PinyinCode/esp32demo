#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

// Cấu hình máy chủ chính
#define SERVER_BASE_URL          "https://esp32-bank-speaker.onrender.com"
#define OTA_SERVER_URL           "https://esp32-ota-server-9yuy.onrender.com"

// Các Endpoint API
#define API_CHECK_BANK_AUDIO     "/api/check-bank-audio"
#define API_BANK_HISTORY         "/api/bank-history"
#define API_BANK_STATS           "/api/bank-stats"
#define API_TRIGGER_EMAIL        "/api/trigger-check-email"
#define API_CHECK_LICENSE        "/api/check-license"
#define API_CHECK_UPDATE         "/api/check-update"

#endif // PROJECT_CONFIG_H
