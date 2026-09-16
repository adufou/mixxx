#include "broadcast/livedataexportmanager.h"

#include <QBuffer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "control/controlproxy.h"
#include "library/coverartcache.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "moc_livedataexportmanager.cpp"
#include "track/track.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveform.h"

namespace {
const QString kPrefGroup = QStringLiteral("[LiveDataExport]");
constexpr bool kDefaultEnabled = false;
constexpr int kDefaultPort = 9090;
constexpr int kLevelsIntervalMs = 8; // ~120 Hz
// Don't let a stalled client's write buffer grow without bound.
constexpr qint64 kMaxSocketBacklogBytes = 1 << 20; // 1 MiB

QString effectGroupForDeck(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}
} // namespace

LiveDataExportManager::LiveDataExportManager(
        UserSettingsPointer pConfig, PlayerManager* pPlayerManager)
        : m_pConfig(pConfig),
          m_pPlayerManager(pPlayerManager),
          m_pMasterBandLow(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_low"))),
          m_pMasterBandMid(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_mid"))),
          m_pMasterBandHigh(std::make_unique<ControlProxy>(
                  QStringLiteral("[Main]"), QStringLiteral("live_band_high"))),
          m_pLevelsTimer(std::make_unique<QTimer>()),
          m_enabled(pConfig->getValue<bool>(ConfigKey(kPrefGroup, "Enabled"), kDefaultEnabled)),
          m_port(pConfig->getValue<int>(ConfigKey(kPrefGroup, "Port"), kDefaultPort)) {
    connect(CoverArtCache::instance(),
            &CoverArtCache::coverFound,
            this,
            &LiveDataExportManager::onCoverFound);
    connect(m_pLevelsTimer.get(),
            &QTimer::timeout,
            this,
            &LiveDataExportManager::sendLiveLevels);

    for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
        BaseTrackPlayer* pDeck = m_pPlayerManager->getDeckBase(i);
        if (!pDeck) {
            continue;
        }
        const QString group = pDeck->getGroup();
        const QString effectGroup = effectGroupForDeck(group);

        auto pState = std::make_unique<DeckExportState>();
        pState->group = group;
        pState->pDeck = pDeck;
        pState->pEqLow = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter1"));
        pState->pEqMid = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter2"));
        pState->pEqHigh = std::make_unique<ControlProxy>(effectGroup, QStringLiteral("parameter3"));
        pState->pKillLow = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter1"));
        pState->pKillMid = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter2"));
        pState->pKillHigh = std::make_unique<ControlProxy>(
                effectGroup, QStringLiteral("button_parameter3"));
        pState->pVisualPlayPosition = VisualPlayPosition::getVisualPlayPosition(group);

        connect(pDeck,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this, group](TrackPointer pTrack) { handleNewTrackLoaded(group, pTrack); });

        m_deckStates.push_back(std::move(pState));
    }

    if (m_enabled) {
        start();
    }
}

LiveDataExportManager::~LiveDataExportManager() {
    stop();
}

void LiveDataExportManager::setEnabled(bool enabled) {
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    m_pConfig->setValue(ConfigKey(kPrefGroup, "Enabled"), enabled);
    if (enabled) {
        start();
    } else {
        stop();
    }
}

void LiveDataExportManager::setPort(int port) {
    if (port == m_port) {
        return;
    }
    m_port = port;
    m_pConfig->setValue(ConfigKey(kPrefGroup, "Port"), port);
    if (m_pServer) {
        stop();
        start();
    }
}

void LiveDataExportManager::start() {
    if (m_pServer) {
        return;
    }
    auto pServer = std::make_unique<QTcpServer>();
    connect(pServer.get(),
            &QTcpServer::newConnection,
            this,
            &LiveDataExportManager::onNewConnection);
    if (!pServer->listen(QHostAddress::LocalHost, static_cast<quint16>(m_port))) {
        qWarning() << "LiveDataExportManager: failed to listen on port" << m_port << "-"
                   << pServer->errorString();
        return;
    }
    m_pServer = std::move(pServer);
    m_pLevelsTimer->start(kLevelsIntervalMs);
}

void LiveDataExportManager::stop() {
    m_pLevelsTimer->stop();
    for (QTcpSocket* pSocket : std::as_const(m_sockets)) {
        pSocket->disconnectFromHost();
    }
    m_sockets.clear();
    m_pServer.reset();
}

void LiveDataExportManager::onNewConnection() {
    if (!m_pServer) {
        return;
    }
    while (m_pServer->hasPendingConnections()) {
        QTcpSocket* pSocket = m_pServer->nextPendingConnection();
        m_sockets.append(pSocket);
        connect(pSocket,
                &QTcpSocket::disconnected,
                this,
                &LiveDataExportManager::onSocketDisconnected);
        connect(pSocket, &QTcpSocket::disconnected, pSocket, &QObject::deleteLater);
    }
}

