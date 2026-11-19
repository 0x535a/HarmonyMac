#!/system/bin/sh
# Observe the two graphics processes independently of their render threads.
export PATH=/system/bin
out=$1
composer_pid=$2
umask 077
exec >>"$out/graphics-threads.log" 2>&1

snapshot() {
    role=$1
    pid=$2
    case "$pid" in ''|*[!0-9]*) return ;; esac
    for task in /proc/"$pid"/task/*; do
        IFS= read -r stat <"$task/stat" || continue
        IFS= read -r name <"$task/comm" || continue
        wait=
        IFS= read -r wait <"$task/wchan"
        state=${stat##*) }
        printf 'role=%s pid=%s tid=%s name=%s state=%s wchan=%s\n' \
            "$role" "$pid" "${task##*/}" "$name" "${state%% *}" "${wait:-unknown}"
    done
    perf=/data/local/tmp/aurora-perf-$pid.csv
    if test -s "$perf"; then
        # The producer retains its open descriptor when the file is moved.
        mv "$perf" "$out/$role-perf.csv"
    fi
}

sample=0
while test "$sample" -lt 64 && kill -0 "$composer_pid" 2>/dev/null; do
    IFS=' ' read -r uptime idle </proc/uptime
    printf 'uptime=%s sample=%s\n' "$uptime" "$sample"
    snapshot composer "$composer_pid"
    if test -r "$out/render-service.pid"; then
        IFS= read -r render_pid <"$out/render-service.pid"
        snapshot render-service "$render_pid"
    fi
    # Persist short batches from this observer, outside the graphics threads.
    if test "$((sample % 2))" = 0; then
        for log in "$out"/*.log "$out"/*-perf.csv; do
            if test -f "$log"; then sync -d "$log" 2>/dev/null; fi
        done
        boot_id=${out##*/}
        player_dir=/data/boot/aurora/bootanimation-diagnostics/$boot_id
        for log in "$player_dir/player.log" "$player_dir"/aurora-perf-*.csv; do
            if test -f "$log"; then sync -d "$log" 2>/dev/null; fi
        done
    fi
    sample=$((sample + 1))
    sleep 3
done
