#!/bin/sh
# SampleTone: bake your samples into the machine and build a flashable OS image.
#
# This repository redistributes neither samples nor firmware, so two folders are
# yours to fill (each has a README):
#
#   samples/    your WAV/AIFF files, baked by bake.py   (mono, 48 kHz, 16-bit is ideal)
#   firmware/   Elektron's stock Digitone OS 1.44 .syx  (or pass --firmware FILE)
#
# Usage: sh build.sh [options]
#   --firmware FILE    use FILE instead of the single .syx in firmware/
#   --samples DIR      bake DIR instead of samples/
#   --elekloader DIR   use the elekloader checkout in DIR (default: ./elekloader,
#                      or $ELEKLOADER)
#   --bake-only        bake and report only: no firmware, no build
#   --keep-table       leave the baked samples.c/samples.h in the tree
#   the rest goes to bake.py: --rate HZ, --root NOTE, --trim SECONDS,
#   --fade MS, --downmix mid|left|right, --max-bytes N
#
# You need elekloader cloned and an m68k cross toolchain (binutils + gcc); set
# ELEKLOADER_CROSS if your tools are not `m68k-linux-gnu-` or `m68k-elf-`.
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"

FW=""
SRC=samples
EL="${ELEKLOADER:-elekloader}"
BAKE_ONLY=0
KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --firmware)
            [ $# -ge 2 ] || { echo "build.sh: --firmware needs a file" >&2; exit 2; }
            FW=$2
            shift 2
            ;;
        --samples)
            [ $# -ge 2 ] || { echo "build.sh: --samples needs a folder" >&2; exit 2; }
            SRC=$2
            shift 2
            ;;
        --elekloader)
            [ $# -ge 2 ] || { echo "build.sh: --elekloader needs a folder" >&2; exit 2; }
            EL=$2
            shift 2
            ;;
        --bake-only)
            BAKE_ONLY=1
            shift
            ;;
        --keep-table)
            KEEP=1
            shift
            ;;
        *)
            break
            ;;
    esac
done

# elekloader may be named as an absolute path; everything else is relative to
# this directory.
case "$EL" in
    /*) ;;
    *) EL="$ROOT/$EL" ;;
esac

fail() {
    echo "build.sh: $1" >&2
    exit 1
}

# ---- what we need ---------------------------------------------------------
command -v python3 >/dev/null 2>&1 || fail "python3 is not on PATH"
[ -d "$SRC" ] || fail "$ROOT/$SRC is missing: put your WAV/AIFF files there (see samples/README.md)"
N=$(find "$SRC" -maxdepth 1 -type f \
        \( -iname '*.wav' -o -iname '*.aif' -o -iname '*.aiff' -o -iname '*.aifc' \) \
    | wc -l | tr -d ' ')
[ "$N" != "0" ] || fail "no WAV/AIFF files in $SRC/, and this project ships none: put yours there first (mono, 48 kHz, 16-bit WAV is the recommended format; samples/README.md has the details)."

if [ "$BAKE_ONLY" = "0" ]; then
    [ -d "$EL" ] || fail "no elekloader checkout at $EL: clone https://github.com/irpina/elekloader there, or pass --elekloader DIR"
    [ -f "$EL/elekloader/devices/digitone_mk1.py" ] || fail "$EL does not look like an elekloader checkout"

    if [ -z "$FW" ]; then
        SET=$(find firmware -maxdepth 1 -type f -iname '*.syx' 2>/dev/null | sort)
        COUNT=$(printf '%s\n' "$SET" | grep -c . || true)
        [ "$COUNT" != "0" ] || fail "no .syx in firmware/: put Elektron's stock Digitone OS 1.44 update file there (see firmware/README.md), or pass --firmware FILE. For a bake without a build, use --bake-only."
        [ "$COUNT" = "1" ] || fail "firmware/ holds $COUNT .syx files; say which one with --firmware FILE:
$(printf '%s\n' "$SET" | sed 's/^/           /')"
        FW=$SET
    fi
    [ -f "$FW" ] || fail "no such file: $FW"

    EXPECT=$(PYTHONPATH="$EL${PYTHONPATH:+:$PYTHONPATH}" python3 -c \
        'from elekloader.devices.digitone_mk1 import DEVICE; print(DEVICE.releases["1.44"].syx_sha256)')
    if command -v shasum >/dev/null 2>&1; then
        GOT=$(shasum -a 256 "$FW" | awk '{print $1}')
    elif command -v sha256sum >/dev/null 2>&1; then
        GOT=$(sha256sum "$FW" | awk '{print $1}')
    else
        GOT=""
    fi
    if [ -z "$GOT" ]; then
        echo "build.sh: no shasum/sha256sum here: the OS version is not checked" >&2
    elif [ "$GOT" != "$EXPECT" ]; then
        echo "build.sh: $FW is not the Digitone mk1 OS 1.44 file (sha256 $GOT)." >&2
        echo "build.sh: this mod is built against 1.44 ($EXPECT)." >&2
        echo "build.sh: Elektron's download page has 1.44 under Digitone / Digitone Keys." >&2
        exit 1
    fi
fi

# ---- bake -----------------------------------------------------------------
STAGE=out/samples-build
mkdir -p "$STAGE"
echo "== samples/"
python3 bake.py --pack "$SRC" --out "$STAGE/samples.c" \
    --header "$STAGE/samples.h" "$@" > "$STAGE/bake.log"
cat "$STAGE/bake.log"
TOTAL=$(sed -n 's/^total \([0-9]*\) bytes.*/\1/p' "$STAGE/bake.log")

