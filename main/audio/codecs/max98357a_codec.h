#ifndef MAX98357A_CODEC_H
#define MAX98357A_CODEC_H

#include "audio_codec.h"
#include <esp_log.h>
#include <driver/i2s_std.h>
#include <cstring>

#define TAG "Max98357aCodec"

class MAX98357AAudioCodec : public AudioCodec {
private:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    int input_sample_rate_;
    int output_sample_rate_;
    gpio_num_t bclk_pin_;
    gpio_num_t lrck_pin_;
    gpio_num_t dout_pin_;
    gpio_num_t mic_sck_pin_;
    gpio_num_t mic_ws_pin_;
    gpio_num_t mic_din_pin_;
    bool has_mic_ = false;

    void CreateSimplexChannels() {
        // TX channel for MAX98357A
        i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        tx_chan_cfg.dma_desc_num = 6;
        tx_chan_cfg.dma_frame_num = 240;
        ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, nullptr));

        i2s_std_config_t tx_std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,  // MAX98357A doesn't need MCLK
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
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));

        // RX channel for INMP441 (if available)
        if (has_mic_) {
            i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
            rx_chan_cfg.dma_desc_num = 6;
            rx_chan_cfg.dma_frame_num = 240;
            ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle_));

            i2s_std_config_t rx_std_cfg = {
                .clk_cfg = {
                    .sample_rate_hz = (uint32_t)input_sample_rate_,
                    .clk_src = I2S_CLK_SRC_DEFAULT,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                },
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = mic_sck_pin_,
                    .ws = mic_ws_pin_,
                    .dout = I2S_GPIO_UNUSED,
                    .din = mic_din_pin_,
                    .invert_flags = {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
                },
            };
            
            ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std_cfg));
            ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
        }
    }

public:
    MAX98357AAudioCodec(int input_sample_rate, int output_sample_rate,
                        gpio_num_t bclk, gpio_num_t lrck, gpio_num_t dout,
                        gpio_num_t mic_sck = GPIO_NUM_NC,
                        gpio_num_t mic_ws = GPIO_NUM_NC,
                        gpio_num_t mic_din = GPIO_NUM_NC)
        : input_sample_rate_(input_sample_rate),
          output_sample_rate_(output_sample_rate),
          bclk_pin_(bclk),
          lrck_pin_(lrck),
          dout_pin_(dout),
          mic_sck_pin_(mic_sck),
          mic_ws_pin_(mic_ws),
          mic_din_pin_(mic_din) {
        
        // Check if microphone is connected
        has_mic_ = (mic_sck != GPIO_NUM_NC && mic_ws != GPIO_NUM_NC && mic_din != GPIO_NUM_NC);
        
        if (has_mic_) {
            duplex_ = true;
            input_channels_ = 1;
        } else {
            duplex_ = false;
            input_channels_ = 0;
        }
        
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
        
        CreateSimplexChannels();
        
        ESP_LOGI(TAG, "MAX98357A Audio Codec initialized");
        if (has_mic_) {
            ESP_LOGI(TAG, "With INMP441 Microphone");
        }
    }

    ~MAX98357AAudioCodec() {
        if (tx_handle_) {
            i2s_channel_disable(tx_handle_);
            i2s_del_channel(tx_handle_);
        }
        if (rx_handle_) {
            i2s_channel_disable(rx_handle_);
            i2s_del_channel(rx_handle_);
        }
    }

    void EnableInput(bool enable) override {
        if (has_mic_) {
            AudioCodec::EnableInput(enable);
        }
    }

    void EnableOutput(bool enable) override {
        AudioCodec::EnableOutput(enable);
    }

    int Read(int16_t* dest, int samples) override {
        if (!has_mic_ || !input_enabled_) {
            return 0;
        }
        
        size_t bytes_read = 0;
        size_t bytes_to_read = samples * sizeof(int16_t);
        
        esp_err_t err = i2s_channel_read(rx_handle_, dest, bytes_to_read, &bytes_read, portMAX_DELAY);
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Read error: %s", esp_err_to_name(err));
            return 0;
        }
        
        return bytes_read / sizeof(int16_t);
    }

    int Write(const int16_t* data, int samples) override {
        if (!output_enabled_) {
            return samples;
        }
        
        size_t bytes_written = 0;
        size_t bytes_to_write = samples * sizeof(int16_t);
        
        esp_err_t err = i2s_channel_write(tx_handle_, data, bytes_to_write, &bytes_written, portMAX_DELAY);
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Write error: %s", esp_err_to_name(err));
            return 0;
        }
        
        return bytes_written / sizeof(int16_t);
    }
};

#endif // MAX98357A_CODEC_H
