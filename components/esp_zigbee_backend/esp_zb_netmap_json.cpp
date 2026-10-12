// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap_json -- see the header.
//
// Strings from the pool go in as std::string: ArduinoJson keeps a bare
// `const char*` by reference, and these documents outlive the snapshot they
// were built from (see reference_arduinojson_const_char_ref). Literals are
// static and are kept by reference on purpose.

#include "esp_zb_netmap_json.h"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace zhc_zb::netmap {

namespace {

const char* type_name(NodeType t) {
    switch (t) {
        case NodeType::Coordinator: return "coordinator";
        case NodeType::Router:      return "router";
        case NodeType::EndDevice:   return "end_device";
        default:                    return "unknown";
    }
}

const char* rel_name(Rel r) {
    switch (r) {
        case Rel::Parent:    return "parent";
        case Rel::Child:     return "child";
        case Rel::Sibling:   return "sibling";
        case Rel::PrevChild: return "prev_child";
        default:             return "none";
    }
}

std::string ieee_str(uint64_t ieee) {
    char s[19];
    std::snprintf(s, sizeof(s), "0x%016" PRIX64, ieee);
    return s;
}

}  // namespace

void build_json(JsonDocument& doc, const Walk& w, uint32_t ts, uint32_t duration_ms) {
    doc.clear();
    doc["ts"] = ts;
    doc["duration_ms"] = duration_ms;
    doc["truncated"] = w.truncated();

    JsonArray nodes = doc["nodes"].to<JsonArray>();
    for (size_t i = 0; i < w.node_count(); ++i) {
        const Node& n = w.nodes()[i];
        const Known* k = w.find_known(n.ieee);
        const bool coord = n.type == NodeType::Coordinator;
        JsonObject o = nodes.add<JsonObject>();
        o["ieee"]   = ieee_str(n.ieee);
        o["nwk"]    = n.nwk;
        o["type"]   = type_name(n.type);
        o["name"]   = std::string(coord ? "Hub" : (k ? k->name : ""));
        o["model"]  = std::string(k ? k->model : "");
        o["known"]  = coord || k != nullptr;
        o["failed"] = n.failed;
    }

    JsonArray links = doc["links"].to<JsonArray>();
    for (size_t i = 0; i < w.link_count(); ++i) {
        const Link& l = w.links()[i];
        // At the node cap an answering fallback router can contribute links
        // whose source never became a node.
        if (!w.seen(l.src) || !w.seen(l.dst)) continue;
        JsonObject o = links.add<JsonObject>();
        o["src"]   = ieee_str(l.src);
        o["dst"]   = ieee_str(l.dst);
        o["lqi"]   = l.lqi;
        o["rel"]   = rel_name(l.rel);
        o["depth"] = l.depth;
    }

    JsonArray not_seen = doc["not_seen"].to<JsonArray>();
    for (size_t i = 0; i < w.known_count(); ++i) {
        const Known& k = w.known()[i];
        if (w.seen(k.ieee)) continue;
        JsonObject o = not_seen.add<JsonObject>();
        o["ieee"]      = ieee_str(k.ieee);
        o["name"]      = std::string(k.name);
        o["last_seen"] = k.last_seen;
    }
}

}  // namespace zhc_zb::netmap
