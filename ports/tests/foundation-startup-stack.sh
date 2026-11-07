#!/system/bin/sh
# One diagnostic snapshot; no PID1 startup delay or recurring sampling.
exec >/dev/console 2>&1
sleep "${HMOS_STACK_DELAY_SECONDS:-5}"
echo FOUNDATION_EARLY_STACK_BEGIN
/system/bin/cat /proc/uptime
for p in /proc/[0-9]*; do
    name=$(/system/bin/cat "$p/comm" 2>/dev/null)
    [ "$name" = foundation ] || continue
    pid=${p##*/}
    echo FOUNDATION_EARLY_PID=$pid
    /system/bin/cat "$p/status" >"/data/boot/aurora/foundation-early-$pid-status.txt"
    /system/bin/aurora/app-stack-probe "$pid" all >"/data/boot/aurora/foundation-early-$pid-stack.txt" 2>&1
    echo FOUNDATION_EARLY_DFX_RESULT=$?
done
echo FOUNDATION_EARLY_STACK_END
if [ "${HMOS_STACK_ONLY:-0}" = 1 ]; then
    /system/bin/sync
    echo FOUNDATION_STACK_ONLY_SHUTDOWN
    /system/bin/aurora/busybox poweroff -f
fi
