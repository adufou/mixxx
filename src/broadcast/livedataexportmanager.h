#pragma once

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPixmap>
#include <memory>
#include <vector>

#include "library/coverart.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class PlayerManager;
class BaseTrackPlayer;
class ControlProxy;
class VisualPlayPosition;
class QTcpServer;
class QTcpSocket;
class QTimer;

/// Streams live performance data (currently loaded track metadata + cover
/// art, and 120 Hz playhead/EQ/master-level updates) over a local plain-TCP,
/// newline-delimited-JSON connection, so an external app (e.g. a Godot
/// overlay) can recreate Mixxx's own per-deck waveform view and a master
/// level meter without Mixxx needing to know anything about the consumer.
///
/// This intentionally does not compute or send the actual 3-band waveform
/// data for the master mix as new DSP: the per-deck waveform is exported
/// as-is from AnalyzerWaveform's precomputed per-track data, and the master
/// mix's live low/mid/high levels are read from the EngineLiveBandLevels
/// ControlObjects on the "[Main]" group.
class LiveDataExportManager : public QObject {
    Q_OBJECT
  public:
    LiveDataExportManager(UserSettingsPointer pConfig, PlayerManager* pPlayerManager);
    ~LiveDataExportManager() override;

    bool isEnabled() const {
        return m_enabled;
    }
    int port() const {
        return m_port;
    }

  public slots:
    void setEnabled(bool enabled);
    void setPort(int port);

  private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onCoverFound(const QObject* pRequester,
            const CoverInfo& coverInfo,
            const QPixmap& pixmap);
    void sendLiveLevels();

  private:
    struct DeckExportState {
        QString group;
        BaseTrackPlayer* pDeck = nullptr;
        std::unique_ptr<ControlProxy> pEqLow;
        std::unique_ptr<ControlProxy> pEqMid;
        std::unique_ptr<ControlProxy> pEqHigh;
        std::unique_ptr<ControlProxy> pKillLow;
        std::unique_ptr<ControlProxy> pKillMid;
        std::unique_ptr<ControlProxy> pKillHigh;
        QSharedPointer<VisualPlayPosition> pVisualPlayPosition;
        QMetaObject::Connection waveformConnection;
    };

    void start();
    void stop();
    void writeLine(const QByteArray& line);
    void handleNewTrackLoaded(const QString& group, TrackPointer pTrack);
    void sendTrackLoaded(const QString& group, TrackPointer pTrack);
    DeckExportState* findDeckByGroup(const QString& group);
    DeckExportState* findDeckByRequester(const QObject* pRequester);

    UserSettingsPointer m_pConfig;
    PlayerManager* m_pPlayerManager;

    std::unique_ptr<ControlProxy> m_pMasterBandLow;
    std::unique_ptr<ControlProxy> m_pMasterBandMid;
    std::unique_ptr<ControlProxy> m_pMasterBandHigh;

    std::vector<std::unique_ptr<DeckExportState>> m_deckStates;

    std::unique_ptr<QTcpServer> m_pServer;
    QList<QTcpSocket*> m_sockets;
    std::unique_ptr<QTimer> m_pLevelsTimer;

    bool m_enabled;
    int m_port;
};
