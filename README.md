```
# Exemple pour le fichier local
external_components:
  - source: github://mon-utilisateur/mon-repo
    components: [video_player]

display:
  - platform: ...  # Votre écran

video_player:
  id: my_video_player
  display_id: mon_ecran
  video_path: /spiffs/video.mjpg
  update_interval: 33ms

# OU pour une source HTTP
video_player:
  id: my_video_player
  display_id: mon_ecran
  url: http://example.com/video.mjpg
  update_interval: 33ms
```
