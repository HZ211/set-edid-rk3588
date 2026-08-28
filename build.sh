#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

SDK_ROOT="${SDK_ROOT:-/home/totainfo/3.RK3588_Linux6.1_Release}"
SYSROOT="${SYSROOT:-${SDK_ROOT}/ubuntu/6.rootfs_nanopi_gst1.26_kvm1.7-20260524}"
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu}"
CROSS_COMPILE="${CROSS_COMPILE:-${TOOLCHAIN_DIR}/bin/aarch64-none-linux-gnu-}"
CC="${CC:-${CROSS_COMPILE}gcc}"
READELF="${READELF:-${CROSS_COMPILE}readelf}"

SOURCE="${SOURCE:-${SCRIPT_DIR}/s_edid.c}"
OUTPUT="${OUTPUT:-${SCRIPT_DIR}/set_edid}"
MULTIARCH_INCLUDE="${SYSROOT}/usr/include/aarch64-linux-gnu"
MULTIARCH_LIB="${SYSROOT}/usr/lib/aarch64-linux-gnu"

die() {
    printf '错误: %s\n' "$*" >&2
    exit 1
}

[[ -x "${CC}" ]] || die "找不到交叉编译器: ${CC}"
[[ -x "${READELF}" ]] || die "找不到 readelf: ${READELF}"
[[ -d "${SYSROOT}" ]] || die "找不到 sysroot: ${SYSROOT}"
[[ -f "${SOURCE}" ]] || die "找不到源文件: ${SOURCE}"
[[ -f "${SCRIPT_DIR}/set_edid.h" ]] || die "找不到头文件: ${SCRIPT_DIR}/set_edid.h"
[[ -d "${MULTIARCH_INCLUDE}" ]] || die "sysroot 缺少多架构头文件目录: ${MULTIARCH_INCLUDE}"
[[ -d "${MULTIARCH_LIB}" ]] || die "sysroot 缺少多架构库目录: ${MULTIARCH_LIB}"

mkdir -p -- "$(dirname -- "${OUTPUT}")"

printf '交叉编译器: %s\n' "${CC}"
printf '目标 sysroot: %s\n' "${SYSROOT}"
printf '输入文件: %s\n' "${SOURCE}"
printf '输出文件: %s\n' "${OUTPUT}"

"${CC}" \
    --sysroot="${SYSROOT}" \
    -isystem "${MULTIARCH_INCLUDE}" \
    -B"${MULTIARCH_LIB}/" \
    -L"${MULTIARCH_LIB}" \
    -Wl,-rpath-link,"${MULTIARCH_LIB}" \
    -O2 \
    -pipe \
    -Wall \
    -Wextra \
    ${CFLAGS:-} \
    -o "${OUTPUT}" \
    "${SOURCE}" \
    ${LDFLAGS:-}

"${READELF}" -h "${OUTPUT}" | grep -Eq 'Machine:[[:space:]]+AArch64' \
    || die "生成文件不是 AArch64 ELF: ${OUTPUT}"

printf '构建成功: %s\n' "${OUTPUT}"
file "${OUTPUT}"
