# onvif-server-cpp

C++ ports of the streaming-critical Python services for the IMX8MP dashcam.

## Status

| Source | Port status | LoC | Notes |
|---|---|---|---|
| `onvif-rtsp-server.py` | ✅ **Ported** → `onvif-rtsp-server.cpp` | 211 → 235 | Lazy-flag mode preserved (DESCRIBE-counted, OPTIONS-immune). Same RTSP mounts, env contract, `/tmp/onvif-rtsp-active` semantics. Drop-in replacement. |
| `onvif-rtsp-watchdog.py` | ✅ **Ported** → `onvif-rtsp-watchdog.cpp` | 113 → 215 | TCP OPTIONS probe, threshold 2, recovery via `systemctl restart onvif-rtsp.service`. |
| `onvif-replay-server.py` | ⛔ **Not ported** | 195 | Currently broken in Python (Profile G replay 503 → 404 regression). Fix Python first, then port. |
| `onvif-imaging-cgi.py` | ⏸️ **Deferred** | 307 | Heavy SOAP/XML + JSON via `isp_ctrl`. Port adds ~700 LoC C++ (libxml2 + jsoncpp). Low ROI — called rarely. |
| `onvif-recording-cgi.py` | ⏸️ **Deferred** | 613 | Largest CGI. Highest risk to break onvif-gui ONVIF surface. Port last if at all. |
| `onvif-analytics-cgi.py` | ⏸️ **Deferred** | 320 | Same reasoning as imaging. |
| `isp_ctrl.c` | ✅ Already C (keep) | 75 | Built unchanged via CMakeLists. |

## Files added/changed/removed

- **Added** `onvif-rtsp-server.cpp` — port of `onvif-rtsp-server.py`
- **Added** `onvif-rtsp-watchdog.cpp` — port of `onvif-rtsp-watchdog.py`
- **Added** `CMakeLists.txt` — builds both C++ binaries + `isp_ctrl`
- **Modified** `onvif-rtsp.service` — `ExecStart` now `/usr/bin/onvif-rtsp-server` (no `python3`)
- **Unchanged** `onvif-rtsp-watchdog.service` — already pointed at `/usr/bin/onvif-rtsp-watchdog`
- **Removed** `onvif-rtsp-server.py`, `onvif-rtsp-watchdog.py` — superseded by the `.cpp` ports.

The earlier `.py` versions are still in git history (`git log --diff-filter=D --summary` will show the deletion commit) if you ever need to inspect them; they're just no longer in the recipe staging tree.

## Compatibility checklist (verified by spec)

| Aspect | Python | C++ port | Match |
|---|---|---|---|
| RTSP mount paths | `/stream`, `/stream_inside` | same | ✅ |
| Listen port | `RTSP_PORT` env, default 554 | same | ✅ |
| Flag file | `/tmp/onvif-rtsp-active` | same | ✅ |
| Flag-set trigger | `pre-describe-request` (NOT `describe-request`) | same | ✅ |
| Flag-clear trigger | client `closed` signal | same | ✅ |
| Re-DESCRIBE no-toggle | per-client-id idempotent counting | per-`GstRTSPClient*` `unordered_set` | ✅ |
| OPTIONS-only immunity | client never DESCRIBE'd → never counted | same logic | ✅ |
| Pipeline | `shmsrc → queue → h264parse → rtph264pay` | same | ✅ |
| `factory.set_shared(True)` | `gst_rtsp_media_factory_set_shared(factory, TRUE)` | ✅ |
| Stale-flag clear at startup | `_remove_flag()` in main | `remove_flag()` in main | ✅ |
| atexit cleanup | `atexit.register(_remove_flag)` | `std::atexit(remove_flag)` | ✅ |
| Signal handler | SIGTERM, SIGINT | `g_unix_signal_add` for both | ✅ |
| Watchdog probe | OPTIONS over TCP, 5 s timeout, threshold 2 | same | ✅ |
| Watchdog state file | `/run/onvif-rtsp-watchdog.fail` | same | ✅ |
| Watchdog recovery | `systemctl restart onvif-rtsp.service` | `fork+execl /bin/systemctl` | ✅ |
| Watchdog logging | `syslog` LOG_DAEMON | same | ✅ |

## Build (Yocto SDK cross-compile)

```bash
source /opt/fsl-imx-xwayland/6.12-walnascar/environment-setup-cortexa53-crypto-poky-linux
cd /home/yokesh/onvif-server-git/onvif-server-cpp
mkdir -p build-cross && cd build-cross
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
file ./onvif-rtsp-server   # ELF aarch64
file ./onvif-rtsp-watchdog
```

