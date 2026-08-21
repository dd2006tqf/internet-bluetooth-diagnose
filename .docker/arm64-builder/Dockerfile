FROM debian:11

ARG DEBIAN_FRONTEND=noninteractive
ARG LIBBPF_VERSION=v1.3.0

RUN printf '%s\n' \
        'Acquire::Retries "8";' \
        'Acquire::http::Timeout "180";' \
        'Acquire::https::Timeout "180";' \
        'APT::Get::Assume-Yes "true";' \
        > /etc/apt/apt.conf.d/80-network-retries && \
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
        ca-certificates \
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
        libsqlite3-dev && \
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
