FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN mkdir build && cd build && \
    cmake .. && \
    cmake --build . --target telegram-bot-api

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libssl3 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/build/telegram-bot-api /usr/local/bin/telegram-bot-api

EXPOSE 8081

ENTRYPOINT ["telegram-bot-api"]
