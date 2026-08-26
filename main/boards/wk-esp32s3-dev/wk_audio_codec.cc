#include "wk_audio_codec.h"
#include <esp_log.h>
#include <driver/i2s_std.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "WkAudioCodec"

WkAudioCodec::WkAudioCodec(int input_sample_rate, int output_sample_rate,
                           int bclk_pin, int lrck_pin, int dout_pin,
                           i2s_std_slot_t speaker_slot,
                           int sck_pin, int ws_pin, int din_pin,
                           i2s_std_slot_t mic_slot)
    : bclk_pin_((gpio_num_t)bclk_pin),
      lrck_pin_((gpio_num_t)lrck_pin),
      dout_pin_((gpio_num_t)dout_pin),
      sck_pin_((gpio_num_t)sck_pin),
      ws_pin_((gpio_num_t)ws_pin),
      din_pin_((gpio_num_t)din_pin),
      speaker_slot_(speaker_slot),
      mic_slot_(mic_slot) {
    
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    output_volume_ = 85;
    input_gain_ = 30;
    
    ESP_LOGI(TAG, "WK Audio Codec created");
    ESP_LOGI(TAG, "SPK: BCLK=%d, LRCK=%d, DOUT=%d, Slot=%s", 
             bclk_pin, lrck_pin, dout_pin,
             speaker_slot == I2S_STD_SLOT_LEFT ? "LEFT" : 
             speaker_slot == I2S_STD_SLOT_RIGHT ? "RIGHT" : "BOTH");
    ESP_LOGI(TAG, "MIC: SCK=%d, WS=%d, DIN=%d, Slot=%s",
             sck_pin, ws_pin, din_pin,
             mic_slot == I2S_STD_SLOT_LEFT ? "LEFT" : 
             mic_slot == I2S_STD_SLOT_RIGHT ? "RIGHT" : "BOTH");
    ESP_LOGI(TAG, "Input SR: %d, Output SR: %d", input_sample_rate, output_sample_rate);
}

WkAudioCodec::~WkAudioCodec() {
    Stop();
}

void WkAudioCodec::CreateDuplexChannels() {
    ESP_LOGI(TAG, "Creating I2S duplex channels");
    
    // ===== TX Channel (MAX98357A Speaker) =====
    i2s_chan_config_t tx_chan_config = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_config, &tx_handle_, nullptr));
    
    i2s_std_config_t tx_config = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (speaker_slot_ == I2S_STD_SLOT_RIGHT) ? I2S_STD_SLOT_RIGHT : 
                         (speaker_slot_ == I2S_STD_SLOT_LEFT) ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH,
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_config));
    ESP_LOGI(TAG, "TX channel initialized for speaker");
    
    // ===== RX Channel (INMP441 Microphone) =====
    i2s_chan_config_t rx_chan_config = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_config, nullptr, &rx_handle_));
    
    i2s_std_config_t rx_config = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (mic_slot_ == I2S_STD_SLOT_LEFT) ? I2S_STD_SLOT_LEFT : 
                         (mic_slot_ == I2S_STD_SLOT_RIGHT) ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_config));
    ESP_LOGI(TAG, "RX channel initialized for microphone");
    
    ESP_LOGI(TAG, "I2S duplex channels created successfully");
}

bool WkAudioCodec::Start() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    
    if (started_) {
        ESP_LOGW(TAG, "Codec already started");
        return true;
    }
    
    ESP_LOGI(TAG, "Starting WK Audio Codec...");
    
    // Create I2S channels
    CreateDuplexChannels();
    
    // Enable channels
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    
    started_ = true;
    input_enabled_ = true;
    output_enabled_ = true;
    
    ESP_LOGI(TAG, "WK Audio Codec started successfully");
    ESP_LOGI(TAG, "Volume: %d%%, Mute: %s", output_volume_, mute_ ? "ON" : "OFF");
    return true;
}

void WkAudioCodec::Stop() {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    
    if (!started_) return;
    
    ESP_LOGI(TAG, "Stopping WK Audio Codec...");
    
    if (tx_handle_) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    
    if (rx_handle_) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
    
    started_ = false;
    input_enabled_ = false;
    output_enabled_ = false;
    ESP_LOGI(TAG, "WK Audio Codec stopped");
}

void WkAudioCodec::UpdateDeviceState() {
    // Nothing special needed for MAX98357A + INMP441
}

int WkAudioCodec::Read(int16_t* dest, int samples) {
    if (!started_ || rx_handle_ == nullptr || !input_enabled_) {
        return 0;
    }
    
    size_t bytes_read = 0;
    size_t bytes_to_read = samples * sizeof(int16_t);
    
    esp_err_t ret = i2s_channel_read(rx_handle_, dest, bytes_to_read, &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S read error: %d", ret);
        return 0;
    }
    
    int samples_read = bytes_read / sizeof(int16_t);
    
    // Apply input gain if needed
    if (input_gain_ != 1.0f) {
        float gain = input_gain_ / 30.0f; // Scale 0-30 to 0-1
        for (int i = 0; i < samples_read; i++) {
            int32_t val = (int32_t)dest[i] * gain;
            dest[i] = (int16_t)std::max(-32768, std::min(32767, val));
        }
    }
    
    return samples_read;
}

int WkAudioCodec::Write(const int16_t* data, int samples) {
    if (!started_ || tx_handle_ == nullptr || !output_enabled_) {
        return 0;
    }
    if (mute_) {
        // Return samples count but don't write (muted)
        return samples;
    }
    
    // Apply volume scaling
    float gain = output_volume_ / 100.0f;
    
    // Allocate buffer for scaled data
    std::vector<int16_t> scaled_data(samples);
    for (int i = 0; i < samples; i++) {
        int32_t val = (int32_t)data[i] * gain;
        scaled_data[i] = (int16_t)std::max(-32768, std::min(32767, val));
    }
    
    size_t bytes_written = 0;
    size_t bytes_to_write = samples * sizeof(int16_t);
    
    esp_err_t ret = i2s_channel_write(tx_handle_, scaled_data.data(), bytes_to_write, &bytes_written, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write error: %d", ret);
        return 0;
    }
    
    return bytes_written / sizeof(int16_t);
}

void WkAudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    current_volume_ = std::max(0, std::min(100, volume));
    output_volume_ = current_volume_;
    ESP_LOGI(TAG, "Set output volume to %d%%", output_volume_);
    
    // Save to settings
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void WkAudioCodec::SetOutputMute(bool mute) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    mute_ = mute;
    ESP_LOGI(TAG, "Set output mute to %s", mute ? "ON" : "OFF");
}

void WkAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    ESP_LOGI(TAG, "Input %s", enable ? "enabled" : "disabled");
}

void WkAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    ESP_LOGI(TAG, "Output %s", enable ? "enabled" : "disabled");
}
