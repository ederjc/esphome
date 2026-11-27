"""Code shared by Infineon PSoC 6 variants."""

from esphome.components.zephyr import zephyr_add_prj_conf
import esphome.config_validation as cv
from esphome.types import ConfigType

FAMILY = "infineon_psoc6"
AI_KIT_BOARD = "cy8ckit_062s2_ai"
ADDITIONAL_FRAMEWORK_PROJECTS = frozenset({"hal_infineon"})
CPU_ARCH = "arm"
DTS_HAL_MODULES = ("hal_infineon",)
PIN_NOTATION = "dotted"
USB_UART_TYPE = None
HAS_POWEROFF = False


def wifi_check_board(board: str) -> None:
    if board != AI_KIT_BOARD:
        raise cv.Invalid(f"Wi-Fi is only supported on the {AI_KIT_BOARD!r} board")


def bootloader(advanced: ConfigType) -> str:
    return ""


def to_code(config: ConfigType) -> None:
    zephyr_add_prj_conf("CPP", True)
    zephyr_add_prj_conf("REQUIRES_FULL_LIBCPP", True)
    zephyr_add_prj_conf("FPU", True)
    zephyr_add_prj_conf("REBOOT", True)
