#!/system/bin/sh
# Evidence from a real ext4 root after switch_root, not a chroot fixture.
exec >/dev/console 2>&1
export PATH=/system/bin/aurora:/system/bin
export GBM_BACKENDS_PATH=/system/lib64/aurora-mesa/gbm GALLIUM_DRIVER=softpipe
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=kms_swrast
echo LOCAL_ROOT_SERVICE_CHECK_BEGIN
echo "LOCAL_ROOT_PREVIOUS_BOOT=$(busybox cat /data/boot/aurora/last-virtual-boot.txt 2>/dev/null)"
echo "LOCAL_ROOT_PID1=$(busybox readlink /proc/1/exe)"
busybox grep ' / ' /proc/mounts
busybox grep -m 1 KernelPageSize /proc/1/smaps
sleep "${HMOS_BOOT_WAIT_SECONDS:-8}"
param get bootevent.samgr.ready
param get bootevent.renderservice.ready
/system/bin/aurora/render-service-probe
echo "LOCAL_ROOT_RS_RESULT=$?"
HMOS_SCENE_HOLD_SECONDS=6 /system/bin/aurora/render-scene-probe
echo "LOCAL_ROOT_SCENE_RESULT=$?"
echo LOCAL_ROOT_PROCESSES
for p in /proc/[0-9]*; do
    name=$(busybox cat "$p/comm" 2>/dev/null)
    case "$name" in
        init|samgr|hdf_devmgr|allocator_host|composer_host|render_service|foundation|appspawn|accesstoken*)
            echo "LOCAL_SERVICE name=$name pid=${p##*/}"
            busybox cat "$p/attr/current" "$p/status" | busybox head -n 12 ;;
    esac
done
echo LOCAL_ROOT_SA_LIST_BEGIN
/system/bin/hidumper -ls
echo "LOCAL_ROOT_SA_LIST_RESULT=$?"
echo LOCAL_ROOT_SA_LIST_END
echo LOCAL_ROOT_HILOG_BEGIN
busybox tail -c 256000 /data/boot/aurora/hilograw.log
echo LOCAL_ROOT_HILOG_END
echo local-root-boot-check >/data/boot/aurora/last-virtual-boot.txt
echo LOCAL_ROOT_SERVICE_CHECK_END
busybox sync
busybox poweroff -f
