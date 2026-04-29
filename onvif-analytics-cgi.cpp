// =============================================================================
// onvif-analytics-cgi.cpp -- C++17 port of onvif-analytics-cgi.py.
//
// ONVIF Analytics Service CGI — Profile M. Exposes the dashcam's DMS/ADAS
// AI detection modules (drowsiness, mobile, seatbelt, tailgating, ANPR,
// crash, driver-id) as ONVIF analytics modules. Recent-detection counts +
// timestamps are derived from files under $AI_DIR/<subdir>/.
// =============================================================================

#include "cgi_soap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::string kXmlns =
    " xmlns:tan=\"http://www.onvif.org/ver20/analytics/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"";

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

const std::string& ai_dir() {
    static const std::string p = env_or("AI_DIR", "/mnt/videodata/ai");
    return p;
}

struct Module {
    const char* token;
    const char* name;
    const char* type;
    const char* description;
    const char* ai_subdir;
};

const Module kModules[] = {
    {"MobilePhoneDetection",  "Mobile Phone Detection",
     "tt:CellMotionDetector", "Detects smartphone usage by driver (DMS)", "mobile"},
    {"SeatbeltDetection",     "Seatbelt Detection",
     "tt:CellMotionDetector", "Detects seatbelt compliance (DMS)",        "seatbelt"},
    {"DrowsinessDetection",   "Drowsiness Detection",
     "tt:CellMotionDetector", "Detects driver drowsiness and fatigue (DMS)", "drowsiness"},
    {"DriverIdentification",  "Driver Identification",
     "tt:CellMotionDetector", "Identifies driver via facial recognition (DMS)", "driver_id"},
    {"TailgatingDetection",   "Tailgating Detection",
     "tt:CellMotionDetector", "Monitors following distance to vehicle ahead (ADAS)", "tailgating"},
    {"ANPRDetection",         "License Plate Recognition",
     "tt:CellMotionDetector", "Automatic number plate recognition (ADAS)", "anpr"},
    {"CrashDetection",        "Crash Detection",
     "tt:CellMotionDetector", "Collision and impact detection via IMU (ADAS)", "crash"},
};

bool is_dms_token(const std::string& token) {
    static const std::set<std::string> dms = {
        "MobilePhoneDetection", "SeatbeltDetection",
        "DrowsinessDetection", "DriverIdentification"
    };
    return dms.count(token) > 0;
}

// Like Python's sorted(glob(<dir>/*)): returns count + epoch of last entry's mtime.
struct DirStats { int count = 0; std::int64_t last_mtime = 0; };
DirStats scan_dir(const std::string& dir) {
    DirStats s;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return s;
    std::vector<fs::directory_entry> entries;
    for (auto const& e : fs::directory_iterator(dir, ec)) entries.push_back(e);
    std::sort(entries.begin(), entries.end(),
              [](auto const& a, auto const& b) { return a.path().filename() < b.path().filename(); });
    s.count = static_cast<int>(entries.size());
    if (!entries.empty()) {
        struct stat st;
        if (::stat(entries.back().path().c_str(), &st) == 0) {
            s.last_mtime = static_cast<std::int64_t>(st.st_mtime);
        }
    }
    return s;
}

// =============================================================================
// Handlers
// =============================================================================

void handle_get_service_capabilities(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<tan:GetServiceCapabilitiesResponse>"
        "<tan:Capabilities RuleSupport=\"true\""
        " AnalyticsModuleSupport=\"true\""
        " CellBasedSceneDescriptionSupported=\"false\">"
        "</tan:Capabilities>"
        "</tan:GetServiceCapabilitiesResponse>");
}

void handle_get_analytics_engines(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<tan:GetAnalyticsEnginesResponse>"
        "<tan:AnalyticsEngine token=\"DMS_Engine\">"
        "<tt:Name>Driver Monitoring System</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "</tan:AnalyticsEngine>"
        "<tan:AnalyticsEngine token=\"ADAS_Engine\">"
        "<tt:Name>Advanced Driver Assistance</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "</tan:AnalyticsEngine>"
        "</tan:GetAnalyticsEnginesResponse>");
}

