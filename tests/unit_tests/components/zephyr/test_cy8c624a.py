"""Tests for the CY8C624A variant's MCUboot support."""

from __future__ import annotations

from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest

from esphome.components.infineon.zephyr_variants import VARIANT_MODULES, cy8c624a
from esphome.components.zephyr import HexValue, zephyr_config_schema
from esphome.components.zephyr.const import (
    BOOTLOADER_MCUBOOT,
    CONF_BOOTLOADER,
    KEY_BOOTLOADER,
    KEY_OVERLAY,
    KEY_PRJ_CONF,
    KEY_SYSBUILD_CONF,
    KEY_ZEPHYR,
)
from esphome.const import (
    CONF_ADVANCED,
    CONF_BOARD,
    CONF_FRAMEWORK,
    CONF_OTA,
    KEY_CORE,
    KEY_FRAMEWORK_VERSION,
    KEY_TARGET_PLATFORM,
    PLATFORM_INFINEON,
)
from esphome.core import CORE
from tests.unit_tests.components.zephyr.zephyr_state import (
    empty_zephyr_data,
    minimal_variant_config,
)

_BOARD = "cy8ckit_062s2_ai"


def _state(tmp_path: Path, bootloader: str) -> None:
    CORE.data[KEY_ZEPHYR] = empty_zephyr_data(variant="CY8C624A", board=_BOARD)
    CORE.data[KEY_ZEPHYR][KEY_BOOTLOADER] = bootloader
    CORE.data[KEY_CORE] = {
        KEY_FRAMEWORK_VERSION: "4.5.0",
        KEY_TARGET_PLATFORM: PLATFORM_INFINEON,
    }
    CORE.config_path = tmp_path / "test.yaml"


def _to_code(tmp_path: Path, bootloader: str, ota: bool = True) -> dict[str, Any]:
    _state(tmp_path, bootloader)
    CORE.config = {CONF_OTA: [{}]} if ota else {}
    with patch.object(cy8c624a.cg, "add_define"):
        cy8c624a.to_code({CONF_BOARD: _BOARD})
    return CORE.data[KEY_ZEPHYR]


@pytest.mark.parametrize(
    ("advanced", "expected"),
    [({}, ""), ({CONF_BOOTLOADER: BOOTLOADER_MCUBOOT}, BOOTLOADER_MCUBOOT)],
)
def test_config_schema_makes_mcuboot_opt_in(
    tmp_path: Path, advanced: dict[str, str], expected: str
) -> None:
    CORE.data[KEY_CORE] = {}
    config = minimal_variant_config("CY8C624A", tmp_path)
    config[CONF_FRAMEWORK][CONF_ADVANCED] = advanced
    zephyr_config_schema(config, VARIANT_MODULES, PLATFORM_INFINEON)
    assert CORE.data[KEY_ZEPHYR][KEY_BOOTLOADER] == expected


def test_without_mcuboot_the_app_owns_the_flash(tmp_path: Path) -> None:
    data = _to_code(tmp_path, "")
    assert "reg = <0x000000 0x1f0000>;" in data[KEY_OVERLAY][""]
    assert "mcuboot" not in data[KEY_OVERLAY]
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT" not in data[KEY_SYSBUILD_CONF]


def test_mcuboot_partitions_both_images_alike(tmp_path: Path) -> None:
    data = _to_code(tmp_path, BOOTLOADER_MCUBOOT)
    app, boot = data[KEY_OVERLAY][""], data[KEY_OVERLAY]["mcuboot"]
    for reg in (
        "reg = <0x000000 0x020000>;",
        "reg = <0x020000 0x0e4000>;",
        "reg = <0x104000 0x0e8000>;",
        "reg = <0x1f0000 0x010000>;",
    ):
        assert reg in app
        assert reg in boot
    assert "zephyr,code-partition = &slot0_partition;" in app
    assert "&slot0_partition" not in boot.split("partitions {")[0]
    assert data[KEY_SYSBUILD_CONF]["SB_CONFIG_BOOTLOADER_MCUBOOT"] == (True, True)
    assert data[KEY_SYSBUILD_CONF]["SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256"] == (
        True,
        True,
    )


def test_mcuboot_and_uploader_share_the_logical_geometry(tmp_path: Path) -> None:
    data = _to_code(tmp_path, BOOTLOADER_MCUBOOT)
    boot, app = data[KEY_PRJ_CONF]["mcuboot"], data[KEY_PRJ_CONF][""]
    assert boot["CONFIG_MCUBOOT_LOGICAL_SECTOR_SIZE"][0] == HexValue(0x4000)
    assert boot["CONFIG_BOOT_MAX_IMG_SECTORS_AUTO"][0] is False
    assert boot["CONFIG_BOOT_MAX_IMG_SECTORS"][0] == 58
    assert app["CONFIG_IMG_CUSTOM_SECTOR_SIZE"][0] == 0x4000
    assert app["CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS"][0] == "--max-sectors 58"


def test_mcuboot_without_ota_has_no_uploader_geometry(tmp_path: Path) -> None:
    data = _to_code(tmp_path, BOOTLOADER_MCUBOOT, ota=False)
    assert "CONFIG_IMG_CUSTOM_SECTOR_SIZE" not in data[KEY_PRJ_CONF][""]
