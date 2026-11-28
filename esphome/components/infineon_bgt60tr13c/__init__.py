from pathlib import Path
import re

from esphome import pins
import esphome.codegen as cg
from esphome.components import spi
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_IRQ_PIN,
    CONF_RAW_DATA_ID,
    CONF_RESET_PIN,
    CONF_TIMEOUT,
)
from esphome.types import ConfigType

CODEOWNERS = ["@ederjc"]
DEPENDENCIES = ["spi"]
DOMAIN = "infineon_bgt60tr13c"
MULTI_CONF = True

CONF_INFINEON_BGT60TR13C_ID = "infineon_bgt60tr13c_id"
CONF_REGISTERS = "registers"
CONF_REGISTERS_FILE = "registers_file"
CONF_NUM_SAMPLES_PER_CHIRP = "num_samples_per_chirp"
CONF_NUM_CHIRPS_PER_FRAME = "num_chirps_per_frame"
CONF_NUM_RX_ANTENNAS = "num_rx_antennas"
CONF_FIFO_THRESHOLD = "fifo_threshold"
CONF_ACQUISITION_MODE = "acquisition_mode"
CONF_USE_INTERRUPT = "use_interrupt"
CONF_FRAME_LOG_INTERVAL = "frame_log_interval"

FIFO_ADDRESS = 0x60
FIFO_SAMPLES = 8192 * 2
REGISTER_WRITE_FLAG = 1 << 24
GEOMETRY_KEYS = (
    CONF_NUM_SAMPLES_PER_CHIRP,
    CONF_NUM_CHIRPS_PER_FRAME,
    CONF_NUM_RX_ANTENNAS,
    CONF_FIFO_THRESHOLD,
    CONF_ACQUISITION_MODE,
    CONF_USE_INTERRUPT,
    CONF_TIMEOUT,
    CONF_FRAME_LOG_INTERVAL,
)

bgt60tr13c_ns = cg.esphome_ns.namespace(DOMAIN)
BGT60TR13C = bgt60tr13c_ns.class_("BGT60TR13C", cg.PollingComponent, spi.SPIDevice)
AcquisitionMode = bgt60tr13c_ns.enum("AcquisitionMode", is_class=True)
ACQUISITION_MODES = {
    "triggered": AcquisitionMode.TRIGGERED,
    "continuous": AcquisitionMode.CONTINUOUS,
}

_HEX_WORD = re.compile(r"^(?:0[xX])?([0-9a-fA-F]+)[uUlL]*$")


