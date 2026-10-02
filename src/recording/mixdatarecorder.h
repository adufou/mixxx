#pragma once

#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QPixmap>
#include <QThread>
#include <functional>
#include <memory>
#include <vector>

#include "audio/types.h"
#include "library/coverart.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class PlayerManager;
class BaseTrackPlayer;
class ControlProxy;
class VisualPlayPosition;
class QFile;
class QJsonObject;
class QTimer;

/// Writes performance data (loaded track metadata, per-track waveform data
/// and cover art, plus 125 Hz playhead/EQ/fader/tempo/master-level samples)
/// to a newline-delimited JSON file next to each audio file produced by
/// Mixxx's recorder, so an external app (e.g. a Godot overlay rendered
/// offline) can replay the mix frame-accurately against the recorded audio.
///
/// Every line carries "t": seconds since the start of the matching audio
/// file, derived from how many frames the engine has produced since the
/// first frame of that file (see EngineSideChain/EngineRecord) rather than
/// from a wall clock, so it can't drift from the audio. It advances once per
/// engine buffer, the same rate the sampled values themselves change at. When the
/// recorder splits into a new audio file, a new data file is started too,
/// with "t" restarting at 0 and a fresh snapshot of every loaded deck.
///
/// The master mix's live low/mid/high levels are read from the
/// EngineLiveBandLevels ControlObjects on the "[Main]" group.
///
/// Values are sampled and stamped on the GUI thread, but everything
/// expensive (PNG/base64/JSON encoding of covers and waveforms, file I/O)
/// runs on a dedicated writer thread, so a track load doesn't stall the
/// 125 Hz sampling. Jobs run in the order they were posted, so lines keep
/// their order.
class MixDataRecorder : public QObject {
    Q_OBJECT
  public:
    MixDataRecorder(UserSettingsPointer pConfig, PlayerManager* pPlayerManager);
    ~MixDataRecorder() override;

  private slots:
    void onRecordingStatusChanged(double status);
    void onCoverFound(const QObject* pRequester,
            const CoverInfo& coverInfo,
            const QPixmap& pixmap);
    void writeLevels();

  private:
    struct DeckState {
        QString group;
        BaseTrackPlayer* pDeck = nullptr;
        std::unique_ptr<ControlProxy> pVolume;
        std::unique_ptr<ControlProxy> pEqLow;
        std::unique_ptr<ControlProxy> pEqMid;
        std::unique_ptr<ControlProxy> pEqHigh;
        std::unique_ptr<ControlProxy> pKillLow;
        std::unique_ptr<ControlProxy> pKillMid;
        std::unique_ptr<ControlProxy> pKillHigh;
        std::unique_ptr<ControlProxy> pBpm;
        std::unique_ptr<ControlProxy> pBeatDistance;
        QSharedPointer<VisualPlayPosition> pVisualPlayPosition;
        QMetaObject::Connection waveformConnection;
        // Cover of the currently loaded track, kept so it can be written
        // into the snapshot at the start of every data file without waiting
        // for another async CoverArtCache round trip.
        QImage coverImage;
    };

    /// Opens/closes the data file to match the recorder's current state.
    /// Returns true if a data file is open afterwards.
    bool syncWithRecordingStatus();
    void openFile(const QString& audioPath);
    void closeFile();
    void writeSnapshot();
    double recordedSeconds() const;
    /// Runs job on the writer thread, after all previously posted jobs.
    void postToWriter(std::function<void()> job);
    /// Writes one line, stamped with the current recorded time unless obj
    /// already carries a "t".
    void writeLine(QJsonObject obj);
    void handleNewTrackLoaded(const QString& group, TrackPointer pTrack);
    void writeTrackLoaded(const QString& group, TrackPointer pTrack);
    void writeTrackLoadedAt(const QString& group, const TrackPointer& pTrack, double t);
    void writeCoverArt(const QString& group, const QImage& cover, double t);
    void scheduleWaveformRetry(const QString& group, TrackPointer pTrack, int attemptsRemaining);
    DeckState* findDeckByGroup(const QString& group);
    DeckState* findDeckByRequester(const QObject* pRequester);

    UserSettingsPointer m_pConfig;
    PlayerManager* m_pPlayerManager;

    std::unique_ptr<ControlProxy> m_pRecordingStatus;
    std::unique_ptr<ControlProxy> m_pSideChainFramesWritten;
    std::unique_ptr<ControlProxy> m_pFileStartFrame;
    std::unique_ptr<ControlProxy> m_pSampleRate;
    std::unique_ptr<ControlProxy> m_pMainBandLow;
    std::unique_ptr<ControlProxy> m_pMainBandMid;
    std::unique_ptr<ControlProxy> m_pMainBandHigh;

    std::vector<std::unique_ptr<DeckState>> m_deckStates;

    std::unique_ptr<QTimer> m_pLevelsTimer;

    QThread m_writerThread;
    // Lives on m_writerThread; jobs are queued to it.
    std::unique_ptr<QObject> m_pWriterContext;
    // Only touched by jobs on m_writerThread.
    std::unique_ptr<QFile> m_pWriterFile;

    // GUI-side view of whether a data file is open (jobs writing to it may
    // still be queued).
    bool m_fileOpen;
    // Audio file the open data file belongs to; a change while recording
    // means the recorder split to a new audio file.
    QString m_audioPath;
    // Audio file whose data file couldn't be opened, so we don't retry (and
    // warn) on every tick.
    QString m_failedAudioPath;
    mixxx::audio::SampleRate m_sampleRate;
    int m_ticksSinceFlush;
};
