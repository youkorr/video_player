#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "video_player.h"

// Inclusions pour ESP-IDF 5.1.5
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Pour la lecture du fichier MJPEG
#include <stdio.h>
#include <string.h>
#include <functional>
#include <memory>
#include <algorithm>

// Ajout de l'inclusion pour jpg2rgb565
#include "esp_jpg_decode.h"

// Déclaration pour jpg2rgb565 si non déclaré ailleurs
#ifndef jpg2rgb565
extern "C" {
  bool jpg2rgb565(const uint8_t *src, size_t src_len, uint8_t *out, jpg_scale_t scale);
}
#endif

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
      ESP_LOGV(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
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
  
  // Initialiser les mutex pour la synchronisation
  this->init_mutex();
  
  // Ne pas échouer immédiatement avec la source HTTP, nous réessaierons dans loop
  if (this->source_ == VideoSource::FILE) {
    if (!this->open_file_source()) {
      this->mark_failed();
      return;
    }
    ESP_LOGI(TAG, "Video loaded: %dx%d, %d frames, %d FPS", 
             this->video_width_, this->video_height_, this->frame_count_, this->video_fps_);
  } else if (this->source_ == VideoSource::HTTP) {
    // Juste journaliser que nous initialiserons plus tard
    ESP_LOGI(TAG, "HTTP source set, will initialize when network is available");
    this->http_initialized_ = false;
  }
  
  ESP_LOGI(TAG, "Display dimensions: %dx%d", display_->get_width(), display_->get_height());
}

void VideoPlayerComponent::init_mutex() {
  // Initialiser les mutex pour la synchronisation
  this->network_mutex_ = xSemaphoreCreateMutex();
  if (this->network_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create network mutex");
    this->mark_failed();
    return;
  }
}

VideoPlayerComponent::~VideoPlayerComponent() {
  // Nettoyer les ressources
  this->cleanup();
}

