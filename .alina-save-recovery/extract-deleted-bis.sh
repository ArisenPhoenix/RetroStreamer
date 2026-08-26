#!/usr/bin/env bash
# Read-only ext4 extract of deleted Sword saves from Alina's Ryujinx BIS slots.
# Writes ONLY to this directory (Internal_SSD /dev/sda1), never to the home nvme.
set -euo pipefail

DEV=/dev/nvme1n1p2
OUT=/mnt/Internal_SSD/Programming/Mixed/ArchStreamer/.alina-save-recovery
mkdir -p "$OUT/dumped"
LOG="$OUT/debugfs.log"
: >"$LOG"

dump_dir() {
  local label=$1
  local inode=$2
  echo "===== $label inode=$inode =====" | tee -a "$LOG"
  sudo debugfs -R "ls -d <$inode>" "$DEV" 2>>"$LOG" | tee -a "$LOG"
}

# Slot directories as of the 04:21 wipe (inodes confirmed via stat).
dump_dir slot0 13762903
dump_dir slot1 17831965
dump_dir account 13775971

# Parse deleted inode numbers from ls -d output and dump regular files.
# debugfs ls -d lines look like:  123456  (12)   1513139  1  1  main
mapfile -t inodes < <(grep -oE '[0-9]{5,}' "$LOG" | sort -u)

i=0
for ino in "${inodes[@]}"; do
  [[ "$ino" == "13762903" || "$ino" == "17831965" || "$ino" == "13775971" ]] && continue
  i=$((i + 1))
  dest="$OUT/dumped/inode-${ino}"
  echo "dump <$ino> -> $dest" | tee -a "$LOG"
  if sudo debugfs -R "dump <$ino> $dest" "$DEV" 2>>"$LOG"; then
    sudo chown merk:family "$dest" 2>/dev/null || true
    stat -c '%n size=%s' "$dest" | tee -a "$LOG"
  else
    echo "dump failed for $ino" | tee -a "$LOG"
  fi
done

echo
echo '=== hashes of dumped files ==='
find "$OUT/dumped" -type f -exec md5sum {} \;
echo
echo '=== known 4am hashes to ignore ==='
echo 'b37fe686f1d59f7adb40595bc7a1c54f  current/old main'
echo '89eb3e8bdf221b7898353700e6fbfb71  bac/backup + 04:23 bis-rescue backup'
