from unittest.mock import PropertyMock, patch

import pytest

from esphome.components import audio
from esphome.components.zephyr import zephyr_set_core_data
from esphome.components.zephyr.const import BOOTLOADER_NONE, KEY_BOOTLOADER
from esphome.components.zephyr_dmic import microphone as dmic
import esphome.config_validation as cv
from esphome.const import CONF_BOARD
from esphome.core import CORE


@pytest.fixture(autouse=True)
def board():
    CORE.config = {}
    zephyr_set_core_data(
        {CONF_BOARD: "cy8ckit_062s2_ai", KEY_BOOTLOADER: BOOTLOADER_NONE}
    )


def test_stereo_stream_limits_match_hardware():
    config = dmic.CONFIG_SCHEMA(
        {"device": "pdm0", "sample_rate": 15625, "num_channels": 2}
    )
    assert config["min_channels"] == config["max_channels"] == 2
    assert config["min_sample_rate"] == config["max_sample_rate"] == 15625
    assert config["bits_per_sample"] == 16


@pytest.mark.parametrize(
    "override",
    [
        {"num_channels": 1},
        {"sample_rate": 16000},
        {"device": "pdm1"},
        {"bits_per_sample": 24},
    ],
)
def test_board_rejects_unimplemented_formats(override):
    with pytest.raises(cv.Invalid):
        dmic.CONFIG_SCHEMA(
            {"device": "pdm0", "sample_rate": 15625, "num_channels": 2, **override}
        )


def test_other_drivers_are_not_assumed_to_have_synchronous_stop_or_metadata():
    with (
        patch.object(dmic, "zephyr_data", return_value={CONF_BOARD: "other_board"}),
        pytest.raises(cv.Invalid, match="synchronous PDMv1"),
    ):
        dmic.CONFIG_SCHEMA({"device": "dmic", "sample_rate": 16000, "num_channels": 1})


def test_two_yaml_owners_of_resolved_pdm0_are_rejected():
    config = dmic.CONFIG_SCHEMA(
        {
            "id": "first_backend",
            "device": "pdm0",
            "sample_rate": 15625,
            "num_channels": 2,
        }
    )
    duplicate = dmic.CONFIG_SCHEMA(
        {
            "id": "second_backend",
            "device": "pdm0",
            "sample_rate": 15625,
            "num_channels": 2,
        }
    )
    with (
        patch.object(dmic.fv, "platform_entries", return_value=[config, duplicate]),
        pytest.raises(cv.Invalid, match="one microphone backend"),
    ):
        dmic.FINAL_VALIDATE_SCHEMA(config)


def test_aliases_cannot_bypass_the_single_supported_node_contract():
    with pytest.raises(cv.Invalid, match="requires pdm0"):
        dmic.CONFIG_SCHEMA(
            {"device": "pdm_alias", "sample_rate": 15625, "num_channels": 2}
        )


@pytest.mark.parametrize(
    "interval,accepted", [(16, True), (20, True), (21, False), (80, False)]
)
def test_poll_interval_reserves_processing_and_slab_headroom(interval, accepted):
    config = dmic.CONFIG_SCHEMA(
        {"device": "pdm0", "sample_rate": 15625, "num_channels": 2}
    )
    with (
        patch.object(dmic.fv, "platform_entries", return_value=[config]),
        patch.object(
            dmic,
            "get_loop_interval",
            return_value=cv.positive_time_period_milliseconds(f"{interval}ms"),
        ),
    ):
        if accepted:
            dmic.FINAL_VALIDATE_SCHEMA(config)
        else:
            with pytest.raises(cv.Invalid, match="headroom"):
                dmic.FINAL_VALIDATE_SCHEMA(config)


@pytest.mark.asyncio
async def test_zephyr_pcm_does_not_load_esp32_resources_or_accept_codecs():
    with (
        patch.object(
            type(CORE),
            "using_toolchain_zephyr",
            new_callable=PropertyMock,
            return_value=True,
        ),
        patch.object(audio, "request_http_client") as http,
        patch.object(audio, "add_idf_component") as idf,
    ):
        assert audio.CONFIG_SCHEMA({}) == {}
        assert audio.AUTO_LOAD() == []
        await audio.to_code({})
        http.assert_not_called()
        idf.assert_not_called()
        with pytest.raises(cv.Invalid, match="codecs are not supported"):
            audio.CONFIG_SCHEMA({"codecs": {"wav": {}}})


@pytest.mark.asyncio
async def test_esp32_retains_idf_audio_and_codec_resources():
    with (
        patch.object(
            type(CORE),
            "using_toolchain_zephyr",
            new_callable=PropertyMock,
            return_value=False,
        ),
        patch.object(
            type(CORE),
            "target_platform",
            new_callable=PropertyMock,
            return_value="esp32",
        ),
        patch.object(audio, "request_http_client") as http,
        patch.object(audio, "require_certificate_bundle") as certificates,
        patch.object(audio, "add_idf_component") as idf,
    ):
        config = audio.CONFIG_SCHEMA({"codecs": {"wav": {}}})
        assert audio.AUTO_LOAD() == ["ring_buffer"]
        await audio.to_code(config)
        http.assert_called_once()
        certificates.assert_called_once()
        idf.assert_any_call(name="esphome/esp-audio-libs", ref="4.1.0")
        idf.assert_any_call(name="esphome/micro-wav", ref="0.2.0")
