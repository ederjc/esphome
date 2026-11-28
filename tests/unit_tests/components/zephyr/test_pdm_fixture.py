"""Regression for deterministic offline bench scheduling, not acoustic claims."""

from pathlib import Path

import yaml

import esphome.config_validation as cv


def test_fixture_has_one_serial_sequencer_with_complete_runs_and_quiet_gaps():
    root = Path(__file__).resolve().parents[4]
    config = yaml.safe_load((root / "ifx" / "pdm-stereo-bench.yaml").read_text())
    assert "interval" not in config
    boot = config["esphome"]["on_boot"]["then"]
    assert [next(iter(action)) for action in boot] == ["lambda", "script.execute"]
    scripts = config["script"]
    assert len(scripts) == 1
    script = scripts[0]
    assert script["mode"] == "single"
    assert boot[-1]["script.execute"] == script["id"]
    sequence = script["then"]
    assert [next(iter(action)) for action in sequence] == [
        "microphone.stop_capture",
        "lambda",
        "repeat",
        "delay",
        "lambda",
    ]
    assert ".stopped()" in sequence[1]["lambda"]
    assert ".complete()" in sequence[-1]["lambda"]
    repeat = sequence[2]["repeat"]
    assert repeat["count"] == 3
    assert [next(iter(action)) for action in repeat["then"]] == ["delay", "if"]
    guarded = repeat["then"][1]["if"]
    assert ".starting()" in guarded["condition"]["lambda"]
    run = guarded["then"]
    assert [next(iter(action)) for action in run] == [
        "microphone.capture",
        "delay",
        "microphone.stop_capture",
        "lambda",
    ]
    assert (
        run[0]["microphone.capture"]
        == run[2]["microphone.stop_capture"]
        == sequence[0]["microphone.stop_capture"]
    )
    assert ".stopped()" in run[-1]["lambda"]

    quiet_ms = cv.positive_time_period_milliseconds(
        repeat["then"][0]["delay"]
    ).total_milliseconds
    run_ms = cv.positive_time_period_milliseconds(run[1]["delay"]).total_milliseconds
    final_quiet_ms = cv.positive_time_period_milliseconds(
        sequence[3]["delay"]
    ).total_milliseconds
    assert quiet_ms >= 1000
    # A five-second delivery window needs first-block/scheduler startup headroom.
    assert run_ms >= 7000
    assert final_quiet_ms >= 1000
    starts = [
        quiet_ms + index * (quiet_ms + run_ms) for index in range(repeat["count"])
    ]
    stops = [start + run_ms for start in starts]
    assert all(starts[index + 1] - stops[index] >= 1000 for index in range(2))
