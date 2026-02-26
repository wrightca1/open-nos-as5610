#!/bin/bash
# Build minimal initramfs for open-nos-as5610 (PPC32).
# Produces: initramfs.cpio.gz
# Requires: busybox (cross or native), or extract from Debian powerpc busybox-static

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${SCRIPT_DIR}/initramfs.cpio.gz"
ROOT="${SCRIPT_DIR}/root"

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,dev,proc,sys,newroot}

# Init script
cp "${SCRIPT_DIR}/init" "$ROOT/init"
chmod +x "$ROOT/init"

# Busybox: use cross-installed busybox or copy from sysroot
if command -v powerpc-linux-gnu-gcc &>/dev/null; then
	# If we have a static busybox for PPC32, copy it
	if [ -x /usr/powerpc-linux-gnu/bin/busybox ]; then
		cp /usr/powerpc-linux-gnu/bin/busybox "$ROOT/bin/busybox"
	elif [ -x "${BUSYBOX_STATIC:-}" ]; then
		cp "$BUSYBOX_STATIC" "$ROOT/bin/busybox"
	else
		echo "WARN: No PPC32 busybox found. Install busybox-static:powerpc or set BUSYBOX_STATIC."
		echo "Creating stub /bin/sh that runs init..."
		echo '#!/bin/busybox sh' > "$ROOT/bin/sh"
		chmod +x "$ROOT/bin/sh"
	fi
else
	echo "WARN: powerpc-linux-gnu-gcc not found; initramfs may need native busybox for target."
fi

if [ -x "$ROOT/bin/busybox" ]; then
	chroot "$ROOT" /bin/busybox --install -s /bin 2>/dev/null || true
	[ -L "$ROOT/bin/sh" ] || ln -sf busybox "$ROOT/bin/sh"
fi

# Device nodes (optional; kernel can use devtmpfs)
# mknod -m 622 "$ROOT/dev/console" c 5 1

( cd "$ROOT" && find . | cpio -o -H newc ) | gzip -9 > "$OUT"
echo "Built: $OUT"
