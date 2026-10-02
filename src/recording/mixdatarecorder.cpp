#include "recording/mixdatarecorder.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>

#include "control/controlproxy.h"
#include "library/coverartcache.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "moc_mixdatarecorder.cpp"
#include "recording/defs_recording.h"
#include "track/track.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveform.h"

namespace {
constexpr int kFormatVersion = 1;
// 125 Hz. Lines are not guaranteed to be evenly spaced (GUI thread timer),
// which is why every line carries its own "t" instead of relying on this.
constexpr int kLevelsIntervalMs = 8;
// Flush the data file roughly once per second, so a crash loses at most
// that much while keeping write syscalls off the per-tick path.
constexpr int kFlushIntervalTicks = 1000 / kLevelsIntervalMs;
// Safety net for the case where a track's waveform arrives via an async
// analysis-DB fetch (e.g. after the Track object got evicted from
// GlobalTrackCache and reloaded) rather than a live analyzer run: the
// waveformUpdated rewrite hookup can race with that fetch, so poll briefly
// as a backstop instead of relying solely on the signal.
constexpr int kWaveformRetryIntervalMs = 250;
constexpr int kWaveformRetryMaxAttempts = 20; // ~5s

QString effectGroupForDeck(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}

QByteArray toLine(const QJsonObject& obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

/// Low/mid/high per waveform frame, L/R averaged: 3 bytes per frame. Cheap
/// enough for the GUI thread; empty if the waveform isn't analyzed yet.
QByteArray lowMidHighOf(const TrackPointer& pTrack) {
    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    if (!pWaveform || pWaveform->getDataSize() == 0) {
        return QByteArray();
    }
    const int frameCount = pWaveform->getDataSize() / 2; // interleaved L/R
    const WaveformData* pData = pWaveform->data();

    QByteArray raw;
    raw.resize(frameCount * 3);
    char* pOut = raw.data();
    for (int i = 0; i < frameCount; ++i) {
        const WaveformData& left = pData[i * 2];
        const WaveformData& right = pData[i * 2 + 1];
        pOut[i * 3 + 0] = static_cast<char>(
                (left.filtered.low + right.filtered.low) / 2);
        pOut[i * 3 + 1] = static_cast<char>(
                (left.filtered.mid + right.filtered.mid) / 2);
        pOut[i * 3 + 2] = static_cast<char>(
                (left.filtered.high + right.filtered.high) / 2);
    }
    return raw;
}

QByteArray trackLoadedLine(const QString& group,
        const QString& artist,
        const QString& title,
        double duration,
        const QByteArray& lowMidHigh,
        double t) {
    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("track_loaded");
    obj[QStringLiteral("deck")] = group;
    obj[QStringLiteral("artist")] = artist;
    obj[QStringLiteral("title")] = title;
    obj[QStringLiteral("t")] = t;

    if (!lowMidHigh.isEmpty()) {
        const int frameCount = static_cast<int>(lowMidHigh.size() / 3);
        obj[QStringLiteral("waveform_frame_count")] = frameCount;
        obj[QStringLiteral("waveform_sample_rate")] =
                duration > 0.0 ? (frameCount / duration) : 0.0;
        obj[QStringLiteral("waveform_low_mid_high_base64")] =
                QString::fromLatin1(lowMidHigh.toBase64());
    } else {
        // Waveform not analyzed yet. Write explicit nulls rather than
        // omitting the keys, so a reader can tell "not ready" apart from
        // "field missing" instead of holding onto a previous track's
        // waveform. A later track_loaded line for the same deck carries the
        // waveform once analysis finishes.
        obj[QStringLiteral("waveform_frame_count")] = 0;
        obj[QStringLiteral("waveform_sample_rate")] = 0.0;
        obj[QStringLiteral("waveform_low_mid_high_base64")] = QJsonValue();
    }
    return toLine(obj);
}

QByteArray coverArtLine(const QString& group, const QImage& cover, double t) {
    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    buffer.open(QIODevice::WriteOnly);
    cover.save(&buffer, "PNG");

    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("cover_art");
    obj[QStringLiteral("deck")] = group;
    obj[QStringLiteral("cover_png_base64")] = QString::fromLatin1(pngBytes.toBase64());
    obj[QStringLiteral("t")] = t;
    return toLine(obj);
}
} // namespace

MixDataRecorder::MixDataRecorder(
        UserSettingsPointer pConfig, PlayerManager* pPlayerManager)
        : m_pConfig(pConfig),
          m_pPlayerManager(pPlayerManager),
          m_pRecordingStatus(std::make_unique<ControlProxy>(
                  QStringLiteral(RECORDING_PREF_KEY), QStringLiteral("status"))),
          m_pSideChainFramesWritten(std::make_unique<ControlProxy>(
                  QStringLiteral(RECORDING_PREF_KEY),
                  QStringLiteral("sidechain_frames_written"))),
          m_pFileStartFrame(std::make_unique<ControlProxy>(
                  QStringLiteral(RECORDING_PREF_KEY), QStringLiteral("file_start_frame"))),
          m_pSampleRate(std::make_unique<ControlProxy>(
                  QStringLiteral("[App]"), QStringLiteral("samplerate"))),
          m_pMainBandLow(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_low"))),
          m_pMainBandMid(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_mid"))),
          m_pMainBandHigh(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_high"))),
          m_pLevelsTimer(std::make_unique<QTimer>()),
          m_pWriterContext(std::make_unique<QObject>()),
          m_fileOpen(false),
          m_ticksSinceFlush(0) {
    m_writerThread.setObjectName(QStringLiteral("MixDataRecorder"));
    m_pWriterContext->moveToThread(&m_writerThread);
    m_writerThread.start(QThread::LowPriority);

    // The default CoarseTimer allows ~5% jitter per tick.
    m_pLevelsTimer->setTimerType(Qt::PreciseTimer);
    m_pRecordingStatus->connectValueChanged(this, &MixDataRecorder::onRecordingStatusChanged);
    connect(CoverArtCache::instance(),
            &CoverArtCache::coverFound,
            this,
            &MixDataRecorder::onCoverFound);
    connect(m_pLevelsTimer.get(),
            &QTimer::timeout,
            this,
            &MixDataRecorder::writeLevels);

    for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
        BaseTrackPlayer* pDeck = m_pPlayerManager->getDeckBase(i);
        if (!pDeck) {
            continue;
        }
        const QString group = pDeck->getGroup();
        const QString effectGroup = effectGroupForDeck(group);

        auto pState = std::make_unique<DeckState>();
        pState->group = group;
        pState->pDeck = pDeck;
        pState->pVolume = std::make_unique<ControlProxy>(group, QStringLiteral("volume"));
        pState->pEqLow = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter1"));
        pState->pEqMid = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter2"));
        pState->pEqHigh = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter3"));
        pState->pKillLow = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter1"));
        pState->pKillMid = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter2"));
        pState->pKillHigh = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter3"));
        // "bpm" reflects the rate-adjusted, actually-playing tempo (as
        // opposed to "file_bpm"/"local_bpm", the track's analyzed tempo).
        pState->pBpm = std::make_unique<ControlProxy>(group, QStringLiteral("bpm"));
        // Fraction (0..1) into the current beat, already accounting for
        // scratching, looping and tempo/rate changes -- use this for
        // beat-phase sync instead of re-deriving it from bpm + playpos.
        pState->pBeatDistance = std::make_unique<ControlProxy>(
                group, QStringLiteral("beat_distance"));
        pState->pVisualPlayPosition = VisualPlayPosition::getVisualPlayPosition(group);

        connect(pDeck,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this, group](TrackPointer pTrack) { handleNewTrackLoaded(group, pTrack); });

        m_deckStates.push_back(std::move(pState));
    }

    onRecordingStatusChanged(m_pRecordingStatus->get());
}

MixDataRecorder::~MixDataRecorder() {
    m_pLevelsTimer->stop();
    closeFile();
    // Quit from within the queue rather than with quit() from here, so every
    // job posted so far (including the close above) still runs.
    postToWriter([this]() { m_writerThread.quit(); });
    m_writerThread.wait();
}

void MixDataRecorder::postToWriter(std::function<void()> job) {
    QMetaObject::invokeMethod(m_pWriterContext.get(), std::move(job), Qt::QueuedConnection);
}

void MixDataRecorder::onRecordingStatusChanged(double status) {
    syncWithRecordingStatus();
    // Keep ticking through READY/SPLIT_CONTINUE: the tick picks up the
    // transition to ON (and the new audio file) even if this slot's
    // notification for it arrives late.
    if (status == RECORD_OFF) {
        m_pLevelsTimer->stop();
    } else if (!m_pLevelsTimer->isActive()) {
        m_pLevelsTimer->start(kLevelsIntervalMs);
    }
}

bool MixDataRecorder::syncWithRecordingStatus() {
    const double status = m_pRecordingStatus->get();
    if (status != RECORD_ON) {
        // READY: audio file not open yet. SPLIT_CONTINUE: the current audio
        // file is being closed, the next one isn't open yet. Either way
        // there's no audio to line data up with.
        closeFile();
        if (status == RECORD_OFF) {
            m_failedAudioPath.clear();
        }
        return false;
    }

    // RecordingManager updates this before it asks EngineRecord to start or
    // split, so while ON it names the audio file currently being written.
    const QString audioPath = m_pConfig->getValueString(
            ConfigKey(QStringLiteral(RECORDING_PREF_KEY), QStringLiteral("Path")));
    if (m_fileOpen && audioPath == m_audioPath) {
        return true;
    }
    closeFile();
    if (audioPath.isEmpty() || audioPath == m_failedAudioPath) {
        return false;
    }
    openFile(audioPath);
    return m_fileOpen;
}

void MixDataRecorder::openFile(const QString& audioPath) {
    const QFileInfo audioInfo(audioPath);
    const QString dataPath = audioInfo.dir().filePath(
            audioInfo.completeBaseName() + QStringLiteral(".jsonl"));
    // Opened here (cheap) so failures can be handled right away, then handed
    // over to the writer thread for all I/O.
    auto pFile = std::make_unique<QFile>(dataPath);
    if (!pFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "MixDataRecorder: failed to open" << dataPath << "-"
                   << pFile->errorString();
        m_failedAudioPath = audioPath;
        return;
    }
    qDebug() << "MixDataRecorder: writing" << dataPath;
    pFile->moveToThread(&m_writerThread);
    postToWriter([this, pFile = pFile.release()]() { m_pWriterFile.reset(pFile); });
    m_fileOpen = true;
    m_audioPath = audioPath;
    m_sampleRate = mixxx::audio::SampleRate::fromDouble(m_pSampleRate->get());
    m_ticksSinceFlush = 0;

    QJsonArray decks;
    for (const auto& pState : m_deckStates) {
        decks.append(pState->group);
    }
    QJsonObject header;
    header[QStringLiteral("type")] = QStringLiteral("header");
    header[QStringLiteral("version")] = kFormatVersion;
    header[QStringLiteral("audio_file")] = audioInfo.fileName();
    header[QStringLiteral("sample_rate")] = static_cast<int>(m_sampleRate.value());
    header[QStringLiteral("levels_rate_hz")] = 1000 / kLevelsIntervalMs;
    header[QStringLiteral("decks")] = decks;
    header[QStringLiteral("t")] = 0.0;
    writeLine(header);

    writeSnapshot();
}

void MixDataRecorder::closeFile() {
    if (!m_fileOpen) {
        return;
    }
    postToWriter([this]() { m_pWriterFile.reset(); });
    m_fileOpen = false;
    m_audioPath.clear();
}

void MixDataRecorder::writeSnapshot() {
    // Decks loaded before this audio file started: their state held from
    // its very beginning, so stamp it at t=0 rather than at the (slightly
    // later) moment we noticed the file.
    for (const auto& pState : m_deckStates) {
        const TrackPointer pTrack = pState->pDeck->getLoadedTrack();
        if (!pTrack) {
            continue;
        }
        writeTrackLoadedAt(pState->group, pTrack, 0.0);
        if (!pState->coverImage.isNull()) {
            writeCoverArt(pState->group, pState->coverImage, 0.0);
        }
    }
}

double MixDataRecorder::recordedSeconds() const {
    if (!m_sampleRate.isValid()) {
        return 0.0;
    }
    // Frames the engine produced since the current audio file's first
    // frame, including those still queued in the sidechain FIFO: they will
    // be written to this file too.
    const double frames = m_pSideChainFramesWritten->get() - m_pFileStartFrame->get();
    return std::max(frames, 0.0) / m_sampleRate.toDouble();
}

void MixDataRecorder::writeLine(QJsonObject obj) {
    if (!m_fileOpen) {
        return;
    }
    if (!obj.contains(QStringLiteral("t"))) {
        obj[QStringLiteral("t")] = recordedSeconds();
    }
    postToWriter([this, line = toLine(obj)]() {
        if (m_pWriterFile) {
            m_pWriterFile->write(line);
        }
    });
}

MixDataRecorder::DeckState* MixDataRecorder::findDeckByGroup(const QString& group) {
    for (auto& pState : m_deckStates) {
        if (pState->group == group) {
            return pState.get();
        }
    }
    return nullptr;
}

MixDataRecorder::DeckState* MixDataRecorder::findDeckByRequester(const QObject* pRequester) {
    for (auto& pState : m_deckStates) {
        if (pState->pDeck == pRequester) {
            return pState.get();
        }
    }
    return nullptr;
}

void MixDataRecorder::handleNewTrackLoaded(const QString& group, TrackPointer pTrack) {
    if (!pTrack) {
        return;
    }
    DeckState* pState = findDeckByGroup(group);
    if (!pState) {
        return;
    }
    pState->coverImage = QImage();

    QObject::disconnect(pState->waveformConnection);
    pState->waveformConnection = connect(pTrack.get(),
            &Track::waveformUpdated,
            this,
            [this, group, pTrack]() {
                // Guard against a rewrite for a track that's already been
                // swapped out on this deck (e.g. a cross-thread signal
                // queued just before the deck loaded a new track).
                DeckState* pState = findDeckByGroup(group);
                if (!pState || pState->pDeck->getLoadedTrack() != pTrack) {
                    return;
                }
                writeTrackLoaded(group, pTrack);
            });

    writeTrackLoaded(group, pTrack);
    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    if (!pWaveform || pWaveform->getDataSize() == 0) {
        scheduleWaveformRetry(group, pTrack, kWaveformRetryMaxAttempts);
    }
    CoverArtCache::requestTrackCover(pState->pDeck, pTrack);
}

void MixDataRecorder::scheduleWaveformRetry(
        const QString& group, TrackPointer pTrack, int attemptsRemaining) {
    if (attemptsRemaining <= 0) {
        return;
    }
    QTimer::singleShot(kWaveformRetryIntervalMs, this, [this, group, pTrack, attemptsRemaining]() {
        DeckState* pState = findDeckByGroup(group);
        if (!pState || pState->pDeck->getLoadedTrack() != pTrack) {
            // Deck moved on to another track; that load's own rewrite path
            // covers it now.
            return;
        }
        const ConstWaveformPointer pWaveform = pTrack->getWaveform();
        if (pWaveform && pWaveform->getDataSize() > 0) {
            writeTrackLoaded(group, pTrack);
            return;
        }
        scheduleWaveformRetry(group, pTrack, attemptsRemaining - 1);
    });
}

void MixDataRecorder::writeTrackLoaded(const QString& group, TrackPointer pTrack) {
    if (!pTrack || !syncWithRecordingStatus()) {
        return;
    }
    writeTrackLoadedAt(group, pTrack, recordedSeconds());
}

void MixDataRecorder::writeTrackLoadedAt(
        const QString& group, const TrackPointer& pTrack, double t) {
    if (!m_fileOpen) {
        return;
    }
    // Gather the data here, while it's consistent with t; encode it on the
    // writer thread.
    postToWriter([this,
                         group,
                         artist = pTrack->getArtist(),
                         title = pTrack->getTitle(),
                         duration = pTrack->getDuration(),
                         lowMidHigh = lowMidHighOf(pTrack),
                         t]() {
        if (m_pWriterFile) {
            m_pWriterFile->write(trackLoadedLine(group, artist, title, duration, lowMidHigh, t));
        }
    });
}

void MixDataRecorder::writeCoverArt(const QString& group, const QImage& cover, double t) {
    if (!m_fileOpen) {
        return;
    }
    postToWriter([this, group, cover, t]() {
        if (m_pWriterFile) {
            m_pWriterFile->write(coverArtLine(group, cover, t));
        }
    });
}

void MixDataRecorder::onCoverFound(
        const QObject* pRequester, const CoverInfo& coverInfo, const QPixmap& pixmap) {
    Q_UNUSED(coverInfo);
    if (pixmap.isNull()) {
        return;
    }
    DeckState* pState = findDeckByRequester(pRequester);
    if (!pState) {
        return;
    }

    // QPixmap is GUI-thread only; QImage can be encoded on the writer thread.
    pState->coverImage = pixmap.toImage();

    if (syncWithRecordingStatus()) {
        writeCoverArt(pState->group, pState->coverImage, recordedSeconds());
    }
}

void MixDataRecorder::writeLevels() {
    if (!syncWithRecordingStatus()) {
        return;
    }

    QJsonObject decksObj;
    for (auto& pState : m_deckStates) {
        QJsonObject deckObj;
        deckObj[QStringLiteral("pos")] = pState->pVisualPlayPosition
                ? pState->pVisualPlayPosition->getEnginePlayPos()
                : -1.0;
        deckObj[QStringLiteral("volume")] = pState->pVolume->get();
        deckObj[QStringLiteral("eq_low")] = pState->pEqLow->get();
        deckObj[QStringLiteral("eq_mid")] = pState->pEqMid->get();
        deckObj[QStringLiteral("eq_high")] = pState->pEqHigh->get();
        deckObj[QStringLiteral("kill_low")] = pState->pKillLow->toBool();
        deckObj[QStringLiteral("kill_mid")] = pState->pKillMid->toBool();
        deckObj[QStringLiteral("kill_high")] = pState->pKillHigh->toBool();
        deckObj[QStringLiteral("bpm")] = pState->pBpm->get();
        deckObj[QStringLiteral("beat_distance")] = pState->pBeatDistance->get();
        decksObj[pState->group] = deckObj;
    }

    QJsonObject mainObj;
    mainObj[QStringLiteral("low")] = m_pMainBandLow->get();
    mainObj[QStringLiteral("mid")] = m_pMainBandMid->get();
    mainObj[QStringLiteral("high")] = m_pMainBandHigh->get();

    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("levels");
    obj[QStringLiteral("decks")] = decksObj;
    obj[QStringLiteral("master")] = mainObj;
    writeLine(obj);

    if (++m_ticksSinceFlush >= kFlushIntervalTicks) {
        postToWriter([this]() {
            if (m_pWriterFile) {
                m_pWriterFile->flush();
            }
        });
        m_ticksSinceFlush = 0;
    }
}
