#!/bin/bash
# Add original composer IPC + RenderService to a generated managed test root.
# BASIS_ROOT is disposable output of prepare-managed-graphics-test.sh.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-render-service-test.sh PROJECT WORK OUTPUT BASIS_ROOT}")
work=$(realpath "${2:?}")
out=${3:?}
basis=$(realpath "${4:?}")
test ! -e "$out"
test -f "$basis/testroot/system/etc/init.cfg"
mkdir -p "$out/root"
cp -a "$basis/." "$out/root/"
root=$out/root/testroot
kernel_tree=${HMOS_KERNEL_TREE:-/usr/local/src/aurora/linux-aurora-d13466ebb8b5f2e2af3d2c8359f4c96e711a5679}
ashmem=$work/ashmem-service-build
mkdir -p "$ashmem"
cp "$project/ports/kernel/ashmem/"{Makefile,hmos_ashmem.c,ashmem-uapi.h} "$ashmem/"
make -C "$kernel_tree" LLVM=-19 M="$ashmem" modules > "$ashmem/build.log" 2>&1
expected_release=$(find "$out/root/bin" -name virtio-gpu.ko -exec modinfo -F vermagic {} \; | awk '{print $1}')
test "$(modinfo -F vermagic "$ashmem/hmos_ashmem.ko" | awk '{print $1}')" = "$expected_release"
cp "$ashmem/hmos_ashmem.ko" "$out/root/bin/"
llvm-strip-19 --strip-debug "$out/root/bin/hmos_ashmem.ko"
python3 - "$out/root/aurora-init" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
assert s.count('binder-setup\n')==1
s=s.replace('binder-setup\n','binder-setup\nbusybox insmod /bin/hmos_ashmem.ko\n')
p.write_text(s)
PY
closure=$work/render-service-closure
if ! test -d "$closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$closure" /system/bin/render_service
fi
rsync -a "$closure/." "$root/"
client_closure=$work/render-service-client-closure
if ! test -d "$client_closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$client_closure" \
        /system/lib64/librender_service_client.z.so
fi
rsync -a "$client_closure/." "$root/"
watcher_closure=$work/param-watcher-closure
if ! test -d "$watcher_closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$watcher_closure" \
        /system/bin/sa_main /system/lib64/libparam_watcher.z.so
fi
rsync -a "$watcher_closure/." "$root/"
mkdir -p "$root/system/profile"
cp /mnt/harmonyos-source/system/system/profile/param_watcher.json "$root/system/profile/"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra \
    "$project/ports/tests/render-service-probe.cpp" -ldl \
    -o "$root/system/bin/aurora/render-service-probe"
"$work/ohos-toolchain/ohos-platform-cxx" -O2 -Wall -Wextra \
    "$project/ports/tests/render-scene-probe.cpp" -ldl \
    -o "$root/system/bin/aurora/render-scene-probe"
bash "$project/ports/tools/build-composer.sh" "$project" "$work"
cp "$work/aurora-graphics-build/libdisplay_composer_vdi_impl.z.so" "$root/vendor/lib64/"
cp -aL /mnt/harmonyos-source/vendor/lib64/libdisplay_composer_driver_1.0.z.so "$root/vendor/lib64/"
cp -aL /mnt/harmonyos-source/vendor/lib64/libdisplay_composer_service_1.3.z.so "$root/vendor/lib64/"
# Reassert the port's dependency graph after copying the original closure.
cp "$work/aurora-graphics-build/libEGL_impl.so" "$root/vendor/lib64/aurora-graphics/"
ln -sfn libEGL_impl.so "$root/vendor/lib64/aurora-graphics/libEGL.so"
ln -sfn /vendor/lib64/aurora-graphics/libEGL_impl.so "$root/system/lib64/libEGL.so"
for lib in libGLESv2.so libGLESv3.so; do
    ln -sfn /system/lib64/aurora-mesa/libGLESv2.so.2 "$root/system/lib64/$lib"
