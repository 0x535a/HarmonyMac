#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run inside an already booted development guest, using original AAFwk tools.
wait_seconds=180
settle_seconds=10
dry_run=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --wait|--settle)
            option=$1
            shift
            [ "$#" -gt 0 ] || { echo "Missing value for $option" >&2; exit 2; }
            case "$1" in ''|*[!0-9]*) echo "Expected nonnegative seconds" >&2; exit 2 ;; esac
            [ "${#1}" -le 3 ] && [ "$1" -le 600 ] || exit 2
            if [ "$option" = --wait ]; then wait_seconds=$1; else settle_seconds=$1; fi
            ;;
        --dry-run) dry_run=1 ;;
        *) echo "Usage: $0 [--wait 0..600] [--settle 0..600] [--dry-run]" >&2; exit 2 ;;
    esac
    shift
done
guide=com.huawei.hmos.hwstartupguide
if [ "$dry_run" -eq 1 ]; then
    echo "Wait at most ${wait_seconds}s for pidof $guide; settle ${settle_seconds}s."
    echo "/system/bin/aa force-stop $guide"
    echo "Observe process exit; inspect the desktop separately. No activation state is changed."
    exit 0
fi
export PATH=/system/bin/aurora:/system/bin
[ -x /system/bin/aa ] || { echo "Run inside the development HarmonyOS guest" >&2; exit 2; }
elapsed=0
while ! pidof "$guide" >/dev/null 2>&1; do
    if [ "$elapsed" -ge "$wait_seconds" ]; then
        echo "OOBE_GUIDE_NOT_OBSERVED: nothing was stopped" >&2
        exit 3
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
sleep "$settle_seconds"
/system/bin/aa force-stop "$guide" || exit 4
elapsed=0
while pidof "$guide" >/dev/null 2>&1; do
    [ "$elapsed" -lt 10 ] || { echo "OOBE_GUIDE_STILL_RUNNING" >&2; exit 4; }
    sleep 1
    elapsed=$((elapsed + 1))
done
echo "OOBE_GUIDE_STOPPED: desktop visibility requires separate observation"
