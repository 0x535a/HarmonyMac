#!/system/bin/sh
# Diagnostic timing only; this sleep does not block PID1 or order services.
exec >/dev/console 2>&1
sleep 5
echo SETTINGS_EARLY_STACK_BEGIN
/system/bin/cat /proc/uptime
for p in /proc/[0-9]*; do
    cmd=$(/system/bin/cat "$p/cmdline" 2>/dev/null)
    name=$(/system/bin/cat "$p/comm" 2>/dev/null)
    if [ "$cmd" != com.ohos.settingsdata ] && [ "$name" != distributeddata ]; then continue; fi
    pid=${p##*/}
    echo SETTINGS_EARLY_PID=$pid
    /system/bin/cat "$p/status" >"/data/boot/aurora/settings-early-$pid-status.txt"
    /system/bin/cat "$p/maps" >"/data/boot/aurora/settings-early-$pid-maps.txt"
    /system/bin/aurora/app-stack-probe "$pid" all >"/data/boot/aurora/settings-early-$pid-stack.txt" 2>&1
    echo SETTINGS_EARLY_DFX_RESULT=$?
done
echo SETTINGS_EARLY_STACK_END
