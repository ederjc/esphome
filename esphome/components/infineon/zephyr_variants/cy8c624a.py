import esphome.codegen as cg
from esphome.components.zephyr import (
    HexValue,
    zephyr_add_overlay,
    zephyr_add_prj_conf,
    zephyr_add_prj_conf_min,
    zephyr_add_sysbuild_conf,
    zephyr_data,
    zephyr_setup_preferences,
)
from esphome.components.zephyr.const import (
    ADVANCED_SCHEMA,
    BOOTLOADER_MCUBOOT,
    BOOTLOADER_SCHEMA,
    CONF_RUNNER,
    KEY_BOOTLOADER,
)
from esphome.components.zephyr.partitions import BootLayout
from esphome.components.zephyr.variants import (
    ZEPHYR_MAINLINE,
    ZephyrVariant,
    resolve_variant,
    set_variant_core_data,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADVANCED,
    CONF_BOARD,
    CONF_FRAMEWORK,
    CONF_OTA,
    CONF_SOURCE,
    CONF_WIFI,
    ThreadModel,
    Toolchain,
)
from esphome.core import CORE
from esphome.types import ConfigType

from . import infineon_psoc6_family

_BOARD = infineon_psoc6_family.AI_KIT_BOARD

_ADVANCED_SCHEMA: cv.Schema = ADVANCED_SCHEMA.extend(BOOTLOADER_SCHEMA)

VARIANT = ZephyrVariant(
    name="CY8C624A",
    family_module=infineon_psoc6_family,
    manifest=ZEPHYR_MAINLINE,
    boards=[_BOARD],
    valid_toolchains=(Toolchain.ZEPHYR,),
    toolchain="arm-zephyr-eabi",
    blobs=(
        "hal_infineon",
        r"img/cat1cm0p/COMPONENT_CM0P_SLEEP/psoc6_02_cm0p_sleep\.bin$",
        ".blobs_psoc6_02_cm0p_sleep_ready",
    ),
    boot=BootLayout(0),
    # Rows are 512 bytes, so MCUboot needs logical sectors, which only offset supports.
    swap_methods=frozenset({"offset"}),
    gpio_port_width=8,
    gpio_node_prefix="gpio_prt",
    uart_node_labels={"UART0": "scb5", "UART1": "scb1"},
    watchdog_max_timeout_ms=6000,
    transports=frozenset({"wifi"}),
    transport_drivers={"wifi": ("WIFI_AIROC", "airoc_wifi")},
    transport_blobs={
        "wifi": (
            "hal_infineon",
            (
                r"img/whd/resources/(firmware/COMPONENT_43439/43439A0\.bin|"
                r"clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0\.clm_blob)$"
            ),
            ".blobs_cyw43439_murata_1yn_ready",
        ),
    },
)

_FLASH_PARTITIONS = """
&{{/flash-controller@40240000}} {{
  ranges;
}};

&flash0 {{
  #address-cells = <1>;
  #size-cells = <1>;
  ranges = <0x0 0x10000000 0x200000>;

  partitions {{
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;
{partitions}
    storage_partition: partition@1f0000 {{
      compatible = "zephyr,mapped-partition";
      label = "storage";
      reg = <0x1f0000 0x010000>;
    }};
  }};
}};
"""

# The CM0+ companion image is linked at the start of the application partition.
# Leave eight 8 KiB sectors at the end of internal flash for Zephyr settings/ZMS.
_CORE_FLASH_OVERLAY = _FLASH_PARTITIONS.format(
    partitions="""
    slot0_partition: partition@0 {
      compatible = "zephyr,mapped-partition";
      label = "image-0";
      reg = <0x000000 0x1f0000>;
    };
"""
)

# MCUboot carries the CM0+ image at 0x0 and is started at 0x2000 by it. Slot 1 is
# one 16 KiB logical sector larger than slot 0 for swap-using-offset; the gap
# after it keeps storage at the same address as without MCUboot.
_MCUBOOT_FLASH_OVERLAY = _FLASH_PARTITIONS.format(
    partitions="""
    boot_partition: partition@0 {
      compatible = "zephyr,mapped-partition";
      label = "mcuboot";
      reg = <0x000000 0x020000>;
    };

    slot0_partition: partition@20000 {
      compatible = "zephyr,mapped-partition";
      label = "image-0";
      reg = <0x020000 0x0e4000>;
    };

    slot1_partition: partition@104000 {
      compatible = "zephyr,mapped-partition";
      label = "image-1";
      reg = <0x104000 0x0e8000>;
    };
"""
)

