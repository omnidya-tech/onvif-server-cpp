// =============================================================================
// onvif-recording-cgi.cpp -- C++17 port of onvif-recording-cgi.py.
//
// ONVIF Recording, Search & Replay Service CGI — Profile G. Indexes the MP4
// recordings under $RECORDING_DIR (default /mnt/videodata/rec/) and exposes
// them via three SOAP services:
//
//   recording_service  — GetRecordings, GetRecordingJobs, GetRecordingJobState,
//                        GetRecordingConfiguration
//   search_service     — FindRecordings, GetRecordingSearchResults,
//                        FindEvents, GetEventSearchResults, EndSearch
//   replay_service     — GetReplayUri, GetReplayConfiguration
//
// The service is selected by argv[1] (set via onvif_cgi_wrapper.sh).
//
// Codec caching: we cache (mtime, size) → "h264"/"h265" results at
// /var/cache/onvif-recording-codec.json so repeated GetRecordings calls
// don't re-probe the moov atom of every file.
//
// Bug-for-bug from Python: the FindRecordings → GetRecordingSearchResults
// flow uses an in-process dict for sessions. Each CGI invocation forks a
// fresh process, so the dict is empty next time. GetRecordingSearchResults
// returns "Unknown search token" until that's redesigned with persistent
// session storage. Fix is out of scope for this port.
// =============================================================================

#include "cgi_soap.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
// OpenSSL 3.0+ marks the one-shot MD5() as deprecated in favour of EVP. We
// only hash short filenames — the one-shot API is fine; suppress the
// warning at the call site rather than dragging in EVP boilerplate.
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/md5.h>     // libcrypto, ubiquitous in Yocto rootfs
#include <unistd.h>

namespace {

const std::string kXmlns =
    " xmlns:trc=\"http://www.onvif.org/ver10/recording/wsdl\""
    " xmlns:tse=\"http://www.onvif.org/ver10/search/wsdl\""
    " xmlns:trp=\"http://www.onvif.org/ver10/replay/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"";

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

const std::string& recording_dir() {
    static const std::string p = env_or("RECORDING_DIR", "/mnt/videodata/rec");
    return p;
}
const std::string& replay_port() {
    static const std::string p = env_or("REPLAY_PORT", "555");
    return p;
}
const std::string& ai_dir() {
    static const std::string p = env_or("AI_DIR", "/mnt/videodata/ai");
    return p;
}

constexpr off_t MIN_PLAYABLE_SIZE = 1024;
constexpr std::size_t TAIL_PROBE_BYTES = 64 * 1024;
const char* CODEC_CACHE_PATH = "/var/cache/onvif-recording-codec.json";

// =============================================================================
// MD5 of the filename, hex-encoded, first 16 chars (matches Python's
// hashlib.md5(fname.encode()).hexdigest()[:16]).
// =============================================================================

std::string md5_token(const std::string& s) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
    char hex[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    hex[32] = 0;
    return std::string(hex, 16);
}

// =============================================================================
// Codec probe: read the last 64 KiB of the file and look for FourCC.
// =============================================================================

std::string probe_tail_fourcc(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) { ::close(fd); return ""; }
    off_t off = size > static_cast<off_t>(TAIL_PROBE_BYTES)
              ? size - static_cast<off_t>(TAIL_PROBE_BYTES) : 0;
    ::lseek(fd, off, SEEK_SET);

    std::string buf;
    buf.resize(TAIL_PROBE_BYTES);
    ssize_t got = 0;
    while (got < static_cast<ssize_t>(buf.size())) {
        ssize_t r = ::read(fd, buf.data() + got, buf.size() - got);
        if (r <= 0) break;
        got += r;
    }
    ::close(fd);
    buf.resize(got);

    auto contains = [&](const char* needle) {
        return buf.find(needle) != std::string::npos;
    };
    if (contains("hvc1") || contains("hev1")) return "h265";
    if (contains("avc1") || contains("avc3")) return "h264";
    return "";
}

