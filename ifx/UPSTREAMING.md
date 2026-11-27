# Keeping changes upstream-ready

**Do not open upstream PRs.** Send changes only to the appropriate `ederjc`
fork. This includes work destined for ESPHome, the Zephyr reference fork,
Zephyr, ESPHome documentation or MCUboot. Keep both mirror branches read-only
for feature development.

- Use existing [ESPHome contribution standards](https://developers.esphome.io/contributing/code/)
  and Zephyr platform conventions. Put PSOC 6 variant configuration under
  `esphome/components/infineon/zephyr_variants/`, keeping shared fixes
  separable from platform and sensor features.
- Keep missing board definitions in ESPHome overlays for the working fork.
  Maintain equivalent board DTS changes on a separate branch in
  [`ederjc/zephyr`](https://github.com/ederjc/zephyr).
- For each user-facing feature, create a matching branch in
  [`ederjc/esphome.io`](https://github.com/ederjc/esphome.io) and link it from
  the firmware PR. Use MCUboot or HAL forks only if a change there is necessary.
- Document any dependency workaround and the intended upstream target in the
  issue or PR. If a bounded attempt to fix the dependency does not work, record
  the fallback and its trade-offs before proceeding.

Keep fork-only policy out of any future handoff, including `ifx/`, the pointer
in `AGENTS.md` (also visible through the Copilot instructions symlink),
`.github/workflows/ifx-ci.yml`, `.github/PULL_REQUEST_TEMPLATE.md`, and
`.github/CODEOWNERS`. Never include local credentials, signing keys or private
hardware logs.
