#pragma once

#include <cstdint>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace sonar::audio {

// Create a pw_stream with `Process` as its realtime callback and connect it offering
// F32 at `rate`, mono or FL/FR. nullptr on failure; `props` is consumed either way.
// `Process` is a template parameter so each callback gets its own static events
// table, which pw_stream keeps a pointer to.
template <void (*Process)(void*)>
[[nodiscard]] pw_stream* openF32Stream(pw_core* core, const char* name, pw_properties* props,
                                       spa_hook& listener, void* owner, pw_direction direction,
                                       pw_stream_flags flags, std::uint32_t rate,
                                       std::uint32_t channels) {
    pw_stream* stream = pw_stream_new(core, name, props);
    if (stream == nullptr) return nullptr;
    static const pw_stream_events kEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = Process,
    };
    pw_stream_add_listener(stream, &listener, &kEvents, owner);

    spa_audio_info_raw format = {};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = rate;
    format.channels = channels;
    if (channels == 1) {
        format.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        format.position[0] = SPA_AUDIO_CHANNEL_FL;
        format.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    std::uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};

    if (pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1) < 0) {
        spa_hook_remove(&listener);
        pw_stream_destroy(stream);
        return nullptr;
    }
    return stream;
}

// Detach and destroy a stream opened above; safe on nullptr.
inline void closeStream(pw_stream*& stream, spa_hook& listener) {
    if (stream != nullptr) {
        spa_hook_remove(&listener);
        pw_stream_destroy(stream);
        stream = nullptr;
    }
}

// The flags every realtime stream here uses, plus AUTOCONNECT when asked.
[[nodiscard]] inline pw_stream_flags rtStreamFlags(bool autoconnect) {
    const int flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                      (autoconnect ? PW_STREAM_FLAG_AUTOCONNECT : 0);
    return static_cast<pw_stream_flags>(flags);
}

}  // namespace sonar::audio
