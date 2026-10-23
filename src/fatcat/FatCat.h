#pragma once
#include <array>
#include "common/FxBase.h"

// Fat Cat: a dynamics-preserving saturator. The input is divided by its own envelope, saturated, then multiplied by the
// envelope again, so the effect works the same at any input level and the dynamics stay. Colour emphasises a band before
// the distortion, Transistor and Grit add two more saturation stages (Shipped) or replace the curve with the three-stage
// PurrOne stack, Fuzz adds clipped high frequencies (inside its own oversampled stage), Stereo Turbo delays the right
// channel a little for width. A hidden soft clipper (no knob) shaves only the tallest peaks before the safety limiter.
// Every knob is smoothed (20 ms) and nothing on the audio thread allocates or waits.
// Original design and code; not affiliated with and containing nothing from any other saturator.
class FatCat : public catfx::FxBase
{
public:
    FatCat();
    void prepare(double sr, int maxBlock) override;
    void process(juce::AudioBuffer<float>&, const juce::AudioBuffer<float>*) override;
    std::vector<catfx::PanelSpec> panels() const override;
    void paintPanel(int index, juce::Graphics&, juce::Rectangle<float>) override;
    std::vector<Preset> presets() const override;
    juce::StringArray randomExclude() const override { return { "out" }; }
private:
    static constexpr int kChunk = 32, kRing = 256, kMask = kRing - 1;
    // 4x oversampling (saturation and fuzz stage) and 2x (hidden clipper), integer latency; each stage has its own
    juce::dsp::Oversampling<float> os { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true },
                                   osFz { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true },
                                   osClip { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
    juce::dsp::StateVariableTPTFilter<float> band[2], fuzzHp[2], nastLp[2];
    std::vector<float> dryRing[2], wRing[2], envRing, turboRing;
    std::array<std::array<float, kChunk>, 2> argBuf {};
    int ringPos = 0, turboPos = 0, lat1 = 0, lat2 = 0, lat3 = 0;
    float env = 0.f, dc[2] { 0.f, 0.f };
    // parameter pointers (looked up once, so the audio thread never builds strings) and 20 ms smoothers
    std::atomic<float> *pDrive {}, *pXs {}, *pGrit {}, *pChar {}, *pFuzz {}, *pColor {}, *pTurbo {}, *pMix {}, *pOut {};
    juce::SmoothedValue<float> sDrive, sXs, sGrit, sFuzz, sColor, sTurbo, sMix, sOut;
    std::atomic<float> lastN { 0.f };
    // recent stereo samples (after turbo) for the particle display
    std::atomic<float> ringL[128], ringR[128];
    std::atomic<int> ringW { 0 };
    struct Particle { float x, y, vx, vy, life; };
    std::vector<Particle> particles;
    juce::Random rnd;
};
