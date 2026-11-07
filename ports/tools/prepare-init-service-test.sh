#!/bin/bash
# Minimal, diskless original-init fixture; not the full migrated rootfs.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-init-service-test.sh PROJECT WORK OUTPUT [MODULES]}")
work=$(realpath "${2:?}")
out=${3:?}
modules=${4:-$work/kernel-test6-package}
bash "$project/ports/tools/prepare-hdf-service-test.sh" "$project" "$work" "$out" "$modules"
root=$out/root
closure=$work/init-service-closure
if ! test -d "$closure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$closure" /system/bin/init
fi
rsync -a "$closure/." "$root/testroot/"
# Optional historical memmgr diagnostics. The real queue mapping failure was
# the VDI's missing virAddr writeback, not a requirement for Kirin memcg stubs.
# These placeholders are never part of the default fixture or final rootfs.
if [ "${HMOS_DIAG_MEMMGR:-0}" = 1 ]; then
memclosure=$work/memmgr-ressched-closure
if ! test -d "$memclosure"; then
    python3 "$project/ports/tools/prepare-core-closure.py" /mnt/harmonyos-source "$memclosure" \
        /system/bin/sa_main /system/lib64/libmemmgrservice.z.so /system/lib64/libresschedsvc.z.so
fi
rsync -a "$memclosure/." "$root/testroot/"
mkdir -p "$root/testroot/system/etc/memmgr" "$root/testroot/system/profile" \
    "$root/testroot/data/service/el1/public/memmgr"
cp -a /mnt/harmonyos-source/system/system/etc/memmgr/. "$root/testroot/system/etc/memmgr/"
cp -a /mnt/harmonyos-source/system/system/profile/memmgrservice.json "$root/testroot/system/profile/"
# Product-side configs: memmgr Init fails without sys_prod's minisystem_config
# (GetRealConfigPath searches /sys_prod/etc before /system/etc). Bring the
# small product config dirs relevant to graphics/memory/scheduling.
for d in memmgr graphic display frame_aware_sched init; do
    if test -d "/mnt/harmonyos-source/sys_prod/etc/$d"; then
        mkdir -p "$root/testroot/sys_prod/etc/$d"
        cp -a "/mnt/harmonyos-source/sys_prod/etc/$d/." "$root/testroot/sys_prod/etc/$d/"
    fi
done
# Diagnostic relaxation: disable every reclaim/quota strategy so memmgr's
# ReclaimStrategyManager can init without the Kirin memcg-v1/hyperhold/zswapd
# kernel interfaces (absent on aurora). Documented test-only config delta.
for cfg in "$root/testroot/sys_prod/etc/memmgr/memmgr_config.xml" \
           "$root/testroot/system/etc/memmgr/memmgr_config.xml"; do
    if test -f "$cfg"; then
        sed -i -e 's|<enable>true</enable>|<enable>false</enable>|g' \
               -e 's|<betaQuotaEnable>true</betaQuotaEnable>|<betaQuotaEnable>false</betaQuotaEnable>|g' \
               "$cfg"
    fi
done
cat > "$root/testroot/system/profile/ressched-min.json" <<'JSON'
{
    "process": "resource_schedule_service",
    "systemability": [
        {
            "name": 1901,
            "libpath": "libresschedsvc.z.so",
            "run-on-create": true,
            "distributed": false,
            "dump_level": 1
        }
    ]
}
JSON
fi
mkdir -p "$root/testroot/system/etc/init" "$root/testroot/tmp" "$root/testroot/dev" \
    "$root/testroot/data/service/el0/startup/init" "$root/testroot/dev/unix/socket"
for tree in system/system vendor; do
    mkdir -p "$root/testroot/${tree#system/}/etc/param"
    cp -a "/mnt/harmonyos-source/$tree/etc/param/." "$root/testroot/${tree#system/}/etc/param/"
done
ln -sfn /system/etc "$root/testroot/etc"
cp "$work/bootstrap-busybox-fullmod/busybox" "$root/testroot/system/bin/aurora/busybox"
cp "$root/bin/service-log-tools" "$root/testroot/system/bin/aurora/service-log-tools"
cp "$root/bin/binder-setup" "$root/testroot/system/bin/aurora/binder-setup"
"$work/ohos-toolchain/ohos-cc" -O2 "$project/ports/tests/token-dump.c" \
    -o "$root/testroot/system/bin/aurora/token-dump"
# Original native-token library plus any missing runtime dependencies, so the
# probe exercises the same GetAccessTokenId code path init uses.
for name in libnativetoken_shared.z.so libsec_shared.z.so \
            libhisysevent.z.so librestorecon.z.so libc++.so; do
    if ! test -e "$root/testroot/system/lib64/$name"; then
        for sub in "" platformsdk/ chipset-pub-sdk/; do
            src="/mnt/harmonyos-source/system/system/lib64/$sub$name"
            if test -e "$src"; then cp -aL "$src" "$root/testroot/system/lib64/$name"; break; fi
        done
        test -e "$root/testroot/system/lib64/$name"
    fi
