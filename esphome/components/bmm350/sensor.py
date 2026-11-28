import esphome.codegen as cg
from esphome.components import i2c, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_FIELD_STRENGTH_X,
    CONF_FIELD_STRENGTH_Y,
    CONF_FIELD_STRENGTH_Z,
    CONF_TEMPERATURE,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_TEMPERATURE,
    ICON_MAGNET,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_MICROTESLA,
)
from esphome.types import ConfigType

DEPENDENCIES = ["i2c"]

bmm350_ns = cg.esphome_ns.namespace("bmm350")
BMM350Component = bmm350_ns.class_(
    "BMM350Component", cg.PollingComponent, i2c.I2CDevice
)

field_strength_schema = sensor.sensor_schema(
    unit_of_measurement=UNIT_MICROTESLA,
    icon=ICON_MAGNET,
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BMM350Component),
            cv.Optional(CONF_FIELD_STRENGTH_X): field_strength_schema,
            cv.Optional(CONF_FIELD_STRENGTH_Y): field_strength_schema,
            cv.Optional(CONF_FIELD_STRENGTH_Z): field_strength_schema,
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x14))
    .extend(
        {
            cv.Optional(CONF_ADDRESS, default=0x14): cv.one_of(0x14, 0x15, int=True),
            cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=40)),
            ),
        }
    )
)


async def to_code(config: ConfigType) -> None:
    var = await i2c.new_i2c_component(config)
    sensors = sensor.sub_sensors(config)
    await sensors(CONF_FIELD_STRENGTH_X, var.set_x_sensor)
    await sensors(CONF_FIELD_STRENGTH_Y, var.set_y_sensor)
    await sensors(CONF_FIELD_STRENGTH_Z, var.set_z_sensor)
    await sensors(CONF_TEMPERATURE, var.set_temperature_sensor)
