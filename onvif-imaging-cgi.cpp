// =============================================================================
// onvif-imaging-cgi.cpp -- C++17 port of onvif-imaging-cgi.py.
//
// ONVIF Imaging Service CGI — Profile T. Drives the VeriSilicon ISP on
// IMX8MP via the isp_ctrl helper (JSON over viv_ext_ctrl V4L2 control).
//
// Behavior is identical to the Python: same SOAP envelope, same ONVIF↔VIV
// mapping for brightness/contrast/saturation, same ISP commands.
// =============================================================================

#include "cgi_soap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

// Imaging xmlns block for envelope responses + faults.
const std::string kXmlns =
    " xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"";

// =============================================================================
// Minimal JSON value parser for isp_ctrl responses.
//
// isp_ctrl emits flat objects like {"brightness": 0, "contrast": 1.0,
// "enable": true, "min": 100, "max": 40000}. No nested objects, no arrays
// of arrays. A 200-line shallow parser is plenty.
// =============================================================================

struct JsonValue {
    enum Kind { Null, Bool, Number, String };
    Kind kind = Null;
    double number = 0.0;
    bool   boolean = false;
    std::string str;

    bool is_number() const { return kind == Number; }
    bool is_bool()   const { return kind == Bool; }
    double as_number(double def = 0.0) const { return is_number() ? number : def; }
    bool   as_bool(bool def = false) const   { return is_bool() ? boolean : def; }
    std::string as_string(const std::string& def = "") const {
        if (kind == String) return str;
        if (kind == Number) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", number);
            return buf;
        }
        return def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse_object(std::map<std::string, JsonValue>& out) {
        skip_ws();
        if (peek() != '{') return false;
        ++pos_;
        skip_ws();
        if (peek() == '}') { ++pos_; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (peek() != ':') return false;
            ++pos_;
            skip_ws();
            JsonValue v;
            if (!parse_value(v)) return false;
            out[key] = std::move(v);
            skip_ws();
            char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return true; }
            return false;
        }
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    void skip_ws() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }

    bool parse_value(JsonValue& v) {
        skip_ws();
        char c = peek();
        if (c == '"')      return parse_string(v.str) ? (v.kind = JsonValue::String, true) : false;
        if (c == 't' || c == 'f') return parse_bool(v);
        if (c == 'n')      return parse_null(v);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(v);
        // Fallback: skip nested objects/arrays as opaque strings.
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{' ? '}' : ']');
            int depth = 1; ++pos_;
            while (pos_ < s_.size() && depth > 0) {
                char x = s_[pos_++];
                if (x == open) ++depth;
                else if (x == close) --depth;
                else if (x == '"') {
                    while (pos_ < s_.size() && s_[pos_] != '"') {
                        if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) pos_ += 2;
                        else ++pos_;
                    }
                    if (pos_ < s_.size()) ++pos_;
                }
            }
            v.kind = JsonValue::String;
            return true;
        }
        return false;
    }

    bool parse_string(std::string& out) {
        if (peek() != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c == '\\' && pos_ < s_.size()) {
                char esc = s_[pos_++];
                switch (esc) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    default:   out.push_back(esc);  break;
                }
            } else {
                out.push_back(c);
            }
        }
        if (pos_ < s_.size() && s_[pos_] == '"') { ++pos_; return true; }
        return false;
    }

    bool parse_bool(JsonValue& v) {
        if (s_.compare(pos_, 4, "true") == 0)  { pos_ += 4; v.kind = JsonValue::Bool; v.boolean = true;  return true; }
        if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.kind = JsonValue::Bool; v.boolean = false; return true; }
        return false;
    }

    bool parse_null(JsonValue& v) {
        if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; v.kind = JsonValue::Null; return true; }
        return false;
    }

    bool parse_number(JsonValue& v) {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                                    s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
        if (pos_ == start) return false;
        v.kind = JsonValue::Number;
        v.number = std::strtod(s_.c_str() + start, nullptr);
        return true;
    }
};

// =============================================================================
// Run isp_ctrl with a JSON command and return the parsed response.
// =============================================================================

