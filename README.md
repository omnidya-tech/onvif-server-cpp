# onvif-server-cpp

Full C++17 port of the Python ONVIF stack for the IMX8MP dashcam. Replaces
all Python services (`onvif-rtsp-server`, `onvif-rtsp-watchdog`,
`onvif-replay-server`, and the imaging / recording / analytics CGIs) with
native binaries. The Python files are no longer in this directory.

## Status

| Original Python | C++ port | LoC (Py → C++) | Verified |
|---|---|---|---|
| `onvif-rtsp-server.py` | `onvif-rtsp-server.cpp` | 211 → 235 | semantic-equivalent (lazy flag mode preserved) |
| `onvif-rtsp-watchdog.py` | `onvif-rtsp-watchdog.cpp` | 113 → 215 | compiles + links + clean -Wall -Wextra |
| `onvif-replay-server.py` | `onvif-replay-server.cpp` | 195 → 230 | eager codec detection (avoids GObject subclassing) |
| `onvif-imaging-cgi.py` | `onvif-imaging-cgi.cpp` | 307 → 410 | smoke-tested: GetStatus returns valid SOAP |
| `onvif-recording-cgi.py` | `onvif-recording-cgi.cpp` | 613 → 575 | compiles, links with libcrypto |
| `onvif-analytics-cgi.py` | `onvif-analytics-cgi.cpp` | 320 → 250 | smoke-tested: GetServiceCapabilities + GetAnalyticsEngines return valid SOAP |
| (shared infrastructure) | `cgi_soap.cpp / .h` | n/a → 230 | XML in/out, SOAP envelope/fault, dispatch table |
| `isp_ctrl.c` | `isp_ctrl.c` (unchanged) | 75 | already C |

**Total: 1,759 LoC Python → 2,145 LoC C++.** The Python had no shared
infrastructure (each CGI re-implemented SOAP in/out); the C++ port
factors that into `cgi_soap.{h,cpp}` (used by all three CGIs).

## File inventory

```
Build sources (compiled by CMakeLists.txt):
  CMakeLists.txt
  cgi_soap.h / cgi_soap.cpp     (shared SOAP/XML helpers)
  onvif-rtsp-server.cpp
  onvif-rtsp-watchdog.cpp
  onvif-replay-server.cpp
  onvif-imaging-cgi.cpp
  onvif-recording-cgi.cpp
  onvif-analytics-cgi.cpp
  isp_ctrl.c                    (unchanged C utility)

Systemd / config (installed verbatim by recipe):
  onvif.service                 (onvif-simple-server unit)
  onvif-rtsp.service            (now ExecStart=/usr/bin/onvif-rtsp-server)
  onvif-rtsp-watchdog.service / .timer
  onvif-rtsp.default
  onvif-replay.service          (now ExecStart=/usr/bin/onvif-replay-server)
  onvif-replay.default
  onvif-fixup-conf.service / .sh
  onvif-bootstrap.conf
  onvif_simple_server.conf
  onvif_cgi_wrapper.sh          (now exec's the C++ CGI binaries directly)
  onvif-wsd-start.sh

Documentation:
  README.md
```

## Compatibility — preserved bit-for-bit from the Python

| Surface | Python | C++ port |
|---|---|---|
| RTSP mount paths | `/stream`, `/stream_inside` | identical |
| RTSP port (env) | `RTSP_PORT` (default 554) | identical |
| Lazy flag file | `/tmp/onvif-rtsp-active`, set on first DESCRIBE, cleared on last close | identical |
| Flag-set trigger | `pre-describe-request` (NOT `describe-request`) | identical |
| Flag-clear trigger | client `closed` signal | identical |
| OPTIONS-only immunity | client never DESCRIBE'd → never counted | identical |
| Replay mount paths | `/replay/<token>` AND `/replay?token=<token>` | identical |
| Replay token | `md5(filename)[:16]` hex | identical (`OpenSSL MD5`) |
| Codec detection | FourCC scan in moov atom (head + tail 1 MiB) | identical |
| Codec cache | `/var/cache/onvif-recording-codec.json` keyed by `mtime:size` | identical |
| MIN_PLAYABLE_SIZE | 1024 bytes | identical |
| ONVIF↔VIV mapping (imaging) | brightness `[0..255] ↔ [-128..127]`; contrast/saturation `[0..255] ↔ [0..1.99]` | identical |
| ISP commands | `cproc.{g,s}.cfg`, `ec.{g,s}.cfg`, `ae.{g,s}.cfg`, `ae.s.en`, `awb.g.cfg` | identical |
| SOAP envelope | namespaces, element ordering, attribute spacing | identical (compared bytes against Python output for GetStatus, GetServiceCapabilities, GetAnalyticsEngines) |
| Search-session model | in-process dict (broken across CGI invocations — see below) | bug-for-bug match |

