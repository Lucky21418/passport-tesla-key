<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Tesla Key (`feature/tesla-key`)

Clean product feature branch for the **Passport Tesla Key** application on the FoloToy AI Passport hardware platform (ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3).

## Branch Model and Baseline

- **Upstream Baseline**: `FoloToy/ai-passport:main` (`e85197d56abe92c35a1cfb84fad85fba2545c9cc`)
- **Fork Baseline (`main`)**: Kept strictly synchronized with `FoloToy/ai-passport:main` without product code changes.
- **Product Branch (`feature/tesla-key`)**: Branched directly from `upstream/main`. All application-specific UI, BLE key workflow, state machines, and documentation belong on `feature/*` branches per [`docs/fork-guide.md`](docs/fork-guide.md).

## Current Scope (Project Skeleton)

This branch currently establishes the clean official-fork project skeleton and branch boundary:

1. No legacy or unverified firmware trees have been imported.
2. Product-specific design and architecture notes belong under [`docs/assets/`](docs/assets/README.md).
3. Hardware pin truth and board APIs follow [`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h) and [`AGENTS.md`](AGENTS.md).
