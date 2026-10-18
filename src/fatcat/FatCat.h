#pragma once
#include "common/FxBase.h"

// Fat Cat: a dynamics-preserving saturator. The input is divided by its own envelope, saturated, then multiplied by the
// envelope again, so the effect works the same at any input level and the dynamics stay. Colour emphasises a band before
// the distortion, Fuzz adds clipped high frequencies, Stereo Turbo delays the right channel a little for width.
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
    juce::dsp::Oversampling<float> os { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    juce::dsp::StateVariableTPTFilter<float> band[2], fuzzHp[2];
    std::vector<float> dryRing[2], envRing, turboRing;
    std::vector<float> nBuf[2];
    int ringPos = 0, turboPos = 0, latency = 0;
    float env = 0.f, dc[2] { 0.f, 0.f };
    std::atomic<float> lastN { 0.f };
    // recent stereo samples (after turbo) for the particle display
    std::atomic<float> ringL[128], ringR[128];
    std::atomic<int> ringW { 0 };
    struct Particle { float x, y, vx, vy, life; };
    std::vector<Particle> particles;
    juce::Random rnd;
};
