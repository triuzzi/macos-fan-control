#!/bin/bash
set -euo pipefail

DIR="/usr/local/bin"
BINARY="$DIR/fan_control"
SUDOERS="/etc/sudoers.d/macos-fan-control"
USER_NAME="${SUDO_USER:-${TARGET_USER_OVERRIDE:-}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[[ "$(id -u)" == "0" ]] || { echo "must run as root" >&2; exit 1; }
[[ -n "$USER_NAME" ]] || { echo "cannot determine user; set TARGET_USER_OVERRIDE" >&2; exit 1; }

make -C "$ROOT/core" >/dev/null
install -o root -g wheel -m 0755 -d "$DIR"
install -o root -g wheel -m 0755 "$ROOT/core/fan_control" "$BINARY"

[[ "$(stat -f '%Su' "$DIR")" == "root" ]] || { echo "$DIR is not root-owned, refusing sudoers rule" >&2; exit 1; }
[[ "$(stat -f '%OLp' "$DIR")" == "755" ]] || { echo "$DIR is group or world writable, refusing sudoers rule" >&2; exit 1; }

STAGED="$(mktemp /tmp/macos-fan-control-sudoers.XXXXXX)"
printf '%s ALL=(root) NOPASSWD: %s\n' "$USER_NAME" "$BINARY" > "$STAGED"
visudo -cqf "$STAGED" || { echo "generated rule failed validation, nothing installed" >&2; rm -f "$STAGED"; exit 1; }
install -o root -g wheel -m 0440 "$STAGED" "$SUDOERS"
rm -f "$STAGED"
visudo -cqf /etc/sudoers >/dev/null

echo "installed $BINARY and $SUDOERS"
