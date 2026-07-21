#!/usr/bin/env bash
# Ensure a minimal Steam userdata layout for Steam ROM Manager when a real
# Steam install/account is missing. Never overwrites a valid shortcuts.vdf.
#
# Account selection (first match wins):
#   1. ARCHSTREAMER_STEAM_ACCOUNT_ID / ARCHSTREAMER_STEAM_DIR
#   2. SRM userSettings.json environmentVariables
#   3. Auto-detect best numeric userdata/<id> under Steam (grid + shortcuts score)
#   4. Create userdata/0 stub only if nothing else exists
set -euo pipefail

STEAM_DIR="${ARCHSTREAMER_STEAM_DIR:-}"
ACCOUNT_ID="${ARCHSTREAMER_STEAM_ACCOUNT_ID:-}"
USERDATA_SETTINGS="${HOME}/.config/steam-rom-manager/userData/userSettings.json"

if [[ (-z "${STEAM_DIR}" || -z "${ACCOUNT_ID}") && -f "${USERDATA_SETTINGS}" ]]; then
  readarray -t _srm_env < <(python3 - "${USERDATA_SETTINGS}" <<'PY'
import json
import sys
from pathlib import Path

settings = json.loads(Path(sys.argv[1]).read_text())
env = settings.get("environmentVariables", {})
steam = env.get("steamDirectory") or ""
accounts = env.get("userAccounts") or []
account = str(accounts[0]) if accounts else ""
print(steam)
print(account)
PY
)
  if [[ -z "${STEAM_DIR}" ]]; then
    STEAM_DIR="${_srm_env[0]:-}"
  fi
  if [[ -z "${ACCOUNT_ID}" ]]; then
    ACCOUNT_ID="${_srm_env[1]:-}"
  fi
fi

STEAM_DIR="${STEAM_DIR:-${HOME}/.local/share/Steam}"

if [[ -z "${ACCOUNT_ID}" ]]; then
  ACCOUNT_ID="$(python3 - "${STEAM_DIR}" <<'PY'
import sys
from pathlib import Path

steam = Path(sys.argv[1])
userdata = steam / "userdata"
best_id = ""
best_score = -1
if userdata.is_dir():
    for account in userdata.iterdir():
        if not account.is_dir() or not account.name.isdigit() or account.name == "0":
            continue
        config = account / "config"
        shortcuts = config / "shortcuts.vdf"
        if not shortcuts.is_file():
            continue
        grid = config / "grid"
        grid_count = 0
        if grid.is_dir():
            grid_count = sum(1 for p in grid.iterdir() if p.is_file() or p.is_symlink())
        score = grid_count * 1000 + shortcuts.stat().st_size
        if score > best_score:
            best_score = score
            best_id = account.name
print(best_id)
PY
)"
fi

if [[ -z "${ACCOUNT_ID}" ]]; then
  ACCOUNT_ID="0"
  echo "No Steam userdata account found; using stub account id 0."
fi

CONFIG_DIR="${STEAM_DIR}/userdata/${ACCOUNT_ID}/config"
GRID_DIR="${CONFIG_DIR}/grid"
SHORTCUTS="${CONFIG_DIR}/shortcuts.vdf"

mkdir -p "${STEAM_DIR}/steamapps"
mkdir -p "${GRID_DIR}"

is_valid_shortcuts() {
  local path="$1"
  python3 - "$path" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.is_file():
    raise SystemExit(1)
data = path.read_bytes()
# Real Steam/SRM files are non-empty binary VDFs starting with \x00shortcuts\x00
if len(data) < 13 or not data.startswith(b"\x00shortcuts\x00"):
    raise SystemExit(1)
raise SystemExit(0)
PY
}

if is_valid_shortcuts "${SHORTCUTS}"; then
  echo "Steam shortcuts already present; leaving unchanged:"
  echo "  ${SHORTCUTS} ($(wc -c < "${SHORTCUTS}") bytes)"
else
  if [[ -e "${SHORTCUTS}" ]]; then
    bak="${SHORTCUTS}.bak-invalid-$(date +%Y%m%d-%H%M%S)"
    cp -a "${SHORTCUTS}" "${bak}"
    echo "Invalid/empty shortcuts.vdf backed up to:"
    echo "  ${bak}"
  fi
  python3 - "${SHORTCUTS}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
path.write_bytes(b"\x00shortcuts\x00\x08\x08")
print(f"Created minimal valid shortcuts.vdf: {path}")
PY
fi

echo "Steam layout ready for SRM:"
echo "  steamDirectory: ${STEAM_DIR}"
echo "  account:        ${ACCOUNT_ID}"
echo "  grid:           ${GRID_DIR}"
echo "  shortcuts:      ${SHORTCUTS}"
