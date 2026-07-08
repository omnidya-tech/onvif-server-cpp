DESCRIPTION = "ONVIF server with Profile S/G/T/M support for IMX8MP dashcam (C++)"
LICENSE = "CLOSED"

SRC_URI = " \
    git://github.com/roleoroleo/onvif_simple_server.git;protocol=https;branch=master;name=upstream;destsuffix=git \
    git://git@github.com/omnidya-tech/onvif-server-cpp.git;protocol=ssh;branch=main;name=omnidya;destsuffix=onvif-server-cpp \
"

SRCREV_upstream = "${AUTOREV}"
SRCREV_omnidya  = "387d133abf2a2b23969b314895f48e28efe45ea5"
SRCREV_FORMAT   = "upstream_omnidya"

S        = "${WORKDIR}/git"
ONVIF_S  = "${UNPACKDIR}/onvif-server-cpp"

# Build deps:
#   json-c, libtomcrypt, zlib  — upstream onvif_simple_server's Makefile
#   glib-2.0, gstreamer1.0,
#   gstreamer1.0-rtsp-server   — onvif-rtsp-server.cpp, onvif-replay-server.cpp
#   libxml2                    — cgi_soap.{h,cpp} (XML in/out for the CGIs)
#   openssl                    — MD5 for recording/replay tokens
DEPENDS = "json-c libtomcrypt zlib glib-2.0 gstreamer1.0 gstreamer1.0-rtsp-server libxml2 openssl"

INSANE_SKIP:${PN} = "already-stripped"

inherit cmake pkgconfig systemd

# Our CMakeLists.txt lives in the omnidya repo (NOT in ${S}, which is reserved
# for the upstream onvif_simple_server git checkout).
OECMAKE_SOURCEPATH = "${ONVIF_S}"

# The default DEBUG_PREFIX_MAP only remaps ${S} and ${B}. Since our cmake
# sources live in ${ONVIF_S}, debug info would otherwise embed absolute
# TMPDIR paths and trigger the buildpaths QA check.
DEBUG_PREFIX_MAP:append = " \
    -fmacro-prefix-map=${ONVIF_S}=/usr/src/debug/${PN}/${EXTENDPE}${PV}-${PR} \
    -fdebug-prefix-map=${ONVIF_S}=/usr/src/debug/${PN}/${EXTENDPE}${PV}-${PR} \
"

# Upstream Makefile flags — unchanged from the original recipe. Applies to
# the oe_runmake of the upstream onvif_simple_server tree in ${S}.
# The upstream Makefile only honours $(CC) and $(INCLUDE), so we embed
# LDFLAGS into CC.
EXTRA_OEMAKE = " \
    CC='${CC} ${LDFLAGS}' \
    INCLUDE='-ffunction-sections -fdata-sections -Wno-error=format-overflow -Wno-error=format-truncation -Wno-error=format-security' \
"

# cmake's default do_compile builds our C++ binaries (cgi_soap library +
# six executables + isp_ctrl) per CMakeLists.txt. Append the upstream
# Makefile build for onvif_simple_server / onvif_notify_server /
# wsd_simple_server.
do_compile:append() {
    oe_runmake -C ${S}
}

ONVIF_DATADIR = "${datadir}/onvif-simple-server"

