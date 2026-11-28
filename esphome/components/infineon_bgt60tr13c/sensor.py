import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import STATE_CLASS_MEASUREMENT, UNIT_HERTZ
from esphome.types import ConfigType

from . import BGT60TR13C, CONF_INFINEON_BGT60TR13C_ID

DEPENDENCIES = ["infineon_bgt60tr13c"]

CONF_FRAME_RATE = "frame_rate"
CONF_SIGNAL_RMS = "signal_rms"
CONF_MOTION_RMS = "motion_rms"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INFINEON_BGT60TR13C_ID): cv.use_id(BGT60TR13C),
        cv.Optional(CONF_FRAME_RATE): sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:speedometer",
        ),
        cv.Optional(CONF_SIGNAL_RMS): sensor.sensor_schema(
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:radar",
        ),
        cv.Optional(CONF_MOTION_RMS): sensor.sensor_schema(
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:motion-sensor",
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_INFINEON_BGT60TR13C_ID])
    if frame_rate := config.get(CONF_FRAME_RATE):
        cg.add(parent.set_frame_rate_sensor(await sensor.new_sensor(frame_rate)))
    if signal_rms := config.get(CONF_SIGNAL_RMS):
        cg.add(parent.set_signal_rms_sensor(await sensor.new_sensor(signal_rms)))
    if motion_rms := config.get(CONF_MOTION_RMS):
        cg.add(parent.set_motion_rms_sensor(await sensor.new_sensor(motion_rms)))
