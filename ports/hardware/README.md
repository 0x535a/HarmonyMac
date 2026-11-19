# Aurora 硬件与系统服务适配

这里的后端将 Linux 设备接口接到原装鸿蒙服务。实际 Apple 硬件、虚拟设备和纯宿主测试的验收范围见 [状态页](../../docs/status.md)。

| 子系统 | 实现与构建入口 |
| --- | --- |
| 电池、背光与键盘照明 | `power_supply`、`backlight`、`keyboard-backlight` 读写标准 Linux 类设备，照明热键走原装 MMI。构建见 [基础硬件](../tools/build-hardware.sh) 和 [键盘照明](../tools/build-keyboard-backlight.sh) |
| Linux 模块 | `module-loader` 通过 kmod 处理内核设备事件，其构建要求 OHOS ABI 的 kmod 34.2 与匹配模块树。[模块工具构建](../tools/build-module-loader.sh) |
| 外置存储 | 存储启动脚本保留原装服务与沙箱，处理 USB 子挂载的传播。exFAT 辅助库使用内核原生页上的 pipe/splice 桥接 FUSE 传输，保留原有用户页保护。[exFAT 库构建](../tools/build-exfat-subpage-transport.sh) |
| Wi-Fi | HAL 包装读取原装扫描结果，将驱动报告的年龄换算到框架使用的开机单调时钟，保留扫描、关联和电源操作。[时钟适配构建](../tools/build-wifi-scan-clock.sh) |
| 蓝牙 | H4 数据通过 Linux HCI 用户通道交给实际控制器，固件由 Linux 加载。构建包含库和 API 探针。[蓝牙构建](../tools/build-bluetooth-linux-hci.sh) |
| 温度与 IIO | thermal/hwmon 通过只读 HDI 提供真实温度，IIO 通过原装 SensorService 发布光线事件。风扇、性能和充电策略未接入。两项构建需要分别传入匹配的 OpenHarmony HDI 生成目录或参考树，另需脚本列出的原装平台依赖。[温度构建](../tools/build-thermal-readonly.sh)、[IIO 构建](../tools/build-iio-sensor.sh) |
| 相机 | videobuf2 补丁覆盖 16 KiB 内核页中尾部不足一页的 4 KiB 进程映射，探针核对 V4L2 MMAP/DMA-BUF 及原装 CameraService 枚举。[内核补丁](../kernel/videobuf2-vmalloc-user-granule.patch)、[V4L2 探针](../tests/apple-isp-capture-probe.c) |
| 软件锁屏认证 | 复用 OpenHarmony UserAuth 的便携实现，提供原子化文件更新和 HDF 注册适配。构建从外部 OpenHarmony 参考树与原装 rootfs 读取依赖。[UserAuth 构建](../tools/build-userauth-reference.sh) |

这些构建脚本用于 Linux 构建环境。准备 [OHOS 工具链](../../docs/build.md) 后，按脚本参数提供匹配版本的公开接口头、自动生成的 HDI 传输代码及外部原装库。Wi-Fi 需要 2.0 WLAN Chip 生成头，编译时会核对扫描结果结构和关键字段。

用于无线适配的 [Asahi 固件转换模块](../firmware/asahi_vendorfw/SOURCE.json) 保留上游源文件与哈希，可用 [转换工具](../tools/prepare-apple-wireless-firmware.py) 从现有 macOS 固件生成本地文件。Wi-Fi、蓝牙和 AVD 的固件文件均由开发者提供。Wi-Fi 监管数据库继续使用内核原有的签名链。

宿主测试可以检查背光、键盘照明、电池、无线扫描时钟、温度、IIO 和内核事件解析。测试夹具使用合成目录或虚拟设备。它们的通过记录不作为实体传感器或无线电验收。
