ARG BUILDER_IMAGE=opensips-builder:latest

FROM ${BUILDER_IMAGE} AS debs

FROM debian:bookworm AS builder
ARG MODULES_LIST=modules.lst.pcscf
RUN export DEBIAN_FRONTEND=noninteractive && \
    apt-get update && apt-get install -y --no-install-recommends binutils && \
    rm -rf /var/lib/apt/lists/*
COPY --from=debs /tmp/deb/ /tmp/debs/
RUN export DEBIAN_FRONTEND=noninteractive && \
    dpkg -i /tmp/debs/*.deb || true && \
    apt-get update && apt-get -f -y install && \
    rm -rf /var/lib/apt/lists/*
COPY make_min_archive.sh /tmp/opensips/make_min_archive.sh
COPY modules/${MODULES_LIST} /tmp/modules.lst
RUN /bin/bash -c "source /tmp/opensips/make_min_archive.sh"
RUN mkdir /tmp/build_image && tar xzf ./opensips_img.tar.gz -C /tmp/build_image

FROM scratch
COPY --from=builder /tmp/build_image /
ENTRYPOINT ["/usr/sbin/opensips", "-F"]
