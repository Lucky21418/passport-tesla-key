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

## 基础版开发与门禁计划（2026-11-24 裁决）

依据 2026-11-24 用户确认的基础版开发决策，基础版后续开发改为连续推进、最后一次性集中验收（补充跟踪表见 [`docs/assets/README.zh_CN.md`](docs/assets/README.zh_CN.md#基础版开发审查与集中真机测试计划2026-11-24)）：

1. **先连续开发、最后一次性集中测试**：鉴于用户无足够时间反复进行中途真机测试，基础版后续改为先连续完成开发，最后执行一次性集中真机测试。
2. **取消开发期中途真机闸门（原 `P02`、`P03`）**：开发期不再以原 `P02`、`P03` 作为中途闸门，也不再每完成一小段就刷机或等待真机验收（中途增量按 [`AGENTS.zh_CN.md`](AGENTS.zh_CN.md#必须执行的验证与交付格式) 记录 `Device tests: NOT RUN` 并在 `Unverified` 列出待测项）。
3. **沿当前候选追加开发与分支命名延后（`P04`、`P05`）**：继续在当前候选上追加开发；`P04` 范围按既定基础版目标执行，不再单独过闸；`P05` 暂不另开正式基础版分支，正式命名待收口时再定。
4. **合并为一次终审官方独审（原 `P10`、`P15`、`P16`）**：中途官方云审不再作为开发关卡；原 `P10`、`P15`、`P16` 合并为开发完成后对最终候选的一次官方独审，审出的问题先改完再出镜像。
5. **单轮集中真机测试与条件功耗测量（含原 `P12`）**：用户只做一轮集中真机测试——刷入终审修复后的镜像，检查启动、屏幕、三键、双击手感、BLE、存储重启、配对界面和休眠唤醒；具备仪表条件时再测功耗（原 `P12`）。
6. **测后正式裁决与交付（覆盖原 `P03`、`P17`）**：集中真机测试完成后再执行正式裁决与交付，统一覆盖原 `P03` 与 `P17`。
7. **仅对必改问题出修复版复测**：若集中测试暴露必须修复的问题，再出一版修复镜像复测，不在开发过程中反复穿插测试。
8. **单次单项派工与实车红线**：派工仍严格保持一次一项；不执行实车配对与实车控制。
9. **双击误触发状态（已共同确认采用方案 E）**：已共同确认采用方案 E（`PK_DOUBLE_MS = 300 ms`、保留跨窗口 / `▲` / `▼` 与 ADC 无效读数的 `120 ms` 保持锁、仅对 `●` 回到有效 `>= 1900 mV` 干净松开区采用 `40 ms` 对称释放确认、`5 ms` `esp_timer` 中值采样按真实采样时刻入队并在深睡前停删定时器、手势进行中不被无关后台清理打断）；真机双击手感验收仍留待末尾一次性集中真机测试完成。

### 当前 Active Goal 与持续开发计划（2026-11-25，基线 `a3a5305`）

- **Active Goal**：在 `cand/d08-dead-code-cleanup` 上，以 `a3a5305` 为已冻结可测基线，连续完成：1）字库按已裁决共识改到思源黑体 Medium 12/16，保留 4bpp；2）不依赖真机的官方低优先级项，一次一项。每项先主机自检，再普通推送。全程不刷机、不实车。
- **冻结基线**：设备上已授权刷入且官方独审通过的是 `a3a530556938f69cd5acbc047bdb23d96a1c492f`（tree `25af623c1ca29e94fd59635be28443655b038adf`），不改写、不 rebase、不自行刷入新树；`main` 保持 `e85197d56abe92c35a1cfb84fad85fba2545c9cc`，`feature/tesla-key` 保持 `47fd967aaa22d619fce3ffa15a3e97d5bd4ccac2`。
- **执行顺序（一次一项）**：
  1. 将 Active Goal 与本计划落盘保存（仅改文档，提交并普通推送）。
  2. 实施字体共识：官方 OFL-1.1 `SourceHanSansSC-Medium.otf` 按 `12px` / `16px` 实际字号、`--bpp 4 --no-compress` 分别生成，无运行时缩放，不新增 `14px`、不改 `1bpp`；深色背景 `COL_MUTED` 可略提亮（靠近 `#A8B0C2`），不改已批准图标与二维码；更新覆盖与宽度门禁通过后提交并普通推送。
  3. 清点此前官方独审中仍未关闭且不依赖真机的低优先级项，按严重度从高到低一次一项修复、主机测试并普通推送；若无则写明结束。
  4. 禁区与硬停止：不做仓库大扫除、不合并 `main` / `feature/tesla-key`、不重开解耦架构、不改 Option E、不刷机、不实车；遇需真机/审批/安全不确定则标 `NEED_HUMAN` 并跳过。

### BLE 与车端协议交互共识（2026-11-25 裁决，可执行约束）

1. **请求/响应计数器分离与每请求防重放窗口**：请求计数器与响应计数器分开；响应防重放窗口按**同一条请求（`req_id`）**建立，第一条响应 `counter` 可以是 `0`，同请求重用拒绝；官方客户端窗口宽度为 **32**（不用 64）。严禁按域共享单一响应窗口而把下一条请求的 `counter == 0` 误判为重放。
2. **VCSEC 单发有界与未知不补发**：VCSEC 同时只发一条指令，单条请求最多收三条回包；未收到回包时车辆可能已执行，不得判成未执行并自动补发。
3. **显式唤醒与禁止休眠轮询**：唤醒必须使用显式 `RKE_ACTION_WAKE_VEHICLE`；不得为读状态隐式唤醒车辆，也不得对休眠车做 Infotainment 定时轮询。
4. **proto3 省略字段保护**：proto3 省略 `vehicleLockState` 会按默认值解码成 `UNLOCKED(0)`；该字段未在报文中出现时，不得将车辆显示或判定为已解锁。
5. **参考资料与外部调参边界**：
   - 官方协议以 `teslamotors/vehicle-command` 为唯一权威；`FoloToy/ai-passport` 为硬件/BSP 官方基线，不含车机协议；其余三个立项参考仓库仅作已核实的失败经验参考，其中 `yoziru/tesla-ble` 不作生产运行时。
   - 不写入过期旧印象（如 0Bu 当前 ADR-0005 并未写「拒绝 counter 0」，ADR-0003 已 superseded）。
   - 三方实现中的超时、660s、7s 等待等仅属调参，不作为本仓规范。

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
