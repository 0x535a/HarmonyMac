# Aurora + HarmonyOS Mac 桌面开发预览

本包按用户确认的“可进入桌面、应用可打开窗口”验收。实际包内测试结果见 evidence；商业激活、应用内部功能和 Apple 真机不在该结论内。

需要 Apple Silicon macOS、Python 3、QEMU 和 lsof。默认4 GiB内存、4 vCPU、1920×1200显示，相对鼠标和虚拟键盘、用户态网络。

1. 在包目录执行 `shasum -a 256 -c SHA256SUMS`。
2. 执行 `python3 start.py --window` 打开macOS窗口；约120秒自动结束向导进入桌面。`--dry-run`查看配置。无`--window`为无窗口诊断模式。
3. 启动器打印runs目录；等待serial.log出现LOCAL_ROOT_INTERACTIVE_READY。通过`python3 tools/guest-console.py RUN '命令'`执行guest命令。
4. `python3 stop.py RUN`正常同步关机，等待启动器退出。
5. `python3 collect.py RUN evidence.tar.gz`导出日志与截图。
6. 停机后`python3 rollback.py`查看回退计划，`--apply`归档原运行层并重建干净运行层，保留虚拟机UUID。

rootfs.qcow2是独立基础盘；启动时创建runtime/system.qcow2持久层。不要同时启动或用其他工具打开同一运行盘。

已知限制：桌面壁纸黑、部分图标不完整；图库停在首次使用页面，浏览器只验证窗口；文件管理器退出。OOBE通过启动脚本在116秒后调用原aa force-stop结束，启动期间可能短暂出现。商业socIdCert/TEE失败仍保留，不视为激活成功。Tablet绝对坐标未经验证，使用相对鼠标。

本包仅在虚拟硬件运行，内核test7、OHOS ABI llvmpipe。未访问U393，未修改EFI/m1n1/GRUB；Apple内屏、GPU及内置输入需独立原生验证。
