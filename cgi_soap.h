// =============================================================================
// cgi_soap.h -- Shared SOAP/XML helpers for the ONVIF CGI binaries.
//
// Each CGI:
//   1. reads CONTENT_LENGTH env var
//   2. reads exactly that many bytes of SOAP request from stdin
//   3. parses the XML, finds the SOAP Body's first child (the action)
//   4. dispatches based on the action's local-name
//   5. writes a SOAP envelope to stdout with HTTP CGI headers
//
// This header centralises that boilerplate so each CGI's `main()` is just
// a dispatch table.
// =============================================================================

#pragma once

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cgi {

// ONVIF / SOAP namespace URIs (kept identical to the Python NS dicts).
constexpr const char* NS_SOAP = "http://www.w3.org/2003/05/soap-envelope";
constexpr const char* NS_TT   = "http://www.onvif.org/ver10/schema";
constexpr const char* NS_TIMG = "http://www.onvif.org/ver20/imaging/wsdl";
constexpr const char* NS_TAN  = "http://www.onvif.org/ver20/analytics/wsdl";
constexpr const char* NS_TRC  = "http://www.onvif.org/ver10/recording/wsdl";
constexpr const char* NS_TSE  = "http://www.onvif.org/ver10/search/wsdl";
constexpr const char* NS_TRP  = "http://www.onvif.org/ver10/replay/wsdl";

// -----------------------------------------------------------------------------
// Request reading
// -----------------------------------------------------------------------------

// Read exactly `n` bytes from stdin. Returns "" on EOF / error before n bytes.
std::string read_stdin(std::size_t n);

// Convenience: read CONTENT_LENGTH worth of stdin. Returns "" if env missing.
std::string read_request_body();

// -----------------------------------------------------------------------------
// Response writing
// -----------------------------------------------------------------------------

// Write a SOAP response with `body_xml` injected into the <s:Body>. The
// envelope's xmlns declarations are passed in `extra_xmlns` (e.g. " xmlns:trc=\"...\"").
// Output goes to stdout with proper Content-Type / Content-Length CGI headers.
void soap_response(const std::string& extra_xmlns, const std::string& body_xml);

// Convenience: SOAP fault.
void soap_fault(const std::string& extra_xmlns,
                const std::string& code,
                const std::string& reason);

// -----------------------------------------------------------------------------
// XML parsing
// -----------------------------------------------------------------------------

// Wrapper that owns an xmlDocPtr.
struct XmlDoc {
    xmlDocPtr doc = nullptr;
    XmlDoc() = default;
    ~XmlDoc() { if (doc) xmlFreeDoc(doc); }
    XmlDoc(const XmlDoc&) = delete;
    XmlDoc& operator=(const XmlDoc&) = delete;
    XmlDoc(XmlDoc&& o) noexcept : doc(o.doc) { o.doc = nullptr; }
    XmlDoc& operator=(XmlDoc&& o) noexcept { if (&o!=this){ if(doc)xmlFreeDoc(doc); doc=o.doc; o.doc=nullptr;} return *this; }
    explicit operator bool() const { return doc != nullptr; }
};

// Parse a SOAP request body. Returns empty XmlDoc on parse error.
XmlDoc parse_xml(const std::string& body);

// Find the SOAP <s:Body> in the parsed document, then return its first child
// element node (the "action" element). Returns nullptr on failure.
xmlNodePtr soap_body_action(xmlDocPtr doc);

// Get the local-name of an element (strip namespace).
std::string local_name(xmlNodePtr node);

// Find first descendant of `root` with the given local name and namespace href.
// `ns_href` may be null/empty to ignore namespace.
xmlNodePtr find_element(xmlNodePtr root, const char* local, const char* ns_href);

// Convenience: text content of an element, trimmed. Returns "" if missing/null.
std::string text_of(xmlNodePtr node);

// -----------------------------------------------------------------------------
// Dispatch table helper
// -----------------------------------------------------------------------------

using ActionHandler = std::function<void(xmlDocPtr root)>;

struct ActionTable {
    std::map<std::string, ActionHandler> handlers;
    std::string fault_xmlns;  // namespace decls for fault responses

    void on(const std::string& action, ActionHandler h) {
        handlers[action] = std::move(h);
    }

    // Dispatch the request body. Reads from stdin, parses, finds action, runs
    // the matching handler. Emits soap_fault() on any error.
    void dispatch();
};

// -----------------------------------------------------------------------------
// Misc utilities (mirrors Python helpers)
// -----------------------------------------------------------------------------

// Convert Unix epoch to ONVIF "%Y-%m-%dT%H:%M:%SZ" UTC string.
std::string epoch_to_onvif(std::int64_t epoch);

// Convert ONVIF datetime back to Unix epoch. Returns 0 on parse error.
std::int64_t onvif_to_epoch(const std::string& s);

// XML attribute escape (just &, <, >, ", ')
std::string xml_escape(const std::string& s);

// Get the IP the client used to reach us (SERVER_ADDR env, falls back to
// HTTP_HOST without :port, then to a hardcoded default).
std::string get_client_ip();

} // namespace cgi
