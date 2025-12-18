FROM debian:bookworm AS builder
RUN export DEBIAN_FRONTEND=noninteractive
#RUN echo 'deb http://deb.debian.org/debian bullseye-backports main' > /etc/apt/sources.list.d/backports.list
RUN apt-get update && apt-get install -y dpkg-dev dnsutils wget software-properties-common net-tools curl lsb-release debhelper sofia-sip-bin flex bison devscripts default-libmysqlclient-dev docbook-xml erlang-dev libconfuse-dev libdb-dev libev-dev libevent-dev libexpat1-dev libgeoip-dev libhiredis-dev libjansson-dev libjson-c-dev libldap2-dev liblua5.1-0-dev libmemcached-dev libmono-2.0-dev libncurses5-dev libpcre3-dev libperl-dev libpq-dev librabbitmq-dev libradcli-dev libreadline-dev libsasl2-dev libsctp-dev libsnmp-dev libsqlite3-dev libsystemd-dev libunistring-dev libxml2-dev pkg-config python3 python3-dev unixodbc-dev uuid-dev xsltproc zlib1g-dev libbson-dev libmaxminddb-dev libmnl-dev libmongoc-dev libphonenumber-dev python-is-python3 python3-dev python-dev-is-python3 ruby-dev libwolfssl-dev libssl-dev musl-dev musl-tools libcurl4-gnutls-dev libmicrohttpd-dev librdkafka-dev git-core libfreediameter-dev libjwt-dev libcurl4-openssl-dev libpcre2-dev
COPY . /tmp/build/opensips/
COPY build-deb.sh /usr/sbin/build-deb.sh
RUN /usr/sbin/build-deb.sh

FROM debian:bookworm
COPY --from=builder /tmp/deb/ /tmp/debs/
RUN export DEBIAN_FRONTEND=noninteractive && \
apt-get update && apt-get install -y wget gnupg2 ca-certificates && \
dpkg -i /tmp/debs/*.deb || true && \
apt-key adv --fetch-keys https://apt.opensips.org/pubkey.gpg && \
echo "deb https://apt.opensips.org bookworm cli-nightly" >/etc/apt/sources.list.d/opensips-cli.list && \
apt-get update && apt-get -f -y install && \
apt-get -y install gnupg2 ca-certificates iproute2 mariadb-client gettext-base gdb opensips-cli sngrep sofia-sip-bin ngrep tcpdump && \
apt-get autoremove --purge -y && \
apt-get clean && \
rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["/usr/sbin/opensips", "-F"]
