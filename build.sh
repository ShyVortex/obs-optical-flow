#!/bin/bash
# Build obs-optical-flow.plugin against the installed OBS.app (no full OBS build needed).
# Usage: ./build.sh [--install]
set -euo pipefail
cd "$(dirname "$0")"

NAME=obs-optical-flow
VERSION=1.0.0
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
BUILD=build

if [[ ! -d "$OBS_APP/Contents/Frameworks/libobs.framework" ]]; then
	echo "ERROR: libobs.framework not found in $OBS_APP" >&2
	exit 1
fi

mkdir -p "$BUILD"

CFLAGS=(-std=c17 -O2 -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter
	-mmacosx-version-min=13.0 -arch arm64
	-Ideps/libobs-headers
	-DPLUGIN_VERSION="\"$VERSION\"")

clang "${CFLAGS[@]}" -c src/plugin-main.c -o "$BUILD/plugin-main.o"
clang "${CFLAGS[@]}" -c src/optical-flow-filter.c -o "$BUILD/optical-flow-filter.o"

clang -bundle -arch arm64 -mmacosx-version-min=13.0 \
	-F "$OBS_APP/Contents/Frameworks" -framework libobs \
	-o "$BUILD/$NAME" "$BUILD/plugin-main.o" "$BUILD/optical-flow-filter.o"

# assemble .plugin bundle
B="$BUILD/$NAME.plugin"
rm -rf "$B"
mkdir -p "$B/Contents/MacOS" "$B/Contents/Resources"
cp bundle/Info.plist "$B/Contents/Info.plist"
cp "$BUILD/$NAME" "$B/Contents/MacOS/$NAME"
cp -R data/ "$B/Contents/Resources/"

codesign --force --sign - "$B"

echo "OK: $B"

if [[ "${1:-}" == "--install" ]]; then
	DEST="$HOME/Library/Application Support/obs-studio/plugins"
	mkdir -p "$DEST"
	rm -rf "$DEST/$NAME.plugin"
	cp -R "$B" "$DEST/"
	echo "Installed to: $DEST/$NAME.plugin"
	echo "Restart OBS, then add the 'Optical Flow Frame Interpolation' filter to a source."
fi
