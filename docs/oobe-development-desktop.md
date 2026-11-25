# 跳过 OOBE 进入开发桌面

项目的开发候选通过结束原装启动向导进入 SceneBoard 桌面。核心命令是：

```sh
/system/bin/aa force-stop com.huawei.hmos.hwstartupguide
```

它调用原装 AAFwk 的应用停止接口。它不会完成设备商业激活、生成 socIdCert、修改 TEE 证明结果或解除锁屏凭据。账号、云服务和应用市场是否可用仍取决于各自服务。本方法针对已有合法镜像的独立开发副本。

## 运行时方法

原装 init、SAMgr、AbilityManager、RenderService、WMS 和 SceneBoard 需要先正常运行。已有桌面候选通常会自行启动 SceneBoard；必要时可在 guest 的授权诊断 shell 中执行原装入口：

```sh
/system/bin/aa start -b com.ohos.sceneboard \
  -a com.ohos.sceneboard.MainAbility -m pc_sceneboard
```

等待实际向导进程出现，再执行 force-stop。过早调用时，向导尚未启动，随后仍会弹出；启动命令返回成功也不能证明桌面已经显示。

[enter-development-desktop.sh](../ports/tests/enter-development-desktop.sh) 提供独立的有界等待版本。在构建开发 rootfs 时将它放到 `/system/bin/aurora/`，按原有脚本的权限和标签安装，在已启动的 guest 中执行：

```sh
/system/bin/sh /system/bin/aurora/enter-development-desktop.sh --wait 180 --settle 10
```

脚本最多等待 180 秒；观察到真实向导进程后再等待 10 秒，然后调用原装 `aa`，并等待进程退出。`--wait 0` 适合进程已经存在的会话。未观察到进程返回 3，停止失败或仍运行返回 4。输出 `OOBE_GUIDE_STOPPED` 仅表示进程退出，桌面是否可见由实际画面确认。

宿主机可先查看计划，不访问 guest：

```sh
sh ports/tests/enter-development-desktop.sh --dry-run
```

已有 [native-desktop-start.sh](../ports/tests/native-desktop-start.sh) 包含同一类进程等待与停止逻辑。新增脚本是可单独使用的入口，没有自动改写既有启动配置。新配置应在原装服务就绪后运行一次，不应无限循环杀进程。

若向导再次被系统启动，重新观察进程和启动来源，再选择是否重跑。force-stop 没有保证跨重启永久生效。锁屏是另一条路径，结束 OOBE 不会替代正常解锁。

## 离线初始化标志：历史候选的配套处理

2026-11-04 的独立 direct-desktop 候选还修改了 SettingsData 的两个本地初始化标志。历史载荷中数据库位于：

```text
data/app/el1/0/database/com.ohos.settingsdata/entry/rdb/settingsdata.db
data/app/el1/0/database/com.ohos.settingsdata/entry/rdb/settingsdata_slave.db
```

两个库分别执行以下逻辑，历史用户 ID 为 100：

```sql
INSERT INTO SETTINGSDATA (KEYWORD, VALUE)
VALUES ('device_provisioned', '1')
ON CONFLICT(KEYWORD) DO UPDATE SET VALUE = excluded.VALUE;

INSERT INTO USER_SETTINGSDATA_SECURE_100 (KEYWORD, VALUE)
VALUES ('user_setup_complete', '1')
ON CONFLICT(KEYWORD) DO UPDATE SET VALUE = excluded.VALUE;
```

数据库路径、主从关系、用户 ID 和 schema 与具体镜像有关。缺表时应先核对版本，不要创建缺失的框架表或批量写入其他用户。

[prepare-oobe-settings.py](../ports/tools/prepare-oobe-settings.py) 将这部分处理整理成**生成新数据库副本**的工具。先正常关闭 VM／目标系统，确认没有进程仍使用该磁盘，在独立可回退层中挂载；保存原数据库及其 WAL／SHM／journal。输入必须是已正确 checkpoint、没有 SQLite sidecar 的离线副本，不能只复制正在使用的 `.db` 而丢弃 WAL。

例如，在 rootfs 外的工作目录准备两个输入副本后运行：

```sh
python3 ports/tools/prepare-oobe-settings.py settingsdata.db settingsdata.prepared.db --user-id 100
python3 ports/tools/prepare-oobe-settings.py settingsdata_slave.db settingsdata_slave.prepared.db --user-id 100
```

