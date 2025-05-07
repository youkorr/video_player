#pragma once

#include "esphome.h"
#include "esphome/components/display/display_buffer.h"

namespace esphome {
namespace video_player {

enum class VideoSource { FILE, HTTP };

class VideoPlayerComponent : public Component {
 public:
  void set_display(display::DisplayBuffer *display) { display_ = display; }
  void set_http_url(const char *url) { 
    http_url_ = url; 
    source_ = VideoSource::HTTP;
  }
  void set_file_path(const char *path) { 
    video_path_ = path; 
    source_ = VideoSource::FILE;
  }
  void set_update_interval(uint32_t interval) { update_interval_ = interval; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  bool process_raw_mjpeg(const uint8_t* data, size_t len);
  bool process_frame(const uint8_t* jpeg_data, size_t jpeg_size);
  static esp_err_t http_event_handler(esp_http_client_event_t *evt);

  display::DisplayBuffer *display_{nullptr};
  const char *http_url_{nullptr};
  const char *video_path_{nullptr};
  VideoSource source_{VideoSource::HTTP};
  uint32_t update_interval_{0};
  uint32_t last_update_{0};
  bool http_initialized_{false};
  uint32_t last_http_init_attempt_{0};
};

}  // namespace video_player
}  // namespace esphome

