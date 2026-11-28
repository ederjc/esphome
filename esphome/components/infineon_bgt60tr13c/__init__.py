from esphome import pins
import esphome.codegen as cg
from esphome.components import spi
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_IRQ_PIN, CONF_RESET_PIN
from esphome.types import ConfigType

CODEOWNERS = ["@ederjc"]
DEPENDENCIES = ["spi"]
DOMAIN = "infineon_bgt60tr13c"
MULTI_CONF = True

bgt60tr13c_ns = cg.esphome_ns.namespace(DOMAIN)
BGT60TR13C = bgt60tr13c_ns.class_("BGT60TR13C", cg.PollingComponent, spi.SPIDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BGT60TR13C),
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_IRQ_PIN): pins.gpio_input_pin_schema,
        }
    )
    .extend(cv.polling_component_schema("2s"))
    .extend(spi.spi_device_schema(True, "1MHz", "MODE0"))
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    irq = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
    cg.add(var.set_reset_pin(reset))
    cg.add(var.set_irq_pin(irq))
