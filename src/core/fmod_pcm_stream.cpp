#include "fmod_pcm_stream.h"

#include "classes/canvas_item.hpp"
#include "classes/node3d.hpp"
#include "fmod_server.h"
#include "helpers/common.h"
#include "helpers/maths.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

using namespace godot;

struct FmodPcmStream::StreamState {
    std::atomic<uint32_t> references {1};
    std::atomic<FMOD::Sound*> sound {nullptr};
    std::atomic<bool> closing {false};
    std::atomic<uint64_t> underruns {0};
    std::atomic<uint64_t> overflows {0};
    std::atomic<uint64_t> rebuffers {0};

    mutable std::mutex buffer_mutex;
    std::vector<float> buffer;
    size_t read_index = 0;
    size_t queued_samples = 0;
    size_t start_threshold_samples = 0;
    int channels = 1;
    bool buffering = true;

    void retain() { references.fetch_add(1, std::memory_order_relaxed); }

    void release() {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) { delete this; }
    }

    void release_sound() {
        FMOD::Sound* owned_sound = sound.exchange(nullptr, std::memory_order_acq_rel);
        if (owned_sound != nullptr) { owned_sound->release(); }
    }

    ~StreamState() { release_sound(); }
};

void FmodPcmStream::_bind_methods() {
    ClassDB::bind_method(
      D_METHOD("initialize", "event_description", "sample_rate", "channels", "capacity_ms", "start_threshold_ms", "decode_buffer_ms"),
      &FmodPcmStream::initialize,
      DEFVAL(48000),
      DEFVAL(1),
      DEFVAL(500),
      DEFVAL(150),
      DEFVAL(100)
    );
    ClassDB::bind_method(D_METHOD("start"), &FmodPcmStream::start);
    ClassDB::bind_method(D_METHOD("stop", "allow_fade_out"), &FmodPcmStream::stop, DEFVAL(false));
    ClassDB::bind_method(D_METHOD("push_pcm", "pcm"), &FmodPcmStream::push_pcm);
    ClassDB::bind_method(D_METHOD("clear"), &FmodPcmStream::clear);
    ClassDB::bind_method(D_METHOD("is_valid"), &FmodPcmStream::is_valid);
    ClassDB::bind_method(D_METHOD("is_started"), &FmodPcmStream::is_started);
    ClassDB::bind_method(D_METHOD("is_buffering"), &FmodPcmStream::is_buffering);
    ClassDB::bind_method(D_METHOD("get_queued_frames"), &FmodPcmStream::get_queued_frames);
    ClassDB::bind_method(D_METHOD("get_underrun_count"), &FmodPcmStream::get_underrun_count);
    ClassDB::bind_method(D_METHOD("get_overflow_count"), &FmodPcmStream::get_overflow_count);
    ClassDB::bind_method(D_METHOD("get_rebuffer_count"), &FmodPcmStream::get_rebuffer_count);
    ClassDB::bind_method(D_METHOD("get_last_error"), &FmodPcmStream::get_last_error);
    ClassDB::bind_method(D_METHOD("set_3d_attributes", "transform"), &FmodPcmStream::set_3d_attributes);
    ClassDB::bind_method(D_METHOD("set_node_attributes", "node"), &FmodPcmStream::set_node_attributes);
    ClassDB::bind_method(D_METHOD("set_distance_scale", "scale"), &FmodPcmStream::set_distance_scale);

    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "started", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "is_started");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "buffering", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "is_buffering");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "queued_frames", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_queued_frames");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "last_error", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_last_error");
}

bool FmodPcmStream::fail(FMOD_RESULT result, const String& message) {
    last_error = vformat("%s: %s", message, FMOD_ErrorString(result));
    GODOT_LOG_ERROR(last_error)
    return false;
}

