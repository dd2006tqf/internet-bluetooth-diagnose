# 可复现 ARM64 构建镜像（Assurance v1 基线，Bullseye）
#
# 三层可复现：
#   1. base image @ sha256:digest
#   2. snapshot.debian.org 明确日期（20260912T000000Z）
#   3. exact package versions
#
# TLS 依赖说明（W1，D 路线）：
#   libssl-dev 不经 APT 安装（其 security 索引/pool 不一致导致安装失败）。
#   改为下载固定版本 .deb + SHA256 校验，dpkg-deb -x 解压到
#   /opt/weaknet-openssl-1.1/ 私有 sysroot，CMake 经 OPENSSL_ROOT_DIR 指向。
#   最终二进制 NEEDED 仍是 libssl.so.1.1（与开发板运行库一致）。
#   详见 scripts/setup_openssl_sysroot.sh。

FROM debian@sha256:6cb68b1be980a0e5b19be25582b34b5cf9cb466d52d08ab4354b79051f2cd298

ARG DEBIAN_FRONTEND=noninteractive
ARG LIBBPF_VERSION=v1.3.0
ARG SNAPSHOT=20260912T000000Z
ARG CA_CERTIFICATES_VERSION=20210119

# ---------- HTTP bootstrap（同一 snapshot，只 bullseye/main）----------
RUN printf '%s\n' \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} bullseye main" \
        > /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates=${CA_CERTIFICATES_VERSION} && \
    rm -rf /var/lib/apt/lists/*

# ---------- HTTPS（同一 snapshot），装全部构建依赖 ----------
RUN printf '%s\n' \
        "deb [check-valid-until=no] https://snapshot.debian.org/archive/debian/${SNAPSHOT} bullseye main" \
        > /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        make \
        cmake \
        pkg-config \
        clang \
        llvm \
        git \
        file \
        tar \
        gzip \
        libdbus-1-dev \
        libgoogle-glog-dev \
        libelf-dev \
        zlib1g-dev \
        libcap-dev \
        libnl-3-dev \
        libnl-genl-3-dev \
        libsqlite3-dev \
        libgtest-dev \
        libgmock-dev && \
    rm -rf /var/lib/apt/lists/*

RUN git clone \
        --depth 1 \
        --branch "${LIBBPF_VERSION}" \
        https://github.com/libbpf/libbpf.git \
        /tmp/libbpf && \
    make -C /tmp/libbpf/src -j1 && \
    make -C /tmp/libbpf/src install \
        PREFIX=/usr/local \
        LIBDIR=/usr/local/lib && \
    echo '/usr/local/lib' \
        > /etc/ld.so.conf.d/local-libbpf.conf && \
    ldconfig && \
    rm -rf /tmp/libbpf

ENV CPATH=/usr/local/include
ENV CPLUS_INCLUDE_PATH=/usr/local/include
ENV LIBRARY_PATH=/usr/local/lib
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

WORKDIR /src
