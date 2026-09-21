# 交叉编译环境（Apple Silicon 上跑 arm64 容器，原生速度，无 Rosetta）
# aarch64-linux-gnu- 交叉 gcc + 设备 sysroot（reference/device/rootfs）出板端二进制。
# M1 编 FFmpeg 时在本镜像上追加 apt 依赖即可，不重装工具链。
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates gcc-aarch64-linux-gnu libc6-dev-arm64-cross make xz-utils curl file \
    && rm -rf /var/lib/apt/lists/*

# 备用：Arm 官方 13.2.Rel1（x86_64 host，需 Rosetta）。
# Arm 已把下载迁到 GitLab，darwin 无 linux-gnu 变体，故仅 Linux 容器可用：
# https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/13.2.Rel1/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu.tar.xz

WORKDIR /work
ENV CROSS=aarch64-linux-gnu-
