import esphome.codegen as cg
from esphome.components import audio, microphone
from esphome.components.zephyr import zephyr_add_prj_conf, zephyr_data
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BOARD,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)
from esphome.core.config import get_loop_interval
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import zephyr_dmic_ns

CONF_DEVICE = "device"
BLOCK_SIZE = 640
BLOCK_COUNT = 8
POLL_OVERHEAD_MS = 20

ZephyrDMICMicrophone = zephyr_dmic_ns.class_(
    "ZephyrDMICMicrophone", microphone.Microphone, cg.Component
)


def _validate_format(config: ConfigType) -> ConfigType:
    if zephyr_data()[CONF_BOARD] != "cy8ckit_062s2_ai":
        raise cv.Invalid(
            "Only CY8CKIT-062S2-AI with the synchronous PDMv1 driver is supported"
        )
    if (
        config[CONF_DEVICE] != "pdm0"
        or config[CONF_SAMPLE_RATE] != 15625
        or config[CONF_NUM_CHANNELS] != 2
    ):
        raise cv.Invalid(
            "CY8CKIT-062S2-AI requires pdm0, 15625 Hz, and two channels. "
            "Its PDMv1 downstream driver uses the independent 8 MHz HF1 clock."
        )
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=config[CONF_NUM_CHANNELS],
        max_channels=config[CONF_NUM_CHANNELS],
        min_sample_rate=config[CONF_SAMPLE_RATE],
        max_sample_rate=config[CONF_SAMPLE_RATE],
    )(config)
    return config


def _final_validate(config: ConfigType) -> None:
    owners = fv.platform_entries(domain="microphone", platform="zephyr_dmic")
    # Only pdm0 is supported; code generation asserts its resolved compatible/address.
    if len(owners) > 1:
        raise cv.Invalid(
            "Only one microphone backend may own resolved PDM0 at 0x40a00000"
        )
    usable_us = (
        (BLOCK_COUNT // 2)
        * BLOCK_SIZE
        * 1_000_000
        // (config[CONF_SAMPLE_RATE] * config[CONF_NUM_CHANNELS] * 2)
    )
    if (get_loop_interval().total_milliseconds + POLL_OVERHEAD_MS) * 1000 > usable_us:
        raise cv.Invalid(
            "PDMv1 requires loop_interval <= 20ms: reserve 20ms processing overhead "
            "and half the PCM slab capacity as headroom"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ZephyrDMICMicrophone),
            cv.Required(CONF_DEVICE): cv.validate_id_name,
            cv.Required(CONF_SAMPLE_RATE): cv.int_range(8000, 48000),
            cv.Required(CONF_NUM_CHANNELS): cv.int_range(1, 2),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_format,
)


async def to_code(config: ConfigType) -> None:
    node = f"DT_NODELABEL({config[CONF_DEVICE]})"
    cg.add_global(
        cg.RawStatement(
            f"static_assert(DT_NODE_HAS_COMPAT({node}, infineon_pdm_v1) && "
            f'DT_REG_ADDR({node}) == 0x40a00000, "Requires the synchronous PDMv1 driver");'
        )
    )
    for index, (label, value) in enumerate(
        (
            ("p10_4_audioss0_pdm_clk", "0x15040a"),
            ("p10_5_audioss0_pdm_data", "0x15050a"),
        )
    ):
        pin = f"DT_PHANDLE_BY_IDX({node}, pinctrl_0, {index})"
        cg.add_global(
            cg.RawStatement(
                f"static_assert(DT_SAME_NODE({pin}, DT_NODELABEL({label})) && "
                f"DT_PROP({pin}, pinmux) == {value}, "
                '"Requires 124-BGA AUDIOSS HSIOM 21 (ACT_9)");'
            )
        )
    device = cg.RawExpression(f"DEVICE_DT_GET(DT_NODELABEL({config[CONF_DEVICE]}))")
    var = cg.new_Pvariable(
        config[CONF_ID],
        device,
        config[CONF_SAMPLE_RATE],
        config[CONF_NUM_CHANNELS],
    )
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    zephyr_add_prj_conf("AUDIO", True)
    zephyr_add_prj_conf("AUDIO_DMIC", True)
