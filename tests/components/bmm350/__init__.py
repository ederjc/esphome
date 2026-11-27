import esphome.codegen as cg
from esphome.types import ConfigType
from tests.testing_helpers import ComponentManifestOverride


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # The standalone sensor platform declares these; the bare test selector does not.
    manifest.dependencies = manifest.dependencies + ["i2c", "sensor"]

    async def to_code_testing(config: ConfigType) -> None:
        cg.add_define("USE_SENSOR")

    manifest.to_code = to_code_testing
