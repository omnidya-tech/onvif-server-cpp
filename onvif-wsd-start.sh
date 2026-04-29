#!/bin/sh
# Resolve wlan0 IP at runtime and start WS-Discovery server.
# Called by onvif.service — avoids systemd escaping headaches.

IFACE="wlan0"

# Wait for the interface to get an IP
echo "Waiting for ${IFACE} IP..."
while true; do
    WLAN_IP=$(ip -4 addr show "$IFACE" 2>/dev/null | awk '/inet / {split($2, a, "/"); print a[1]}')
    [ -n "$WLAN_IP" ] && break
    sleep 1
done

echo "Got ${IFACE} IP: ${WLAN_IP}"

# Multicast route for WS-Discovery
ip route add 239.255.255.250/32 dev "$IFACE" 2>/dev/null || true

# Start WSD server (foreground, exec replaces this shell)
exec /usr/bin/wsd_simple_server \
    -i "$IFACE" \
    -x "http://${WLAN_IP}/onvif/device_service" \
    -m IMX8MP-Dashcam \
    -n Omnidya \
    -p /run/wsd_simple_server.pid \
    -f
