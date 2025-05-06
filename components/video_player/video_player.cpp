#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "video_player.h"

// Inclusions pour ESP-IDF 5.1.5
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

// Pour la lecture du fichier MJPEG
#include <stdio.h>
#include <string.h>

// Ajout de l'inclusion pour jpg2rgb565
#include "esp_jpg_decode.h"

// Déclaration externe de jpg2rgb565 si nécessaire
extern "C" {
  bool jpg2rgb565(const uint8_t *src, size_t src_len, uint8_t *out, jpg_scale_t scale);
  enum jpg_scale_t {
    JPG_SCALE_NONE,
    JPG_SCALE_2X,
    JPG_SCALE_4X,
    JPG_SCALE_8X,
  };
}

namespace esphome {
namespace video_player {

static const char *TAG = "video_player";

// Structure qui représente l'en-tête MJPEG
typedef struct {
  uint32_t signature;    // Devrait être "MJPG"
  uint32_t width;        // Largeur de la vidéo
  uint32_t height;       // Hauteur de la vidéo
  uint32_t frame_count;  // Nombre total de frames
  uint32_t fps;          // Frames par seconde
} mjpeg_header_t;

// Structure qui représente l'en-tête d'un frame MJPEG
typedef struct {
  uint32_t size;         // Taille des données JPEG en octets
  uint32_t timestamp;    // Timestamp du frame en millisecondes
} mjpeg_frame_header_t;

// Callback pour la lecture HTTP
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  VideoPlayerComponent* player = static_cast<VideoPlayerComponent*>(evt->user_data);
  
  switch(evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      // Ici nous recevons des données, mais nous devons les gérer manuellement
      // dans notre composant car les données peuvent arriver fragmentées
      ESP_LOGV(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      break;
    default:
      break;
  }
  return ESP_OK;
}

void VideoPlayerComponent::setup() {
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "Display not set!");
    this->mark_failed();
    return;
  }
  
  if (this->source_ == VideoSource::FILE) {
    if (!this->open_file_source()) {
      this->mark_failed();
      return;
    }
  } else if (this->source_ == VideoSource::HTTP) {
    if (!this->open_http_source()) {
      this->mark_failed();
      return;
    }
  }
  
  ESP_LOGI(TAG, "Video loaded: %dx%d, %d frames, %d FPS", 
           this->video_width_, this->video_height_, this->frame_count_, this->video_fps_);
  ESP_LOGI(TAG, "Display dimensions: %dx%d", display_->get_width(), display_->get_height());
}

bool VideoPlayerComponent::open_file_source() {
  // Initialiser le système de fichiers
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = false
  };
  
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
    return false;
  }
  
  // Ouvrir le fichier vidéo
  FILE* video_file = fopen(this->video_path_, "rb");
  if (!video_file) {
    ESP_LOGE(TAG, "Failed to open video file: %s", this->video_path_);
    return false;
  }
  
  // Lire l'en-tête MJPEG
  mjpeg_header_t header;
  size_t read_size = fread(&header, 1, sizeof(header), video_file);
  if (read_size != sizeof(header)) {
    ESP_LOGE(TAG, "Failed to read MJPEG header");
    fclose(video_file);
    return false;
  }
  
  // Vérifier la signature
  if (header.signature != 0x47504A4D) {  // "MJPG" en little-endian
    ESP_LOGE(TAG, "Invalid MJPEG signature");
    fclose(video_file);
    return false;
  }
  
  this->video_width_ = header.width;
  this->video_height_ = header.height;
  this->frame_count_ = header.frame_count;
  this->video_fps_ = header.fps;
  this->video_file_ = video_file;
  
  // Calculer l'intervalle entre les frames basé sur le FPS
  if (this->update_interval_ == 0) {
    this->update_interval_ = 1000 / this->video_fps_;
  }
  
  return true;
}

