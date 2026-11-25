# HarmonyMac 文档

## 机型与验证

- [状态与验证范围](status.md)汇总两台机器的原生进展及 macOS 虚拟测试范围。
- [Apple M3 Pro / J514S](devices/j514s-m3-pro.md)记录 U393 外置根盘、Asahi 单屏候选及硬件验证。
- [Apple M1 Max / J316c](devices/j316c-m1-max.md)记录内置 NVMe、SMC 看门狗、BCM4387 和板载硬件探测。

## 运行与开发

- [运行与原生启动](running.md)涵盖 macOS 开发预览、真机部署和日志反馈。
- [进入开发桌面](oobe-development-desktop.md)介绍结束 OOBE、离线初始化和回退。
- [构建环境与流程](build.md)列出依赖版本、构建入口、分项验证和预览打包。
- [架构说明](architecture.md)介绍启动链、内核接口和显示路径。
- [第三方来源与许可](third-party.md)列出 Aurora、OpenHarmony、Mesa 等上游材料。
- [贡献与仓库检查](../CONTRIBUTING.md)说明如何补充机型记录和提交验证结果。

## 适配源码

| 目录 | 内容 |
| --- | --- |
| [ports/kernel](../ports/kernel/README.md) | 内核补丁、AccessToken 和 ashmem |
| [ports/graphics](../ports/graphics/README.md) | DRM allocator/composer、EGL 桥接和 Mesa 补丁 |
| [ports/hardware](../ports/hardware/README.md) | 电池、照明、存储、传感器、Wi-Fi 和模块加载 |
| [ports/audio](../ports/audio/README.md) | USB ALSA 播放和实验性 J514 板载音频 |
| [ports/bluetooth](../ports/bluetooth)、[ports/firmware](../ports/firmware) | Linux HCI 传输和 Asahi 固件转换 |
| [ports/initramfs](../ports/initramfs)、[ports/config](../ports/config) | 启动、模块加载、日志收集和 HDF 配置 |
| [ports/tools](../ports/tools)、[ports/tests](../ports/tests) | 构建、rootfs 适配、虚拟验证和分项探针 |
| [ports/release](../ports/release/README.md) | 开发预览的启动、停止、日志收集和回退 |
