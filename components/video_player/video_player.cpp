#include "video_player.h"
#include "esp_http_client.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace video_player {

static const char *TAG = "video_player";

void VideoPlayerComponent::setup() {
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "Display not set!");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "MJPEG raw stream player ready");
  ESP_LOGI(TAG, "Display dimensions: %dx%d", 
           display_->get_width(), display_->get_height());
}

bool VideoPlayerComponent::process_raw_mjpeg(const uint8_t* data, size_t len) {
  // Recherche du marqueur JPEG de début (0xFFD8)
  const uint8_t* jpeg_start = nullptr;
  for (size_t i = 0; i < len - 1; i++) {
    if (data[i] == 0xFF && data[i+1] == 0xD8) {
      jpeg_start = data + i;
      break;
    }
  }

  if (!jpeg_start) {
    ESP_LOGD(TAG, "No JPEG start marker found");
    return false;
  }

  // Recherche du marqueur JPEG de fin (0xFFD9)
  const uint8_t* jpeg_end = nullptr;
  for (size_t i = (jpeg_start - data); i < len - 1; i++) {
    if (data[i] == 0xFF && data[i+1] == 0xD9) {
      jpeg_end = data + i + 2;
      break;
    }
  }

  if (!jpeg_end) {
    ESP_LOGD(TAG, "No JPEG end marker found");
    return false;
  }

  size_t jpeg_size = jpeg_end - jpeg_start;
  ESP_LOGD(TAG, "Found JPEG frame: %d bytes", jpeg_size);

  // Effacer l'écran avant d'afficher la nouvelle image
  display_->fill(COLOR_BLACK);

  // Traitement du frame JPEG
  return process_frame(jpeg_start, jpeg_size);
}

bool VideoPlayerComponent::process_frame(const uint8_t* jpeg_data, size_t jpeg_size) {
  // Configuration pour la décodage JPEG
  camera_fb_t fb;
  fb.buf = (uint8_t*)jpeg_data;
  fb.len = jpeg_size;
  fb.width = display_->get_width();
  fb.height = display_->get_height();
  fb.format = PIXFORMAT_JPEG;

  // Allouer un buffer pour l'image décodée
  uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(
    display_->get_width() * display_->get_height() * 2,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );

  if (!rgb_buf) {
    ESP_LOGE(TAG, "Failed to allocate RGB buffer");
    return false;
  }

  // Décoder le JPEG
  bool decoded = fmt2rgb888(&fb, PIXFORMAT_RGB565, rgb_buf);
  if (!decoded) {
    ESP_LOGE(TAG, "JPEG decoding failed");
    heap_caps_free(rgb_buf);
    return false;
  }

  // Afficher l'image
  for (int y = 0; y < display_->get_height(); y++) {
    for (int x = 0; x < display_->get_width(); x++) {
      int idx = (y * display_->get_width() + x) * 2;
      uint16_t pixel = (rgb_buf[idx+1] << 8) | rgb_buf[idx];
      uint8_t r = ((pixel >> 11) & 0x1F) << 3;
      uint8_t g = ((pixel >> 5) & 0x3F) << 2;
      uint8_t b = (pixel & 0x1F) << 3;
      display_->draw_pixel_at(x, y, Color(r, g, b));
    }
  }

  heap_caps_free(rgb_buf);
  return true;
}

esp_err_t VideoPlayerComponent::http_event_handler(esp_http_client_event_t *evt) {
  VideoPlayerComponent* player = static_cast<VideoPlayerComponent*>(evt->user_data);
  
  switch(evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      if (!player->process_raw_mjpeg((const uint8_t*)evt->data, evt->data_len)) {
        ESP_LOGW(TAG, "Failed to process MJPEG frame");
      }
      break;
      
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP disconnected");
      player->http_initialized_ = false;
      break;
      
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP error");
      player->http_initialized_ = false;
      break;
      
    default:
      break;
  }
  return ESP_OK;
}

void VideoPlayerComponent::loop() {
  const uint32_t now = millis();
  
  // Initialisation HTTP si nécessaire
  if (source_ == VideoSource::HTTP && !http_initialized_) {
    if (now - last_http_init_attempt_ > 5000) {
      last_http_init_attempt_ = now;
      
      esp_http_client_config_t config = {
        .url = http_url_,
        .event_handler = http_event_handler,
        .user_data = this,
        .timeout_ms = 5000,
        .buffer_size = 4096
      };
      
      esp_http_client_handle_t client = esp_http_client_init(&config);
      if (client) {
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
      }
    }
    return;
  }
  
  // Mise à jour périodique
  if (now - last_update_ < update_interval_) {
    return;
  }
  last_update_ = now;
  
  display_->update();
}

void VideoPlayerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "MJPEG Raw Stream Player:");
  ESP_LOGCONFIG(TAG, "  Source: %s", source_ == VideoSource::FILE ? "File" : "HTTP");
  if (source_ == VideoSource::HTTP) {
    ESP_LOGCONFIG(TAG, "  URL: %s", http_url_);
  } else {
    ESP_LOGCONFIG(TAG, "  File: %s", video_path_);
  }
}

}  // namespace video_player
}  // namespace esphome


