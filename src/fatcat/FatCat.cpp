#include "FatCat.h"

using namespace catfx;

FatCat::FatCat()
    : FxBase("Fat Cat",
        { { "drive", "Drive", " %", 0.f, 100.f, 40.f, 0.f, "Drive: how hard the sound is saturated. Even at 0 % a very light touch of warmth stays on by design. Dynamics are kept at any input level." },
          { "fuzz", "Fuzz", " %", 0.f, 100.f, 0.f, 0.f, "Fuzz: adds clipped high frequencies (above about 1.5 kHz) for a buzzy, raw top end." },
          { "color", "Color", " %", 0.f, 100.f, 0.f, 0.f, "Color: emphasises one band of the sound before it is saturated. Low = bass and warmth, high = bite and air." },
          { "turbo", "Stereo Turbo", " %", 0.f, 100.f, 0.f, 0.f, "Stereo Turbo: delays the right channel by a few milliseconds for width. Check the mix in mono, too much can thin it out." },
          { "mix", "Mix", " %", 0.f, 100.f, 100.f, 0.f, "Mix: blend between the original (dry) and the saturated (wet) sound." },
          { "out", "Output", " dB", -24.f, 12.f, 0.f, 0.f, "Output: final level. Use it to match the loudness of the original when comparing." } },
        {}) { particles.reserve(400); for (auto& v : ringL) v = 0.f; for (auto& v : ringR) v = 0.f; }

std::vector<catfx::PanelSpec> FatCat::panels() const
{
    return { { "Saturation", { "drive" }, {}, 1.f },
             { "Tone", { "color", "fuzz" }, {}, 1.5f },
             { "Stereo", { "turbo" }, {}, 1.f },
             { "Level", { "mix", "out" }, {}, 1.2f } };
}

std::vector<FxBase::Preset> FatCat::presets() const
{
    return {
        { "Init", { } },
        { "Subtle Glue", { { "drive", 20.f }, { "mix", 60.f } } },
        { "Fat Drums", { { "drive", 55.f }, { "color", 25.f }, { "mix", 75.f } } },
        { "Chonky Bass", { { "drive", 70.f }, { "color", 15.f } } },
        { "Fuzzy Lead", { { "drive", 60.f }, { "fuzz", 55.f }, { "color", 60.f } } },
        { "Wide Pad", { { "drive", 30.f }, { "turbo", 65.f }, { "color", 40.f }, { "mix", 70.f } } },
        { "Wall Of Dirt", { { "drive", 95.f }, { "fuzz", 80.f }, { "color", 50.f } } } };
}

static float colourFreq(float color) { return 150.f * std::pow(40.f, color * 0.01f); } // 150 Hz to 6 kHz

void FatCat::prepare(double sr, int bs)
{
    os.initProcessing((size_t) bs);
    os.reset();
    latency = (int) std::lround(os.getLatencyInSamples());
    setLatencySamples(latency);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) bs, 1 };
    for (auto& f : band) { f.prepare(spec); f.reset(); f.setType(juce::dsp::StateVariableTPTFilterType::bandpass); }
    for (auto& f : fuzzHp) { f.prepare(spec); f.reset(); f.setType(juce::dsp::StateVariableTPTFilterType::highpass); f.setCutoffFrequency(1500.f); }
    for (auto& r : dryRing) r.assign(256, 0.f);
    for (auto& v : nBuf) v.assign((size_t) bs, 0.f);
    envRing.assign(256, 0.f);
    turboRing.assign(4096, 0.f);
    ringPos = turboPos = 0; env = 0.f; dc[0] = dc[1] = 0.f;
}

