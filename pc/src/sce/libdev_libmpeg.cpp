// libdev's debug console and libmpeg, as far as the PC build has them.
//
// The debug console (mglib's performance meter text) prints nothing. The
// MPEG decoder is not there yet: a stream reads as already ended, so the
// game's movies finish at once. Decoding the PSS movies (MPEG-2 video and
// ADPCM sound) is to come, through FFmpeg.

#include "../host/sdk.h"

#pragma push_macro("NULL")
#pragma push_macro("u_long")
#undef NULL
#define u_long ps2_u_long
#include <libdev.h>
#include <libmpeg.h>
#undef NULL
#undef u_long
#pragma pop_macro("u_long")
#pragma pop_macro("NULL")

#include <cstring>

extern "C" {

void sceDevConsInit() {}

int sceDevConsOpen(u_int x, u_int y, u_int width, u_int height) {
    (void) x;
    (void) y;
    (void) width;
    (void) height;
    return 1;
}

void sceDevConsClose(int handle) { (void) handle; }

void sceDevConsAttribute(int handle, u_char attributes) {
    (void) handle;
    (void) attributes;
}

void sceDevConsDraw(int handle) { (void) handle; }

int sceMpegInit(void) { return 0; }

int sceMpegCreate(sceMpeg *mp, u_char *work_area, int work_area_size) {
    (void) work_area;
    (void) work_area_size;
    std::memset(mp, 0, sizeof(*mp));
    return 0;
}

int sceMpegDelete(sceMpeg *mp) {
    (void) mp;
    return 0;
}

int sceMpegReset(sceMpeg *mp) {
    mp->frameCount = 0;
    return 0;
}

int sceMpegGetPicture(sceMpeg *mp, sceIpuRGB32 *rgb32, int mbcount) {
    (void) mp;
    (void) rgb32;
    (void) mbcount;
    return -1;
}

int sceMpegIsEnd(sceMpeg *mp) {
    (void) mp;
    return 1;
}

sceMpegCallback sceMpegAddCallback(sceMpeg *mp, sceMpegCbType type, sceMpegCallback callback, void *anyData) {
    (void) mp;
    (void) type;
    (void) anyData;
    return callback;
}

sceMpegCallback sceMpegAddStrCallback(sceMpeg *mp, sceMpegStrType strType, int ch, sceMpegCallback callback,
                                      void *anyData) {
    (void) mp;
    (void) strType;
    (void) ch;
    (void) anyData;
    return callback;
}

int sceMpegDemuxPssRing(sceMpeg *mp, u_char *start, int size, u_char *buf_start, int buf_size) {
    // Everything offered is taken, and dropped.
    (void) mp;
    (void) start;
    (void) buf_start;
    (void) buf_size;
    return size;
}

} // extern "C"