const std::string& isp_ctrl_path() {
    static const std::string p = env_or("ISP_CTRL", "/usr/bin/isp_ctrl");
    return p;
}
const std::string& imaging_video_dev() {
    static const std::string p = env_or("IMAGING_VIDEO_DEV", "/dev/video2");
    return p;
}
int isp_stream_id() {
    static const int id = std::atoi(env_or("ISP_STREAM_ID", "0"));
    return id;
}

// Serialize a small key-to-JsonValue map (Number/Bool/String only) to JSON.
std::string serialize_cmd(const std::map<std::string, JsonValue>& cmd) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : cmd) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"'); out.append(k); out.append("\":");
        switch (v.kind) {
            case JsonValue::Number: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6g", v.number);
                out.append(buf);
                break;
            }
            case JsonValue::Bool:
                out.append(v.boolean ? "true" : "false");
                break;
            case JsonValue::String:
                out.push_back('"');
                for (char c : v.str) {
                    if (c == '"' || c == '\\') out.push_back('\\');
                    out.push_back(c);
                }
                out.push_back('"');
                break;
            default:
                out.append("null");
                break;
        }
    }
    out.push_back('}');
    return out;
}

// Run isp_ctrl with [video_dev, json_cmd]. Returns parsed JSON object on
// success, empty map on any failure (matches Python's broad except).
std::map<std::string, JsonValue>
isp_cmd(std::map<std::string, JsonValue> cmd) {
    cmd.try_emplace("streamid", JsonValue{JsonValue::Number, double(isp_stream_id()), false, {}});
    std::string json_cmd = serialize_cmd(cmd);

    int pipefd[2];
    if (::pipe(pipefd) != 0) return {};

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        // Child: stdout → pipe, stderr → /dev/null
        ::dup2(pipefd[1], 1);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, 2); ::close(devnull); }
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // 5-second alarm — matches subprocess.check_output(timeout=5).
        ::alarm(5);
        ::execl(isp_ctrl_path().c_str(),
                isp_ctrl_path().c_str(),
                imaging_video_dev().c_str(),
                json_cmd.c_str(),
                nullptr);
        _exit(127);
    }

    ::close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t r;
    while ((r = ::read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(r));
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    // Trim
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    if (out.empty()) return {};

    JsonParser p(out);
    std::map<std::string, JsonValue> resp;
    if (!p.parse_object(resp)) return {};
    return resp;
}

// Convenience: build a typed JsonValue.
JsonValue jv_num(double x)        { return {JsonValue::Number, x, false, {}}; }
JsonValue jv_bool(bool b)         { return {JsonValue::Bool, 0.0, b, {}}; }
JsonValue jv_str(const std::string& s){ JsonValue v; v.kind=JsonValue::String; v.str=s; return v; }

// =============================================================================
// Handlers
// =============================================================================

std::map<std::string, JsonValue> isp_get_cproc()    { return isp_cmd({{"id", jv_str("cproc.g.cfg")}}); }
std::map<std::string, JsonValue> isp_get_exposure() { return isp_cmd({{"id", jv_str("ec.g.cfg")}}); }
std::map<std::string, JsonValue> isp_get_ae()       { return isp_cmd({{"id", jv_str("ae.g.cfg")}}); }

