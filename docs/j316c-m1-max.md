# Apple M1 Max / J316c（MacBook Pro 16-inch, 2021）真机适配记录

更新于 2026-11-24。本页记录在 **Apple MacBook Pro (16-inch, M1 Max, 2021)**（SoC `t6001`，机型代号 `J316c`）上的真机启动与硬件适配进展，严格区分已真机验证项与待完成项。

## 测试环境

- **真机硬件**：Apple MacBook Pro (16-inch, M1 Max, 2021)，`t6001-j316c`，内屏 `3456×2160 @ 120 Hz`
- **内核**：`7.2.11-aurora-hmos-j514s-test7`（16K 页内核 + `user4k=on` + `hmos-userpages`）
- **根分区**：内置 Apple NVMe 独立 Linux ext4 分区（约 84.3 GB，未改动 macOS APFS 分区），无外置盘直接启动

## 已真机验证

| 模块 | 实机现象与证据 |
| --- | --- |
| 内置 NVMe 与 SMC 看门狗 | 加载 `nvme-apple` 与 `macsmc-reboot` 后，消除启动约 10 秒处的 SMC 硬复位；内置 NVMe 冷启动到 `bootevent.wms.ready=true` 约 **28.3 秒** |
| Asahi GPU 硬件加速与直送 | 实机渲染器为 **Apple M1 Max (G13C C0)**，`3456×2160 @ 120.0 FPS`（8.33 ms/帧），KMS 直送 `direct=1` |
| 原装锁屏与开发桌面 | `user_auth_host` 可正常解锁；在离线 `settingsdata.db` / `settingsdata_slave.db` 中同步写入 `settings.control.ota_old_version`、`buildversionrelease` 和 `is_ota_finished=1`（见 [prepare-oobe-settings.py](../ports/tools/prepare-oobe-settings.py) `--ota-version`）后，消除 `SCBOtaManager` 触发的 `HwStartupGuide` 弹窗；`SCBSmartDock6` 与静态壁纸正常显示 |
| 键盘与触控板 | `spi-hid-apple-of`、`hid-apple`、`hid-magicmouse` 正常加载；MMI 将 `Apple SPI Trackpad` 识别为触控板（`deviceType: 9`）并产生多指事件 |
| 屏幕背光硬件 | 内核暴露 `/sys/class/backlight/apple-panel-bl`（`max_brightness=500`），实机确认 `composer_host` 内置 `backlight.c` 与 `powermgr` 可真实修改背光亮度（`140 -> 300`） |
| BCM4387 蓝牙（内核侧） | PCIe `14e4:5f71` 由 `hci_bcm4377` 加载本机提取的 BCM4387 固件后，实机成功创建 `/sys/class/bluetooth/hci0` |
| BCM4387 Wi-Fi 与板载音频（内核探测） | PCIe `14e4:4433` 由 `brcmfmac` 加载 BCM4387 固件并启动 `ThreadX v5.6`；6 颗 `tas2764` 扬声器功放与 `cs42l84` 耳机编解码器完成 I2C 探测 |

## J316c 适配中定位的关键问题

1. **initramfs 需包含 `macsmc-reboot`**：未加载 `macsmc-reboot` 时，J316c 的 SMC 因收不到 `KERNEL_STARTED` 状态会在启动约 10 秒触发硬复位。已在 [build-native-initramfs.py](../ports/tools/build-native-initramfs.py) 的基础模块列表中补充 `macsmc-reboot`。
2. **initramfs 需提供 `/sbin/modprobe`**：`brcmfmac` 探测 BCM4387 时会通过内核 `request_module` 请求 `brcmfmac-wcc`；缺少 `/sbin/modprobe` 会导致 `brcmf_fwvid_request_module: mod=wcc: failed -2`。已在 [build-native-initramfs.py](../ports/tools/build-native-initramfs.py) 中补充 `sbin/modprobe -> ../bin/modprobe` 软链。
3. **`SCBOtaManager` 二次触发 OOBE**：当 `const.build.ver.physical` 未设置时，`SCBOtaManager` 会比对 `settings.control.ota_old_version` 与 `deviceInfo.displayVersion`（如 `OpenHarmony 6.0.0.110`），不匹配则仍会拉起 `HwStartupGuide`。已在 [prepare-oobe-settings.py](../ports/tools/prepare-oobe-settings.py) 中新增可选参数 `--ota-version`。
4. **板载音频 DMA 依赖 `apple-admac`**：仅加载 `snd-soc-macaudio` / `snd-soc-apple-mca` / `clk-apple-nco` / `tas2764` / `cs42l84` 时，会因缺少 `apple-admac` 报 `ASoC: can't create pcm Primary :-517`（`-EPROBE_DEFER`）。
5. **macOS 侧操作内置 NVMe ext4 分区**：Apple NVMe 物理扇区为 4K，在 macOS 下使用 `debugfs` / `e2fsck` 时应操作块设备 `/dev/diskXsY`，避免使用 `/dev/rdiskXsY`（1K 非对齐读会返回 `EINVAL`）。

## 尚未验证 / 已知限制

- 补充 `/sbin/modprobe` 后的 `wlan0` 入网实测，以及补充 `apple-admac` 后的 `card0` 实体放音与扬声器保护。
- OHOS 蓝牙 HDI（`blue_host` + `libbt_vendor.z.so`）与音频 HDI（`audio_host` + `libaudio_primary_impl.z.so`）在 J316c 上的联合验证。
- 部分应用内隐私同意弹窗点击无响应、`Settings` 退出码 `254`、以及浏览器 / 应用市场 / 小艺在 J316c 上的启动问题仍待继续排查。
- `EngineServiceAbility` 动态视频壁纸因硬件解码器配置失败（`errCode=3`）导致黑屏，目前依赖回退到静态壁纸。
