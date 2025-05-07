#include "video_player.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_jpg_decode.h"
#include "freertos/FreeRTOS.h"

namespace esphome {
namespace video_player {

static const char *TAG = "video.player";

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  auto *player = static_cast<VideoPlayerComponent *>(evt->user_data);
  
  switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      player->process_mjpeg_chunk((uint8_t *)evt->data, evt->data_len);
      break;
    default:
      break;
  }
  return ESP_OK;
}

void VideoPlayerComponent::setup() {
  ESP_LOGI(TAG, "Initializing MJPEG Player");
  if (parent_ == nullptr) {
    ESP_LOGE(TAG, "No display configured!");
    mark_failed();
    return;
  }
}

bool VideoPlayerComponent::process_mjpeg_chunk(const uint8_t *data, size_t len) {
  static uint8_t *jpg_buf = nullptr;
  static size_t jpg_size = 0;
  
  // Recherche du marqueur JPEG (0xFFD8)
  for (size_t i = 0; i < len - 1; i++) {
    if (data[i] == 0xFF && data[i + 1] == 0xD8) {
      // Trouvé un nouveau frame, traiter le précédent s'il existe
      if (jpg_buf != nullptr && jpg_size > 0) {
        decode_jpeg(jpg_buf, jpg_size);
        free(jpg_buf);
      }
      
      // Allouer buffer pour nouveau frame
      jpg_size = len - i;
      jpg_buf = (uint8_t *)malloc(jpg_size);
      if (!jpg_buf) {
        ESP_LOGE(TAG, "Memory allocation failed");
        return false;
      }
      memcpy(jpg_buf, data + i, jpg_size);
      return true;
    }
  }
  
  // Ajouter des données au frame en cours
  if (jpg_buf != nullptr) {
    uint8_t *tmp = (uint8_t *)realloc(jpg_buf, jpg_size + len);
    if (!tmp) {
      free(jpg_buf);
      jpg_buf = nullptr;
      return false;
    }
    jpg_buf = tmp;
    memcpy(jpg_buf + jpg_size, data, len);
    jpg_size += len;
  }
  
  return true;
}

bool VideoPlayerComponent::decode_jpeg(const uint8_t *src, size_t len) {
  const int width = parent_->get_width();
  const int height = parent_->get_height();
  uint8_t *rgb_buf = (uint8_t *)heap_caps_malloc(width * height * 2, MALLOC_CAP_SPIRAM);
  
  if (!rgb_buf) {
    ESP_LOGE(TAG, "RGB buffer allocation failed");
    return false;
  }

  bool decoded = jpg2rgb565(src, len, rgb_buf, JPG_SCALE_NONE);
  if (!decoded) {
    ESP_LOGE(TAG, "JPEG decode failed");
    free(rgb_buf);
    return false;
  }

  // Afficher l'image
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 2;
      uint16_t pixel = (rgb_buf[idx + 1] << 8) | rgb_buf[idx];
      parent_->draw_pixel_at(x, y, Color(
        ((pixel >> 11) & 0x1F) << 3,
        ((pixel >> 5) & 0x3F) << 2,
        (pixel & 0x1F) << 3
      ));
    }
  }

  free(rgb_buf);
  parent_->update();
  return true;
}

void VideoPlayerComponent::loop() {
  if (millis() - last_update_ < update_interval_ || failed()) {
    return;
  }
  last_update_ = millis();

  if (!initialized_) {
    esp_http_client_config_t config = {
      .url = url_,
      .event_handler = http_event_handler,
      .user_data = this,
      .buffer_size = 4096
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
      initialized_ = true;
      esp_http_client_perform(client);
      esp_http_client_cleanup(client);
    }
  }
}

}  // namespace video_player
}  // namespace esphome