void handle_get_imaging_settings(xmlDocPtr) {
    auto cproc = isp_get_cproc();
    auto ec    = isp_get_exposure();
    auto ae    = isp_get_ae();

    double brightness = cproc.count("brightness") ? cproc["brightness"].as_number(0)   : 0;
    double contrast   = cproc.count("contrast")   ? cproc["contrast"].as_number(1.0)   : 1.0;
    double saturation = cproc.count("saturation") ? cproc["saturation"].as_number(1.0) : 1.0;
    double hue        = cproc.count("hue")        ? cproc["hue"].as_number(0.0)        : 0.0;

    int onvif_brightness = static_cast<int>(brightness) + 128;
    int onvif_contrast   = static_cast<int>(contrast * 128);
    int onvif_saturation = static_cast<int>(saturation * 128);
    int onvif_sharpness  = 128;

    double gain = ec.count("gain") ? ec["gain"].as_number(0) : 0;
    double exp_time = ec.count("time") ? ec["time"].as_number(0) : 0;
    bool ae_enabled = ae.count("enable") ? ae["enable"].as_bool(true) : true;
    const char* exp_mode = ae_enabled ? "AUTO" : "MANUAL";

    std::ostringstream body;
    body << "<timg:GetImagingSettingsResponse>"
            "<timg:ImagingSettings>"
         << "<tt:Brightness>"      << onvif_brightness << "</tt:Brightness>"
         << "<tt:ColorSaturation>" << onvif_saturation << "</tt:ColorSaturation>"
         << "<tt:Contrast>"        << onvif_contrast   << "</tt:Contrast>"
         << "<tt:Sharpness>"       << onvif_sharpness  << "</tt:Sharpness>"
            "<tt:Exposure>"
         << "<tt:Mode>" << exp_mode << "</tt:Mode>"
         << "<tt:ExposureTime>" << exp_time << "</tt:ExposureTime>"
         << "<tt:Gain>" << gain << "</tt:Gain>"
            "</tt:Exposure>"
            "<tt:WhiteBalance><tt:Mode>AUTO</tt:Mode></tt:WhiteBalance>"
            "<tt:Extension>"
         << "<tt:SimpleItem Name=\"VIV_Brightness\" Value=\"" << brightness << "\"/>"
         << "<tt:SimpleItem Name=\"VIV_Contrast\" Value=\""   << contrast   << "\"/>"
         << "<tt:SimpleItem Name=\"VIV_Saturation\" Value=\"" << saturation << "\"/>"
         << "<tt:SimpleItem Name=\"VIV_Hue\" Value=\""        << hue        << "\"/>"
            "</tt:Extension>"
            "</timg:ImagingSettings>"
            "</timg:GetImagingSettingsResponse>";

    cgi::soap_response(kXmlns, body.str());
}

