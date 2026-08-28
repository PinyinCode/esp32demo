#include "my_ir.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

static const char *TAG = "MY_IR";
static rmt_channel_handle_t s_tx_chan = NULL;

void ir_tx_init(int gpio_num) {
    ESP_LOGI(TAG, "Initializing RMT TX channel on GPIO %d", gpio_num);
    
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, // 1 MHz -> 1 tick = 1 microsecond (µs)
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_tx_chan));
    
    // Cài đặt cấu hình sóng mang (Carrier) cho hồng ngoại, ví dụ 38kHz phổ biến của điều hòa/TV
    rmt_carrier_config_t carrier_config = {
        .duty_cycle = 33,
        .frequency_hz = 38000,
        .flags.polarity_active_low = false,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(s_tx_chan, &carrier_config));
    ESP_ERROR_CHECK(rmt_enable(s_tx_chan));
}

void ir_send_raw(uint32_t *durations, size_t length) {
    if (!s_tx_chan) {
        ESP_LOGE(TAG, "RMT TX channel not initialized!");
        return;
    }

    // Chuyển đổi mảng thời gian thành các xung RMT
    rmt_symbol_word_t *raw_symbols = malloc(length * sizeof(rmt_symbol_word_t));
    if (!raw_symbols) {
        ESP_LOGE(TAG, "Memory allocation failed for IR symbols");
        return;
    }

    for (size_t i = 0; i < length; i++) {
        // Lẻ là mức cao (có sóng mang), chẵn là mức thấp (tắt sóng mang)
        uint32_t duration = durations[i];
        if (i % 2 == 0) {
            raw_symbols[i] = (rmt_symbol_word_t){
                .val = 0,
                .duration0 = duration > 32767 ? 32767 : duration,
                .level0 = 1,
                .duration1 = 0,
                .level1 = 0,
            };
        } else {
            raw_symbols[i] = (rmt_symbol_word_t){
                .val = 0,
                .duration0 = duration > 32767 ? 32767 : duration,
                .level0 = 0,
                .duration1 = 0,
                .level1 = 0,
            };
        }
    }

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // Không lặp lại
    };

    ESP_ERROR_CHECK(rmt_transmit(s_tx_chan, &rmt_encode_raw, raw_symbols, length * sizeof(rmt_symbol_word_t), &transmit_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_tx_chan, portMAX_DELAY));

    free(raw_symbols);
}
