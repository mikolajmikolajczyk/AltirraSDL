// EmbedAudioTap implementation. See audio_tap.h for the rationale.

#include <stdafx.h>

#ifdef __EMSCRIPTEN__

#include "audio_tap.h"
#include <cstring>

EmbedAudioTap g_audioTap;

void EmbedAudioTap::WriteRawAudio(const float* left, const float* /*right*/,
                                  uint32_t count, uint32_t /*timestamp*/) {
    // Stereo when `right` is non-null, otherwise mono in `left`. We store
    // the left channel only — Web Audio mixes mono → 2-channel automatically
    // (matches AltirraSDL's pushStereoAsMono fast path the simulator uses by
    // default). Stereo support deferred until the host plays in stereo.
    const size_t before = mBuf.size();
    mBuf.resize(before + count);
    std::memcpy(mBuf.data() + before, left, count * sizeof(float));

    // Cap ring buffer to ~0.5s @ 48kHz so a paused host doesn't grow it
    // unboundedly.
    constexpr size_t kMax = 24000;
    if (mBuf.size() > kMax) {
        mBuf.erase(mBuf.begin(), mBuf.begin() + (mBuf.size() - kMax));
    }
}

#endif  // __EMSCRIPTEN__