if [ "$BAKE_ONLY" = "1" ]; then
    echo
    echo "bake only: nothing built. The table is in $STAGE/samples.c"
    echo "note:      a build.sh run bakes into the tree, builds, and puts the"
    echo "           shipped empty table back afterwards."
    exit 0
fi

# ---- build ----------------------------------------------------------------
ID=$(python3 -c 'import json;print(json.load(open("mod.json"))["id"])')
VER=$(python3 -c 'import json;print(json.load(open("mod.json"))["ele3_version"])')

if [ -z "${ELEKLOADER_CROSS:-}" ]; then
    if command -v m68k-elf-as >/dev/null 2>&1; then
        ELEKLOADER_CROSS=m68k-elf-
    elif command -v m68k-linux-gnu-as >/dev/null 2>&1; then
        ELEKLOADER_CROSS=m68k-linux-gnu-
    else
        fail "no m68k cross toolchain on PATH (looked for m68k-elf-as and m68k-linux-gnu-as). Install binutils and gcc for m68k, or set ELEKLOADER_CROSS to your tools' prefix."
    fi
fi
export ELEKLOADER_CROSS
export PYTHONPATH="$EL${PYTHONPATH:+:$PYTHONPATH}"
echo "== toolchain: ${ELEKLOADER_CROSS} (set ELEKLOADER_CROSS to override)"

# The mod compiles the table from the source tree, so the baked pack is put
# there for the build and the shipped empty table is put back afterwards,
# whatever happens (a trap, so a failure or a Ctrl-C restores it too).
cp samples.c "$STAGE/samples.c.orig"
cp samples.h "$STAGE/samples.h.orig"
restore() {
    cp "$STAGE/samples.c.orig" samples.c
    cp "$STAGE/samples.h.orig" samples.h
}
trap restore EXIT INT TERM
cp "$STAGE/samples.c" samples.c
cp "$STAGE/samples.h" samples.h

echo
echo "== build"
newest() { ls -t $1 2>/dev/null | head -n 1; }
python3 -m elekloader.sdk.build "$EL/mods/core-dn1"     --stock "$FW"
python3 -m elekloader.sdk.build "$EL/mods/machines-dn1" --stock "$FW"
python3 -m elekloader.sdk.build .                       --stock "$FW"

CORE=$(newest "$EL/mods/core-dn1/out/core-*.elemod")
MACH=$(newest "$EL/mods/machines-dn1/out/machines-*.elemod")
MOD=$(newest "out/$ID-*.elemod")
[ -n "$CORE" ] && [ -n "$MACH" ] && [ -n "$MOD" ] || fail "a build produced no .elemod (see the output above)"

python3 -m elekloader.lint "$MOD" --stock "$FW" --with "$CORE" --with "$MACH"
IMAGE="out/$ID-1.44-$VER.syx"
python3 -m elekloader.patch --stock "$FW" \
    --mod "$CORE" --mod "$MACH" --mod "$MOD" \
    --out "$IMAGE" --version "$VER"

if [ "$KEEP" = "0" ]; then
    restore
    trap - EXIT INT TERM
fi

echo
echo "image:    $IMAGE ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"
echo "samples:  $N file(s), $TOTAL B of PCM baked in (cap 1200000)"
echo "flash:    Elektron Transfer over USB, or any SysEx tool"
echo "recovery: hold FUNC while powering on, press TRIG 4, send the stock OS"
if [ "$KEEP" = "0" ]; then
    echo "note:     samples.c/samples.h in the tree are the shipped empty table again;"
    echo "          the baked pack stays in $STAGE/ and your samples stay in $SRC/"
fi
