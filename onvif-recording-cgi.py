#!/usr/bin/env python3
"""
ONVIF Recording, Search & Replay Service CGI — Profile G
Exposes dashcam SD-card recordings via ONVIF.

Recordings live at RECORDING_DIR (default /mnt/videodata/rec/) with naming:
    <channel>_<start_epoch>_<end_epoch>.mp4

The CGI is invoked with a service name argument:
    recording_service  — GetRecordings, GetRecordingJobs, etc.
    search_service     — FindRecordings, GetRecordingSearchResults, etc.
    replay_service     — GetReplayUri
"""

import os
import sys
import glob
import json
import time
import hashlib
from datetime import datetime, timezone

RECORDING_DIR = os.environ.get('RECORDING_DIR', '/mnt/videodata/rec')
REPLAY_PORT = os.environ.get('REPLAY_PORT', '555')

# Files smaller than this are still-being-written or empty placeholders;
# advertising their tokens leads to RTSP 503 when the client tries to play them.
MIN_PLAYABLE_SIZE = 1024

# Codec cache. scan_recordings() validates each MP4 has a recognisable codec
# FourCC (hvc1/hev1/avc1/avc3); files that don't are genuinely corrupt (e.g.
# missing `moov`) and would cause DESCRIBE → 400/503 in the replay server.
# We cache the per-file result keyed by (mtime, size) so only new/changed
# files pay the probe cost. First-ever call scans ~1k files in ~5s; after
# that it's near-instant.
CODEC_CACHE_PATH = '/var/cache/onvif-recording-codec.json'
_TAIL_PROBE_BYTES = 64 * 1024


def _probe_tail_fourcc(path):
    """Scan the last 64 KiB of the file for codec FourCC in the `moov` atom.

    Dashcam MP4s write `moov` at the end of the file (streaming-style),
    so head-only scans miss it. Returns 'h264', 'h265', or None.
    """
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            f.seek(max(0, size - _TAIL_PROBE_BYTES))
            tail = f.read()
    except OSError:
        return None
    if b'hvc1' in tail or b'hev1' in tail:
        return 'h265'
    if b'avc1' in tail or b'avc3' in tail:
        return 'h264'
    return None


def _load_codec_cache():
    try:
        with open(CODEC_CACHE_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def _save_codec_cache(cache):
    tmp = CODEC_CACHE_PATH + '.tmp'
    try:
        os.makedirs(os.path.dirname(CODEC_CACHE_PATH), exist_ok=True)
        with open(tmp, 'w') as f:
            json.dump(cache, f)
        os.rename(tmp, CODEC_CACHE_PATH)
    except Exception:
        pass

NS_SOAP = 'http://www.w3.org/2003/05/soap-envelope'
NS_TRC  = 'http://www.onvif.org/ver10/recording/wsdl'
NS_TSE  = 'http://www.onvif.org/ver10/search/wsdl'
NS_TRP  = 'http://www.onvif.org/ver10/replay/wsdl'
NS_TT   = 'http://www.onvif.org/ver10/schema'


def epoch_to_onvif(epoch):
    """Convert Unix epoch to ONVIF DateTime string."""
    dt = datetime.fromtimestamp(int(epoch), tz=timezone.utc)
    return dt.strftime('%Y-%m-%dT%H:%M:%SZ')


def onvif_to_epoch(s):
    """Convert ONVIF DateTime string to Unix epoch."""
    s = s.rstrip('Z').split('.')[0]
    try:
        dt = datetime.strptime(s, '%Y-%m-%dT%H:%M:%S')
        return int(dt.replace(tzinfo=timezone.utc).timestamp())
    except ValueError:
        return 0


def scan_recordings():
    """Scan the recording directory and return a list of recording dicts.

    Files whose codec can't be identified (no hvc1/hev1/avc1/avc3 FourCC in
    the moov atom) are skipped — these are corrupt MP4s on the SD card that
    would fail RTSP DESCRIBE on the replay server. The per-file probe result
    is cached on disk keyed by (mtime, size).
    """
    recordings = []
    pattern = os.path.join(RECORDING_DIR, '*.mp4')
    cache = _load_codec_cache()
    cache_dirty = False

    for path in sorted(glob.glob(pattern)):
        try:
            st = os.stat(path)
        except OSError:
            continue
        if st.st_size < MIN_PLAYABLE_SIZE:
            continue

        fname = os.path.basename(path)
        cache_key = f'{int(st.st_mtime)}:{st.st_size}'
        entry = cache.get(fname)
        if entry and entry.get('k') == cache_key:
            codec = entry.get('c')
        else:
            codec = _probe_tail_fourcc(path)
            cache[fname] = {'k': cache_key, 'c': codec}
            cache_dirty = True
        if codec is None:
            # Corrupt file — don't advertise it to clients.
            continue

        name_no_ext = fname.rsplit('.', 1)[0]
        parts = name_no_ext.rsplit('_', 2)
        if len(parts) >= 3:
            channel = parts[0]
            try:
                start_epoch = int(parts[1])
                end_epoch = int(parts[2])
            except ValueError:
                continue
        else:
            # Fallback: use file mtime
            channel = name_no_ext
            start_epoch = int(st.st_mtime) - 120
            end_epoch = int(st.st_mtime)

        token = hashlib.md5(fname.encode()).hexdigest()[:16]
        recordings.append({
            'token': token,
            'channel': channel,
            'start': start_epoch,
            'end': end_epoch,
            'path': path,
            'filename': fname,
            'size': st.st_size,
            'codec': codec,
        })

    if cache_dirty:
        # Prune cache entries for files no longer on disk.
        live = {os.path.basename(p) for p in glob.glob(pattern)}
        for k in list(cache.keys()):
            if k not in live:
                del cache[k]
        _save_codec_cache(cache)

    return recordings


def soap_response(body_xml):
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"'
        ' xmlns:trc="http://www.onvif.org/ver10/recording/wsdl"'
        ' xmlns:tse="http://www.onvif.org/ver10/search/wsdl"'
        ' xmlns:trp="http://www.onvif.org/ver10/replay/wsdl"'
        ' xmlns:tt="http://www.onvif.org/ver10/schema">'
        '<s:Body>' + body_xml + '</s:Body>'
        '</s:Envelope>'
    )
    sys.stdout.write('Content-Type: application/soap+xml; charset=utf-8\r\n')
    sys.stdout.write(f'Content-Length: {len(xml)}\r\n')
    sys.stdout.write('\r\n')
    sys.stdout.write(xml)


