"""Bounded PSOC 6 PDMv1 streaming through Zephyr's DMIC API."""

import esphome.codegen as cg

CODEOWNERS = ["@ederjc"]
DEPENDENCIES = ["zephyr"]
DOMAIN = "zephyr_dmic"

zephyr_dmic_ns = cg.esphome_ns.namespace("zephyr_dmic")