工具默认设置上述两项。它在内存中修改数据库并生成新文件，检查 SQLite 完整性，打印输入／输出 SHA256 和旧值；拒绝覆盖输入或已有输出，以及缺少必需表或带 journal sidecar 的输入。无关设置保留。它不挂载镜像、不替换 rootfs 中的数据库，也不安装启动脚本。

确认两个输出和备份完整后，再由自己的离线镜像流程安装，保留各原文件的 UID、GID、权限及 SELinux 标签。生成文件默认为 0600、属于运行工具的宿主用户，不能把这些元数据直接当作 guest 的正确值。所有数据库必须在系统停止时成组替换；启动前检查文件系统。

这些值只影响本地首次启动／初始化分支。历史候选设置后，向导仍可能重新进入，最终仍使用运行时 force-stop。不能将数据库修改称为永久关闭 OOBE 或商业激活成功。

### J316c 的 OTA 版本设置

J316c 真机测试中，`const.build.ver.physical` 未设置时，`SCBOtaManager` 会再次打开向导。若遇到这一情况，在上述两个数据库副本的命令中都加上 `--ota-version "<目标系统的 deviceInfo.displayVersion>"`。测试镜像的值为 `OpenHarmony 6.0.0.110`，应以实际目标系统为准。

该选项在全局和指定用户的现有目标表内将 `settings.control.ota_old_version`、`buildversionrelease` 设为传入版本，并将 `is_ota_finished` 设为 `1`。真机上写入这些值的结果见 [J316c 实测记录](devices/j316c-m1-max.md#已真机验证)。

## 预装清单尝试及其限制

历史候选还做过两项可回退尝试：从 `system/etc/app/install_list.json` 的 `install_list` 数组中移除 `app_dir` 为 `/system/app/HwStartupGuide` 的条目；将 `system/app/HwStartupGuide/HwStartupGuide.hap` 改名为同目录的 `HwStartupGuide.hap.disabled`，保留原内容。

下面的代码只生成新的安装清单文件，不写 rootfs，也不收录厂商原清单：

```sh
python3 - install_list.json install_list.development.json <<'PY'
import json
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
data = json.loads(source.read_text())
rows = data['install_list']
matches = [row for row in rows if row.get('app_dir') == '/system/app/HwStartupGuide']
if len(matches) != 1:
    raise SystemExit('Expected exactly one HwStartupGuide entry; inspect this image first')
data['install_list'] = [row for row in rows if row not in matches]
with output.open('x') as file:
    json.dump(data, file, ensure_ascii=False, indent=2)
    file.write('\n')
PY
```

这影响预装扫描输入，不等于卸载已经注册到 BMS／数据区的包。历史测试在清单／HAP 处理后仍需开机结束向导，因此不推荐把删除包当作运行时方法的替代。还可能影响恢复初始化流程；仅在独立副本上复现，并保留原清单和 HAP 路径。

## 启动时序与回退

历史 `noobeboot110` 脚本在 guest uptime 到 116 秒后调用一次 force-stop，随后等待 3 秒；这是当时固定预览包的时序，不是所有机器的固定阈值。已有 [预览说明](../ports/release/README-direct-desktop.md) 中的“约 120 秒”指该版本。现在的独立 helper 按真实进程等待，仍须结合目标服务就绪状态。

运行时 force-stop 没有直接的“撤销停止”操作；正常重启后可由原启动流程重新进入向导。若需要完整恢复 OOBE，停机后还原本轮修改过的数据库、sidecar、预装清单、HAP 名称和启动脚本，保留原元数据。恢复整个修改前的独立 overlay／快照更可靠；不要把已有使用数据随意覆盖成旧库。

## 证据范围与源码验证

历史 VM 和用户实测确认过结束向导后进入桌面、打开应用窗口。2026-11-16 的运行流程仍使用同一原装 force-stop 接口。本次提交是方法和代码整理，没有重新启动 VM 或写入任何活动镜像。

新增离线工具仅通过合成 SQLite fixture 与宿主 shell dry-run 验证，未声称新版工具已经在每个商业镜像或 Apple 真机部署：

```sh
python3 scripts/test-oobe-tools.py
python3 scripts/check-repository.py
```