def soap_fault(code, reason):
    soap_response(
        f'<s:Fault><s:Code><s:Value>{code}</s:Value></s:Code>'
        f'<s:Reason><s:Text xml:lang="en">{reason}</s:Text></s:Reason>'
        f'</s:Fault>'
    )


def get_client_ip():
    """Get the IP the client used to reach us (for RTSP URI construction)."""
    return os.environ.get('SERVER_ADDR',
           os.environ.get('HTTP_HOST', '192.168.20.2').split(':')[0])


# ── Recording Service handlers ───────────────────────────────────────

def handle_get_recordings():
    recs = scan_recordings()
    items = []
    for r in recs:
        items.append(
            '<trc:RecordingItem>'
            f'<tt:RecordingToken>{r["token"]}</tt:RecordingToken>'
            '<tt:Configuration>'
            '<tt:Source>'
            f'<tt:SourceId>{r["channel"]}</tt:SourceId>'
            f'<tt:Name>{r["channel"]}</tt:Name>'
            '<tt:Location>Vehicle</tt:Location>'
            '<tt:Description>Dashcam recording</tt:Description>'
            '<tt:Address></tt:Address>'
            '</tt:Source>'
            '<tt:Content>Video</tt:Content>'
            '<tt:MaximumRetentionTime>PT0S</tt:MaximumRetentionTime>'
            '</tt:Configuration>'
            '<tt:Tracks>'
            '<tt:Track>'
            '<tt:TrackToken>video</tt:TrackToken>'
            '<tt:TrackType>Video</tt:TrackType>'
            '<tt:Description>H.264/H.265 video</tt:Description>'
            f'<tt:DataFrom>{epoch_to_onvif(r["start"])}</tt:DataFrom>'
            f'<tt:DataTo>{epoch_to_onvif(r["end"])}</tt:DataTo>'
            '</tt:Track>'
            '</tt:Tracks>'
            '</trc:RecordingItem>'
        )
    soap_response(
        '<trc:GetRecordingsResponse>'
        + ''.join(items) +
        '</trc:GetRecordingsResponse>'
    )


