import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_BROAN_ID, BroanComponent

DEPENDENCIES = ["broan"]

CONF_FAN_MODE_SOURCE = "fan_mode_source"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BROAN_ID): cv.use_id(BroanComponent),
        cv.Optional(CONF_FAN_MODE_SOURCE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:remote",
        ),
    }
)


async def to_code(config):
    broan_component = await cg.get_variable(config[CONF_BROAN_ID])

    if fan_mode_source_config := config.get(CONF_FAN_MODE_SOURCE):
        sens = await text_sensor.new_text_sensor(fan_mode_source_config)
        cg.add(broan_component.set_fan_mode_source_text_sensor(sens))
