import pytest

from esphome.components.bmm350.sensor import CONFIG_SCHEMA
import esphome.config_validation as cv


@pytest.mark.parametrize("address", [0x14, 0x15])
def test_accepts_bmm350_addresses(address):
    config = CONFIG_SCHEMA(
        {
            "address": address,
            "field_strength_x": {"name": "Field X"},
            "temperature": {"name": "Temperature"},
        }
    )
    assert config["address"] == address
    assert config["field_strength_x"]["unit_of_measurement"] == "µT"
    assert config["temperature"]["unit_of_measurement"] == "°C"


@pytest.mark.parametrize("address", [0x10, 0x13, 0x16, 0x68])
def test_rejects_other_sensor_addresses(address):
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA({"address": address})


@pytest.mark.parametrize("interval", ["1ms", "39ms", "0s"])
def test_rejects_polling_faster_than_fixed_odr(interval):
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA({"update_interval": interval})


def test_accepts_odr_boundary():
    config = CONFIG_SCHEMA({"update_interval": "40ms"})
    assert config["update_interval"].total_milliseconds == 40
