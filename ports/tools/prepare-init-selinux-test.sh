#!/bin/bash
# Experimental layer over prepare-init-service-test.sh: bring up the ORIGINAL
# SELinux policy path so the real samgr ServiceChecker can evaluate. This is a
# diagnostic fixture, not the final labeled rootfs. The l410 CIL policy is
# compiled on-target by /system/bin/secilc via init's libselinuxadp plugin.
# We force permissive so the unlabeled diskless fixture is not blocked by
# file/process context denials while we learn whether the policy loads and what
# context the shell-launched services actually receive.
set -euo pipefail
project=$(realpath "${1:?usage: prepare-init-selinux-test.sh PROJECT WORK OUTPUT [MODULES]}")
work=$(realpath "${2:?}")
out=${3:?}
modules=${4:-$work/kernel-test6-package}
bash "$project/ports/tools/prepare-init-service-test.sh" "$project" "$work" "$out" "$modules"
root=$out/root
tr=$root/testroot
src=/mnt/harmonyos-source/system/system

# 1) SELinux runtime: secilc + libsepol + policy loader + module engine + adapter.
mkdir -p "$tr/system/lib64/init" "$tr/system/etc/selinux"
copy_lib() {
    local name=$1 dest=${2:-$tr/system/lib64}
    test -e "$dest/$name" && return 0
    local sub p
    for sub in "" init/ platformsdk/ chipset-pub-sdk/ chipset-sdk-sp/ chipset-sdk/; do
        p="$src/lib64/$sub$name"
        if test -e "$p"; then cp -aL "$p" "$dest/$name"; return 0; fi
    done
    echo "MISSING LIB $name" >&2; return 1
}
for l in libselinux.z.so libsepol.z.so libload_policy.z.so librestorecon.z.so \
         libsec_shared.z.so libpcre2.z.so libinit_module_engine.so libc++.so \
         libparaperm_checker.z.so; do
    copy_lib "$l"
done
copy_lib libselinuxadp.z.so "$tr/system/lib64/init"
cp -aL "$src/bin/secilc" "$tr/system/bin/secilc"

# 2) Policy sources (CIL) + config + contexts. Contexts already partly copied by
#    the hdf layer; copy the whole selinux tree to be safe, then force permissive.
cp -aL "$src/etc/selinux/." "$tr/system/etc/selinux/" 2>/dev/null || true
# Full device policy = system CIL + vendor CIL + both file_contexts + versions.
if test -d /mnt/harmonyos-source/vendor/etc/selinux; then
    mkdir -p "$tr/vendor/etc/selinux"
    cp -aL /mnt/harmonyos-source/vendor/etc/selinux/. "$tr/vendor/etc/selinux/" 2>/dev/null || true
fi
if test -f "$tr/system/etc/selinux/config"; then
    sed -i 's/^SELINUX=.*/SELINUX=permissive/' "$tr/system/etc/selinux/config"
else
    echo "SELINUX=permissive" > "$tr/system/etc/selinux/config"
fi

# 3) aurora-init: mount selinuxfs before handing off to the original init, and
#    rbind /sys so the selinuxfs submount propagates into the chroot (a plain
#    bind drops nested mounts, the same class of bug as binderfs earlier).
#    Also fork a pre-exec watchdog that dumps PID1 block state to the console,
#    since the original init goes silent right after loading the policy.
python3 - "$root/aurora-init" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'selinuxfs' not in s:
    s = s.replace('mount -t devtmpfs devtmpfs /dev\n',
                  'mount -t devtmpfs devtmpfs /dev\n'
                  'mkdir -p /sys/fs/selinux /tmp\n'
                  'mount -t selinuxfs selinuxfs /sys/fs/selinux 2>/dev/null || echo NO_SELINUXFS\n', 1)
s = s.replace('mount --bind /sys /testroot/sys\n',
              'mount --rbind /sys /testroot/sys\n', 1)
open(p, 'w').write(s)
PY

# Attach a full strace to the original init only when HMOS_TRACE_INIT=1
# (slows init down; keep off for service-chain runs).
if [ "${HMOS_TRACE_INIT:-0}" = 1 ]; then
    python3 - "$root/aurora-init" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'init.trace' not in s:
    s = s.replace('echo ORIGINAL_INIT_PID1_EXEC\n',
                  '(\n'
                  '    while [ "$(cat /proc/1/comm 2>/dev/null)" != "init" ]; do :; done\n'
                  '    /diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace \\\n'
                  '        -p 1 -f -o /tmp/init.trace 2>/tmp/init-strace-err.log &\n'
                  ') &\n'
                  'echo ORIGINAL_INIT_PID1_EXEC\n', 1)
