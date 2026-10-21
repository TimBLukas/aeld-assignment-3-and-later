#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
    if [ ! -d "${OUTDIR}/linux-stable/.git" ]; then
		echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
		git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
    fi
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
	# deepclean
	make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper

	# defconfig
	make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

	# build vmlinux
	make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

	# build modules and devicetree
	# make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules
	make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
	
	# Verify build
	if [ ! -f "arch/${ARCH}/boot/Image" ]; then
	    echo "ERROR: Kernel Image was not generated"
	    exit 1
	fi

	cp "arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]; then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
ROOTFS="${OUTDIR}/rootfs"

mkdir -p "${ROOTFS}"/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p "${ROOTFS}"/{usr/bin,usr/lib,usr/sbin,var/log}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]; then
    # git clone git://busybox.net/busybox.git
    git clone https://git.busybox.net/busybox.git

    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
else
    cd busybox
fi

# TODO: Make and install busybox
# Clean and configure BusyBox
make distclean
make defconfig

# Build BusyBox for AArch64
make -j4 ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- \
    CC=aarch64-none-linux-gnu-gcc

# Install BusyBox into the target root filesystem
make ARCH=arm64 \
    CROSS_COMPILE=aarch64-none-linux-gnu- \
    CC=aarch64-none-linux-gnu-gcc \
    CONFIG_PREFIX="${ROOTFS}" install

# TODO: Add library dependencies to rootfs
echo "Library dependencies"
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
BUSYBOX="${ROOTFS}/bin/busybox"

# Copy the dynamic linker
INTERPRETER=$(${CROSS_COMPILE}readelf -l "$BUSYBOX" | sed -n 's/.*interpreter: \(.*\)]/\1/p')
if [ -n "$INTERPRETER" ]; then
    INTERPRETER_PATH=$(find "$SYSROOT" -name "$(basename "$INTERPRETER")" -print -quit)
    if [ -n "$INTERPRETER_PATH" ]; then
        sudo mkdir -p "$ROOTFS$(dirname "$INTERPRETER")"
        sudo cp -L "$INTERPRETER_PATH" "$ROOTFS$INTERPRETER"
    fi
fi

# Copy shared libraries
${CROSS_COMPILE}readelf -d "$BUSYBOX" | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p' |
while read -r LIB; do
    LIB_PATH=$(find "$SYSROOT" -name "$LIB" -print -quit)
    if [ -n "$LIB_PATH" ]; then
        sudo cp -L "$LIB_PATH" "$ROOTFS/lib64/"
    else
        echo "Library not found: $LIB"
    fi
done



# TODO: Make device nodes
sudo mkdir -p "${ROOTFS}/dev"

if [ ! -c "${ROOTFS}/dev/null" ]; then
    sudo rm -rf "${ROOTFS}/dev/null"
    sudo mknod -m 666 "${ROOTFS}/dev/null" c 1 3
fi

if [ ! -c "${ROOTFS}/dev/console" ]; then
    sudo rm -rf "${ROOTFS}/dev/console"
    sudo mknod -m 600 "${ROOTFS}/dev/console" c 5 1
fi


# TODO: Clean and build the writer utility

cd ${FINDER_APP_DIR}
make clean
make writer CC="${CROSS_COMPILE}gcc"

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
# Create home directory
mkdir -p "${ROOTFS}/home/conf"
# Copy required files
cp finder.sh finder-test.sh writer autorun-qemu.sh "${ROOTFS}/home/"
cp conf/username.txt conf/assignment.txt "${ROOTFS}/home/conf/"
# make files executable
chmod +x "${ROOTFS}/home/finder.sh" "${ROOTFS}/home/finder-test.sh" \
         "${ROOTFS}/home/writer" "${ROOTFS}/home/autorun-qemu.sh"


cp start-qemu-terminal.sh "${ROOTFS}/home/start-qemu-terminal.sh"

# TODO: Chown the root directory
sudo chown root:root "${ROOTFS}"


# TODO: Create initramfs.cpio.gz
cd "${ROOTFS}"

sudo find . -print0 | sudo cpio --null -o -H newc --owner=0:0 > "${OUTDIR}/initramfs.cpio"

gzip -f "${OUTDIR}/initramfs.cpio"
