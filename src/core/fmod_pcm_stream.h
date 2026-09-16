#ifndef GODOTFMOD_FMOD_PCM_STREAM_H
#define GODOTFMOD_FMOD_PCM_STREAM_H

#include "classes/node.hpp"
#include "classes/ref_counted.hpp"
#include "fmod_studio.hpp"
#include "studio/fmod_event_description.h"
#include "variant/packed_float32_array.hpp"
#include "variant/transform3d.hpp"

#include <cstdint>

namespace godot {
    class FmodPcmStream : public RefCounted {
        GDCLASS(FmodPcmStream, RefCounted)

        struct StreamState;

        StreamState* state = nullptr;
        FMOD::Studio::EventInstance* event_instance = nullptr;
        String last_error;
        float distance_scale = 1.0f;
        bool started = false;

        bool fail(FMOD_RESULT result, const String& message);
        void release_owner_state();

        static FMOD_RESULT F_CALL event_callback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters);
        static FMOD_RESULT F_CALL pcm_read_callback(FMOD_SOUND* sound, void* data, unsigned int data_length);

    protected:
        static void _bind_methods();

    public:
        FmodPcmStream() = default;
        ~FmodPcmStream() override;

        bool initialize(
          const Ref<FmodEventDescription>& event_description,
          int sample_rate = 48000,
          int channels = 1,
          int capacity_ms = 500,
          int start_threshold_ms = 150,
          int decode_buffer_ms = 20
        );
        bool start();
        void stop(bool allow_fade_out = false);
        int push_pcm(const PackedFloat32Array& pcm);
        void clear();

        bool is_valid() const;
        bool is_started() const;
        bool is_buffering() const;
        int get_queued_frames() const;
        uint64_t get_underrun_count() const;
        uint64_t get_overflow_count() const;
        uint64_t get_rebuffer_count() const;
        String get_last_error() const;

        void set_3d_attributes(const Transform3D& transform) const;
        void set_node_attributes(Node* node) const;
        void set_distance_scale(float scale);
    };
}// namespace godot

#endif// GODOTFMOD_FMOD_PCM_STREAM_H
