FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential cmake git libssl-dev libboost-all-dev \
    pkg-config python3 wget libcurl4-openssl-dev \
    ninja-build meson libspandsp-dev nlohmann-json3-dev \
    default-libmysqlclient-dev gdb curl \
    && rm -rf /var/lib/apt/lists/*

# Install WebSocket++
WORKDIR /usr/src
RUN git clone https://github.com/zaphoyd/websocketpp.git \
    && cd websocketpp && mkdir build && cd build \
    && cmake .. && make install

# Install Pistache
RUN git clone https://github.com/pistacheio/pistache.git \
    && cd pistache && git checkout v0.4.26 \
    && meson setup build --buildtype=release \
    -DPISTACHE_USE_SSL=true -DPISTACHE_BUILD_TESTS=false \
    && ninja -C build && ninja -C build install

# Configure PJSIP
RUN mkdir -p /usr/local/include/pjsip_config

# Build PJSIP with epoll and high-capacity limits
# PJ_IOQUEUE_MAX_HANDLES: 1024 -> 16384 for handling more concurrent connections
# PJSIP_MAX_TSX_COUNT: 1000 -> 10000 for more simultaneous transactions
# PJSIP_MAX_DIALOG_COUNT: 1000 -> 10000 for more simultaneous dialogs
# PJSUA_MAX_CALLS: 1000 -> 5000 for more simultaneous calls
RUN wget https://github.com/pjsip/pjproject/archive/2.15.1.tar.gz \
    && tar -xzf 2.15.1.tar.gz && cd pjproject-2.15.1 \
    && echo "#define PJ_HAS_EPOLL 1" > pjlib/include/pj/config_site.h \
    && echo "#define PJ_IOQUEUE_MAX_HANDLES 16384" >> pjlib/include/pj/config_site.h \
    && echo "#define PJSIP_MAX_TSX_COUNT 10000" >> pjlib/include/pj/config_site.h \
    && echo "#define PJSIP_MAX_DIALOG_COUNT 10000" >> pjlib/include/pj/config_site.h \
    && echo "#define PJSUA_MAX_CALLS 5000" >> pjlib/include/pj/config_site.h \
    && echo "#define PJ_IS_LITTLE_ENDIAN 1" >> pjlib/include/pj/config_site.h \
    && echo "#define PJ_IS_BIG_ENDIAN 0" >> pjlib/include/pj/config_site.h \
    && ./configure CFLAGS="-fPIC -O2" --enable-shared --disable-video --enable-epoll \
    && make dep && make && make install && ldconfig

ENV INCLUDE_PATH="/app/include"

# Copy service sources into /app/sip_outbound (matches start.sh expectations)
WORKDIR /app/sip_outbound
COPY . .

# Build service
RUN make

RUN chmod +x start.sh

# Cleanup
RUN apt-get remove -y build-essential cmake git pkg-config \
    python3 wget ninja-build meson \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* /usr/src/*

EXPOSE 5060/udp 5060/tcp 5061/tcp 8086/tcp

CMD ["./start.sh"]