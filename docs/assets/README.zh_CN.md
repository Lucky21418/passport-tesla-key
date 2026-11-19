<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Fork 补充文档与素材目录（`docs/assets/`）

## 目录用途

本目录用于存放 `feature/tesla-key` 分支下辅助根目录 [`README.zh_CN.md`](../../README.zh_CN.md) 的 fork 专属补充文档、架构说明、产品设计规范与文档配图。

## 目录结构约定

- 维护中的 Markdown 文档遵循双语配对规则：默认英文路径 `<name>.md` 与简体中文配对路径 `<name>.zh_CN.md`，并在顶部保留双向语言切换链接。
- 固件编译复用的二进制资源（字体、图片、音频）存放在仓库根目录 [`assets/`](../../assets/README.zh_CN.md)，不放在 `docs/assets/`。

## AI 使用方法

在 `feature/tesla-key` 分支开展工作时，先从 [`AGENTS.zh_CN.md`](../../AGENTS.zh_CN.md) 进入，结合本分支根目录 [`README.zh_CN.md`](../../README.zh_CN.md)、上游 [`docs/README.zh_CN.md`](../README.zh_CN.md) 以及本目录下的补充说明理解项目背景。

## 边界

`docs/assets/` 下的内容仅归属本 fork，不得同步或提 PR 回上游 `FoloToy/ai-passport:main`（上游 `main` 在此目录仅保留 `.gitkeep`）。