void LiveDataExportManager::onSocketDisconnected() {
    QTcpSocket* pSocket = qobject_cast<QTcpSocket*>(sender());
    if (pSocket) {
        m_sockets.removeAll(pSocket);
    }
}

void LiveDataExportManager::writeLine(const QByteArray& line) {
    for (QTcpSocket* pSocket : std::as_const(m_sockets)) {
        if (pSocket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }
        if (pSocket->bytesToWrite() > kMaxSocketBacklogBytes) {
            continue;
        }
        pSocket->write(line);
    }
}

LiveDataExportManager::DeckExportState* LiveDataExportManager::findDeckByGroup(
        const QString& group) {
    for (auto& pState : m_deckStates) {
        if (pState->group == group) {
            return pState.get();
        }
    }
    return nullptr;
}

LiveDataExportManager::DeckExportState* LiveDataExportManager::findDeckByRequester(
        const QObject* pRequester) {
    for (auto& pState : m_deckStates) {
        if (pState->pDeck == pRequester) {
            return pState.get();
        }
    }
    return nullptr;
}

void LiveDataExportManager::handleNewTrackLoaded(const QString& group, TrackPointer pTrack) {
    if (!m_enabled || !pTrack) {
        return;
    }
    DeckExportState* pState = findDeckByGroup(group);
    if (!pState) {
        return;
    }

    QObject::disconnect(pState->waveformConnection);
    pState->waveformConnection = connect(pTrack.get(),
            &Track::waveformUpdated,
            this,
            [this, group, pTrack]() { sendTrackLoaded(group, pTrack); });

    sendTrackLoaded(group, pTrack);
    CoverArtCache::requestTrackCover(pState->pDeck, pTrack);
}

void LiveDataExportManager::sendTrackLoaded(const QString& group, TrackPointer pTrack) {
    if (!m_enabled || !pTrack) {
        return;
    }

    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("track_loaded");
    obj[QStringLiteral("deck")] = group;
    obj[QStringLiteral("artist")] = pTrack->getArtist();
    obj[QStringLiteral("title")] = pTrack->getTitle();
    obj[QStringLiteral("album")] = pTrack->getAlbum();

    const ConstWaveformPointer pWaveform = pTrack->getWaveform();
    if (pWaveform && pWaveform->getDataSize() > 0) {
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

        const double duration = pTrack->getDuration();
        obj[QStringLiteral("waveform_frame_count")] = frameCount;
        obj[QStringLiteral("waveform_sample_rate")] =
                duration > 0.0 ? (frameCount / duration) : 0.0;
        obj[QStringLiteral("waveform_low_mid_high_base64")] =
                QString::fromLatin1(raw.toBase64());
    }

    writeLine(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void LiveDataExportManager::onCoverFound(
        const QObject* pRequester, const CoverInfo& coverInfo, const QPixmap& pixmap) {
    Q_UNUSED(coverInfo);
    if (!m_enabled || pixmap.isNull()) {
        return;
    }
    DeckExportState* pState = findDeckByRequester(pRequester);
    if (!pState) {
        return;
    }

    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");

    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("cover_art");
    obj[QStringLiteral("deck")] = pState->group;
    obj[QStringLiteral("cover_png_base64")] = QString::fromLatin1(pngBytes.toBase64());

    writeLine(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void LiveDataExportManager::sendLiveLevels() {
    if (!m_enabled || m_sockets.isEmpty()) {
        return;
    }

    QJsonObject decksObj;
    for (auto& pState : m_deckStates) {
        QJsonObject deckObj;
        deckObj[QStringLiteral("pos")] = pState->pVisualPlayPosition
                ? pState->pVisualPlayPosition->getEnginePlayPos()
                : -1.0;
        deckObj[QStringLiteral("eq_low")] = pState->pEqLow->get();
        deckObj[QStringLiteral("eq_mid")] = pState->pEqMid->get();
        deckObj[QStringLiteral("eq_high")] = pState->pEqHigh->get();
        deckObj[QStringLiteral("kill_low")] = pState->pKillLow->toBool();
        deckObj[QStringLiteral("kill_mid")] = pState->pKillMid->toBool();
        deckObj[QStringLiteral("kill_high")] = pState->pKillHigh->toBool();
        decksObj[pState->group] = deckObj;
    }

    QJsonObject masterObj;
    masterObj[QStringLiteral("low")] = m_pMasterBandLow->get();
    masterObj[QStringLiteral("mid")] = m_pMasterBandMid->get();
    masterObj[QStringLiteral("high")] = m_pMasterBandHigh->get();

    QJsonObject obj;
    obj[QStringLiteral("type")] = QStringLiteral("levels");
    obj[QStringLiteral("decks")] = decksObj;
    obj[QStringLiteral("master")] = masterObj;

    writeLine(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}
