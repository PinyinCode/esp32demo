#include "max98357a_codec.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "Max98357aCodec"

Max98357aCodec::Max98357aCodec(int input_sample_rate, int output_sample_rate,
                               gpio_num_t bclk, gpio_num_t lrck, gpio_num_t dout,
                               gpio_num_t sck, gpio_num_t ws, gpio_num_t din)
    : bclk_pin_(bclk), lrck_pin_(lrck), dout_pin_(dout),
      sck_pin_(sck), ws_pin_(ws), din_pin_(din) {
    
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    output_volume_ = 80;
    
    ESP_LOGI(TAG, "MAX98357A Codec created");
}

Max98357aCodec::~Max98357aCodec() {
    if (tx_handle_) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
    }
    if (rx_handle_) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
    }
}

void Max98357aCodec::CreateDuplexChannels() {
    // TX Channel (I2S_NUM_0)
    i2s_chan_config_t tx_chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, nullptr));
    
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_pin_,
            .ws = lrck_pin_,
            .dout = dout_pin_,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));
    
    // RX Channel (I2S_NUM_1)
    i2s_chan_config_t rx_chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle_));
    
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = sck_pin_,
            .ws = ws_pin_,
            .dout = I2S_GPIO_UNUSED,
            .din = din_pin_,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std_cfg));
    
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    
    ESP_LOGI(TAG, "I2S channels created");
}

void Max98357aCodec::SetOutputVolume(int volume) {
    volume = std::max(0, std::min(100, volume));
    AudioCodec::SetOutputVolume(volume);
}

void Max98357aCodec::EnableInput(bool enable) {
    AudioCodec::EnableInput(enable);
}

void Max98357aCodec::EnableOutput(bool enable) {
    AudioCodec::EnableOutput(enable);
    if (enable && tx_handle_ == nullptr) {
        CreateDuplexChannels();
    }
}

int Max98357aCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || rx_handle_ == nullptr) return 0;
    
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return 0;
    return bytes_read / sizeof(int16_t);
}

int Max98357aCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || tx_handle_ == nullptr) return samples;
    
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, data, samples * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return 0;
    return bytes_written / sizeof(int16_t);
}
