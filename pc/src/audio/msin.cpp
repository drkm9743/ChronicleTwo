// The EE side of the MIDI stream input module on the PC: the messages are
// appended, byte for byte, to the buffers of the context's output group,
// which the game copies to IOP memory for EZMIDI to play (ezmidi.cpp).
//
// A buffer starts with its size and the bytes queued, then the bytes.

#include "../host/sdk.h"

#include <cstring>

namespace {

struct StreamBuffer {
    int           size;   /**< Size of the whole buffer in bytes. */
    int           length; /**< Bytes queued. */
    unsigned char messages[1];
};

const int kHeaderSize = 8;

/** The buffer of a port, from the last buffer group of the context; NULL when there is none. */
StreamBuffer *BufferOf(sceCslCtx *ctx, unsigned int port) {
    if (ctx == NULL || ctx->buffGrpNum < 1) {
        return NULL;
    }

    sceCslBuffGrp &group = ctx->buffGrp[ctx->buffGrpNum - 1];

    if (port >= (unsigned int) group.buffNum) {
        return NULL;
    }

    return static_cast<StreamBuffer *>(group.buffCtx[port].buff);
}

int Append(sceCslCtx *ctx, unsigned int port, const unsigned char *bytes, int count) {
    StreamBuffer *buffer = BufferOf(ctx, port);

    if (buffer == NULL) {
        return -1;
    }

    if (buffer->length + count > buffer->size - kHeaderSize) {
        return -1;
    }

    std::memcpy(&buffer->messages[buffer->length], bytes, count);
    buffer->length += count;
    return 0;
}

} // namespace

extern "C" {

int sceMSIn_Init(sceCslCtx *ctx) {
    return ctx != NULL && ctx->buffGrpNum > 0 ? 0 : -1;
}

int sceMSIn_PutMsg(sceCslCtx *ctx, unsigned int port, unsigned int msg) {
    unsigned char bytes[3] = {(unsigned char) msg, (unsigned char) (msg >> 8), (unsigned char) (msg >> 16)};
    int           count = (bytes[0] & 0xE0) == 0xC0 ? 2 : 3;
    return Append(ctx, port, bytes, count);
}

int sceMSIn_PutHsMsg(sceCslCtx *ctx, unsigned int port, unsigned char *msg) {
    return Append(ctx, port, msg, msg[0] == 0xFD ? 7 : 5);
}

} // extern "C"
