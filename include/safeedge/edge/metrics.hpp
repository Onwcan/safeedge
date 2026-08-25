// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "safeedge/edge/runtime_snapshot.hpp"

namespace safeedge::edge {

/// Renders a snapshot in the Prometheus text exposition format.
///
/// Runs on the HTTP thread, never on the real-time path, so it is free to
/// allocate and build a string. That separation is the reason RuntimeSnapshot
/// exists at all.
///
/// `snapshot_is_fresh` is false when the caller could not get a clean read out
/// of the seqlock within its retry budget. It is exported rather than hidden:
/// a scrape that silently served stale numbers as current would be worse than
/// one that says so.
// @satisfies REQ-EDGE-004
[[nodiscard]] std::string renderPrometheus(const RuntimeSnapshot& snapshot,
                                           bool snapshot_is_fresh);

}  // namespace safeedge::edge