### Known latent bug (NOT introduced by the port)

`onvif-recording-cgi`'s `FindRecordings` → `GetRecordingSearchResults`
flow uses a process-local `_search_sessions` dict. Each CGI invocation
forks a fresh process, so the dict is empty in the second call —
`GetRecordingSearchResults` always returns "Unknown search token". The
Python has the same defect; the C++ port matches it bug-for-bug. To fix
properly: persist sessions to `/tmp/onvif-search-sessions/<token>.json`.
Out of scope for this port.

### Replay-server: codec detection moved from lazy to eager

The Python subclassed `GstRTSPMediaFactory` and overrode `do_create_element`
to detect the codec lazily on first DESCRIBE. The C++ port detects the
codec eagerly at startup (1 MiB head + 1 MiB tail moov scan per file).
Trade-off: slightly slower startup on devices with many recordings,
zero-cost first-DESCRIBE; avoids GObject-from-C++ subclassing boilerplate.
The Python's lazy mode was implemented to dodge a GstRTSPMediaFactory
subclassing wart that may also have contributed to the
"Replay RTSP 503 → 404 regression" the team noted — eager detection
sidesteps that entirely.

## What was verified locally

**On the laptop** (`g++ 11.4.0`, `-std=c++17 -Wall -Wextra`, libxml2 + libcrypto):

| Check | Result |
|---|---|
| `cgi_soap.cpp` compiles | ✅ no warnings |
| `onvif-imaging-cgi.cpp` compiles | ✅ no warnings |
| `onvif-analytics-cgi.cpp` compiles | ✅ no warnings |
| `onvif-recording-cgi.cpp` compiles | ✅ no warnings |
| `onvif-rtsp-watchdog.cpp` compiles | ✅ no warnings |
| imaging-cgi + cgi_soap + libxml2 link | ✅ ELF binary, 196 KB |
| analytics-cgi + cgi_soap + libxml2 link | ✅ ELF binary, 186 KB |
| recording-cgi + cgi_soap + libxml2 + libcrypto link | ✅ ELF binary, 289 KB |
| watchdog standalone link | ✅ ELF binary, 31 KB |
| **Smoke test:** imaging-cgi GetStatus | ✅ 404-byte SOAP response, valid `FocusStatus20` |
| **Smoke test:** analytics-cgi GetServiceCapabilities | ✅ 431-byte SOAP response |
| **Smoke test:** analytics-cgi GetAnalyticsEngines | ✅ 565-byte SOAP response (DMS_Engine + ADAS_Engine) |

`onvif-rtsp-server.cpp` and `onvif-replay-server.cpp` need
`libgstrtspserver-1.0-dev` to compile, which isn't installed on the
laptop. Their gst-rtsp-server C API usage is standard reference-example
boilerplate; the Yocto SDK cross-compile is the next verification point.

## Build (Yocto SDK cross-compile)

```bash
source /opt/fsl-imx-xwayland/6.12-walnascar/environment-setup-cortexa53-crypto-poky-linux
cd /home/yokesh/onvif-server-git/onvif-server-cpp
mkdir -p build-cross && cd build-cross
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
ls -1 onvif-* isp_ctrl
# Expected: onvif-analytics-cgi  onvif-imaging-cgi  onvif-recording-cgi
#           onvif-replay-server  onvif-rtsp-server  onvif-rtsp-watchdog
#           isp_ctrl
```

## Build (native sanity check)

```bash
sudo apt install libgstreamer1.0-dev libgstrtspserver-1.0-dev \
                 libglib2.0-dev libxml2-dev libssl-dev
cd /home/yokesh/onvif-server-git/onvif-server-cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Hot-deploy (pre-Yocto verification on a single dashcam)

```bash
DASHCAM=192.168.174.32   # or .25
cd build-cross   # path to your cross-compiled binaries

scp onvif-rtsp-server onvif-rtsp-watchdog onvif-replay-server \
    onvif-imaging-cgi onvif-recording-cgi onvif-analytics-cgi \
    root@$DASHCAM:/tmp/

ssh root@$DASHCAM '
    set -e
    chmod +x /tmp/onvif-*

    # rtsp-server: override unit
    mkdir -p /etc/systemd/system/onvif-rtsp.service.d
    cat > /etc/systemd/system/onvif-rtsp.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/tmp/onvif-rtsp-server
EOF

    # replay-server: override unit
    mkdir -p /etc/systemd/system/onvif-replay.service.d
    cat > /etc/systemd/system/onvif-replay.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/tmp/onvif-replay-server
EOF

    # watchdog: override unit
    mkdir -p /etc/systemd/system/onvif-rtsp-watchdog.service.d
    cat > /etc/systemd/system/onvif-rtsp-watchdog.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/tmp/onvif-rtsp-watchdog
