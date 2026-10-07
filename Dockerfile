# WaveMorphFS: build, then a slim runtime with the tools it shells out to
FROM debian:trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config libfuse3-dev libflac-dev nlohmann-json3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev flac ffmpeg \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build \
 && build/wavemorphfs-tests

FROM debian:trixie-slim
LABEL org.opencontainers.image.title="WaveMorphFS" \
      org.opencontainers.image.description="FUSE filesystem presenting pristine music files (CUE images, APE, SACD ISOs) as split, tagged tracks" \
      org.opencontainers.image.licenses="GPL-3.0-or-later" \
      org.opencontainers.image.source="https://github.com/lorenzo-zurini/WaveMorphFS"
RUN apt-get update \
 && apt-get install -y --no-install-recommends fuse3 libfuse3-4 libflac14 flac ffmpeg util-linux ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && echo user_allow_other >> /etc/fuse.conf \
 && useradd --uid 1000 --user-group --no-create-home --home-dir /home/wavemorph wavemorph   # fusermount3 needs a passwd entry
COPY --from=build /src/build/wavemorphfs /usr/local/bin/wavemorphfs
COPY docker/entrypoint.sh /entrypoint.sh
# bind a host directory here with rshared propagation; the mount appears in mnt/
ENV WAVEMORPH_MOUNT=/wavemorph/mnt \
    WAVEMORPH_TAGS_DIR=/wavemorph/tags \
    WAVEMORPH_CACHE_DIR=/wavemorph/cache \
    WAVEMORPH_LOG=info
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 CMD mountpoint -q "$WAVEMORPH_MOUNT"
ENTRYPOINT ["/entrypoint.sh"]
