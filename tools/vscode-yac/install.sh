#!/bin/sh
# Install the Yac VS Code/Cursor extension via symlink.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
TARGET=${1:-cursor}

case "$TARGET" in
  cursor) EXT_DIR="${HOME}/.cursor/extensions" ;;
  code|vscode) EXT_DIR="${HOME}/.vscode/extensions" ;;
  *)
    echo "usage: $0 [cursor|code]" >&2
    exit 1
    ;;
esac

mkdir -p "$EXT_DIR"
LINK="$EXT_DIR/yac-lang.yac-0.1.0"
ln -sfn "$ROOT" "$LINK"
echo "Linked $LINK -> $ROOT"
echo "Reload the window: Developer: Reload Window"
