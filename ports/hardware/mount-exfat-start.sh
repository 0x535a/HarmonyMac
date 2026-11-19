#!/system/bin/sh
# Keep the stock exFAT implementation and arguments; adapt only /dev/fuse I/O.
# Service-local preload remains inherited; never alter global linker settings.
LD_PRELOAD="/system/lib64/aurora-storage/libfuse-subpage-transport.so${LD_PRELOAD:+:$LD_PRELOAD}"
export LD_PRELOAD
exec /system/bin/aurora/mount.exfat.aurora-original "$@"
