# ===========================================
# Stage 1: Build Stage
# ===========================================

FROM ubuntu:24.04 AS builder
LABEL authors="ican"

ENV DEBIAN_FRONTEND=noninteractive

# Install build essentials, Python/pip, Git, and WebRTC dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    python3-pip \
    git \
    ninja-build \
    pkg-config \
    ca-certificates \
    libssl-dev \
    libsrtp2-dev \
    libglib2.0-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    libnice-dev \
    && rm -rf /var/lib/apt/lists/*

# Install modern CMake (4.2+) via pip
RUN pip install --break-system-packages "cmake>=4.2.0"

WORKDIR /app

COPY . /app

# Configure CMake project
RUN cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja

# Compile the application
RUN cmake --build build --config Release -j$(nproc)

# ===================================================
# Stage 2: Runtime Stage
# ===================================================
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive

# Install GStreamer runtime libraries and essential WebRTC plugins
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    libglib2.0-0 \
    libgstreamer1.0-0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libnice10 \
    gstreamer1.0-nice \
    && rm -rf /var/lib/apt/lists/*

# Generate a non-root user for security
RUN #useradd -m -u 1000 appuser && mkdir -p /app && chown -R appuser:appuser /app
RUN mkdir -p /app && chown -R ubuntu:ubuntu /app
#USER appuser
USER ubuntu

WORKDIR /app

# Copy the compiled binary from the builder stage
COPY --from=builder /app/build/webrtc_app /app/webrtc_app

# GStreamer plugins registry path settings
ENV GST_PLUGIN_SYSTEM_PATH=/usr/lib/x86_64-linux-gnu/gstreamer-1.0

ENV PORT=9001
EXPOSE 9001

CMD ["/app/webrtc_app"]