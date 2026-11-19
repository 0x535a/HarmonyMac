# HarmonyMac

将 HarmonyOS 用户态运行在 Apple Silicon 的 Aurora Linux 内核上。

HarmonyOS 提供自己的 init、系统服务、SceneBoard 桌面、输入栈和应用；Aurora 提供 Linux 内核与 Apple 硬件驱动。开发与虚拟预检使用 macOS + UTM/QEMU，目标环境独立启动鸿蒙用户态。

## 当前状态

这是实验性适配项目。以下进度更新于 **2026-11-19**，真机验证范围为 **Apple M3 Pro / J514S**。

- 已从外置根盘原生启动鸿蒙桌面并正常关机，触控板、Wi-Fi 扫描和入网、蓝牙设备发现已有实机记录。
- 已实测 OHOS ABI Mesa/Asahi 使用 Apple M3 Pro GPU，并保留 llvmpipe 软件回退。全高内屏模式为 3024×1964 @ 120 Hz。启动动画与 DMA-BUF 直送已有实机分项，用户确认最新候选的鼠标顿挫已解决，实际新帧速率仍待量化。
- 最新开发候选在 HDMI 多屏导致 Composer 崩溃后回退为单屏，保留异步提交、孵化服务恢复及实验性板载音频，该单屏组合已实机正常启动并可用。浏览器仍无法启动，板载音频不稳定。
- macOS/HVF 已验证签名 HAP、虚拟 USB 和网络、音频、存储及部分硬件接口。实体扬声器、外屏扩展、休眠与最终联合回归仍待完成。
- 开发候选可结束 OOBE 进入桌面。华为账号、云服务、应用市场及商业激活尚未验证。

详细进度见 [状态与验证范围](docs/status.md)，实现见 [架构说明](docs/architecture.md)。

## 镜像下载

HarmonyOS 镜像通过[百度网盘](https://pan.baidu.com/s/1DmrbZjC8IqnzeOEG4j0vOg?pwd=4yzw)单独提供，提取码：`4yzw`。

该链接沿用已有镜像，版本、SHA256 和对应源码提交待补充。11 月 19 日的开发候选暂无公开镜像。

仓库只包含适配源码、补丁和文档。ISO、rootfs、虚拟磁盘、内核二进制、模块、固件、SDK 和运行日志均不收录。

## 开发资料

- [构建环境与流程](docs/build.md)：依赖、固定版本、脚本入口及复现缺口。
- [运行与原生启动说明](docs/running.md)：预览包使用方式和原生部署边界。
- [跳过 OOBE 进入开发桌面](docs/oobe-development-desktop.md)：原装停止接口、离线初始化标志、代码和回退。
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