void handle_set_imaging_settings(xmlDocPtr doc) {
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr settings = cgi::find_element(root, "ImagingSettings", cgi::NS_TIMG);
    if (!settings) {
        cgi::soap_fault(kXmlns, "s:Receiver", "Missing ImagingSettings element");
        return;
    }

    std::map<std::string, JsonValue> cproc_args;

    if (xmlNodePtr el = cgi::find_element(settings->children, "Brightness", cgi::NS_TT)) {
        std::string t = cgi::text_of(el);
        if (!t.empty()) {
            int val = static_cast<int>(std::strtod(t.c_str(), nullptr)) - 128;
            if (val < -128) val = -128;
            if (val > 127)  val = 127;
            cproc_args["brightness"] = jv_num(val);
        }
    }
    if (xmlNodePtr el = cgi::find_element(settings->children, "Contrast", cgi::NS_TT)) {
        std::string t = cgi::text_of(el);
        if (!t.empty()) {
            double val = std::strtod(t.c_str(), nullptr) / 128.0;
            if (val < 0.0)  val = 0.0;
            if (val > 1.99) val = 1.99;
            // round to 3 decimals
            val = std::round(val * 1000.0) / 1000.0;
            cproc_args["contrast"] = jv_num(val);
        }
    }
    if (xmlNodePtr el = cgi::find_element(settings->children, "ColorSaturation", cgi::NS_TT)) {
        std::string t = cgi::text_of(el);
        if (!t.empty()) {
            double val = std::strtod(t.c_str(), nullptr) / 128.0;
            if (val < 0.0)  val = 0.0;
            if (val > 1.99) val = 1.99;
            val = std::round(val * 1000.0) / 1000.0;
            cproc_args["saturation"] = jv_num(val);
        }
    }
    if (!cproc_args.empty()) {
        cproc_args["id"] = jv_str("cproc.s.cfg");
        isp_cmd(cproc_args);
    }

    if (xmlNodePtr exp = cgi::find_element(settings->children, "Exposure", cgi::NS_TT)) {
        if (xmlNodePtr mode_el = cgi::find_element(exp->children, "Mode", cgi::NS_TT)) {
            std::string mode = cgi::text_of(mode_el);
            for (auto& c : mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            isp_cmd({{"id", jv_str("ae.s.en")}, {"enable", jv_bool(mode == "AUTO")}});
        }
        std::map<std::string, JsonValue> ec_args;
        if (xmlNodePtr el = cgi::find_element(exp->children, "ExposureTime", cgi::NS_TT)) {
            std::string t = cgi::text_of(el);
            if (!t.empty()) ec_args["time"] = jv_num(std::strtod(t.c_str(), nullptr));
        }
        if (xmlNodePtr el = cgi::find_element(exp->children, "Gain", cgi::NS_TT)) {
            std::string t = cgi::text_of(el);
            if (!t.empty()) ec_args["gain"] = jv_num(std::strtod(t.c_str(), nullptr));
        }
        if (!ec_args.empty()) {
            ec_args["id"] = jv_str("ec.s.cfg");
            isp_cmd(ec_args);
        }
    }

    cgi::soap_response(kXmlns, "<timg:SetImagingSettingsResponse/>");
}

void handle_get_options(xmlDocPtr) {
    auto ec = isp_get_exposure();
    double gain_min = ec.count("gain.min") ? ec["gain.min"].as_number(1.0)   : 1.0;
    double gain_max = ec.count("gain.max") ? ec["gain.max"].as_number(16.0)  : 16.0;
    double time_min = ec.count("inte.min") ? ec["inte.min"].as_number(100)   : 100;
    double time_max = ec.count("inte.max") ? ec["inte.max"].as_number(40000) : 40000;

    std::ostringstream body;
    body << "<timg:GetOptionsResponse><timg:ImagingOptions>"
            "<tt:Brightness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Brightness>"
            "<tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:ColorSaturation>"
            "<tt:Contrast><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Contrast>"
            "<tt:Sharpness><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Sharpness>"
            "<tt:Exposure>"
            "<tt:Mode><tt:Item>AUTO</tt:Item><tt:Item>MANUAL</tt:Item></tt:Mode>"
         << "<tt:MinExposureTime>" << time_min << "</tt:MinExposureTime>"
         << "<tt:MaxExposureTime>" << time_max << "</tt:MaxExposureTime>"
         << "<tt:MinGain>" << gain_min << "</tt:MinGain>"
         << "<tt:MaxGain>" << gain_max << "</tt:MaxGain>"
            "</tt:Exposure>"
            "<tt:WhiteBalance>"
            "<tt:Mode><tt:Item>AUTO</tt:Item><tt:Item>MANUAL</tt:Item></tt:Mode>"
            "</tt:WhiteBalance>"
            "</timg:ImagingOptions></timg:GetOptionsResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_move_options(xmlDocPtr) {
    cgi::soap_response(kXmlns, "<timg:GetMoveOptionsResponse/>");
}

void handle_get_status(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<timg:GetStatusResponse>"
        "<timg:Status>"
        "<tt:FocusStatus20>"
        "<tt:Position>0</tt:Position>"
        "<tt:MoveStatus>IDLE</tt:MoveStatus>"
        "</tt:FocusStatus20>"
        "</timg:Status>"
        "</timg:GetStatusResponse>");
}

} // namespace

#include <cmath> // for std::round

int main() {
    cgi::ActionTable t;
    t.fault_xmlns = kXmlns;
    t.on("GetImagingSettings", handle_get_imaging_settings);
    t.on("SetImagingSettings", handle_set_imaging_settings);
    t.on("GetOptions",         handle_get_options);
    t.on("GetMoveOptions",     handle_get_move_options);
    t.on("GetStatus",          handle_get_status);
    t.on("Move", [](xmlDocPtr){ cgi::soap_response(kXmlns, "<timg:MoveResponse/>"); });
    t.on("Stop", [](xmlDocPtr){ cgi::soap_response(kXmlns, "<timg:StopResponse/>"); });
    t.dispatch();
    return 0;
}