void FatCat::process(juce::AudioBuffer<float>& buf, const juce::AudioBuffer<float>*)
{
    const int n = buf.getNumSamples(), nch = juce::jmin(2, buf.getNumChannels());
    const float drive = get("drive") * 0.01f, fuzz = get("fuzz") * 0.01f, color = get("color"), turbo = get("turbo") * 0.01f;
    const float mix = get("mix") * 0.01f, outG = juce::Decibels::decibelsToGain(get("out"));
    const float g = 1.f + drive * 24.f, norm = 1.f / std::tanh(g);
    const float aA = std::exp(-1.f / (0.001f * (float) fs)), aR = std::exp(-1.f / (0.05f * (float) fs));
    const float colourK = color * 0.01f * 1.6f;
    for (int c = 0; c < nch; ++c) { band[c].setCutoffFrequency(colourFreq(color)); band[c].setResonance(1.4f); }

    // dry (delayed to match latency), colour emphasis and envelope normalisation at the base rate
    std::vector<float>& e = envRing;
    for (int i = 0; i < n; ++i)
    {
        float pk = 0.f;
        for (int c = 0; c < nch; ++c)
        {
            const float x = buf.getReadPointer(c)[i];
            dryRing[c][(size_t) ((ringPos + i) & 255)] = x;
            const float y = x + colourK * band[c].processSample(0, x);
            buf.getWritePointer(c)[i] = y;
            pk = std::max(pk, std::abs(y));
        }
        const float a = pk > env ? aA : aR; env = a * env + (1.f - a) * pk;
        e[(size_t) ((ringPos + i) & 255)] = env;
        const float inv = 1.f / (env + 1.0e-5f);
        for (int c = 0; c < nch; ++c) buf.getWritePointer(c)[i] *= inv;
    }
    { float m = 0.f; for (int i = 0; i < n; ++i) m = std::max(m, std::abs(buf.getReadPointer(0)[i])); lastN = m; }

    juce::dsp::AudioBlock<float> block(buf.getArrayOfWritePointers(), (size_t) nch, (size_t) n);
    auto up = os.processSamplesUp(block);
    for (size_t c = 0; c < up.getNumChannels(); ++c)
    {
        auto* d = up.getChannelPointer(c);
        for (size_t i = 0; i < up.getNumSamples(); ++i) d[i] = std::tanh(juce::jlimit(-8.f, 8.f, d[i]) * g) * norm;
    }
    os.processSamplesDown(block);

    // re-apply the (latency aligned) envelope, add fuzz, mix, turbo, output
    const int dl = std::min(turboRing.size() - 1, (size_t) (turbo * 0.012f * fs));
    for (int i = 0; i < n; ++i)
    {
        const float ev = e[(size_t) ((ringPos + i - latency) & 255)];
        float wet[2] { 0.f, 0.f };
        for (int c = 0; c < nch; ++c)
        {
            float w = buf.getReadPointer(c)[i] * ev;
            dc[c] += 0.0005f * (w - dc[c]); w -= dc[c];
            if (fuzz > 0.f)
            {
                const float h = fuzzHp[c].processSample(0, w);
                w += fuzz * 0.5f * std::tanh(h / (ev + 1.0e-5f) * (4.f + 24.f * fuzz)) * ev;
            }
            const float dry = dryRing[c][(size_t) ((ringPos + i - latency) & 255)];
            wet[c] = (w * mix + dry * (1.f - mix)) * outG;
        }
        if (nch == 2 && dl > 0)
        {
            turboRing[(size_t) turboPos] = wet[1];
            wet[1] = turboRing[(size_t) ((turboPos - dl + (int) turboRing.size()) % (int) turboRing.size())];
            turboPos = (turboPos + 1) % (int) turboRing.size();
        }
        for (int c = 0; c < nch; ++c) buf.getWritePointer(c)[i] = wet[c];
        { const int w = ringW.load(std::memory_order_relaxed); ringL[w & 127].store(wet[0], std::memory_order_relaxed); ringR[w & 127].store(nch > 1 ? wet[1] : wet[0], std::memory_order_relaxed); ringW.store(w + 1, std::memory_order_relaxed); }
    }
    ringPos = (ringPos + n) & 255;
}

