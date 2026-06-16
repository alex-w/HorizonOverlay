#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/libHorizonOverlay.dylib" >&2
  exit 2
fi

plugin_lib="$1"
if [[ ! -f "$plugin_lib" ]]; then
  echo "Cannot find plugin library: $plugin_lib" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target_dir="$HOME/Library/Application Support/Stellarium/modules/HorizonOverlay"

mkdir -p "$target_dir"
cp "$plugin_lib" "$target_dir/libHorizonOverlay.dylib"

if [[ ! -f "$target_dir/config.ini" ]]; then
  cp "$repo_root/data/config.ini" "$target_dir/config.ini"
fi

if [[ ! -f "$target_dir/obstructions.txt" ]]; then
  cp "$repo_root/data/obstructions.txt" "$target_dir/obstructions.txt"
else
  backup_dir="$target_dir/backups"
  backup_file="$backup_dir/obstructions-$(date +%Y%m%d-%H%M%S).txt"
  mkdir -p "$backup_dir"
  cp "$target_dir/obstructions.txt" "$backup_file"
  echo "Preserved existing obstructions.txt"
  echo "Backup saved to:"
  echo "$backup_file"
fi

if command -v lconvert >/dev/null 2>&1; then
  mkdir -p "$target_dir/translations/horizonoverlay"
  for po_file in "$repo_root"/plugin/HorizonOverlay/translations/horizonoverlay/*.po; do
    [[ -f "$po_file" ]] || continue
    lang="$(basename "$po_file" .po)"
    lconvert -i "$po_file" -o "$target_dir/translations/horizonoverlay/$lang.qm"
  done
fi

echo "Installed HorizonOverlay to:"
echo "$target_dir"
