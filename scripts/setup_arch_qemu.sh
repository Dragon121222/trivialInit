#!/usr/bin/env bash
set -euo pipefail

# Create a full Arch Linux QEMU disk image with trivialInit replacing systemd
#
# This builds a proper disk image you can boot repeatedly, not just an initramfs.
# Requires: qemu-img, pacstrap (or manual bootstrap), arch-install-scripts
#
# Usage:
#   ./scripts/setup_arch_qemu.sh [--image-size 4G] [--build-dir ./build-static]

IMAGE_SIZE="4G"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-static"
QEMU_DIR="${PROJECT_DIR}/qemu"
IMAGE="${QEMU_DIR}/arch-trivialinit.qcow2"
MOUNT_DIR="${QEMU_DIR}/mnt"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image-size) IMAGE_SIZE="$2"; shift 2;;
        --build-dir)  BUILD_DIR="$2"; shift 2;;
        *) echo "Unknown: $1"; exit 1;;
    esac
done

mkdir -p "$QEMU_DIR"

echo "=== Arch Linux + trivialInit QEMU Setup ==="
echo ""

# Method 1: If pacstrap is available (running on Arch)
if command -v pacstrap &>/dev/null; then
    echo "--- Using pacstrap (Arch host detected) ---"

    # Create disk image
    qemu-img create -f qcow2 "$IMAGE" "$IMAGE_SIZE"

    # Load nbd module and connect
    sudo modprobe nbd max_part=4
    sudo qemu-nbd --connect=/dev/nbd0 "$IMAGE"

    # Partition: simple single-partition layout
    sudo parted /dev/nbd0 --script mklabel gpt
    sudo parted /dev/nbd0 --script mkpart primary ext4 1MiB 100%
    sudo mkfs.ext4 /dev/nbd0p1

    # Mount and pacstrap
    mkdir -p "$MOUNT_DIR"
    sudo mount /dev/nbd0p1 "$MOUNT_DIR"

    sudo pacstrap "$MOUNT_DIR" base linux linux-firmware busybox

    # Install trivialInit
    sudo cp "$BUILD_DIR/trivialInit" "$MOUNT_DIR/sbin/trivialinit"
    sudo chmod 755 "$MOUNT_DIR/sbin/trivialinit"

    # Replace systemd's init symlink
    sudo ln -sf /sbin/trivialinit "$MOUNT_DIR/sbin/init"

    # Fstab
    echo '/dev/vda1 / ext4 defaults 0 1' | sudo tee "$MOUNT_DIR/etc/fstab"

    # Hostname
    echo 'trivialInit-arch' | sudo tee "$MOUNT_DIR/etc/hostname"

    # Cleanup
    sudo umount "$MOUNT_DIR"
    sudo qemu-nbd --disconnect /dev/nbd0

    echo ""
    echo "Image ready: $IMAGE"
    echo "Boot with:"
    echo "  qemu-system-x86_64 -hda $IMAGE -m 1G -smp 2 -nographic \\"
    echo "    -append 'console=ttyS0 root=/dev/vda1 init=/sbin/init'"

else
    echo "--- No pacstrap found. Generating manual bootstrap script. ---"
    echo ""
    echo "On your Arch machine (dragonpad), run:"
    echo ""
    cat << 'INSTRUCTIONS'
# 1. Build trivialInit statically:
cd trivialInit
cmake -B build-static -DTINIT_STATIC=ON -DTINIT_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-Os"
cmake --build build-static -j$(nproc) --target trivialInit

# 2. Create QEMU disk image:
qemu-img create -f qcow2 qemu/arch-trivialinit.qcow2 4G
sudo modprobe nbd max_part=4
sudo qemu-nbd --connect=/dev/nbd0 qemu/arch-trivialinit.qcow2
sudo parted /dev/nbd0 --script mklabel gpt
sudo parted /dev/nbd0 --script mkpart primary ext4 1MiB 100%
sudo mkfs.ext4 /dev/nbd0p1

# 3. Install Arch + trivialInit:
sudo mkdir -p qemu/mnt
sudo mount /dev/nbd0p1 qemu/mnt
sudo pacstrap qemu/mnt base linux busybox
sudo cp build-static/trivialInit qemu/mnt/sbin/trivialinit
sudo chmod 755 qemu/mnt/sbin/trivialinit
sudo ln -sf /sbin/trivialinit qemu/mnt/sbin/init
echo '/dev/vda1 / ext4 defaults 0 1' | sudo tee qemu/mnt/etc/fstab
echo 'trivialInit-arch' | sudo tee qemu/mnt/etc/hostname
sudo umount qemu/mnt
sudo qemu-nbd --disconnect /dev/nbd0

# 4. Boot:
qemu-system-x86_64 \
    -drive file=qemu/arch-trivialinit.qcow2,format=qcow2 \
    -kernel /boot/vmlinuz-linux \
    -append "console=ttyS0 root=/dev/vda1 init=/sbin/init loglevel=3" \
    -nographic -m 1G -smp 2 -enable-kvm -no-reboot
INSTRUCTIONS
fi
