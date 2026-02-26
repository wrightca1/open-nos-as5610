#!/bin/bash
# Run BDE validation test (Phase 1d).
# On build server: run under QEMU (no /dev/nos-bde → expect failure).
# On AS5610: copy build/tests/bde_validate and run natively after loading BDE modules.

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
BINARY="${BUILD_DIR}/tests/bde_validate"

if [ ! -x "$BINARY" ]; then
	echo "Build first: ./scripts/build-on-build-server.sh (or cmake --build build)"
	exit 1
fi

if [ -c /dev/nos-bde ]; then
	echo "Running on target (/dev/nos-bde present)"
	exec "$BINARY"
fi

# No BDE device: try QEMU if available (will fail open but proves binary runs)
if command -v qemu-ppc-static &>/dev/null && [ -d /usr/powerpc-linux-gnu ]; then
	echo "Running under QEMU (no /dev/nos-bde — expect open to fail)"
	exec qemu-ppc-static -L /usr/powerpc-linux-gnu "$BINARY"
fi

echo "No /dev/nos-bde and no qemu-ppc-static. Copy bde_validate to AS5610 and run there after: insmod nos_kernel_bde.ko; insmod nos_user_bde.ko"
exit 1