void VideoPlayerComponent::cleanup() {
  // Nettoyer les ressources HTTP
  if (this->http_buffer_ != nullptr) {
    heap_caps_free(this->http_buffer_);
    this->http_buffer_ = nullptr;
  }
  
  // Fermer le fichier vidéo
  if (this->video_file_ != nullptr) {
    fclose(this->video_file_);
    this->video_file_ = nullptr;
  }
  
  // Libérer les mutex
  if (this->network_mutex_ != nullptr) {
    vSemaphoreDelete(this->network_mutex_);
    this->network_mutex_ = nullptr;
  }
  
  // Démonter SPIFFS si nécessaire
  if (this->spiffs_mounted_) {
    esp_vfs_spiffs_unregister(NULL);
    this->spiffs_mounted_ = false;
  }
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
  this->spiffs_mounted_ = true;
  
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
  if (header.signature != 0xFEFFD8FF) {  // "MJPG" en little-endian
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

  // Vérifier si l'URL commence par http:// ou https://
  if (strncmp(this->http_url_, "http://", 7) != 0 && 
      strncmp(this->http_url_, "https://", 8) != 0) {
    ESP_LOGE(TAG, "Invalid HTTP URL format: %s", this->http_url_);
    return false;
  }
  
  // Vérifier que le réseau est initialisé avant de continuer
  if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
    ESP_LOGW(TAG, "Network interface not ready, deferring HTTP initialization");
    return false;
  }

  // Ajouter une vérification d'activité WebDAV
  bool webdav_active = false;  // Remplacer par une vérification réelle si possible
  if (webdav_active) {
    ESP_LOGW(TAG, "WebDAV transfer in progress, deferring video initialization");
    return false;
  }
  
  // Acquérir le mutex réseau avec un pattern RAII
  bool mutex_acquired = false;
  if (xSemaphoreTake(this->network_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    mutex_acquired = true;
  } else {
    ESP_LOGE(TAG, "Failed to acquire network mutex");
    return false;
  }
  
  // Créer un gestionnaire de nettoyage pour s'assurer que le mutex est libéré
  auto mutex_guard = std::unique_ptr<bool, std::function<void(bool*)>>(
    &mutex_acquired,
    [this](bool* acquired) {
      if (*acquired) {
        xSemaphoreGive(this->network_mutex_);
      }
    }
  );
  
  ESP_LOGI(TAG, "Connecting to HTTP source: %s", this->http_url_);

  // Afficher l'état du réseau
  esp_netif_ip_info_t ip_info;
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    ESP_LOGI(TAG, "Network - IP: " IPSTR ", Gateway: " IPSTR, 
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
  }

  // Utiliser une taille de buffer plus petite pour réduire le risque de fragmentation
  const size_t http_chunk_size = 4096;
  
  // Configuration HTTP avec plus de robustesse
  esp_http_client_config_t config = {};
  config.url = this->http_url_;
  config.event_handler = http_event_handler;
  config.user_data = this;
  config.timeout_ms = 5000;
  config.buffer_size = http_chunk_size;
  config.disable_auto_redirect = false;
  config.skip_cert_common_name_check = true;  // Plus tolérant pour HTTPS
  
  // Initialisation client avec gestion d'erreur
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return false;
  }
  
  // RAII pour le client HTTP
  auto http_client_guard = std::unique_ptr<esp_http_client_handle_t, std::function<void(esp_http_client_handle_t*)>>(
    &client,
    [](esp_http_client_handle_t* client) {
      if (*client != NULL) {
        esp_http_client_cleanup(*client);
      }
    }
  );
  
  // Réinitialiser le timer watchdog
  esp_task_wdt_reset();
  
  // Ouvrir la connexion avec surveillance de l'état
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    return false;
  }
  
  // Vérifier la réponse HTTP
  int content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP response - Status: %d, Content-Length: %d", status_code, content_length);
  
  if (status_code != 200) {
    ESP_LOGE(TAG, "HTTP request failed with status code %d", status_code);
    esp_http_client_close(client);
    return false;
  }
  
  if (content_length <= 0) {
    ESP_LOGW(TAG, "Content length unknown or zero, proceeding cautiously");
  }
  
  // Lire l'en-tête avec plus de diagnostics
  size_t initial_buffer_size = 8192;
  uint8_t* header_buffer = (uint8_t*)heap_caps_malloc(initial_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!header_buffer) {
    ESP_LOGW(TAG, "Failed to allocate from internal memory, trying SPIRAM");
    header_buffer = (uint8_t*)heap_caps_malloc(initial_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!header_buffer) {
      ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
      esp_http_client_close(client);
      return false;
    }
  }
  
  // RAII pour le buffer
  auto buffer_guard = std::unique_ptr<uint8_t, std::function<void(uint8_t*)>>(
    header_buffer,
    [](uint8_t* ptr) {
      if (ptr != nullptr) {
        heap_caps_free(ptr);
      }
    }
  );
  
  // Timeout et diagnostic améliorés pour la lecture
  const int read_timeout_ms = 5000;
  int64_t start_time = esp_timer_get_time();
  
  // Lire l'en-tête MJPEG avec plus de diagnostics
  int read_len = esp_http_client_read(client, (char*)header_buffer, sizeof(mjpeg_header_t));
  if (read_len != sizeof(mjpeg_header_t)) {
    ESP_LOGE(TAG, "Failed to read MJPEG header from HTTP (got %d bytes, expected %d)",
             read_len, sizeof(mjpeg_header_t));
    
    // Afficher les premiers octets pour le débogage
    if (read_len > 0) {
      char hex_dump[100] = {0};
      char *ptr = hex_dump;
      for (int i = 0; i < std::min(read_len, 16); i++) {
        ptr += sprintf(ptr, "%02X ", header_buffer[i]);
      }
      ESP_LOGE(TAG, "First bytes: %s", hex_dump);
      
      // Vérifier si c'est potentiellement un JPEG standard
      if (read_len >= 2 && header_buffer[0] == 0xFF && header_buffer[1] == 0xD8) {
        ESP_LOGW(TAG, "Detected standard JPEG data instead of MJPEG container");
      }
    }
    
    esp_http_client_close(client);
    return false;
  }
  
  // Réinitialiser le timer
  esp_task_wdt_reset();
  
  // Analyser l'en-tête avec plus de tolérance
  mjpeg_header_t* header = (mjpeg_header_t*)header_buffer;
  ESP_LOGI(TAG, "Signature reçue: 0x%08X (attendue: 0x47504A4D)", header->signature);
  
  // Signature MJPEG alternative possible
  static const uint32_t MJPEG_SIGNATURE_ALT = 0x4A504547;  // "JPEG"
  
  if (header->signature != 0x47504A4D && header->signature != MJPEG_SIGNATURE_ALT) {
    ESP_LOGE(TAG, "Invalid MJPEG signature from HTTP: 0x%08X", header->signature);
    esp_http_client_close(client);
    return false;
  }
  
  // Validation des paramètres d'en-tête avec valeurs par défaut
  if (header->width == 0 || header->height == 0 || 
      header->width > 4096 || header->height > 4096) {
    ESP_LOGE(TAG, "Invalid video dimensions: %dx%d", header->width, header->height);
    esp_http_client_close(client);
    return false;
  }
  
  if (header->frame_count == 0 || header->fps == 0 || header->fps > 120) {
    ESP_LOGW(TAG, "Suspicious video parameters: %d frames, %d FPS - using defaults", 
             header->frame_count, header->fps);
    // Utiliser des valeurs par défaut si nécessaire
    if (header->frame_count == 0) header->frame_count = 100;
    if (header->fps == 0 || header->fps > 120) header->fps = 30;
  }
  
  this->video_width_ = header->width;
  this->video_height_ = header->height;
  this->frame_count_ = header->frame_count;
  this->video_fps_ = header->fps;
  
  ESP_LOGI(TAG, "Video parameters: %dx%d, %d frames, %d FPS", 
           this->video_width_, this->video_height_, this->frame_count_, this->video_fps_);
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
    
    // Vérifier la taille du frame pour éviter des allocations trop grandes
    if (frame_header.size > 1024*1024) {  // Limite à 1MB
      ESP_LOGE(TAG, "Frame size too large: %u bytes", frame_header.size);
      return false;
    }
    
    // Allouer un buffer pour les données JPEG
    uint8_t* jpeg_data = nullptr;
    
    // Essayer d'abord avec la mémoire interne
    jpeg_data = (uint8_t*)heap_caps_malloc(frame_header.size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!jpeg_data) {
      // Si ça échoue, essayer avec SPIRAM
      jpeg_data = (uint8_t*)heap_caps_malloc(frame_header.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!jpeg_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG data");
        return false;
      }
    }
    
    // Lire les données JPEG
    read_size = fread(jpeg_data, 1, frame_header.size, this->video_file_);
    if (read_size != frame_header.size) {
      ESP_LOGE(TAG, "Failed to read JPEG data");
      heap_caps_free(jpeg_data);
      return false;
    }
    
    ESP_LOGD(TAG, "Read frame: %d bytes", frame_header.size);
    
    // Clear display
    display_->fill(Color::BLACK);
    
    // Traiter le frame
    bool result = process_frame(jpeg_data, frame_header.size);
    
    // Libérer la mémoire
    heap_caps_free(jpeg_data);
    
    return result;
  }
  else if (this->source_ == VideoSource::HTTP) {
    // Lire depuis le buffer HTTP
    if (this->http_buffer_ == nullptr || 
        this->http_buffer_pos_ + sizeof(mjpeg_frame_header_t) >= this->http_buffer_size_used_) {
      ESP_LOGW(TAG, "HTTP buffer empty or insufficient data, pos=%d, size=%d", 
               this->http_buffer_pos_, this->http_buffer_size_used_);
      
      // Si nous avons atteint la fin du buffer, recommencer depuis le début
      if (this->loop_video_) {
        ESP_LOGI(TAG, "End of HTTP buffer, restarting");
        this->http_buffer_pos_ = sizeof(mjpeg_header_t);  // Sauter l'en-tête
        
        // Si nous sommes toujours à la fin, c'est qu'il n'y a pas assez de données
        if (this->http_buffer_pos_ + sizeof(mjpeg_frame_header_t) >= this->http_buffer_size_used_) {
          ESP_LOGE(TAG, "Not enough data in HTTP buffer");
          return false;
        }
      } else {
        return false;
      }
    }
    
    // Réinitialiser le watchdog avant de traiter le frame
    esp_task_wdt_reset();
    
    // Lire l'en-tête du frame
    mjpeg_frame_header_t* frame_header = (mjpeg_frame_header_t*)(this->http_buffer_ + this->http_buffer_pos_);
    this->http_buffer_pos_ += sizeof(mjpeg_frame_header_t);
    
    // Vérifier la taille du frame
    if (frame_header->size > 1024*1024 || frame_header->size == 0) {  // Limite à 1MB
      ESP_LOGE(TAG, "Invalid frame size: %u bytes", frame_header->size);
      return false;
    }
    
    if (this->http_buffer_pos_ + frame_header->size > this->http_buffer_size_used_) {
      ESP_LOGE(TAG, "Insufficient data in HTTP buffer for frame");
      return false;
    }
    
    // Pointer vers les données JPEG dans le buffer
    uint8_t* jpeg_data = this->http_buffer_ + this->http_buffer_pos_;
    this->http_buffer_pos_ += frame_header->size;
    
    ESP_LOGD(TAG, "Read HTTP frame: %d bytes", frame_header->size);
    
    // Clear display
    display_->fill(Color::BLACK);
    
    // Traiter le frame
    return process_frame(jpeg_data, frame_header->size);
  }
  
  return false;
}

