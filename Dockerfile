# Build environment for the module against a distribution kernel, and for
# the Debian package.
#   docker build --build-arg IMAGE=debian:12 --build-arg HEADERS=linux-headers-amd64 -t hwsv4l2-build .
ARG IMAGE=ubuntu:24.04
FROM ${IMAGE}
ARG HEADERS=linux-headers-generic
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ${HEADERS} kmod dkms dh-dkms debhelper dpkg-dev fakeroot \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
