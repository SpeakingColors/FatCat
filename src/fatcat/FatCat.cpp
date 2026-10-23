#include "FatCat.h"
#include "../common/Viz.h"

using namespace catfx;

namespace
{
// Smooth analog-style clip. Up to about 75 % drive the knee is wide and rounded (thick and smooth);
// from 75 % to 100 % the knee tightens progressively so it turns harsher only at the very end.
// boost 1.4 is the shipped strength (about 40 % more obvious than the first version).
struct SatShape
{
    float g = 1.f, p = 1.6f, norm = 1.f;
    SatShape() = default;
    explicit SatShape(float drive, float boost = 1.f)
    {
        const float t = juce::jlimit(0.f, 1.f, (drive - 0.75f) / 0.25f), sm = t * t * (3.f - 2.f * t);
        g = 1.f + 22.f * drive * boost; p = 1.6f + 4.4f * sm;
        norm = std::pow(1.f + std::pow(g, p), 1.f / p) / g;      // so a full-scale input still maps to full scale
    }
    float operator()(float x) const { const float u = x * g; return u / std::pow(1.f + std::pow(std::abs(u), p), 1.f / p) * norm; }
};

// PurrOne colour stack: opamp tanh Drive, asymmetric Transistor (even harmonics), lopsided Fuzz (Grit). Amounts 0 to 1.
float sstep(float t) { t = juce::jlimit(0.f, 1.f, t); return t * t * (3.f - 2.f * t); }
float asymTanh(float u, float nl) { return u >= 0.f ? std::tanh(u) : nl * std::tanh(u / nl); }
float purrShaper(float x, float drive, float xs, float fuzz)
{
    const float dn = sstep((drive - 0.8f) * 5.f), xn = sstep((xs - 0.8f) * 5.f), fn = sstep((fuzz - 0.8f) * 5.f);
    float y = x;
    if (drive > 0.001f) { y = std::tanh(x * (1.f + drive * 5.f + dn * 30.f)); if (dn > 0.f) y += dn * (std::tanh(y * (2.f + 10.f * dn)) - y); }
    if (xs > 0.001f)
    {
        const float nl = std::max(0.25f, 1.f - 0.3f * xs - 0.25f * xn), u = y * (1.f + xs * 5.f + xn * 30.f);
        y = asymTanh(u, nl); y += (0.3f * xs + 0.25f * xn) * y * y;
        if (xn > 0.f) { const float y2 = asymTanh(y * (2.f + 10.f * xn) + 0.15f * xn, nl); y += xn * (y2 - y); }
    }
    if (fuzz > 0.001f)
    {
        const float fz = 1.f + fuzz * 14.f + fn * 70.f, bf = 0.5f * std::min(1.f, fuzz);
        y = std::tanh(y * fz + bf) - std::tanh(bf);
        if (fn > 0.f) { const float r = std::sqrt(y * y + 0.02f) - 0.1414f; y += fn * 0.7f * (r - 0.3f); y = std::tanh(y * (1.f + 0.8f * fn)); }
    }
    return y;
}
// level match so the stack does not get quieter as it gets nastier
float purrMakeup(float drive, float xs, float fuzz, float ref)
{
    float a = 0.f, b = 0.f;
    for (int i = 0; i < 64; ++i) { const float x = ref * std::sin(juce::MathConstants<float>::twoPi * (float) i / 64.f), y = purrShaper(x, drive, xs, fuzz); a += x * x; b += y * y; }
    const float m = b > 1.0e-9f ? std::sqrt(a / b) : 1.f;
    return juce::jlimit(0.25f, 4.f, m);
}

// The curve the sound goes through. Shipped: stronger plain saturation, with optional Transistor and Grit stages on top.
// PurrOne stack: the three stages replace it.
struct FatShaper
{
    bool pur = false, stage2 = false; SatShape base; float d = 0.f, x = 0.f, z = 0.f, x1 = 0.f, z1 = 0.f, mk = 1.f;
    void set(float drive, float xs, float gz, bool purrOne)
    {
        pur = purrOne; d = drive; x = xs; z = gz;
        if (pur) { mk = purrMakeup(d, x, z, 0.7f); stage2 = false; }
        else
        {
            base = SatShape(drive, 1.4f); x1 = std::min(2.f, xs * 1.4f); z1 = std::min(2.f, gz * 1.4f);
            stage2 = xs > 0.f || gz > 0.f; mk = stage2 ? purrMakeup(0.f, x1, z1, 0.95f) : 1.f;
        }
    }
    float operator()(float v) const
    {
        if (pur) return purrShaper(v, d, x, z) * mk;
        const float b = base(v);
        return stage2 ? purrShaper(b, 0.f, x1, z1) * mk : b;
    }
    // amount of the PurrOne nastiness (closes the fizz low-pass as it grows)
    float nast() const { return pur ? std::max({ sstep((d - 0.8f) * 5.f), sstep((x - 0.8f) * 5.f), sstep((z - 0.8f) * 5.f) }) : 0.f; }
};

// hidden soft clipper: identity under 0.7, smooth knee up to 0.97
float softClipHidden(float v)
{
    constexpr float k = 0.7f, c = 0.97f; const float m = std::abs(v);
    return m <= k ? v : std::copysign(k + (c - k) * std::tanh((m - k) / (c - k)), v);
}
}

