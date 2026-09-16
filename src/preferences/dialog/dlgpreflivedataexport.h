#pragma once

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgpreflivedataexportdlg.h"
#include "preferences/usersettings.h"

class LiveDataExportManager;

class DlgPrefLiveDataExport : public DlgPreferencePage, public Ui::DlgPrefLiveDataExportDlg {
    Q_OBJECT
  public:
    DlgPrefLiveDataExport(QWidget* parent,
            UserSettingsPointer pConfig,
            LiveDataExportManager* pLiveDataExportManager);
    ~DlgPrefLiveDataExport() override = default;

  public slots:
    void slotApply() override;
    void slotUpdate() override;
    void slotResetToDefaults() override;

  private:
    UserSettingsPointer m_pConfig;
    LiveDataExportManager* m_pLiveDataExportManager;
};
