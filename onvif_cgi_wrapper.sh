#!/bin/sh
# CGI wrapper for ONVIF services.
# Dynamically detects which interface the request came from (usb0 or wlan0)
# and patches the onvif config so service URLs have the correct IP.
# Routes core services to onvif_simple_server and Profile G/T/M to Python CGIs.

SERVICE=$(basename "$SCRIPT_NAME")
CONF="/etc/onvif_simple_server.conf"

# Detect the interface matching the incoming request IP (SERVER_ADDR from lighttpd)
if [ -n "$SERVER_ADDR" ]; then
    MATCHED_IF=""
    for IF in usb0 wlan0 eth0; do
        IF_IP=$(ip -4 addr show "$IF" 2>/dev/null | awk '/inet / {split($2,a,"/"); print a[1]}')
        if [ "$IF_IP" = "$SERVER_ADDR" ]; then
            MATCHED_IF="$IF"
            break
        fi
    done

    # If we matched an interface and it differs from config, patch it
    if [ -n "$MATCHED_IF" ]; then
        CURRENT_IF=$(awk -F= '/^ifs=/{print $2}' "$CONF")
        if [ "$CURRENT_IF" != "$MATCHED_IF" ]; then
            sed -i "s/^ifs=.*/ifs=$MATCHED_IF/" "$CONF"
        fi
    fi
fi

case "$SERVICE" in
    imaging_service)
        exec /usr/bin/onvif-imaging-cgi
        ;;
    recording_service|search_service)
        exec /usr/bin/onvif-recording-cgi "$SERVICE"
        ;;
    replay_service)
        exec /usr/bin/onvif-recording-cgi replay_service
        ;;
    analytics_service)
        exec /usr/bin/onvif-analytics-cgi
        ;;
    *)
        cd /usr/share/onvif-simple-server
        exec /usr/bin/onvif_simple_server "$SERVICE"
        ;;
esac
