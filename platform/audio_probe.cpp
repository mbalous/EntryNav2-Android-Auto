// Copyright (c) 2026 mbalous
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "audio_probe.hpp"

#include <poll.h>

#include <cerrno>

#include "evo_log.hpp"
#include "vmost_client.hpp"

namespace evo {
namespace {

const uint8_t kConnectionMaster = 0x03;
const uint16_t kConnectionTable = 0xC03;

struct Reply {
    bool received;
    std::vector<uint8_t> payload;
    Reply() : received(false) {}
};

void onMessage(void* message, void* user) {
    Reply* reply = static_cast<Reply*>(user);
    VmostMsgView view;
    if (!reply || !parseVmostMsg(message, view) || view.fblock != kConnectionMaster || view.inst != 0 ||
        view.fkt != kConnectionTable || view.op != kMostOpStatus || reply->received) {
        return;
    }
    reply->received = true;
    if (view.len) {
        reply->payload.assign(view.data, view.data + view.len);
    }
}

}  // namespace

bool readAudioRouteTable(std::vector<uint8_t>& payload, unsigned waitMs) {
    payload.clear();
    VmostClient client;
    const VmostClient::Load loaded = client.load();
    if (loaded != VmostClient::kLoaded) {
        ELOG("AudioProbe", "vMOST unavailable (%d): %s", static_cast<int>(loaded), client.error().c_str());
        return false;
    }
    Reply reply;
    if (client.connect(onMessage, &reply, 0) != 0) {
        ELOG("AudioProbe", "vMOST connection refused");
        client.unload();
        return false;
    }
    const bool registered = client.shadowRegister(kConnectionMaster, 0) == 0;
    int fd = -1;
    bool ok = false;
    if (!registered) {
        ELOG("AudioProbe", "ConnectionMaster shadow registration refused");
    } else if (client.fileDescriptorGet(&fd) != 0 || fd < 0) {
        ELOG("AudioProbe", "cannot obtain vMOST descriptor");
    } else {
        const uint8_t position[] = {0, 0};
        VmostMsgOut request;
        buildVmostGet(request, kConnectionMaster, 0, kConnectionTable, position, sizeof(position));
        if (client.send(&request) != 0) {
            ELOG("AudioProbe", "connection table Get refused");
        } else {
            const uint32_t start = monoMs();
            while (!reply.received) {
                const uint32_t elapsed = monoMs() - start;
                if (elapsed >= waitMs) {
                    break;
                }
                const uint32_t remaining = waitMs - elapsed;
                pollfd p = {fd, POLLIN, 0};
                const int result = poll(&p, 1, remaining < 100 ? static_cast<int>(remaining) : 100);
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                if (result < 0 || (result > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
                    ELOG("AudioProbe", "vMOST connection failed while awaiting the table");
                    break;
                }
                if (result > 0 && (p.revents & POLLIN) && client.processMessages() != 0) {
                    ELOG("AudioProbe", "cannot process vMOST reply");
                    break;
                }
            }
            if (!reply.received) {
                ELOG("AudioProbe", "connection table did not answer");
            } else if (reply.payload.size() < 2 || (reply.payload.size() - 2) % 3 != 0) {
                ELOG("AudioProbe", "unexpected connection table length: %u bytes",
                     static_cast<unsigned>(reply.payload.size()));
            } else {
                payload.swap(reply.payload);
                ok = true;
            }
        }
    }
    if (registered && client.shadowDeregister(kConnectionMaster, 0) != 0) {
        ELOG("AudioProbe", "shadow deregistration failed");
        ok = false;
    }
    if (client.disconnect() != 0) {
        ELOG("AudioProbe", "vMOST disconnect failed");
        ok = false;
    }
    client.unload();
    if (!ok) {
        payload.clear();
    }
    return ok;
}

}  // namespace evo
