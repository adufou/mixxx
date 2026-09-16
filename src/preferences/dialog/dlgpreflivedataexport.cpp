#include "preferences/dialog/dlgpreflivedataexport.h"

#include "broadcast/livedataexportmanager.h"
#include "moc_dlgpreflivedataexport.cpp"

namespace {
constexpr bool kDefaultEnabled = false;
constexpr int kDefaultPort = 9090;
} // namespace

DlgPrefLiveDataExport::DlgPrefLiveDataExport(QWidget* parent,
        UserSettingsPointer pConfig,
        LiveDataExportManager* pLiveDataExportManager)
        : DlgPreferencePage(parent),
          m_pConfig(pConfig),
          m_pLiveDataExportManager(pLiveDataExportManager) {
    setupUi(this);
    slotUpdate();
}

void DlgPrefLiveDataExport::slotApply() {
    const bool enabled = checkBoxEnabled->isChecked();
    const int port = spinBoxPort->value();
    m_pLiveDataExportManager->setPort(port);
    m_pLiveDataExportManager->setEnabled(enabled);
}

void DlgPrefLiveDataExport::slotUpdate() {
    checkBoxEnabled->setChecked(m_pLiveDataExportManager->isEnabled());
    spinBoxPort->setValue(m_pLiveDataExportManager->port());
}

void DlgPrefLiveDataExport::slotResetToDefaults() {
    checkBoxEnabled->setChecked(kDefaultEnabled);
    spinBoxPort->setValue(kDefaultPort);
}
