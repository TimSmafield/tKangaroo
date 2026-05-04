ARG CUDA_DEVEL_IMAGE=nvidia/cuda:12.4.1-devel-ubuntu22.04
ARG CUDA_RUNTIME_IMAGE=nvidia/cuda:12.4.1-runtime-ubuntu22.04

FROM ${CUDA_DEVEL_IMAGE} AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential libgmp-dev libssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# The legacy Makefile expects these historical tool paths.
RUN ln -s /usr/local/cuda /usr/local/cuda-8.0 && \
    ln -s /usr/bin/g++ /usr/bin/g++-4.8

WORKDIR /opt/Kangaroo
ARG BRANCH=local
ARG CCAP=61
ARG GIT_COMMIT=unknown
COPY . .

RUN echo "Building local workspace (BRANCH arg kept for CLI compatibility: ${BRANCH})" \
 && make clean || true

RUN make -j"$(nproc)" gpu=1 ccap=${CCAP} GIT_COMMIT=${GIT_COMMIT} all \
 && make -j"$(nproc)" gpu=1 ccap=${CCAP} GIT_COMMIT=${GIT_COMMIT} perf \
 && mkdir -p /out \
 && install -m755 ./kangaroo /out/kangaroo \
 && install -m755 ./kangaroo-perf /out/kangaroo-perf

FROM ${CUDA_RUNTIME_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgomp1 libgmp10 libssl3 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

ARG INSTALL_TAILSCALE=0
RUN if [ "${INSTALL_TAILSCALE}" = "1" ]; then \
      apt-get update && apt-get install -y --no-install-recommends \
        curl gnupg iproute2 iptables \
      && curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/jammy.noarmor.gpg \
        -o /usr/share/keyrings/tailscale-archive-keyring.gpg \
      && curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/jammy.tailscale-keyring.list \
        -o /etc/apt/sources.list.d/tailscale.list \
      && apt-get update && apt-get install -y --no-install-recommends tailscale \
      && rm -rf /var/lib/apt/lists/*; \
    fi

ENV NVIDIA_VISIBLE_DEVICES=all
ENV NVIDIA_DRIVER_CAPABILITIES=compute,utility

WORKDIR /work
COPY --from=build /out/kangaroo /usr/local/bin/kangaroo
COPY --from=build /out/kangaroo-perf /usr/local/bin/kangaroo-perf
COPY docker/kangaroo-entrypoint.sh /usr/local/bin/kangaroo-entrypoint
RUN chmod +x /usr/local/bin/kangaroo-entrypoint
ENTRYPOINT ["kangaroo-entrypoint"]
CMD ["kangaroo"]
