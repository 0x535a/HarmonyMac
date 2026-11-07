#!/bin/bash
# File-level migration to an already-created dedicated HarmonyOS filesystem.
# Does not partition or format a disk. Does not start the original installer.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
source_root=${1:?usage: populate-rootfs.sh READ_ONLY_SOURCE_ROOT TARGET_MOUNT [EXPECTED_UUID]}
target=${2:?}
expected_uuid=${3:-de5b04b6-597d-4382-b6a3-9e04388ccd6d}
test "$(id -u)" = 0
test "$(findmnt -n -o TARGET --target "$target")" = "$target"
test "$(findmnt -n -o UUID --target "$target")" = "$expected_uuid"
test "$(findmnt -n -o FSTYPE --target "$target")" = ext4
test -f "$source_root/system/system/bin/init"
test -f "$source_root/payload/state-seed.tar.zst"
test ! -e "$target/.aurora-rootfs-populated"
test ! -e "$target/system/bin/init"
mkdir -p "$target/boot/aurora-migration"
record=$target/boot/aurora-migration
printf 'File-level migration started; source remains read-only.\n' > "$record/progress.txt"

# The system image includes the root directory and /system itself. Keep the
# original ownership, hardlinks, capabilities, ACLs, and security attributes.
rsync -aHAX --numeric-ids --exclude=/lost+found --info=stats2 \
    "$source_root/system/" "$target/" > "$record/system-copy.txt"
for name in vendor sys_prod chip_prod log; do
    mkdir -p "$target/$name"
    rsync -aHAX --numeric-ids --exclude=/lost+found --info=stats2 \
        "$source_root/$name/" "$target/$name/" > "$record/$name-copy.txt"
done
mkdir -p "$target/data"

# The original state seed also includes an optional Debian application rootfs.
# Do not install a second distribution into the all-HarmonyOS target runtime.
# Other original state is staged for subsequent per-service adaptation.
zstd -dc "$source_root/payload/state-seed.tar.zst" | \
    tar --xattrs --xattrs-include='*' --acls --numeric-owner \
        --exclude='./.hmos-data/vendor/l410-linux/rootfs-bookworm' \
        --strip-components=2 -xpf - -C "$target/data" './.hmos-data'
cp "$source_root/payload/state-labels.tsv" "$record/original-state-labels.tsv"
printf '%s\n' \
    'Original optional Debian compatibility root excluded: /data/vendor/l410-linux/rootfs-bookworm' \
    'Original L410 wrappers and hardware-specific config still require Aurora adaptation.' \
    'State SELinux label sidecar retained; restore/rebase it before final boot acceptance.' \
    > "$record/pending-adaptation.txt"
test "$(readlink "$target/init")" = /system/bin/init
test -f "$target/system/lib/ld-musl-aarch64.so.1"
test -x "$target/system/bin/init"
sync
printf 'Source files populated; not yet a boot-qualified Aurora system.\n' > "$target/.aurora-rootfs-populated"
df -h "$target" > "$record/capacity.txt"
cat "$record/capacity.txt"
