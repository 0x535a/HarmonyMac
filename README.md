# HarmonyMac

将 HarmonyOS 用户态运行在 Apple Silicon 的 Aurora Linux 内核上。

HarmonyOS 提供自己的 init、系统服务、SceneBoard 桌面、输入栈和应用；Aurora 提供 Linux 内核与 Apple 硬件驱动。开发与虚拟预检使用 macOS + UTM/QEMU，目标环境独立启动鸿蒙用户态。

## 真机验证

HarmonyMac 仍处于实验性适配阶段。以下结果来自两台真机上的不同候选构建。

| 机型 | 原生启动与实测进展 |
| --- | --- |
| [Apple M3 Pro / J514S](docs/devices/j514s-m3-pro.md) | 从 U393 外置 ext4 根盘启动并正常关机。已验证 Asahi 单屏桌面、触控板、BCM4388 Wi-Fi 入网和蓝牙发现。 |
| [Apple M1 Max / J316c](docs/devices/j316c-m1-max.md) | 从内置 NVMe 的独立 ext4 分区启动。实测 Asahi 直送、锁屏解锁、触控板多指事件及背光调节。使用仓库构建器生成的 initramfs 冷启动和 BCM4387 入网仍待验证。 |

各机型的硬件差异、待验证项和虚拟测试范围见 [状态与验证范围](docs/status.md)。

## 镜像下载

HarmonyOS 镜像通过[百度网盘](https://pan.baidu.com/s/1DmrbZjC8IqnzeOEG4j0vOg?pwd=4yzw)单独提供，提取码：`4yzw`。

该链接沿用已有镜像，版本、SHA256 和对应源码提交待补充。仓库暂未提供 J514S 的 11 月 19 日开发候选和 J316c 真机测试构建的镜像链接。

仓库只包含适配源码、补丁和文档。ISO、rootfs、虚拟磁盘、内核二进制、模块、固件、SDK 和运行日志均不收录。

## 使用与开发

- 机型、架构及源码入口见 [文档索引](docs/README.md)。
- macOS 开发预览和真机部署要求见 [运行与原生启动](docs/running.md)。
- 依赖版本、脚本入口和复现缺口见 [构建环境与流程](docs/build.md)。
- [贡献与仓库检查](CONTRIBUTING.md)。

## 许可

本项目原创代码、脚本和文档采用 [GPL-3.0-only](LICENSE)。第三方文件与基于上游修改的材料保留各自许可，详见 [来源说明](docs/third-party.md)。HarmonyOS 镜像及第三方二进制的权利不由本仓库许可授予。
