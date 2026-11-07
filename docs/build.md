# 构建环境与流程

当前公开的是开发源码与分项工具，完整镜像的一键重建尚未整理完成。以下说明脚本的真实输入和已知缺口；不能仅克隆仓库后运行全部脚本就得到已验收系统。

## 依赖与版本

| 材料 | 已记录版本 / 要求 |
| --- | --- |
| Aurora 内核 | `omacom/linux-aurora`，`d13466ebb8b5f2e2af3d2c8359f4c96e711a5679` |
| OpenHarmony AccessToken | `kernel_linux_5.10`，`4e38b39c0ca8cc8768061c5ba645c1b748c66d95` |
| Mesa | `7c86130d26cecd50c7af1ac01ff3fa1705d58045` |
| OHOS 工具链 | SDK 6.0.0.47 / API20 sysroot；脚本引用 Clang 15.0.4 resource 目录 |
| Linux 构建工具 | Clang/LLVM 19、CMake、Meson、Ninja、pkg-config、readelf、patch |
| 目标依赖 | OHOS ABI 的 libdrm 2.4.124、zlib 1.3.1、expat 2.7.1、libc++，llvmpipe 另需目标 LLVM 19 |
| macOS 虚拟验证 | Apple Silicon、QEMU AArch64/HVF、qemu-img、lsof、Python 3、Pillow |

Python 工具使用 `hashlib.file_digest`、`Path.is_relative_to` 等接口，建议 Python 3.12 或更新版本；原生构建器依赖支持 `tarfile` data filter 的版本。SDK 和图形依赖哈希见 [图形依赖记录](../ports/graphics/source-lock.json)，其中 Mesa backend 字段记录早期 softpipe 基线。

原装系统库、镜像、SDK、固件均由开发者自行提供。原始 ISO 读取工具针对 `L410_INSTALL` 及固定八段 SquashFS 布局，不支持任意 HarmonyOS 安装镜像。

## 工具链与图形

在 Linux 构建 VM 中，准备外部 SDK、原装 rootfs 与独立工作目录。下面使用占位路径，只展示工具调用顺序：

```sh
PROJECT="$PWD"
WORK=/path/to/build-work
SDK=/path/to/ohos-sdk
PLATFORM_ROOT=/path/to/original-root
mkdir -p "$WORK"
python3 ports/tools/prepare-ohos-toolchain.py \
  "$SDK" "$WORK/ohos-toolchain" \
  --prefix "$WORK/ohos-deps" --platform-root "$PLATFORM_ROOT"
```

先准备 OHOS ABI 的 libdrm/zlib/expat 等头文件与库到 `ohos-deps/system`。`build-buffer-vdi.sh` 和部分探针仍从只读原镜像挂载路径 `/mnt/harmonyos-source` 读取平台库，需要按脚本要求准备该路径。

```sh
bash ports/tools/build-ohos-softpipe.sh /path/to/mesa "$WORK"
bash ports/tools/build-buffer-vdi.sh "$PROJECT" "$WORK"
bash ports/tools/build-composer.sh "$PROJECT" "$WORK"
bash ports/tools/build-egl-window-bridge.sh "$PROJECT" "$WORK"
```

llvmpipe 是独立构建前缀，使用 `build-ohos-llvm.sh` 和 `build-ohos-llvmpipe.sh`。先将 `ports/tools/ohos-llvm19-toolchain.cmake` 与 `ohos-llvm-config.py` 复制到工作目录根，再构建目标 LLVM。CMake 文件按相邻 `ohos-toolchain/` 定位编译器。完整 LLVM 源码来源锁定与依赖构建流程仍待补齐，不能直接将 softpipe 命令替换为 llvmpipe 命令。

## 内核与 initramfs

在固定 Aurora 源码的独立构建树中检查并应用 `kernel/` 补丁，添加 `accesstokenid/` 驱动及 Kconfig/Makefile 接入，准备匹配 Apple 机器的初始 `.config` 后，使用 `build-test-kernel.sh` 构建。脚本会检查原有 Apple 选项未丢失。

已收录历史构建的 [test7.config](../ports/kernel/test7.config)，可作为同一基线的配置参考；补丁应用驱动脚本和模块安装交接流程尚未补齐。`kernel/ashmem/` 为独立模块，必须针对同一内核构建。

`build-native-initramfs.py --help` 列出匹配 modules、静态 bootstrap、固件、UUID 读取工具与输出目录等输入。原生构建器固定 test7；模块缺失或 vermagic 不匹配会拒绝构建。

## 分项验证

`tests/` 保留图形、Binder、页接口、服务就绪及输入探针；`prepare-*-test.sh` 构建对应磁盘外 fixture，`run-composer-vm.py` / `run-service-vm.py` 在 macOS 运行虚拟预检。每个 fixture 需要它对应的内核、模块、原装服务和依赖闭包。

建议顺序为：页契约与 IPC → allocator/composer → EGL 与真实 NativeWindow 队列 → 原装 init/HDF → RenderService → 桌面/输入 → 真机。以实际像素、交互和日志为结论，不将脚本退出成功当作全部功能通过。

## 预览打包

`package-local-preview.py --help` 提供外部磁盘、匹配内核/initrd/modules 包、历史证据和新输出目录参数。llvmpipe 额外要求显式传入 `--llvmpipe-evidence` 与 `--activation-evidence`，不再隐式访问旧工作目录。

该工具仍保留固定产物哈希、root UUID、历史证据文件名与目录布局检查，用于已验证候选的开发打包。升级产物需要重新验证并更新这些约束。打包本身不会授予新镜像验收状态。
