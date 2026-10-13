#!/usr/bin/env bash
# Installs a launcher for tui_chat: a small command called tui_chat in ~/.local/bin
# and an application entry that opens it in a kitty window, so it shows up in an
# application launcher.
#
# Nothing here runs at login. It only adds two files, and removes them again with
# --uninstall. A file with the same name that this script did not write is never
# overwritten.
#
# usage: scripts/install-launcher.sh [--uninstall]
#
# Environment:
#   TUI_CHAT_BIN        the nchat binary to run, default <repo>/build/bin/nchat
#   TUI_CHAT_BIN_DIR    where the command goes, default ~/.local/bin
#   TUI_CHAT_APPS_DIR   where the entry goes, default ${XDG_DATA_HOME:-~/.local/share}/applications

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"

bin="${TUI_CHAT_BIN:-$repo/build/bin/nchat}"
bin_dir="${TUI_CHAT_BIN_DIR:-$HOME/.local/bin}"
apps_dir="${TUI_CHAT_APPS_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/applications}"

command_file="$bin_dir/tui_chat"
entry_file="$apps_dir/tui_chat.desktop"
marker="managed by tui_chat scripts/install-launcher.sh"

ours() {
  [ -f "$1" ] && grep -q "$marker" "$1"
}

if [ "${1:-}" = "--uninstall" ]; then
  for f in "$command_file" "$entry_file"; do
    if ours "$f"; then
      rm -f "$f"
      echo "removed $f"
    elif [ -e "$f" ]; then
      echo "left $f alone, it was not written by this script"
    fi
  done
  exit 0
fi

if [ ! -x "$bin" ]; then
  echo "no tui_chat binary at $bin" >&2
  echo "build it first (see docs/how-it-works.md), or set TUI_CHAT_BIN" >&2
  exit 1
fi

# The entry's Exec line is split on spaces by the launcher, so keep paths plain.
case "$bin$command_file" in
  *[!A-Za-z0-9_./-]*)
    echo "this script only handles paths made of letters, digits and _ . / -" >&2
    exit 1
    ;;
esac

if ! command -v kitty >/dev/null 2>&1; then
  echo "kitty was not found on PATH, the entry opens tui_chat in a kitty window" >&2
  exit 1
fi

for f in "$command_file" "$entry_file"; do
  if [ -e "$f" ] && ! ours "$f"; then
    echo "$f already exists and was not written by this script, not touching it" >&2
    exit 1
  fi
done

mkdir -p "$bin_dir" "$apps_dir"

# exec keeps the real binary path as the program's own name, which is what the
# kitty Shift+Enter line in docs/how-it-works.md matches on.
cat > "$command_file" <<EOF
#!/bin/sh
# $marker
exec $bin "\$@"
EOF
chmod 755 "$command_file"

# --class gives the window its own name so window rules and launchers can tell it
# apart from a plain terminal. The command is given by full path so the entry works
# even when the launcher was started without ~/.local/bin on its PATH.
cat > "$entry_file" <<EOF
# $marker
[Desktop Entry]
Type=Application
Name=tui_chat
Comment=Chat in the terminal
Exec=kitty --class=tui_chat $command_file
Icon=internet-chat
Terminal=false
Categories=Network;Chat;InstantMessaging;
StartupWMClass=tui_chat
EOF
chmod 644 "$entry_file"

echo "installed $command_file"
echo "installed $entry_file"

if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "$entry_file" && echo "entry validated"
fi

case ":$PATH:" in
  *":$bin_dir:"*) ;;
  *) echo "note: $bin_dir is not on PATH, the entry still works, typing tui_chat will not" ;;
esac
