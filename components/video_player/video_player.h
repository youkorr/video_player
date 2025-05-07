#pragma once

#include "esphome.h"
#include "esphome/components/display/display_buffer.h"

namespace esphome {
namespace video_player {

class VideoPlayerComponent : public Component, public Parented<display::DisplayBuffer> {
 public:
  void set_stream_url(const char *url) { url_ = url; }
  void set_update_interval(uint32_t interval) { update_interval_ = interval; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool process_mjpeg_chunk(const uint8_t *data, size_t len);
  bool decode_jpeg(const uint8_t *src, size_t len);
  
  const char *url_{nullptr};
  uint32_t update_interval_{50};
  uint32_t last_update_{0};
  bool initialized_{false};
};
  
}  // namespace video_player
}  // namespace esphome

