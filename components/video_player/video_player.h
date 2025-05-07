#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"

// Include necessary ESP-IDF libraries
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esphome {
namespace video_player {

enum class VideoSource {
  FILE,
  HTTP
};

class VideoPlayerComponent : public Component {
 public:
  VideoPlayerComponent() = default;
  ~VideoPlayerComponent();
  
  void setup() override;
  void loop() override;
  
  // Remove the 'override' keyword here since it's not overriding a parent method
  void dump_info();
  
  void set_display(display::Display *display) { this->display_ = display; }
  void set_video_path(const char *path) { this->video_path_ = path; this->source_ = VideoSource::FILE; }
  void set_http_url(const char *url) { this->http_url_ = url; this->source_ = VideoSource::HTTP; }
  void set_update_interval(uint32_t interval) { this->update_interval_ = interval; }
  void set_loop_video(bool loop) { this->loop_video_ = loop; }
  
 protected:
  bool open_file_source();
  bool open_http_source();
  bool read_next_frame();
  bool process_frame(const uint8_t* jpeg_data, size_t jpeg_size);
  void init_mutex();
  void cleanup();
  
  display::Display *display_{nullptr};
  const char *video_path_{nullptr};
  const char *http_url_{nullptr};
  FILE *video_file_{nullptr};
  
  uint32_t video_width_{0};
  uint32_t video_height_{0};
  uint32_t frame_count_{0};
  uint32_t video_fps_{0};
  uint32_t current_frame_{0};
  
  uint32_t update_interval_{0};  // Milliseconds
  uint32_t last_update_{0};
  
  VideoSource source_{VideoSource::FILE};
  bool loop_video_{true};
  bool spiffs_mounted_{false};
  
  // HTTP buffer
  uint8_t *http_buffer_{nullptr};
  size_t http_buffer_size_{0};
  size_t http_buffer_size_used_{0};
  size_t http_buffer_pos_{0};
  
  // Mutex for network operations
  SemaphoreHandle_t network_mutex_{nullptr};
};

}  // namespace video_player
}  // namespace esphome

