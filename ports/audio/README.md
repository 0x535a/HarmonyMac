# ALSA 播放与 J514 实验

Audio VDI 保留原装 AudioServer、HDF 和 vendor wrapper，默认开放受支持的 USB ALSA 播放。预期输入为 S16LE、双声道和设备支持的 44.1／48 kHz。原装 OHAudio、声音页音量与静音已在 QEMU USB Audio 测试。启动参数 `aurora.audio_disabled=1` 停用适配层输出。

J514 实机候选接入了内置 PCM、六声道分频和耳机切换，用户反馈板载音频不稳定。公开构建将这条路由作为显式实验配置。扬声器使用内核联锁和 −20 dB 上限，热反馈、实体耳机切换和持续播放仍需验收。虚拟六声道 PCM 与 DSP 结果见 [状态页](../../docs/status.md)。

## 构建

按 [构建说明](../../docs/build.md) 准备 OHOS 工具链。将匹配的 ALSA 头文件放在 `$WORK/audio-service-20261115/include/alsa`，OHOS ABI 的 `libasound.so` 放在 `$WORK/audio-service-20261115/libs`，然后运行 [VDI 构建器](../tools/build-audio-vdi.sh)。

```sh
bash ports/tools/build-audio-vdi.sh "$PROJECT" "$WORK"
```

`--j514-experimental` 编译实机候选路由，`--j514-fixture` 编译受虚拟启动标志约束的 snd_aloop 夹具。三个构建配置使用同一输出目录，留档时使用独立的 WORK 或先保存产物。实际生效仍需要匹配的原装 Audio HDF 配置、平台库和 Linux 驱动。

## 验证

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror ports/tests/j514-dsp-test.c -lm -o /tmp/harmonymac-j514-dsp-test
/tmp/harmonymac-j514-dsp-test
```

DSP 测试核对 44.1/48 kHz 下的六路分配、左右串扰和分频衰减。原装 ABI 的 [VDI 探针](../tests/audio-vdi-probe.c)、[J514 探针](../tests/j514-vdi-probe.c) 与 [OHAudio 播放探针](../tests/audio-renderer-probe.c) 需要相应的 OHOS 运行环境。J514 探针只接受命名为 AppleJ514 的 snd_aloop 夹具。
