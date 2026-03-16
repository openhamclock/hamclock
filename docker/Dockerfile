## PLEASE DON'T CHANGE ANYTHING IN THIS FILE UNLESS YOU'RE developing.
## REFER TO THE DOCUMENTATION FOR CONFIGURATION OF YOUR HAMCLOCK!
FROM alpine:3.23
ARG HC_SIZE=2400x1440

USER root
WORKDIR /root/hamclock
# Install prerequisites
RUN apk update && \
    apk add curl make g++ libx11-dev openssl unzip perl

# Install Hamclock
COPY ESPHamClock/ ESPHamClock/
RUN cd ESPHamClock && \
    ls && \
    make -j 4 hamclock-web-${HC_SIZE} && \
    make install

COPY hamclock-contrib/ hamclock-contrib/
# Install Hamclock Contrib and move hceeprom to hamclock directory
RUN cp hamclock-contrib/hceeprom.pl /root/hamclock/ESPHamClock && \
    chmod +x /root/hamclock/ESPHamClock/hceeprom.pl

# Copy runfile
COPY docker/run.sh ./
COPY docker/backend_host ./
RUN chmod +x run.sh

WORKDIR /root/hamclock/ESPHamClock
CMD ["/root/hamclock/run.sh"]
