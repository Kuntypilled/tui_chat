#!/usr/bin/env bash
# Builds the omachatd helper that tui_chat talks to, from upstream OmaChat at a
# pinned commit, and installs it where tui_chat looks for it.
#
# The helper is a separate program under a different license (AGPL-3.0-or-later).
# It is built from upstream source here and is never copied into this repository.
#
# usage: scripts/build-helper.sh [install-dir]
#   install-dir defaults to ${XDG_DATA_HOME:-~/.local/share}/tui_chat
#
# Needs: git, python3, a C compiler (gcc or clang), and Go 1.27.0 or newer.

set -euo pipefail

REPO_URL="https://github.com/onelegdave/omachat"
PINNED_COMMIT="3dbd975f8a0fbf427bbb7c09179664f02a824e53"
MIN_GO="1.27.0"

install_dir="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/tui_chat}"

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "missing required program: $1" >&2; exit 1; }
}

need git
need python3
need go
if ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1 && [ -z "${CC:-}" ]; then
  echo "missing a C compiler (gcc or clang), needed for the helper's SQLite" >&2
  exit 1
fi

# version_ge A B: true when A >= B, comparing dotted numbers
version_ge() {
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

go_version="$(go env GOVERSION | sed 's/^go//; s/[^0-9.].*$//')"
if ! version_ge "$go_version" "$MIN_GO"; then
  echo "Go $MIN_GO or newer is required, found $go_version" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Fetching OmaChat at ${PINNED_COMMIT:0:12}..."
git clone --quiet "$REPO_URL" "$work/src"
git -C "$work/src" checkout --quiet "$PINNED_COMMIT"

echo "Building the helper..."
# keep the build script's lock and state files out of the home directory
XDG_STATE_HOME="$work/state" python3 -I "$work/src/scripts/updates.py" build >/dev/null

mkdir -p "$install_dir"
install -m 0755 "$work/src/bin/omachatd" "$install_dir/omachatd"

echo "Installed $install_dir/omachatd"
"$install_dir/omachatd" -version
