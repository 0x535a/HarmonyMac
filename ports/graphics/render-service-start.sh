#!/system/bin/sh
# Prefer Asahi when a usable render node exists. Init supplies software fallback.
program=/system/bin/render_service
role=render_service
if test "$1" = --bootanimation; then
    program=/system/bin/bootanimation
    role=bootanimation
    shift
fi

record_backend() {
    if test "$role" = bootanimation; then
        if test -n "${AURORA_BOOTANIMATION_TRACE_FILE:-}"; then
            printf 'stage=graphics-backend %s\n' "$*" >>"$AURORA_BOOTANIMATION_TRACE_FILE"
            export AURORA_RENDER_DIAGNOSTICS_DIR="${AURORA_BOOTANIMATION_TRACE_FILE%/*}"
            export AURORA_PERF_TRACE=1 AURORA_PERF_DELAY_MS=0
            export AURORA_PERF_TRACE_DIR="$AURORA_RENDER_DIAGNOSTICS_DIR"
        fi
        echo "AURORA_BOOTANIMATION_GRAPHICS $*"
        return 0
    fi
    echo "AURORA_RENDER_SERVICE $*"
    IFS= read -r boot_id </proc/sys/kernel/random/boot_id || return 0
    diagnostics=/data/boot/aurora/native-diagnostics/$boot_id
    if test -d "$diagnostics"; then
        export AURORA_RENDER_DIAGNOSTICS_DIR="$diagnostics"
        (umask 077; printf '%s\n' "$*" >"$diagnostics/render-service-graphics.txt") 2>/dev/null
        case " $cmdline " in
            *" hmos.mode=boot "*)
                export AURORA_PERF_TRACE=1 AURORA_PERF_DELAY_MS=25000 AURORA_DIRECT_BUFFER_TRACE=1
                printf '%s\n' "$$" >"$diagnostics/render-service.pid"
                if test -f "/data/local/tmp/aurora-perf-$$.csv"; then
                    /system/bin/mv "/data/local/tmp/aurora-perf-$$.csv" "$diagnostics/render-service-perf-previous.csv"
                fi
                exec >>"$diagnostics/render-service.log" 2>&1
                ;;
        esac
    fi
}

IFS= read -r cmdline </proc/cmdline
unset AURORA_EGL_RENDER_BACKEND AURORA_EGL_GBM_FRONT AURORA_EGL_GPU_BLIT
case " $cmdline " in
    *" aurora.graphics=software "*)
        record_backend "selected=software reason=explicit-override"
        exec "$program" "$@"
        ;;
esac

fallback=asahi-render-node-unavailable
for entry in /sys/class/drm/renderD*; do
    driver=$(/system/bin/readlink "$entry/device/driver" 2>/dev/null) || continue
    case "$driver" in
        */asahi|*/asahi-g15)
            node=/dev/dri/${entry##*/}
            if test -c "$node" && test -r "$node" && test -w "$node"; then
                export AURORA_EGL_RENDER_BACKEND=asahi
                unset GBM_ALWAYS_SOFTWARE GALLIUM_DRIVER LIBGL_ALWAYS_SOFTWARE MESA_LOADER_DRIVER_OVERRIDE
                case " $cmdline " in
                    *" aurora.scanout=copy "*) ;;
                    *)
                        export AURORA_EGL_GPU_BLIT=1
                        if test "$role" = render_service; then
                            export AURORA_EGL_GBM_FRONT=1
                        fi
                        ;;
                esac
                record_backend "selected=asahi node=$node driver=${driver##*/} gbm_front=${AURORA_EGL_GBM_FRONT:-0} gpu_blit=${AURORA_EGL_GPU_BLIT:-0}"
                exec "$program" "$@"
            fi
            fallback=asahi-render-node-inaccessible
            ;;
    esac
done
record_backend "fallback=software reason=$fallback"
exec "$program" "$@"
