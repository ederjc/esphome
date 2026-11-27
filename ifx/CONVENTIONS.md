# Fork conventions

## Branches and work items

| Branch in `ederjc/esphome` | Purpose |
| --- | --- |
| `dev` | Read-only mirror of `esphome/esphome:dev` |
| `zephyr-ref` | Read-only mirror of `esphomeplatformzephyr/esphome:framework-zephyr-aligned` |
| `ifx/dev` | Default integration branch, based on `zephyr-ref` |

Open topic branches from `ifx/dev` and target `ifx/dev` in this fork. Keep
shared Zephyr work, the Infineon platform and individual sensor changes in
separate PRs. Use the [GitHub Project](https://github.com/users/ederjc/projects/1)
for milestone epics and sub-issues, assign issues to `@ederjc`, and keep
Implementation status, Milestone, Area, Agent role, Target release and Upstream
target current.
Move work through Backlog, Ready, In progress, In review, HW test (if
applicable), and Done.

Platform, Zephyr and sensor owners coordinate integration through the project.
If blocked on a dependency, make one bounded fix attempt in the relevant fork,
then document the result and the fallback in the issue and PR.

## Commits and review

- Use imperative [Conventional Commits](https://www.conventionalcommits.org/),
  for example `feat(infineon): add PSOC 6 board support`.
- Use `Eder Julian <Julian.Eder@infineon.com>` as the commit author. Include
  `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`
  on Copilot-authored commits.
- Link the issue, focused CI, docs branch (for user-facing changes) and hardware
  evidence from the PR. For changes that cannot affect firmware behavior, record
  why hardware testing is not applicable.
- Require approval by a different agent, green fork CI and the hardware evidence
  (or a justified not-applicable note) before merging. The author may merge after
  those gates pass; the maintainer reviews progress at each milestone.
- `@ederjc` is the fork code owner via `.github/CODEOWNERS`. GitHub uses it in
  preference to the generated root `CODEOWNERS`, which remains unchanged.

## Releases

Tag usable, hardware-verified increments with `vX.Y.Z`; do not push upstream
calendar-version tags. `v0.1.0` is boot, GPIO, logger and preferences.
Increment MINOR for each hardware-verified feature, PATCH for fixes, and use
`v1.0.0` when M1 platform support is complete.

Attach release notes, hardware evidence and the reference-kit YAML (without
secrets). Include a factory `.hex` for KitProg3 and a signed MCUboot OTA `.bin`
when the OTA path is available. Build these separately from bench images:
firmware containing Wi-Fi, API or OTA secrets must never be published. Keep
signing keys out of releases and Git.
