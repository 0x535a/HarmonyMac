# Aurora 鸿蒙显示后端

原装 RenderService 和启动动画经 EGL 桥选择 Mesa/Asahi 或软件渲染。Composer 将兼容的 GBM/DMA-BUF 直接交给 DRM/KMS，支持同步复制回退。默认只向原装服务暴露内屏。已有真机和虚拟测试范围见 [状态页](../../docs/status.md)。

## 构建

先按 [构建说明](../../docs/build.md) 准备 OHOS 工具链、Mesa 依赖、原装平台库和 LLVM 19。Asahi 与 llvmpipe 共用固定的 [Mesa 源码提交](source-lock.json)。在独立 Mesa 工作树检查并应用 VA 重用补丁，先构建同版本的宿主 `mesa_clc`、`vtn_bindgen2`、`asahi_clc` 到 `build/src` 下，再构建 OHOS 目标库。

```sh
git -C "$MESA" apply --check "$PROJECT/ports/graphics/patches/mesa-asahi-bo-va-recycle.patch"
git -C "$MESA" apply "$PROJECT/ports/graphics/patches/mesa-asahi-bo-va-recycle.patch"
bash ports/tools/build-ohos-llvmpipe.sh "$MESA" "$WORK" --asahi
```

Asahi 构建安装到 `$WORK/asahi-deps/system`。将其中的 Mesa 头文件和目标库整合到 `$WORK/ohos-deps/system`，保留 libdrm 和其他依赖，然后构建 [allocator](../tools/build-buffer-vdi.sh)、[Composer](../tools/build-composer.sh) 和 [EGL 桥](../tools/build-egl-window-bridge.sh)。软件专用构建省略 `--asahi`，使用独立输出目录。

## 后端与回退

- [RenderService 启动脚本](render-service-start.sh) 检查 Asahi DRM render node；匹配的 init 环境须提供软件回退所需的 GBM/Mesa 设置。传入 `--bootanimation` 时为原装启动动画选择后端。
- [Composer 启动脚本](composer-start.sh) 默认启用直送，并清除实验性的多屏环境开关。内核参数 `aurora.scanout=copy` 禁用直送，`aurora.graphics=software` 同时选择软件渲染。两个脚本由匹配候选的服务配置调用。
- Composer 若能获得 KMS out-fence，在正常直送翻页时返回该围栏。调试时可在 Composer 服务环境设置 `AURORA_KMS_SYNC_COMMIT=1` 强制同步，或设置 `AURORA_KMS_CLEAR_COPY=1` 恢复内存清理时的全屏拷贝。

## 测试入口

[扫描输出探针](../tests/egl-scanout-cycle-probe.cpp) 覆盖 36 帧局部更新、呈现围栏及逐像素比对。它在可写虚拟测试层中使用匹配的 OHOS EGL、allocator 和 Composer；宿主不能用系统 libEGL 替代这些目标库。另有 [帧标记探针](../tests/frame-marker-probe.cpp)、[逐帧诊断分析](../tools/analyze-frame-content.py) 和 [原生呈现间隔汇总](../tools/summarize-native-direct-presents.py)。分析同一次启动的日志时以 boot ID 为边界。

[启动交接单测](../tests/boot-handoff-test.cpp) 可直接用宿主 C++17 编译。它验证动画交接仅在本轮启动窗口有效，并在完成后 2 秒停止等待。
