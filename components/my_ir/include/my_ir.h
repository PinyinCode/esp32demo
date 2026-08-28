#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo chân GPIO phát hồng ngoại (ví dụ chân GPIO 4)
void ir_tx_init(int gpio_num);

// Phát một chuỗi dữ liệu thô (raw data) xung hồng ngoại
void ir_send_raw(uint32_t *durations, size_t length);

#ifdef __cplusplus
}
#endif
