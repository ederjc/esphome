import esphome.codegen as cg
from esphome.components.zephyr import (
    zephyr_add_overlay,
    zephyr_add_prj_conf,
    zephyr_setup_preferences,
)
from esphome.components.zephyr.const import CONF_RUNNER
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
    CONF_SOURCE,
    ThreadModel,
    Toolchain,
)
from esphome.types import ConfigType

from . import infineon_psoc6_family

_BOARD = "cy8ckit_062s2_ai"

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
    gpio_port_width=8,
    gpio_node_prefix="gpio_prt",
    uart_node_labels={"UART0": "scb5", "UART1": "scb1"},
    watchdog_max_timeout_ms=6000,
)

# The CM0+ companion image is linked at the start of the application partition.
# Leave eight 8 KiB sectors at the end of internal flash for Zephyr settings/ZMS.
_CORE_FLASH_OVERLAY = """
/ {
  chosen {
    zephyr,code-partition = &slot0_partition;
    zephyr,settings-partition = &storage_partition;
  };
};

&{/flash-controller@40240000} {
  ranges;
};

&flash0 {
  #address-cells = <1>;
  #size-cells = <1>;
  ranges = <0x0 0x10000000 0x200000>;

  partitions {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;

    slot0_partition: partition@0 {
      compatible = "zephyr,mapped-partition";
      label = "image-0";
      reg = <0x000000 0x1f0000>;
    };

    storage_partition: partition@1f0000 {
      compatible = "zephyr,mapped-partition";
      label = "storage";
      reg = <0x1f0000 0x010000>;
    };
  };
};
"""

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
    advanced = framework.get(CONF_ADVANCED, {})
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
    if config[CONF_BOARD] == _BOARD:
        zephyr_add_overlay(_CORE_FLASH_OVERLAY)
