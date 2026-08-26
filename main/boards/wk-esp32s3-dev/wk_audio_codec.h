#ifndef _WK_AUDIO_CODEC_H_
#define _WK_AUDIO_CODEC_H_

#include "audio_codec.h"
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <mutex>

class WkAudioCodec : public AudioCodec {
private:
    gpio_num_t bclk_pin_;
    gpio_num_t lrck_pin_;
    gpio_num_t dout_pin_;
    gpio_num_t sck_pin_;
    gpio_num_t ws_pin_;
    gpio_num_t din_pin_;
    i2s_std_slot_t speaker_slot_;
    i2s_std_slot_t mic_slot_;
    
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    
    bool started_ = false;
    bool mute_ = false;
    std::mutex data_if_mutex_;
    int current_volume_ = 85;

    void CreateDuplexChannels();
    void UpdateDeviceState();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    WkAudioCodec(int input_sample_rate, int output_sample_rate,
                 int bclk_pin, int lrck_pin, int dout_pin,
                 i2s_std_slot_t speaker_slot,
                 int sck_pin, int ws_pin, int din_pin,
                 i2s_std_slot_t mic_slot);
    virtual ~WkAudioCodec();

    virtual bool Start() override;
    virtual void Stop() override;
    virtual void SetOutputVolume(int volume) override;
    virtual void SetOutputMute(bool mute) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _WK_AUDIO_CODEC_H_
