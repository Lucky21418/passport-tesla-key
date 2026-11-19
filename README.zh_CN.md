<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Passport Tesla Key（`feature/tesla-key`）

基于 FoloToy AI Passport 硬件平台（ESP32-C3、8 MB Flash、无 PSRAM、ESP-IDF 5.5.3）的 **Passport Tesla Key** 干净产品功能分支。

## 分支模型与基线

- **上游基线**：`FoloToy/ai-passport:main`（`e85197d56abe92c35a1cfb84fad85fba2545c9cc`）
- **Fork 基线（`main`）**：与 `FoloToy/ai-passport:main` 严格保持同步，不混入具体产品代码。
- **产品分支（`feature/tesla-key`）**：直接从 `upstream/main` 分叉创建。所有应用专属 UI、BLE 钥匙工作流、状态机与补充说明均遵循 [`docs/fork-guide.zh_CN.md`](docs/fork-guide.zh_CN.md) 在 `feature/*` 分支上开发。

## 当前范围（项目骨架）

本分支当前仅建立干净的官方 fork 项目骨架与分支边界：

1. 未迁入任何旧分支或未通过审计的固件代码树。
2. 产品专属设计与架构补充材料存放于 [`docs/assets/`](docs/assets/README.zh_CN.md)。
3. 硬件引脚事实与板级接口遵循 [`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h) 与 [`AGENTS.zh_CN.md`](AGENTS.zh_CN.md)。
