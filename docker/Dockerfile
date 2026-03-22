## PLEASE DON'T CHANGE ANYTHING IN THIS FILE UNLESS YOU'RE developing.
## REFER TO THE DOCUMENTATION FOR CONFIGURATION OF YOUR HAMCLOCK!
FROM alpine:3.23
ARG HC_SIZE
ARG HC_UID
ARG HC_GID

USER root
WORKDIR /opt/hamclock
# Install prerequisites
RUN apk update && \
    apk add curl make g++ libx11-dev openssl unzip perl && \
    rm -rf /var/cache/apk/*

# Install Hamclock
COPY ESPHamClock/ ESPHamClock/
COPY hamclock-contrib/ hamclock-contrib/
COPY docker/run.sh ./
COPY docker/backend_host ./
RUN addgroup -g $HC_GID hamclock && \
    adduser  -u $HC_UID -G hamclock -h /opt/hamclock -HD -s /sbin/nologin hamclock && \
    \
    cp hamclock-contrib/hceeprom.pl ESPHamClock/ && \
    chmod +x ESPHamClock/hceeprom.pl && \
    chmod +x run.sh && \
    \
    mkdir /opt/hamclock/.hamclock && \
    chown -R hamclock:hamclock /opt/hamclock && \
    cd ESPHamClock && \
    \
    if [ -n "$HC_SIZE" ]; then \
        make -j 4 hamclock-web-${HC_SIZE} && \
        make install && \
        make clean; \
    else \
		for size in 800x480 1600x960 2400x1440 3200x1920; do \
            make -j 4 hamclock-web-$size && \
            cp hamclock-web-$size /usr/local/bin/ && \
            make clean; \
        done; \
        chown hamclock /usr/local/bin/hamclock-web-*; \
    fi

USER hamclock

WORKDIR /opt/hamclock/ESPHamClock
CMD ["/opt/hamclock/run.sh"]
