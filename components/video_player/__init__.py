"""Composant pour la lecture de vidéo MJPEG sur un écran."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID, CONF_DISPLAY_ID, CONF_UPDATE_INTERVAL, CONF_URL

DEPENDENCIES = ["display", "api"]
CODEOWNERS = ["@votre_nom_utilisateur"]

video_player_ns = cg.esphome_ns.namespace("video_player")
VideoPlayerComponent = video_player_ns.class_("VideoPlayerComponent", cg.Component)

CONF_VIDEO_PATH = "video_path"

VIDEO_SCHEMA = cv.Schema({
    cv.Required(CONF_DISPLAY_ID): cv.use_id(display.DisplayBuffer),
    cv.Optional(CONF_UPDATE_INTERVAL, default="33ms"): cv.update_interval,
})

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VideoPlayerComponent),
        cv.Optional(CONF_VIDEO_PATH): cv.string,
        cv.Optional(CONF_URL): cv.url,
    }
).extend(VIDEO_SCHEMA).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    display_ = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(display_))
    
    if CONF_VIDEO_PATH in config:
        cg.add(var.set_video_path(config[CONF_VIDEO_PATH]))
    
    if CONF_URL in config:
        cg.add(var.set_http_url(config[CONF_URL]))
    
    if CONF_UPDATE_INTERVAL in config:
        cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
