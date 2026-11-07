# Aurora + HarmonyOS 本地开发预览

这是 macOS/HVF 与 UTM 构建验证产物。完整桌面、基础应用、厂商硬件激活和 Apple 真机行为尚未完成验收；以 manifest.json 的 qualification 和对应证据为准。

需要 macOS Apple Silicon、Python 3、QEMU（包含 qemu-img、qemu-system-aarch64）和 lsof。默认使用 4 GiB 内存、4 vCPU、1920×1200 virtio 显示、虚拟键鼠及 QEMU 用户态以太网。

1. 校验：`shasum -a 256 -c SHA256SUMS`。
2. 查看启动命令：`python3 start.py --dry-run`。
3. 启动：`python3 start.py`。它创建 runtime/system.qcow2 持久可写层，保留独立 rootfs.qcow2 基础镜像。
4. 等待对应 runs 目录的 serial.log 出现 LOCAL_ROOT_INTERACTIVE_READY，再通过 `python3 tools/guest-console.py RUN '命令'` 使用临时诊断端口。屏幕可用 `python3 tools/guest-pointer.py RUN --capture frame.ppm` 抓取。当前 Tablet 绝对指针未经校准，使用原装 UITest 进行坐标输入。
5. 正常停止：`python3 stop.py RUN`。只有核实 guest poweroff 后才可挂载或修改运行盘。
6. 收集日志：`python3 collect.py RUN evidence.tar.gz`。
7. 回退：先停止虚拟机，运行 `python3 rollback.py` 查看操作，再运行 `python3 rollback.py --apply`；旧可写层会保留为归档。

默认启动的是原装鸿蒙 init/PID 1 和系统服务。Debian 仅用于构建。诊断服务、虚拟网络和证据标记属于开发预览配置。

原装向导尚未完成厂商激活。较新的 test7/llvmpipe 候选已通过真实 JIT、像素、原装队列和向导页面刷新预检；DCM 接通后，SoC 证明请求在 TEEC 初始化处失败，无法取得 socIdCert。具体内核、渲染器和验收范围以 manifest.json 及 evidence 为准。本包不包含厂商私钥或已激活身份，正式安全策略与真机外设留待对应验收。

本包不读写 U393、EFI、m1n1 或 GRUB。kernel/native-<test6或test7>-package.tar.zst 是匹配版本的配置、modules/dtbs 交接材料；原生部署需另行核对机器身份、分区和启动回退方案。

较新的test7集成预览会在runtime/virtual-machine.json为本运行实例创建并保存UUID；正常重启和rollback保留它，原装init由实际虚拟磁盘serial计算UDID。不同VM实例应使用独立runtime目录；不要复制其他设备身份。--dry-run不创建身份或运行盘。包内kernel、initrd与模块使用已验证固定元组，不能混用。
