from esphome import pins
from esphome.components.zephyr import zephyr_add_overlay, zephyr_variant
from esphome.components.zephyr.gpio import ZEPHYR_PIN_SCHEMA, zephyr_pin_to_code
from esphome.const import CONF_NUMBER, PLATFORM_INFINEON
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType


@pins.PIN_SCHEMA_REGISTRY.register(PLATFORM_INFINEON, ZEPHYR_PIN_SCHEMA)
async def infineon_pin_to_code(config: ConfigType) -> MockObj:
    pin = await zephyr_pin_to_code(config)
    variant = zephyr_variant()
    port = config[CONF_NUMBER] // variant.gpio_port_width
    zephyr_add_overlay(f'&{variant.gpio_node_prefix}{port} {{ status = "okay"; }};')
    return pin