// =============================================================================
// Codec cache stored as a tiny JSON: { "<fname>": {"k": "<mtime>:<size>", "c": "h264"} }
// We use a hand-rolled writer/reader since the schema is fixed.
// =============================================================================

struct CodecCacheEntry { std::string key; std::string codec; };

void load_codec_cache(std::map<std::string, CodecCacheEntry>& out) {
    std::ifstream f(CODEC_CACHE_PATH);
    if (!f) return;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Tiny scanner: walk the top-level object, then each "fname": {...}.
    auto skip_ws = [&](std::size_t& i){ while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    auto read_str = [&](std::size_t& i, std::string& out_str) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out_str.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) { out_str.push_back(s[i+1]); i += 2; }
            else { out_str.push_back(s[i++]); }
        }
        if (i < s.size() && s[i] == '"') { ++i; return true; }
        return false;
    };

    std::size_t i = 0;
    skip_ws(i);
    if (i >= s.size() || s[i] != '{') return;
    ++i;
    skip_ws(i);
    while (i < s.size() && s[i] != '}') {
        std::string fname;
        if (!read_str(i, fname)) return;
        skip_ws(i);
        if (i >= s.size() || s[i] != ':') return;
        ++i;
        skip_ws(i);
        if (i >= s.size() || s[i] != '{') return;
        ++i;
        CodecCacheEntry entry;
        // Walk inner object until '}'.
        while (i < s.size() && s[i] != '}') {
            skip_ws(i);
            std::string field;
            if (!read_str(i, field)) return;
            skip_ws(i);
            if (i >= s.size() || s[i] != ':') return;
            ++i;
            skip_ws(i);
            std::string val;
            if (i < s.size() && s[i] == '"') { read_str(i, val); }
            else if (s.compare(i, 4, "null") == 0) { val.clear(); i += 4; }
            else {
                while (i < s.size() && s[i] != ',' && s[i] != '}') { val.push_back(s[i++]); }
            }
            if (field == "k") entry.key = val;
            else if (field == "c") entry.codec = val;
            skip_ws(i);
            if (i < s.size() && s[i] == ',') { ++i; skip_ws(i); }
        }
        if (i < s.size() && s[i] == '}') ++i;
        out.emplace(fname, std::move(entry));
        skip_ws(i);
        if (i < s.size() && s[i] == ',') { ++i; skip_ws(i); }
    }
}

void save_codec_cache(const std::map<std::string, CodecCacheEntry>& cache) {
    // Ensure /var/cache exists. mkdir(2) fails harmlessly with EEXIST.
    const char* CACHE_DIR = "/var/cache";
    ::mkdir(CACHE_DIR, 0755);

    std::string tmp = std::string(CODEC_CACHE_PATH) + ".tmp";
    std::ofstream f(tmp);
    if (!f) return;
    f << "{";
    bool first = true;
    for (const auto& [fname, e] : cache) {
        if (!first) f << ",";
        first = false;
        f << "\"" << fname << "\":{\"k\":\"" << e.key << "\",\"c\":";
        if (e.codec.empty()) f << "null";
        else                  f << "\"" << e.codec << "\"";
        f << "}";
    }
    f << "}";
    f.close();
    if (!f) return;
    ::rename(tmp.c_str(), CODEC_CACHE_PATH);
}

// =============================================================================
// Scan recordings: returns a sorted list of dicts ~ Python's scan_recordings().
// =============================================================================

struct Recording {
    std::string token;
    std::string channel;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string path;
    std::string filename;
    off_t size = 0;
    std::string codec;
};

