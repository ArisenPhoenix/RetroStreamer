#!/usr/bin/env bash
# List deleted ext4 inodes of Sword-save size, dump any that are still readable.
# Output stays on Internal_SSD.
set -euo pipefail

DEV=/dev/nvme1n1p2
OUT=/mnt/Internal_SSD/Programming/Mixed/ArchStreamer/.alina-save-recovery
mkdir -p "$OUT/lsdel-dumped"
LSDEL="$OUT/lsdel.txt"

echo "Running debugfs lsdel (can take a few minutes)..."
sudo debugfs -R 'lsdel' "$DEV" >"$LSDEL" 2>"$OUT/lsdel.err" || true
wc -l "$LSDEL" "$OUT/lsdel.err"

echo
echo '=== deleted inodes with size 1513139 (Sword main/backup) ==='
# lsdel columns: Inode  Owner  Mode  Size  Blocks  Time deleted
awk '$4==1513139 {print}' "$LSDEL" | tee "$OUT/lsdel-sword.txt"
echo
echo '=== deleted inodes with size 789 (poke_trade) ==='
awk '$4==789 {print}' "$LSDEL" | tee "$OUT/lsdel-poke.txt"

while read -r ino rest; do
  [[ -z "${ino:-}" ]] && continue
  dest="$OUT/lsdel-dumped/inode-${ino}"
  echo "dump <$ino> -> $dest"
  sudo debugfs -R "dump <$ino> $dest" "$DEV" 2>>"$OUT/lsdel.err" || echo "dump failed $ino"
  sudo chown merk:family "$dest" 2>/dev/null || true
done < <(awk '$4==1513139 {print $1}' "$LSDEL")

echo
echo '=== hashes ==='
find "$OUT/lsdel-dumped" -type f -exec md5sum {} \;
echo 'known: b37fe686=old main  89eb3e8b=bac/backup'