bool FmodPcmStream::initialize(const Ref<FmodEventDescription>& event_description, int sample_rate, int channels, int capacity_ms, int start_threshold_ms, int decode_buffer_ms) {
    if (state != nullptr || event_instance != nullptr) {
        last_error = "FmodPcmStream is already initialized.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }
    if (event_description.is_null() || !event_description->is_valid()) {
        last_error = "FmodPcmStream needs a valid event description.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }
    if (sample_rate <= 0 || (channels != 1 && channels != 2)) {
        last_error = "FmodPcmStream needs a positive sample rate and one or two channels.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }
    if (capacity_ms <= 0 || start_threshold_ms < 0 || start_threshold_ms > capacity_ms || decode_buffer_ms <= 0) {
        last_error = "FmodPcmStream buffer times are invalid.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }

    FmodServer* server = FmodServer::get_singleton();
    FMOD::System* core_system = server == nullptr ? nullptr : server->get_core_system();
    if (core_system == nullptr) {
        last_error = "FMOD is not initialized.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }

    auto* new_state = new StreamState;
    const size_t capacity_frames =
      std::max(static_cast<size_t>(1), static_cast<size_t>(sample_rate) * static_cast<size_t>(capacity_ms) / 1000);
    new_state->channels = channels;
    new_state->buffer.resize(capacity_frames * static_cast<size_t>(channels));
    new_state->start_threshold_samples = static_cast<size_t>(sample_rate) * static_cast<size_t>(start_threshold_ms)
                                       / 1000 * static_cast<size_t>(channels);

    FMOD_CREATESOUNDEXINFO sound_info {};
    sound_info.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
    sound_info.numchannels = channels;
    sound_info.defaultfrequency = sample_rate;
    sound_info.decodebuffersize = static_cast<unsigned int>(std::max(1, sample_rate * decode_buffer_ms / 1000));
    sound_info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    // OPENUSER requires a finite length. One hour keeps its loop outside a
    // normal voice session without approaching the 32-bit byte limit.
    sound_info.length = static_cast<unsigned int>(
      static_cast<uint64_t>(sample_rate) * static_cast<uint64_t>(channels) * sizeof(float) * 60 * 60
    );
    sound_info.pcmreadcallback = pcm_read_callback;
    sound_info.userdata = new_state;

    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result = core_system->createSound(nullptr, FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL, &sound_info, &sound);
    if (result != FMOD_OK) {
        new_state->release();
        return fail(result, "Cannot create the FMOD PCM sound");
    }
    new_state->sound.store(sound, std::memory_order_release);

    result = event_description->get_wrapped()->createInstance(&event_instance);
    if (result != FMOD_OK) {
        new_state->release();
        event_instance = nullptr;
        return fail(result, "Cannot create the FMOD PCM event");
    }

    result = event_instance->setUserData(new_state);
    if (result != FMOD_OK) {
        event_instance->release();
        event_instance = nullptr;
        new_state->release();
        return fail(result, "Cannot attach state to the FMOD PCM event");
    }

    const FMOD_STUDIO_EVENT_CALLBACK_TYPE callback_mask = static_cast<FMOD_STUDIO_EVENT_CALLBACK_TYPE>(
      FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND | FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND | FMOD_STUDIO_EVENT_CALLBACK_DESTROYED
    );
    result = event_instance->setCallback(event_callback, callback_mask);
    if (result != FMOD_OK) {
        event_instance->setUserData(nullptr);
        event_instance->release();
        event_instance = nullptr;
        new_state->release();
        return fail(result, "Cannot set the FMOD PCM event callback");
    }

    new_state->retain();
    state = new_state;
    last_error = "";
    return true;
}

bool FmodPcmStream::start() {
    if (!is_valid()) {
        last_error = "FmodPcmStream is not initialized.";
        GODOT_LOG_ERROR(last_error)
        return false;
    }
    if (started) { return true; }
    const FMOD_RESULT result = event_instance->start();
    if (result != FMOD_OK) { return fail(result, "Cannot start the FMOD PCM event"); }
    started = true;
    return true;
}

void FmodPcmStream::stop(bool allow_fade_out) {
    if (event_instance == nullptr) { return; }
    if (state != nullptr) { state->closing.store(true, std::memory_order_release); }
    event_instance->stop(allow_fade_out ? FMOD_STUDIO_STOP_ALLOWFADEOUT : FMOD_STUDIO_STOP_IMMEDIATE);
    event_instance->release();
    event_instance = nullptr;
    started = false;
}

int FmodPcmStream::push_pcm(const PackedFloat32Array& pcm) {
    if (state == nullptr || state->closing.load(std::memory_order_acquire) || pcm.is_empty()) { return 0; }
    if (pcm.size() % state->channels != 0) {
        last_error = "PCM sample count must contain complete frames.";
        GODOT_LOG_ERROR(last_error)
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->buffer_mutex);
    const size_t capacity = state->buffer.size();
    size_t source_offset = 0;
    size_t samples_to_write = static_cast<size_t>(pcm.size());

    if (samples_to_write > capacity) {
        source_offset = samples_to_write - capacity;
        source_offset -= source_offset % static_cast<size_t>(state->channels);
        samples_to_write = capacity;
        state->overflows.fetch_add(source_offset / static_cast<size_t>(state->channels), std::memory_order_relaxed);
    }

    if (state->queued_samples + samples_to_write > capacity) {
        const size_t samples_to_drop = state->queued_samples + samples_to_write - capacity;
        state->read_index = (state->read_index + samples_to_drop) % capacity;
        state->queued_samples -= samples_to_drop;
        state->overflows.fetch_add(samples_to_drop / static_cast<size_t>(state->channels), std::memory_order_relaxed);
    }

    const float* source = pcm.ptr() + source_offset;
    size_t write_index = (state->read_index + state->queued_samples) % capacity;
    const size_t first_part = std::min(samples_to_write, capacity - write_index);
    std::memcpy(state->buffer.data() + write_index, source, first_part * sizeof(float));
    if (first_part < samples_to_write) {
        std::memcpy(state->buffer.data(), source + first_part, (samples_to_write - first_part) * sizeof(float));
    }
    state->queued_samples += samples_to_write;
    return static_cast<int>(samples_to_write / static_cast<size_t>(state->channels));
}

void FmodPcmStream::clear() {
    if (state == nullptr) { return; }
    std::lock_guard<std::mutex> lock(state->buffer_mutex);
    state->read_index = 0;
    state->queued_samples = 0;
    state->buffering = true;
}

bool FmodPcmStream::is_valid() const {
    return state != nullptr && event_instance != nullptr && event_instance->isValid();
}

bool FmodPcmStream::is_started() const {
    return started;
}

bool FmodPcmStream::is_buffering() const {
    if (state == nullptr) { return true; }
    std::lock_guard<std::mutex> lock(state->buffer_mutex);
    return state->buffering;
}

int FmodPcmStream::get_queued_frames() const {
    if (state == nullptr) { return 0; }
    std::lock_guard<std::mutex> lock(state->buffer_mutex);
    return static_cast<int>(state->queued_samples / static_cast<size_t>(state->channels));
}

uint64_t FmodPcmStream::get_underrun_count() const {
    return state == nullptr ? 0 : state->underruns.load(std::memory_order_relaxed);
}

uint64_t FmodPcmStream::get_overflow_count() const {
    return state == nullptr ? 0 : state->overflows.load(std::memory_order_relaxed);
}

uint64_t FmodPcmStream::get_rebuffer_count() const {
    return state == nullptr ? 0 : state->rebuffers.load(std::memory_order_relaxed);
}

String FmodPcmStream::get_last_error() const {
    return last_error;
}

void FmodPcmStream::set_3d_attributes(const Transform3D& transform) const {
    if (event_instance == nullptr) { return; }
    const FMOD_3D_ATTRIBUTES attributes = get_3d_attributes_from_transform3d(transform, distance_scale);
    ERROR_CHECK(event_instance->set3DAttributes(&attributes));
}

void FmodPcmStream::set_node_attributes(Node* node) const {
    if (event_instance == nullptr || node == nullptr || !node->is_inside_tree()) { return; }
    if (auto* canvas_item = Node::cast_to<CanvasItem>(node)) {
        const FMOD_3D_ATTRIBUTES attributes = get_3d_attributes_from_transform2d(canvas_item->get_global_transform(), distance_scale);
        ERROR_CHECK(event_instance->set3DAttributes(&attributes));
        return;
    }
    if (auto* node_3d = Node::cast_to<Node3D>(node)) {
        const FMOD_3D_ATTRIBUTES attributes = get_3d_attributes_from_transform3d(node_3d->get_global_transform(), distance_scale);
        ERROR_CHECK(event_instance->set3DAttributes(&attributes));
    }
}

void FmodPcmStream::set_distance_scale(float scale) {
    distance_scale = scale;
}

FMOD_RESULT F_CALL FmodPcmStream::event_callback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters) {
    auto* instance = reinterpret_cast<FMOD::Studio::EventInstance*>(event);
    void* user_data = nullptr;
    if (instance->getUserData(&user_data) != FMOD_OK || user_data == nullptr) { return FMOD_ERR_INVALID_PARAM; }
    auto* stream_state = static_cast<StreamState*>(user_data);

    if (type == FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND) {
        FMOD::Sound* stream_sound = stream_state->sound.load(std::memory_order_acquire);
        if (stream_sound == nullptr) { return FMOD_ERR_INVALID_HANDLE; }
        auto* properties = static_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters);
        properties->sound = reinterpret_cast<FMOD_SOUND*>(stream_sound);
        properties->subsoundIndex = -1;
    } else if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND) {
        stream_state->release_sound();
    } else if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROYED) {
        instance->setUserData(nullptr);
        stream_state->release();
    }

    return FMOD_OK;
}

FMOD_RESULT F_CALL FmodPcmStream::pcm_read_callback(FMOD_SOUND* sound, void* data, unsigned int data_length) {
    auto* core_sound = reinterpret_cast<FMOD::Sound*>(sound);
    void* user_data = nullptr;
    if (core_sound->getUserData(&user_data) != FMOD_OK || user_data == nullptr) {
        std::memset(data, 0, data_length);
        return FMOD_OK;
    }
    auto* stream_state = static_cast<StreamState*>(user_data);
    auto* output = static_cast<float*>(data);
    const size_t requested_samples = data_length / sizeof(float);
    std::memset(data, 0, data_length);

    if (stream_state->closing.load(std::memory_order_acquire)) { return FMOD_OK; }

    std::unique_lock<std::mutex> lock(stream_state->buffer_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        stream_state->underruns.fetch_add(1, std::memory_order_relaxed);
        return FMOD_OK;
    }

    if (stream_state->buffering) {
        if (stream_state->queued_samples < stream_state->start_threshold_samples) { return FMOD_OK; }
        stream_state->buffering = false;
    }

    const size_t samples_to_read = std::min(requested_samples, stream_state->queued_samples);
    const size_t capacity = stream_state->buffer.size();
    const size_t first_part = std::min(samples_to_read, capacity - stream_state->read_index);
    std::memcpy(output, stream_state->buffer.data() + stream_state->read_index, first_part * sizeof(float));
    if (first_part < samples_to_read) {
        std::memcpy(output + first_part, stream_state->buffer.data(), (samples_to_read - first_part) * sizeof(float));
    }
    stream_state->read_index = (stream_state->read_index + samples_to_read) % capacity;
    stream_state->queued_samples -= samples_to_read;

    if (samples_to_read < requested_samples) {
        stream_state->underruns.fetch_add(1, std::memory_order_relaxed);
        stream_state->rebuffers.fetch_add(1, std::memory_order_relaxed);
        stream_state->buffering = true;
    }

    return FMOD_OK;
}

void FmodPcmStream::release_owner_state() {
    if (state != nullptr) {
        state->release();
        state = nullptr;
    }
}

FmodPcmStream::~FmodPcmStream() {
    stop(false);
    release_owner_state();
}
