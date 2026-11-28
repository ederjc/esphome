"""Tests for the infineon_bgt60tr13c register-list parsing and validation."""

from __future__ import annotations

from collections.abc import Generator

import pytest

from esphome.components.infineon_bgt60tr13c import (
    CONF_ACQUISITION_MODE,
    CONF_FIFO_THRESHOLD,
    CONF_INFINEON_BGT60TR13C_ID,
    CONF_NUM_CHIRPS_PER_FRAME,
    CONF_NUM_RX_ANTENNAS,
    CONF_NUM_SAMPLES_PER_CHIRP,
    CONF_REGISTERS,
    _validate_acquisition,
    parse_register_text,
    validate_register_word,
)
from esphome.components.infineon_bgt60tr13c.sensor import _final_validate
from esphome.config import Config
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIMEOUT
from esphome.core import ID, TimePeriodMilliseconds
import esphome.final_validate as fv


def test_parse_register_text_reads_first_initializer_only() -> None:
    text = """
    #define XENSIV_BGT60TRXX_CONF_NUM_REGS (2)
    /* block comment 0xDEADBEEF */
    static uint32_t regs[] = {
        0x1b000000UL, // inline comment 0x1
        0x1d000123
    };
    static uint32_t other[] = { 0x1f000000 };
    """
    assert parse_register_text(text) == [0x1B000000, 0x1D000123]


def test_parse_register_text_accepts_plain_list() -> None:
    assert parse_register_text("0x19000000\n1B000000, 0x1d000000;") == [
        0x19000000,
        0x1B000000,
        0x1D000000,
    ]


def test_parse_register_text_rejects_non_hex_token() -> None:
    with pytest.raises(cv.Invalid, match="not a hexadecimal word"):
        parse_register_text("{ 0x19000000, foo }")


@pytest.mark.parametrize(
    ("word", "message"),
    [
        (0x18000000, "write flag"),
        (0xC1000000, "only addresses below 0x60"),
    ],
)
def test_validate_register_word_rejects_invalid(word: int, message: str) -> None:
    with pytest.raises(cv.Invalid, match=message):
        validate_register_word(word)


def _config(**overrides: int) -> dict:
    config = {
        CONF_REGISTERS: [0x19000000],
        CONF_NUM_SAMPLES_PER_CHIRP: 128,
        CONF_NUM_CHIRPS_PER_FRAME: 16,
        CONF_NUM_RX_ANTENNAS: 1,
    }
    config.update(overrides)
    return config


def test_validate_acquisition_defaults_threshold_to_frame_size() -> None:
    config = _validate_acquisition(_config())
    assert config[CONF_FIFO_THRESHOLD] == 2048
    assert config["acquisition_mode"] == "triggered"


def test_validate_acquisition_requires_registers_for_geometry() -> None:
    with pytest.raises(cv.Invalid, match="requires 'registers'"):
        _validate_acquisition({CONF_NUM_SAMPLES_PER_CHIRP: 128})


@pytest.mark.parametrize(
    ("overrides", "message"),
    [
        ({CONF_NUM_SAMPLES_PER_CHIRP: 3, CONF_NUM_CHIRPS_PER_FRAME: 1}, "24-bit"),
        ({CONF_NUM_CHIRPS_PER_FRAME: 129}, "smaller than"),
        ({CONF_NUM_CHIRPS_PER_FRAME: 128}, "smaller than"),
        ({CONF_FIFO_THRESHOLD: 4096}, "even sample count"),
        ({CONF_FIFO_THRESHOLD: 3}, "even sample count"),
    ],
)
def test_validate_acquisition_rejects_bad_geometry(
    overrides: dict, message: str
) -> None:
    with pytest.raises(cv.Invalid, match=message):
        _validate_acquisition(_config(**overrides))


def test_validate_acquisition_accepts_frame_just_below_fifo() -> None:
    config = _validate_acquisition(
        _config(**{CONF_NUM_SAMPLES_PER_CHIRP: 1022, CONF_NUM_CHIRPS_PER_FRAME: 16})
    )
    assert config[CONF_FIFO_THRESHOLD] == 16352


def _continuous(**overrides: object) -> dict:
    return _config(
        **{
            CONF_ACQUISITION_MODE: "continuous",
            CONF_TIMEOUT: TimePeriodMilliseconds(milliseconds=1500),
            **overrides,
        }
    )


def test_validate_acquisition_accepts_continuous_frame_below_half_fifo() -> None:
    config = _validate_acquisition(
        _continuous(**{CONF_NUM_SAMPLES_PER_CHIRP: 1024, CONF_NUM_CHIRPS_PER_FRAME: 7})
    )
    assert config[CONF_FIFO_THRESHOLD] == 7168


def test_validate_acquisition_rejects_continuous_half_fifo_frame() -> None:
    with pytest.raises(cv.Invalid, match="smaller than half"):
        _validate_acquisition(
            _continuous(
                **{CONF_NUM_SAMPLES_PER_CHIRP: 1024, CONF_NUM_CHIRPS_PER_FRAME: 8}
            )
        )


def test_validate_acquisition_requires_timeout_in_continuous_mode() -> None:
    config = _continuous()
    del config[CONF_TIMEOUT]
    with pytest.raises(cv.Invalid, match="required in continuous mode"):
        _validate_acquisition(config)


@pytest.fixture
def radar_full_config() -> Generator[Config]:
    full = Config()
    radar = {CONF_ID: ID("radar", is_declaration=True)}
    full["infineon_bgt60tr13c"] = [radar]
    full.declare_ids.append((radar[CONF_ID], ["infineon_bgt60tr13c", 0, CONF_ID]))
    token = fv.full_config.set(full)
    yield full
    fv.full_config.reset(token)


def _sensor_config() -> dict:
    return {CONF_INFINEON_BGT60TR13C_ID: ID("radar")}


def test_sensor_final_validate_rejects_diagnostic_parent(
    radar_full_config: Config,
) -> None:
    with pytest.raises(cv.Invalid, match="need acquisition"):
        _final_validate(_sensor_config())


def test_sensor_final_validate_accepts_acquisition_parent(
    radar_full_config: Config,
) -> None:
    radar_full_config["infineon_bgt60tr13c"][0][CONF_REGISTERS] = [0x19000000]
    _final_validate(_sensor_config())
