FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential cmake pkg-config libwxgtk3.2-dev libavcodec-dev libavutil-dev libswscale-dev libdav1d-dev patchelf file ca-certificates libfuse2 squashfs-tools desktop-file-utils && rm -rf /var/lib/apt/lists/*
