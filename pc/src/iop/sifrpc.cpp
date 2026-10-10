// SIF RPC and the IOP heap on the PC: binding to a server finds the one this
// process provides, and a call runs it at once, so no request is ever left
// pending. Module loading and IOP reboots have nothing to do and succeed.

#include "../audio/servers.h"
#include "../host/sdk.h"
#include "iop.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace pc {
namespace iop {

RpcHandler FindServer(unsigned int number) {
    switch (number) {
        case audio::kEzBgmRpcNumber:
            return audio::EzBgmServer;
        case audio::kEzMidiRpcNumber:
            return audio::EzMidiServer;
        default:
            return NULL;
    }
}

} // namespace iop
} // namespace pc

namespace {

/**
 * What a bound client's `server` points at. The game only checks it against
 * zero; the RPC number the client was bound to is kept in its header.
 */
char bound_server_token;

} // namespace

extern "C" {

void sceSifInitRpc(int mode) {
    (void) mode;
}

int sceSifRebootIop(const char *path) {
    (void) path;
    return 1;
}

int sceSifSyncIop(void) {
    return 1;
}

int sceSifLoadModule(const char *path, int arg_len, const char *args) {
    static int next_id = 1;

    (void) arg_len;
    (void) args;
    SDL_Log("IOP module %s is built in", path);
    return next_id++;
}

void sceSifExitCmd(void) {}

int sceSifBindRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode) {
    (void) mode;
    std::memset(client, 0, sizeof(*client));
    client->header.rpc_id = number;

    if (pc::iop::FindServer(number) != NULL) {
        client->server = reinterpret_cast<sceSifServeData *>(&bound_server_token);
    } else {
        SDL_Log("No IOP server for RPC number 0x%X", number);
    }

    return 0;
}

int sceSifCheckStatRpc(struct sceSifClientData *client) {
    (void) client;
    return 0;
}

int sceSifCallRpc(struct sceSifClientData *client, unsigned int number, unsigned int mode, void *send,
                  int send_size, void *receive, int receive_size, void (*end_callback)(void *),
                  void *end_parameter) {
    (void) mode;
    pc::iop::RpcHandler handler = client->server != NULL ? pc::iop::FindServer(client->header.rpc_id) : NULL;

    if (handler == NULL) {
        return -1;
    }

    {
        std::lock_guard<std::recursive_mutex> guard(pc::iop::Lock());
        void                                 *reply = handler(number, send, send_size);

        if (reply != NULL && receive != NULL && receive_size > 0) {
            std::memmove(receive, reply, receive_size);
        }
    }

    if (end_callback != NULL) {
        end_callback(end_parameter);
    }

    return 0;
}

int sceSifInitIopHeap(void) {
    return 0;
}

void *sceSifAllocIopHeap(unsigned int size) {
    return pc::iop::PointerOf(pc::iop::Alloc(size));
}

int sceSifFreeIopHeap(void *address) {
    pc::iop::Free(pc::iop::AddressOf(address));
    return 0;
}

void *sceSifAllocSysMemory(int mode, unsigned int size, void *address) {
    (void) mode;
    (void) address;
    return pc::iop::PointerOf(pc::iop::Alloc(size));
}

int sceSifFreeSysMemory(void *address) {
    pc::iop::Free(pc::iop::AddressOf(address));
    return 0;
}

int sceSifSetDma(struct sceSifDmaData *transfer, int count) {
    std::lock_guard<std::recursive_mutex> guard(pc::iop::Lock());

    for (int i = 0; i < count; i++) {
        void *destination = pc::iop::Translate(pc::iop::AddressOf(transfer[i].addr), transfer[i].size);

        if (destination == NULL) {
            SDL_Log("SIF DMA to %p (%d bytes) is outside IOP memory", transfer[i].addr, transfer[i].size);
            return 0;
        }

        std::memmove(destination, transfer[i].data, transfer[i].size);
    }

    return 1;
}

int sceSifDmaStat(int id) {
    (void) id;
    return -1;
}

} // extern "C"