_APP_CHOSEN_OVERLAY = """
/ {
  chosen {
    zephyr,code-partition = &slot0_partition;
    zephyr,settings-partition = &storage_partition;
  };
};
"""

# MCUboot swaps 16 KiB logical sectors: 512-byte rows would need 1856 sectors of
# swap status. 58 covers slot 1. Rows are erased and programmed in one step, so the
# 512-byte write block is also MCUboot's trailer alignment (MCUBOOT_BOOT_MAX_ALIGN).
_MCUBOOT_LOGICAL_SECTOR = 0x4000
_MCUBOOT_MAX_SECTORS = 0xE8000 // _MCUBOOT_LOGICAL_SECTOR


# MXCRYPTO v2 is at CRYPTO_BASE in the CY8C624ABZI-S2D44 device header.
_ENTROPY_OVERLAY = """
/ {
  chosen {
    zephyr,entropy = &psoc6_trng;
  };
};

&{/soc} {
  crypto@40100000 {
    compatible = "infineon,mxcrypto";
    reg = <0x40100000 0x10000>;

    psoc6_trng: trng {
      compatible = "infineon,mxcrypto-trng";
    };
  };
};
"""

# CY8CKIT-062S2-AI BSP design.modus wiring. P2.0–P2.5 use SDHC0 on
# HSIOM active function 14, as on Zephyr's same-SoC CY8CPROTO-062-4343W.
_WIFI_OVERLAY = """
#include <zephyr/dt-bindings/pinctrl/ifx_cat1-pinctrl.h>

&pinctrl {
  wifi_sdio_d0: wifi-sdio-d0 {
    pinmux = <DT_CAT1_PINMUX(2, 0, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
  wifi_sdio_d1: wifi-sdio-d1 {
    pinmux = <DT_CAT1_PINMUX(2, 1, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
  wifi_sdio_d2: wifi-sdio-d2 {
    pinmux = <DT_CAT1_PINMUX(2, 2, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
  wifi_sdio_d3: wifi-sdio-d3 {
    pinmux = <DT_CAT1_PINMUX(2, 3, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
  wifi_sdio_cmd: wifi-sdio-cmd {
    pinmux = <DT_CAT1_PINMUX(2, 4, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
  wifi_sdio_clk: wifi-sdio-clk {
    pinmux = <DT_CAT1_PINMUX(2, 5, HSIOM_SEL_ACT_14)>;
    drive-push-pull;
    input-enable;
  };
};

&gpio_prt2 { status = "okay"; };
&gpio_prt4 { status = "okay"; };

&clk_hf4 {
  status = "okay";
  clock-div = <1>;
  clocks = <&fll0>;
};

&sdhc0 {
  status = "okay";
  pinctrl-0 = <&wifi_sdio_cmd &wifi_sdio_clk &wifi_sdio_d0
               &wifi_sdio_d1 &wifi_sdio_d2 &wifi_sdio_d3>;
  pinctrl-names = "default";
  bus-width = <4>;
  max-bus-freq = <50000000>;
  min-bus-freq = <400000>;
  power-delay-ms = <1000>;
  no-1-8-v;
  clocks = <&clk_hf4>;

  airoc_wifi: airoc-wifi {
    compatible = "infineon,airoc-wifi";
    wifi-reg-on-gpios = <&gpio_prt2 6 GPIO_ACTIVE_HIGH>;
    wifi-host-wake-gpios = <&gpio_prt4 1 GPIO_ACTIVE_HIGH>;
  };
};
"""


