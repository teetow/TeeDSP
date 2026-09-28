FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make git ca-certificates pkg-config python3 python3-paho-mqtt libpipewire-0.3-dev \
    libcpp-httplib-dev nlohmann-json3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src src
COPY plugin plugin
COPY tests tests
COPY linux linux
RUN cmake -S linux -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build -j2 && ctest --test-dir /build --output-on-failure \
    && python3 -m unittest discover -s linux/tests -p 'test_*.py'

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpipewire-0.3-0 pipewire-bin libcpp-httplib0.11 python3 python3-paho-mqtt \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/teedsp /usr/local/bin/teedsp
COPY linux/web /app/web
COPY linux/route.py /app/route.py
COPY linux/volume_bridge.py /app/volume_bridge.py
WORKDIR /app
ENV PYTHONUNBUFFERED=1
USER 1001:1001
EXPOSE 8080
CMD ["python3", "/app/route.py"]
