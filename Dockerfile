# SPDX-License-Identifier: GPL-3.0-or-later
# WaveMorphFS: build and test, then a slim runtime with just the libraries the
# filesystem links (the flac/ffmpeg tools are only used by verify/compare and the
# tests, which run in the build stage).
FROM debian:trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config libflac-dev libwavpack-dev nlohmann-json3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev flac ffmpeg \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# only what the build reads, so documentation edits keep the layer cache
COPY CMakeLists.txt ./
COPY src src
COPY tests tests
ARG WAVEMORPH_VERSION=""
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWAVEMORPH_WERROR=ON ${WAVEMORPH_VERSION:+-DWAVEMORPH_VERSION=$WAVEMORPH_VERSION} \
 && cmake --build build \
 && build/wavemorphfs-tests

FROM debian:trixie-slim
ARG WAVEMORPH_VERSION=""
LABEL org.opencontainers.image.title="WaveMorphFS" \
      org.opencontainers.image.description="NFS server presenting pristine music files (CUE images, APE, WavPack, SACD ISOs) as split, tagged tracks" \
      org.opencontainers.image.licenses="GPL-3.0-or-later" \
      org.opencontainers.image.source="https://github.com/lorenzo-zurini/WaveMorphFS" \
      org.opencontainers.image.version="${WAVEMORPH_VERSION}"
RUN apt-get update \
 && apt-get install -y --no-install-recommends libflac14 libwavpack1 libavformat61 libavcodec61 libavutil59 libswscale8 util-linux ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --uid 1000 --user-group --no-create-home --home-dir /home/wavemorph wavemorph
COPY --from=build /src/build/wavemorphfs /usr/local/bin/wavemorphfs
COPY docker/entrypoint.sh /entrypoint.sh
# other containers mount it as an NFS volume (see the README)
ENV WAVEMORPH_NFS=2049 \
    WAVEMORPH_TAGS_DIR=/wavemorph/tags \
    WAVEMORPH_CACHE_DIR=/wavemorph/cache \
    WAVEMORPH_LOG=info
EXPOSE 2049
HEALTHCHECK --interval=10s --timeout=5s --start-period=30s --retries=3 CMD ["/entrypoint.sh", "health"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["/entrypoint.sh"]
