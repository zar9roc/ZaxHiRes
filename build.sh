#!/bin/sh
# Builds ZaxHiRes.asi (32-bit Windows DLL) and installs it next to Zax.exe.
# No C runtime: compile to an object, link with lld-link against import
# libraries for kernel32/user32 only, entry point DllMain.
set -e
cd "$(dirname "$0")"
ZIG_DIR=${ZIG_DIR:-$HOME/.local/opt/zig-linux-x86_64-0.13.0}
ZIG=$ZIG_DIR/zig
DEFS=$ZIG_DIR/lib/libc/mingw/lib32
OUT=build
mkdir -p $OUT

for lib in kernel32 user32; do
    "$ZIG" dlltool -m i386 -k -d "$DEFS/$lib.def" -l "$OUT/lib$lib.a"
done
"$ZIG" cc -target x86-windows-gnu -c -O2 -Wall -fno-builtin -fno-stack-protector \
    -o "$OUT/zaxhires.o" zaxhires.c
"$ZIG" lld-link -dll -machine:x86 -entry:DllMain@12 -nodefaultlib -subsystem:windows \
    -map:"$OUT/ZaxHiRes.map" -out:ZaxHiRes.asi "$OUT/zaxhires.o" "$OUT/libkernel32.a" "$OUT/libuser32.a"
rm -f ZaxHiRes.lib
# Install next to Zax.exe: GAME_DIR, else ../Game if it exists.
GAME_DIR=${GAME_DIR:-../Game}
if [ -f "$GAME_DIR/Zax.exe" ]; then
    cp ZaxHiRes.asi "$GAME_DIR/"
    echo "built and installed $GAME_DIR/ZaxHiRes.asi"
else
    echo "built ZaxHiRes.asi (set GAME_DIR to the folder with Zax.exe to install it)"
fi
