#ifndef MAX98357A_CODEC_H
#define MAX98357A_CODEC_H

#include "audio_codec.h"
#include <driver/i2s_std.h>
#include <vector>
#include <mutex>

class Max98357aCodec : public AudioCodec {
private:
    gpio_num_t bclk_pin_;
    gpio_num_t lrck_pin_;
    gpio_num_t dout_pin_;
    gpio_num_t sck_pin_;
    gpio_num_t ws_pin_;
    gpio_num_t din_pin_;
    
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    
    bool started_ = false;
    std::mutex data_if_mutex_;
    std::vector<int16_t> scaled_buffer_;

    void CreateDuplexChannels();

public:
    Max98357aCodec(int input_sample_rate, int output_sample_rate,
                   gpio_num_t bclk, gpio_num_t lrck, gpio_num_t dout,
                   gpio_num_t sck, gpio_num_t ws, gpio_num_t din);
    virtual ~Max98357aCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

private:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};

#endif // MAX98357A_CODEC_H
