#!/system/bin/sh
# Original DDMS cannot initialize its DeviceManager client before SA4802 exists.
/system/bin/aurora/sa-service-ready --wait 60 4802
result=$?
if [ "$result" != 0 ]; then
    echo DDMS_DEVICE_MANAGER_NOT_READY
    exit "$result"
fi
echo DDMS_DEVICE_MANAGER_READY_EXEC
exec /system/bin/sa_main /system/profile/distributeddata.json
