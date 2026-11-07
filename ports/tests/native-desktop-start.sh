#!/system/bin/sh
# Native development boot: original services and SceneBoard, no test shutdown.
export PATH=/system/bin/aurora:/system/bin
mkdir -p /data/boot/aurora
exec >/data/boot/aurora/native-desktop.log 2>&1
echo NATIVE_DESKTOP_START
cat /proc/uptime
n=0
while test "$n" -lt 120; do
    n=$((n + 1))
    if /system/bin/aurora/samgr-service-probe 401 501 4606 4607 >/dev/null 2>&1; then break; fi
    sleep 1
done
/system/bin/aa start -b com.ohos.sceneboard -a com.ohos.sceneboard.MainAbility -m pc_sceneboard
# Use the same bounded direct-desktop path accepted in the VM; wait for the
# guide's real process before stopping it. No fabricated activation state.
n=0
while test "$n" -lt 180; do
    n=$((n + 1))
    if pidof com.huawei.hmos.hwstartupguide >/dev/null; then
        sleep 10
        /system/bin/aa force-stop com.huawei.hmos.hwstartupguide
        break
    fi
    sleep 1
done
echo NATIVE_DESKTOP_READY
cat /proc/uptime
/system/bin/hidumper -s 4606 -a -a
# Return to original init. Never execute VM diag UART or automatic poweroff.
