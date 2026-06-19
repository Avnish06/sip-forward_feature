# Use the latest Debian image
FROM debian:latest

# Set environment variables
ENV ASTERISK_VERSION=20.19.0
# Install required dependencies
# policy-rc.d prevents apt from trying to start the mariadb service during build
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && chmod +x /usr/sbin/policy-rc.d && \
    apt-get update && apt-get install -y \
    build-essential \
    wget \
    curl \
    libcurl4-openssl-dev \
    nano \
    pkg-config \
    autoconf \
    automake \
    libtool \
    libssl-dev \
    libncurses5-dev \
    libnewt-dev \
    libxml2-dev \
    libxslt1-dev \
    uuid-dev \
    libsqlite3-dev \
    libedit-dev \
    libjansson-dev \
    default-libmysqlclient-dev \
    default-mysql-client \
    libsrtp2-dev \
    libspandsp-dev \
    iputils-ping \
    net-tools \
    procps \
    mariadb-client \
    mariadb-server \
    xsltproc \
    doxygen \
    && rm -rf /var/lib/apt/lists/*

# Create asterisk user and group
RUN groupadd -r asterisk && useradd -r -g asterisk asterisk

# Download and extract Asterisk source code
RUN wget https://downloads.asterisk.org/pub/telephony/asterisk/asterisk-${ASTERISK_VERSION}.tar.gz && \
    tar -xzf asterisk-${ASTERISK_VERSION}.tar.gz && \
    cd asterisk-${ASTERISK_VERSION} && \
    ./configure --with-mysql --with-jansson --with-curl && \
    make menuselect.makeopts && \
    menuselect/menuselect \
    --enable CORE-SOUNDS-EN-GSM \
    --enable res_config_mysql \
    --enable chan_pjsip \
    --enable chan_sip \
    --enable res_pjsip \
    --enable res_pjsip_session \
    --enable res_pjsip_pubsub \
    --enable res_pjsip_transport_websocket \
    --enable res_pjsip_outbound_registration \
    --enable res_pjsip_endpoint_identifier_user \
    --enable res_pjsip_endpoint_identifier_ip \
    --enable res_pjsip_registrar \
    --enable res_pjsip_authenticator_digest \
    --enable res_pjsip_outbound_authenticator_digest \
    --enable res_pjsip_nat \
    --enable res_pjsip_sdp_rtp \
    --enable func_curl \
    menuselect.makeopts && \
    make && make install && make samples && make config

# Create necessary directories and set permissions
RUN mkdir -p /var/lib/asterisk && \
    mkdir -p /var/log/asterisk && \
    mkdir -p /var/run/asterisk && \
    mkdir -p /var/spool/asterisk && \
    mkdir -p /usr/local/scripts && \
    chown -R asterisk:asterisk /var/lib/asterisk && \
    chown -R asterisk:asterisk /var/log/asterisk && \
    chown -R asterisk:asterisk /var/run/asterisk && \
    chown -R asterisk:asterisk /var/spool/asterisk && \
    chown -R asterisk:asterisk /etc/asterisk

# Copy custom Asterisk configuration files
COPY config/ /etc/asterisk/
COPY scripts/ /usr/local/scripts/

# Create symbolic link for MySQL configuration; normalize + make scripts executable
RUN ln -sf /etc/asterisk/res_config_mysql.conf /etc/asterisk/res_mysql.conf && \
    sed -i 's/\r$//' /usr/local/scripts/*.sh && \
    chmod +x /usr/local/scripts/*.sh && \
    chown -R asterisk:asterisk /etc/asterisk /usr/local/scripts

# Expose SIP and RTP ports
# PJSIP on 5060, traditional SIP on 5061
EXPOSE 5060/udp 5060/tcp 5061/udp 5061/tcp 10000-10100/udp

# Set the working directory
WORKDIR /etc/asterisk

# Switch to asterisk user for security
USER asterisk

# Start Asterisk in the foreground
CMD ["asterisk", "-f"]

