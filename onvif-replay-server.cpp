// =============================================================================
// onvif-replay-server.cpp -- C++17 port of onvif-replay-server.py.
//
// ONVIF Replay RTSP Server — Profile G. Serves recorded MP4 files from the
// dashcam SD card via RTSP.
//
// Mount paths (per recording, identical to Python):
//     /replay/<token>            — path style; ffprobe, onvif-gui, Happytime
//     /replay?token=<token>      — query style; clients that copy GetReplayUri verbatim
//
// The token is the first 16 hex chars of MD5(filename), matching the
// onvif-recording-cgi GetReplayUri output.
//
// Codec detection is eager (at server startup) by scanning the moov atom
// in the file's head and tail for the FourCC. The previous Python design
// used lazy detection via a GstRTSPMediaFactory subclass; subclassing
// GObject from C++ adds boilerplate without measurable benefit, so we
// pre-resolve the codec and bake the pipeline string at mount time.
//
// Env overrides:
//     RECORDING_DIR  (default /mnt/videodata/rec)
//     REPLAY_PORT    (default 555)
// =============================================================================

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib-unix.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
// See onvif-recording-cgi.cpp for rationale on suppressing OpenSSL-3 deprecation.
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/md5.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

constexpr off_t MIN_PLAYABLE_SIZE = 1024;
constexpr std::size_t HEAD_BYTES  = 1024 * 1024;
constexpr std::size_t TAIL_BYTES  = 1024 * 1024;

// =============================================================================
// MD5 token (first 16 hex chars of MD5(filename))
// =============================================================================

std::string md5_token(const std::string& s) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
    char hex[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return std::string(hex, 16);
}

// =============================================================================
// Codec probe: scan first 1 MiB and last 1 MiB of the MP4 for a FourCC.
// Same heuristic as the Python detect_codec().
// =============================================================================

std::string detect_codec(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        g_print("[replay] open failed for %s: %s\n", path.c_str(), g_strerror(errno));
        return "";
    }
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) { ::close(fd); return ""; }

    std::string buf;
    // Head
    ::lseek(fd, 0, SEEK_SET);
    std::size_t head_n = std::min(static_cast<std::size_t>(size), HEAD_BYTES);
    buf.resize(head_n);
    ::read(fd, buf.data(), head_n);

    // Tail (only if file is bigger than head window)
    if (size > static_cast<off_t>(HEAD_BYTES)) {
        std::size_t tail_n = std::min<std::size_t>(static_cast<std::size_t>(size), TAIL_BYTES);
        std::string tail;
        tail.resize(tail_n);
        ::lseek(fd, size - static_cast<off_t>(tail_n), SEEK_SET);
        ::read(fd, tail.data(), tail_n);
        buf.append(tail);
    }
    ::close(fd);

    auto contains = [&](const char* needle) {
        return buf.find(needle) != std::string::npos;
    };
    if (contains("hvc1") || contains("hev1")) return "h265";
    if (contains("avc1") || contains("avc3")) return "h264";
    return "";
}

// =============================================================================
// Build the H.264/H.265 RTP pipeline for a file.
// =============================================================================

std::string build_pipeline(const std::string& path, const std::string& codec) {
    std::string parser = (codec == "h265") ? "h265parse" : "h264parse";
    std::string payer  = (codec == "h265") ? "rtph265pay" : "rtph264pay";
    return "filesrc location=\"" + path + "\" ! "
           "qtdemux name=demux "
           "demux.video_0 ! queue ! " + parser + " config-interval=-1 ! " +
           payer + " name=pay0 pt=96 config-interval=1";
}

void add_factory(GstRTSPMountPoints* mounts, const std::string& mount_path,
                 const std::string& pipeline_str) {
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    // gst-launch-style pipelines must be wrapped in (...) for media-factory.
    std::string launch = "( " + pipeline_str + " )";
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    // Per-client pipelines (Profile G replay isn't shareable mid-stream).
    gst_rtsp_media_factory_set_shared(factory, FALSE);
    gst_rtsp_mount_points_add_factory(mounts, mount_path.c_str(), factory);
    g_print("[replay] Mounted %s -> %s\n", mount_path.c_str(), pipeline_str.c_str());
    fflush(stdout);
}

// =============================================================================
// Mount every playable MP4 found at startup.
// =============================================================================

int mount_all(GstRTSPMountPoints* mounts, const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) {
        g_print("[replay] WARNING: Recording directory not found: %s\n", dir.c_str());
        return 0;
    }
    std::vector<std::string> files;
    while (struct dirent* de = ::readdir(d)) {
        std::string n = de->d_name;
        if (n.size() < 4 || n.substr(n.size() - 4) != ".mp4") continue;
        files.push_back(std::move(n));
    }
    ::closedir(d);
    std::sort(files.begin(), files.end());

    int mounted = 0, skipped = 0;
    for (const std::string& fname : files) {
        std::string path = dir + "/" + fname;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || st.st_size < MIN_PLAYABLE_SIZE) {
            ++skipped; continue;
        }
        std::string codec = detect_codec(path);
        if (codec.empty()) {
            ++skipped;
            g_print("[replay] no codec detected for %s, skipping\n", path.c_str());
            continue;
        }
        std::string token = md5_token(fname);
        std::string pipeline = build_pipeline(path, codec);

        add_factory(mounts, "/replay/" + token, pipeline);
        add_factory(mounts, "/replay?token=" + token, pipeline);
        ++mounted;
    }
    g_print("[replay] Pre-mounted %d recordings, skipped %d\n", mounted, skipped);
    fflush(stdout);
    return mounted;
}

// =============================================================================
// Signals
// =============================================================================

gboolean on_signal(gpointer user_data) {
    GMainLoop* loop = static_cast<GMainLoop*>(user_data);
    g_print("[replay] Caught termination signal — shutting down\n");
    fflush(stdout);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

} // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    std::string rec_dir   = env_or("RECORDING_DIR", "/mnt/videodata/rec");
    std::string port      = env_or("REPLAY_PORT",   "555");

    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server, port.c_str());

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    mount_all(mounts, rec_dir);
    g_object_unref(mounts);

    if (gst_rtsp_server_attach(server, nullptr) == 0) {
        g_printerr("Failed to attach RTSP server to default main context\n");
        return 1;
    }
    g_print("[replay] Replay RTSP server ready on port %s\n", port.c_str());
    g_print("[replay] Recording directory: %s\n", rec_dir.c_str());
    fflush(stdout);

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_unix_signal_add(SIGINT,  on_signal, loop);

    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(server);
    return 0;
}