std::vector<Recording> scan_recordings() {
    std::vector<Recording> out;

    DIR* d = ::opendir(recording_dir().c_str());
    if (!d) return out;

    std::vector<std::string> filenames;
    while (struct dirent* de = ::readdir(d)) {
        std::string n = de->d_name;
        if (n.size() < 4 || n.substr(n.size() - 4) != ".mp4") continue;
        filenames.push_back(std::move(n));
    }
    ::closedir(d);
    std::sort(filenames.begin(), filenames.end());

    std::map<std::string, CodecCacheEntry> cache;
    load_codec_cache(cache);
    bool dirty = false;

    for (const std::string& fname : filenames) {
        std::string path = recording_dir() + "/" + fname;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;
        if (st.st_size < MIN_PLAYABLE_SIZE) continue;

        char keybuf[64];
        std::snprintf(keybuf, sizeof(keybuf),
                      "%lld:%lld",
                      static_cast<long long>(st.st_mtime),
                      static_cast<long long>(st.st_size));
        std::string cache_key = keybuf;

        std::string codec;
        auto it = cache.find(fname);
        if (it != cache.end() && it->second.key == cache_key) {
            codec = it->second.codec;
        } else {
            codec = probe_tail_fourcc(path);
            cache[fname] = CodecCacheEntry{cache_key, codec};
            dirty = true;
        }
        if (codec.empty()) continue;  // corrupt / unsupported

        // Parse "<channel>_<start_epoch>_<end_epoch>.mp4".
        std::string name_no_ext = fname.substr(0, fname.size() - 4);
        // rsplit('_', 2)
        auto last  = name_no_ext.rfind('_');
        auto first = (last == std::string::npos) ? std::string::npos : name_no_ext.rfind('_', last - 1);

        Recording r;
        r.path = path;
        r.filename = fname;
        r.size = st.st_size;
        r.codec = codec;
        r.token = md5_token(fname);

        if (first != std::string::npos && last != std::string::npos && first < last) {
            r.channel = name_no_ext.substr(0, first);
            try {
                r.start = std::stoll(name_no_ext.substr(first + 1, last - first - 1));
                r.end   = std::stoll(name_no_ext.substr(last + 1));
            } catch (...) { continue; }
        } else {
            r.channel = name_no_ext;
            r.start = static_cast<std::int64_t>(st.st_mtime) - 120;
            r.end   = static_cast<std::int64_t>(st.st_mtime);
        }
        out.push_back(std::move(r));
    }

    if (dirty) {
        std::set<std::string> live(filenames.begin(), filenames.end());
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (!live.count(it->first)) it = cache.erase(it);
            else ++it;
        }
        save_codec_cache(cache);
    }
    return out;
}

// =============================================================================
// Search session state. Process-local; matches Python bug-for-bug.
// =============================================================================

struct SearchSession {
    std::vector<Recording> recordings;
    std::vector<std::pair<std::int64_t, std::string>> events;   // <epoch, file-of-rendered-event>
    std::size_t offset = 0;
    enum Kind { Recordings, Events } kind = Recordings;
};

std::map<std::string, SearchSession> g_sessions;
int g_search_counter = 0;

// =============================================================================
// Recording service handlers
// =============================================================================

void handle_get_recordings(xmlDocPtr) {
    auto recs = scan_recordings();
    std::ostringstream body;
    body << "<trc:GetRecordingsResponse>";
    for (const auto& r : recs) {
        body << "<trc:RecordingItem>"
                "<tt:RecordingToken>" << r.token << "</tt:RecordingToken>"
                "<tt:Configuration>"
                "<tt:Source>"
                "<tt:SourceId>" << cgi::xml_escape(r.channel) << "</tt:SourceId>"
                "<tt:Name>"     << cgi::xml_escape(r.channel) << "</tt:Name>"
                "<tt:Location>Vehicle</tt:Location>"
                "<tt:Description>Dashcam recording</tt:Description>"
                "<tt:Address></tt:Address>"
                "</tt:Source>"
                "<tt:Content>Video</tt:Content>"
                "<tt:MaximumRetentionTime>PT0S</tt:MaximumRetentionTime>"
                "</tt:Configuration>"
                "<tt:Tracks>"
                "<tt:Track>"
                "<tt:TrackToken>video</tt:TrackToken>"
                "<tt:TrackType>Video</tt:TrackType>"
                "<tt:Description>H.264/H.265 video</tt:Description>"
                "<tt:DataFrom>" << cgi::epoch_to_onvif(r.start) << "</tt:DataFrom>"
                "<tt:DataTo>"   << cgi::epoch_to_onvif(r.end)   << "</tt:DataTo>"
                "</tt:Track>"
                "</tt:Tracks>"
                "</trc:RecordingItem>";
    }
    body << "</trc:GetRecordingsResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_recording_jobs(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<trc:GetRecordingJobsResponse>"
        "<trc:JobItem>"
        "<tt:JobToken>dashcam_continuous</tt:JobToken>"
        "<tt:JobConfiguration>"
        "<tt:RecordingToken>active</tt:RecordingToken>"
        "<tt:Mode>Active</tt:Mode>"
        "<tt:Priority>1</tt:Priority>"
        "<tt:Source>"
        "<tt:SourceToken>"
        "<tt:Token>front</tt:Token>"
        "<tt:Type>http://www.onvif.org/ver10/schema/VideoSource</tt:Type>"
        "</tt:SourceToken>"
        "</tt:Source>"
        "</tt:JobConfiguration>"
        "</trc:JobItem>"
        "</trc:GetRecordingJobsResponse>");
}

