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

## 显示路线

原装应用与 RenderService 使用鸿蒙 NativeWindow 队列。EGL 桥在可用的 Apple render node 上选择 Mesa/Asahi，软件回退使用 llvmpipe。RenderService 最终输出可将兼容的 GBM 前缓冲导出为 DMA-BUF，由 Composer 持有扫描输出生命周期并交给 Apple DRM/KMS。无法导入的布局使用 GPU blit 或原有 CPU 拷贝路径。

直送稳定后，Composer 在驱动提供 KMS out-fence 时可异步提交，交接期和回退路径仍保留同步提交。启动动画使用同一套后端选择，交接期间短暂保留最后一帧动画，直到第一帧有效 UI 或超时。默认只向原装服务暴露内屏。多屏的 VSync 回调生命周期和桌面会话仍需适配。

平台 C++ 边界使用原装 `std::__h` / `libc++.so`，Mesa 内部 SDK `std::__n1` 对象不跨边界。固定镜像使用 56 字节 BufferHandle ABI。基础图层由 RenderService 合成，独立硬件 overlay/cursor 加速未验证。

后端选择、回退及探针见 [显示后端](../ports/graphics/README.md)。

## 原生启动经验

外置 USB-C 根盘依赖 Type-C 图，显示驱动提供部分路由 provider。将 appledrm 延迟到挂载根盘后加载会形成启动顺序依赖，因此原生 bootstrap 在查找根盘前加载相关显示模块。PMP 驱动也必须包含在 initramfs 中并先于 appledrm 加载。

保留的原生脚本含历史根 UUID、精确内核版本和日志目标约束，是特定验证基线；迁移至另一台机器前需要逐项调整，见 [运行说明](running.md)。

## L410 参考边界

最初参考材料来自 L410 HarmonyOS 适配工作。麒麟/Mali、功放、指纹和 Linux 应用兼容平台等实现没有整套复制到本仓库。Apple 平台的内核、allocator/composer 与 EGL 桥接实现集中在 `ports/`，不依赖仓库外的 `l410/` 源码目录。原装镜像中的系统库仍属于外部构建输入。