def parse_register_text(text: str) -> list[int]:
    """Parse register words from a configurator-style C array or a plain hex list.

    Comments and preprocessor lines are ignored. When braces are present only the first
    brace-enclosed initializer is used, so geometry ``#define`` lines in a generated
    header are not mistaken for register words.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    text = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )
    if "{" in text:
        start = text.index("{") + 1
        end = text.find("}", start)
        if end < 0:
            raise cv.Invalid("Register array has no closing '}'")
        text = text[start:end]
    words = []
    for token in re.split(r"[\s,;]+", text):
        if not token:
            continue
        match = _HEX_WORD.match(token)
        if match is None:
            raise cv.Invalid(f"Register entry '{token}' is not a hexadecimal word")
        words.append(int(match.group(1), 16))
    return words


def validate_register_word(value: int) -> int:
    value = cv.hex_int_range(min=0, max=0xFFFFFFFF)(value)
    if not value & REGISTER_WRITE_FLAG:
        raise cv.Invalid(
            f"Register word 0x{value:08X} lacks the SPI write flag (bit 24); "
            "use configurator output words, not bare register values"
        )
    if (value >> 25) >= FIFO_ADDRESS:
        raise cv.Invalid(
            f"Register word 0x{value:08X} addresses 0x{value >> 25:02X}; "
            f"only addresses below 0x{FIFO_ADDRESS:02X} are configurable"
        )
    return value


def validate_registers_file(value: object) -> list[int]:
    path: Path = cv.file_(value)
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as err:
        raise cv.Invalid(f"Cannot read register file '{path}': {err}") from err
    return parse_register_text(text)


def validate_register_list(value: list[int]) -> list[int]:
    if not value:
        raise cv.Invalid("The register list must not be empty")
    return [validate_register_word(word) for word in value]


def _validate_acquisition(config: ConfigType) -> ConfigType:
    has_registers = CONF_REGISTERS in config or CONF_REGISTERS_FILE in config
    if not has_registers:
        for key in GEOMETRY_KEYS:
            if key in config:
                raise cv.Invalid(
                    f"'{key}' requires '{CONF_REGISTERS}' or '{CONF_REGISTERS_FILE}'",
                    path=[key],
                )
        return config
    for key in (
        CONF_NUM_SAMPLES_PER_CHIRP,
        CONF_NUM_CHIRPS_PER_FRAME,
        CONF_NUM_RX_ANTENNAS,
    ):
        if key not in config:
            raise cv.Invalid(
                f"'{key}' is required with a register list; take it from the "
                "configurator output that produced the registers",
                path=[key],
            )
    frame_samples = (
        config[CONF_NUM_SAMPLES_PER_CHIRP]
        * config[CONF_NUM_CHIRPS_PER_FRAME]
        * config[CONF_NUM_RX_ANTENNAS]
    )
    if frame_samples % 2:
        raise cv.Invalid(
            f"A frame of {frame_samples} samples does not fill whole 24-bit FIFO words"
        )
    # FSTAT FIFO FULL is a reported error, so a frame must leave at least one word free.
    if frame_samples >= FIFO_SAMPLES:
        raise cv.Invalid(
            f"A frame of {frame_samples} samples must be smaller than the "
            f"{FIFO_SAMPLES}-sample FIFO"
        )
    mode = config.setdefault(CONF_ACQUISITION_MODE, "triggered")
    if mode == "continuous":
        # The next frame fills the FIFO while the current one is read.
        if frame_samples >= FIFO_SAMPLES // 2:
            raise cv.Invalid(
                f"A continuous frame of {frame_samples} samples must be smaller than half the "
                f"{FIFO_SAMPLES}-sample FIFO; use 'triggered' mode"
            )
        if CONF_TIMEOUT not in config:
            raise cv.Invalid(
                f"'{CONF_TIMEOUT}' is required in continuous mode; set it above the "
                "profile's frame period",
                path=[CONF_TIMEOUT],
            )
    threshold = config.setdefault(CONF_FIFO_THRESHOLD, frame_samples)
    if threshold % 2 or not 2 <= threshold <= frame_samples:
        raise cv.Invalid(
            f"'{CONF_FIFO_THRESHOLD}' must be an even sample count between 2 and the "
            f"frame size ({frame_samples})",
            path=[CONF_FIFO_THRESHOLD],
        )
    if CONF_REGISTERS_FILE in config:
        config[CONF_REGISTERS] = validate_register_list(config[CONF_REGISTERS_FILE])
    config.setdefault(CONF_USE_INTERRUPT, True)
    config.setdefault(CONF_TIMEOUT, cv.positive_time_period_milliseconds("500ms"))
    config.setdefault(CONF_FRAME_LOG_INTERVAL, 1)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BGT60TR13C),
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint32),
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
            cv.Exclusive(CONF_REGISTERS, "registers"): cv.All(
                cv.ensure_list(cv.hex_uint32_t), validate_register_list
            ),
            cv.Exclusive(CONF_REGISTERS_FILE, "registers"): validate_registers_file,
            cv.Optional(CONF_NUM_SAMPLES_PER_CHIRP): cv.int_range(min=1, max=1024),
            cv.Optional(CONF_NUM_CHIRPS_PER_FRAME): cv.int_range(min=1, max=4096),
            cv.Optional(CONF_NUM_RX_ANTENNAS): cv.int_range(min=1, max=3),
            cv.Optional(CONF_FIFO_THRESHOLD): cv.int_range(min=2, max=FIFO_SAMPLES),
            cv.Optional(CONF_ACQUISITION_MODE): cv.enum(ACQUISITION_MODES, lower=True),
            cv.Optional(CONF_USE_INTERRUPT): cv.boolean,
            cv.Optional(CONF_TIMEOUT): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FRAME_LOG_INTERVAL): cv.uint32_t,
        }
    )
    .extend(cv.polling_component_schema("2s"))
    .extend(spi.spi_device_schema(True, "1MHz", "MODE0")),
    _validate_acquisition,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    irq = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
    cg.add(var.set_reset_pin(reset))
    cg.add(var.set_irq_pin(irq))
    if CONF_REGISTERS not in config:
        return
    words = config[CONF_REGISTERS]
    registers = cg.static_const_array(
        config[CONF_RAW_DATA_ID],
        cg.ArrayInitializer(*[cg.RawExpression(f"0x{w:08X}UL") for w in words]),
    )
    cg.add(var.set_registers(registers, len(words)))
    cg.add(
        var.set_frame_geometry(
            config[CONF_NUM_SAMPLES_PER_CHIRP],
            config[CONF_NUM_CHIRPS_PER_FRAME],
            config[CONF_NUM_RX_ANTENNAS],
        )
    )
    cg.add(var.set_fifo_threshold(config[CONF_FIFO_THRESHOLD]))
    cg.add(var.set_acquisition_mode(ACQUISITION_MODES[config[CONF_ACQUISITION_MODE]]))
    cg.add(var.set_use_interrupt(config[CONF_USE_INTERRUPT]))
    cg.add(var.set_frame_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_frame_log_interval(config[CONF_FRAME_LOG_INTERVAL]))