void handle_get_recording_job_state(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<trc:GetRecordingJobStateResponse>"
        "<trc:State>"
        "<tt:RecordingToken>active</tt:RecordingToken>"
        "<tt:State>Active</tt:State>"
        "<tt:Sources>"
        "<tt:SourceToken>"
        "<tt:Token>front</tt:Token>"
        "<tt:Type>http://www.onvif.org/ver10/schema/VideoSource</tt:Type>"
        "</tt:SourceToken>"
        "<tt:State>Active</tt:State>"
        "<tt:Tracks>"
        "<tt:SourceTag>video</tt:SourceTag>"
        "<tt:Destination>video</tt:Destination>"
        "<tt:State>Active</tt:State>"
        "</tt:Tracks>"
        "</tt:Sources>"
        "</trc:State>"
        "</trc:GetRecordingJobStateResponse>");
}

void handle_get_recording_configuration(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<trc:GetRecordingConfigurationResponse>"
        "<trc:RecordingConfiguration>"
        "<tt:Source>"
        "<tt:SourceId>front</tt:SourceId>"
        "<tt:Name>Front Camera</tt:Name>"
        "<tt:Location>Vehicle</tt:Location>"
        "<tt:Description>Continuous dashcam recording</tt:Description>"
        "<tt:Address></tt:Address>"
        "</tt:Source>"
        "<tt:Content>Video</tt:Content>"
        "<tt:MaximumRetentionTime>PT0S</tt:MaximumRetentionTime>"
        "</trc:RecordingConfiguration>"
        "</trc:GetRecordingConfigurationResponse>");
}

// =============================================================================
// Search service handlers
// =============================================================================

