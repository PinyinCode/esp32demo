#include "wk_esp32s3_dev.h"

esp_err_t WkEsp32s3Dev::aht20_write(uint8_t cmd, const uint8_t* data, size_t len) {
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (0x38 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_handle, cmd, true);
    for (size_t i = 0; i < len; i++) {
        i2c_master_write_byte(cmd_handle, data[i], true);
    }
    i2c_master_stop(cmd_handle);
    esp_err_t ret = i2c_master_cmd_begin(AHT20_I2C_PORT, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
    return ret;
}

esp_err_t WkEsp32s3Dev::aht20_read(uint8_t* data, size_t len) {
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_read_byte(cmd_handle, (0x38 << 1) | I2C_MASTER_READ, true);
    for (size_t i = 0; i < len - 1; i++) {
        i2c_master_read_byte(cmd_handle, &data[i], I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd_handle, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd_handle);
    esp_err_t ret = i2c_master_cmd_begin(AHT20_I2C_PORT, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
    return ret;
}

esp_err_t WkEsp32s3Dev::InitAHT20() {
    aht20_ = new aht20_handle_t{false, false, 0.0f, 0.0f, 0};
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t status = 0;
    if (aht20_read(&status, 1) != ESP_OK || !(status & 0x08)) {
        uint8_t reset_cmd = AHT20_CMD_SOFT_RESET;
        aht20_write(reset_cmd, nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t cal_data = 0x08;
        aht20_write(AHT20_CMD_CALIBRATE, &cal_data, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    aht20_->initialized = true;
    return ESP_OK;
}

esp_err_t WkEsp32s3Dev::ReadAHT20(float* temperature, float* humidity) {
    if (!aht20_ || !aht20_->initialized) return ESP_FAIL;
    uint8_t trigger_data[2] = {0x33, 0x00};
    if (aht20_write(AHT20_CMD_TRIGGER, trigger_data, 2) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(80));
    
    uint8_t data[6];
    if (aht20_read(data, 6) != ESP_OK) return ESP_FAIL;
    
    uint32_t hum_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    
    *humidity = (float)hum_raw * 100.0f / 1048576.0f;
    *temperature = (float)temp_raw * 200.0f / 1048576.0f - 50.0f;
    
    aht20_->temperature = *temperature;
    aht20_->humidity = *humidity;
    aht20_->last_read_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

void WkEsp32s3Dev::UpdateSensorData() {
    float t, h;
    ReadAHT20(&t, &h);
}

void WkEsp32s3Dev::InitializeAHT20Mcp() {
    McpServer::GetInstance().AddTool(
        "self.sensor.get_env", 
        "Lấy nhiệt độ và độ ẩm từ cảm biến AHT20", 
        PropertyList{}, // <--- Thêm tham số properties
        [this](const PropertyList& args, std::string& result) { // <--- Đổi const cJSON* thành const PropertyList&
            float t = 0, h = 0;
            ReadAHT20(&t, &h);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"temperature\": %.2f, \"humidity\": %.2f}", t, h);
            result = buf;
            return true;
        }
    );
}

void WkEsp32s3Dev::InitializeUltrasonic() {
    // Khởi tạo giao tiếp I2C hoặc chân GPIO cho cảm biến khoảng cách
}

float WkEsp32s3Dev::ReadUltrasonicDistanceCm() {
    return 25.0f; // Giá trị giả định mẫu
}

void WkEsp32s3Dev::InitializeAdc() {
    adc_oneshot_unit_init_config_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    esp_adc_oneshot_new_unit(&init_config, &adc_handle_);
}

void WkEsp32s3Dev::InitializeBatteryMcp() {
    McpServer::GetInstance().AddTool(
        "self.sensor.get_battery", 
        "Lấy phần trăm dung lượng pin", 
        PropertyList{}, // <--- Thêm tham số properties
        [this](const PropertyList& args, std::string& result) { // <--- Đổi kiểu callback
            result = "{\"battery_percent\": 85}";
            return true;
        }
    );
}

void WkEsp32s3Dev::InitializeSensorMcp() {
    McpServer::GetInstance().AddTool(
        "self.sensor.get_distance", 
        "Lấy khoảng cách từ cảm biến ToF/Ultrasonic", 
        PropertyList{}, // <--- Thêm tham số properties
        [this](const PropertyList& args, std::string& result) { // <--- Đổi kiểu callback
            float dist = ReadUltrasonicDistanceCm();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"distance_cm\": %.2f}", dist);
            result = buf;
            return true;
        }
    );
}