def handle_get_recording_jobs():
    """Return a single recording job representing the active dashcam recording."""
    soap_response(
        '<trc:GetRecordingJobsResponse>'
        '<trc:JobItem>'
        '<tt:JobToken>dashcam_continuous</tt:JobToken>'
        '<tt:JobConfiguration>'
        '<tt:RecordingToken>active</tt:RecordingToken>'
        '<tt:Mode>Active</tt:Mode>'
        '<tt:Priority>1</tt:Priority>'
        '<tt:Source>'
        '<tt:SourceToken>'
        '<tt:Token>front</tt:Token>'
        '<tt:Type>http://www.onvif.org/ver10/schema/VideoSource</tt:Type>'
        '</tt:SourceToken>'
        '</tt:Source>'
        '</tt:JobConfiguration>'
        '</trc:JobItem>'
        '</trc:GetRecordingJobsResponse>'
    )


def handle_get_recording_job_state():
    """The dashcam is always recording."""
    soap_response(
        '<trc:GetRecordingJobStateResponse>'
        '<trc:State>'
        '<tt:RecordingToken>active</tt:RecordingToken>'
        '<tt:State>Active</tt:State>'
        '<tt:Sources>'
        '<tt:SourceToken>'
        '<tt:Token>front</tt:Token>'
        '<tt:Type>http://www.onvif.org/ver10/schema/VideoSource</tt:Type>'
        '</tt:SourceToken>'
        '<tt:State>Active</tt:State>'
        '<tt:Tracks>'
        '<tt:SourceTag>video</tt:SourceTag>'
        '<tt:Destination>video</tt:Destination>'
        '<tt:State>Active</tt:State>'
        '</tt:Tracks>'
        '</tt:Sources>'
        '</trc:State>'
        '</trc:GetRecordingJobStateResponse>'
    )


def handle_get_recording_configuration(root):
    """GetRecordingConfiguration for a specific token."""
    soap_response(
        '<trc:GetRecordingConfigurationResponse>'
        '<trc:RecordingConfiguration>'
        '<tt:Source>'
        '<tt:SourceId>front</tt:SourceId>'
        '<tt:Name>Front Camera</tt:Name>'
        '<tt:Location>Vehicle</tt:Location>'
        '<tt:Description>Continuous dashcam recording</tt:Description>'
        '<tt:Address></tt:Address>'
        '</tt:Source>'
        '<tt:Content>Video</tt:Content>'
        '<tt:MaximumRetentionTime>PT0S</tt:MaximumRetentionTime>'
        '</trc:RecordingConfiguration>'
        '</trc:GetRecordingConfigurationResponse>'
    )


# ── Search Service handlers ──────────────────────────────────────────

# In-memory search sessions
_search_sessions = {}
_search_counter = 0


def handle_find_recordings(root):
    """FindRecordings — initiate a recording search with optional time filter."""
    global _search_counter

    # Parse optional time range from Scope > IncludedSources > RecordingInformationFilter
    start_filter = 0
    end_filter = int(time.time()) + 86400

    # Try to extract time filter from SearchScope
    scope = root.find(f'.//{{{NS_TSE}}}Scope')
    if scope is None:
        scope = root.find('.//{http://www.onvif.org/ver10/search/wsdl}Scope')
    if scope is not None:
        rec_filter = scope.find(f'{{{NS_TSE}}}RecordingInformationFilter')
        if rec_filter is None:
            rec_filter = scope.find('{http://www.onvif.org/ver10/search/wsdl}RecordingInformationFilter')
        if rec_filter is not None and rec_filter.text:
            # Simple parsing for "boolean(//Track[tt:DataFrom > ... ])"
            pass  # Complex XPath filters — return all and let client filter

    # Scan and store results
    _search_counter += 1
    token = f'search_{_search_counter}'
    recs = scan_recordings()

    # Apply time filter
    filtered = [r for r in recs
                if r['end'] >= start_filter and r['start'] <= end_filter]

    _search_sessions[token] = {
        'results': filtered,
        'offset': 0,
    }

    soap_response(
        '<tse:FindRecordingsResponse>'
        f'<tse:SearchToken>{token}</tse:SearchToken>'
        '</tse:FindRecordingsResponse>'
    )