FatCat::FatCat()
    : FxBase("Fat Cat",
        { { "drive", "Drive", " %", 0.f, 100.f, 40.f, 0.f, "Drive: how hard the sound is saturated. Even at 0 % a very light touch of warmth stays on by design. Dynamics are kept at any input level. With the PurrOne stack character this is the opamp-style tanh stage; above 80 % it stacks a second stage.", 300.f },
          { "xs", "Transistor", " %", 0.f, 100.f, 0.f, 0.f, "Transistor: asymmetric soft clip with even harmonics, warm and thick, not buzzy. Adds on top of Drive in Shipped, and is the middle stage of the PurrOne stack.", 200.f },
          { "grit", "Grit", " %", 0.f, 100.f, 0.f, 0.f, "Grit: lopsided fuzz-style clip, above 80 % it adds a smooth octave-up growl. Adds on top of Drive in Shipped, and is the last stage of the PurrOne stack.", 200.f },
          { "fuzz", "Fuzz", " %", 0.f, 100.f, 0.f, 0.f, "Fuzz: adds clipped high frequencies (above about 1.5 kHz) for a buzzy, raw top end.", 300.f },
          { "color", "Color", " %", 0.f, 100.f, 0.f, 0.f, "Color: emphasises one band of the sound before it is saturated. Low = bass and warmth, high = bite and air." },
          { "turbo", "Stereo Turbo", " %", 0.f, 100.f, 0.f, 0.f, "Stereo Turbo: delays the right channel by a few milliseconds for width. Check the mix in mono, too much can thin it out." },
          { "mix", "Mix", " %", 0.f, 100.f, 100.f, 0.f, "Mix: blend between the original (dry) and the saturated (wet) sound." },
          { "out", "Output", " dB", -24.f, 12.f, 0.f, 0.f, "Output: final level. Use it to match the loudness of the original when comparing." } },
        { { "schar", "Drive character", { "Shipped", "PurrOne stack" }, 0, "Shipped is the plain saturation (Transistor and Grit can be added on top). The PurrOne stack is the three-stage colour shaper from the PurrOne synth: opamp drive, Neve-style transistor and fuzz, nastier above 80 %, smooth (no digital fizz), with a fizz low-pass that closes as it gets nastier." } })
{
    particles.reserve(400); for (auto& v : ringL) v = 0.f; for (auto& v : ringR) v = 0.f;
    pDrive = apvts.getRawParameterValue("drive"); pXs = apvts.getRawParameterValue("xs"); pGrit = apvts.getRawParameterValue("grit");
    pChar = apvts.getRawParameterValue("schar"); pFuzz = apvts.getRawParameterValue("fuzz"); pColor = apvts.getRawParameterValue("color");
    pTurbo = apvts.getRawParameterValue("turbo"); pMix = apvts.getRawParameterValue("mix"); pOut = apvts.getRawParameterValue("out");
}

std::vector<catfx::PanelSpec> FatCat::panels() const
{
    return { { "Saturation", { "drive", "xs", "grit" }, { "schar" }, 1.4f },
             { "Tone", { "color", "fuzz" }, {}, 1.3f },
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
        { "Wall Of Dirt", { { "drive", 95.f }, { "fuzz", 80.f }, { "color", 50.f } } },
        { "Colour Stack", { { "schar", 1.f }, { "drive", 45.f }, { "xs", 45.f }, { "grit", 15.f } } },
        { "Dirty Stack", { { "schar", 1.f }, { "drive", 70.f }, { "xs", 70.f }, { "grit", 40.f }, { "color", 30.f } } },
        { "Warm Transistor", { { "drive", 35.f }, { "xs", 60.f }, { "mix", 80.f } } },
        { "Gritty Bass", { { "drive", 55.f }, { "xs", 40.f }, { "grit", 55.f }, { "color", 15.f } } } };
}

