#pragma once

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esp_err.h"

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
  
  void dump_info();
  
  void set_display(display::Display *display) { this->display_ = display; }
  void set_file_path(const char *path) { 
    this->video_path_ = path;
    this->source_ = VideoSource::FILE;
  }
  void set_http_url(const char *url) { 
    this->http_url_ = url;
    this->source_ = VideoSource::HTTP;
  }
  void set_loop(bool loop) { this->loop_video_ = loop; }
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ = interval_ms; }
  
  // Destructeur pour nettoyer les ressources
  ~VideoPlayerComponent();

 protected:
  // Méthodes internes
  void init_mutex();
  bool open_file_source();
  bool open_http_source();
  bool read_next_frame();
  bool process_frame(const uint8_t* jpeg_data, size_t jpeg_size);
  void cleanup();
  
  // Composants externes
  display::Display *display_{nullptr};
  
  // Propriétés de la source vidéo
  const char *video_path_{nullptr};
  const char *http_url_{nullptr};
  VideoSource source_{VideoSource::FILE};
  bool loop_video_{true};
  
  // Propriétés de la vidéo
  uint32_t video_width_{0};
  uint32_t video_height_{0};
  uint32_t frame_count_{0};
  uint32_t video_fps_{30};
  uint32_t current_frame_{0};
  
  // Timing
  uint32_t update_interval_{0};
  uint32_t last_update_{0};
  
  // Source FILE
  FILE *video_file_{nullptr};
  bool spiffs_mounted_{false};
  
  // Source HTTP
  uint8_t *http_buffer_{nullptr};
  size_t http_buffer_size_{0};
  size_t http_buffer_size_used_{0};
  size_t http_buffer_pos_{0};
  
  // Mutex pour la synchronisation
  SemaphoreHandle_t network_mutex_{nullptr};
  
  // Nouvelles variables ajoutées pour la gestion d'initialisation HTTP différée
  bool http_initialized_{false};
  uint32_t last_http_init_attempt_{0};
};

}  // namespace video_player
}  // namespace esphome


