#include "engine/enginelivebandlevels.h"

#include "engine/filters/enginefilterbessel4.h"
#include "moc_enginelivebandlevels.cpp"
#include "util/defs.h"
#include "util/math.h"
#include "util/sample.h"

namespace {

// Same low/mid crossover frequencies AnalyzerWaveform uses to build the
// per-track waveform data, so a live band split (e.g. of the main mix, which
// has no precomputed waveform of its own) lines up with the offline one.
constexpr double kLowMidFreqHz = 600.0;
constexpr double kMidHighFreqHz = 4000.0;

// Rate at which the band-level ControlObjects are updated (using a sample
// rate of 44100 Hz), independent of the audio buffer/callback size.
constexpr unsigned int kLiveBandUpdateRate = 120; // in Hz (1/s)

} // namespace

EngineLiveBandLevels::EngineLiveBandLevels(const QString& group)
        : m_bandLow(ConfigKey(group, QStringLiteral("live_band_low"))),
          m_bandMid(ConfigKey(group, QStringLiteral("live_band_mid"))),
          m_bandHigh(ConfigKey(group, QStringLiteral("live_band_high"))),
          m_pLowBuf(SampleUtil::alloc(kMaxEngineSamples)),
          m_pMidBuf(SampleUtil::alloc(kMaxEngineSamples)),
          m_pHighBuf(SampleUtil::alloc(kMaxEngineSamples)),
          m_filterSampleRate(mixxx::audio::SampleRate()),
          m_sampleRate(QStringLiteral("[App]"), QStringLiteral("samplerate")) {
    reset();
}

EngineLiveBandLevels::~EngineLiveBandLevels() {
    SampleUtil::free(m_pLowBuf);
    SampleUtil::free(m_pMidBuf);
    SampleUtil::free(m_pHighBuf);
}

void EngineLiveBandLevels::createFilters(mixxx::audio::SampleRate sampleRate) {
    m_pFilterLow = std::make_unique<EngineFilterBessel4Low>(sampleRate, kLowMidFreqHz);
    m_pFilterMid = std::make_unique<EngineFilterBessel4Band>(
            sampleRate, kLowMidFreqHz, kMidHighFreqHz);
    m_pFilterHigh = std::make_unique<EngineFilterBessel4High>(sampleRate, kMidHighFreqHz);

    // Settle filters for silence in preroll to avoid ramping noise (same
    // reasoning as AnalyzerWaveform::createFilters, Issue #7776).
    m_pFilterLow->assumeSettled();
    m_pFilterMid->assumeSettled();
    m_pFilterHigh->assumeSettled();

    m_filterSampleRate = sampleRate;
}

void EngineLiveBandLevels::process(CSAMPLE* pInOut, const int iBufferSize) {
    const auto sampleRate = mixxx::audio::SampleRate::fromDouble(m_sampleRate.get());
    if (!sampleRate.isValid()) {
        return;
    }
    if (sampleRate != m_filterSampleRate) {
        createFilters(sampleRate);
    }

    m_pFilterLow->process(pInOut, m_pLowBuf, iBufferSize);
    m_pFilterMid->process(pInOut, m_pMidBuf, iBufferSize);
    m_pFilterHigh->process(pInOut, m_pHighBuf, iBufferSize);

    m_samplesAccumulated += static_cast<unsigned int>(iBufferSize / 2);

    if (m_samplesAccumulated >= (sampleRate / kLiveBandUpdateRate)) {
        const double low = static_cast<double>(
                SampleUtil::rms(m_pLowBuf, static_cast<SINT>(iBufferSize)));
        const double mid = static_cast<double>(
                SampleUtil::rms(m_pMidBuf, static_cast<SINT>(iBufferSize)));
        const double high = static_cast<double>(
                SampleUtil::rms(m_pHighBuf, static_cast<SINT>(iBufferSize)));

        m_bandLow.set(math_clamp(low, 0.0, 1.0));
        m_bandMid.set(math_clamp(mid, 0.0, 1.0));
        m_bandHigh.set(math_clamp(high, 0.0, 1.0));

        m_samplesAccumulated = 0;
    }
}

void EngineLiveBandLevels::reset() {
    m_bandLow.set(0);
    m_bandMid.set(0);
    m_bandHigh.set(0);
    m_samplesAccumulated = 0;
}
