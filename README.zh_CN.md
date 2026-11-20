<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Passport Tesla Key（`feature/tesla-key`）

基于 FoloToy AI Passport 硬件平台（ESP32-C3、8 MB Flash、无 PSRAM、ESP-IDF 5.5.3）的 **Passport Tesla Key** 产品功能分支。

## 分支模型与基线

- **上游基线**：`FoloToy/ai-passport:main`（`e85197d56abe92c35a1cfb84fad85fba2545c9cc`）
- **Fork 基线（`main`）**：与 `FoloToy/ai-passport:main` 严格保持同步，不承载产品固件代码改动。
- **产品分支（`feature/tesla-key`）**：直接从 `upstream/main` 分叉创建。所有应用专属 UI、BLE 钥匙工作流、状态机与补充说明均遵循 [`docs/fork-guide.zh_CN.md`](docs/fork-guide.zh_CN.md) 在 `feature/*` 分支上开发。

## M2 受控迁入范围（`0dc5301`）

本分支依据父级 P1–P3 裁决与 Gate-A/B/C 准入门禁，受控迁入已独立验收通过的 `0dc5301c52fd4d3ee1185227380a9d323e249860`（S0–S3 FIX2）协议与编排核心：

1. **官方 BSP 基线与按键时序对齐（Gate-A / P1）**：
   - 完整保留官方 [`components/bsp/`](components/bsp/include/bsp_pins.h) 不变：LCD SPI2 保持 **80 MHz**（`BSP_LCD_PCLK_HZ`），保留官方万锂 500 mAh CW2017 电量计参数、ES8311 休眠校验及驱动层按键电压锁（**40 ms** 确认 / **120 ms** 释放保持）。
   - 应用层 [`pk_btn_adapter`](components/tesla_key/include/pk_btn_adapter.h) 与 [`pk_policy.h`](components/tesla_key/include/pk_policy.h) 对齐 **40 ms / 120 ms** 电压锁，同时保留 **180 ms** 单击/双击窗口、**500 ms** 长按阈值、GPIO0 ADC 毫伏窗口（`{0,150}` / `{150,447}` / `{447,1900}` mV，松开 `>= 1900 mV`）以及已冻结的 13 页独立应用 UI 交互。
2. **素材与第三方代码许可合规（Gate-B / P2）**：
   - 旧树中许可未闭环的 31 枚位图图标与 QR 位图已全部剥离（移除 `main/app_icons.c`）；界面图标槽位与 VIN 配网卡片改用纯文本/已许可符号标签（`main/ui_widgets.c`）。
   - 已明确许可的第三方协议资产与应用内中文字库子集已在 [`docs/assets/README.zh_CN.md`](docs/assets/README.zh_CN.md) 登记。
3. **内容安全与可配置 Proto 工具链（Gate-C）**：
   - [`tools/gen_tesla_proto.sh`](tools/gen_tesla_proto.sh) 支持通过环境变量（`NANOPB_PYTHON` / `NANOPB_GENERATOR`）配置，无任何硬编码个人绝对路径，并严格锁定 `protobuf==5.28.3`、`grpcio-tools==1.68.0`、`nanopb==0.4.9.1`。
   - 严禁迁入旧 S4 提交 `33560de`、清理候选 `7e0582c` 或外部非授权目录。

## 自定义 8 MB 分区布局与烧录已存数据政策

遵循 [`docs/development/engineering/firmware-layout.zh_CN.md`](docs/development/engineering/firmware-layout.zh_CN.md)，本应用采用经过校验的自定义 8 MB [`partitions.csv`](partitions.csv)：

| 分区名 | 类型 / 子类型 | 偏移量 | 大小 | 用途 |
| --- | --- | ---: | ---: | --- |
| `nvs` | `data` / `nvs` | `0x9000` | `0x6000` | ESP-IDF 与应用 NVS 键值存储 |
| `phy_init` | `data` / `phy` | `0xF000` | `0x1000` | 射频 PHY 初始化数据 |
| `factory` | `app` / `factory` | `0x10000` | `0x7B0000` | 单一主应用固件分区 |
| `bblog` | `data` / `0x40` | `0x7C0000` | `0x40000` | 本应用专属有界黑匣子诊断环形区（仅属本应用可选设计，不构成通用模板强制要求） |

### 烧录与已存数据政策

严格遵循 [`docs/development/engineering/firmware-layout.zh_CN.md#烧录与已存数据`](docs/development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)：

- 下载（烧录）新固件前**无需备份设备内原有固件**，但这**不代表保证保留用户数据，也不授权执行整片擦除（`idf.py erase-flash`）**。
- 从偏移 `0x0` 烧录经过验证的合并镜像 `build/FoloToy-AI-Passport-full.bin` 会填充各分区之间的间隙，**可能重置 NVS 与 `bblog` 数据区**（包括已保存的车辆钥匙与设置）。`0x0` 合并镜像仅用于空片初始化或明确意图的完整刷新。
- 若需在开发中保留现有 NVS 状态，须在分区兼容且获得明确授权的前提下使用分段烧录（`idf.py flash`）。

## 验证门禁

```bash
./tools/validate.sh --static                  # 仓库规范检查、字形覆盖、actionlint、官方 13 项 + 产品主机测试
./tools/gen_tesla_proto.sh --check            # 校验 nanopb 生成源码与 schema/options 一致性
./tools/validate.sh --firmware                # 隔离 ESP-IDF 5.5.3 构建、合并镜像、固件布局与归档校验
python3 tools/archive_firmware.py verify <archive-dir>
```
