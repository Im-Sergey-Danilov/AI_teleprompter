# whisper.cpp server with CUDA, exposing an OpenAI-shaped transcription route.
#
#   build arg CUDA_ARCH is the compute capability of the target GPU
#   (89 = Ada / RTX 40xx, 86 = Ampere / RTX 30xx, 75 = Turing / RTX 20xx).
#   Restricting it to one arch cuts the build from ~40 min to ~8.

ARG CUDA_IMAGE=12.6.3
FROM nvidia/cuda:${CUDA_IMAGE}-devel-ubuntu22.04 AS build

ARG WHISPER_TAG=v1.7.4
ARG CUDA_ARCH=89

RUN apt-get update && apt-get install -y --no-install-recommends \
        git cmake build-essential ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch ${WHISPER_TAG} https://github.com/ggml-org/whisper.cpp.git .

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DGGML_CUDA=ON \
        -DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCH} \
        -DWHISPER_BUILD_SERVER=ON \
        -DWHISPER_BUILD_EXAMPLES=ON \
        -DWHISPER_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)" --target whisper-server

# --------------------------------------------------------------------------
ARG CUDA_IMAGE=12.6.3
FROM nvidia/cuda:${CUDA_IMAGE}-runtime-ubuntu22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl ffmpeg \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bin/whisper-server /usr/local/bin/whisper-server
COPY --from=build /src/samples/jfk.wav /opt/jfk.wav

# --convert writes its scratch wav into the working directory
WORKDIR /tmp
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/whisper-server"]
