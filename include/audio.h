#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTERCOM_AUDIO_SAMPLE_RATE 8000U
#define INTERCOM_AUDIO_CHANNELS 1U
#define INTERCOM_AUDIO_BITS_PER_SAMPLE 16U
#define INTERCOM_AUDIO_SAMPLES_PER_FRAME 32U
#define INTERCOM_AUDIO_PLAYBACK_QUEUE_DEPTH 4U
#define INTERCOM_AUDIO_FRAME_HEADER_BYTES 16U
#define INTERCOM_AUDIO_FRAME_VERSION 1U

typedef struct {
    uint16_t version;
    uint16_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t sample_count;
    uint16_t sequence;
    uint32_t timestamp_ms;
    int16_t samples[INTERCOM_AUDIO_SAMPLES_PER_FRAME];
} intercom_audio_frame_t;

typedef void (*intercom_audio_capture_cb)(void *context, int16_t *samples, size_t sample_count);
typedef void (*intercom_audio_playback_cb)(void *context, const int16_t *samples,
                                           size_t sample_count);

typedef struct {
    bool enabled;
    bool capture_ready;
    bool playback_ready;
    uint16_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t samples_per_frame;
    uint32_t next_sequence;
    uint32_t last_timestamp_ms;
    size_t captured_frames;
    size_t encoded_frames;
    size_t decoded_frames;
    size_t queued_playback_frames;
    size_t played_frames;
    size_t dropped_frames;
    size_t overruns;
    size_t underruns;
    int16_t last_sample;
    bool has_last_sample;
    intercom_audio_frame_t playback_queue[INTERCOM_AUDIO_PLAYBACK_QUEUE_DEPTH];
    size_t queued_frame_count;
    intercom_audio_capture_cb capture_cb;
    void *capture_context;
    intercom_audio_playback_cb playback_cb;
    void *playback_context;
} intercom_audio_subsystem_t;

void intercom_audio_init(intercom_audio_subsystem_t *audio);
void intercom_audio_set_enabled(intercom_audio_subsystem_t *audio, bool enabled);
void intercom_audio_set_capture_callback(intercom_audio_subsystem_t *audio,
                                        intercom_audio_capture_cb cb, void *context);
void intercom_audio_set_playback_callback(intercom_audio_subsystem_t *audio,
                                         intercom_audio_playback_cb cb, void *context);
bool intercom_audio_capture_frame(intercom_audio_subsystem_t *audio, intercom_audio_frame_t *frame);
bool intercom_audio_encode_frame(const intercom_audio_frame_t *frame, uint8_t *buffer,
                                 size_t buffer_len, size_t *encoded_len);
bool intercom_audio_decode_frame(const uint8_t *buffer, size_t buffer_len,
                                 intercom_audio_frame_t *frame);
bool intercom_audio_playback_frame(intercom_audio_subsystem_t *audio,
                                   const intercom_audio_frame_t *frame);
bool intercom_audio_drain_playback_queue(intercom_audio_subsystem_t *audio);
void intercom_audio_note_encoded_frame(intercom_audio_subsystem_t *audio);
void intercom_audio_note_decoded_frame(intercom_audio_subsystem_t *audio);
void intercom_audio_note_playback_dropped(intercom_audio_subsystem_t *audio);
size_t intercom_audio_frame_bytes(const intercom_audio_frame_t *frame);
const char *intercom_audio_state_name(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
