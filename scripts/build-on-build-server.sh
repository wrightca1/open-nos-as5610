#!/bin/bash
#
# Build open-nos-as5610 on the remote build server.
# Syncs the repo to the server and runs the remote build script.
#
# Usage:
#   ./scripts/build-on-build-server.sh [OPTIONS]
#
# Options:
#   USE_BUILD_SERVER=modern   Use 10.22.1.4 (default: debian8 → 10.22.1.5)
#   BUILD_KERNEL=1            On server: clone and build Linux 5.10 (slow)
#   KERNEL_SRC=/path          On server: use this kernel tree for BDE (no clone)
#
# Prereqs: SSH key to build server (smiley@10.22.1.5 or 10.22.1.4)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Build host config: use ONL build-config if present
if [ -f "$REPO_ROOT/../build-config.sh" ]; then
    source "$REPO_ROOT/../build-config.sh"
    BUILD_HOST=$(get_build_host)
    BUILD_HOST_DESC=$(get_build_host_desc)
else
    BUILD_USER="${BUILD_USER:-smiley}"
    BUILD_HOST_DEBIAN8="10.22.1.5"
    BUILD_HOST_MODERN="10.22.1.4"
    USE_BUILD_SERVER="${USE_BUILD_SERVER:-debian8}"
    if [ "$USE_BUILD_SERVER" = "modern" ]; then
        BUILD_HOST="$BUILD_HOST_MODERN"
        BUILD_HOST_DESC="Modern Debian"
    else
        BUILD_HOST="$BUILD_HOST_DEBIAN8"
        BUILD_HOST_DESC="Debian 8 (Jessie)"
    fi
fi
BUILD_USER="${BUILD_USER:-smiley}"

# Remote directory: same layout as local
REMOTE_DIR="open-nos-as5610-build-$(date +%Y%m%d-%H%M%S)"
BUILD_KERNEL="${BUILD_KERNEL:-0}"
KERNEL_SRC="${KERNEL_SRC:-}"

log() { echo "[$(date +'%H:%M:%S')] $1"; }
error() { echo "[ERROR] $1" >&2; exit 1; }

log "=============================================="
log "open-nos-as5610 build on build server"
log "=============================================="
log "Build host: $BUILD_USER@$BUILD_HOST ($BUILD_HOST_DESC)"
log "Remote dir: ~/$REMOTE_DIR"
log ""

# Rsync repo to build server (exclude build artifacts and git)
log "Syncing repo to $BUILD_HOST..."
rsync -az --delete \
    --exclude='.git' \
    --exclude='build/' \
    --exclude='*.ko' \
    --exclude='*.o' \
    --exclude='*.mod' \
    --exclude='*.cmd' \
    --exclude='*.a' \
    --exclude='Module.symvers' \
    "$REPO_ROOT/" "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/" \
    || error "rsync failed"

# Copy remote build script and run it
REMOTE_SCRIPT="$SCRIPT_DIR/remote-build.sh"
if [ ! -f "$REMOTE_SCRIPT" ]; then
    error "Remote build script not found: $REMOTE_SCRIPT"
fi

log "Copying remote build script..."
scp "$REMOTE_SCRIPT" "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/scripts/remote-build.sh" \
    || error "scp failed"

log "Running build on server..."
ssh "${BUILD_USER}@${BUILD_HOST}" "cd $REMOTE_DIR && BUILD_KERNEL=$BUILD_KERNEL KERNEL_SRC='$KERNEL_SRC' ./scripts/remote-build.sh"

log ""
log "Build finished. Artifacts on server: $REMOTE_DIR"
log "To copy BDE modules: scp ${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/bde/*.ko ."
log ""
