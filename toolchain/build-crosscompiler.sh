#!/usr/bin/env bash
set -euo pipefail

BINUTILS_VER=2.43
GCC_VER=14.2.0
PREFIX="${PREFIX:-$HOME/opt/cross}"
TARGET=x86_64-elf
JOBS="$(nproc)"

mkdir -p "$PREFIX" /tmp/cross-build
cd /tmp/cross-build

echo "==> binutils $BINUTILS_VER"
[ -f binutils-$BINUTILS_VER.tar.xz ] || \
  curl -LO "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz"
tar xf binutils-$BINUTILS_VER.tar.xz
mkdir -p build-binutils && cd build-binutils
../binutils-$BINUTILS_VER/configure \
    --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS" && make install
cd ..

export PATH="$PREFIX/bin:$PATH"

echo "==> gcc $GCC_VER"
[ -f gcc-$GCC_VER.tar.xz ] || \
  curl -LO "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
tar xf gcc-$GCC_VER.tar.xz
cd gcc-$GCC_VER && ./contrib/download_prerequisites && cd ..
mkdir -p build-gcc && cd build-gcc
../gcc-$GCC_VER/configure \
    --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c,c++ --without-headers
make -j"$JOBS" all-gcc all-target-libgcc
make install-gcc install-target-libgcc

echo
echo "Готово. Додайте до ~/.bashrc:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""