## Build (native sanity check)

```bash
sudo apt install libgstreamer1.0-dev libgstrtspserver-1.0-dev libglib2.0-dev
cd /home/yokesh/onvif-server-git/onvif-server-cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Hot-deploy to a single dashcam (skip Yocto, fastest verification)

```bash
DASHCAM=192.168.174.32   # or .25
scp build-cross/onvif-rtsp-server   root@$DASHCAM:/tmp/onvif-rtsp-server.new
scp build-cross/onvif-rtsp-watchdog root@$DASHCAM:/tmp/onvif-rtsp-watchdog.new

ssh root@$DASHCAM '
    set -e
    chmod +x /tmp/onvif-rtsp-server.new /tmp/onvif-rtsp-watchdog.new

    mkdir -p /etc/systemd/system/onvif-rtsp.service.d
    cat > /etc/systemd/system/onvif-rtsp.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/tmp/onvif-rtsp-server.new
EOF

    mkdir -p /etc/systemd/system/onvif-rtsp-watchdog.service.d
    cat > /etc/systemd/system/onvif-rtsp-watchdog.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/tmp/onvif-rtsp-watchdog.new
EOF

    systemctl daemon-reload
    systemctl restart onvif-rtsp.service
    systemctl status onvif-rtsp.service --no-pager | head -10
'
```

End-to-end verification:

```bash
ssh root@$DASHCAM 'journalctl -u onvif-rtsp.service -n 20 --no-pager'
# Expect: "RTSP server listening on port 554 (lazy flag mode)"
#         "[front] mount /stream -> ( shmsrc ... )"
#         "[inside] mount /stream_inside -> ( shmsrc ... )"

ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name,width,height \
    rtsp://admin:admin@$DASHCAM:554/stream
ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name,width,height \
    rtsp://admin:admin@$DASHCAM:554/stream_inside
# Both must return: codec_name=h264, width=1280, height=720
```

When a client DESCRIBEs, journal should log:
```
[FLAG] +client (count=1) → /tmp/onvif-rtsp-active SET
```
…and on disconnect:
```
[FLAG] -client (count=0) → /tmp/onvif-rtsp-active CLEARED
```

## Yocto integration (recipe additions)

```bbappend
SRC_URI += " \
    file://onvif-rtsp-server.cpp \
    file://onvif-rtsp-watchdog.cpp \
    file://CMakeLists.txt \
"

DEPENDS += " \
    glib-2.0 \
    gstreamer1.0 \
    gstreamer1.0-rtsp-server \
"

inherit cmake

# Switch over: stop installing the .py files. Leave python3 in RDEPENDS for
# now since the analytics/imaging/recording CGIs still need it.
do_install:append() {
    rm -f ${D}${bindir}/onvif-rtsp-server.py
    rm -f ${D}${bindir}/onvif-rtsp-watchdog
}

# Bump PR so sstate-cache won't reuse the old build.
PR = "r<bump>"
```

## Rollback

**Per-device rollback (during hot-deploy testing only):** removing the
override drop-ins falls back to whatever the recipe-installed binary is.
Once the C++ binaries are in the squashfs (after a Yocto rebuild + RAUC
install), the override removal still leaves you on the C++ binary —
because that's now what the recipe installs.

```bash
ssh root@$DASHCAM '
    rm -f /etc/systemd/system/onvif-rtsp.service.d/override.conf
    rm -f /etc/systemd/system/onvif-rtsp-watchdog.service.d/override.conf
    systemctl daemon-reload
    systemctl restart onvif-rtsp.service
'
```

**Full rollback to Python:** the `.py` files are no longer in this
directory, so this isn't possible from a single rebuild. Either:

1. Boot the previous RAUC slot (`rauc status` → `rauc mark-active other` →
   `reboot`) — the inactive slot still contains the prior image with the
   Python-based services, and is the cleanest recovery if anything goes
   wrong post-deploy.
2. Or restore the `.py` files from git history (`git checkout HEAD~1 --
   onvif-rtsp-server.py onvif-rtsp-watchdog.py`) and revert the recipe
   changes.

## Known caveats

1. **`onvif-replay-server.py` left in Python.** It was already broken; porting forward without fixing the upstream defect would just obscure the bug.
2. **CGIs left in Python.** Porting them correctly requires libxml2/jsoncpp wiring, careful SOAP-envelope byte-equivalence, and lighttpd CGI testing. Don't undertake without a test harness for the ONVIF surface.
3. **No on-device hardware test yet.** Equivalence verified by reading the Python and the gst-rtsp-server C API docs side-by-side. The hot-deploy steps above are the next verification step.
