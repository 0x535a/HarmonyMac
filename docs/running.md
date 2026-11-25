# 运行与原生启动

## macOS 开发预览

仓库的 `ports/release/` 是预览包工具源码，需要配套镜像、内核、initrd 和 manifest。已有镜像的下载入口见 [项目 README](../README.md#镜像下载)。

拿到完整预览包后，在**预览包目录**中执行：

```sh
shasum -a 256 -c SHA256SUMS
python3 start.py --dry-run
python3 start.py --window
```

依赖 Apple Silicon macOS、QEMU、Python 3 和 lsof。以具体包的 README 和 manifest 为准：普通预览与 direct-desktop 候选的启动行为不同。

启动器创建 `runtime/system.qcow2` 持久可写层和 runs 记录。避免并发打开同一运行盘。使用 `python3 stop.py RUN` 请求正常关机，确认退出后再挂载或修改磁盘；`collect.py` 收集运行记录，`rollback.py` 默认查看回退计划，`--apply` 才执行回退。

诊断控制台、虚拟网络和结束 OOBE 的处理属于开发配置。direct-desktop 候选结束向导后进入桌面，不表示设备已激活；部分应用仅验证能打开窗口。

具体命令、有界等待脚本、离线初始化标志及回退见 [跳过 OOBE 进入开发桌面](oobe-development-desktop.md)。

## 原生部署

仓库的原生脚本源自 J514S 的 test7／test8 验证基线。部署时为自己的设备准备独立根分区、匹配固件及可回退启动项，并核对脚本中的磁盘身份、UUID、内核和设备树。

- `initramfs/native-stage-init` 约束历史根 UUID 和内核版本，默认只读 rescue。boot 模式还要求 rootfs 上的准备标记及有效的系统电池读数。
- `initramfs/native-log-collect` 含原测试环境的日志保存目标和设备约束，需要审查后适配。
- `tools/populate-rootfs.sh` 写入已经挂载的独立 ext4，默认 UUID 是原测试分区；使用其他目标必须显式传入其实际 UUID。
- `tools/adapt-local-root.py` 与服务配置工具修改离线 rootfs，不能把目录参数指向正在运行的系统根。

原 L410 ISO 的安装器会清空目标整盘，不能直接作为 Apple 平台或已有分区布局的安装方法。构建流程只使用只读提取的载荷。

### J514S 外置根盘

原生测试使用 U393 外置 ext4 根盘。Type-C 显示路由的驱动加载顺序见 [架构说明](architecture.md#原生启动经验)。

现有 R1 的电池时序修复可从匹配输入离线重建，使用 [R1 initramfs 修订工具](../ports/tools/patch-r1-native-battery-initrd.py)。实验性的全高内屏使用 [appledrm initramfs 工具](../ports/tools/build-appledrm-notch-experiment.py)，只在匹配 initramfs 中首次加载 appledrm 时传入 `show_notch=1`。两者的 `--help` 列出输入和输出；构建过程仅生成文件。

[孵化服务恢复工具](../ports/tools/add-spawn-services.py) 从开发者提供的原装 init 目录读取三个服务，输出新的紧凑 init.cfg。应核对服务、权限和回退后再集成。恢复定义后的单屏版仍有浏览器启动故障，见 [J514S 实测记录](devices/j514s-m3-pro.md)。

原 U393 的 GPT/分区重排脚本和 EFI、GRUB、m1n1 的个人备份未收录。

### J316c 内置 NVMe

原生测试使用内置 Apple NVMe 上独立的 ext4 根分区。早期启动需要匹配的 `nvme-apple` 和 `macsmc-reboot` 驱动。使用仓库构建器产物的冷启动和 BCM4387 入网仍待验证。SMC 时序、无线模块选项及 macOS 下的 ext4 访问记录见 [J316c 实测记录](devices/j316c-m1-max.md)。

## 日志与反馈

原生失败启动可由 bootstrap 自动保存日志；现有实现优先使用外置测试盘，回退日志目标包含内置 ESP。采用该机制前应核对自己设备的保存位置。公开反馈前移除设备标识、个人路径、网络地址和账号信息。

报告问题时提供机型、源码提交、镜像/内核版本、是否原生或 HVF、复现步骤，以及实际画面与关键日志。固件加载、服务就绪与桌面可见应分别记录。