def handle_get_recording_search_results(root):
    """GetRecordingSearchResults — return paginated results for a search token."""
    # Extract search token
    token_el = root.find(f'.//{{{NS_TSE}}}SearchToken')
    if token_el is None:
        token_el = root.find('.//{http://www.onvif.org/ver10/search/wsdl}SearchToken')
    if token_el is None or not token_el.text:
        soap_fault('s:Sender', 'Missing SearchToken')
        return

    token = token_el.text
    session = _search_sessions.get(token)
    if not session:
        soap_fault('s:Sender', f'Unknown search token: {token}')
        return

    # Parse optional MaxResults / WaitTime
    max_results = 50
    max_el = root.find(f'.//{{{NS_TSE}}}MaxResults')
    if max_el is None:
        max_el = root.find('.//{http://www.onvif.org/ver10/search/wsdl}MaxResults')
    if max_el is not None and max_el.text:
        max_results = int(max_el.text)

    results = session['results']
    offset = session['offset']
    page = results[offset:offset + max_results]
    session['offset'] = offset + len(page)

    state = 'Completed' if session['offset'] >= len(results) else 'Searching'

    items = []
    for r in page:
        items.append(
            '<tt:RecordingInformation>'
            f'<tt:RecordingToken>{r["token"]}</tt:RecordingToken>'
            '<tt:Source>'
            f'<tt:SourceId>{r["channel"]}</tt:SourceId>'
            f'<tt:Name>{r["channel"]}</tt:Name>'
            '<tt:Location>Vehicle</tt:Location>'
            '<tt:Description>Dashcam recording</tt:Description>'
            '<tt:Address></tt:Address>'
            '</tt:Source>'
            '<tt:Content>Video</tt:Content>'
            f'<tt:EarliestRecording>{epoch_to_onvif(r["start"])}</tt:EarliestRecording>'
            f'<tt:LatestRecording>{epoch_to_onvif(r["end"])}</tt:LatestRecording>'
            '<tt:RecordingStatus>Stopped</tt:RecordingStatus>'
            '<tt:Track>'
            '<tt:TrackToken>video</tt:TrackToken>'
            '<tt:TrackType>Video</tt:TrackType>'
            '<tt:Description>H.264/H.265 video</tt:Description>'
            f'<tt:DataFrom>{epoch_to_onvif(r["start"])}</tt:DataFrom>'
            f'<tt:DataTo>{epoch_to_onvif(r["end"])}</tt:DataTo>'
            '</tt:Track>'
            '</tt:RecordingInformation>'
        )

    soap_response(
        '<tse:GetRecordingSearchResultsResponse>'
        '<tse:ResultList>'
        f'<tt:SearchState>{state}</tt:SearchState>'
        + ''.join(items) +
        '</tse:ResultList>'
        '</tse:GetRecordingSearchResultsResponse>'
    )


def handle_find_events(root):
    """FindEvents — search for events; return search token."""
    global _search_counter
    _search_counter += 1
    token = f'evtsearch_{_search_counter}'

    # Scan AI detection images as events
    ai_dir = os.environ.get('AI_DIR', '/mnt/videodata/ai')
    events = []
    for dirpath, _, filenames in os.walk(ai_dir):
        for fname in sorted(filenames):
            if not fname.endswith('.png') and not fname.endswith('.jpg'):
                continue
            fpath = os.path.join(dirpath, fname)
            category = os.path.basename(dirpath)
            stat = os.stat(fpath)
            events.append({
                'time': int(stat.st_mtime),
                'type': category,
                'file': fname,
            })

    _search_sessions[token] = {'results': events, 'offset': 0, 'kind': 'events'}
    soap_response(
        '<tse:FindEventsResponse>'
        f'<tse:SearchToken>{token}</tse:SearchToken>'
        '</tse:FindEventsResponse>'
    )


def handle_get_event_search_results(root):
    """GetEventSearchResults — return AI detection events."""
    token_el = root.find(f'.//{{{NS_TSE}}}SearchToken')
    if token_el is None:
        token_el = root.find('.//{http://www.onvif.org/ver10/search/wsdl}SearchToken')
    if token_el is None or not token_el.text:
        soap_fault('s:Sender', 'Missing SearchToken')
        return

    token = token_el.text
    session = _search_sessions.get(token)
    if not session:
        soap_fault('s:Sender', f'Unknown search token: {token}')
        return

    max_results = 50
    results = session['results']
    offset = session['offset']
    page = results[offset:offset + max_results]
    session['offset'] = offset + len(page)

    state = 'Completed' if session['offset'] >= len(results) else 'Searching'

    items = []
    for ev in page:
        items.append(
            '<tt:Result>'
            f'<tt:RecordingToken>event</tt:RecordingToken>'
            '<tt:TrackToken>video</tt:TrackToken>'
            f'<tt:Time>{epoch_to_onvif(ev["time"])}</tt:Time>'
            '<tt:Event>'
            '<tt:Topic>tns1:RuleEngine/CellMotionDetector/Motion</tt:Topic>'
            '<tt:Source>'
            f'<tt:SimpleItem Name="DetectionType" Value="{ev["type"]}"/>'
            '</tt:Source>'
            '<tt:Data>'
            f'<tt:SimpleItem Name="IsMotion" Value="true"/>'
            f'<tt:SimpleItem Name="File" Value="{ev["file"]}"/>'
            '</tt:Data>'
            '</tt:Event>'
            '</tt:Result>'
        )

    soap_response(
        '<tse:GetEventSearchResultsResponse>'
        '<tse:ResultList>'
        f'<tt:SearchState>{state}</tt:SearchState>'
        + ''.join(items) +
        '</tse:ResultList>'
        '</tse:GetEventSearchResultsResponse>'
    )


