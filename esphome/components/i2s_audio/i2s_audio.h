#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#if ESP_IDF_VERSION_MAJOR < 5
#include <driver/i2s.h>
#else
#include <driver/i2s_std.h>
#endif

namespace esphome {
namespace i2s_audio {

class I2SAudioComponent;

class I2SAudioBase : public Parented<I2SAudioComponent> {
 public:
#if ESP_IDF_VERSION_MAJOR < 5
  void set_i2s_mode(i2s_mode_t mode) { this->i2s_mode_ = mode; }
  void set_channel(i2s_channel_fmt_t channel) { this->channel_ = channel; }
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_bits_per_sample(i2s_bits_per_sample_t bits_per_sample) { this->bits_per_sample_ = bits_per_sample; }
  void set_bits_per_channel(i2s_bits_per_chan_t bits_per_channel) { this->bits_per_channel_ = bits_per_channel; }
  void set_use_apll(uint32_t use_apll) { this->use_apll_ = use_apll; }
#endif

 protected:
#if ESP_IDF_VERSION_MAJOR < 5
  i2s_mode_t i2s_mode_{};
  i2s_channel_fmt_t channel_;
  uint32_t sample_rate_;
  i2s_bits_per_sample_t bits_per_sample_;
  i2s_bits_per_chan_t bits_per_channel_;
  bool use_apll_;
#endif
};

class I2SAudioIn : public I2SAudioBase {};

class I2SAudioOut : public I2SAudioBase {};

class I2SAudioComponent : public Component {
 public:
  void setup() override;

#if ESP_IDF_VERSION_MAJOR < 5
  i2s_pin_config_t get_pin_config() const {
    return {
        .mck_io_num = this->mclk_pin_,
        .bck_io_num = this->bclk_pin_,
        .ws_io_num = this->lrclk_pin_,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
  }
#else
  i2s_std_gpio_config_t get_pin_config() const {
    return {
        .mclk = (gpio_num_t)this->mclk_pin_,
        .bclk = (gpio_num_t)this->bclk_pin_,
        .ws = (gpio_num_t)this->lrclk_pin_,
        .dout = I2S_GPIO_UNUSED, // add local ports
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {
          .mclk_inv = false,
          .bclk_inv = false,
          .ws_inv = false,
        }
    };
  }
#endif 

  void set_mclk_pin(int pin) { this->mclk_pin_ = pin; }
  void set_bclk_pin(int pin) { this->bclk_pin_ = pin; }
  void set_lrclk_pin(int pin) { this->lrclk_pin_ = pin; }

  void lock() { this->lock_.lock(); }
  bool try_lock() { return this->lock_.try_lock(); }
  void unlock() { this->lock_.unlock(); }

  i2s_port_t get_port() const { return this->port_; }

 protected:
  Mutex lock_;

  I2SAudioIn *audio_in_{nullptr};
  I2SAudioOut *audio_out_{nullptr};
#if ESP_IDF_VERSION_MAJOR < 5
  int mclk_pin_{I2S_PIN_NO_CHANGE};
  int bclk_pin_{I2S_PIN_NO_CHANGE};
#else
  int mclk_pin_{I2S_GPIO_UNUSED};
  int bclk_pin_{I2S_GPIO_UNUSED};
#endif
  int lrclk_pin_;
  i2s_port_t port_{};
};

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32
