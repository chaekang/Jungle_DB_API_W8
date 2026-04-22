FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-format \
        clangd \
        cmake \
        gdb \
        git \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        valgrind \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