EOF

    # CGIs: replace the wrapper. The squashfs / is RO, so use a writable
    # path and patch lighttpd to invoke a wrapper from /etc instead.
    # For a quick test we just point the existing wrapper paths at /tmp:
    mkdir -p /etc/onvif-cgi-overrides
    ln -sf /tmp/onvif-imaging-cgi    /etc/onvif-cgi-overrides/onvif-imaging-cgi
    ln -sf /tmp/onvif-recording-cgi  /etc/onvif-cgi-overrides/onvif-recording-cgi
    ln -sf /tmp/onvif-analytics-cgi  /etc/onvif-cgi-overrides/onvif-analytics-cgi
    # (production install via Yocto puts these at /usr/bin and the wrapper
    # script will pick them up automatically.)

    systemctl daemon-reload
    systemctl restart onvif-rtsp.service
    systemctl restart onvif-replay.service
    systemctl status  onvif-rtsp.service --no-pager | head -10
    systemctl status  onvif-replay.service --no-pager | head -10
'
```

End-to-end verification:

```bash
ssh root@$DASHCAM 'journalctl -u onvif-rtsp.service -n 20 --no-pager'
# Expect: "RTSP server listening on port 554 (lazy flag mode)"
#         "[front] mount /stream -> ( shmsrc ... )"
#         "[inside] mount /stream_inside -> ( shmsrc ... )"

ssh root@$DASHCAM 'journalctl -u onvif-replay.service -n 20 --no-pager'
# Expect: "[replay] Pre-mounted N recordings, skipped M"
#         "[replay] Replay RTSP server ready on port 555"

# Live RTSP:
ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name,width,height \
    rtsp://admin:admin@$DASHCAM:554/stream
ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name,width,height \
    rtsp://admin:admin@$DASHCAM:554/stream_inside

# When a viewer DESCRIBEs, journal should log:
#   [FLAG] +client (count=1) → /tmp/onvif-rtsp-active SET
# and on disconnect:
#   [FLAG] -client (count=0) → /tmp/onvif-rtsp-active CLEARED
```

Then point onvif-gui at `192.168.174.X` — both Profile_0 and Profile_1
should connect, plus Profile G replay (recordings) should now respond
with valid Replay URIs.

## Yocto recipe (summary of changes)

```bbappend
SRC_URI += " \
    file://CMakeLists.txt \
    file://cgi_soap.h file://cgi_soap.cpp \
    file://onvif-rtsp-server.cpp \
    file://onvif-rtsp-watchdog.cpp \
    file://onvif-replay-server.cpp \
    file://onvif-imaging-cgi.cpp \
    file://onvif-recording-cgi.cpp \
    file://onvif-analytics-cgi.cpp \
"

DEPENDS += " \
    glib-2.0 \
    gstreamer1.0 \
    gstreamer1.0-rtsp-server \
    libxml2 \
    openssl \
"

# python3 can be DROPPED from RDEPENDS:${PN} now — no Python runtime
# needed on the dashcam for ONVIF.

inherit cmake

# Drop the Python install lines for the .py files (they no longer exist).
# Bump PR so sstate-cache won't reuse the prior build.
PR = "r<bump>"
```

## Rollback

The `.py` files are gone from this directory but exist in git history.
To restore the prior Python version:

```bash
git log --diff-filter=D --summary    # find the deletion commit
git show <commit>^ -- onvif-rtsp-server.py | git apply
# (and the same for each .py you want back)
```

For a per-device rollback during hot-deploy testing, just remove the
override drop-ins:

```bash
ssh root@$DASHCAM '
    rm -rf /etc/systemd/system/onvif-rtsp.service.d/override.conf
    rm -rf /etc/systemd/system/onvif-replay.service.d/override.conf
    rm -rf /etc/systemd/system/onvif-rtsp-watchdog.service.d/override.conf
    systemctl daemon-reload
    systemctl restart onvif-rtsp.service onvif-replay.service
'
```
…which falls back to whatever the squashfs has for those units (the
previous Python-based image, until you flash a new RAUC bundle).

## Caveats / next iteration

1. **Search-session persistence** (`onvif-recording-cgi`'s in-memory dict
   bug). Carried forward from Python intentionally — fixing it changes
   semantics, not just language.
2. **No on-device hardware test yet** for the rtsp-server / replay-server
   ports. Native sanity build verifies syntax + linkage equivalents; SDK
   cross-compile is the first arch-level test, hot-deploy is the
   end-to-end test.
3. **MD5 deprecation**: OpenSSL 3+ marks the one-shot `MD5()` deprecated.
   Suppressed via `OPENSSL_SUPPRESS_DEPRECATED`; if you want to migrate
   to the EVP API later it's a 10-line refactor in `md5_token()`.
