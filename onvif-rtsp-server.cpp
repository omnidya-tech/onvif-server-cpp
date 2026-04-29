// =============================================================================
// onvif-rtsp-server.cpp -- C++17 port of onvif-rtsp-server.py
//
// Drop-in replacement for the Python RTSP-server. Same RTSP mount paths,
// same env-var contract, same /tmp/onvif-rtsp-active flag semantics, same
// pipeline. Compatible with onvif-simple-server profile URLs and with the
// dashcam-service C++ OnvifActivityWatcher.
//
// Lazy-flag behavior (mirrors the Python):
//   - Flag created on first DESCRIBE (counted per-client by GstRTSPClient*).
//   - Flag removed when the last counted client closes.
//   - OPTIONS-only probes (the watchdog) never count → no spurious flag toggle.
//   - Repeated DESCRIBEs from the same client do NOT toggle the flag.
//
// Trigger is `pre-describe-request` (NOT `describe-request`) because:
//   1. Pre-describe fires BEFORE GstRTSPServer prerolls the media pipeline.
//      This gives the C++ dashcam-service watcher (poll=500ms) time to open
//      its streaming valve while preroll waits for shmsrc caps.
//   2. With describe-request as the trigger, preroll would stall (valve
//      still closed) and the signal would never fire.
//
// Env overrides via /etc/default/onvif-rtsp:
//   SHM_PATH         (/tmp/front.raw)
//   SHM_PATH_INSIDE  (/tmp/inside.raw)
//   RTSP_PORT        (554)
//   RTSP_PATH        (/stream)
//   RTSP_PATH_INSIDE (/stream_inside)
// =============================================================================

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/rtsp/gstrtspdefs.h>
#include <glib-unix.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr const char* FLAG_FILE = "/tmp/onvif-rtsp-active";

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

void create_flag() {
    int fd = ::open(FLAG_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd >= 0) {
        ::close(fd);
    } else {
        g_print("[FLAG] failed to create %s: %s\n", FLAG_FILE, g_strerror(errno));
        fflush(stdout);
    }
}

void remove_flag() {
    if (::unlink(FLAG_FILE) != 0 && errno != ENOENT) {
        g_print("[FLAG] failed to remove %s: %s\n", FLAG_FILE, g_strerror(errno));
        fflush(stdout);
    }
}

// =============================================================================
// ViewerCounter -- tracks unique RTSP clients via DESCRIBE/closed and toggles
// FLAG_FILE on 0↔1 transitions. A client is counted at most once between its
// first DESCRIBE and its `closed` signal. OPTIONS-only probes never trigger.
// =============================================================================
class ViewerCounter {
public:
    void on_describe(GstRTSPClient* client) {
        bool fire_create = false;
        int count_now;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (counted_.count(client)) return;
            counted_.insert(client);
            ++count_;
            if (count_ == 1) fire_create = true;
            count_now = count_;
        }
        if (fire_create) {
            create_flag();
            g_print("[FLAG] +client (count=%d) → /tmp/onvif-rtsp-active SET\n",
                    count_now);
        } else {
            g_print("[FLAG] +client (count=%d)\n", count_now);
        }
        fflush(stdout);
    }

    void on_closed(GstRTSPClient* client) {
        bool fire_remove = false;
        int count_now;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = counted_.find(client);
            if (it == counted_.end()) return; // never DESCRIBE'd (probe-only)
            counted_.erase(it);
            --count_;
            if (count_ <= 0) {
                count_ = 0;
                fire_remove = true;
            }
            count_now = count_;
        }
        if (fire_remove) {
            remove_flag();
            g_print("[FLAG] -client (count=%d) → /tmp/onvif-rtsp-active CLEARED\n",
                    count_now);
        } else {
            g_print("[FLAG] -client (count=%d)\n", count_now);
        }
        fflush(stdout);
    }

