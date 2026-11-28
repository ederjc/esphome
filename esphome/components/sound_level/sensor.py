from esphome import automation
import esphome.codegen as cg
from esphome.components import microphone, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEVICE_CLASS,
    CONF_ID,
    CONF_MEASUREMENT_DURATION,
    CONF_MICROPHONE,
    CONF_UNIT_OF_MEASUREMENT,
    DEVICE_CLASS_SOUND_PRESSURE,
    PLATFORM_ESP32,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL,
)
from esphome.core import CORE
from esphome.types import ConfigType

AUTO_LOAD = ["audio"]
CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["microphone"]


CONF_PASSIVE = "passive"
CONF_PEAK = "peak"
CONF_RMS = "rms"

sound_level_ns = cg.esphome_ns.namespace("sound_level")
SoundLevelComponent = sound_level_ns.class_("SoundLevelComponent", cg.Component)


def _validate_platform(config: ConfigType) -> ConfigType:
    if CORE.using_toolchain_zephyr:
        if len(config[CONF_MICROPHONE]["channels"]) != 1:
            raise cv.Invalid(
                "Zephyr sound_level requires one selected microphone channel. "
                "Use separate passive sources with channels: [0] and [1] "
                "for independent stereo levels.",
                [CONF_MICROPHONE, "channels"],
            )
        for key in (CONF_PEAK, CONF_RMS):
            if key in config:
                config[key].setdefault(CONF_UNIT_OF_MEASUREMENT, "dBFS")
        return config
    for key in (CONF_PEAK, CONF_RMS):
        if key in config:
            config[key].setdefault(CONF_UNIT_OF_MEASUREMENT, UNIT_DECIBEL)
            config[key].setdefault(CONF_DEVICE_CLASS, DEVICE_CLASS_SOUND_PRESSURE)
    return cv.only_on([PLATFORM_ESP32])(config)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SoundLevelComponent),
            cv.Optional(CONF_MEASUREMENT_DURATION, default="1000ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=50),
                    max=cv.TimePeriod(seconds=60),
                ),
            ),
            cv.Optional(
                CONF_MICROPHONE, default={}
            ): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                max_channels=4,
            ),
            cv.Required(CONF_PASSIVE): cv.boolean,
            cv.Optional(CONF_PEAK): sensor.sensor_schema(
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_RMS): sensor.sensor_schema(
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_platform,
)

FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(
            CONF_MICROPHONE
        ): microphone.final_validate_microphone_source_schema("sound_level"),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=config[CONF_PASSIVE]
    )
    cg.add(var.set_microphone_source(mic_source))

    cg.add(var.set_measurement_duration(config[CONF_MEASUREMENT_DURATION]))

    sensors = sensor.sub_sensors(config)
    await sensors(CONF_PEAK, var.set_peak_sensor)
    await sensors(CONF_RMS, var.set_rms_sensor)


SOUND_LEVEL_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(SoundLevelComponent),
    }
)


automation.register_apply_action(
    "sound_level.start", SOUND_LEVEL_ACTION_SCHEMA, automation.ApplyCall("start()")
)
automation.register_apply_action(
    "sound_level.stop", SOUND_LEVEL_ACTION_SCHEMA, automation.ApplyCall("stop()")
)
