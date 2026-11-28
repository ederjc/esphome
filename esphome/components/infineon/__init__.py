"""Infineon microcontrollers built with Zephyr."""

from esphome.components import zephyr
from esphome.const import PLATFORM_INFINEON
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.schema_extractors import SCHEMA_EXTRACT, schema_extractor
from esphome.types import ConfigType

from . import gpio as gpio
from .zephyr_variants import VARIANT_MODULES

AUTO_LOAD = ["zephyr", "preferences"]
CODEOWNERS = ["@ederjc"]
DOMAIN = "infineon"
IS_TARGET_PLATFORM = True


@schema_extractor("schema")
def CONFIG_SCHEMA(config: ConfigType) -> ConfigType:
    if config is SCHEMA_EXTRACT:
        return zephyr.zephyr_toolchain_schema(VARIANT_MODULES)
    return zephyr.zephyr_config_schema(config, VARIANT_MODULES, PLATFORM_INFINEON)


copy_files = zephyr.copy_files
get_download_types = zephyr.get_download_types
process_stacktrace = zephyr.process_stacktrace
upload_program = zephyr.upload_program


@coroutine_with_priority(CoroPriority.PLATFORM)
async def to_code(config: ConfigType) -> None:
    await zephyr.zephyr_toolchain_to_code(config)


def run_compile(args: object, config: ConfigType) -> bool:
    return CORE.using_toolchain_zephyr and zephyr.run_compile(args, config)
