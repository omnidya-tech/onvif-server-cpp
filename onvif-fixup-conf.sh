#!/bin/sh
# Boot-time fixups for /etc/onvif_simple_server.conf shipped with default values.
# 1) serial_num=001 → wlan0 MAC (onvif-gui dedupes cameras by serial)
# 2) drop Profile_2_H265 — with 3 profiles present, the onvif_simple_server
#    binary returns an empty GetProfilesResponse, breaking stream discovery
#    in any Profile S client. The H265 replay URL (rtsp://:555/replay) is
#    served by a separate daemon, so advertising it via Profile_2 is optional.
CONF=/etc/onvif_simple_server.conf
JSON=/etc/onvif_simple_server.json
[ -f "$CONF" ] || exit 0

MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d :)
if [ -n "$MAC" ] && grep -q "^serial_num=001$" "$CONF"; then
    sed -i "s/^serial_num=001$/serial_num=$MAC/" "$CONF"
fi
if [ -n "$MAC" ] && [ -f "$JSON" ] && grep -q "\"serial_num\"[[:space:]]*:[[:space:]]*\"001\"" "$JSON"; then
    sed -i "s/\"serial_num\"[[:space:]]*:[[:space:]]*\"001\"/\"serial_num\": \"$MAC\"/" "$JSON"
fi

if grep -q "^name=Profile_2" "$CONF"; then
    python3 -c "
import re
with open(\"$CONF\") as f: t=f.read()
t=re.sub(r\"\\n\\nname=Profile_2[^\\n]*(?:\\n(?!name=|\\n)[^\\n]*)*\", \"\", t, flags=re.M)
with open(\"$CONF\",\"w\") as f: f.write(t)
" 2>/dev/null
fi
exit 0
