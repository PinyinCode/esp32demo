#ifndef MAX98357A_CODEC_H
#define MAX98357A_CODEC_H

#include "audio_codec.h"
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <vector>
#include <mutex>
#include <algorithm>

#define TAG "Max98357aCodec"

class Max98357aCodec : public AudioCodec {
private:
    // GPIO pins
    gpio_num_t bclk_pin_;
    gpio_num_t lrck_pin_;
    gpio_num_t dout_pin_;
    gpio_num_t sck_pin_;
    gpio_num_t ws_pin_;
    gpio_num_t din_pin_;
    
    // I2S handles
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    
    // State
    bool started_ = false;
    bool mute_ = false;
    std::mutex data_if_mutex_;
    std::vector<int16_t> scaled_buffer_;  // Buffer for volume scaling

    void CreateDuplexChannels();

public:
    Max98357aCodec(int input_sample_rate, int output_sample_rate,
                   gpio_num_t bclk, gpio_num_t lrck, gpio_num_t dout,
                   gpio_num_t sck, gpio_num_t ws, gpio_num_t din);
    
    ~Max98357aCodec();

    // AudioCodec interface
    bool Start() override;
    void Stop() override;
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
    void SetOutputVolume(int volume) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
};

#endif // MAX98357A_CODEC_H
