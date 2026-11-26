#ifndef NARNIA_IOS_H
#define NARNIA_IOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NarniaSession NarniaSession;

typedef struct {
   uint32_t width;
   uint32_t height;
   float fps;
} NarniaStreamInfo;

typedef struct {
   uint64_t capture_us;
   const uint8_t *data;
   size_t len;
   bool is_idr;
} NarniaVideoFrame;

typedef struct {
   const uint8_t *data;
   size_t len;
} NarniaAudioPacket;

NarniaSession *narnia_connect(const char *url, const char *cert_sha256_hex, uint32_t timeout_ms,
                              char *error, size_t error_len);
NarniaStreamInfo narnia_stream_info(const NarniaSession *session);
int32_t narnia_next_video(NarniaSession *session, uint32_t timeout_ms, NarniaVideoFrame *out);
int32_t narnia_next_audio(NarniaSession *session, uint32_t timeout_ms, NarniaAudioPacket *out);
int32_t narnia_send_key(NarniaSession *session, uint32_t evdev_code, bool pressed, bool repeat);
int32_t narnia_send_pointer_relative(NarniaSession *session, double dx, double dy);
int32_t narnia_send_pointer_absolute(NarniaSession *session, double x, double y, uint32_t width,
                                     uint32_t height);
int32_t narnia_send_mouse_button(NarniaSession *session, uint16_t button, bool pressed);
int32_t narnia_send_scroll(NarniaSession *session, double dx, double dy);
int32_t narnia_send_gamepad(NarniaSession *session, uint32_t buttons, const int16_t axes[6]);
void narnia_close(NarniaSession *session);

#ifdef __cplusplus
}
#endif

#endif
