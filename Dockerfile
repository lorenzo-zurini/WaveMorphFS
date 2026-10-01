# WaveMorphFS: build, then a slim runtime with the tools it shells out to
FROM rust:1-bookworm AS build
WORKDIR /src
COPY . .
RUN cargo build --release --locked

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends fuse3 flac ffmpeg util-linux ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && echo user_allow_other >> /etc/fuse.conf \
 && useradd --uid 1000 --user-group --no-create-home --home-dir /home/wavemorph wavemorph   # fusermount3 needs a passwd entry
COPY --from=build /src/target/release/wavemorphfs /usr/local/bin/wavemorphfs
COPY docker/entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
