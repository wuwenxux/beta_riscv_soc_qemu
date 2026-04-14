#!/bin/bash
# Build Ubuntu 25.10 (questing) minimal rootfs for Beta QEMU
set -e

DOCKER_HOST=unix:///Users/qingyuanren/.colima/default/docker.sock
export DOCKER_HOST

WORKDIR=/Users/qingyuanren/Desktop/QEMU
OUTPUT=$WORKDIR/ubuntu-25.10-rootfs.cpio.gz

echo "=== Building Ubuntu 25.10 rootfs in Docker ==="
docker run --rm --privileged \
    -v "$WORKDIR:/work" \
    ubuntu:25.10 \
    bash -c '
set -e
apt-get update -qq 2>/dev/null || true
apt-get install -y -qq debootstrap qemu-user-static cpio 2>/dev/null | tail -1

ROOTFS=/tmp/rootfs
mkdir -p $ROOTFS

echo "--- Running debootstrap (minbase) ---"
debootstrap --arch=riscv64 --variant=minbase --foreign questing $ROOTFS http://ports.ubuntu.com/ubuntu-ports/

echo "--- Setting up QEMU user emulation ---"
cp /usr/bin/qemu-riscv64-static $ROOTFS/usr/bin/ 2>/dev/null || true

echo "--- Running second stage ---"
chroot $ROOTFS /debootstrap/debootstrap --second-stage 2>/dev/null || true

echo "--- Configuring rootfs ---"
# Hostname
echo "beta-qemu" > $ROOTFS/etc/hostname

# fstab
cat > $ROOTFS/etc/fstab << EOF2
proc /proc proc defaults 0 0
sysfs /sys sysfs defaults 0 0
devtmpfs /dev devtmpfs defaults 0 0
EOF2

# Serial console
mkdir -p $ROOTFS/etc/systemd/system/getty.target.wants
ln -sf /lib/systemd/system/serial-getty@.service \
    $ROOTFS/etc/systemd/system/getty.target.wants/serial-getty@ttyS0.service 2>/dev/null || true

# Root password (empty for testing)
chroot $ROOTFS passwd -d root 2>/dev/null || true

# Init script for non-systemd fallback
cat > $ROOTFS/init << EOF2
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo "==========================================="
echo "  Beta SoC QEMU - Ubuntu 25.10 Booted!"
echo "==========================================="
cat /proc/cpuinfo | grep -c processor | xargs echo "CPUs online:"
uname -a
exec /sbin/init 2>/dev/null || exec /bin/sh
EOF2
chmod +x $ROOTFS/init

# Remove QEMU static binary
rm -f $ROOTFS/usr/bin/qemu-riscv64-static

echo "--- Creating cpio archive ---"
cd $ROOTFS
find . | cpio -H newc -o 2>/dev/null | gzip -9 > /work/ubuntu-25.10-rootfs.cpio.gz
echo "=== Ubuntu 25.10 rootfs created ==="
'

ls -lh "$OUTPUT"
echo "=== Done: $OUTPUT ==="