done
"$work/ohos-toolchain/ohos-cc" -O2 "$project/ports/tests/nativetoken-probe.c" \
    "$root/testroot/system/lib64/libnativetoken_shared.z.so" \
    -o "$root/testroot/system/bin/aurora/nativetoken-probe"
# Display mapper HDI: the queue probe dlopens libmapper_service_1.0; l410 has
# no HDF mapper node, so the client wrapper falls back to the passthrough
# implementation. Bring the full display HDI client/stub/passthrough set so
# the dependency walk (mapper, metadata, commontype, composer stubs) settles
# in one pass.
vsrc=/mnt/harmonyos-source/vendor/lib64
mkdir -p "$root/testroot/vendor/lib64/chipsetsdk" "$root/testroot/vendor/lib64/passthrough"
copy_vendor_lib() {
    if test -e "$vsrc/$1" && ! test -e "$root/testroot/vendor/lib64/$1"; then
        cp -aL "$vsrc/$1" "$root/testroot/vendor/lib64/$1"
    fi
}
for f in libmapper_driver_1.0.z.so libmetadata_driver_1.1.z.so \
         libdisplay_buffer_stub_1.0.z.so libdisplay_buffer_stub_1.1.z.so \
         libdisplay_buffer_stub_1.2.z.so libdisplay_buffer_stub_1.3.z.so \
         libdisplay_commontype_stub_1.0.z.so libdisplay_commontype_stub_1.1.z.so \
         libdisplay_commontype_stub_2.0.z.so libdisplay_commontype_stub_2.1.z.so \
         libdisplay_commontype_stub_2.2.z.so \
         libdisplay_composer_stub_1.0.z.so libdisplay_composer_stub_1.1.z.so \
         libdisplay_composer_stub_1.2.z.so libdisplay_composer_stub_1.3.z.so \
         libdisplay_composer_service_1.3.z.so libcomposer_buffer_cache.z.so \
         libhdi.z.so libhitrace_meter.so libutils.z.so \
         libhdf_ipc_adapter.z.so libhdf_utils.z.so libhdf_base.z.so \
         libhdf_host.z.so libhdf_driver_common.z.so; do
    copy_vendor_lib "$f"
done
for f in chipsetsdk/libmapper_service_1.0.z.so chipsetsdk/libmapper_service_1.2.z.so \
         chipsetsdk/libmetadata_service_1.1.z.so \
         passthrough/libmapper_service_1.0.z.so passthrough/libmapper_service_1.2.z.so \
         passthrough/libmapper_service_1.3.z.so passthrough/libmetadata_service_1.1.z.so; do
    copy_vendor_lib "$f"
done
"$work/ohos-toolchain/ohos-cc" -O2 "$project/ports/tests/exec-child-probe.c" \
    -o "$root/testroot/system/bin/aurora/exec-child-probe"
python3 - "$root/testroot/system/etc/init.cfg" <<'PY'
import json,sys
config = {'jobs': [{'name': 'pre-init', 'cmds': [
                            'mkdir /data/service/el0/access_token 0700 root root',
                            'load_access_token_id ',
                            'setparam persist.sys.hilog.binary.on false',
                            'setparam persist.sys.hilog.loggable.global D']},
                   {'name': 'post-init', 'cmds': ['start aurora-service-test']}],
          'services': [{'name': 'aurora-service-test',
                        'path': ['/system/bin/sh', '/system/bin/aurora/service-test.sh'],
                        'uid': 'root', 'gid': ['root'], 'once': 1,
                        'caps': ['SYS_BOOT', 'SYS_ADMIN', 'SYS_PTRACE', 'DAC_OVERRIDE', 'DAC_READ_SEARCH'],
                        'disabled': 1, 'sandbox': 0}]}
open(sys.argv[1], 'w').write(json.dumps(config, indent=2)+'\n')
PY
cat > "$root/testroot/system/bin/aurora/service-test.sh" <<'EOF'
#!/system/bin/sh
exec >/dev/console 2>&1
export PATH=/system/bin/aurora:/system/bin
echo ORIGINAL_INIT_SERVICE_BEGIN
busybox readlink /proc/1/exe
param get const.ohos.fullname
param get const.product.software.version
echo OHOS_INIT_TOKEN_DIAG_BEGIN
/system/bin/aurora/token-dump shell-service
busybox ls -la /data/service/el0/access_token/ 2>&1
echo "----nativetoken.json----"
busybox cat /data/service/el0/access_token/nativetoken.json 2>&1
echo OHOS_INIT_TOKEN_DIAG_END
# Raw hilog datagram capture: hilog-sockets dump binds the same sockets init
# would hand to hilogd, then echoes every log datagram verbatim to a file.
# The L410 hilogd dict-compression pipeline cannot be decoded offline, so we
# read the wire directly; the Mac-side `strings` pass extracts the text.
/system/bin/aurora/hilog-sockets dump >/tmp/hilograw.log 2>&1 &
sleep 1
samgr >/tmp/samgr.log 2>&1 &
sleep 2
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -tt \
    -e trace=openat,ioctl -o /tmp/devmgr.strace \
    /vendor/bin/hdf_devmgr >/tmp/devmgr.log 2>&1 &