void FatCat::paintPanel(int idx, juce::Graphics& g, juce::Rectangle<float> r)
{
    using namespace catfx::viz;
    g.setFont(juce::FontOptions(11.f));
    if (idx == 0)       // saturation curve
    {
        const float gn = 1.f + get("drive") * 0.01f * 24.f, norm = 1.f / std::tanh(gn);
        auto p = r.withTrimmedLeft(8.f).reduced(10.f, 14.f); p.removeFromBottom(10.f);
        g.setColour(grid()); g.drawRect(p); g.drawLine(p.getX(), p.getBottom(), p.getRight(), p.getY());
        juce::Path path;
        for (int i = 0; i <= 160; ++i) { const float x = (float) i / 160.f, y = std::tanh(x * gn) * norm; const float px = p.getX() + x * p.getWidth(), py = p.getBottom() - y * p.getHeight(); i ? path.lineTo(px, py) : path.startNewSubPath(px, py); }
        g.setColour(accent()); g.strokePath(path, juce::PathStrokeType(2.f));
        const float lv = juce::jlimit(0.f, 1.f, lastN.load()), yy = std::tanh(lv * gn) * norm;
        g.setColour(juce::Colours::white); g.fillEllipse(p.getX() + lv * p.getWidth() - 3.f, p.getBottom() - yy * p.getHeight() - 3.f, 6.f, 6.f);
        g.setColour(dim());
        g.drawText("output", (int) r.getX() + 2, (int) r.getY() + 1, 60, 11, juce::Justification::left);
        g.drawText("drive x" + juce::String(gn, 1), (int) r.getRight() - 80, (int) r.getY() + 1, 76, 11, juce::Justification::right);
        g.drawText("level relative to its own peak (dynamics kept)", p.toNearestInt().withY((int) p.getBottom() + 1).withHeight(11), juce::Justification::centred);
    }
    else if (idx == 1)  // tone: colour band and fuzz
    {
        const float fc = colourFreq(get("color")), k = get("color") * 0.01f * 1.6f, fz = get("fuzz") * 0.01f;
        auto p = r.withTrimmedLeft(34.f).withTrimmedRight(8.f).withTrimmedTop(16.f).withTrimmedBottom(14.f);
        g.setColour(bg().brighter(0.04f)); g.fillRect(p);
        dbAxis(g, p, -6.f, 18.f, 6.f); freqAxis(g, p);
        juce::Path cp, fp;
        for (int i = 0; i <= 200; ++i)
        {
            const float fr = 20.f * std::pow(1000.f, (float) i / 200.f), rr = fr / fc, q = 1.4f;
            const float bp = (rr / q) / std::sqrt((1.f - rr * rr) * (1.f - rr * rr) + (rr / q) * (rr / q));
            const float cdb = juce::jlimit(-6.f, 18.f, 20.f * std::log10(std::max(1.f + k * bp, 1e-3f)));
            const float rf = fr / 1500.f, hp = rf * rf / std::sqrt((1.f - rf * rf) * (1.f - rf * rf) + (rf / 0.707f) * (rf / 0.707f));
            const float fdb = juce::jlimit(-6.f, 18.f, 20.f * std::log10(1.f + 2.f * fz * hp));
            const float px = p.getX() + (float) i / 200.f * p.getWidth();
            const float cy = p.getBottom() - (cdb + 6.f) / 24.f * p.getHeight(), fy = p.getBottom() - (fdb + 6.f) / 24.f * p.getHeight();
            i ? cp.lineTo(px, cy) : cp.startNewSubPath(px, cy); i ? fp.lineTo(px, fy) : fp.startNewSubPath(px, fy);
        }
        g.setColour(green()); g.strokePath(cp, juce::PathStrokeType(2.f));
        g.setColour(accent()); g.strokePath(fp, juce::PathStrokeType(2.f));
        g.setColour(green()); g.drawText("color " + juce::String((int) fc) + " Hz", (int) p.getX() + 4, (int) r.getY() + 1, 120, 11, juce::Justification::left);
        g.setColour(accent()); g.drawText("fuzz (added highs)", (int) r.getRight() - 130, (int) r.getY() + 1, 126, 11, juce::Justification::right);
    }
    else if (idx == 2)  // stereo: particle spray from the real left/right signal
    {
        auto p = r.reduced(6.f, 8.f);
        g.setColour(bg().brighter(0.05f)); g.fillRect(p);
        const float cx = p.getCentreX(), cy = p.getCentreY(), sx = p.getWidth() * 0.45f, sy = p.getHeight() * 0.45f;
        g.setColour(grid()); g.drawLine(p.getX(), cy, p.getRight(), cy); g.drawLine(cx, p.getY(), cx, p.getBottom());
        const int w = ringW.load(std::memory_order_relaxed);
        for (int i = 0; i < 24; ++i)   // spawn from the newest samples
        {
            const int j = (w - 1 - i * 3) & 127;
            const float l = ringL[j].load(std::memory_order_relaxed), rr = ringR[j].load(std::memory_order_relaxed);
            if (particles.size() >= 360) break;
            particles.push_back({ (l - rr) * 0.5f, (l + rr) * 0.5f, rnd.nextFloat() * 0.06f - 0.03f, rnd.nextFloat() * 0.06f - 0.03f, 1.f });
        }
        for (auto& pt : particles) { pt.x += pt.vx; pt.y += pt.vy; pt.life -= 0.045f; }
        particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& q) { return q.life <= 0.f; }), particles.end());
        for (auto& pt : particles)
        {
            const float px = cx + juce::jlimit(-1.2f, 1.2f, pt.x * 2.f) * sx, py = cy - juce::jlimit(-1.2f, 1.2f, pt.y) * sy;
            g.setColour(blue().interpolatedWith(accent(), juce::jlimit(0.f, 1.f, std::abs(pt.x) * 3.f)).withAlpha(pt.life * 0.8f));
            g.fillEllipse(px - 1.5f, py - 1.5f, 3.f, 3.f);
        }
        g.setColour(dim()); g.setFont(juce::FontOptions(10.f));
        g.drawText("side", (int) p.getRight() - 30, (int) cy + 2, 28, 10, juce::Justification::right); g.drawText("mid", (int) cx + 3, (int) p.getY() + 1, 30, 10, juce::Justification::left);
        g.drawText("right delay " + juce::String(get("turbo") * 0.01f * 12.f, 1) + " ms", p.toNearestInt().withY((int) p.getBottom() - 12).withHeight(11), juce::Justification::centred);
    }
    else                // level: input and output with dB scales
    {
        auto area = r.reduced(6.f, 6.f);
        auto left = area.removeFromLeft(area.getWidth() * 0.5f);
        meter(g, left, inLevel.load(), "IN dB");
        meter(g, area, outLevel.load(), "OUT dB");
    }
}
