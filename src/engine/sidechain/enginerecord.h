#pragma once

#include <QDataStream>
#include <QFile>
#include <memory>

#include "audio/types.h"
#include "control/pollingcontrolproxy.h"
#include "encoder/encoder.h"
#include "encoder/encodercallback.h"
#include "engine/sidechain/sidechainworker.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlObject;
class ControlProxy;

class EngineRecord : public QObject, public EncoderCallback, public SideChainWorker {
    Q_OBJECT
  public:
    EngineRecord(UserSettingsPointer pConfig);
    ~EngineRecord() override;

    void process(const CSAMPLE* pBuffer, const int iBufferSize) override;
    void shutdown() override {}

    // writes compressed audio to file
    void write(const unsigned char *header, const unsigned char *body, int headerLen, int bodyLen) override;
    // gets stream position
    int tell() override;
    // sets stream position
    void seek(int pos) override;
    // gets stream length
    int filelen()  override;

    // creates or opens an audio file
    bool openFile();
    // closes the audio file
    void closeFile();
    int updateFromPreferences();
    bool fileOpen();
    bool openCueFile();
    void closeCueFile();

  signals:
    // emitted to notify RecordingManager
    void bytesRecorded(int bytes);

    // Emitted when recording state changes. 'recording' represents whether
    // recording is active and 'error' is true if an error occurred. Currently
    // only one error can occur: the specified file was unable to be opened for
    // writing.
    void isRecording(bool recording, bool error);
    void durationRecorded(quint64 durationInt);

  private:
    int getActiveTracks();
    // Check if the metadata has changed since the previous check. We also check
    // when was the last check performed to avoid using too much CPU and as well
    // to avoid changing the metadata during scratches.
    bool metaDataHasChanged();

    void writeCueLine();
    // Resets the frame count for a newly opened audio file and publishes
    // where in the sidechain stream it starts. Must run before the status is
    // set to RECORD_ON, so that anyone seeing ON also sees the start frame
    // of the new file.
    void resetFrames();

    UserSettingsPointer m_pConfig;
    EncoderPointer m_pEncoder;
    QString m_encoding;
    QString m_fileName;
    QString m_baTitle;
    QString m_baAuthor;
    QString m_baAlbum;

    QFile m_file;
    QFile m_cueFile;
    QDataStream m_dataStream;

    PollingControlProxy m_sampleRateControl;
    ControlProxy* m_pRecReady;
    quint64 m_frames;
    // Sidechain stream position (see EngineSideChain) of the first frame of
    // the chunk being processed.
    PollingControlProxy m_sideChainFramesRead;
    // Sidechain stream position of the first frame of the current audio
    // file. Together with EngineSideChain's frames_written count this gives
    // the audio file position of what the engine is producing right now,
    // which MixDataRecorder stamps its data with. (Our own m_frames lags
    // that by up to the sidechain FIFO's size, ~0.6 s, and moves in
    // FIFO-sized jumps.)
    std::unique_ptr<ControlObject> m_pFileStartFrame;
    mixxx::audio::SampleRate m_sampleRate;
    quint64 m_recordedDuration;
    QString getRecordedDurationStr();

    int m_iMetaDataLife;
    TrackPointer m_pCurrentTrack;

    QString m_cueFileName;
    quint64 m_cueTrack;
    bool m_bCueIsEnabled;
};
