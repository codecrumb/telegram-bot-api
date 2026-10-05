FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    patch \
    libssl-dev \
    zlib1g-dev \
    gperf \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN git submodule update --init --recursive

# adds HttpConnectionBase::write_file: streams a file to the peer with backpressure
RUN patch -p1 -d td < patches/td-http-file-streaming.patch

RUN cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build && \
    cmake --build build --target telegram-bot-api --parallel && \
    cp build/telegram-bot-api /telegram-bot-api

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libssl3 \
    zlib1g \
    libstdc++6 \
    ca-certificates \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /telegram-bot-api /usr/local/bin/telegram-bot-api

WORKDIR /data

EXPOSE 8081

ENTRYPOINT ["telegram-bot-api", "--dir", "/data"]
