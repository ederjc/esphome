"""Code shared by Infineon PSoC 6 variants."""

from esphome.components.zephyr import zephyr_add_prj_conf
from esphome.types import ConfigType

FAMILY = "infineon_psoc6"
ADDITIONAL_FRAMEWORK_PROJECTS = frozenset({"hal_infineon"})
CPU_ARCH = "arm"
DTS_HAL_MODULES = ("hal_infineon",)
PIN_NOTATION = "dotted"
USB_UART_TYPE = None
HAS_POWEROFF = False


def bootloader(advanced: ConfigType) -> str:
    return ""


def to_code(config: ConfigType) -> None:
    zephyr_add_prj_conf("CPP", True)
    zephyr_add_prj_conf("REQUIRES_FULL_LIBCPP", True)
    zephyr_add_prj_conf("FPU", True)
    zephyr_add_prj_conf("REBOOT", True)
