#!/usr/bin/env python3
"""
ONVIF Replay RTSP Server — Profile G
Serves recorded MP4 files from the dashcam SD card via RTSP.

Mount point:  /replay?token=<recording_token>   or   /replay/<recording_token>

The token is an MD5 hash prefix of the filename, produced by
onvif-recording-cgi.py.  This server maps it back to the file on disk,
probes its codec with gst-discoverer-1.0, and streams it using either an
H.264 or H.265 RTP pipeline.

Override defaults via /etc/default/onvif-replay:
    RECORDING_DIR  — directory with MP4 recordings
    REPLAY_PORT    — RTSP listen port (default 555)
"""

import os
import sys
import glob
import signal
import hashlib
import subprocess

import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib


RECORDING_DIR = os.environ.get('RECORDING_DIR', '/mnt/videodata/rec')
REPLAY_PORT = os.environ.get('REPLAY_PORT', '555')

# Files smaller than this are still-being-written or empty placeholders;
# qtdemux fails preroll on them.
MIN_PLAYABLE_SIZE = 1024


def detect_codec(path):
    """Return 'h264', 'h265', or None by scanning the MP4 `moov` atom for the
    codec FourCC in the stsd sample description. Avoids gst-discoverer, whose
    default NXP AIUR backend returns nothing for these dashcam files.

    Dashcam MP4s sometimes store `moov` at the front and sometimes at the
    end, so we probe both head and tail of the file.
    """
    try:
        with open(path, 'rb') as f:
            head = f.read(1024 * 1024)
            size = os.path.getsize(path)
            if size > 1024 * 1024:
                f.seek(max(0, size - 1024 * 1024))
                tail = f.read()
            else:
                tail = b''
    except OSError as exc:
        print(f'[replay] open failed for {path}: {exc}', flush=True)
        return None

    blob = head + tail
    # stsd FourCC codes — presence is a reliable signal.
    if b'hvc1' in blob or b'hev1' in blob:
        return 'h265'
    if b'avc1' in blob or b'avc3' in blob:
        return 'h264'
    return None


class ReplayFactory(GstRtspServer.RTSPMediaFactory):
    """Factory for one MP4. Codec is probed lazily on first DESCRIBE so that
    server startup does not block on gst-discoverer for every file."""

    def __init__(self, file_path):
        super().__init__()
        self._file_path = file_path
        self._codec = None
        self.connect('media-configure', self._on_media_configure)

    def do_create_element(self, url):
        if self._codec is None:
            self._codec = detect_codec(self._file_path)
        if self._codec is None:
            print(f'[replay] no codec detected for {self._file_path}, '
                  f'refusing to build pipeline', flush=True)
            return None
        codec = self._codec

        if codec == 'h265':
            pipeline_str = (
                f'filesrc location="{self._file_path}" ! '
                'qtdemux name=demux '
                'demux.video_0 ! queue ! h265parse config-interval=-1 ! '
                'rtph265pay name=pay0 pt=96 config-interval=1'
            )
        else:
            pipeline_str = (
                f'filesrc location="{self._file_path}" ! '
                'qtdemux name=demux '
                'demux.video_0 ! queue ! h264parse config-interval=-1 ! '
                'rtph264pay name=pay0 pt=96 config-interval=1'
            )
        print(f'[replay] Pipeline ({codec}): {pipeline_str}', flush=True)
        try:
            return Gst.parse_launch(pipeline_str)
        except GLib.Error as exc:
            print(f'[replay] parse_launch failed: {exc}', flush=True)
            return None

    def _on_media_configure(self, factory, media):
        media.connect('prepared',
                      lambda m: print(f'[replay] prepared {self._file_path}',
                                      flush=True))
        media.connect('unprepared',
                      lambda m: print(f'[replay] unprepared {self._file_path}',
                                      flush=True))
        media.connect('new-state',
                      lambda m, s: print(f'[replay] state={s} {self._file_path}',
                                         flush=True))


class ReplayServer(GstRtspServer.RTSPServer):
    """Pre-mounts every playable recording at startup.

    The previous strategy — mounting inside a `describe-request` handler —
    does not work: describe-request is an after-the-fact client signal that
    fires *after* GstRTSPServer has already resolved the mount and generated
    the 404 response. Mounts must exist before any DESCRIBE arrives.
    """

    def __init__(self):
        super().__init__()
        self.set_service(REPLAY_PORT)
        self._mounts = self.get_mount_points()
        self._mount_all()

    def _mount_all(self):
        mounted = skipped = 0
        paths = sorted(glob.glob(os.path.join(RECORDING_DIR, '*.mp4')))
        for path in paths:
            try:
                if os.path.getsize(path) < MIN_PLAYABLE_SIZE:
                    skipped += 1
                    continue
            except OSError:
                skipped += 1
                continue

            fname = os.path.basename(path)
            token = hashlib.md5(fname.encode()).hexdigest()[:16]

            # Path-style: /replay/<token> — ffprobe, onvif-gui, Happytime
            f1 = ReplayFactory(path)
            f1.set_shared(False)
            self._mounts.add_factory(f'/replay/{token}', f1)

            # Query-style: /replay?token=<token> — clients that copy
            # GetReplayUri verbatim.
            f2 = ReplayFactory(path)
            f2.set_shared(False)
            self._mounts.add_factory(f'/replay?token={token}', f2)

            mounted += 1
        print(f'[replay] Pre-mounted {mounted} recordings, skipped {skipped}',
              flush=True)


def main():
    Gst.init(None)

    if not os.path.isdir(RECORDING_DIR):
        print(f'[replay] WARNING: Recording directory not found: {RECORDING_DIR}',
              flush=True)

    server = ReplayServer()
    server.attach(None)

    print(f'[replay] Replay RTSP server ready on port {REPLAY_PORT}', flush=True)
    print(f'[replay] Recording directory: {RECORDING_DIR}', flush=True)

    loop = GLib.MainLoop()

    def shutdown(signum, frame):
        loop.quit()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    try:
        loop.run()
    except KeyboardInterrupt:
        loop.quit()


if __name__ == '__main__':
    main()
