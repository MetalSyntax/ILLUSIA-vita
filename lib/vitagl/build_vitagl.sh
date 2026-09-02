#!/bin/bash
# Rebuilds the vendored vitaGL only when its flags actually changed. A plain
# stamp-file OUTPUT with no DEPENDS is considered up to date forever once it
# exists, so `make clean` would never re-run after editing VITAGL_MAKE_FLAGS
# in CMakeLists.txt -- this script does the "did the flags change" check
# itself instead, from a custom_target that always runs.
MAKE_FLAGS="$1"
STAMP="$2"
SRC_DIR="$3"

echo "$MAKE_FLAGS" > "${STAMP}.tmp"
if ! cmp -s "${STAMP}.tmp" "$STAMP"; then
    echo "vitaGL flags changed, rebuilding..."
    cd "$SRC_DIR" && make clean
    cp "${STAMP}.tmp" "$STAMP"
fi
