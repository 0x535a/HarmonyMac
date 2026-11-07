# 运行与原生启动

## macOS 开发预览

仓库的 `ports/release/` 是预览包工具源码，需要配套镜像、内核、initrd 和 manifest。镜像网盘链接将集中在根 README 中。

拿到完整预览包后，在**预览包目录**中执行：

```sh
shasum -a 256 -c SHA256SUMS
python3 start.py --dry-run
python3 start.py --window
```

依赖 Apple Silicon macOS、QEMU、Python 3 和 lsof。以具体包的 README 和 manifest 为准：普通预览与 direct-desktop 候选的启动行为不同。

启动器创建 `runtime/system.qcow2` 持久可写层和 runs 记录。避免并发打开同一运行盘。使用 `python3 stop.py RUN` 请求正常关机，确认退出后再挂载或修改磁盘；`collect.py` 收集运行记录，`rollback.py` 默认查看回退计划，`--apply` 才执行回退。

诊断控制台、虚拟网络和结束 OOBE 的处理属于开发配置。direct-desktop 候选结束向导后进入桌面，不表示设备已激活；部分应用仅验证能打开窗口。

## 原生部署

当前仓库没有面向任意机器的一键安装器。保留的原生脚本对应已有 J514S/test7 验证基线：

- `initramfs/native-stage-init` 约束历史根 UUID 和内核版本，默认只读 rescue；boot 模式还要求 rootfs 上的准备标记。
- `initramfs/native-log-collect` 含原测试环境的日志保存目标和设备约束，需要审查后适配。
- `tools/populate-rootfs.sh` 写入已经挂载的独立 ext4，默认 UUID 是原测试分区；使用其他目标必须显式传入其实际 UUID。
- `tools/adapt-local-root.py` 与服务配置工具修改离线 rootfs，不能把目录参数指向正在运行的系统根。

原 U393 的 GPT/分区重排脚本未收录，EFI、GRUB 和 m1n1 的个人备份也未收录。移植者需要为自己的设备准备独立根分区、匹配固件及可回退启动项，重新核对磁盘身份、UUID 和脚本中的固定参数。

原 L410 ISO 的安装器会清空目标整盘，不能直接作为 Apple 平台或已有分区布局的安装方法。构建流程只使用只读提取的载荷。

## 日志与反馈

原生失败启动可由 bootstrap 自动保存日志；现有实现优先使用外置测试盘，回退日志目标包含内置 ESP。采用该机制前应核对自己设备的保存位置。公开反馈前移除设备标识、个人路径、网络地址和账号信息。

报告问题时提供机型、源码提交、镜像/内核版本、是否原生或 HVF、复现步骤，以及实际画面与关键日志。固件加载、服务就绪与桌面可见应分别记录。
