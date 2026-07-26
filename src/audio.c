#include "audio.h"

#include <string.h>

static uint16_t audio_read_u16_le(const uint8_t *buffer) {
    return (uint16_t)((uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8));
}

static uint32_t audio_read_u32_le(const uint8_t *buffer) {
    return ((uint32_t)buffer[0]) | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static int16_t audio_read_i16_le(const uint8_t *buffer) {
    return (int16_t)(buffer[0] | ((uint16_t)buffer[1] << 8));
}

static void audio_write_u16_le(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value & 0xFFu);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void audio_write_u32_le(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value & 0xFFu);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void audio_generate_samples(intercom_audio_subsystem_t *audio, intercom_audio_frame_t *frame) {
    if (audio == NULL || frame == NULL) {
        return;
    }

    /* Build a deterministic PCM pattern that is easy to reason about in tests:
     * the base offset is derived from the sequence number and the sample index so
     * the same frame produces a stable signed waveform without requiring any math library. */
    for (size_t index = 0; index < frame->sample_count; ++index) {
        const uint32_t base = (audio->next_sequence * 31U) + (uint32_t)index * 17U;
        const int32_t sample_value = (int32_t)((base % 512U) - 256U) * 8;
        frame->samples[index] = (int16_t)sample_value;
    }
}

void intercom_audio_init(intercom_audio_subsystem_t *audio) {
    if (audio == NULL) {
        return;
    }

    memset(audio, 0, sizeof(*audio));
    audio->enabled = true;
    audio->capture_ready = true;
    audio->playback_ready = true;
    audio->sample_rate = INTERCOM_AUDIO_SAMPLE_RATE;
    audio->channels = INTERCOM_AUDIO_CHANNELS;
    audio->bits_per_sample = INTERCOM_AUDIO_BITS_PER_SAMPLE;
    audio->samples_per_frame = INTERCOM_AUDIO_SAMPLES_PER_FRAME;
    audio->next_sequence = 1U;
}

void intercom_audio_set_enabled(intercom_audio_subsystem_t *audio, bool enabled) {
    if (audio == NULL) {
        return;
    }

    audio->enabled = enabled;
}

void intercom_audio_set_capture_callback(intercom_audio_subsystem_t *audio,
                                        intercom_audio_capture_cb cb, void *context) {
    if (audio == NULL) {
        return;
    }

    audio->capture_cb = cb;
    audio->capture_context = context;
}

void intercom_audio_set_playback_callback(intercom_audio_subsystem_t *audio,
                                         intercom_audio_playback_cb cb, void *context) {
    if (audio == NULL) {
        return;
    }

    audio->playback_cb = cb;
    audio->playback_context = context;
}

bool intercom_audio_capture_frame(intercom_audio_subsystem_t *audio, intercom_audio_frame_t *frame) {
    if (audio == NULL || frame == NULL) {
        return false;
    }

    if (!audio->enabled) {
        audio->underruns++;
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = INTERCOM_AUDIO_FRAME_VERSION;
    frame->sample_rate = audio->sample_rate;
    frame->channels = audio->channels;
    frame->bits_per_sample = audio->bits_per_sample;
    frame->sample_count = audio->samples_per_frame;
    frame->sequence = (uint16_t)audio->next_sequence;
    frame->timestamp_ms = audio->last_timestamp_ms +
                          ((uint32_t)frame->sample_count * 1000U) / frame->sample_rate;

    if (audio->capture_cb != NULL) {
        audio->capture_cb(audio->capture_context, frame->samples, frame->sample_count);
    } else {
        audio_generate_samples(audio, frame);
    }

    audio->captured_frames++;
    audio->next_sequence++;
    audio->last_timestamp_ms = frame->timestamp_ms;
    audio->capture_ready = true;
    audio->has_last_sample = frame->sample_count > 0U;
    if (audio->has_last_sample) {
        audio->last_sample = frame->samples[frame->sample_count - 1U];
    }

    return true;
}

bool intercom_audio_encode_frame(const intercom_audio_frame_t *frame, uint8_t *buffer,
                                 size_t buffer_len, size_t *encoded_len) {
    if (frame == NULL || buffer == NULL || encoded_len == NULL) {
        return false;
    }

    if (frame->sample_count > INTERCOM_AUDIO_SAMPLES_PER_FRAME || frame->sample_count == 0U) {
        return false;
    }

    if (frame->bits_per_sample != INTERCOM_AUDIO_BITS_PER_SAMPLE) {
        return false;
    }

    const size_t required_len = INTERCOM_AUDIO_FRAME_HEADER_BYTES +
                                ((size_t)frame->sample_count * sizeof(int16_t));
    if (buffer_len < required_len) {
        return false;
    }

    size_t offset = 0U;
    audio_write_u16_le(buffer + offset, frame->version);
    offset += 2U;
    audio_write_u16_le(buffer + offset, frame->sample_rate);
    offset += 2U;
    audio_write_u16_le(buffer + offset, frame->channels);
    offset += 2U;
    audio_write_u16_le(buffer + offset, frame->bits_per_sample);
    offset += 2U;
    audio_write_u16_le(buffer + offset, frame->sample_count);
    offset += 2U;
    audio_write_u16_le(buffer + offset, frame->sequence);
    offset += 2U;
    audio_write_u32_le(buffer + offset, frame->timestamp_ms);
    offset += 4U;

    for (size_t index = 0; index < frame->sample_count; ++index) {
        const int16_t sample = frame->samples[index];
        buffer[offset++] = (uint8_t)(sample & 0xFF);
        buffer[offset++] = (uint8_t)((sample >> 8) & 0xFF);
    }

    *encoded_len = required_len;
    return true;
}

bool intercom_audio_decode_frame(const uint8_t *buffer, size_t buffer_len,
                                 intercom_audio_frame_t *frame) {
    if (buffer == NULL || frame == NULL) {
        return false;
    }

    if (buffer_len < INTERCOM_AUDIO_FRAME_HEADER_BYTES) {
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    size_t offset = 0U;
    frame->version = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->sample_rate = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->channels = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->bits_per_sample = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->sample_count = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->sequence = audio_read_u16_le(buffer + offset);
    offset += 2U;
    frame->timestamp_ms = audio_read_u32_le(buffer + offset);
    offset += 4U;

    if (frame->version != INTERCOM_AUDIO_FRAME_VERSION || frame->sample_count == 0U ||
        frame->sample_count > INTERCOM_AUDIO_SAMPLES_PER_FRAME ||
        frame->bits_per_sample != INTERCOM_AUDIO_BITS_PER_SAMPLE ||
        frame->channels != INTERCOM_AUDIO_CHANNELS ||
        buffer_len != INTERCOM_AUDIO_FRAME_HEADER_BYTES +
                           ((size_t)frame->sample_count * sizeof(int16_t))) {
        return false;
    }

    for (size_t index = 0; index < frame->sample_count; ++index) {
        frame->samples[index] = audio_read_i16_le(buffer + offset);
        offset += 2U;
    }

    return true;
}

bool intercom_audio_playback_frame(intercom_audio_subsystem_t *audio,
                                   const intercom_audio_frame_t *frame) {
    if (audio == NULL || frame == NULL) {
        return false;
    }

    if (!audio->enabled) {
        audio->underruns++;
        return false;
    }

    if (audio->queued_frame_count >= INTERCOM_AUDIO_PLAYBACK_QUEUE_DEPTH) {
        audio->overruns++;
        return false;
    }

    audio->playback_queue[audio->queued_frame_count] = *frame;
    audio->queued_frame_count++;
    audio->queued_playback_frames++;
    audio->playback_ready = true;
    return true;
}

bool intercom_audio_drain_playback_queue(intercom_audio_subsystem_t *audio) {
    if (audio == NULL) {
        return false;
    }

    bool drained = false;
    while (audio->queued_frame_count > 0U) {
        const intercom_audio_frame_t frame = audio->playback_queue[0U];
        if (audio->queued_frame_count > 1U) {
            memmove(&audio->playback_queue[0U], &audio->playback_queue[1U],
                    (audio->queued_frame_count - 1U) * sizeof(audio->playback_queue[0U]));
        }
        audio->queued_frame_count--;

        if (audio->playback_cb != NULL) {
            audio->playback_cb(audio->playback_context, frame.samples, frame.sample_count);
        }

        audio->played_frames++;
        audio->has_last_sample = frame.sample_count > 0U;
        if (audio->has_last_sample) {
            audio->last_sample = frame.samples[frame.sample_count - 1U];
        }
        drained = true;
    }

    return drained;
}

void intercom_audio_note_encoded_frame(intercom_audio_subsystem_t *audio) {
    if (audio != NULL) {
        audio->encoded_frames++;
    }
}

void intercom_audio_note_decoded_frame(intercom_audio_subsystem_t *audio) {
    if (audio != NULL) {
        audio->decoded_frames++;
    }
}

void intercom_audio_note_playback_dropped(intercom_audio_subsystem_t *audio) {
    if (audio != NULL) {
        audio->dropped_frames++;
    }
}

size_t intercom_audio_frame_bytes(const intercom_audio_frame_t *frame) {
    if (frame == NULL) {
        return 0U;
    }

    return INTERCOM_AUDIO_FRAME_HEADER_BYTES + ((size_t)frame->sample_count * sizeof(int16_t));
}

const char *intercom_audio_state_name(bool enabled) {
    return enabled ? "enabled" : "disabled";
}
