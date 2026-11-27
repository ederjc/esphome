#include "zephyr_dmic.h"

#ifdef USE_ZEPHYR
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome::zephyr_dmic {

static const char *const TAG = "zephyr_dmic";
static const struct device *owned_device = nullptr;

bool ZephyrDMICMicrophone::polling_budget_valid_() const {
  const uint64_t required_us = (static_cast<uint64_t>(App.get_loop_interval()) + POLL_OVERHEAD_MS) * 1000;
  const uint64_t usable_us = (BLOCK_COUNT / 2) * BLOCK_SIZE * 1000000ULL /
                            (this->audio_stream_info_.get_sample_rate() * this->audio_stream_info_.get_channels() * 2);
  return required_us <= usable_us;
}

void ZephyrDMICMicrophone::setup() {
  if (!device_is_ready(this->device_)) {
    ESP_LOGE(TAG, "DMIC device is not ready");
    this->mark_failed();
    return;
  }
  if (owned_device != nullptr || this->audio_stream_info_.get_sample_rate() != 15625 ||
      this->audio_stream_info_.get_channels() != 2 || !this->polling_budget_valid_()) {
    ESP_LOGE(TAG, "PDMv1 device already owned or capture/polling contract rejected");
    this->mark_failed();
    return;
  }
  k_mem_slab_init(&this->slab_, this->buffers_.data(), BLOCK_SIZE, BLOCK_COUNT);
  struct pcm_stream_cfg stream{};
  stream.pcm_rate = this->audio_stream_info_.get_sample_rate();
  stream.pcm_width = 16;
  stream.block_size = BLOCK_SIZE;
  stream.mem_slab = &this->slab_;
  struct dmic_cfg config{};
  config.io.min_pdm_clk_freq = 1000000;
  config.io.max_pdm_clk_freq = 3250000;
  config.io.min_pdm_clk_dc = 40;
  config.io.max_pdm_clk_dc = 60;
  config.streams = &stream;
  config.channel.req_num_chan = this->audio_stream_info_.get_channels();
  config.channel.req_num_streams = 1;
  config.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
  if (config.channel.req_num_chan == 2) {
    config.channel.req_chan_map_lo |= dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
  }
  int result = dmic_configure(this->device_, &config);
  if (result != 0 || config.channel.act_num_chan != config.channel.req_num_chan ||
      config.channel.act_num_streams != 1 || config.channel.act_chan_map_lo != config.channel.req_chan_map_lo ||
      config.channel.act_chan_map_hi != 0) {
    ESP_LOGE(TAG, "DMIC configuration/format rejected (%d)", result);
    if (result == 0)
      dmic_trigger(this->device_, DMIC_TRIGGER_RESET);
    this->mark_failed();
    return;
  }
  owned_device = this->device_;
  this->configured_ = true;
}

void ZephyrDMICMicrophone::start() {
  if (!this->configured_ || this->is_failed() || this->state_ != microphone::STATE_STOPPED)
    return;
  this->state_ = microphone::STATE_STARTING;
  const int result = dmic_trigger(this->device_, DMIC_TRIGGER_START);
  if (result != 0) {
    ESP_LOGE(TAG, "DMIC start failed (%d)", result);
    this->state_ = microphone::STATE_STOPPED;
    this->status_set_error();
    return;
  }
  this->status_clear_error();
  this->state_ = microphone::STATE_RUNNING;
}

void ZephyrDMICMicrophone::stop() {
  if (!this->configured_ || this->state_ == microphone::STATE_STOPPED)
    return;
  this->state_ = microphone::STATE_STOPPING;
  const int result = dmic_trigger(this->device_, DMIC_TRIGGER_STOP);
  if (result != 0) {
    ESP_LOGE(TAG, "DMIC stop failed (%d)", result);
    this->status_set_error();
    this->mark_failed();
  }
  this->state_ = microphone::STATE_STOPPED;
}

void ZephyrDMICMicrophone::loop() {
  if (!this->is_running())
    return;
  if (!this->polling_budget_valid_()) {
    ESP_LOGE(TAG, "Polling interval no longer fits the PDMv1 slab/headroom budget");
    this->stop();
    this->status_set_error();
    return;
  }
  for (size_t i = 0; i < BLOCK_COUNT; i++) {
    void *buffer = nullptr;
    size_t size = 0;
    const int result = dmic_read(this->device_, 0, &buffer, &size, 0);
    if (result == -EAGAIN || result == -ENOMSG)
      break;
    if (result != 0) {
      ESP_LOGE(TAG, "DMIC capture failed (%d)", result);
      this->stop();
      this->status_set_error();
      break;
    }
    if (buffer == nullptr || size != BLOCK_SIZE || size % this->audio_stream_info_.frames_to_bytes(1) != 0) {
      if (buffer != nullptr)
        k_mem_slab_free(&this->slab_, buffer);
      ESP_LOGE(TAG, "Invalid DMIC PCM block");
      this->stop();
      this->status_set_error();
      break;
    }
    // DMIC has no hardware capture timestamp; zero is explicitly unknown.
    this->publish_audio_({static_cast<uint8_t *>(buffer), size}, this->audio_stream_info_, 0);
    k_mem_slab_free(&this->slab_, buffer);
    if (!this->is_running())
      break;
  }
}

void ZephyrDMICMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "Zephyr DMIC: %s, %u Hz, %u channels, signed 16-bit PCM", this->device_->name,
                static_cast<unsigned>(this->audio_stream_info_.get_sample_rate()),
                this->audio_stream_info_.get_channels());
}

}  // namespace esphome::zephyr_dmic
#endif
