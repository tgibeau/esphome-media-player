import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["network"]
CODEOWNERS = ["@tgibeau"]
MULTI_CONF = False

sonos_player_ns = cg.esphome_ns.namespace("sonos_player")
SonosPlayer = sonos_player_ns.class_("SonosPlayer", cg.PollingComponent)

CONF_SONOS_IP = "ip"
CONF_TITLE_SENSOR = "title_sensor"
CONF_ARTIST_SENSOR = "artist_sensor"
CONF_ARTWORK_SENSOR = "artwork_sensor"
CONF_STATE_SENSOR = "state_sensor"
CONF_SOURCE_SENSOR = "source_sensor"
CONF_GROUP_MEMBERS_SENSOR = "group_members_sensor"
CONF_DURATION_SENSOR = "duration_sensor"
CONF_POSITION_SENSOR = "position_sensor"
CONF_VOLUME_SENSOR = "volume_sensor"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SonosPlayer),
        cv.Optional(CONF_SONOS_IP, default=""): cv.string,
        cv.Optional(CONF_TITLE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_ARTIST_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_ARTWORK_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_STATE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_SOURCE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_GROUP_MEMBERS_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_DURATION_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_POSITION_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_VOLUME_SENSOR): cv.use_id(sensor.Sensor),
    }
).extend(cv.polling_component_schema("1s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_ip(config[CONF_SONOS_IP]))

    for conf_key, method in [
        (CONF_TITLE_SENSOR, "set_title_sensor"),
        (CONF_ARTIST_SENSOR, "set_artist_sensor"),
        (CONF_ARTWORK_SENSOR, "set_artwork_sensor"),
        (CONF_STATE_SENSOR, "set_state_sensor"),
        (CONF_SOURCE_SENSOR, "set_source_sensor"),
        (CONF_GROUP_MEMBERS_SENSOR, "set_group_members_sensor"),
    ]:
        if conf_key in config:
            sens = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, method)(sens))

    for conf_key, method in [
        (CONF_DURATION_SENSOR, "set_duration_sensor"),
        (CONF_POSITION_SENSOR, "set_position_sensor"),
        (CONF_VOLUME_SENSOR, "set_volume_sensor"),
    ]:
        if conf_key in config:
            sens = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, method)(sens))
