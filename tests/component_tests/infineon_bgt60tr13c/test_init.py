"""Tests for the infineon_bgt60tr13c register-list parsing and validation."""

from __future__ import annotations

import pytest

from esphome.components.infineon_bgt60tr13c import (
    CONF_FIFO_THRESHOLD,
    CONF_NUM_CHIRPS_PER_FRAME,
    CONF_NUM_RX_ANTENNAS,
    CONF_NUM_SAMPLES_PER_CHIRP,
    CONF_REGISTERS,
    _validate_acquisition,
    parse_register_text,
    validate_register_word,
)
import esphome.config_validation as cv


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
        ({CONF_NUM_CHIRPS_PER_FRAME: 129}, "exceeds"),
        ({CONF_FIFO_THRESHOLD: 4096}, "even sample count"),
        ({CONF_FIFO_THRESHOLD: 3}, "even sample count"),
    ],
)
def test_validate_acquisition_rejects_bad_geometry(
    overrides: dict, message: str
) -> None:
    with pytest.raises(cv.Invalid, match=message):
        _validate_acquisition(_config(**overrides))