static float colourFreq(float color) { return 150.f * std::pow(40.f, color * 0.01f); } // 150 Hz to 6 kHz

void FatCat::prepare(double sr, int bs)
{
    const auto chunkMax = (size_t) juce::jmax(kChunk, juce::jmin(bs, kChunk));
    for (auto* o : { &os, &osFz, &osClip }) { o->initProcessing(chunkMax); o->reset(); }
    lat1 = (int) std::lround(os.getLatencyInSamples()); lat2 = (int) std::lround(osFz.getLatencyInSamples()); lat3 = (int) std::lround(osClip.getLatencyInSamples());
    jassert (lat1 + lat2 < kRing - kChunk);
    setLatencySamples(lat1 + lat2 + lat3);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) bs, 1 };
    for (auto& f : band) { f.prepare(spec); f.reset(); f.setType(juce::dsp::StateVariableTPTFilterType::bandpass); f.setResonance(1.4f); }
    for (auto& f : fuzzHp) { f.prepare(spec); f.reset(); f.setType(juce::dsp::StateVariableTPTFilterType::highpass); f.setCutoffFrequency(1500.f); }
    for (auto& f : nastLp) { f.prepare(spec); f.reset(); f.setType(juce::dsp::StateVariableTPTFilterType::lowpass); f.setCutoffFrequency(14000.f); }
    for (auto& r : dryRing) r.assign(kRing, 0.f);
    for (auto& r : wRing) r.assign(kRing, 0.f);
    envRing.assign(kRing, 0.f);
    turboRing.assign(4096, 0.f);
    ringPos = turboPos = 0; env = 0.f; dc[0] = dc[1] = 0.f;
    // 20 ms smoothing on every knob (never slower, so a knob move is heard at once)
    for (auto* s : { &sDrive, &sXs, &sGrit, &sFuzz, &sColor, &sTurbo, &sMix, &sOut }) s->reset(sr, 0.02);
    sDrive.setCurrentAndTargetValue(get("drive")); sXs.setCurrentAndTargetValue(get("xs")); sGrit.setCurrentAndTargetValue(get("grit"));
    sFuzz.setCurrentAndTargetValue(get("fuzz")); sColor.setCurrentAndTargetValue(pColor->load()); sTurbo.setCurrentAndTargetValue(pTurbo->load());
    sMix.setCurrentAndTargetValue(pMix->load()); sOut.setCurrentAndTargetValue(pOut->load());
}

