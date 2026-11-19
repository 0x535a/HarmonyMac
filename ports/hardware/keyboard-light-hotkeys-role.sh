#!/system/bin/sh
export LD_LIBRARY_PATH=/system/lib64/ndk:/system/lib64:/system/lib64/platformsdk:/system/lib64/chipset-pub-sdk:/system/lib64/chipset-sdk-sp:/vendor/lib64
umask 027
exec </dev/null >/data/boot/aurora/keyboard-light-hotkeys.log 2>&1
/system/bin/aurora/sa-service-ready --wait 90 3101 || exit 2
exec /system/bin/aurora/keyboard-light-hotkeys
