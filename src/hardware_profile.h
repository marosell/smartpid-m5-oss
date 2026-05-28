#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace proofpro_hw {

static constexpr gpio_num_t DC1_PIN = GPIO_NUM_12;
static constexpr gpio_num_t DC2_PIN = GPIO_NUM_13;
static constexpr gpio_num_t RL1_PIN = GPIO_NUM_26;
static constexpr gpio_num_t RL2_PIN = GPIO_NUM_16;

// M5Stack Basic speaker/mic pins. These are deliberately not initialized by
// M5.begin(); the SmartPID base uses safety-sensitive outputs and should not
// inherit generic M5Stack audio startup behavior.
static constexpr gpio_num_t SPEAKER_DAC_PIN = GPIO_NUM_25;
static constexpr gpio_num_t MIC_ADC_PIN = GPIO_NUM_34;

template <typename M5Config>
void applyM5Config(M5Config& cfg) {
#ifndef DESKTOP_BUILD
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.external_speaker_value = 0;
    cfg.internal_imu = false;
    cfg.external_imu = false;
    cfg.internal_rtc = false;
    cfg.external_rtc = false;
#endif
}

inline void configureSpeaker() {
#ifndef DESKTOP_BUILD
    auto spk = M5.Speaker.config();
    spk.pin_data_out = SPEAKER_DAC_PIN;
    spk.pin_bck = I2S_PIN_NO_CHANGE;
    spk.pin_ws = I2S_PIN_NO_CHANGE;
    spk.pin_mck = I2S_PIN_NO_CHANGE;
    spk.i2s_port = I2S_NUM_0;
    spk.use_dac = true;
    spk.buzzer = false;
    spk.magnification = 8;
    spk.sample_rate = 96000;
    M5.Speaker.config(spk);
#endif
}

inline void configureMic() {
#ifndef DESKTOP_BUILD
    auto mic = M5.Mic.config();
    mic.pin_data_in = MIC_ADC_PIN;
    mic.pin_bck = I2S_PIN_NO_CHANGE;
    mic.pin_ws = I2S_PIN_NO_CHANGE;
    mic.pin_mck = I2S_PIN_NO_CHANGE;
    mic.i2s_port = I2S_NUM_0;
    mic.use_adc = true;
    mic.over_sampling = 4;
    mic.sample_rate = 16000;
    M5.Mic.config(mic);
#endif
}

inline void holdSpeakerQuiet() {
#ifndef DESKTOP_BUILD
    pinMode((int)SPEAKER_DAC_PIN, OUTPUT);
    digitalWrite((int)SPEAKER_DAC_PIN, LOW);
#endif
}

}  // namespace proofpro_hw