private:
    std::mutex mutex_;
    int count_ = 0;
    std::unordered_set<GstRTSPClient*> counted_;
};

// =============================================================================
// GObject signal handlers
// =============================================================================

// Returns GST_RTSP_STS_OK so the accumulator on `pre-describe-request`
// allows the request through (returning 0/INVALID would block DESCRIBE).
GstRTSPStatusCode on_pre_describe(GstRTSPClient* client,
                                  GstRTSPContext* /*ctx*/,
                                  gpointer user_data) {
    ViewerCounter* counter = static_cast<ViewerCounter*>(user_data);
    counter->on_describe(client);
    return GST_RTSP_STS_OK;
}

void on_client_closed(GstRTSPClient* client, gpointer user_data) {
    ViewerCounter* counter = static_cast<ViewerCounter*>(user_data);
    counter->on_closed(client);
}

void on_client_connected(GstRTSPServer* /*server*/,
                         GstRTSPClient*  client,
                         gpointer user_data) {
    // Same wiring as the Python: pre-describe-request before media preroll,
    // closed when the client tears down. The watchdog's OPTIONS probe does
    // NOT fire pre-describe, so it never bumps the count.
    g_signal_connect(client, "pre-describe-request",
                     G_CALLBACK(on_pre_describe), user_data);
    g_signal_connect(client, "closed",
                     G_CALLBACK(on_client_closed), user_data);
}

// =============================================================================
// Pipeline / mount construction
// =============================================================================

std::string build_pipeline(const std::string& shm_path) {
    return "( shmsrc socket-path=" + shm_path + " is-live=true do-timestamp=true ! "
           "queue max-size-buffers=3 leaky=downstream ! "
           "h264parse config-interval=-1 ! "
           "rtph264pay name=pay0 pt=96 config-interval=1 )";
}

void add_mount(GstRTSPServer* server, const char* label,
               const char* shm_path, const char* mount_path) {
    std::string pipeline = build_pipeline(shm_path);

    GstRTSPMountPoints*  mounts  = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();

    gst_rtsp_media_factory_set_launch(factory, pipeline.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    gst_rtsp_mount_points_add_factory(mounts, mount_path, factory);
    g_object_unref(mounts);

    g_print("[%s] mount %s -> %s\n", label, mount_path, pipeline.c_str());
    fflush(stdout);
}

// =============================================================================
// SIGTERM/SIGINT handler
// =============================================================================

gboolean on_signal(gpointer user_data) {
    GMainLoop* loop = static_cast<GMainLoop*>(user_data);
    g_print("Caught termination signal — shutting down\n");
    fflush(stdout);
    remove_flag();
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

} // namespace

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    const char* shm_front  = env_or("SHM_PATH",         "/tmp/front.raw");
    const char* shm_inside = env_or("SHM_PATH_INSIDE",  "/tmp/inside.raw");
    const char* port       = env_or("RTSP_PORT",        "554");
    const char* path_front = env_or("RTSP_PATH",        "/stream");
    const char* path_inside= env_or("RTSP_PATH_INSIDE", "/stream_inside");

    // Defensive: clear any stale flag from an unclean prior exit so the C++
    // watcher starts in a known state.
    remove_flag();
    std::atexit(remove_flag);

    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server, port);

    add_mount(server, "front",  shm_front,  path_front);
    add_mount(server, "inside", shm_inside, path_inside);

    ViewerCounter counter;
    g_signal_connect(server, "client-connected",
                     G_CALLBACK(on_client_connected), &counter);

    if (gst_rtsp_server_attach(server, nullptr) == 0) {
        g_printerr("Failed to attach RTSP server to default main context\n");
        remove_flag();
        return 1;
    }

    g_print("RTSP server listening on port %s (lazy flag mode)\n", port);
    fflush(stdout);

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_unix_signal_add(SIGINT,  on_signal, loop);

    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(server);
    remove_flag();
    return 0;
}