void handle_find_recordings(xmlDocPtr) {
    // Time filter parsing is not actually applied (Python comment: "Complex
    // XPath filters — return all and let client filter").
    std::string token = "search_" + std::to_string(++g_search_counter);
    SearchSession s;
    s.kind = SearchSession::Recordings;
    s.recordings = scan_recordings();
    g_sessions[token] = std::move(s);

    std::ostringstream body;
    body << "<tse:FindRecordingsResponse>"
         << "<tse:SearchToken>" << token << "</tse:SearchToken>"
         << "</tse:FindRecordingsResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_recording_search_results(xmlDocPtr doc) {
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr tn = cgi::find_element(root, "SearchToken", cgi::NS_TSE);
    if (!tn) {
        cgi::soap_fault(kXmlns, "s:Sender", "Missing SearchToken");
        return;
    }
    std::string token = cgi::text_of(tn);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) {
        cgi::soap_fault(kXmlns, "s:Sender", "Unknown search token: " + token);
        return;
    }
    SearchSession& s = it->second;

    int max_results = 50;
    if (xmlNodePtr m = cgi::find_element(root, "MaxResults", cgi::NS_TSE)) {
        std::string t = cgi::text_of(m);
        if (!t.empty()) max_results = std::atoi(t.c_str());
    }

    std::size_t end = std::min(s.offset + static_cast<std::size_t>(max_results),
                               s.recordings.size());
    bool completed = (end >= s.recordings.size());

    std::ostringstream body;
    body << "<tse:GetRecordingSearchResultsResponse>"
            "<tse:ResultList>"
            "<tt:SearchState>" << (completed ? "Completed" : "Searching") << "</tt:SearchState>";
    for (std::size_t i = s.offset; i < end; ++i) {
        const auto& r = s.recordings[i];
        body << "<tt:RecordingInformation>"
                "<tt:RecordingToken>" << r.token << "</tt:RecordingToken>"
                "<tt:Source>"
                "<tt:SourceId>" << cgi::xml_escape(r.channel) << "</tt:SourceId>"
                "<tt:Name>"     << cgi::xml_escape(r.channel) << "</tt:Name>"
                "<tt:Location>Vehicle</tt:Location>"
                "<tt:Description>Dashcam recording</tt:Description>"
                "<tt:Address></tt:Address>"
                "</tt:Source>"
                "<tt:Content>Video</tt:Content>"
                "<tt:EarliestRecording>" << cgi::epoch_to_onvif(r.start) << "</tt:EarliestRecording>"
                "<tt:LatestRecording>"   << cgi::epoch_to_onvif(r.end)   << "</tt:LatestRecording>"
                "<tt:RecordingStatus>Stopped</tt:RecordingStatus>"
                "<tt:Track>"
                "<tt:TrackToken>video</tt:TrackToken>"
                "<tt:TrackType>Video</tt:TrackType>"
                "<tt:Description>H.264/H.265 video</tt:Description>"
                "<tt:DataFrom>" << cgi::epoch_to_onvif(r.start) << "</tt:DataFrom>"
                "<tt:DataTo>"   << cgi::epoch_to_onvif(r.end)   << "</tt:DataTo>"
                "</tt:Track>"
                "</tt:RecordingInformation>";
    }
    body << "</tse:ResultList>"
            "</tse:GetRecordingSearchResultsResponse>";
    s.offset = end;
    cgi::soap_response(kXmlns, body.str());
}

// Walk $AI_DIR recursively and collect (mtime, fname).
void walk_events(const std::string& root,
                 std::vector<std::pair<std::int64_t, std::string>>& out) {
    DIR* d = ::opendir(root.c_str());
    if (!d) return;
    std::vector<std::string> sub_files;
    std::vector<std::string> subdirs;
    while (struct dirent* de = ::readdir(d)) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
        std::string p = root + "/" + de->d_name;
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) subdirs.push_back(de->d_name);
        else if (S_ISREG(st.st_mode)) {
            std::string n = de->d_name;
            if (n.size() >= 4) {
                std::string ext = n.substr(n.size() - 4);
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".png" || ext == ".jpg") {
                    out.emplace_back(static_cast<std::int64_t>(st.st_mtime), n);
                }
            }
        }
    }
    ::closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& s : subdirs) walk_events(root + "/" + s, out);
}

void handle_find_events(xmlDocPtr) {
    std::string token = "evtsearch_" + std::to_string(++g_search_counter);
    SearchSession s;
    s.kind = SearchSession::Events;
    walk_events(ai_dir(), s.events);
    g_sessions[token] = std::move(s);

    cgi::soap_response(kXmlns,
        "<tse:FindEventsResponse><tse:SearchToken>" + token +
        "</tse:SearchToken></tse:FindEventsResponse>");
}