void FatCat::process(juce::AudioBuffer<float>& buf, const juce::AudioBuffer<float>*)
{
    const int n = buf.getNumSamples(), nch = juce::jmin(2, buf.getNumChannels());
    if (nch < 1) return;
    sDrive.setTargetValue(get("drive")); sXs.setTargetValue(get("xs")); sGrit.setTargetValue(get("grit"));
    sFuzz.setTargetValue(get("fuzz")); sColor.setTargetValue(pColor->load()); sTurbo.setTargetValue(pTurbo->load());
    sMix.setTargetValue(pMix->load()); sOut.setTargetValue(pOut->load());
    const bool pur = pChar->load() > 0.5f;
    const float aA = std::exp(-1.f / (0.001f * (float) fs)), aR = std::exp(-1.f / (0.05f * (float) fs));
    const int L12 = lat1 + lat2, tsz = (int) turboRing.size();
    float peakN = 0.f; FatShaper shaper;

    for (int s0 = 0; s0 < n; s0 += kChunk)
    {
        const int len = std::min(kChunk, n - s0);
        // chunk-level values (coefficients follow the 20 ms ramps every 32 samples)
        const float drive = sDrive.skip(len) * 0.01f, xs = sXs.skip(len) * 0.01f, gz = sGrit.skip(len) * 0.01f, fuzz = sFuzz.skip(len) * 0.01f, color = sColor.skip(len);
        shaper.set(drive, xs, gz, pur);
        const float colourK = color * 0.01f * 1.6f, nast = shaper.nast();
        for (int c = 0; c < nch; ++c)
        {
            band[c].setCutoffFrequency(colourFreq(color));
            if (nast > 0.f) nastLp[c].setCutoffFrequency(std::max(3000.f, 14000.f - 10000.f * nast)); else nastLp[c].reset();
        }
        // A: dry copy, colour emphasis, envelope normalisation (base rate)
        for (int i = 0; i < len; ++i)
        {
            const int p = (ringPos + i) & kMask; float pk = 0.f;
            for (int c = 0; c < nch; ++c)
            {
                const float x = buf.getReadPointer(c)[s0 + i];
                dryRing[c][(size_t) p] = x;
                const float y = x + colourK * band[c].processSample(0, x);
                buf.getWritePointer(c)[s0 + i] = y; pk = std::max(pk, std::abs(y));
            }
            const float a = pk > env ? aA : aR; env = a * env + (1.f - a) * pk;
            envRing[(size_t) p] = env;
            const float inv = 1.f / (env + 1.0e-5f);
            for (int c = 0; c < nch; ++c) buf.getWritePointer(c)[s0 + i] *= inv;
            peakN = std::max(peakN, std::abs(buf.getReadPointer(0)[s0 + i]));
        }
        // B: saturation at 4x (nasty ranges alias badly at 2x)
        {
            juce::dsp::AudioBlock<float> block(buf.getArrayOfWritePointers(), (size_t) nch, (size_t) s0, (size_t) len);
            auto up = os.processSamplesUp(block);
            for (size_t c = 0; c < up.getNumChannels(); ++c) { auto* d = up.getChannelPointer(c); for (size_t i = 0; i < up.getNumSamples(); ++i) d[i] = shaper(juce::jlimit(-8.f, 8.f, d[i])); }
            os.processSamplesDown(block);
        }
        // C: re-apply the latency aligned envelope, optional fizz low-pass, DC block, prepare the fuzz stage input
        for (int i = 0; i < len; ++i)
        {
            const int p = (ringPos + i) & kMask; const float ev1 = envRing[(size_t) ((p - lat1) & kMask)];
            for (int c = 0; c < nch; ++c)
            {
                float w = buf.getReadPointer(c)[s0 + i];
                if (nast > 0.f) w = nastLp[c].processSample(0, w);
                w *= ev1; dc[c] += 0.0005f * (w - dc[c]); w -= dc[c];
                wRing[c][(size_t) p] = w;
                argBuf[(size_t) c][(size_t) i] = fuzzHp[c].processSample(0, w) / (ev1 + 1.0e-5f) * (4.f + 24.f * fuzz);
            }
        }
        // D: fuzz stage (tanh) inside its own 4x oversampled block, so it does not alias
        const bool fuzzOn = fuzz > 0.f;
        if (fuzzOn)
        {
            float* ptr[2] { argBuf[0].data(), argBuf[1].data() };
            juce::dsp::AudioBlock<float> ab(ptr, (size_t) nch, (size_t) len);
            auto up = osFz.processSamplesUp(ab);
            for (size_t c = 0; c < up.getNumChannels(); ++c) { auto* d = up.getChannelPointer(c); for (size_t i = 0; i < up.getNumSamples(); ++i) d[i] = std::tanh(d[i]); }
            osFz.processSamplesDown(ab);
        }
        // E: add fuzz, dry/wet, output, stereo turbo (all aligned to the same latency)
        for (int i = 0; i < len; ++i)
        {
            const int p = (ringPos + i) & kMask; const float ev = envRing[(size_t) ((p - L12) & kMask)];
            const float mix = sMix.getNextValue() * 0.01f, outG = juce::Decibels::decibelsToGain(sOut.getNextValue()), dl = std::min((float) (tsz - 2), sTurbo.getNextValue() * 0.01f * 0.012f * (float) fs);
            float wet[2] { 0.f, 0.f };
            for (int c = 0; c < nch; ++c)
            {
                float w = wRing[c][(size_t) ((p - lat2) & kMask)];
                if (fuzzOn) w += fuzz * 0.5f * argBuf[(size_t) c][(size_t) i] * ev;
                const float dry = dryRing[c][(size_t) ((p - L12) & kMask)];
                wet[c] = (w * mix + dry * (1.f - mix)) * outG;
            }
            if (nch == 2)
            {
                turboRing[(size_t) turboPos] = wet[1];
                const float rp = (float) turboPos - dl, rf = rp < 0.f ? rp + (float) tsz : rp; const int i0 = (int) rf, i1 = (i0 + 1) % tsz; const float fr = rf - (float) i0;
                wet[1] = turboRing[(size_t) i0] * (1.f - fr) + turboRing[(size_t) i1] * fr;
                turboPos = (turboPos + 1) % tsz;
            }
            for (int c = 0; c < nch; ++c) buf.getWritePointer(c)[s0 + i] = wet[c];
            { const int w = ringW.load(std::memory_order_relaxed); ringL[w & 127].store(wet[0], std::memory_order_relaxed); ringR[w & 127].store(nch > 1 ? wet[1] : wet[0], std::memory_order_relaxed); ringW.store(w + 1, std::memory_order_relaxed); }
        }
        // F: hidden soft clipper at 2x (no knob), right before the safety limiter
        {
            juce::dsp::AudioBlock<float> block(buf.getArrayOfWritePointers(), (size_t) nch, (size_t) s0, (size_t) len);
            auto up = osClip.processSamplesUp(block);
            for (size_t c = 0; c < up.getNumChannels(); ++c) { auto* d = up.getChannelPointer(c); for (size_t i = 0; i < up.getNumSamples(); ++i) d[i] = softClipHidden(d[i]); }
            osClip.processSamplesDown(block);
        }
        ringPos = (ringPos + len) & kMask;
    }
    lastN = peakN;
}

