FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 컴파일러, 디버거, 메모리 검사, 네트워크 확인 도구
RUN apt-get update && apt-get install -y \
    build-essential \
    gdb \
    valgrind \
    curl \
    netcat-openbsd \
    lsof \
    iproute2 \
    net-tools \
    tcpdump \
    strace \
    apache2-utils \
    man-db manpages-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
