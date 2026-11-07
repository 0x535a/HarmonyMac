# Aurora 鸿蒙显示后端

当前为 OHOS ABI Mesa 软件渲染、原装 NativeWindow 队列与 DRM/KMS 输出。最新真机状态见 [状态说明](../../docs/status.md)，构建入口见 [构建说明](../../docs/build.md)。

## 组件

- `drm_device.c` 枚举 DRM card，忽略 simpledrm，优先 appledrm 与内屏。
- `buffer_vdi.cpp` 使用 dumb/PRIME 分配线性 RGBA/RGBX/BGRA/BGRX 缓冲，写回原装 HDI 所需的 `BufferHandle.virAddr`，拒绝 protected buffer。
- `composer.cpp` 全部 layer 回退 CLIENT，将客户端目标复制到持久双缓冲，执行 DMA-BUF CPU 写入同步并提交 KMS。同步等待翻页，超时保留 pending 状态。
- `egl_window_bridge.cpp` 以独立 `libEGL_impl.so` 加载 OHOS Mesa EGL，通过 GBM backing 保留 GLES 默认 framebuffer 语义；swap 转换像素并提交到鸿蒙队列，按实际 geometry 延迟创建 backing。
- `patches/` 包含 Mesa profiler 开销修正、资源释放和有界绘制诊断补丁。诊断补丁按需使用。

## ABI 与测试

平台边界使用原装 `std::__h` 和 `libc++.so`，SDK 的 `std::__n1` 对象不跨边界传递。`buffer_handle.h` 为固定镜像的 56 字节内部 ABI，不能换成 API20 的 64 字节布局。

构建入口在 `../tools/`：`build-buffer-vdi.sh`、`build-composer.sh`、`build-egl-window-bridge.sh`。测试源码在 `../tests/`，覆盖 allocator/composer、EGL、原装队列、RenderService 和 VSync。

## 限制

当前单连接器、首选模式，无硬件 overlay/cursor、HDR 或完整热插拔处理。vsync 按所选 mode 周期调度，尚未锁定硬件 vblank。CPU 复制性能、多线程、多窗口及跨进程 metadata 语义需要继续验证。真机光标拖影与首显延迟尚未完成修复验收。

接口来源和固定提交见 [第三方说明](../../docs/third-party.md)。
