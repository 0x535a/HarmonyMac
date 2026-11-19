#!/system/bin/sh
# A flat persistent root retains empty volume mountpoints after power loss.
# The stock daemon rejects an existing path. Remove only empty UUID directories
# before it starts; rmdir refuses mounted or nonempty directories. Never recurse.
for base in /mnt/data/external /mnt/data/external_fuse; do
    if test -L "$base" || ! test -d "$base"; then
        echo "AURORA_STORAGE_MOUNTPOINT_PARENT_SKIPPED path=$base"
        continue
    fi
    for path in "$base"/*; do
        name=${path##*/}
        case "$name" in
            ''|*[!0-9a-fA-F-]*) continue ;;
        esac
        case "$name" in
            ????-????|????????????????|????????-????-????-????-????????????) ;;
            *) continue ;;
        esac
        test -L "$path" && continue
        test -d "$path" || continue
        if rmdir "$path"; then
            echo "AURORA_STORAGE_EMPTY_MOUNTPOINT_REMOVED path=$path"
        else
            echo "AURORA_STORAGE_MOUNTPOINT_RETAINED path=$path"
        fi
    done
done
if ! /system/bin/aurora/storage-mount-root; then
    echo "AURORA_STORAGE_SHARED_ROOT_SETUP_INCOMPLETE"
fi
LD_PRELOAD=/system/lib64/l410-storage-compat-v303/libstorage_no_hmdfs_param.so
export LD_PRELOAD
exec /system/bin/storage_daemon