def config_schema(config: ConfigType) -> ConfigType:
    config = dict(config)
    config.setdefault(CONF_BOARD, _BOARD)
    framework = config[CONF_FRAMEWORK]
    if (source := framework.get(CONF_SOURCE)) is None:
        raise cv.Invalid(
            "CY8C624A requires a Zephyr source with PSoC 6 HWINFO and MXCRYPTO "
            "entropy support. Use the source in the Infineon platform documentation.",
            [CONF_FRAMEWORK, CONF_SOURCE],
        )
    advanced = framework[CONF_ADVANCED] = _ADVANCED_SCHEMA(
        framework.get(CONF_ADVANCED, {})
    )
    resolved = resolve_variant(VARIANT, config, "mainline PSoC 6 support")
    set_variant_core_data(
        resolved,
        config,
        bootloader=infineon_psoc6_family.bootloader(advanced),
        sdk_source=source,
        runner=advanced.get(CONF_RUNNER),
    )
    return config


def to_code(config: ConfigType) -> None:
    cg.add_define("ESPHOME_BOARD", config[CONF_BOARD])
    cg.add_define("ESPHOME_VARIANT", VARIANT.name)
    cg.add_define(ThreadModel.MULTI_ATOMICS)
    zephyr_add_prj_conf("HWINFO", True)
    zephyr_add_prj_conf("ENTROPY_GENERATOR", True)
    zephyr_add_overlay(_ENTROPY_OVERLAY)
    zephyr_add_prj_conf("USE_DT_CODE_PARTITION", True)
    # Zephyr NVS cannot mount a flash device with 512-byte minimum writes.
    zephyr_setup_preferences("ZMS")
    zephyr_add_prj_conf("ZMS_CUSTOMIZE_BLOCK_SIZE", True)
    zephyr_add_prj_conf("ZMS_CUSTOM_BLOCK_SIZE", 512)
    zephyr_add_prj_conf("ZMS_NO_DOUBLE_WRITE", True)
    zephyr_add_prj_conf("SETTINGS_ZMS_SECTOR_SIZE_MULT", 16)
    if zephyr_data()[KEY_BOOTLOADER] == BOOTLOADER_MCUBOOT:
        _mcuboot_to_code()
    elif config[CONF_BOARD] == _BOARD:
        zephyr_add_overlay(_APP_CHOSEN_OVERLAY + _CORE_FLASH_OVERLAY)
    if config[CONF_BOARD] == _BOARD and CONF_WIFI in CORE.config:
        zephyr_add_overlay(_WIFI_OVERLAY)
        zephyr_add_prj_conf("CYW43439", True)
        zephyr_add_prj_conf("CYW43439_MURATA_1YN", True)
        # CYW43439 startup allocations exceed the 15 KB used by the 4343W board.
        zephyr_add_prj_conf_min("HEAP_MEM_POOL_SIZE", 32 * 1024)


def _mcuboot_to_code() -> None:
    zephyr_add_sysbuild_conf("BOOTLOADER_MCUBOOT", True)
    # sysbuild's own BOOT_SIGNATURE_TYPE choice overrides a per-image setting.
    zephyr_add_sysbuild_conf("BOOT_SIGNATURE_TYPE_ECDSA_P256", True)
    zephyr_add_overlay(_APP_CHOSEN_OVERLAY + _MCUBOOT_FLASH_OVERLAY)
    zephyr_add_overlay(_MCUBOOT_FLASH_OVERLAY, image="mcuboot")
    zephyr_add_prj_conf(
        "MCUBOOT_LOGICAL_SECTOR_SIZE",
        HexValue(_MCUBOOT_LOGICAL_SECTOR),
        image="mcuboot",
    )
    zephyr_add_prj_conf("MCUBOOT_VERIFY_LOGICAL_SECTORS", True, image="mcuboot")
    zephyr_add_prj_conf("BOOT_MAX_IMG_SECTORS_AUTO", False, image="mcuboot")
    zephyr_add_prj_conf("BOOT_MAX_IMG_SECTORS", _MCUBOOT_MAX_SECTORS, image="mcuboot")
    if CORE.config.get(CONF_OTA):
        # Stage uploads one logical sector into slot 1, where MCUboot expects them.
        zephyr_add_prj_conf("IMG_CUSTOM_SECTOR_SIZE", _MCUBOOT_LOGICAL_SECTOR)
    # imgtool otherwise checks the image against a 128-sector trailer.
    zephyr_add_prj_conf(
        "MCUBOOT_EXTRA_IMGTOOL_ARGS", f"--max-sectors {_MCUBOOT_MAX_SECTORS}"
    )