void handle_get_analytics_modules(xmlDocPtr) {
    std::ostringstream body;
    body << "<tan:GetAnalyticsModulesResponse>";
    for (const auto& m : kModules) {
        body << "<tan:AnalyticsModule token=\"" << m.token << "\">"
                "<tt:Name>" << m.name << "</tt:Name>"
                "<tt:Type>" << m.type << "</tt:Type>"
                "<tt:Parameters>"
                "<tt:SimpleItem Name=\"Enabled\" Value=\"true\"/>"
                "<tt:SimpleItem Name=\"Description\" Value=\""
             << cgi::xml_escape(m.description) << "\"/>"
                "<tt:SimpleItem Name=\"AIDirectory\" Value=\""
             << cgi::xml_escape(ai_dir() + "/" + m.ai_subdir) << "\"/>"
                "</tt:Parameters>"
                "</tan:AnalyticsModule>";
    }
    body << "</tan:GetAnalyticsModulesResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_supported_analytics_modules(xmlDocPtr) {
    std::ostringstream body;
    body << "<tan:GetSupportedAnalyticsModulesResponse>";
    for (const auto& m : kModules) {
        body << "<tan:SupportedAnalyticsModule>"
                "<tt:Type>" << m.type << "</tt:Type>"
                "<tt:Name>" << m.name << "</tt:Name>"
                "<tt:Parameters>"
                "<tt:SimpleItem Name=\"Token\" Value=\"" << m.token << "\"/>"
                "<tt:SimpleItem Name=\"Category\" Value=\""
             << (is_dms_token(m.token) ? "DMS" : "ADAS") << "\"/>"
                "</tt:Parameters>"
                "</tan:SupportedAnalyticsModule>";
    }
    body << "</tan:GetSupportedAnalyticsModulesResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_analytics_engine_inputs(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<tan:GetAnalyticsEngineInputsResponse>"
        "<tan:AnalyticsEngineInput token=\"DMS_Input\">"
        "<tt:SourceToken>VideoSource_inside</tt:SourceToken>"
        "<tt:Name>Interior Camera (DMS)</tt:Name>"
        "</tan:AnalyticsEngineInput>"
        "<tan:AnalyticsEngineInput token=\"ADAS_Input\">"
        "<tt:SourceToken>VideoSource_front</tt:SourceToken>"
        "<tt:Name>Front Camera (ADAS)</tt:Name>"
        "</tan:AnalyticsEngineInput>"
        "</tan:GetAnalyticsEngineInputsResponse>");
}

void handle_get_rules(xmlDocPtr) {
    std::ostringstream body;
    body << "<tan:GetRulesResponse>";
    for (const auto& m : kModules) {
        body << "<tan:Rule token=\"Rule_" << m.token << "\">"
                "<tt:Name>" << m.name << " Rule</tt:Name>"
                "<tt:Type>" << m.type << "</tt:Type>"
                "<tt:Parameters>"
                "<tt:SimpleItem Name=\"Sensitivity\" Value=\"50\"/>"
                "</tt:Parameters>"
                "</tan:Rule>";
    }
    body << "</tan:GetRulesResponse>";
    cgi::soap_response(kXmlns, body.str());
}

void handle_get_supported_rules(xmlDocPtr) {
    cgi::soap_response(kXmlns,
        "<tan:GetSupportedRulesResponse>"
        "<tan:SupportedRules>"
        "<tt:RuleDescription Name=\"CellMotionDetector\">"
        "<tt:Parameters>"
        "<tt:SimpleItemDescription Name=\"Sensitivity\" Type=\"xs:integer\"/>"
        "</tt:Parameters>"
        "</tt:RuleDescription>"
        "</tan:SupportedRules>"
        "</tan:GetSupportedRulesResponse>");
}

void handle_get_analytics_state(xmlDocPtr) {
    std::ostringstream body;
    body << "<tan:GetAnalyticsStateResponse>";
    for (const auto& m : kModules) {
        std::string subdir = ai_dir() + "/" + m.ai_subdir;
        DirStats stats = scan_dir(subdir);
        std::string latest = stats.last_mtime ? cgi::epoch_to_onvif(stats.last_mtime) : "";

        body << "<tan:AnalyticsState token=\"" << m.token << "\">"
                "<tt:Name>" << m.name << "</tt:Name>"
                "<tt:Parameters>"
                "<tt:SimpleItem Name=\"Active\" Value=\"true\"/>"
                "<tt:SimpleItem Name=\"DetectionCount\" Value=\"" << stats.count << "\"/>"
                "<tt:SimpleItem Name=\"LastDetection\" Value=\"" << latest << "\"/>"
                "</tt:Parameters>"
                "</tan:AnalyticsState>";
    }
    body << "</tan:GetAnalyticsStateResponse>";
    cgi::soap_response(kXmlns, body.str());
}

} // namespace

int main() {
    cgi::ActionTable t;
    t.fault_xmlns = kXmlns;
    t.on("GetServiceCapabilities",       handle_get_service_capabilities);
    t.on("GetAnalyticsEngines",          handle_get_analytics_engines);
    t.on("GetAnalyticsModules",          handle_get_analytics_modules);
    t.on("GetSupportedAnalyticsModules", handle_get_supported_analytics_modules);
    t.on("GetAnalyticsEngineInputs",     handle_get_analytics_engine_inputs);
    t.on("GetRules",                     handle_get_rules);
    t.on("GetSupportedRules",            handle_get_supported_rules);
    t.on("GetAnalyticsState",            handle_get_analytics_state);
    t.dispatch();
    return 0;
}
