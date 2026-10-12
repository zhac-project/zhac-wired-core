// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap_walk -- the network map's walk (spec 2026-10-12 §3.2), with no
// ESP or esp-zigbee dependency so the host test drives it.
//
// The glue (esp_zb_netmap.cpp) feeds it the hub's devices, the coordinator's
// own neighbour table, then one Mgmt_Lqi_rsp page (or a failure) per ask. The
// walk decides which router to ask next, pages through each table, retries a
// failed page once, and keeps the nodes and links found.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zhc_zb::netmap {

inline constexpr size_t  kMaxNodes   = 256;
inline constexpr size_t  kMaxLinks   = 1024;
inline constexpr size_t  kMaxRouters = 128;
// Pages asked of one router. 32 pages of 2-3 records cover any real table and
// stop a router that keeps answering (the spec's "always ends" rule).
inline constexpr uint8_t kMaxPages   = 32;
inline constexpr uint8_t kTries      = 2;    // a page is retried once

enum class NodeType : uint8_t { Coordinator = 0, Router = 1, EndDevice = 2, Unknown = 3 };
enum class Rel : uint8_t { Parent = 0, Child = 1, Sibling = 2, None = 3, PrevChild = 4 };

// A device the hub knows: a snapshot of the pool taken when the scan starts.
struct Known {
    uint64_t ieee;
    uint16_t nwk;
    uint8_t  type;        // ZapDevice::device_type: 0 coordinator, 1 router, 2 end device
    uint32_t last_seen;   // epoch s, 0 = not seen with a set clock
    char     name[30];
    char     model[34];
};

// One neighbour-table record, from the coordinator's own table or a Mgmt_Lqi_rsp.
struct Neighbor {
    uint64_t ext_pan;     // 0 = not reported (the local table): taken as ours
    uint64_t ieee;
    uint16_t nwk;
    uint8_t  type;        // device_type 0..3
    uint8_t  rel;         // relationship 0..4 (dst is src's ...)
    uint8_t  depth;
    uint8_t  lqi;
    bool     no_lqi = false;   // the radio gave no reading (local table, rssi 0): lqi is meaningless
};

struct Node {
    uint64_t ieee;
    uint16_t nwk;
    NodeType type;
    bool     failed;      // a router that never answered
};

struct Link {
    uint64_t src;         // the device whose table holds the record
    uint64_t dst;         // its neighbour
    uint8_t  lqi;
    Rel      rel;
    uint8_t  depth;
    bool     no_lqi = false;
};

struct Ask {
    uint64_t ieee;
    uint16_t nwk;
    uint8_t  start_index;
};

class Walk {
public:
    // Starts a walk. `known` must stay valid until the JSON is built.
    void begin(uint64_t coord_ieee, uint64_t our_ext_pan, const Known* known, size_t n_known);
    // The coordinator's own table: links coordinator -> neighbour.
    void add_local(const Neighbor* recs, size_t n);
    // The next page to request; false when the walk is over.
    bool next_ask(Ask* out);
    // The answer to `a`: the router's table holds `total` entries and the
    // answer carries `n` of them from `start_index` on.
    void on_page(const Ask& a, uint8_t total, uint8_t start_index, const Neighbor* recs, size_t n);
    // `a` timed out or came back with an error status.
    void on_fail(const Ask& a);

    const Node*  nodes() const { return nodes_; }
    size_t       node_count() const { return n_nodes_; }
    const Link*  links() const { return links_; }
    size_t       link_count() const { return n_links_; }
    bool         truncated() const { return truncated_; }
    size_t       routers_done() const { return q_head_; }   // asked to the end, or given up on
    size_t       routers_total() const { return q_len_; }   // queued so far (grows during the walk)
    const Known* known() const { return known_; }
    size_t       known_count() const { return n_known_; }
    const Known* find_known(uint64_t ieee) const;
    // True when some table listed `ieee`, or it is the coordinator.
    bool         seen(uint64_t ieee) const { return find_node(ieee) != nullptr; }

private:
    struct Queued { uint64_t ieee; uint16_t nwk; };

    const Node* find_node(uint64_t ieee) const;
    Node*       find_node(uint64_t ieee);
    Node*       add_node(uint64_t ieee, uint16_t nwk, NodeType type);
    void        add_record(uint64_t src, const Neighbor& r);
    void        enqueue(uint64_t ieee, uint16_t nwk);
    bool        is_current(const Ask& a) const;
    void        finish_router();

    uint64_t     coord_   = 0;
    uint64_t     ext_pan_ = 0;
    const Known* known_   = nullptr;
    size_t       n_known_ = 0;
    Node         nodes_[kMaxNodes];
    size_t       n_nodes_ = 0;
    Link         links_[kMaxLinks];
    size_t       n_links_ = 0;
    Queued       queue_[kMaxRouters];
    size_t       q_len_  = 0;
    size_t       q_head_ = 0;    // the router being asked
    uint8_t      start_  = 0;    // its next page
    uint8_t      pages_  = 0;    // pages it answered
    uint8_t      tries_  = 0;    // tries of the current page
    bool         fallback_done_ = false;
    bool         truncated_     = false;
};

}  // namespace zhc_zb::netmap
