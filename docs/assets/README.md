<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fork Supplemental Documentation and Assets (`docs/assets/`)

## Purpose

This directory holds fork-owned supplemental documentation, architecture notes, product design specifications, and documentation images that support the root [`README.md`](../../README.md) on the `feature/tesla-key` branch.

## Structure Convention

- Keep bilingual Markdown documents paired as `<name>.md` (English default) and `<name>.zh_CN.md` (Simplified Chinese) with reciprocal language links at the top.
- Store reusable firmware binary assets (fonts, images, audio compiled into firmware) under repository-root [`assets/`](../../assets/README.md), not in `docs/assets/`.

## AI Agent Usage

When working on `feature/tesla-key`, start from [`AGENTS.md`](../../AGENTS.md), read the branch root [`README.md`](../../README.md) together with upstream [`docs/README.md`](../README.md), and consult files in `docs/assets/` for fork-specific product context.

## Boundary

Content in `docs/assets/` is private to this fork and must not be proposed back to upstream `FoloToy/ai-passport:main`. Upstream keeps `docs/assets/` empty except for `.gitkeep`.
