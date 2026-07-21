FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/bin/Kufar-Telegram-Notifier /app/Kufar-Telegram-Notifier
COPY kufar-configuration.json /app/kufar-configuration.json

ENV KUFAR_CONFIG_PATH=/app/kufar-configuration.json \
    KUFAR_CACHE_PATH=/data/cached-data.json

ENTRYPOINT ["/app/Kufar-Telegram-Notifier"]