bool VideoPlayerComponent::open_http_source() {
  if (this->http_url_ == nullptr) {
    ESP_LOGE(TAG, "HTTP URL not set!");
    return false;
  }
  
  ESP_LOGI(TAG, "Connecting to HTTP source: %s", this->http_url_);

  // Configuration du client HTTP - corrigé l'ordre des champs
  esp_http_client_config_t config = {};
  config.url = this->http_url_;
  config.event_handler = http_event_handler;
  config.user_data = this;
  config.timeout_ms = 10000;
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return false;
  }
  
  // Télécharger l'en-tête MJPEG
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  
  int content_length = esp_http_client_fetch_headers(client);
  if (content_length < 0) {
    ESP_LOGE(TAG, "HTTP client fetch headers failed");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  
  // Allouer de la mémoire pour le buffer HTTP
  this->http_buffer_size_ = 262144;  // 256KB buffer
  this->http_buffer_ = (uint8_t*)heap_caps_malloc(this->http_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->http_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  
  // Lire l'en-tête MJPEG
  int read_len = esp_http_client_read(client, (char*)this->http_buffer_, sizeof(mjpeg_header_t));
  if (read_len != sizeof(mjpeg_header_t)) {
    ESP_LOGE(TAG, "Failed to read MJPEG header from HTTP");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(this->http_buffer_);
    this->http_buffer_ = nullptr;
    return false;
  }
  
  // Analyser l'en-tête
  mjpeg_header_t* header = (mjpeg_header_t*)this->http_buffer_;
  if (header->signature != 0x47504A4D) {
    ESP_LOGE(TAG, "Invalid MJPEG signature from HTTP");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(this->http_buffer_);
    this->http_buffer_ = nullptr;
    return false;
  }
  
  this->video_width_ = header->width;
  this->video_height_ = header->height;
  this->frame_count_ = header->frame_count;
  this->video_fps_ = header->fps;
  
  // Calculer l'intervalle entre les frames basé sur le FPS
  if (this->update_interval_ == 0) {
    this->update_interval_ = 1000 / this->video_fps_;
  }
  
  // Lire le reste des données disponibles
  size_t buffer_pos = sizeof(mjpeg_header_t);
  int remaining_len = esp_http_client_read(client, (char*)(this->http_buffer_ + buffer_pos), 
                                          this->http_buffer_size_ - buffer_pos);
  if (remaining_len > 0) {
    buffer_pos += remaining_len;
  }
  
  this->http_buffer_pos_ = 0;  // Position de lecture dans le buffer
  
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  
  ESP_LOGI(TAG, "HTTP video source initialized successfully");
  return true;
}

bool VideoPlayerComponent::read_next_frame() {
  if (this->source_ == VideoSource::FILE) {
    if (!this->video_file_) {
      return false;
    }
    
    // Lire l'en-tête du frame
    mjpeg_frame_header_t frame_header;
    size_t read_size = fread(&frame_header, 1, sizeof(frame_header), this->video_file_);
    if (read_size != sizeof(frame_header)) {
      // Si on arrive à la fin du fichier, on boucle
      if (feof(this->video_file_)) {
        ESP_LOGI(TAG, "End of video, restarting");
        fseek(this->video_file_, sizeof(mjpeg_header_t), SEEK_SET);
        return false;
      }
      ESP_LOGE(TAG, "Failed to read frame header");
      return false;
    }
    
    // Allouer un buffer pour les données JPEG
    uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(frame_header.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_data) {
      ESP_LOGE(TAG, "Failed to allocate memory for JPEG data");
      return false;
    }
    
    // Lire les données JPEG
    read_size = fread(jpeg_data, 1, frame_header.size, this->video_file_);
    if (read_size != frame_header.size) {
      ESP_LOGE(TAG, "Failed to read JPEG data");
      free(jpeg_data);
      return false;
    }
    
    ESP_LOGD(TAG, "Read frame: %d bytes", frame_header.size);
    
    // Clear display
    display_->fill(Color::BLACK);
    
    // Allouer un buffer pour l'image RGB565
    size_t rgb_buf_size = this->video_width_ * this->video_height_ * 2; // 2 bytes par pixel pour RGB565
    uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(rgb_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (rgb_buf) {
      // Convertir JPEG en RGB565 (sans mise à l'échelle)
      if (jpg2rgb565(jpeg_data, frame_header.size, rgb_buf, JPG_SCALE_NONE)) {
        // Calculer les facteurs de mise à l'échelle
        float scale_x = (float)this->video_width_ / display_->get_width();
        float scale_y = (float)this->video_height_ / display_->get_height();
        
        // Dessiner sur l'écran
        for (int y = 0; y < (int)this->video_height_; y++) {
          int display_y = (int)(y / scale_y);
          if (display_y >= display_->get_height()) continue;
          
          for (int x = 0; x < (int)this->video_width_; x++) {
            int display_x = (int)(x / scale_x);
            if (display_x >= display_->get_width()) continue;
            
            // Pour RGB565, chaque pixel fait 2 octets
            int idx = (y * this->video_width_ + x) * 2;
            uint16_t pixel = (rgb_buf[idx + 1] << 8) | rgb_buf[idx];
            
            // Convertir RGB565 en RGB888
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            
            display_->draw_pixel_at(display_x, display_y, Color(r, g, b));
          }
        }
        ESP_LOGD(TAG, "Frame converted and drawn");
      } else {
        ESP_LOGE(TAG, "JPEG conversion failed");
      }
      free(rgb_buf);
    } else {
      ESP_LOGE(TAG, "Failed to allocate RGB buffer (requested %d bytes)", rgb_buf_size);
    }
    
    free(jpeg_data);
    return true;
  }
  else if (this->source_ == VideoSource::HTTP) {
    // Lire depuis le buffer HTTP
    // Ceci est une implémentation simplifiée - dans la réalité,
    // vous devriez gérer le rechargement du buffer HTTP quand il est épuisé
    
    if (this->http_buffer_ == nullptr || 
        this->http_buffer_pos_ + sizeof(mjpeg_frame_header_t) >= this->http_buffer_size_) {
      ESP_LOGE(TAG, "HTTP buffer empty or insufficient data");
      return false;
    }
    
    // Lire l'en-tête du frame
    mjpeg_frame_header_t* frame_header = (mjpeg_frame_header_t*)(this->http_buffer_ + this->http_buffer_pos_);
    this->http_buffer_pos_ += sizeof(mjpeg_frame_header_t);
    
    if (this->http_buffer_pos_ + frame_header->size > this->http_buffer_size_) {
      ESP_LOGE(TAG, "Insufficient data in HTTP buffer for frame");
      return false;
    }
    
    // Pointer vers les données JPEG dans le buffer
    uint8_t* jpeg_data = this->http_buffer_ + this->http_buffer_pos_;
    this->http_buffer_pos_ += frame_header->size;
    
    ESP_LOGD(TAG, "Read HTTP frame: %d bytes", frame_header->size);
    
    // Clear display
    display_->fill(Color::BLACK);
    
    // Allouer un buffer pour l'image RGB565
    size_t rgb_buf_size = this->video_width_ * this->video_height_ * 2; // 2 bytes par pixel pour RGB565
    uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(rgb_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (rgb_buf) {
      // Convertir JPEG en RGB565 (sans mise à l'échelle)
      if (jpg2rgb565(jpeg_data, frame_header->size, rgb_buf, JPG_SCALE_NONE)) {
        // Calculer les facteurs de mise à l'échelle
        float scale_x = (float)this->video_width_ / display_->get_width();
        float scale_y = (float)this->video_height_ / display_->get_height();
        
        // Dessiner sur l'écran
        for (int y = 0; y < (int)this->video_height_; y++) {
          int display_y = (int)(y / scale_y);
          if (display_y >= display_->get_height()) continue;
          
          for (int x = 0; x < (int)this->video_width_; x++) {
            int display_x = (int)(x / scale_x);
            if (display_x >= display_->get_width()) continue;
            
            // Pour RGB565, chaque pixel fait 2 octets
            int idx = (y * this->video_width_ + x) * 2;
            uint16_t pixel = (rgb_buf[idx + 1] << 8) | rgb_buf[idx];
            
            // Convertir RGB565 en RGB888
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            
            display_->draw_pixel_at(display_x, display_y, Color(r, g, b));
          }
        }
        ESP_LOGD(TAG, "Frame converted and drawn");
      } else {
        ESP_LOGE(TAG, "JPEG conversion failed");
      }
      free(rgb_buf);
    } else {
      ESP_LOGE(TAG, "Failed to allocate RGB buffer (requested %d bytes)", rgb_buf_size);
    }
    
    return true;
  }
  
  return false;
}

void VideoPlayerComponent::loop() {
  const uint32_t now = millis();
  if (now - last_update_ < update_interval_) {
    return;
  }
  last_update_ = now;
  
  if (read_next_frame()) {
    display_->update();
    this->current_frame_++;
  }
}

void VideoPlayerComponent::dump_info() {
  ESP_LOGCONFIG(TAG, "Video Player:");
  ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", this->video_width_, this->video_height_);
  ESP_LOGCONFIG(TAG, "  Frames: %d", this->frame_count_);
  ESP_LOGCONFIG(TAG, "  FPS: %d", this->video_fps_);
  ESP_LOGCONFIG(TAG, "  Source: %s", this->source_ == VideoSource::FILE ? "File" : "HTTP");
  if (this->source_ == VideoSource::FILE) {
    ESP_LOGCONFIG(TAG, "  File: %s", this->video_path_);
  } else {
    ESP_LOGCONFIG(TAG, "  URL: %s", this->http_url_);
  }
}

}  // namespace video_player
}  // namespace esphome
