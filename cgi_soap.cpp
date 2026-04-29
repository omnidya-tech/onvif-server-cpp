// =============================================================================
// cgi_soap.cpp -- Implementation of shared SOAP/XML helpers.
// =============================================================================

#include "cgi_soap.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>

#include <unistd.h>

namespace cgi {

// -----------------------------------------------------------------------------
// stdin reading
// -----------------------------------------------------------------------------

std::string read_stdin(std::size_t n) {
    std::string out;
    out.resize(n);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(0, out.data() + got, n - got);
        if (r <= 0) {
            out.resize(got);
            return out;
        }
        got += static_cast<std::size_t>(r);
    }
    return out;
}

std::string read_request_body() {
    const char* cl = std::getenv("CONTENT_LENGTH");
    if (!cl || !*cl) return "";
    char* end = nullptr;
    long n = std::strtol(cl, &end, 10);
    if (n <= 0) return "";
    return read_stdin(static_cast<std::size_t>(n));
}

// -----------------------------------------------------------------------------
// Response writing
// -----------------------------------------------------------------------------

void soap_response(const std::string& extra_xmlns, const std::string& body_xml) {
    std::string xml;
    xml.reserve(256 + body_xml.size() + extra_xmlns.size());
    xml.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"");
    xml.append(extra_xmlns);
    xml.append("><s:Body>");
    xml.append(body_xml);
    xml.append("</s:Body></s:Envelope>");

    // CGI headers — \r\n required, separator blank line then body.
    std::cout << "Content-Type: application/soap+xml; charset=utf-8\r\n";
    std::cout << "Content-Length: " << xml.size() << "\r\n";
    std::cout << "\r\n";
    std::cout.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    std::cout.flush();
}

void soap_fault(const std::string& extra_xmlns,
                const std::string& code,
                const std::string& reason) {
    std::string body;
    body.reserve(128 + code.size() + reason.size());
    body.append("<s:Fault><s:Code><s:Value>");
    body.append(xml_escape(code));
    body.append("</s:Value></s:Code><s:Reason><s:Text xml:lang=\"en\">");
    body.append(xml_escape(reason));
    body.append("</s:Text></s:Reason></s:Fault>");
    soap_response(extra_xmlns, body);
}

// -----------------------------------------------------------------------------
// XML parsing
// -----------------------------------------------------------------------------

XmlDoc parse_xml(const std::string& body) {
    XmlDoc d;
    d.doc = xmlReadMemory(body.data(),
                          static_cast<int>(body.size()),
                          "request.xml", nullptr,
                          XML_PARSE_NOENT | XML_PARSE_NOBLANKS);
    return d;
}

xmlNodePtr soap_body_action(xmlDocPtr doc) {
    if (!doc) return nullptr;
    xmlNodePtr env = xmlDocGetRootElement(doc);
    if (!env) return nullptr;

    // Find <s:Body> (any namespace prefix; we match by ns_href + local-name).
    for (xmlNodePtr n = env->children; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(n->name, BAD_CAST "Body") != 0) continue;
        if (n->ns && n->ns->href &&
            xmlStrcmp(n->ns->href, BAD_CAST NS_SOAP) != 0) continue;

        // Return first element child.
        for (xmlNodePtr c = n->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE) return c;
        }
        return nullptr;
    }
    return nullptr;
}

std::string local_name(xmlNodePtr node) {
    if (!node || !node->name) return "";
    return reinterpret_cast<const char*>(node->name);
}

xmlNodePtr find_element(xmlNodePtr root, const char* local, const char* ns_href) {
    if (!root || !local) return nullptr;
    // BFS-ish: search the descendants in document order.
    for (xmlNodePtr n = root; n; n = n->next) {
        if (n->type == XML_ELEMENT_NODE && n->name &&
            xmlStrcmp(n->name, BAD_CAST local) == 0) {
            if (!ns_href || !*ns_href ||
                (n->ns && n->ns->href &&
                 xmlStrcmp(n->ns->href, BAD_CAST ns_href) == 0)) {
                return n;
            }
        }
        if (n->children) {
            xmlNodePtr hit = find_element(n->children, local, ns_href);
            if (hit) return hit;
        }
    }
    return nullptr;
}

std::string text_of(xmlNodePtr node) {
    if (!node) return "";
    xmlChar* s = xmlNodeGetContent(node);
    if (!s) return "";
    std::string out(reinterpret_cast<const char*>(s));
    xmlFree(s);

    // Trim ASCII whitespace.
    auto isws = [](unsigned char c){ return std::isspace(c); };
    while (!out.empty() && isws(static_cast<unsigned char>(out.back())))  out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && isws(static_cast<unsigned char>(out[start]))) ++start;
    return out.substr(start);
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

void ActionTable::dispatch() {
    std::string body = read_request_body();
    if (body.empty()) {
        soap_fault(fault_xmlns, "s:Sender", "Empty request");
        return;
    }
    XmlDoc d = parse_xml(body);
    if (!d) {
        soap_fault(fault_xmlns, "s:Sender", "Malformed XML");
        return;
    }
    xmlNodePtr action = soap_body_action(d.doc);
    if (!action) {
        soap_fault(fault_xmlns, "s:Sender", "Missing or empty SOAP Body");
        return;
    }
    std::string name = local_name(action);
    auto it = handlers.find(name);
    if (it == handlers.end()) {
        soap_fault(fault_xmlns, "s:Sender", "Action not supported: " + name);
        return;
    }
    it->second(d.doc);
}

// -----------------------------------------------------------------------------
// Misc utilities
// -----------------------------------------------------------------------------

std::string epoch_to_onvif(std::int64_t epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    struct tm gm;
    gmtime_r(&t, &gm);
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday,
                  gm.tm_hour, gm.tm_min, gm.tm_sec);
    return buf;
}

std::int64_t onvif_to_epoch(const std::string& s_in) {
    // Strip trailing 'Z' and any fractional-seconds portion.
    std::string s = s_in;
    if (!s.empty() && s.back() == 'Z') s.pop_back();
    auto dot = s.find('.');
    if (dot != std::string::npos) s.erase(dot);

    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                    &Y, &M, &D, &h, &m, &sec) != 6) return 0;

    struct tm tmv{};
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = sec;
    // timegm interprets tmv as UTC (matches Python's tzinfo=UTC + .timestamp()).
    std::time_t epoch = timegm(&tmv);
    if (epoch == static_cast<std::time_t>(-1)) return 0;
    return static_cast<std::int64_t>(epoch);
}

std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out.append("&amp;");  break;
            case '<':  out.append("&lt;");   break;
            case '>':  out.append("&gt;");   break;
            case '"':  out.append("&quot;"); break;
            case '\'': out.append("&apos;"); break;
            default:   out.push_back(c);     break;
        }
    }
    return out;
}

std::string get_client_ip() {
    if (const char* sa = std::getenv("SERVER_ADDR"); sa && *sa) return sa;
    if (const char* hh = std::getenv("HTTP_HOST"); hh && *hh) {
        std::string s = hh;
        auto colon = s.find(':');
        if (colon != std::string::npos) s.erase(colon);
        return s;
    }
    return "192.168.20.2";
}

} // namespace cgi
