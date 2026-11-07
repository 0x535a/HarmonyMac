# 架构

## 运行环境

启动链由 Apple 平台引导组件、m1n1/UEFI/GRUB、Aurora 内核及独立 initramfs 组成。initramfs 加载匹配的 Apple 驱动、发现 HarmonyOS 根分区，随后 switch_root 到原装 `/system/bin/init`。

目标用户态使用 HarmonyOS 自己的参数服务、SAMgr、HDF、Foundation、RenderService、SceneBoard 与 MMI。Debian/UTM 用作构建、诊断和救援环境。

## 内核适配

固定 Aurora 基线与 OpenHarmony 来源见 [source-lock.json](../ports/kernel/source-lock.json)。核心工作包括：

- 在 16 KiB 内核页环境中处理鸿蒙 4 KiB 用户页契约。
- Binder 映射边界和原生页分配处理。
- AccessToken/ftoken 身份字段与 Binder 传递。
- ashmem 兼容模块。
- exec 后用户页策略与 BPF ringbuf 用户映射。

内核、modules、设备树及 initramfs 需要使用同一构建版本。固定 test7 名称仍保留在原生工具中，升级需要重新核对。

## 软件显示路线

应用/RenderService → OHOS EGL/NativeWindow 桥 → Mesa llvmpipe → 原装缓冲队列 → HDF allocator/composer → DRM/KMS → Apple 显示驱动。

`buffer_vdi.cpp` 使用 DRM dumb/PRIME 缓冲；`egl_window_bridge.cpp` 在 Mesa GBM 与鸿蒙 NativeWindow 队列之间传递像素；`composer.cpp` 将客户端合成目标复制到持久扫描缓冲并提交 KMS。当前所有 layer 回退为 CLIENT，没有硬件 overlay/cursor 加速。

平台 C++ 边界使用原装 `std::__h` / `libc++.so`，Mesa 内部 SDK `std::__n1` 对象不跨边界。固定镜像使用 56 字节 BufferHandle ABI，不能直接换成公开 API20 的 64 字节结构。

真机只有显示节点、无 renderD 节点时，现有 llvmpipe 路线需要显式设置 `GBM_ALWAYS_SOFTWARE=1`；仅设置 `LIBGL_ALWAYS_SOFTWARE` 不能代替 GBM 软件路径选择。

## 原生启动经验

外置 USB-C 根盘依赖 Type-C 图，显示驱动提供部分路由 provider。将 appledrm 延迟到挂载根盘后加载会形成启动顺序依赖，因此原生 bootstrap 在查找根盘前加载相关显示模块。PMP 驱动也必须包含在 initramfs 中并先于 appledrm 加载。

保留的原生脚本含历史根 UUID、精确内核版本和日志目标约束，是特定验证基线；迁移至另一台机器前需要逐项调整，见 [运行说明](running.md)。

## L410 参考边界

最初参考材料来自 L410 HarmonyOS 适配工作。麒麟/Mali、功放、指纹和 Linux 应用兼容平台等实现没有整套复制到本仓库。Apple 平台的内核、allocator/composer 与 EGL 桥接实现集中在 `ports/`，不依赖仓库外的 `l410/` 源码目录。原装镜像中的系统库仍属于外部构建输入。
