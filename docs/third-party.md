# 第三方来源与许可

本仓库原创代码、脚本和文档默认采用根目录的 GPL-3.0-only。文件已有上游版权、许可或 SPDX 标记时，以该文件及上游许可为准。内核补丁遵循所修改内核文件的许可，Mesa 补丁遵循对应上游文件的许可。

| 材料 | 来源 | 许可处理 |
| --- | --- | --- |
| Aurora 内核补丁 | [omacom/linux-aurora](https://github.com/omacom/linux-aurora)，`d13466ebb8b5f2e2af3d2c8359f4c96e711a5679` | 沿用对应 Linux 文件许可，通常为 GPL-2.0；未复制完整内核 |
| AccessToken 驱动与原始对照文件 | [OpenHarmony kernel_linux_5.10](https://github.com/openharmony/kernel_linux_5.10)，`4e38b39c0ca8cc8768061c5ba645c1b748c66d95`，`drivers/accesstokenid/` | 保留 Huawei 版权及 GPL-2.0 标记；移植变化见 kernel README |
| ashmem 实现与 UAPI | `kernel/ashmem/` | 实现为 GPL-2.0-only；UAPI 保留 Google 版权及 GPL-2.0 OR Apache-2.0 |
| 显示 IDL 与接口头 | [OpenHarmony drivers_interface](https://github.com/openharmony/drivers_interface)，`9eaf9abff9c390f6a4bf95271cf482e72c3b8c29` | 保留 Apache-2.0 与 Huawei Device 版权；结构翻译头保留 Apache-2.0 标记 |
| HDF 加载协议参考 | [OpenHarmony drivers_peripheral](https://github.com/openharmony/drivers_peripheral)，`8cd47d4bb09a4d160e8b713b2fbccf8d41e2025f` | 参考上游协议；目标库由外部镜像提供 |
| DFX dump catcher 声明 | `tests/include/dfx_dump_catcher.h`，OpenHarmony DFX 接口 | 保留 Apache-2.0 与 Huawei Device 版权；原记录未固定独立来源提交 |
| Mesa 补丁 | 基线 `7c86130d26cecd50c7af1ac01ff3fa1705d58045` | 按对应 Mesa 文件原许可使用；完整版权材料由外部源码树提供 |
| LLVM、libdrm、zlib、expat、BusyBox | 外部构建输入 | 未捆绑源码或二进制；分发构建产物时另行保留其许可与对应源码要求 |

历史记录中 `DisplayComposerType.idl` SHA256 为 `8be06dca7cba51335a31d31c8f979e20ffa3dd8149b7ce181667f5bcb3dc41a0`；drivers_peripheral codeload tar SHA256 为 `372784383662527e8beea176a1290252efb3c50188e761db8a0e3fd64c2e89f3`。

最初开发材料包含 L410 适配源码与 GPL-3.0 许可文本，本仓库未整套收录。HarmonyOS 原装服务、系统库、SDK、固件、镜像和商业服务属于外部材料，本仓库 GPL 不授予其再分发权。

许可全文：[GPL-3.0-only](../LICENSE)、[GPL-2.0](../LICENSES/GPL-2.0-only.txt)、[Apache-2.0](../LICENSES/Apache-2.0.txt)。