open(p, 'w').write(s)
PY
fi
python3 - "$root/aurora-init" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
watchdog = '''dump_pid1() {
    busybox grep -E 'State|Threads' /proc/1/status 2>/dev/null
    echo "wchan: $(cat /proc/1/wchan 2>/dev/null)"
    echo "-- page sizes (KernelPageSize of first maps):"
    echo "pid1: $(busybox grep -m1 KernelPageSize /proc/1/smaps 2>/dev/null)"
    echo "-- procs:"
    for p in /proc/[0-9]*; do
        echo "proc $p $(cat $p/comm 2>/dev/null)"
    done
    echo "-- init.trace failing execve:"
    busybox grep -E 'execve.*EINVAL|execve.*ENOEXEC' /tmp/init.trace 2>/dev/null | busybox head -n 5
}
(
    sleep 20
    echo WATCHDOG_ROUND0_BEGIN; dump_pid1; echo WATCHDOG_ROUND0_END
    sleep 40
    echo WATCHDOG_ROUND1_BEGIN; dump_pid1; echo WATCHDOG_ROUND1_END
) &
'''
s = s.replace('echo ORIGINAL_INIT_PID1_EXEC\n', watchdog + 'echo ORIGINAL_INIT_PID1_EXEC\n', 1)
open(p, 'w').write(s)
PY

# 4) service-test.sh: report SELinux state and the shell's own context so we can
#    see whether the policy loaded and what domain shell-launched services get.
python3 - "$tr/system/bin/aurora/service-test.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
diag = (
    'echo OHOS_INIT_SELINUX_DIAG_BEGIN\n'
    'busybox cat /sys/fs/selinux/enforce 2>&1\n'
    'busybox cat /sys/fs/selinux/policyvers 2>&1\n'
    'echo "ctx-self:"; busybox cat /proc/self/attr/current 2>&1\n'
    'echo "ctx-pid1:"; busybox cat /proc/1/attr/current 2>&1\n'
    'echo OHOS_INIT_SELINUX_DIAG_END\n'
)
s = s.replace('echo OHOS_INIT_TOKEN_DIAG_END\n',
              'echo OHOS_INIT_TOKEN_DIAG_END\n' + diag, 1)
open(p, 'w').write(s)
PY

chmod 755 "$root/aurora-init" "$tr/system/bin/aurora/service-test.sh" "$tr/system/bin/secilc"

# 5) Out-of-init reproduction harness: exec the same OHOS binary from the
#    kernel context and from u:r:init:s0 (via /proc/self/attr/exec), bare and
#    under full strace, so the execve EINVAL can be bisected without init.
cat > "$root/bin/selinux-exec-test.sh" <<'EOF'
#!/bin/sh
echo "TEST1 kernel-ctx exec of OHOS sh:"
busybox chroot /testroot /system/bin/sh -c 'echo DIRECT_SH_KERNELCTX_OK' 2>&1
echo "TEST1_RC=$?"
echo "TEST2 init-ctx exec of OHOS sh:"
(
  echo u:r:init:s0 > /proc/self/attr/exec 2>/dev/null
  exec busybox chroot /testroot /system/bin/sh -c 'echo DIRECT_SH_INITCTX_OK'
) 2>&1
echo "TEST2_RC=$?"
echo "TEST3 init-ctx exec under strace:"
cat > /tmp/t3.sh <<'EOS'
#!/bin/sh
echo u:r:init:s0 > /proc/self/attr/exec
exec /bin/busybox chroot /testroot /system/bin/sh -c 'echo DIRECT_SH_INITCTX_STRACED_OK'
EOS
chmod 755 /tmp/t3.sh
/diag/ld-linux-aarch64.so.1 --library-path /diag /diag/strace -f \
    -e trace=execve,openat,read,mmap,close,newfstatat \
    -o /tmp/direct.trace /bin/sh /tmp/t3.sh 2>&1
echo "TEST3_RC=$?"
echo "-- direct.trace key lines:"
busybox grep -E 'execve|EINVAL|ENOEXEC|ld-musl|attr' /tmp/direct.trace 2>/dev/null | busybox head -n 40
echo "TEST4 exec-child matrix (init-ctx dyntrans, per-state bisection):"
echo "== matrix: none"
busybox chroot /testroot /system/bin/aurora/exec-child-probe 2>&1
echo "== matrix: -c (bset full + capset => gained => secure-exec expected fail)"
busybox chroot /testroot /system/bin/aurora/exec-child-probe -c 2>&1
echo "== matrix: -b -c (real init child caps state)"
busybox chroot /testroot /system/bin/aurora/exec-child-probe -b -c 2>&1
echo "== matrix: -x (exact init child sequence)"
busybox chroot /testroot /system/bin/aurora/exec-child-probe -x 2>&1
echo "TEST4_MATRIX_END"
EOF
cat > "$tr/system/bin/aurora/probe-target.sh" <<'EOF'
#!/system/bin/sh
echo EXEC_CHILD_TARGET_OK
EOF
chmod 755 "$tr/system/bin/aurora/probe-target.sh"
chmod 755 "$root/bin/selinux-exec-test.sh"
python3 - "$root/aurora-init" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('    echo WATCHDOG_ROUND0_BEGIN; dump_pid1; echo WATCHDOG_ROUND0_END\n',
              '    echo WATCHDOG_ROUND0_BEGIN; dump_pid1\n'
              '    /bin/selinux-exec-test.sh\n'
              '    echo WATCHDOG_ROUND0_END\n', 1)
open(p, 'w').write(s)
PY
(cd "$root"; find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$out/initrd.img"
printf 'Prepared SELinux-policy experiment fixture %s\n' "$out/initrd.img"