void FatCat::paintPanel(int idx, juce::Graphics& g, juce::Rectangle<float> r)
{
    using namespace catfx::viz;
    g.setFont(juce::FontOptions(11.f));
    if (idx == 0)       // saturation curve
    {
        FatShaper shape; shape.set(get("drive") * 0.01f, get("xs") * 0.01f, get("grit") * 0.01f, get("schar") > 0.5f); const float gn = shape.base.g;
        auto p = r.withTrimmedLeft(8.f).reduced(10.f, 14.f); p.removeFromBottom(10.f);
        g.setColour(grid()); g.drawRect(p); g.drawLine(p.getX(), p.getBottom(), p.getRight(), p.getY());
        juce::Path path;
        for (int i = 0; i <= 160; ++i) { const float x = (float) i / 160.f, y = juce::jlimit(0.f, 1.2f, shape(x)); const float px = p.getX() + x * p.getWidth(), py = p.getBottom() - y * p.getHeight(); i ? path.lineTo(px, py) : path.startNewSubPath(px, py); }
        g.setColour(accent()); g.strokePath(path, juce::PathStrokeType(2.f));
        const float lv = juce::jlimit(0.f, 1.f, lastN.load()), yy = shape(lv);
        g.setColour(juce::Colours::white); g.fillEllipse(p.getX() + lv * p.getWidth() - 3.f, p.getBottom() - yy * p.getHeight() - 3.f, 6.f, 6.f);
        g.setColour(green());
        g.drawText("output", (int) r.getX() + 2, (int) r.getY() + 1, 60, 11, juce::Justification::left);
        g.setColour(accent()); g.drawText(shape.pur ? juce::String("PurrOne: drive + transistor + grit") : "shipped: drive x" + juce::String(gn, 1) + (shape.stage2 ? " + transistor/grit" : ""), (int) r.getRight() - 190, (int) r.getY() + 1, 186, 11, juce::Justification::right);
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
        g.setFont(juce::FontOptions(10.f));
        g.setColour(accent()); g.drawText("side", (int) p.getRight() - 30, (int) cy + 2, 28, 10, juce::Justification::right); g.setColour(accent()); g.drawText("mid", (int) cx + 3, (int) p.getY() + 1, 30, 10, juce::Justification::left);
        g.setColour(blue()); g.drawText("right delay " + juce::String(get("turbo") * 0.01f * 12.f, 1) + " ms", p.toNearestInt().withY((int) p.getBottom() - 12).withHeight(11), juce::Justification::centred);
    }
    else                // level: input and output with dB scales
    {
        auto area = r.reduced(6.f, 6.f);
        auto left = area.removeFromLeft(area.getWidth() * 0.5f);
        meter(g, left, inLevel.load(), "IN dB");
        meter(g, area, outLevel.load(), "OUT dB");
    }
}
