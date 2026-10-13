#!/bin/bash
# desk-info 编译脚本：静态链接 ARM soft-float 二进制（独立于设备 hard-float uClibc）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SDK_PATH="${SDK_PATH:-$SCRIPT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC="$SDK_PATH/bin/arm-buildroot-linux-uclibcgnueabi-gcc"

mkdir -p "$BUILD_DIR"
echo "CC: $CC"
"$CC" -static -Os -mcpu=cortex-a5 -mfloat-abi=soft \
    -o "$BUILD_DIR/desk-info" "$SCRIPT_DIR/main.c"
echo "built: $BUILD_DIR/desk-info"
ls -la "$BUILD_DIR/desk-info"
