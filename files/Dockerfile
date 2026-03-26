FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/ /build/src/

RUN aarch64-linux-gnu-gcc \
    -shared -fPIC \
    -O2 -ffast-math \
    -fvisibility=hidden \
    -o /build/dissolver.so \
    /build/src/dissolver.c \
    /build/src/plugin.c \
    -I/build/src \
    -lm \
    && echo "Build succeeded"
