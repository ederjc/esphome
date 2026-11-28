from pathlib import Path
from unittest.mock import PropertyMock, patch

import pytest
import yaml

from esphome.components.sound_level import sensor as sound_level
import esphome.config_validation as cv
from esphome.core import CORE


def source(channels=None):
    config = {"microphone": "bench_mic"}
    if channels is not None:
        config["channels"] = channels
    return {"microphone": config, "passive": True, "peak": {"name": "Peak"}}


@pytest.mark.parametrize("channels", [None, [0], [1]])
def test_zephyr_requires_one_selected_channel(channels):
    with patch.object(
        type(CORE),
        "using_toolchain_zephyr",
        new_callable=PropertyMock,
        return_value=True,
    ):
        config = sound_level.CONFIG_SCHEMA(source(channels))
    assert config["microphone"]["channels"] == ([0] if channels is None else channels)
    assert config["microphone"]["bits_per_sample"] == 16


def test_zephyr_rejects_pooled_stereo_instead_of_discarding_a_channel():
    with (
        patch.object(
            type(CORE),
            "using_toolchain_zephyr",
            new_callable=PropertyMock,
            return_value=True,
        ),
        pytest.raises(cv.Invalid, match="one selected microphone channel"),
    ):
        sound_level.CONFIG_SCHEMA(source([0, 1]))


def test_esp32_retains_pooled_stereo_configuration():
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
    ):
        config = sound_level.CONFIG_SCHEMA(source([0, 1]))
    assert config["microphone"]["channels"] == [0, 1]


def test_other_frameworks_are_not_silently_enabled():
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
            return_value="esp8266",
        ),
        pytest.raises(cv.Invalid, match="only available"),
    ):
        sound_level.CONFIG_SCHEMA(source([0]))


@pytest.mark.parametrize("zephyr", [False, True])
def test_output_defaults_distinguish_relative_dbfs_from_esp32_sound_pressure(zephyr):
    with (
        patch.object(
            type(CORE),
            "using_toolchain_zephyr",
            new_callable=PropertyMock,
            return_value=zephyr,
        ),
        patch.object(
            type(CORE),
            "target_platform",
            new_callable=PropertyMock,
            return_value="esp32",
        ),
    ):
        config = sound_level.CONFIG_SCHEMA({**source([0]), "rms": {"name": "RMS"}})
    for output in ("peak", "rms"):
        assert config[output]["unit_of_measurement"] == ("dBFS" if zephyr else "dB")
        if zephyr:
            assert "device_class" not in config[output]
        else:
            assert config[output]["device_class"] == "sound_pressure"


def test_fixture_only_logs_aggregate_values_from_two_passive_mono_sources():
    root = Path(__file__).resolve().parents[4]
    config = yaml.safe_load(
        (root / "tests" / "components" / "sound_level" / "test.zephyr.yaml").read_text()
    )
    assert not (
        {"wifi", "api", "ota", "mqtt", "web_server", "interval"} & config.keys()
    )
    assert len(config["microphone"]) == 1
    assert config["script"][0]["mode"] == "single"
    assert [item["microphone"]["channels"] for item in config["sensor"]] == [[0], [1]]
    for item in config["sensor"]:
        assert item["passive"]
        assert item["microphone"]["gain_factor"] == 1
        for output in ("peak", "rms"):
            assert item[output]["internal"]
            assert "dBFS" in item[output]["on_value"][0]["lambda"]
            assert "ESP_LOGI" in item[output]["on_value"][0]["lambda"]
    boot = config["esphome"]["on_boot"]["then"]
    assert (
        'ESP_LOGI("sl_bench", "BOOT sound_level aggregate-only bench")'
        in boot[0]["lambda"]
    )
    sequence = config["script"][0]["then"]
    assert sequence[0] == {"microphone.stop_capture": "stereo_mic"}
    assert (
        cv.positive_time_period_milliseconds(sequence[1]["delay"]).total_milliseconds
        >= 1000
    )
    repeat = sequence[2]["repeat"]
    assert repeat["count"] == 3
    actions = repeat["then"]
    assert [next(iter(action)) for action in actions] == [
        "lambda",
        "microphone.capture",
        "delay",
        "lambda",
        "microphone.stop_capture",
        "delay",
    ]
    assert (
        '"START run=%u"' in actions[0]["lambda"]
        and "iteration + 1" in actions[0]["lambda"]
    )
    assert (
        '"STOP run=%u"' in actions[3]["lambda"]
        and "iteration + 1" in actions[3]["lambda"]
    )
    duration = cv.positive_time_period_milliseconds(
        config["sensor"][0]["measurement_duration"]
    ).total_milliseconds
    assert (
        cv.positive_time_period_milliseconds(actions[2]["delay"]).total_milliseconds
        >= 6 * duration
    )
    assert (
        cv.positive_time_period_milliseconds(actions[-1]["delay"]).total_milliseconds
        >= 1000
    )
    assert sequence[-1] == {"lambda": 'ESP_LOGI("sl_bench", "COMPLETE runs=3");'}
