#!/system/bin/sh
# Use direct scanout when the buffer meets the Composer's import requirements.
IFS= read -r cmdline </proc/cmdline
unset AURORA_GRAPHICS_TRACE AURORA_MULTI_DISPLAY
case " $cmdline " in
    *" aurora.scanout=copy "*|*" aurora.graphics=software "*) unset AURORA_DIRECT_SCANOUT ;;
    *) export AURORA_DIRECT_SCANOUT=1 ;;
esac
IFS= read -r boot_id </proc/sys/kernel/random/boot_id
diagnostics=/data/boot/aurora/native-diagnostics/$boot_id
if test -d "$diagnostics"; then
    (umask 077; printf 'direct_scanout=%s\n' "${AURORA_DIRECT_SCANOUT:-0}" >"$diagnostics/composer-graphics.txt")
    case " $cmdline " in
        *" hmos.mode=boot "*)
            export AURORA_KMS_DIAGNOSTICS_FILE="$diagnostics/kms.log"
            export AURORA_PERF_TRACE=1 AURORA_PERF_DELAY_MS=25000 AURORA_DIRECT_BUFFER_TRACE=1
            printf '%s\n' "$$" >"$diagnostics/composer.pid"
            if test -f "/data/local/tmp/aurora-perf-$$.csv"; then
                /system/bin/mv "/data/local/tmp/aurora-perf-$$.csv" "$diagnostics/composer-perf-previous.csv"
            fi
            /system/bin/sh /system/bin/aurora/graphics-diag-watch.sh "$diagnostics" "$$" &
            export LD_PRELOAD="/system/lib64/libaurora_kms_diagnostics.z.so${LD_PRELOAD:+:$LD_PRELOAD}"
            ;;
    esac
    exec >>"$diagnostics/composer.log" 2>&1
fi
exec /vendor/bin/hdf_devhost "$@"