bool VideoPlayerComponent::process_frame(const uint8_t* jpeg_data, size_t jpeg_size) {
  // Configurer une taille maximale pour le buffer RGB
  const size_t max_rgb_buf_size = 256*256*2;  // 256x256 pixels maximum en RGB565
  
  // Calculer la taille réelle nécessaire
  size_t rgb_buf_size = this->video_width_ * this->video_height_ * 2;  // 2 bytes par pixel pour RGB565
  
  // Limiter la taille du buffer RGB
  if (rgb_buf_size > max_rgb_buf_size) {
    ESP_LOGW(TAG, "RGB buffer size too large (%d bytes), limiting to %d bytes", 
             rgb_buf_size, max_rgb_buf_size);
    rgb_buf_size = max_rgb_buf_size;
  }
  
  // Allouer le buffer RGB
  uint8_t *rgb_buf = nullptr;
  
  // Essayer d'abord avec la mémoire interne
  rgb_buf = (uint8_t *)heap_caps_malloc(rgb_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!rgb_buf) {
    // Si ça échoue, essayer avec SPIRAM
    rgb_buf = (uint8_t *)heap_caps_malloc(rgb_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buf) {
      ESP_LOGE(TAG, "Failed to allocate RGB buffer (requested %d bytes)", rgb_buf_size);
      return false;
    }
  }
  
  // Utiliser un unique_ptr pour garantir la libération de mémoire
  auto rgb_buf_guard = std::unique_ptr<uint8_t, std::function<void(uint8_t*)>>(
    rgb_buf,
    [](uint8_t* ptr) {
      if (ptr != nullptr) {
        heap_caps_free(ptr);
      }
    }
  );
  
  // Déterminer l'échelle à utiliser
  jpg_scale_t scale = JPG_SCALE_NONE;
  
  // Si la vidéo est trop grande, on utilise un scaling
  if (this->video_width_ > display_->get_width() * 2 || 
      this->video_height_ > display_->get_height() * 2) {
    scale = JPG_SCALE_2X;
    ESP_LOGD(TAG, "Using 2x downscaling for JPEG");
  }
  
  // Réinitialiser le watchdog avant la conversion JPEG
  esp_task_wdt_reset();
  
  // Convertir JPEG en RGB565
  bool conversion_success = jpg2rgb565(jpeg_data, jpeg_size, rgb_buf, scale);
  
  if (conversion_success) {
    // Calculer les dimensions après scaling
    uint32_t scaled_width = (scale == JPG_SCALE_NONE) ? this->video_width_ : (this->video_width_ / 2);
    uint32_t scaled_height = (scale == JPG_SCALE_NONE) ? this->video_height_ : (this->video_height_ / 2);
    
    // Calculer les facteurs de mise à l'échelle pour l'affichage
    float scale_x = (float)scaled_width / display_->get_width();
    float scale_y = (float)scaled_height / display_->get_height();
    
    // Réinitialiser le watchdog avant le rendu
    esp_task_wdt_reset();
    
    // Dessiner sur l'écran
    for (int y = 0; y < (int)display_->get_height(); y++) {
      int src_y = (int)(y * scale_y);
      if (src_y >= (int)scaled_height) continue;
      
      for (int x = 0; x < (int)display_->get_width(); x++) {
        int src_x = (int)(x * scale_x);
        if (src_x >= (int)scaled_width) continue;
        
        // Pour RGB565, chaque pixel fait 2 octets
        int idx = (src_y * scaled_width + src_x) * 2;
        
        // Vérifier les bornes
        if (idx + 1 >= (int)rgb_buf_size) continue;
        
        uint16_t pixel = (rgb_buf[idx + 1] << 8) | rgb_buf[idx];
        
        // Convertir RGB565 en RGB888
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        
        display_->draw_pixel_at(x, y, Color(r, g, b));
      }
    }
    ESP_LOGD(TAG, "Frame converted and drawn");
  } else {
    ESP_LOGE(TAG, "JPEG conversion failed");
  }
  
  // rgb_buf sera automatiquement libéré par rgb_buf_guard
  return conversion_success;
}

