# Development fixture image. All mutable inputs are resolved in sources.lock.json.
ARG BASE_IMAGE=debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
FROM ${BASE_IMAGE}
ARG DEBIAN_SNAPSHOT
ARG JANUS_COMMIT
ARG JANUS_SHA256
ARG COTURN_COMMIT
ARG COTURN_SHA256
ENV DEBIAN_FRONTEND=noninteractive
RUN rm -f /etc/apt/sources.list.d/debian.sources && \
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s/ trixie main\n' "$DEBIAN_SNAPSHOT" > /etc/apt/sources.list && \
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s/ trixie-security main\n' "$DEBIAN_SNAPSHOT" >> /etc/apt/sources.list && \
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s/ trixie-updates main\n' "$DEBIAN_SNAPSHOT" >> /etc/apt/sources.list && \
    apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends ca-certificates curl build-essential cmake pkg-config autoconf automake libtool gengetopt \
      libglib2.0-dev libconfig-dev libnice-dev libjansson-dev libssl-dev libsrtp2-dev libusrsctp-dev \
      libmicrohttpd-dev libwebsockets-dev libopus-dev libogg-dev libcurl4-openssl-dev libevent-dev libsqlite3-dev python3 && \
    mkdir -p /opt/fixture && dpkg-query -W > /opt/fixture/debian-packages.txt && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN curl --fail --location --retry 3 "https://codeload.github.com/meetecho/janus-gateway/tar.gz/${JANUS_COMMIT}" -o janus.tar.gz && \
    echo "${JANUS_SHA256}  janus.tar.gz" | sha256sum -c - && mkdir janus && tar -xzf janus.tar.gz -C janus --strip-components=1 && \
    cd janus && sh autogen.sh && ./configure --prefix=/opt/janus --disable-docs --disable-post-processing \
      --disable-all-plugins --enable-plugin-echotest --enable-plugin-videoroom --enable-plugin-audiobridge \
      --enable-plugin-videocall --enable-plugin-textroom --enable-plugin-streaming \
      --disable-all-transports --enable-rest --enable-websockets --enable-data-channels \
      --disable-all-handlers --disable-all-loggers --disable-javascript-es-module --disable-javascript-umd-module \
      --disable-javascript-iife-module --disable-javascript-common-js-module && \
    make -j4 && make install && cp COPYING /opt/fixture/JANUS-COPYING && rm -rf /build/janus /build/janus.tar.gz
RUN curl --fail --location --retry 3 "https://codeload.github.com/coturn/coturn/tar.gz/${COTURN_COMMIT}" -o coturn.tar.gz && \
    echo "${COTURN_SHA256}  coturn.tar.gz" | sha256sum -c - && mkdir coturn && tar -xzf coturn.tar.gz -C coturn --strip-components=1 && \
    cmake -S coturn -B coturn/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/coturn -DBUILD_TESTING=OFF -DWITH_MYSQL=OFF && \
    cmake --build coturn/build -j4 && cmake --install coturn/build && \
    chmod 755 /opt/coturn/bin/turnserver && \
    cp coturn/LICENSE /opt/fixture/COTURN-LICENSE && rm -rf /build/coturn /build/coturn.tar.gz
COPY . /opt/fixture/
WORKDIR /opt/fixture
ENTRYPOINT ["python3", "/opt/fixture/entrypoint.py"]
