#!/bin/bash
# hp-supplies Windows 构建（MSYS2 UCRT64 环境）
# 依赖: pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gtk3 \
#                mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-pkgconf
# 用法: 在 MSYS2 UCRT64 shell 里  bash packaging/windows/build-msys2.sh [版本号]
set -e
cd "$(dirname "$0")/../.."
export PATH=/ucrt64/bin:/mingw64/bin:$PATH
VER="${1:-1.2.1}"
OUT=out/windows
mkdir -p "$OUT"

PKGS=$(pkgconf --cflags --libs gtk+-3.0 libusb-1.0)

echo "=== [1/3] 编译 console 版 (hp-supplies.exe) ==="
gcc -O2 -Wall -DAPP_VERSION="\"$VER\"" -o "$OUT/hp-supplies.exe" \
    src/main.c $PKGS -lpthread -lws2_32

echo "=== [2/3] 编译 GUI 版 (hp-supplies-gui.exe, 无控制台窗口) ==="
gcc -O2 -Wall -DAPP_VERSION="\"$VER\"" -mwindows -o "$OUT/hp-supplies-gui.exe" \
    src/main.c $PKGS -lpthread -lws2_32

echo "=== [3/3] 完成 ==="
ls -la "$OUT"