void VideoPlayerComponent::loop() {
  const uint32_t now = millis();
  
  // Vérifier si HTTP a besoin d'initialisation
  if (this->source_ == VideoSource::HTTP && !this->http_initialized_) {
    if (now - last_http_init_attempt_ > 5000) { // Essayer toutes les 5 secondes
      last_http_init_attempt_ = now;
      if (this->open_http_source()) {
        this->http_initialized_ = true;
        ESP_LOGI(TAG, "HTTP source initialized successfully");
      } else {
        ESP_LOGW(TAG, "HTTP initialization deferred, will retry");
        return;
      }
    } else {
      // Pas encore le moment de réessayer
      return;
    }
  }
  
  if (now - last_update_ < update_interval_) {
    return;
  }
  last_update_ = now;
  
  // Cession de tâche plus longue pour éviter d'affamer la pile réseau
  vTaskDelay(pdMS_TO_TICKS(5));
  
  // Réinitialiser le watchdog avant le traitement du frame
  esp_task_wdt_reset();
  
  if (read_next_frame()) {
    display_->update();
    this->current_frame_++;
    
    // Pour déboguer la mémoire
    if (this->current_frame_ % 100 == 0) {
      ESP_LOGI(TAG, "Memory - Free: %u bytes, Min Free: %u bytes",
               esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
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



