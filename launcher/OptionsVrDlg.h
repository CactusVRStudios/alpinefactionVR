#pragma once

#include <common/config/GameConfig.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>

class OptionsDisplayDlg;

class OptionsVrDlg : public CDialog
{
public:
    OptionsVrDlg(GameConfig& conf, OptionsDisplayDlg& display_dlg);
    void OnSave();

protected:
    BOOL OnInitDialog() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;

private:
    void UpdateControlState();

    GameConfig& m_conf;
    OptionsDisplayDlg& m_display_dlg;
    CToolTip m_tool_tip;
    CComboBox m_turn_mode_combo;
};
