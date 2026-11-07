#!/bin/bash
# Add the original OHOS MMI service to a disposable RenderService root.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-input-service-test.sh PROJECT WORK OUTPUT BASIS_ROOT}")
work=$(realpath "${2:?}")
out=${3:?}
basis=$(realpath "${4:?}")
test ! -e "$out"
mkdir -p "$out/root"
cp -a "$basis/." "$out/root/"
root=$out/root/testroot
musl-gcc -static -O2 -Wall -Wextra "$project/ports/tools/input-device-permissions.c" \
    -o "$out/root/bin/input-device-permissions"
closure=$work/mmi-service-closure
if ! test -d "$closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$closure" \
        /system/bin/sa_main /system/lib64/libmmi-server.z.so
fi
rsync -a "$closure/." "$root/"
# Dependency closures include the original graphics frontend. Keep the
# already-built Aurora graphics route active after adding MMI dependencies.
ln -sfn /vendor/lib64/aurora-graphics/libEGL_impl.so "$root/system/lib64/libEGL.so"
for lib in libGLESv2.so libGLESv3.so; do
    ln -sfn /system/lib64/aurora-mesa/libGLESv2.so.2 "$root/system/lib64/$lib"
done
cp /mnt/harmonyos-source/system/system/profile/multimodalinput.json "$root/system/profile/"
client=$work/mmi-input-client-closure
if ! test -d "$client"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$client" \
        /system/lib64/ndk/libohinput.so
fi
rsync -a "$client/." "$root/"
ln -sfn /vendor/lib64/aurora-graphics/libEGL_impl.so "$root/system/lib64/libEGL.so"
for lib in libGLESv2.so libGLESv3.so; do
    ln -sfn /system/lib64/aurora-mesa/libGLESv2.so.2 "$root/system/lib64/$lib"
done
"$work/ohos-toolchain/ohos-cc" -O2 -Wall -Wextra \
    "$project/ports/tests/input-device-probe.c" -ldl \
    -o "$root/system/bin/aurora/input-device-probe"
for name in multimodalinput mmi_device_config.ini; do
    cp -a "/mnt/harmonyos-source/system/system/etc/$name" "$root/system/etc/"
done
mkdir -p "$root/data/service/el1/public/multimodalinput" "$root/data/service/el1/public/udev"
input_uid=$(awk -F: '$1=="input" {print $3}' "$root/system/etc/passwd")
input_gid=$(awk -F: '$1=="input" {print $4}' "$root/system/etc/passwd")
test -n "$input_uid" && test -n "$input_gid"
chown "$input_uid:$input_gid" "$root/data/service/el1/public/multimodalinput"
python3 - "$root" <<'PY'
from pathlib import Path
import json,sys
r=Path(sys.argv[1]);p=r/'system/etc/init.cfg';c=json.loads(p.read_text())
original=json.loads(Path('/mnt/harmonyos-source/system/system/etc/init/multimodalinput.cfg').read_text())
s=next(s for s in original['services'] if s['name']=='multimodalinput')
for key in ['critical','start-mode']:s.pop(key,None)
s.update(disabled=1,once=1,sandbox=0,console=1)
c['services'].append(s)
c['jobs'][1]['cmds'].insert(-1,'start multimodalinput')
p.write_text(json.dumps(c,indent=2)+'\n')
p=r.parent/'aurora-init';s=p.read_text()
s=s.replace('echo ORIGINAL_INIT_PID1_EXEC\n', '''# evdev DAC for the input service; actual Apple deployment uses uevent rules.
/bin/input-device-permissions INPUT_UID INPUT_GID
echo ORIGINAL_INIT_PID1_EXEC
'''.replace('INPUT_UID',str(next(l.split(':')[2] for l in (r/'system/etc/passwd').read_text().splitlines() if l.startswith('input:'))))
   .replace('INPUT_GID',str(next(l.split(':')[3] for l in (r/'system/etc/passwd').read_text().splitlines() if l.startswith('input:')))))
p.write_text(s)
p=r/'system/bin/aurora/service-test.sh';s=p.read_text()
s=s.replace('echo ORIGINAL_INIT_SERVICE_END', '''/system/bin/aurora/input-device-probe
echo "MMI_DEVICE_RESULT=$?"
echo MMI_SERVICE_SNAPSHOT
for p in /proc/[0-9]*; do
    name=$(busybox cat "$p/comm" 2>/dev/null)
    case "$name" in
        multimodalinput)
            echo "MMI_PROCESS pid=${p##*/}"
            busybox cat "$p/attr/current" "$p/status" | busybox head -n 13
            busybox ls -l "$p/fd" ;;
    esac
done
echo MMI_HILOG_BEGIN
busybox tail -c 45000 /tmp/hilograw.log
echo MMI_HILOG_END
echo ORIGINAL_INIT_SERVICE_END''')
p.write_text(s)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img.tmp"
mv "$out/initrd.img.tmp" "$out/initrd.img"
printf 'Prepared original MMI integration fixture %s\n' "$out/initrd.img"
