#!/usr/bin/env bash
# Build the Firework artifact: patched glibc, kernel, user-level runtime and the
# evaluated applications. Everything lives in this tree:
#
#   glibc/    glibc 2.35 with the partitioned heap/stack layout   -> glibc/build/libc.so.6
#   kernel/   Linux 6.3 with Firework (mm/firework*.c, syscalls)  -> kernel/arch/x86/boot/bzImage
#   tests/    runtime (libfirework.so), applications, scripts    -> tests/runtime/build/lib/libfirework.so
#
# Usage:
#   ./build.sh                 # glibc + kernel + runtime + apps (no kernel install)
#   ./build.sh glibc|kernel|runtime|apps
#   ./build.sh kernel-install  # install kernel + modules + headers, update grub (sudo), then reboot
#
# The evaluation machine ships with all of this prebuilt and the kernel
# installed; see tests/README.md for running the experiments.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
J="$(nproc)"

build_glibc() {
    mkdir -p "$ROOT/glibc/build"
    cd "$ROOT/glibc/build"
    [ -f Makefile ] || ../configure --prefix=/usr --disable-werror
    make -j"$J"
    echo "glibc: $ROOT/glibc/build/libc.so.6"
}

build_kernel() {
    cd "$ROOT/kernel"
    [ -f .config ] || cp firework.config .config
    make olddefconfig
    make -j"$J"
    echo "kernel: $ROOT/kernel/arch/x86/boot/bzImage ($(make -s kernelrelease))"
}

install_kernel() {
    cd "$ROOT/kernel"
    ./build_kernel.sh install     # modules_install, install, headers_install, update-grub
}

build_runtime() {
    cmake -S "$ROOT/tests/runtime" -B "$ROOT/tests/runtime/build" >/dev/null
    make -C "$ROOT/tests/runtime/build" -j"$J"
    echo "runtime: $ROOT/tests/runtime/build/lib/libfirework.so"
}

build_apps() {
    for app in dataframe collab_filter pagerank feed_gen sci_kernel locktest unshare batching; do
        make -C "$ROOT/tests/$app" clean all
    done
}

case "${1:-all}" in
    glibc)          build_glibc ;;
    kernel)         build_kernel ;;
    kernel-install) install_kernel ;;
    runtime)        build_runtime ;;
    apps)           build_apps ;;
    all)            build_glibc; build_kernel; build_runtime; build_apps ;;
    *) echo "usage: $0 [all|glibc|kernel|kernel-install|runtime|apps]" >&2; exit 2 ;;
esac