sleep 2
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -tt \
    -e trace=openat,ioctl -o /tmp/allocator.strace \
    /system/bin/sh -c 'LD_LIBRARY_PATH=/vendor/lib64:/system/lib64/aurora-mesa exec /vendor/bin/hdf_devhost -i 0 -n allocator_host -p 0 -s 0' >/tmp/allocator.log 2>&1 &
sleep 2
if test -f /system/profile/memmgrservice.json; then
    /system/bin/sa_main /system/profile/ressched-min.json >/tmp/ressched.log 2>&1 &
    sleep 3
    /diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -tt \
        -o /tmp/memmgr.strace \
        /system/bin/sa_main /system/profile/memmgrservice.json >/tmp/memmgr.log 2>&1 &
    sleep 4
fi
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f -tt \
    -e trace=ioctl,connect,openat,poll,epoll_pwait,nanosleep,mmap,munmap \
    -o /tmp/queue.strace \
    /system/bin/aurora/native-queue-probe >/tmp/queue.log 2>&1
echo "OHOS_INIT_QUEUE_RESULT=$?"
for name in samgr devmgr allocator ressched memmgr queue; do
    echo "OHOS_INIT_LOG_$name"
    service-log-tools dump /tmp/$name.log
done
echo OHOS_INIT_DEVMGR_STRACE
busybox grep -E 'hcb|hcs|liballocator|vdi|devhost|SET_CONTEXT|binder' /tmp/devmgr.strace 2>/dev/null | busybox tail -n 40
echo OHOS_INIT_ALLOCATOR_STRACE
busybox grep -E 'liballocator|vdi|/dev/dri|card|dma_heap|hcs|hcb|binder' /tmp/allocator.strace 2>/dev/null | busybox tail -n 50
echo OHOS_INIT_QUEUE_STRACE_TAIL
busybox grep -E 'vdi_impl|/dev/dri|dma_heap|card0|BINDER_WRITE|mmap|munmap|Restarting|SIGALRM' /tmp/queue.strace 2>/dev/null | busybox tail -n 40
echo OHOS_INIT_QUEUE_LIB_HITS
busybox grep -E 'openat.*\.so.* = [0-9]' /tmp/queue.strace 2>/dev/null | busybox tail -n 30
echo OHOS_INIT_QUEUE_METADATA_OPENS
busybox grep 'metadata' /tmp/queue.strace 2>/dev/null | busybox head -n 30
echo OHOS_INIT_MEMMGR_STRACE
busybox grep -E 'SIG|memcg|proc/sys|eswap|zswap|kswapd|openat.*= -1' /tmp/memmgr.strace 2>/dev/null | busybox tail -n 45
echo OHOS_INIT_HILOG_RAW_BEGIN
busybox tail -c 24000 /tmp/hilograw.log 2>/dev/null
echo OHOS_INIT_HILOG_RAW_END
echo ORIGINAL_INIT_SERVICE_END
busybox poweroff -f
EOF
cat > "$root/aurora-init" <<'EOF'
#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo 8 >/proc/sys/kernel/printk
mkdir -p /dev/binderfs /dev/unix/socket
# Stub the Kirin memcg-v1 interface files memmgr probes at init; the aurora
# kernel has no memcg v1, and the strategies reading them are disabled in the
# fixture config. Diagnostic-only placeholders.
if test -f /testroot/system/profile/memmgrservice.json; then
mkdir -p /dev/memcg/isulad /dev/memcg/stratovirt /dev/cgroup /dev/stune/system-background /dev/cpuset/system-background /dev/blkio/system-background
for f in cgroup.procs memory.eswap_info memory.zswapd_pressure memory.avail_lru_anon_wmark memory.space_info memory.total_info_per_app memory.psi_health_info memory.zswapd_presure_show memory.app_score memory.name memory.zswapd_single_memcg_param memory.reclaim memory.swapon memory.use_priority_oom memory.soft_limit_in_bytes memory.limit_in_bytes memory.usage_in_bytes memory.stat memory.pressure_level; do
    test -e /dev/memcg/$f || echo 0 > /dev/memcg/$f
    chmod 666 /dev/memcg/$f 2>/dev/null
done
for f in /dev/stune/system-background/cgroup.procs /dev/cpuset/system-background/cgroup.procs /dev/blkio/system-background/cgroup.procs; do
    test -e $f || echo > $f
done
fi
mount -t binder binder /dev/binderfs
binder-setup
busybox insmod /bin/virtio_dma_buf.ko
busybox insmod /bin/virtio-gpu.ko
mount --bind /dev /testroot/dev
mount --bind /dev/binderfs /testroot/dev/binderfs
mount -t proc proc /testroot/proc
mount --bind /sys /testroot/sys
# Diagnostics only: Debian/glibc strace from the disposable initramfs, kept
# outside the Harmony root except for this bind, mirroring prepare-hdf's /diag.
mkdir -p /testroot/diag
mount --bind /diag /testroot/diag
echo ORIGINAL_INIT_PID1_EXEC
exec chroot /testroot /system/bin/init --second-stage
EOF
chmod 755 "$root/aurora-init" "$root/testroot/system/bin/aurora/service-test.sh"
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
