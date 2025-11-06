#!/bin/sh

#set -x

# You probably will need to change this
OPENWRT_PATH="../openwrt"

# This is most likely okay but you might need to adjust it
COMPILER_PATH=$(readlink -f $OPENWRT_PATH/staging_dir/toolchain-mips_24kc_gcc-*_musl/bin)

# This should be fine
LINUX_PATH=$(readlink -f $OPENWRT_PATH/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.*)

CROSS_COMPILE="$COMPILER_PATH/mips-openwrt-linux-musl-"

# STAGING_DIR doesn't do anything but it stops a warning from the compiler wrapper
make \
	STAGING_DIR=$(readlink -f .) \
	CROSS_COMPILE=$COMPILER_PATH/mips-openwrt-linux-musl- \
	ARCH=mips \
	-C "$LINUX_PATH" \
	M=$PWD \
	${@:-modules}