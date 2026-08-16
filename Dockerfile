# build container
FROM alpine:latest AS build

# install build dependencies
RUN apk add --no-cache \
    build-base \
    cmake \
    git \
    pkgconf \
    linux-headers \
    ca-certificates \
    lame-dev \
    libshout-dev \
    libconfig-dev \
    fftw-dev \
    libusb-dev \
    pulseaudio-dev \
    soapy-sdr-dev \
    airspyone-host-dev

# Alpine ships CMake 4, which dropped compatibility with cmake_minimum_required
# < 3.5, allow configuring them anyway.
ENV CMAKE_POLICY_VERSION_MINIMUM=3.5

# set working dir for compiling dependencies
WORKDIR /build_dependencies

# compile / install rtl-sdr-blog version of rtl-sdr for v4 support.
RUN git clone --depth 1 https://github.com/rtlsdrblog/rtl-sdr-blog && \
    git -C rtl-sdr-blog log -1 --format='rtl-sdr-blog checked out: %H (%ci)' && \
    cmake -S rtl-sdr-blog -B rtl-sdr-blog/build \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DDETACH_KERNEL_DRIVER=ON && \
    cmake --build rtl-sdr-blog/build -j4 && \
    cmake --install rtl-sdr-blog/build

# compile / install libmirisdr-4
RUN git clone --depth 1 https://github.com/f4exb/libmirisdr-4 && \
    git -C libmirisdr-4 log -1 --format='libmirisdr-4 checked out: %H (%ci)' && \
    cmake -S libmirisdr-4 -B libmirisdr-4/build -DCMAKE_INSTALL_PREFIX=/usr && \
    cmake --build libmirisdr-4/build -j4 && \
    cmake --install libmirisdr-4/build

# compile / install the SoapySDR airspy driver module. SoapySDR itself comes from the
# soapy-sdr-dev package; its device modules are not packaged for Alpine.
RUN git clone --depth 1 https://github.com/pothosware/SoapyAirspy && \
    git -C SoapyAirspy log -1 --format='SoapyAirspy checked out: %H (%ci)' && \
    cmake -S SoapyAirspy -B SoapyAirspy/build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release && \
    cmake --build SoapyAirspy/build -j4 && \
    cmake --install SoapyAirspy/build

# set working dir for project build
WORKDIR /rtl_airband_build

# copy in the rtl_airband source, coping in the full repo so find_version will be correct
COPY ./ .

# configure and build
# TODO: detect platforms
RUN cmake -B build_dir -DPLATFORM=generic -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DBUILD_UNITTESTS=TRUE && \
    VERBOSE=1 cmake --build build_dir -j4

# make sure unit tests pass
RUN ./build_dir/src/unittests


# application container
FROM alpine:latest

# install runtime dependencies
RUN apk add --no-cache \
    tini \
    libstdc++ \
    lame-libs \
    libshout \
    libconfig++ \
    fftw-single-libs \
    libpulse \
    libusb \
    ca-certificates \
    soapy-sdr \
    airspyone-host-libs

# copy (from build container) the source-built SDR libraries and the airspy Soapy module
COPY --from=build /usr/lib/librtlsdr.so* /usr/lib/
COPY --from=build /usr/lib/libmirisdr.so* /usr/lib/
COPY --from=build /usr/lib/SoapySDR/modules0.8/libairspySupport.so /usr/lib/SoapySDR/modules0.8/

# blacklist the in-kernel DVB drivers so rtl-sdr can claim the device
RUN mkdir -p /etc/modprobe.d && \
    printf '\nblacklist dvb_usb_rtl28xxun\nblacklist rtl2832\nblacklist rtl2830\n' \
    >> /etc/modprobe.d/rtl_sdr.conf

# Copy rtl_airband from the build container
COPY LICENSE /app/
COPY --from=build /rtl_airband_build/build_dir/src/unittests /app/
COPY --from=build /rtl_airband_build/build_dir/src/rtl_airband /app/
RUN chmod a+x /app/unittests /app/rtl_airband

# make sure unit tests pass
RUN /app/unittests

# Use tini as init and run rtl_airband from /app/
ENTRYPOINT ["/sbin/tini", "--"]
WORKDIR /app/
CMD ["/app/rtl_airband", "-F", "-e", "-c", "/app/rtl_airband.conf"]
