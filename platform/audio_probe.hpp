// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Read the ConnectionMaster audio connection table without changing a route.
#ifndef ENTRY_AAD_PLATFORM_AUDIO_PROBE_HPP
#define ENTRY_AAD_PLATFORM_AUDIO_PROBE_HPP

#include <stdint.h>

#include <vector>

namespace evo {

// Runs in a separate diagnostic process, not alongside the service's process-global vMOST connection.
// The raw Status payload is returned on success. A missing, malformed or timed-out reply is a failure.
bool readAudioRouteTable(std::vector<uint8_t>& payload, unsigned waitMs);

}  // namespace evo

#endif
