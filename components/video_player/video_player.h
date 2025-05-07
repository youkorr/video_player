#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"

// Include FreeRTOS header for mutex
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
  VideoPlayerComponent() : Component() {}
  ~VideoPlayerComponent();
  
  void setup() override;
  void loop() override;
  void dump_info() override;
  
  void set_display(display::Display *display) { display_ = display; }
  void set_update_interval(uint32_t update_interval) { update_interval_ = update_interval; }
  
  // Configuration pour source fichier
  void set_video_path(const char* path) { 
    video_path_ = path; 
    source_ = VideoSource::FILE;
  }
  
  // Configuration pour source HTTP
  void set_http_url(const char* url) { 
    http_url_ = url; 
    source_ = VideoSource::HTTP;
  }
  
  // Option pour boucler la vidéo
  void set_loop(bool loop) { loop_video_ = loop; }
  
 protected:
  bool open_file_source();
  bool open_http_source();
  bool read_next_frame();
  bool process_frame(const uint8_t* jpeg_data, size_t jpeg_size);
  void init_mutex();
  void cleanup();
  
  // Propriétés de l'affichage
  display::Display *display_{nullptr};
  
  // Source vidéo
  VideoSource source_{VideoSource::FILE};
  const char* video_path_{nullptr};
  const char* http_url_{nullptr};
  
  // Gestion du fichier
  FILE* video_file_{nullptr};
  bool spiffs_mounted_{false};
  
  // Buffer HTTP pour la lecture en streaming
  uint8_t* http_buffer_{nullptr};
  size_t http_buffer_size_{0};
  size_t http_buffer_pos_{0};
  size_t http_buffer_size_used_{0};  // Taille utilisée dans le buffer
  
  // Synchronisation
  SemaphoreHandle_t network_mutex_{nullptr};
  
  // Propriétés de la vidéo
  uint32_t video_width_{0};
  uint32_t video_height_{0};
  uint32_t frame_count_{0};
  uint32_t video_fps_{0};
  uint32_t current_frame_{0};
  
  // Gestion du timing
  uint32_t update_interval_{0};
  uint32_t last_update_{0};
  
  // Options
  bool loop_video_{true};
};

}  // namespace video_player
}  // namespace esphome

