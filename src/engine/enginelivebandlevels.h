#pragma once

#include <memory>

#include "audio/types.h"
#include "control/controlobject.h"
#include "control/pollingcontrolproxy.h"
#include "engine/engineobject.h"

class EngineFilterBessel4Low;
class EngineFilterBessel4Band;
class EngineFilterBessel4High;

// Computes a live low/mid/high band-magnitude split of an audio buffer,
// exposed as ControlObjects (like EngineVuMeter's vu_meter values) so it can
// be polled from outside the engine thread. Unlike the per-track waveform
// data (computed once, offline, by AnalyzerWaveform), this runs every engine
// callback and is intended for signals that have no precomputed waveform of
// their own, e.g. the main mix output.
class EngineLiveBandLevels : public EngineObject {
    Q_OBJECT
  public:
    explicit EngineLiveBandLevels(const QString& group);
    ~EngineLiveBandLevels() override;

    void process(CSAMPLE* pInOut, const int iBufferSize) override;

    void reset();

  private:
    void createFilters(mixxx::audio::SampleRate sampleRate);

    ControlObject m_bandLow;
    ControlObject m_bandMid;
    ControlObject m_bandHigh;

    std::unique_ptr<EngineFilterBessel4Low> m_pFilterLow;
    std::unique_ptr<EngineFilterBessel4Band> m_pFilterMid;
    std::unique_ptr<EngineFilterBessel4High> m_pFilterHigh;

    CSAMPLE* m_pLowBuf;
    CSAMPLE* m_pMidBuf;
    CSAMPLE* m_pHighBuf;

    mixxx::audio::SampleRate m_filterSampleRate;
    PollingControlProxy m_sampleRate;

    unsigned int m_samplesAccumulated;
};