# Override do_install entirely — we install from two source trees
# (upstream Makefile artifacts in ${S}; cmake artifacts in ${B}) plus all
# the systemd/config/web-stub files from the omnidya repo (${ONVIF_S}).
do_install() {
    install -d ${D}${bindir}

    # ── Upstream onvif_simple_server binaries (from ${S} via Makefile) ──
    install -m 0755 ${S}/onvif_simple_server  ${D}${bindir}/
    install -m 0755 ${S}/onvif_notify_server  ${D}${bindir}/
    install -m 0755 ${S}/wsd_simple_server    ${D}${bindir}/

    # ── C++ binaries (from ${B} via cmake) ──
    install -m 0755 ${B}/onvif-rtsp-server     ${D}${bindir}/
    install -m 0755 ${B}/onvif-rtsp-watchdog   ${D}${bindir}/
    install -m 0755 ${B}/onvif-replay-server   ${D}${bindir}/
    install -m 0755 ${B}/onvif-imaging-cgi     ${D}${bindir}/
    install -m 0755 ${B}/onvif-recording-cgi   ${D}${bindir}/
    install -m 0755 ${B}/onvif-analytics-cgi   ${D}${bindir}/
    install -m 0755 ${B}/isp_ctrl              ${D}${bindir}/

    # ── CGI wrapper (dispatches to onvif_simple_server or the C++ CGIs) ──
    install -m 0755 ${ONVIF_S}/onvif_cgi_wrapper.sh ${D}${bindir}/

    # The lighttpd CGI rule in /etc/lighttpd.d/onvif-cgi.conf hard-codes
    # /etc/onvif_cgi_wrapper.sh as the cgi.assign target. Install a symlink
    # to the real binary so lighttpd can exec it. Without this, every CGI
    # call returns 500 Internal Server Error.
    install -d ${D}${sysconfdir}
    ln -sf ${bindir}/onvif_cgi_wrapper.sh ${D}${sysconfdir}/onvif_cgi_wrapper.sh

    # ── WSD startup wrapper (resolves wlan0 IP dynamically) ──
    install -m 0755 ${ONVIF_S}/onvif-wsd-start.sh ${D}${bindir}/

    # ── XML template files for onvif_simple_server (CGI) ──
    install -d ${D}${ONVIF_DATADIR}
    for d in device_service_files media_service_files media2_service_files \
             ptz_service_files events_service_files deviceio_service_files \
             generic_files notify_files; do
        if [ -d ${S}/$d ]; then
            install -d ${D}${ONVIF_DATADIR}/$d
            install -m 0644 ${S}/$d/* ${D}${ONVIF_DATADIR}/$d/
        fi
    done

    # ── Customised device-service templates (override upstream) ──
    if [ -d ${ONVIF_S}/device_service_files_overrides ]; then
        install -m 0644 ${ONVIF_S}/device_service_files_overrides/*.xml \
            ${D}${ONVIF_DATADIR}/device_service_files/
    fi

    # ── Customised media-service templates that override the upstream
    #    ones. The upstream `_both.xml` and `GetVideoEncoderConfiguration.xml`
    #    hard-code FrameRateLimit=30, BitrateLimit=5000, GovLength=40,
    #    Quality=100, H264Profile=High — values that don't match what
    #    dashcam-service's gstreamer pipeline actually produces (10 fps,
    #    1200 kbps, GOP=10, Main profile). Overriding them here makes the
    #    Media tab in onvif-gui report the real numbers. ──
    if [ -d ${ONVIF_S}/media_service_files_overrides ]; then
        install -m 0644 ${ONVIF_S}/media_service_files_overrides/*.xml \
            ${D}${ONVIF_DATADIR}/media_service_files/
    fi

    # ── WSD template files for wsd_simple_server ──
    install -d ${D}${sysconfdir}/wsd_simple_server
    if [ -d ${S}/wsd_files ]; then
        install -m 0644 ${S}/wsd_files/* ${D}${sysconfdir}/wsd_simple_server/
    fi

    # ── ONVIF config at the default path the binary expects ──
    install -d ${D}${sysconfdir}
    install -m 0644 ${ONVIF_S}/onvif_simple_server.conf \
        ${D}${sysconfdir}/onvif_simple_server.conf

    # ── RTSP / Replay environment overrides ──
    install -d ${D}${sysconfdir}/default
    install -m 0644 ${ONVIF_S}/onvif-rtsp.default \
        ${D}${sysconfdir}/default/onvif-rtsp
    install -m 0644 ${ONVIF_S}/onvif-replay.default \
        ${D}${sysconfdir}/default/onvif-replay

    # ── systemd units ──
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${ONVIF_S}/onvif.service \
        ${D}${systemd_system_unitdir}/
    install -m 0644 ${ONVIF_S}/onvif-rtsp.service \
        ${D}${systemd_system_unitdir}/
    install -m 0644 ${ONVIF_S}/onvif-replay.service \
        ${D}${systemd_system_unitdir}/
    install -m 0644 ${ONVIF_S}/onvif-rtsp-watchdog.service \
        ${D}${systemd_system_unitdir}/
    install -m 0644 ${ONVIF_S}/onvif-rtsp-watchdog.timer \
        ${D}${systemd_system_unitdir}/

    # ── Boot-time normalizer for /etc/onvif_simple_server.conf ──
    install -m 0755 ${ONVIF_S}/onvif-fixup-conf.sh \
        ${D}${sysconfdir}/onvif-fixup-conf.sh
    install -m 0644 ${ONVIF_S}/onvif-fixup-conf.service \
        ${D}${systemd_system_unitdir}/onvif-fixup-conf.service

    # ── tmpfiles.d entry so gps.service does not spin on /tmp/modem_info ──
    install -d ${D}${sysconfdir}/tmpfiles.d
    install -m 0644 ${ONVIF_S}/onvif-bootstrap.conf \
        ${D}${sysconfdir}/tmpfiles.d/onvif-bootstrap.conf

    # ── ONVIF CGI stub pages (lighttpd document-root = /www/pages/) ──
    install -d ${D}/www/pages/onvif
    for s in device_service media_service media2_service ptz_service \
             events_service deviceio_service imaging_service \
             recording_service search_service replay_service \
             analytics_service \
             notification_service subscription_service pullpoint_service; do
        touch ${D}/www/pages/onvif/$s
        chmod 0644 ${D}/www/pages/onvif/$s
    done
}

# Runtime dependencies — python3 is GONE. Replaced with libxml2 + libcrypto
# for the CGIs and gstreamer1.0-rtsp-server for the live + replay servers.
RDEPENDS:${PN} += " \
    glib-2.0 \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-libav \
    gstreamer1.0-rtsp-server \
    libxml2 \
    libcrypto \
    v4l-utils \
"

FILES:${PN} += " \
    ${sysconfdir}/onvif_simple_server.conf \
    ${sysconfdir}/onvif_cgi_wrapper.sh \
    ${sysconfdir}/onvif-fixup-conf.sh \
    ${sysconfdir}/tmpfiles.d/onvif-bootstrap.conf \
    ${sysconfdir}/wsd_simple_server \
    ${sysconfdir}/default/onvif-rtsp \
    ${sysconfdir}/default/onvif-replay \
    ${ONVIF_DATADIR} \
    ${systemd_system_unitdir}/onvif.service \
    ${systemd_system_unitdir}/onvif-rtsp.service \
    ${systemd_system_unitdir}/onvif-replay.service \
    ${systemd_system_unitdir}/onvif-fixup-conf.service \
    ${systemd_system_unitdir}/onvif-rtsp-watchdog.service \
    ${systemd_system_unitdir}/onvif-rtsp-watchdog.timer \
    /www/pages/onvif \
"

SYSTEMD_SERVICE:${PN} = "onvif.service onvif-rtsp.service onvif-replay.service onvif-fixup-conf.service onvif-rtsp-watchdog.timer"
