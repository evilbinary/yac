#!/bin/sh
# Install the Yac VS Code/Cursor extension via symlink.
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
TARGET=${1:-cursor}

case "$TARGET" in
  cursor) EXT_DIR="${HOME}/.cursor/extensions" ;;
  code|vscode) EXT_DIR="${HOME}/.vscode/extensions" ;;
  trae-cn) EXT_DIR="${HOME}/.trae-cn/extensions" ;;

  *)
    echo "usage: $0 [cursor|code]" >&2
    exit 1
    ;;
esac

VERSION=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/package.json" | head -n 1)
if [ -z "$VERSION" ]; then
  echo "could not read version from $ROOT/package.json" >&2
  exit 1
fi

mkdir -p "$EXT_DIR"
NAME="yac-lang.yac-${VERSION}"
LINK="$EXT_DIR/$NAME"
ln -sfn "$ROOT" "$LINK"

# Cursor/VS Code skip any folder named in .obsolete. Sideloaded symlink
# installs often get listed there, which looks like "the plugin does nothing".
OBSOLETE="$EXT_DIR/.obsolete"
if [ -f "$OBSOLETE" ]; then
  python3 -c "
import json, sys
path, key = sys.argv[1], sys.argv[2]
with open(path) as f:
    data = json.load(f)
if not isinstance(data, dict):
    sys.exit(0)
if data.pop(key, None) is None:
    sys.exit(0)
with open(path, 'w') as f:
    json.dump(data, f, separators=(',', ':'))
print('Removed', key, 'from .obsolete')
" "$OBSOLETE" "$NAME"
fi

echo "Linked $LINK -> $ROOT"
echo "Reload the window: Developer: Reload Window"
