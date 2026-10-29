# Container image for fuzzing libriscv's ELF loader with libFuzzer.
#
# Build context must be the repository root so that ../lib is available:
#   docker build -t libriscv-fuzz -f fuzz/Dockerfile .
#
# This is normally driven by fuzz/docker.sh.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
	cmake make git ca-certificates \
	clang-18 lld-18 llvm-18 libclang-rt-18-dev \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18

WORKDIR /app

# Only copy what the fuzzers need, to avoid pulling in the host's prebuilt
# build/ directories (build, build_c, build_libfuzzer, ...).
COPY lib /app/lib
COPY fuzz/CMakeLists.txt fuzz/fuzz.cpp fuzz/helpers.cpp /app/fuzz/
COPY fuzz/testcases   /app/fuzz/testcases
COPY fuzz/testcases64 /app/fuzz/testcases64

# Real RISC-V ELF binaries as seed corpus. The ELF loader rejects random
# bytes at the header, so without a valid ELF seed the fuzzer never reaches
# interesting code. led_hello.elf is rv32, hello is rv64.
COPY binaries/led_hello.elf /app/fuzz/seeds32/led_hello.elf
COPY binaries/hello         /app/fuzz/seeds64/hello

# Build only the ELF loader fuzzers (32- and 64-bit).
WORKDIR /app/fuzz/build
RUN cmake .. -DRISCV_128I=ON -DRISCV_BINARY_TRANSLATION=OFF \
 && make -j"$(nproc)" elffuzzer32 elffuzzer64

# Fuzzers run from here, e.g. ./elffuzzer64 ...
WORKDIR /app/fuzz/build
