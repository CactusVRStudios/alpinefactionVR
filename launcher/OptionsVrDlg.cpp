#include "OptionsVrDlg.h"

#include "OptionsDisplayDlg.h"
#include "resource.h"

OptionsVrDlg::OptionsVrDlg(GameConfig& conf, OptionsDisplayDlg& display_dlg) :
    CDialog(IDD_OPTIONS_VR),
    m_conf(conf),
    m_display_dlg(display_dlg)
{
}

BOOL OptionsVrDlg::OnInitDialog()
{
    AttachItem(IDC_VR_TURN_MODE_COMBO, m_turn_mode_combo);
    m_turn_mode_combo.AddString("Snap turn");
    m_turn_mode_combo.AddString("Smooth turn");

    CheckDlgButton(IDC_ENABLE_VR_CHECK, m_conf.vr_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_VR_FAST_WEAPON_SWITCH_CHECK,
        m_conf.vr_fast_weapon_switch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_VR_SHAKE_RELOAD_CHECK,
        m_conf.vr_shake_reload ? BST_CHECKED : BST_UNCHECKED);
    m_turn_mode_combo.SetCurSel(static_cast<int>(m_conf.vr_turn_mode.value()));
    SetDlgItemInt(IDC_VR_SNAP_ANGLE_EDIT, m_conf.vr_snap_turn_degrees, false);
    SetDlgItemInt(IDC_VR_SMOOTH_SPEED_EDIT,
        m_conf.vr_smooth_turn_degrees_per_second, false);
    SetDlgItemInt(IDC_VR_SHAKE_RELOAD_THRESHOLD_EDIT,
        m_conf.vr_shake_reload_threshold_cm_s, false);
    m_display_dlg.SetVrEnabled(m_conf.vr_enabled);
    UpdateControlState();

    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_ENABLE_VR_CHECK),
        "Launch Red Faction through OpenXR. Direct3D 11 is required; multiplayer is experimental.");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_TURN_MODE_COMBO),
        "Choose discrete snap turning or continuous smooth turning for the right thumbstick.");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_SNAP_ANGLE_EDIT),
        "Snap-turn angle in degrees (15 to 180).");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_SMOOTH_SPEED_EDIT),
        "Maximum smooth-turn speed in degrees per second (30 to 360).");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_FAST_WEAPON_SWITCH_CHECK),
        "Equip the next or previous weapon immediately instead of opening the confirmation HUD.");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_SHAKE_RELOAD_CHECK),
        "Reload by making one forceful downward shake with the right controller.");
    m_tool_tip.AddTool(GetDlgItem(IDC_VR_SHAKE_RELOAD_THRESHOLD_EDIT),
        "Downward controller speed required to trigger shake reload (cm/s; lower values are more sensitive).");
    return TRUE;
}

BOOL OptionsVrDlg::OnCommand(WPARAM wparam, [[maybe_unused]] LPARAM lparam)
{
    if (LOWORD(wparam) == IDC_ENABLE_VR_CHECK && HIWORD(wparam) == BN_CLICKED) {
        const bool enabled = IsDlgButtonChecked(IDC_ENABLE_VR_CHECK) == BST_CHECKED;
        m_display_dlg.SetVrEnabled(enabled);
        UpdateControlState();
        return TRUE;
    }
    if (LOWORD(wparam) == IDC_VR_TURN_MODE_COMBO && HIWORD(wparam) == CBN_SELCHANGE) {
        UpdateControlState();
        return TRUE;
    }
    if (LOWORD(wparam) == IDC_VR_SHAKE_RELOAD_CHECK && HIWORD(wparam) == BN_CLICKED) {
        UpdateControlState();
        return TRUE;
    }
    return FALSE;
}

void OptionsVrDlg::OnSave()
{
    m_conf.vr_enabled = IsDlgButtonChecked(IDC_ENABLE_VR_CHECK) == BST_CHECKED;
    m_conf.vr_fast_weapon_switch =
        IsDlgButtonChecked(IDC_VR_FAST_WEAPON_SWITCH_CHECK) == BST_CHECKED;
    m_conf.vr_shake_reload =
        IsDlgButtonChecked(IDC_VR_SHAKE_RELOAD_CHECK) == BST_CHECKED;
    m_conf.vr_turn_mode = static_cast<GameConfig::VrTurnMode>(
        std::max(m_turn_mode_combo.GetCurSel(), 0));
    m_conf.vr_snap_turn_degrees = GetDlgItemInt(IDC_VR_SNAP_ANGLE_EDIT, false);
    m_conf.vr_smooth_turn_degrees_per_second =
        GetDlgItemInt(IDC_VR_SMOOTH_SPEED_EDIT, false);
    m_conf.vr_shake_reload_threshold_cm_s =
        GetDlgItemInt(IDC_VR_SHAKE_RELOAD_THRESHOLD_EDIT, false);
    if (m_conf.vr_enabled) {
        m_conf.renderer = GameConfig::Renderer::d3d11;
    }
}

void OptionsVrDlg::UpdateControlState()
{
    const bool vr_enabled = IsDlgButtonChecked(IDC_ENABLE_VR_CHECK) == BST_CHECKED;
    const bool smooth = m_turn_mode_combo.GetCurSel() ==
        static_cast<int>(GameConfig::VrTurnMode::smooth);
    m_turn_mode_combo.EnableWindow(vr_enabled);
    GetDlgItem(IDC_VR_FAST_WEAPON_SWITCH_CHECK).EnableWindow(vr_enabled);
    GetDlgItem(IDC_VR_SHAKE_RELOAD_CHECK).EnableWindow(vr_enabled);
    GetDlgItem(IDC_VR_SHAKE_RELOAD_THRESHOLD_EDIT).EnableWindow(
        vr_enabled && IsDlgButtonChecked(IDC_VR_SHAKE_RELOAD_CHECK) == BST_CHECKED);
    GetDlgItem(IDC_VR_SNAP_ANGLE_EDIT).EnableWindow(vr_enabled && !smooth);
    GetDlgItem(IDC_VR_SMOOTH_SPEED_EDIT).EnableWindow(vr_enabled && smooth);
}
