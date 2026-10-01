# WaveMorphFS: build, then a slim runtime with the tools it shells out to
FROM debian:trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config libfuse3-dev libflac-dev nlohmann-json3-dev libavformat-dev libavcodec-dev libavutil-dev flac ffmpeg \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build \
 && build/wavemorphfs-tests

FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends fuse3 libfuse3-4 libflac14 flac ffmpeg util-linux ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && echo user_allow_other >> /etc/fuse.conf \
 && useradd --uid 1000 --user-group --no-create-home --home-dir /home/wavemorph wavemorph   # fusermount3 needs a passwd entry
COPY --from=build /src/build/wavemorphfs /usr/local/bin/wavemorphfs
COPY docker/entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