void handle_get_event_search_results(xmlDocPtr doc) {
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr tn = cgi::find_element(root, "SearchToken", cgi::NS_TSE);
    if (!tn) {
        cgi::soap_fault(kXmlns, "s:Sender", "Missing SearchToken");
        return;
    }
    std::string token = cgi::text_of(tn);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) {
        cgi::soap_fault(kXmlns, "s:Sender", "Unknown search token: " + token);
        return;
    }
    SearchSession& s = it->second;

    constexpr std::size_t MAX_RESULTS = 50;
    std::size_t end = std::min(s.offset + MAX_RESULTS, s.events.size());
    bool completed = (end >= s.events.size());

    std::ostringstream body;
    body << "<tse:GetEventSearchResultsResponse>"
            "<tse:ResultList>"
            "<tt:SearchState>" << (completed ? "Completed" : "Searching") << "</tt:SearchState>";
    for (std::size_t i = s.offset; i < end; ++i) {
        const auto& [t, fname] = s.events[i];
        body << "<tt:Result>"
                "<tt:RecordingToken>event</tt:RecordingToken>"
                "<tt:TrackToken>video</tt:TrackToken>"
                "<tt:Time>" << cgi::epoch_to_onvif(t) << "</tt:Time>"
                "<tt:Event>"
                "<tt:Topic>tns1:RuleEngine/CellMotionDetector/Motion</tt:Topic>"
                "<tt:Source>"
                "<tt:SimpleItem Name=\"DetectionType\" Value=\""
                  << cgi::xml_escape(fname) << "\"/>"
                "</tt:Source>"
                "<tt:Data>"
                "<tt:SimpleItem Name=\"IsMotion\" Value=\"true\"/>"
                "<tt:SimpleItem Name=\"File\" Value=\""
                  << cgi::xml_escape(fname) << "\"/>"
                "</tt:Data>"
                "</tt:Event>"
                "</tt:Result>";
    }
    body << "</tse:ResultList>"
            "</tse:GetEventSearchResultsResponse>";
    s.offset = end;
    cgi::soap_response(kXmlns, body.str());
}

void handle_end_search(xmlDocPtr doc) {
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (xmlNodePtr tn = cgi::find_element(root, "SearchToken", cgi::NS_TSE)) {
        g_sessions.erase(cgi::text_of(tn));
    }
    cgi::soap_response(kXmlns, "<tse:EndSearchResponse/>");
}

// =============================================================================
// Replay service handlers
// =============================================================================

void handle_get_replay_uri(xmlDocPtr doc) {
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr tn = cgi::find_element(root, "RecordingToken", cgi::NS_TRP);
    if (!tn) {
        cgi::soap_fault(kXmlns, "s:Sender", "Missing RecordingToken");
        return;
    }
    std::string rec_token = cgi::text_of(tn);
    if (rec_token.empty()) {
        cgi::soap_fault(kXmlns, "s:Sender", "Missing RecordingToken");
        return;
    }
    std::string ip = cgi::get_client_ip();

    std::ostringstream body;
    body << "<trp:GetReplayUriResponse>"
         << "<trp:Uri>rtsp://" << ip << ":" << replay_port()
         << "/replay?token=" << rec_token << "</trp:Uri>"
         << "</trp:GetReplayUriResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_replay_configuration(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<trp:GetReplayConfigurationResponse>"
        "<trp:Configuration>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        "</trp:Configuration>"
        "</trp:GetReplayConfigurationResponse>");
}

} // namespace

int main(int argc, char** argv) {
    std::string service = (argc > 1) ? argv[1] : "recording_service";

    cgi::ActionTable t;
    t.fault_xmlns = kXmlns;

    if (service == "recording_service") {
        t.on("GetRecordings",             handle_get_recordings);
        t.on("GetRecordingJobs",          handle_get_recording_jobs);
        t.on("GetRecordingJobState",      handle_get_recording_job_state);
        t.on("GetRecordingConfiguration", handle_get_recording_configuration);
    } else if (service == "search_service") {
        t.on("FindRecordings",            handle_find_recordings);
        t.on("GetRecordingSearchResults", handle_get_recording_search_results);
        t.on("FindEvents",                handle_find_events);
        t.on("GetEventSearchResults",     handle_get_event_search_results);
        t.on("EndSearch",                 handle_end_search);
    } else if (service == "replay_service") {
        t.on("GetReplayUri",              handle_get_replay_uri);
        t.on("GetReplayConfiguration",    handle_get_replay_configuration);
    }

    t.dispatch();
    return 0;
}
