#!/bin/bash
# Extract AS5610-52X DTB from a Cumulus Linux installer (.bin) or FIT (.itb).
# Cumulus .bin is a shell script + tar; the FIT is in installer/data.tar as uImage-powerpc.itb.
# Usage: ./extract-dtb.sh <CumulusLinux-*.bin or path/to/uImage-powerpc.itb> [output.dtb]
# Requires: fdtget (device-tree-compiler) or dumpimage (u-boot-tools), or Docker.

set -e
INPUT="${1:?Usage: $0 <CumulusLinux-*.bin|path/to/uImage-powerpc.itb> [output.dtb]}"
OUT="${2:-as5610_52x.dtb}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Index of accton_as5610_52x_dtb in Cumulus uImage-powerpc.itb (from dumpimage -l)
AS5610_DTB_INDEX=5

# If input is a .bin (has "exit_marker" = shell script + tar), extract FIT from payload
ITB="$INPUT"
if grep -q "exit_marker" "$INPUT" 2>/dev/null; then
	echo "Extracting FIT from Cumulus .bin payload..."
	TMP=$(mktemp -d)
	trap "rm -rf '$TMP'" EXIT
	sed -e '1,/^exit_marker$/d' "$INPUT" | tar xf - -C "$TMP" || { echo "Failed to extract .bin payload"; exit 1; }
	if [ ! -f "$TMP/installer/data.tar" ]; then
		echo "No installer/data.tar in payload"; exit 1
	fi
	tar xf "$TMP/installer/data.tar" -C "$TMP" uImage-powerpc.itb 2>/dev/null || { echo "No uImage-powerpc.itb in data.tar"; exit 1; }
	ITB="$TMP/uImage-powerpc.itb"
fi

# FIT is a valid FDT; extract DTB using fdtget (works on macOS and Linux with device-tree-compiler)
extract_with_fdtget() {
	local itb="$1" out="$2"
	if ! command -v fdtget &>/dev/null; then return 1; fi
	if ! fdtget -l "$itb" /images 2>/dev/null | grep -q "accton_as5610_52x_dtb"; then return 1; fi
	fdtget -t r "$itb" /images/accton_as5610_52x_dtb data > "$out" 2>/dev/null && [ -s "$out" ]
}

extract_with_dumpimage() {
	local itb="$1" out="$2"
	if ! command -v dumpimage &>/dev/null; then return 1; fi
	if ! dumpimage -l "$itb" 2>/dev/null | grep -q "accton_as5610_52x_dtb\|Image $AS5610_DTB_INDEX"; then return 1; fi
	dumpimage -p "$AS5610_DTB_INDEX" -o "$out" "$itb" 2>/dev/null
}

if extract_with_fdtget "$ITB" "$OUT"; then
	echo "Extracted to $OUT (fdtget)"
	exit 0
fi
if extract_with_dumpimage "$ITB" "$OUT"; then
	echo "Extracted to $OUT (dumpimage)"
	exit 0
fi

# Fallback: Docker with dumpimage
if command -v docker &>/dev/null; then
	echo "Trying Docker (u-boot-tools)..."
	DIR=$(dirname "$ITB")
	ITB_NAME=$(basename "$ITB")
	OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
	if docker run --rm -v "$DIR:/in:ro" -v "$(dirname "$OUT_ABS"):/out" debian:bookworm bash -c \
		"apt-get update -qq && apt-get install -y -qq u-boot-tools >/dev/null && dumpimage -p $AS5610_DTB_INDEX -o /out/$(basename "$OUT") /in/$ITB_NAME" 2>/dev/null; then
		echo "Extracted to $OUT (via Docker)"
		exit 0
	fi
fi

echo "Failed to extract DTB. Need: fdtget (device-tree-compiler) or dumpimage (u-boot-tools) or Docker."
exit 1
