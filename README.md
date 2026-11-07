# HarmonyMac

将 HarmonyOS 用户态运行在 Apple Silicon 的 Aurora Linux 内核上。

HarmonyOS 提供自己的 init、系统服务、SceneBoard 桌面、输入栈和应用；Aurora 提供 Linux 内核与 Apple 硬件驱动。开发与虚拟预检使用 macOS + UTM/QEMU，目标环境独立启动鸿蒙用户态。

## 当前状态

这是实验性适配项目。当前真机验证范围为 **Apple M3 Pro / J514S**，其他机型尚未验证。以下状态依据截至 2026-11-05 的开发记录整理。

- 真机已显示鸿蒙桌面，可打开设置，触控板可操作。
- 当前图形路线使用 OHOS ABI Mesa llvmpipe 软件渲染；尚未接通 Apple GPU 硬件加速。
- 真机存在严重光标拖影、约半分钟首显延迟。composer 的 DMA-BUF 同步修复已部署，小型帧提交检查通过，真机效果待复验。
- macOS/HVF 已完成原装 init、服务链、真实图形队列和像素等分项验证；这些结果不代表完整系统通过验收。
- 华为账号、云服务、应用市场和商业设备激活未验证。当前商业激活在 TEE/SoC 证明处失败；开发候选可通过结束 OOBE 进入桌面。
- 音频、Wi-Fi、蓝牙、休眠、多屏及长期稳定性等尚未完成真机验收。

详细进度见 [状态与验证范围](docs/status.md)，实现见 [架构说明](docs/architecture.md)。

## 镜像下载

HarmonyOS 镜像通过[百度网盘](https://pan.baidu.com/s/1DmrbZjC8IqnzeOEG4j0vOg?pwd=4yzw)单独提供，提取码：`4yzw`。

镜像版本、SHA256 和对应源码提交待补充。

仓库只包含适配源码、补丁和文档。ISO、rootfs、虚拟磁盘、内核二进制、模块、固件、SDK 和运行日志均不收录。

## 开发资料

- [构建环境与流程](docs/build.md)：依赖、固定版本、脚本入口及复现缺口。
- [运行与原生启动说明](docs/running.md)：预览包使用方式和原生部署边界。
- [第三方来源与许可](docs/third-party.md)：Aurora、OpenHarmony、Mesa 等上游材料。
- [贡献与仓库检查](CONTRIBUTING.md)。

```text
ports/
├── kernel/      # 内核补丁、AccessToken 与 ashmem
├── graphics/    # DRM allocator/composer、EGL 桥接与 Mesa 补丁
├── initramfs/   # 启动、模块加载与自动日志收集
├── config/      # HDF 配置
├── tools/       # 构建、rootfs 适配与虚拟验证工具
├── tests/       # 接口、服务、图形和输入探针
└── release/     # 开发预览的启动、停止、日志收集和回退工具
```

## 许可

本项目原创代码、脚本和文档采用 [GPL-3.0-only](LICENSE)。第三方文件与基于上游修改的材料保留各自许可，详见 [来源说明](docs/third-party.md)。HarmonyOS 镜像及第三方二进制的权利不由本仓库许可授予。
