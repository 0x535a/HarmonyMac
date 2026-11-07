# Aurora 的鸿蒙内核接口

`accesstokenid/` 基于 OpenHarmony `kernel_linux_5.10` 的
`OpenHarmony-6.0-Release` 固定提交
`4e38b39c0ca8cc8768061c5ba645c1b748c66d95`：

- `drivers/accesstokenid/access_tokenid.c`
- `drivers/accesstokenid/access_tokenid.h`

保留原 GPL-2.0 许可与版权说明。当前移植显式包含现代内核的 cred/uaccess
头文件，并在本驱动定义原平台的 nwebspawn UID（3081）。

`ohos-access-token.patch` 以本项目固定 Aurora 源码为基线，将官方 token/ftoken
字段与 Binder 调用身份传递接入内核，包含 feature、access-token 和 sender-info
ioctl。与旧 L410 探针垫片不同，这里直接修改源码，不依赖内部函数地址和
kretprobe。是否满足完整鸿蒙安全策略仍需单独验收。

先对固定源码检查补丁，再复制驱动目录到 `drivers/accesstokenid/`，启用
`CONFIG_ACCESS_TOKENID=y`。这些步骤只应用到独立测试内核构建树。
原生启动的内核和 modules 必须使用同一构建版本，不加载旧麒麟模块。
