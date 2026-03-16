#!/bin/sh

if [ -z "$BACKEND_HOST" -a -e /root/hamclock/backend_host ]; then
    BACKEND_HOST="$(grep -v '^#' /root/hamclock/backend_host)"
fi
if [ -n "$BACKEND_HOST" ]; then
    BACKEND_ARG="-b $BACKEND_HOST"
fi

perl hceeprom.pl NV_CALLSIGN $CALLSIGN && \
perl hceeprom.pl NV_DE_GRID $LOCATOR && \
perl hceeprom.pl NV_DE_LAT $LAT && \
perl hceeprom.pl NV_DE_LNG $LONG && \
perl hceeprom.pl NV_BCMODE $VOACAP_MODE && \
perl hceeprom.pl NV_BCPOWER $VOACAP_POWER && \
perl hceeprom.pl NV_CALL_BG_COLOR $CALLSIGN_BACKGROUND_COLOR && \
perl hceeprom.pl NV_CALL_BG_RAINBOW $CALLSIGN_BACKGROUND_RAINBOW && \
perl hceeprom.pl NV_CALL_FG_COLOR $CALLSIGN_COLOR && \
perl hceeprom.pl NV_FLRIGHOST $FLRIG_HOST && \
perl hceeprom.pl NV_FLRIGPORT $FLRIG_PORT && \
perl hceeprom.pl NV_FLRIGUSE $USE_FLRIG && \
perl hceeprom.pl NV_METRIC_ON $USE_METRIC && \

# this extra work causes the container to stop quickly. We need to 
# kill our own jobs or bash will zombie and then docker takes 10 seconds
# before it sends kill -9. The wait will respond to a TERM whereas 
# tail does not so we need to background tail.
cleanup() {
    echo "Caught SIGTERM, shutting down services..."
    kill $(jobs -p)
    exit 0
}

# Trap the TERM signal
trap cleanup SIGTERM

/usr/local/bin/hamclock -t 10 $BACKEND_ARG &
wait $!
