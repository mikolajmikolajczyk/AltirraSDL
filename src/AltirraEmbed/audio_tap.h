// EmbedAudioTap — receives raw PCM samples from ATAudioOutput's mixer
// pipeline before native delivery and buffers them for the JS host to pull
// via AltirraCore::getAudioSamples() each frame.
//
// Mono only: the host (madside) feeds Web Audio's 2-channel context which
// auto-mixes mono → stereo. Buffer is capped to ~0.5s @ 48kHz so a paused
// host doesn't grow it unboundedly.

#pragma once

#ifdef __EMSCRIPTEN__

#include <at/ataudio/audiooutput.h>
#include <cstdint>
#include <vector>

class EmbedAudioTap : public IATAudioTap {
public:
    void WriteRawAudio(const float* left, const float* right,
                       uint32_t count, uint32_t timestamp) override;

    std::vector<float> mBuf;
};

// Process-global singleton — `g_sim.GetAudioOutput()->SetAudioTap()` wires
// it in during EnsureInitialized().
extern EmbedAudioTap g_audioTap;

#endif  // __EMSCRIPTEN__
