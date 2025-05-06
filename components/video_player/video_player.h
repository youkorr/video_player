#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/api/api_server.h"
#include <stdio.h>

namespace esphome {
namespace video_player {

enum class VideoSource {
  FILE,
  HTTP
};

class VideoPlayerComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_info() override;
  
  void set_display(display::DisplayBuffer *display) { display_ = display; }
  void set_video_path(const char* path) { video_path_ = path; }
  void set_update_interval(uint32_t update_interval) { update_interval_ = update_interval; }
  void set_http_url(const char* url) { 
    http_url_ = url; 
    source_ = VideoSource::HTTP;
  }

 protected:
  bool open_file_source();
  bool open_http_source();
  bool read_next_frame();
  
  display::DisplayBuffer *display_{nullptr};
  const char* video_path_{"/spiffs/video.mjpg"};
  const char* http_url_{nullptr};
  VideoSource source_{VideoSource::FILE};
  
  FILE* video_file_{nullptr};
  uint32_t video_width_{0};
  uint32_t video_height_{0};
  uint32_t frame_count_{0};
  uint32_t video_fps_{30};
  uint32_t current_frame_{0};
  
  uint32_t update_interval_{33};  // 33ms = ~30fps par défaut
  uint32_t last_update_{0};
  
  // Buffer pour les données HTTP
  uint8_t* http_buffer_{nullptr};
  size_t http_buffer_size_{0};
  size_t http_buffer_pos_{0};
};

}  // namespace video_player
}  // namespace esphome
