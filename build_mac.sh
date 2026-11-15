#!/bin/sh
# Build ObjectDimensions.plugin (the "Object Dimensions" node) for macOS,
# universal arm64 + x86_64.  Needs only the Xcode command-line tools; the
# LightWave SDK headers are vendored in sdk/include.
#
#   sh build_mac.sh      build + checks, output in build/mac/
#
# Mirrors the SDK's samples.xcconfig: a .plugin bundle (Mach-O bundle), macOS
# 11.0+, _MACOS / LW_OSX / MAC_OS_X defines, only _mod_descrip exported.
set -eu
cd "$(dirname "$0")"

NAME=ObjectDimensions
VERSION=1.0
SERVERS=2           # the node's handler and interface
OUT=build/mac
OBJ="$OUT/obj"
BUNDLE="$OUT/$NAME.plugin"

ARCHS="-arch arm64 -arch x86_64"
MIN="-mmacosx-version-min=11.0"
DEFS="-D_MACOS -DLW_OSX -DMAC_OS_X=1 -DNDEBUG"
INC="-Isdk/include"
WARN="-Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
      -Wno-missing-field-initializers -Wno-deprecated-declarations -Wno-visibility"
# No -fvisibility=hidden: _mod_descrip carries an export attribute only under
# MSVC, so hidden visibility would hide it from -exported_symbol. The
# exported-symbol list alone keeps every other symbol private.
CFLAGS="$ARCHS $MIN -std=c11 -O2 -fPIC $DEFS $INC $WARN"

rm -rf "$BUNDLE" "$OBJ"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources" "$OBJ"

echo "== Compiling $NAME.plugin ($VERSION)"
for src in src/*.c; do
    o="$OBJ/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    clang $CFLAGS -c "$src" -o "$o"
done

echo "== Linking"
# shellcheck disable=SC2086
clang $ARCHS $MIN -bundle -o "$BUNDLE/Contents/MacOS/$NAME" "$OBJ"/*.o \
      -Wl,-exported_symbol,__mod_descrip

sed "s/@VERSION@/$VERSION/g" mac/Info.plist.in > "$BUNDLE/Contents/Info.plist"

# Ad-hoc signature over the whole bundle (arm64 code must be signed to load;
# a Developer ID signature + notarisation replaces this for distribution).
codesign --force --sign - "$BUNDLE"

echo "== Checks"
lipo -info "$BUNDLE/Contents/MacOS/$NAME"
# The module descriptor must be the (only) exported symbol LightWave looks up.
nm -gU "$BUNDLE/Contents/MacOS/$NAME" | grep -q " __mod_descrip$" \
    || { echo "ERROR: _mod_descrip is not exported"; exit 1; }
nm -gU "$BUNDLE/Contents/MacOS/$NAME"
codesign --verify --verbose "$BUNDLE"
# Every symbol must resolve at load time (LightWave dlopens with RTLD_NOW),
# and ServerDesc must list the node with a name and an activate function.
cat > "$OUT/dlopen_smoke.c" <<'SMOKE'
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <lwmodule.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
    ModuleDescriptor *m = (ModuleDescriptor *)dlsym(h, "_mod_descrip");
    if (!m) { printf("no _mod_descrip\n"); return 1; }
    int n = 0;
    for (ServerRecord *r = m->serverDefs; r->className; ++r, ++n) {
        if (!r->name || !r->activate) { printf("bad server row %d\n", n); return 1; }
        printf("  %s  %s\n", r->className, r->name);
    }
    printf("dlopen OK, _mod_descrip found, %d servers\n", n);
    return n == atoi(argv[2]) ? 0 : 1;
}
SMOKE
for a in arm64 x86_64; do
    clang -arch $a $MIN $DEFS $INC -o "$OUT/dlopen_smoke_$a" "$OUT/dlopen_smoke.c"
    if arch -$a /usr/bin/true 2>/dev/null; then
        echo "-- $a"; arch -$a "$OUT/dlopen_smoke_$a" "$BUNDLE/Contents/MacOS/$NAME" $SERVERS
    else
        echo "-- $a: skipped (cannot run this slice here)"
    fi
done

echo "== Packaging"
rm -f "$OUT/$NAME-mac.zip"
ditto -c -k --keepParent "$BUNDLE" "$OUT/$NAME-mac.zip"
echo "Built $OUT/$NAME-mac.zip"
