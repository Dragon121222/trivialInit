#!/usr/bin/env bash
set -euo pipefail

# trivialInit QEMU test harness
# Builds a minimal initramfs with trivialInit as /init (PID 1),
# boots it under QEMU with an Arch Linux kernel.
#
# Prerequisites:
#   - qemu-system-x86_64
#   - An Arch Linux kernel (linux package) or a vmlinuz
#   - Static-linked trivialInit binary
#
# Usage:
#   ./scripts/qemu_test.sh [--kernel /path/to/vmlinuz] [--build-dir /path/to/build]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-static"
QEMU_DIR="${PROJECT_DIR}/qemu"
INITRAMFS="${QEMU_DIR}/initramfs.cpio.gz"
ROOTFS_DIR="${QEMU_DIR}/rootfs"
KERNEL=""

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kernel)  KERNEL="$2"; shift 2;;
        --build-dir) BUILD_DIR="$2"; shift 2;;
        *) echo "Unknown arg: $1"; exit 1;;
    esac
done

# Find kernel
if [[ -z "$KERNEL" ]]; then
    # Try standard Arch locations
    for k in /boot/vmlinuz-linux /boot/vmlinuz-linux-lts; do
        if [[ -f "$k" ]]; then KERNEL="$k"; break; fi
    done
fi

if [[ -z "$KERNEL" ]]; then
    echo "ERROR: No kernel found. Install linux package or pass --kernel."
    echo "  On Arch: sudo pacman -S linux"
    echo "  Or download: wget https://geo.mirror.pkgbuild.com/core/os/x86_64/linux-*.pkg.tar.zst"
    exit 1
fi

echo "=== trivialInit QEMU Test ==="
echo "Kernel: $KERNEL"
echo "Build:  $BUILD_DIR"

# Step 1: Build static binary
echo ""
echo "--- Building trivialInit (static) ---"
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTINIT_STATIC=ON \
    -DTINIT_BUILD_TESTS=OFF \
    -DCMAKE_CXX_FLAGS="-Os"

cmake --build "$BUILD_DIR" -j"$(nproc)" --target trivialInit

file "$BUILD_DIR/trivialInit"
echo ""

# Step 2: Build initramfs
echo "--- Building initramfs ---"
rm -rf "$ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR"/{bin,sbin,etc,proc,sys,dev,run,tmp,var/log}
mkdir -p "$ROOTFS_DIR"/usr/{bin,sbin,lib/systemd/system}
mkdir -p "$ROOTFS_DIR"/lib/systemd/system

# Install our init
cp "$BUILD_DIR/trivialInit" "$ROOTFS_DIR/sbin/init"
chmod 755 "$ROOTFS_DIR/sbin/init"
ln -sf /sbin/init "$ROOTFS_DIR/init"

# Install busybox for basic utilities (getty, shell, etc)
if command -v busybox &>/dev/null; then
    cp "$(command -v busybox)" "$ROOTFS_DIR/bin/busybox"
    # Create essential symlinks
    for cmd in sh ash bash ls cat echo mount umount mkdir rm cp mv \
               ps kill sleep date hostname dmesg getty login; do
        ln -sf busybox "$ROOTFS_DIR/bin/$cmd"
    done
elif [[ -f /usr/bin/busybox ]]; then
    cp /usr/bin/busybox "$ROOTFS_DIR/bin/busybox"
    for cmd in sh ash ls cat echo mount umount mkdir rm cp mv \
               ps kill sleep date hostname dmesg getty login; do
        ln -sf busybox "$ROOTFS_DIR/bin/$cmd"
    done
else
    echo "WARNING: busybox not found. Only trivialInit will be available."
    # At minimum, create a shell script as /bin/sh
    cat > "$ROOTFS_DIR/bin/sh" << 'MINSH'
#!/sbin/init
# Placeholder — no shell available
MINSH
    chmod 755 "$ROOTFS_DIR/bin/sh"
fi

# Create sample systemd-compatible unit files for testing
cat > "$ROOTFS_DIR/lib/systemd/system/basic.target" << 'UNIT'
[Unit]
Description=Basic System
UNIT

cat > "$ROOTFS_DIR/lib/systemd/system/sysinit.target" << 'UNIT'
[Unit]
Description=System Initialization
After=basic.target
Requires=basic.target
UNIT

cat > "$ROOTFS_DIR/lib/systemd/system/multi-user.target" << 'UNIT'
[Unit]
Description=Multi-User System
After=sysinit.target
Requires=sysinit.target
UNIT

cat > "$ROOTFS_DIR/lib/systemd/system/console-getty.service" << 'UNIT'
[Unit]
Description=Console Getty
After=sysinit.target
Requires=sysinit.target

[Service]
Type=simple
ExecStart=/bin/getty -L 115200 ttyS0 linux
Restart=always

[Install]
WantedBy=multi-user.target
UNIT

cat > "$ROOTFS_DIR/lib/systemd/system/hostname.service" << 'UNIT'
[Unit]
Description=Set Hostname
After=basic.target

[Service]
Type=oneshot
ExecStart=/bin/hostname trivialInit-qemu

[Install]
WantedBy=sysinit.target
UNIT

cat > "$ROOTFS_DIR/lib/systemd/system/dmesg.service" << 'UNIT'
[Unit]
Description=Kernel Log
After=sysinit.target

[Service]
Type=oneshot
ExecStart=/bin/dmesg --level=err,warn

[Install]
WantedBy=multi-user.target
UNIT

# /etc/hostname
echo "trivialInit-qemu" > "$ROOTFS_DIR/etc/hostname"

# /etc/passwd (minimal)
cat > "$ROOTFS_DIR/etc/passwd" << 'PASSWD'
root:x:0:0:root:/root:/bin/sh
nobody:x:65534:65534:Nobody:/:/bin/false
PASSWD

cat > "$ROOTFS_DIR/etc/group" << 'GROUP'
root:x:0:
tty:x:5:
nobody:x:65534:
GROUP

# Pack initramfs
echo "Packing initramfs..."
(cd "$ROOTFS_DIR" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$INITRAMFS")
echo "Initramfs: $(du -h "$INITRAMFS" | cut -f1)"

# Step 3: Boot QEMU
echo ""
echo "--- Booting QEMU ---"
echo "Press Ctrl-A X to exit QEMU"
echo ""

exec qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 init=/sbin/init loglevel=3 panic=5" \
    -nographic \
    -m 512M \
    -smp 2 \
    -no-reboot \
    -enable-kvm 2>/dev/null || \
exec qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 init=/sbin/init loglevel=3 panic=5" \
    -nographic \
    -m 512M \
    -smp 2 \
    -no-reboot