done
for path in system/system/etc/graphic system/system/etc/graphics system/system/fonts sys_prod/etc/graphic; do
    if test -d "/mnt/harmonyos-source/$path"; then
        target=${path#system/}
        mkdir -p "$root/$target"
        cp -a "/mnt/harmonyos-source/$path/." "$root/$target/"
    fi
done
mkdir -p "$root/data/service/el0/render_service" "$root/data/system/RTShaderCache" \
    "$root/data/local/shader_cache/local/system" "$root/data/vendor/display"
"$work/hdf-core-source/framework/tools/hc-gen/build/hc-gen" \
    -o "$root/vendor/etc/hdfconfig/hdf_default.hcb" "$project/ports/config/hdf-graphics-test.hcs"
python3 - "$root" <<'PY'
from pathlib import Path
import json,sys
r=Path(sys.argv[1]); p=r/'system/etc/init.cfg'; c=json.loads(p.read_text())
env=[dict(name=k,value=v) for k,v in {
    'LD_LIBRARY_PATH':'/vendor/lib64/aurora-graphics:/vendor/lib64:/system/lib64/aurora-mesa',
    'GBM_BACKENDS_PATH':'/system/lib64/aurora-mesa/gbm','GALLIUM_DRIVER':'softpipe',
    'LIBGL_ALWAYS_SOFTWARE':'1','MESA_LOADER_DRIVER_OVERRIDE':'kms_swrast',
    'NEED_OPENGL':'1'}.items()]
c['services']+= [dict(name='composer_host',
    path=['/vendor/bin/hdf_devhost','-i','1','-n','composer_host','-p','0','-s','0'],
    uid='root',gid=['root'],secon='u:r:composer_host:s0',disabled=1,once=0,sandbox=0,console=1,env=env),
    dict(name='render_service',path=['/system/bin/render_service'],uid='root',gid=['root'],
    secon='u:r:render_service:s0',disabled=1,once=1,sandbox=0,console=1,env=env)]
watcher=json.loads(Path('/mnt/harmonyos-source/system/system/etc/init/param_watcher.cfg').read_text())['services'][0]
for k in ['critical','start-mode']:watcher.pop(k,None)
watcher.update(disabled=1,once=1,sandbox=0,console=1)
c['services'].append(watcher)
c['jobs'][0]['cmds']+=['setparam persist.sys.graphic.GpuApitype 0']
c['jobs'][1]['cmds'][3:3]=['start param_watcher','start composer_host','start render_service']
# Services are started by init jobs, not by an unlabeled shell trying to
# bypass the original parameter service's service-control permission checks.
p.write_text(json.dumps(c,indent=2)+'\n')
p=r/'system/bin/aurora/service-test.sh';s=p.read_text()
block='''
echo RENDER_SERVICE_INIT_START_BEGIN
sleep 3
sleep 12
/system/bin/aurora/render-service-probe
echo "RS_CLIENT_RESULT=$?"
/system/bin/aurora/render-scene-probe
echo "RS_SCENE_RESULT=$?"
echo RENDER_SERVICE_PROC_SNAPSHOT
for p in /proc/[0-9]*; do
    name=$(busybox cat "$p/comm" 2>/dev/null)
    case "$name" in
        composer_host|render_service)
            echo "RENDER_PROCESS name=$name pid=${p##*/}"
            busybox cat "$p/attr/current" "$p/status" | busybox head -n 13 ;;
    esac
done
echo RENDER_SERVICE_HILOG_LATE_BEGIN
busybox tail -c 42000 /tmp/hilograw.log
echo RENDER_SERVICE_HILOG_LATE_END
echo RENDER_SERVICE_INIT_START_END
'''
s=s.replace('echo ORIGINAL_INIT_SERVICE_END\n',block+'echo ORIGINAL_INIT_SERVICE_END\n')
p.write_text(s)
PY
(cd "$out/root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img.tmp"
mv "$out/initrd.img.tmp" "$out/initrd.img"
printf 'Prepared original RenderService diagnostic fixture %s\n' "$out/initrd.img"
