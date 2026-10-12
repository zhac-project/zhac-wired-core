// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zb_netmap_walk -- see the header. Linear scans throughout: at most 256
// nodes, 1024 links and 128 routers, once per manual scan.

#include "esp_zb_netmap_walk.h"

namespace zhc_zb::netmap {

namespace {
bool valid_ieee(uint64_t ieee) { return ieee != 0 && ieee != UINT64_MAX; }
NodeType node_type(uint8_t t) { return t <= 3 ? static_cast<NodeType>(t) : NodeType::Unknown; }
Rel rel_of(uint8_t r) { return r <= 4 ? static_cast<Rel>(r) : Rel::None; }
}  // namespace

void Walk::begin(uint64_t coord_ieee, uint64_t our_ext_pan, const Known* known, size_t n_known) {
    coord_ = coord_ieee;
    ext_pan_ = our_ext_pan;
    known_ = known;
    n_known_ = known ? n_known : 0;
    n_nodes_ = n_links_ = q_len_ = q_head_ = 0;
    start_ = pages_ = tries_ = 0;
    fallback_done_ = truncated_ = false;
    add_node(coord_ieee, 0x0000, NodeType::Coordinator);
}

const Known* Walk::find_known(uint64_t ieee) const {
    for (size_t i = 0; i < n_known_; ++i)
        if (known_[i].ieee == ieee) return &known_[i];
    return nullptr;
}

const Node* Walk::find_node(uint64_t ieee) const {
    for (size_t i = 0; i < n_nodes_; ++i)
        if (nodes_[i].ieee == ieee) return &nodes_[i];
    return nullptr;
}

Node* Walk::find_node(uint64_t ieee) {
    return const_cast<Node*>(static_cast<const Walk*>(this)->find_node(ieee));
}

Node* Walk::add_node(uint64_t ieee, uint16_t nwk, NodeType type) {
    if (Node* n = find_node(ieee)) {
        n->nwk = nwk;   // the latest table wins: a rejoined device has a new short address
        if (n->type == NodeType::Unknown) n->type = type;
        return n;
    }
    if (n_nodes_ == kMaxNodes) { truncated_ = true; return nullptr; }
    nodes_[n_nodes_] = Node{ieee, nwk, type, false};
    return &nodes_[n_nodes_++];
}

void Walk::enqueue(uint64_t ieee, uint16_t nwk) {
    if (ieee == coord_) return;
    for (size_t i = 0; i < q_len_; ++i) {
        if (queue_[i].ieee != ieee) continue;
        if (i > q_head_) queue_[i].nwk = nwk;   // not asked yet: take the newer address
        return;
    }
    if (q_len_ == kMaxRouters) { truncated_ = true; return; }
    queue_[q_len_++] = Queued{ieee, nwk};
}

void Walk::add_record(uint64_t src, const Neighbor& r) {
    if (!valid_ieee(r.ieee) || r.ieee == src) return;
    if (r.ext_pan != 0 && r.ext_pan != ext_pan_) return;   // a neighbour in another network
    const NodeType t = node_type(r.type);
    if (!add_node(r.ieee, r.nwk, t)) return;
    for (size_t i = 0; i < n_links_; ++i)
        if (links_[i].src == src && links_[i].dst == r.ieee) return;   // the same record on two pages
    if (n_links_ == kMaxLinks) { truncated_ = true; return; }
    links_[n_links_++] = Link{src, r.ieee, r.lqi, rel_of(r.rel), r.depth};
    if (t == NodeType::Router) enqueue(r.ieee, r.nwk);
}

void Walk::add_local(const Neighbor* recs, size_t n) {
    for (size_t i = 0; i < n; ++i) add_record(coord_, recs[i]);
}

bool Walk::next_ask(Ask* out) {
    if (q_head_ == q_len_ && !fallback_done_) {
        // Routers the hub knows that no table listed: ask them too (spec §3.2.2).
        fallback_done_ = true;
        for (size_t i = 0; i < n_known_; ++i)
            if (known_[i].type == 1 && valid_ieee(known_[i].ieee)) enqueue(known_[i].ieee, known_[i].nwk);
    }
    if (q_head_ == q_len_) return false;
    *out = Ask{queue_[q_head_].ieee, queue_[q_head_].nwk, start_};
    return true;
}

bool Walk::is_current(const Ask& a) const {
    return q_head_ < q_len_ && a.ieee == queue_[q_head_].ieee && a.start_index == start_;
}

void Walk::finish_router() {
    ++q_head_;
    start_ = pages_ = tries_ = 0;
}

void Walk::on_page(const Ask& a, uint8_t total, uint8_t start_index, const Neighbor* recs, size_t n) {
    if (!is_current(a)) return;   // a late answer to an ask already given up on
    add_node(a.ieee, a.nwk, NodeType::Router);   // it answered: a pool-only router is on the map now
    for (size_t i = 0; i < n; ++i) add_record(a.ieee, recs[i]);
    tries_ = 0;
    ++pages_;
    const unsigned next = unsigned(start_) + unsigned(n);
    // Done: an empty page, the end of its table, an answer for another index
    // than asked (it would send that page forever), or the page cap.
    if (n == 0 || next >= total || start_index != start_ || pages_ >= kMaxPages || next > 255) {
        finish_router();
        return;
    }
    start_ = static_cast<uint8_t>(next);
}

void Walk::on_fail(const Ask& a) {
    if (!is_current(a)) return;
    if (++tries_ < kTries) return;   // ask the same page again
    // Listed by some table: on the map as "no reply". Only the pool knew it:
    // no node, so it stays in not_seen.
    if (Node* nd = find_node(a.ieee)) nd->failed = true;
    finish_router();
}

}  // namespace zhc_zb::netmap
