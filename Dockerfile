# Stage 1: Build environment
FROM debian:bookworm-slim AS builder

# Install necessary packages
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    ninja-build \
    cmake=3.25.1-1 \
    git \
    pkg-config \
    libusb-1.0-0-dev \
    libmp3lame-dev \
    libshout3-dev \
    libconfig++-dev \
    libfftw3-dev \
    libsoapysdr-dev \
    libpulse-dev \
    libzmq3-dev \
    dpkg-dev
# Clean up apt cache
RUN rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# install rtlsdr
RUN git clone --branch v1.3.6 --single-branch https://github.com/rtlsdrblog/rtl-sdr-blog.git && \
    cd rtl-sdr-blog && \
    rm -rf build && mkdir build && \
    cmake -G Ninja -B build -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release && \
    mkdir -p /tmp/rtl-sdr-blog/DEBIAN && \
    ARCH=$(dpkg --print-architecture) && \
    echo "Package: rtl-sdr-blog\nVersion: 1.3.6\nArchitecture: $ARCH\nMaintainer: Builder <builder@example.com>\nDescription: RTL-SDR Blog drivers\nDepends: libusb-1.0-0" > /tmp/rtl-sdr-blog/DEBIAN/control && \
    DESTDIR=/tmp/rtl-sdr-blog cmake --install build && \
    dpkg-deb --build --root-owner-group /tmp/rtl-sdr-blog && \
    mv /tmp/rtl-sdr-blog.deb /workspace/ && \
    cd .. && rm -rf rtl-sdr-blog /tmp/rtl-sdr-blog

# install airspy
RUN git clone --branch v1.0.10 --single-branch https://github.com/airspy/airspyone_host.git && \
    cd airspyone_host && \
    rm -rf build && mkdir build && \
    cmake -G Ninja -B build -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release && \
    mkdir -p /tmp/airspyone_host/DEBIAN && \
    ARCH=$(dpkg --print-architecture) && \
    echo "Package: airspyone-host\nVersion: 1.0.10\nArchitecture: $ARCH\nMaintainer: Builder <builder@example.com>\nDescription: Airspy host drivers\nDepends: libusb-1.0-0" > /tmp/airspyone_host/DEBIAN/control && \
    DESTDIR=/tmp/airspyone_host cmake --install build && \
    dpkg-deb --build --root-owner-group /tmp/airspyone_host && \
    mv /tmp/airspyone_host.deb /workspace/ && \
    cd .. && rm -rf airspyone_host /tmp/airspyone_host

# Copy local source code into the builder stage
COPY . /workspace/rtlsdr-airband-src

# install rtlsdr-airband
RUN cd /workspace/rtlsdr-airband-src && \
    rm -rf build && mkdir build && \
    VERSION=$(git describe --tags --always --dirty=-dev || echo "unknown") && \
    cmake -G Ninja -B build -S . -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -D NFM=ON -D MIRISDR=OFF -D PLATFORM=native . && \
    cmake --build build --config Release && \
    mkdir -p /tmp/RTLSDR-Airband/DEBIAN && \
    ARCH=$(dpkg --print-architecture) && \
    echo "Package: rtlsdr-airband\nVersion: $VERSION\nArchitecture: $ARCH\nMaintainer: Builder <builder@example.com>\nDescription: RTLSDR-Airband application (local build)\nDepends: libconfig++9v5, libfftw3-single3 | libfftw3-double3, libmp3lame0, libpulse0, libshout3, libsoapysdr0.8, libusb-1.0-0, libzmq5, libc6" > /tmp/RTLSDR-Airband/DEBIAN/control && \
    DESTDIR=/tmp/RTLSDR-Airband cmake --install build && \
    dpkg-deb --build --root-owner-group /tmp/RTLSDR-Airband && \
    mv /tmp/RTLSDR-Airband.deb /workspace/ && \
    rm -rf /workspace/rtlsdr-airband-src /tmp/RTLSDR-Airband

# Stage 2: Final image
FROM debian:bookworm-slim

# Install runtime dependencies from your previous final stage
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libc6 \
    libconfig++9v5 \
    libfftw3-single3 \
    libmp3lame0 \
    libpulse0 \
    libshout3 \
    libsoapysdr0.8 \
    soapysdr0.8-module-rtlsdr \
    soapysdr-module-airspy \
    libusb-1.0-0 \
    libzmq5 \
    # Clean up apt cache in the same layer
    && rm -rf /var/lib/apt/lists/*

# Copy the built .deb packages from the builder stage
COPY --from=builder /workspace/*.deb /tmp/

# Install the .deb packages and handle dependencies
RUN dpkg -i /tmp/*.deb || apt-get update && apt-get install -f -y --no-install-recommends

# Clean up the .deb files
RUN rm /tmp/*.deb