def handle_end_search(root):
    """EndSearch — clean up a search session."""
    token_el = root.find(f'.//{{{NS_TSE}}}SearchToken')
    if token_el is None:
        token_el = root.find('.//{http://www.onvif.org/ver10/search/wsdl}SearchToken')
    if token_el is not None and token_el.text:
        _search_sessions.pop(token_el.text, None)
    soap_response('<tse:EndSearchResponse/>')


# ── Replay Service handlers ──────────────────────────────────────────

def handle_get_replay_uri(root):
    """GetReplayUri — return RTSP replay URL for a recording token."""
    rec_token_el = root.find(f'.//{{{NS_TRP}}}RecordingToken')
    if rec_token_el is None:
        rec_token_el = root.find('.//{http://www.onvif.org/ver10/replay/wsdl}RecordingToken')
    if rec_token_el is None or not rec_token_el.text:
        soap_fault('s:Sender', 'Missing RecordingToken')
        return

    rec_token = rec_token_el.text
    ip = get_client_ip()

    soap_response(
        '<trp:GetReplayUriResponse>'
        f'<trp:Uri>rtsp://{ip}:{REPLAY_PORT}/replay?token={rec_token}</trp:Uri>'
        '</trp:GetReplayUriResponse>'
    )


def handle_get_replay_configuration():
    soap_response(
        '<trp:GetReplayConfigurationResponse>'
        '<trp:Configuration>'
        '<tt:SessionTimeout>PT60S</tt:SessionTimeout>'
        '</trp:Configuration>'
        '</trp:GetReplayConfigurationResponse>'
    )


# ── Dispatcher ────────────────────────────────────────────────────────

def main():
    import xml.etree.ElementTree as ET

    content_length = int(os.environ.get('CONTENT_LENGTH', 0))
    if content_length == 0:
        soap_fault('s:Sender', 'Empty request')
        return

    body = sys.stdin.buffer.read(content_length)

    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        soap_fault('s:Sender', 'Malformed XML')
        return

    soap_body = root.find(f'{{{NS_SOAP}}}Body')
    if soap_body is None:
        soap_fault('s:Sender', 'Missing SOAP Body')
        return

    action_el = None
    for child in soap_body:
        action_el = child
        break
    if action_el is None:
        soap_fault('s:Sender', 'Empty SOAP Body')
        return

    tag = action_el.tag.split('}')[-1] if '}' in action_el.tag else action_el.tag

    # Which service are we handling?
    service = sys.argv[1] if len(sys.argv) > 1 else 'recording_service'

    handlers = {}

    if service == 'recording_service':
        handlers = {
            'GetRecordings':              handle_get_recordings,
            'GetRecordingJobs':           handle_get_recording_jobs,
            'GetRecordingJobState':       handle_get_recording_job_state,
            'GetRecordingConfiguration':  lambda: handle_get_recording_configuration(root),
        }
    elif service == 'search_service':
        handlers = {
            'FindRecordings':             lambda: handle_find_recordings(root),
            'GetRecordingSearchResults':  lambda: handle_get_recording_search_results(root),
            'FindEvents':                 lambda: handle_find_events(root),
            'GetEventSearchResults':      lambda: handle_get_event_search_results(root),
            'EndSearch':                  lambda: handle_end_search(root),
        }
    elif service == 'replay_service':
        handlers = {
            'GetReplayUri':               lambda: handle_get_replay_uri(root),
            'GetReplayConfiguration':     handle_get_replay_configuration,
        }

    handler = handlers.get(tag)
    if handler:
        handler()
    else:
        soap_fault('s:Sender', f'Action not supported: {service}/{tag}')


if __name__ == '__main__':
    main()
