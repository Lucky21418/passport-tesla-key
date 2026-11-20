<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Fork 补充文档与素材登记目录（`docs/assets/`）

## 目录用途

本目录用于存放 `feature/tesla-key` 分支下辅助根目录 [`README.zh_CN.md`](../../README.zh_CN.md) 的 fork 专属补充文档、架构说明与素材许可登记记录。

## 目录结构约定

- 维护中的 Markdown 文档遵循双语配对规则：默认英文路径 `<name>.md` 与简体中文配对路径 `<name>.zh_CN.md`，并在顶部保留双向语言切换链接。
- 固件编译复用的二进制资源存放在仓库根目录 [`assets/`](../../assets/README.zh_CN.md)，不放在 `docs/assets/`。

## M2 素材与第三方材料登记表（Gate-B / P2 裁决）

依据父级裁决 P2 与 [`assets/README.zh_CN.md`](../../assets/README.zh_CN.md)，凡再分发许可链未闭环的素材严禁进入本仓库：

| 类别 | 路径 / 位置 | 来源与版本 / Commit | 许可证 | 在 `feature/tesla-key` 中的状态 |
| --- | --- | --- | --- | --- |
| **已剥离的旧树图标与 QR 位图** | *已移除*（不迁入 `main/app_icons.c`） | 旧树 31 枚 20×20 单色图标与 58×58 QR 位图 | 再分发许可链未闭环 | **已在 M2 全部剥离（P2 方案①）**。由 [`main/ui_widgets.c`](../../main/ui_widgets.c) 与 [`main/app_icons.h`](../../main/app_icons.h) 中的纯文本/已许可符号标签替代。 |
| **Tesla BLE 协议 Schema** | [`components/tesla_key/proto/schema/`](../../components/tesla_key/proto/schema/LICENSE) | `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` | Apache-2.0 许可证（随附于 `components/tesla_key/proto/schema/LICENSE`） | **保留并登记**。通过 [`tools/gen_tesla_proto.sh`](../../tools/gen_tesla_proto.sh) 校验一致性。 |
| **nanopb 运行时与生成配置** | [`components/tesla_key/proto/nanopb/`](../../components/tesla_key/proto/nanopb/LICENSE.txt) | `nanopb` `0.4.9.1`（锁定 `protobuf==5.28.3`、`grpcio-tools==1.68.0`） | zlib 许可证（随附于 `components/tesla_key/proto/nanopb/LICENSE.txt`） | **保留并登记**。通过 `car_server.options` 保留 `oneof` 速度字段真实存在性（presence）。 |
| **应用中文字库位图子集** | [`assets/fonts/app_font_source_han_sans_sc_12.c`](../../assets/fonts/app_font_source_han_sans_sc_12.c)、[`assets/fonts/app_font_source_han_sans_sc_16.c`](../../assets/fonts/app_font_source_han_sans_sc_16.c)、[`assets/fonts/app_font_cjk_chars.txt`](../../assets/fonts/app_font_cjk_chars.txt)、[`tools/build_cjk_fonts.py`](../../tools/build_cjk_fonts.py) | Adobe Source Han Sans SC Regular（`adobe-fonts/source-han-sans` 标签 `2.005R`，源文件 SHA-256 `f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256`），经钉选版本 `lv_font_conv` `1.5.3` 生成的 475 字形 4-bpp 未压缩 LVGL 子集（12 px 与 16 px） | SIL Open Font License 1.1（随附于 [`assets/fonts/OFL-1.1-SourceHanSans.txt`](../../assets/fonts/OFL-1.1-SourceHanSans.txt)） | **已按 M2-FIX Option B 重建并登记**。通过 [`main/app_font_cjk.c`](../../main/app_font_cjk.c) 中的薄封装 `app_font_cjk_12()` / `app_font_cjk_16()` 暴露，并通过 `tools/check_glyphs.py` 与 `tests/test_ui_text_widths.py` 校验。 |
| **官方电芯电量计参数** | [`assets/battery/wanli-500mah-profile2-xil40x-20261020.txt`](../../assets/battery/wanli-500mah-profile2-xil40x-20261020.txt) | 官方上游万锂新能源 500 mAh CW2017 参数文件 | 上游项目提供数据文件 | **与 `upstream/main` 保持一致，未作任何改动**（P5 裁决）。 |

## 自定义分区与烧录政策摘要

`feature/tesla-key` 分支在 8 MB Flash 边界内配置了 `nvs`（`0x9000`）、`phy_init`（`0xF000`）、`factory`（`0x10000`，大小 `0x7B0000`）以及 256 KB 的 `bblog` 数据分区（`0x7C0000`–`0x800000`）。该布局通过 `./tools/validate.sh --firmware` 强制校验，仅属本分支应用设计，不构成通用模板强制要求。完整的烧录与已存数据政策请参见 [`docs/development/engineering/firmware-layout.zh_CN.md`](../development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

## 边界

`docs/assets/` 下的内容仅归属本 fork，不得同步或提 PR 回上游 `FoloToy/ai-passport:main`（上游 `main` 在此目录仅保留 `.gitkeep`）。
