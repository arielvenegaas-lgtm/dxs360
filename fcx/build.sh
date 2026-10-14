#!/bin/sh
#
# Build a genuine Xbox 360 .xex for the "Aurora Bricks" bring-up using the
# FreeChainXenon toolchain. Adapted from FreeChainXenon/FCX-Test-2/build.sh.
#
# Requires:
#   - The FreeChainXenon toolchain, with FREECHAINXENON pointing at its prefix
#     (defaults to /usr/local/fcx). Its bin/ must be on PATH.
#
# Output: AuroraBricks.xex  (genuine XEX2, self-contained: the ELF is embedded
# as a PE ".elf" section).
#
set -e

TOOLCHAIN_PREFIX=${FREECHAINXENON:-/usr/local/fcx}
TOOLCHAIN_NAME_PREFIX="powerpc64-fcx-xenon"
BASE_ADDR=0x82000000
ALIGNMENT=0x10000
PROGRAM_NAME="AuroraBricks"

if [ ! -d "${TOOLCHAIN_PREFIX}/ldscripts" ]; then
    echo "error: FreeChainXenon not found at ${TOOLCHAIN_PREFIX}" >&2
    echo "       set FREECHAINXENON to your toolchain prefix" >&2
    exit 1
fi

# Process the ELF linker script.
${TOOLCHAIN_NAME_PREFIX}-cpp -DBASE_ADDR="${BASE_ADDR}" -DALIGNMENT="${ALIGNMENT}" \
    -o fcxlds.lds "${TOOLCHAIN_PREFIX}/ldscripts/fcxlds.lds.tmpl"

# Build the freestanding PPC ELF.
${TOOLCHAIN_NAME_PREFIX}-gcc -Wa,-mregnames,-a32,-mppc64 \
    -Wl,-T,fcxlds.lds -m32 -mpowerpc64 -nostdlib \
    -o "${PROGRAM_NAME}.elf" crt0.s bringup.c

# Prepare the PE-side ELF loader, embedding our ELF via .incbin.
sed "s|EXEC_NAME|${PROGRAM_NAME}.elf|" \
    "${TOOLCHAIN_PREFIX}/elfloader/fcxelfldr.s.tmpl" > fcxelfldr.s
${TOOLCHAIN_NAME_PREFIX}-as -mregnames -a32 -mppc64 -o fcxelfldr.o fcxelfldr.s

# Link it as an Xbox-subsystem PE image.
${TOOLCHAIN_NAME_PREFIX}pe-ld --accept-unknown-input-arch \
    --image-base="${BASE_ADDR}" --section-alignment="${ALIGNMENT}" \
    --subsystem=xbox --disable-dynamicbase --disable-long-section-names \
    -T "${TOOLCHAIN_PREFIX}/ldscripts/fcxpelds.lds" \
    --major-subsystem-version 1 --minor-subsystem-version 0 \
    --major-os-version 0 --minor-os-version 0 \
    -o "${PROGRAM_NAME}.exe" fcxelfldr.o

# Turn the PE into an XEX.
synthxex --skip-machine-check --input "${PROGRAM_NAME}.exe" --output "${PROGRAM_NAME}.xex"

echo "built ${PROGRAM_NAME}.xex"
