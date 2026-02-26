#!/bin/bash
# Extract AS5610-52X DTB from a Cumulus Linux FIT image.
# Usage: ./extract-dtb.sh <CumulusLinux-*.bin> [output.dtb]
# Requires: u-boot-tools (dumpimage).

set -e
FIT="${1:?Usage: $0 <Cumulus-FIT.bin> [output.dtb]}"
OUT="${2:-as5610_52x.dtb}"

if ! command -v dumpimage &>/dev/null; then
	echo "Need dumpimage (apt-get install u-boot-tools)"
	exit 1
fi
echo "Listing subimages in $FIT..."
dumpimage -l "$FIT"
echo ""
echo "Extracting FDT (typically index 2 or 3)..."
for idx in 2 3 1 4 0; do
	if dumpimage -i "$FIT" -p "$idx" -o "$OUT" 2>/dev/null; then
		echo "Extracted to $OUT (subimage $idx). Verify: dtc -I dtb -O dts -o - $OUT | head -20"
		exit 0
	fi
done
echo "Failed to extract. Try: dumpimage -l $FIT and then dumpimage -i $FIT -p <index> -o $OUT"
exit 1
