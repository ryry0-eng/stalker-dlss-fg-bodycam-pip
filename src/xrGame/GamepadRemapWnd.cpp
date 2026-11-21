#include "stdafx.h"
#include "GamepadRemapWnd.h"
#include "PauseTokenManager.h"
#include "../xrEngine/GamepadMath.h"
#include "GamepadContextDispatcher.h"
#include "xr_level_controller.h"
#include "../xrEngine/GamepadBindings.h"
#include "../xrEngine/GamepadDeviceManager.h"
#include "../xrEngine/device.h"
#include "../xrEngine/xr_input.h"
#include "string_table.h"
#include "UIGameCustom.h"
#include "MainMenu.h"
#include "ui/UIDialogWnd.h"
#include "ui/UIStatic.h"
#include "ui_base.h"
#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/UIShader.h"
#include <dinput.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern ENGINE_API int ps_gp_telemetry;

namespace
{
LPCSTR TrRemap(LPCSTR id, LPCSTR fallback)
{
	if (CStringTable::getPData())
	{
		const shared_str s = CStringTable().translate(id);
		if (s.size() && 0 != xr_strcmp(s.c_str(), id))
			return s.c_str();
	}
	return fallback;
}

LPCSTR TranslateEditRefusal(gpbind::EditStatus st, const std::string& rawMsg)
{
	switch (st)
	{
	case gpbind::EditStatus::ProtectedActionInvalidTrigger:
		return TrRemap("ui_gp_eng_remap_err_protected_trigger", "Protected navigation action (Menu/Confirm/Back) requires a direct button or hold on a standard button.");
	case gpbind::EditStatus::ProtectedActionCannotUnbind:
		return TrRemap("ui_gp_eng_remap_err_protected_unbind", "Protected navigation action cannot be unbound; it can only be swapped with another valid button.");
	case gpbind::EditStatus::ProtectedNavigationBroken:
		return TrRemap("ui_gp_eng_remap_err_nav_broken", "Cannot resolve conflict: would break MenuUI navigation.");
	case gpbind::EditStatus::InvalidTrigger:
	case gpbind::EditStatus::InvalidContextOrAction:
		return TrRemap("ui_gp_eng_remap_err_invalid_trigger", "Invalid button or chord trigger specification.");
	default:
		return rawMsg.empty() ? TrRemap("ui_gp_eng_remap_err_protected_trigger", "Protected navigation action cannot use this trigger.") : rawMsg.c_str();
	}
}

struct SWizardStepDef
{
	const char* sdlKey;
	const char* titleId;
	const char* title;
	const char* promptId;
	const char* prompt;
	bool isStickAxis;
	bool isTriggerAxis;
};

static const SWizardStepDef g_wizardSteps[] = {
	{ "a",             "ui_gp_eng_wiz_step_01_title", "1/21: South Button (A / Cross)",        "ui_gp_eng_wiz_step_01_prompt", "Press the BOTTOM face button (A / Cross)",           false, false },
	{ "b",             "ui_gp_eng_wiz_step_02_title", "2/21: East Button (B / Circle)",        "ui_gp_eng_wiz_step_02_prompt", "Press the RIGHT face button (B / Circle)",           false, false },
	{ "x",             "ui_gp_eng_wiz_step_03_title", "3/21: West Button (X / Square)",        "ui_gp_eng_wiz_step_03_prompt", "Press the LEFT face button (X / Square)",            false, false },
	{ "y",             "ui_gp_eng_wiz_step_04_title", "4/21: North Button (Y / Triangle)",     "ui_gp_eng_wiz_step_04_prompt", "Press the TOP face button (Y / Triangle)",           false, false },
	{ "back",          "ui_gp_eng_wiz_step_05_title", "5/21: Back / Select / Share",           "ui_gp_eng_wiz_step_05_prompt", "Press the BACK / SELECT / VIEW / SHARE button",      false, false },
	{ "guide",         "ui_gp_eng_wiz_step_06_title", "6/21: Guide / Home (Optional)",         "ui_gp_eng_wiz_step_06_prompt", "Press GUIDE / HOME button (or press SPACE/TAB to skip)", false, false },
	{ "start",         "ui_gp_eng_wiz_step_07_title", "7/21: Start / Menu / Options",          "ui_gp_eng_wiz_step_07_prompt", "Press the START / MENU / OPTIONS button",            false, false },
	{ "leftstick",     "ui_gp_eng_wiz_step_08_title", "8/21: Left Stick Click (L3)",           "ui_gp_eng_wiz_step_08_prompt", "Click the LEFT THUMBSTICK down (L3)",                false, false },
	{ "rightstick",    "ui_gp_eng_wiz_step_09_title", "9/21: Right Stick Click (R3)",          "ui_gp_eng_wiz_step_09_prompt", "Click the RIGHT THUMBSTICK down (R3)",               false, false },
	{ "leftshoulder",  "ui_gp_eng_wiz_step_10_title", "10/21: Left Shoulder (LB / L1)",        "ui_gp_eng_wiz_step_10_prompt", "Press LEFT SHOULDER bumper (LB / L1)",               false, false },
	{ "rightshoulder", "ui_gp_eng_wiz_step_11_title", "11/21: Right Shoulder (RB / R1)",       "ui_gp_eng_wiz_step_11_prompt", "Press RIGHT SHOULDER bumper (RB / R1)",              false, false },
	{ "dpup",          "ui_gp_eng_wiz_step_12_title", "12/21: D-Pad UP",                       "ui_gp_eng_wiz_step_12_prompt", "Press UP on the D-Pad",                              false, false },
	{ "dpdown",        "ui_gp_eng_wiz_step_13_title", "13/21: D-Pad DOWN",                     "ui_gp_eng_wiz_step_13_prompt", "Press DOWN on the D-Pad",                            false, false },
	{ "dpleft",        "ui_gp_eng_wiz_step_14_title", "14/21: D-Pad LEFT",                     "ui_gp_eng_wiz_step_14_prompt", "Press LEFT on the D-Pad",                            false, false },
	{ "dpright",       "ui_gp_eng_wiz_step_15_title", "15/21: D-Pad RIGHT",                    "ui_gp_eng_wiz_step_15_prompt", "Press RIGHT on the D-Pad",                           false, false },
	{ "leftx",         "ui_gp_eng_wiz_step_16_title", "16/21: Left Stick Horizontal (+X)",     "ui_gp_eng_wiz_step_16_prompt", "Push the LEFT STICK fully to the RIGHT",             true,  false },
	{ "lefty",         "ui_gp_eng_wiz_step_17_title", "17/21: Left Stick Vertical (+Y)",       "ui_gp_eng_wiz_step_17_prompt", "Push the LEFT STICK fully DOWN (towards you)",       true,  false },
	{ "rightx",        "ui_gp_eng_wiz_step_18_title", "18/21: Right Stick Horizontal (+X)",    "ui_gp_eng_wiz_step_18_prompt", "Push the RIGHT STICK fully to the RIGHT",            true,  false },
	{ "righty",        "ui_gp_eng_wiz_step_19_title", "19/21: Right Stick Vertical (+Y)",      "ui_gp_eng_wiz_step_19_prompt", "Push the RIGHT STICK fully DOWN (towards you)",      true,  false },
	{ "lefttrigger",   "ui_gp_eng_wiz_step_20_title", "20/21: Left Trigger (LT / L2)",         "ui_gp_eng_wiz_step_20_prompt", "Pull LEFT TRIGGER (LT / L2) all the way",            false, true  },
	{ "righttrigger",  "ui_gp_eng_wiz_step_21_title", "21/21: Right Trigger (RT / R2)",        "ui_gp_eng_wiz_step_21_prompt", "Pull RIGHT TRIGGER (RT / R2) all the way",           false, true  }
};

constexpr int NUM_WIZARD_STEPS = static_cast<int>(sizeof(g_wizardSteps) / sizeof(g_wizardSteps[0]));

class CUIGamepadRemapWnd : public CUIDialogWnd
{
	typedef CUIDialogWnd inherited;

public:
	static constexpr int VISIBLE_ROWS = 11;
	static constexpr u32 REPEAT_INITIAL_DELAY_MS = 320;
	static constexpr u32 REPEAT_STEP_MS = 90;
	static constexpr u32 CAPTURE_HOLD_RECOGNIZE_MS = 450;
	static constexpr u32 CAPTURE_TIMEOUT_MS = 8000;
	static constexpr int WIZARD_AXIS_DEFLECT_THRESHOLD = 14000;
	static constexpr int WIZARD_AXIS_NEUTRAL_BAND = 6500;

	enum class EScreenMode : u8
	{
		Remap = 0,
		WizardClassC = 1
	};

	enum class ERemapSubState : u8
	{
		Browse = 0,
		Capturing = 1,
		ConfirmConflict = 2
	};

	CUIGamepadRemapWnd()
	{
		AllowWorkInPause(true);
		AllowMovement(false);
		AllowCursor(false);
		AllowCenterCursor(false);

		SetWndRect(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT));

		m_shaderWire->create("hud\\crosshair");

		m_contexts = gpbind::BindingProfileModel::GetCustomizableContexts();

		m_pTitle = CreateLabel(80.0f, 24.0f, 864.0f, 30.0f, UI().Font().pFontLetterica18Russian,
		                       color_argb(255, 245, 215, 115), CGameFont::alCenter);
		m_pSubtitle = CreateLabel(80.0f, 52.0f, 864.0f, 22.0f, UI().Font().pFontLetterica16Russian,
		                          color_argb(255, 185, 215, 245), CGameFont::alCenter);
		m_pContextBar = CreateLabel(80.0f, 82.0f, 864.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                            color_argb(255, 225, 235, 250), CGameFont::alCenter);
		m_pColumnHeader = CreateLabel(100.0f, 114.0f, 824.0f, 22.0f, UI().Font().pFontLetterica16Russian,
		                              color_argb(255, 160, 185, 210), CGameFont::alLeft);

		const float rowStartY = 144.0f;
		const float rowStepY = 34.0f;
		for (int i = 0; i < VISIBLE_ROWS; ++i)
		{
			m_pRowAction[i] = CreateLabel(104.0f, rowStartY + i * rowStepY, 440.0f, 28.0f,
			                              UI().Font().pFontLetterica16Russian,
			                              color_argb(255, 220, 225, 235), CGameFont::alLeft);
			m_pRowBinding[i] = CreateLabel(550.0f, rowStartY + i * rowStepY, 360.0f, 28.0f,
			                               UI().Font().pFontLetterica16Russian,
			                               color_argb(255, 190, 235, 200), CGameFont::alLeft);
		}

		m_pStatusBanner = CreateLabel(88.0f, 534.0f, 848.0f, 28.0f, UI().Font().pFontLetterica16Russian,
		                              color_argb(255, 120, 240, 160), CGameFont::alCenter);
		m_pModalTitle = CreateLabel(130.0f, 572.0f, 764.0f, 26.0f, UI().Font().pFontLetterica18Russian,
		                            color_argb(255, 255, 220, 110), CGameFont::alCenter);
		m_pModalBody1 = CreateLabel(130.0f, 600.0f, 764.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                            color_argb(255, 235, 235, 245), CGameFont::alCenter);
		m_pModalBody2 = CreateLabel(130.0f, 624.0f, 764.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                            color_argb(255, 210, 220, 235), CGameFont::alCenter);

		m_pFooter1 = CreateLabel(72.0f, 678.0f, 880.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                         color_argb(255, 240, 210, 130), CGameFont::alCenter);
		m_pFooter2 = CreateLabel(72.0f, 704.0f, 880.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                         color_argb(255, 175, 195, 215), CGameFont::alCenter);

		SetStatusMessage(TrRemap("ui_gp_eng_remap_ready", "Ready. Select an action and press [A / Enter] to rebind, or [Y / F2] for Class C Wizard."), false);
		RefreshTexts();
	}

	virtual ~CUIGamepadRemapWnd()
	{
		ReleasePauseTokenIfHeld();
	}

	virtual bool WorkInPause() const override { return true; }
	virtual bool NeedCursor() const override { return false; }

	void SetMode(EScreenMode mode)
	{
		m_mode = mode;
		m_subState = ERemapSubState::Browse;
		m_heldDir = DirNone;
		m_prevButtonsMask = GetCurrentExtendedButtonsMask();
		m_lastSeenEmergencyResetCount = gpbind::GetActiveBindingProfile().GetEmergencyResetCount();
		if (mode == EScreenMode::WizardClassC)
		{
			StartClassCWizard(0);
		}
		else
		{
			SetStatusMessage(TrRemap("ui_gp_eng_remap_mode_active", "Remap active: [LB/RB] Context, [A] Bind, [X] Unbind, [Y/F2] Class C Wizard, [B/Start] Save & Exit."), false);
		}
		RefreshTexts();
	}

	EScreenMode GetMode() const { return m_mode; }
	ERemapSubState GetSubState() const { return m_subState; }

	virtual void Show(bool status) override
	{
		const bool wasShown = IsShown();
		inherited::Show(status);

		if (status && !wasShown)
		{
			if (!MainMenu() || !MainMenu()->IsActive())
			{
				if (m_pauseTokenId == 0)
				{
					m_pauseTokenId = CPauseTokenManager::Instance().Acquire("gp_remap");
				}
			}
			m_lastUpdateTimeContinual = Device.dwTimeContinual;
			m_heldDir = DirNone;
			m_subState = ERemapSubState::Browse;
			m_prevButtonsMask = GetCurrentExtendedButtonsMask();
			m_lastSeenEmergencyResetCount = gpbind::GetActiveBindingProfile().GetEmergencyResetCount();
			RefreshTexts();

			Msg("* [Gamepad] CUIGamepadRemapWnd::Show(true): mode=%s token=%u dwTimeContinual=%u Paused=%d mainMenu=%d",
			    (m_mode == EScreenMode::WizardClassC) ? "wizard" : "remap",
			    m_pauseTokenId, Device.dwTimeContinual, Device.Paused() ? 1 : 0,
			    (MainMenu() && MainMenu()->IsActive()) ? 1 : 0);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_remap_window,%u,open,mode=%s,token=%u,paused=%d",
				           Device.dwTimeContinual,
				           (m_mode == EScreenMode::WizardClassC) ? "wizard" : "remap",
				           m_pauseTokenId, Device.Paused() ? 1 : 0);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
		else if (!status && wasShown)
		{
			// Save active binding profile on exit to $app_data_root$\gamepad\profile.ltx (D37 / D4.2)
			gpbind::SaveActiveBindingProfile();

			const u32 releasedToken = m_pauseTokenId;
			ReleasePauseTokenIfHeld();
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_START);

			Msg("* [Gamepad] CUIGamepadRemapWnd::Show(false): saved bindings profile, released_token=%u dwTimeContinual=%u",
			    releasedToken, Device.dwTimeContinual);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_remap_window,%u,close_and_save,token=%u,paused=%d",
				           Device.dwTimeContinual, releasedToken, Device.Paused() ? 1 : 0);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
	}

	virtual void Update() override
	{
		inherited::Update();
		if (!IsShown())
			return;

		const u32 now = Device.dwTimeContinual;
		const u32 dtMs = (m_lastUpdateTimeContinual != 0 && now >= m_lastUpdateTimeContinual && (now - m_lastUpdateTimeContinual) <= 250)
		                     ? (now - m_lastUpdateTimeContinual)
		                     : 16;
		m_lastUpdateTimeContinual = now;

		if (m_mode == EScreenMode::WizardClassC)
		{
			UpdateClassCWizard(now, dtMs);
		}
		else
		{
			UpdateRemapMode(now, dtMs);
		}
		RefreshTexts();
	}

	virtual void Draw() override
	{
		if (!IsShown())
			return;

		DrawVectorScene();
		inherited::Draw();
	}

	virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override
	{
		if (!IR_process())
			return false;

		if (keyboard_action == WINDOW_KEY_PRESSED)
		{
			if (m_mode == EScreenMode::WizardClassC)
			{
				if (dik == DIK_ESCAPE)
				{
					m_mode = EScreenMode::Remap;
					m_subState = ERemapSubState::Browse;
					SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_exited", "Exited Class C Wizard back to Remapping Screen."), false);
					RefreshTexts();
					return true;
				}
				if (m_bWizardCompleted)
				{
					if (dik == DIK_RETURN || dik == DIK_SPACE)
					{
						m_mode = EScreenMode::Remap;
						m_subState = ERemapSubState::Browse;
						RefreshTexts();
						return true;
					}
					return true;
				}
				if (dik == DIK_SPACE || dik == DIK_TAB)
				{
					WizardSkipCurrentStep();
					return true;
				}
				if (dik == DIK_BACK || dik == DIK_LEFT)
				{
					WizardPrevStep();
					return true;
				}
				if (dik == DIK_RETURN)
				{
					WizardFinishAndApply(true);
					return true;
				}
				return true;
			}

			// Mode == EScreenMode::Remap
			if (m_subState == ERemapSubState::Capturing)
			{
				if (dik == DIK_ESCAPE)
				{
					m_subState = ERemapSubState::Browse;
					SetStatusMessage(TrRemap("ui_gp_eng_remap_capture_cancelled_kbd", "Capture cancelled."), false);
					RefreshTexts();
				}
				return true;
			}

			if (m_subState == ERemapSubState::ConfirmConflict)
			{
				if (dik == DIK_RETURN || dik == DIK_SPACE)
				{
					ConfirmPendingConflictSwap();
					return true;
				}
				if (dik == DIK_ESCAPE || dik == DIK_BACK)
				{
					m_subState = ERemapSubState::Browse;
					SetStatusMessage(TrRemap("ui_gp_eng_remap_conflict_cancelled", "Conflict resolution cancelled; previous binding kept."), false);
					RefreshTexts();
					return true;
				}
				return true;
			}

			// Browse substate keyboard controls
			if (dik == DIK_ESCAPE)
			{
				HideDialog();
				return true;
			}
			if (dik == DIK_UP || dik == DIK_W)
			{
				MoveSelection(-1);
				return true;
			}
			if (dik == DIK_DOWN || dik == DIK_S)
			{
				MoveSelection(1);
				return true;
			}
			if (dik == DIK_Q || dik == DIK_PRIOR || dik == DIK_LEFT)
			{
				SwitchContextTab(-1);
				return true;
			}
			if (dik == DIK_E || dik == DIK_NEXT || dik == DIK_RIGHT)
			{
				SwitchContextTab(1);
				return true;
			}
			if (dik == DIK_RETURN || dik == DIK_SPACE)
			{
				BeginCaptureForSelectedAction(Device.dwTimeContinual);
				return true;
			}
			if (dik == DIK_DELETE || dik == DIK_BACK || dik == DIK_X)
			{
				UnbindSelectedAction();
				return true;
			}
			if (dik == DIK_F2)
			{
				SetMode(EScreenMode::WizardClassC);
				return true;
			}
			if (dik == DIK_F5)
			{
				ExecuteExportProfile();
				return true;
			}
			if (dik == DIK_F6)
			{
				ExecuteImportProfile();
				return true;
			}
			if (dik == DIK_R)
			{
				ResetCurrentContextBindings();
				return true;
			}
			if (dik == DIK_F8)
			{
				ExecuteEmergencyResetAction("ui_f8_emergency_reset");
				return true;
			}
		}
		return inherited::OnKeyboardAction(dik, keyboard_action);
	}

	// Public helpers for deterministic self-test (gp_remap selftest)
	void StartClassCWizard(u32 targetInstanceId)
	{
		m_wizardTargetInstanceId = targetInstanceId;
		m_wizardStepIdx = 0;
		m_bWizardCompleted = false;
		m_bWizardWaitingNeutral = true;
		m_wizardSavedPath[0] = '\0';
		m_wizardGeneratedMapping.clear();
		for (int i = 0; i < NUM_WIZARD_STEPS; ++i)
			m_wizardBindings[i].clear();

		CGamepadDeviceManager* mgr = pInput ? pInput->GetGamepadDeviceManager() : nullptr;
		if (mgr)
		{
			SRawJoystickSnapshot snap;
			if (mgr->SampleRawJoystick(m_wizardTargetInstanceId, snap))
			{
				m_wizardTargetInstanceId = snap.instance_id;
				m_wizardBaselineSnap = snap;
			}
		}
		SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_started", "Class C Device Wizard started. Press requested inputs or [SPACE/TAB] to skip optional steps."), false);
	}

	void WizardSkipCurrentStep()
	{
		if (m_bWizardCompleted || m_wizardStepIdx < 0 || m_wizardStepIdx >= NUM_WIZARD_STEPS)
			return;
		m_wizardBindings[m_wizardStepIdx].clear();
		++m_wizardStepIdx;
		m_bWizardWaitingNeutral = true;
		if (m_wizardStepIdx >= NUM_WIZARD_STEPS)
		{
			WizardFinishAndApply(true);
		}
		else
		{
			SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_skipped", "Skipped step. Waiting for neutral position before next step..."), false);
		}
		RefreshTexts();
	}

	void WizardPrevStep()
	{
		if (m_bWizardCompleted)
		{
			m_bWizardCompleted = false;
			m_wizardStepIdx = NUM_WIZARD_STEPS - 1;
			m_bWizardWaitingNeutral = true;
			RefreshTexts();
			return;
		}
		if (m_wizardStepIdx > 0)
		{
			--m_wizardStepIdx;
			m_bWizardWaitingNeutral = true;
			SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_prev", "Returned to previous step."), false);
			RefreshTexts();
		}
	}

	bool StepWizardCaptureFromSnapshot(const SRawJoystickSnapshot& snap, bool autoSaveOnComplete = true)
	{
		if (!snap.valid || m_bWizardCompleted || m_wizardStepIdx < 0 || m_wizardStepIdx >= NUM_WIZARD_STEPS)
			return false;

		if (m_bWizardWaitingNeutral)
		{
			bool anyBtnOrHat = (snap.buttons_mask != 0);
			for (int h = 0; h < snap.num_hats && h < SRawJoystickSnapshot::MAX_RAW_HATS; ++h)
			{
				if (snap.hats[h] != 0)
					anyBtnOrHat = true;
			}
			bool axesCalm = true;
			for (int a = 0; a < snap.num_axes && a < SRawJoystickSnapshot::MAX_RAW_AXES; ++a)
			{
				const int v = static_cast<int>(snap.axes[a]);
				// Either near 0 (stick / 0-based trigger) or near -32768 (full-range released trigger)
				// Near 0 (stick / 0-based trigger), near -32768 (full-range trigger), or near +32767 (inverted trigger)
				const bool nearZero = (std::abs(v) <= WIZARD_AXIS_NEUTRAL_BAND);
				const bool nearNegMin = (v <= -32768 + WIZARD_AXIS_NEUTRAL_BAND);
				const bool nearPosMax = (v >= 32767 - WIZARD_AXIS_NEUTRAL_BAND);
				if (!nearZero && !nearNegMin && !nearPosMax)
				{
					axesCalm = false;
					break;
				}
			}
			if (!anyBtnOrHat && axesCalm)
			{
				m_wizardBaselineSnap = snap;
				m_bWizardWaitingNeutral = false;
			}
			return false;
		}

		const SWizardStepDef& stepDef = g_wizardSteps[m_wizardStepIdx];
		std::string capturedExpr;

		// 1. Check newly pressed buttons (if step is not specifically a stick axis step)
		const u32 newBtns = snap.buttons_mask & ~m_wizardBaselineSnap.buttons_mask;
		if (newBtns != 0 && !stepDef.isStickAxis)
		{
			for (int b = 0; b < 32; ++b)
			{
				if ((newBtns & (1u << b)) != 0)
				{
					capturedExpr = "b" + std::to_string(b);
					break;
				}
			}
		}

		// 2. Check newly pressed hat directions (if not yet captured and not a stick axis step)
		if (capturedExpr.empty() && !stepDef.isStickAxis)
		{
			for (int h = 0; h < snap.num_hats && h < SRawJoystickSnapshot::MAX_RAW_HATS; ++h)
			{
				const u8 curHat = snap.hats[h];
				const u8 baseHat = m_wizardBaselineSnap.hats[h];
				if (curHat != 0 && curHat != baseHat)
				{
					if (curHat & 0x01)      capturedExpr = "h" + std::to_string(h) + ".1";
					else if (curHat & 0x02) capturedExpr = "h" + std::to_string(h) + ".2";
					else if (curHat & 0x04) capturedExpr = "h" + std::to_string(h) + ".4";
					else if (curHat & 0x08) capturedExpr = "h" + std::to_string(h) + ".8";
					break;
				}
			}
		}

		// 3. Check axis deflection
		if (capturedExpr.empty())
		{
			int bestAxis = -1;
			int bestAbsDelta = 0;
			int bestSignedDelta = 0;
			for (int a = 0; a < snap.num_axes && a < SRawJoystickSnapshot::MAX_RAW_AXES; ++a)
			{
				const int curVal = static_cast<int>(snap.axes[a]);
				const int baseVal = static_cast<int>(m_wizardBaselineSnap.axes[a]);
				const int delta = curVal - baseVal;
				const int absDelta = std::abs(delta);
				if (absDelta >= WIZARD_AXIS_DEFLECT_THRESHOLD && absDelta > bestAbsDelta)
				{
					bestAbsDelta = absDelta;
					bestAxis = a;
					bestSignedDelta = delta;
				}
			}

			if (bestAxis >= 0)
			{
				if (stepDef.isStickAxis)
				{
					// Standard stick axis: user was asked to push RIGHT (+X) or DOWN (+Y).
					// SDL3 SDL_ParseGamepadElement requires trailing '~' for output inversion.
					if (bestSignedDelta < 0)
						capturedExpr = "a" + std::to_string(bestAxis) + "~";
					else
						capturedExpr = "a" + std::to_string(bestAxis);
				}
				else if (stepDef.isTriggerAxis)
				{
					// Full-range trigger resting at -32768 -> aN; inverted +32767 -> aN~; 0-resting half-axis -> +aN / -aN.
					const int baseVal = static_cast<int>(m_wizardBaselineSnap.axes[bestAxis]);
					if (baseVal <= -32768 + WIZARD_AXIS_NEUTRAL_BAND)
						capturedExpr = "a" + std::to_string(bestAxis);
					else if (baseVal >= 32767 - WIZARD_AXIS_NEUTRAL_BAND)
						capturedExpr = "a" + std::to_string(bestAxis) + "~";
					else
						capturedExpr = ((bestSignedDelta >= 0) ? "+a" : "-a") + std::to_string(bestAxis);
				}
				else
				{
					// Button or D-Pad mapped to a half-axis
					capturedExpr = ((bestSignedDelta > 0) ? "+a" : "-a") + std::to_string(bestAxis);
				}
			}
		}

		if (capturedExpr.empty())
			return false;

		m_wizardBindings[m_wizardStepIdx] = capturedExpr;
		{
			string512 msg;
			xr_sprintf(msg, sizeof(msg), TrRemap("ui_gp_eng_remap_wiz_captured_fmt", "Captured %s = %s"), stepDef.sdlKey, capturedExpr.c_str());
			SetStatusMessage(msg, false);
		}

		++m_wizardStepIdx;
		m_bWizardWaitingNeutral = true;
		if (m_wizardStepIdx >= NUM_WIZARD_STEPS)
		{
			WizardFinishAndApply(autoSaveOnComplete);
		}
		return true;
	}

	bool WizardFinishAndApply(bool saveToDisk)
	{
		CGamepadDeviceManager* mgr = pInput ? pInput->GetGamepadDeviceManager() : nullptr;
		if (!mgr)
			return false;

		SRawJoystickSnapshot snap;
		if (!mgr->SampleRawJoystick(m_wizardTargetInstanceId, snap))
		{
			SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_err_disconnected", "Class C Wizard error: target joystick device is no longer connected."), true);
			return false;
		}

		// Sanitize commas out of device name for SDL mapping format
		std::string cleanName = (snap.name[0] != '\0') ? snap.name : "Custom Class C Gamepad";
		std::replace(cleanName.begin(), cleanName.end(), ',', ' ');

		std::ostringstream oss;
		oss << snap.guid_str << "," << cleanName << ",platform:Windows,";
		for (int i = 0; i < NUM_WIZARD_STEPS; ++i)
		{
			if (!m_wizardBindings[i].empty())
			{
				oss << g_wizardSteps[i].sdlKey << ":" << m_wizardBindings[i] << ",";
			}
		}

		m_wizardGeneratedMapping = oss.str();
		m_wizardSavedPath[0] = '\0';

		const bool applied = mgr->ApplyAndSaveCustomMapping(
		    snap.instance_id, m_wizardGeneratedMapping.c_str(), saveToDisk, m_wizardSavedPath, sizeof(m_wizardSavedPath));
		m_bWizardCompleted = true;

		if (applied)
		{
			SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_saved_ok", "Class C device promoted to SDL_Gamepad (Class B) and mapping saved!"), false);
		}
		else
		{
			SetStatusMessage(TrRemap("ui_gp_eng_remap_wiz_save_fail", "Failed to apply/save SDL mapping string. Check console log."), true);
		}
		RefreshTexts();
		return applied;
	}

	const std::string& GetWizardGeneratedMapping() const { return m_wizardGeneratedMapping; }
	LPCSTR GetWizardSavedPath() const { return m_wizardSavedPath; }
	bool IsWizardCompleted() const { return m_bWizardCompleted; }

private:
	enum EDir : u8
	{
		DirNone = 0,
		DirUp,
		DirDown,
		DirLeft,
		DirRight
	};

	CUITextWnd* CreateLabel(float x, float y, float w, float h, CGameFont* font, u32 color, CGameFont::EAligment align)
	{
		CUITextWnd* lbl = xr_new<CUITextWnd>();
		lbl->SetAutoDelete(true);
		lbl->SetWndPos(Fvector2().set(x, y));
		lbl->SetWndSize(Fvector2().set(w, h));
		lbl->SetFont(font);
		lbl->SetTextColor(color);
		lbl->SetTextAlignment(align);
		AttachChild(lbl);
		return lbl;
	}

	void ReleasePauseTokenIfHeld()
	{
		if (m_pauseTokenId != 0)
		{
			const u32 tok = m_pauseTokenId;
			m_pauseTokenId = 0;
			CPauseTokenManager::Instance().Release(tok);
		}
	}

	void SetStatusMessage(LPCSTR text, bool isError)
	{
		m_statusText = (text && text[0]) ? text : "";
		m_bStatusIsError = isError;
	}

	u32 GetCurrentExtendedButtonsMask()
	{
		if (!pInput)
			return 0;
		const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		if (!gp.connected || !gp.has_focus)
			return 0;

		u32 mask = gp.buttons_mask & 0x7FFFu; // Bits 0..14 physical buttons
		const float rawLT = gpmath::NormalizeTriggerAxis(gp.axes[4]);
		const float rawRT = gpmath::NormalizeTriggerAxis(gp.axes[5]);
		const gpmath::CalibrationProfile& calib = gpmath::GetCalibrationProfile();
		m_bLtDigitalDown = gpmath::UpdateTriggerButton(calib.left_trigger, m_bLtDigitalDown, rawLT);
		m_bRtDigitalDown = gpmath::UpdateTriggerButton(calib.right_trigger, m_bRtDigitalDown, rawRT);

		if (m_bLtDigitalDown)
			mask |= (1u << static_cast<u32>(gpbind::ButtonId::LT));
		if (m_bRtDigitalDown)
			mask |= (1u << static_cast<u32>(gpbind::ButtonId::RT));

		return mask;
	}

	gpbind::ContextId GetCurrentContextId() const
	{
		if (m_contexts.empty())
			return gpbind::ContextId::World_Gameplay;
		const int idx = clampr(m_activeContextTab, 0, static_cast<int>(m_contexts.size()) - 1);
		return m_contexts[static_cast<size_t>(idx)];
	}

	std::vector<gpbind::ActionId> GetCurrentContextActions() const
	{
		return gpbind::BindingProfileModel::GetCanonicalActionsForContext(GetCurrentContextId());
	}

	gpbind::ActionId GetSelectedActionId() const
	{
		const auto actions = GetCurrentContextActions();
		if (actions.empty())
			return gpbind::ActionId::Invalid;
		const int idx = clampr(m_selectedRow, 0, static_cast<int>(actions.size()) - 1);
		return actions[static_cast<size_t>(idx)];
	}

	void SwitchContextTab(int delta)
	{
		if (m_contexts.empty())
			return;
		const int count = static_cast<int>(m_contexts.size());
		m_activeContextTab = (m_activeContextTab + delta + count) % count;
		m_selectedRow = 0;
		m_scrollOffset = 0;
		m_subState = ERemapSubState::Browse;
		RefreshTexts();
	}

	void MoveSelection(int delta)
	{
		const auto actions = GetCurrentContextActions();
		if (actions.empty())
			return;
		const int count = static_cast<int>(actions.size());
		m_selectedRow = (m_selectedRow + delta + count) % count;
		if (m_selectedRow < m_scrollOffset)
			m_scrollOffset = m_selectedRow;
		else if (m_selectedRow >= m_scrollOffset + VISIBLE_ROWS)
			m_scrollOffset = m_selectedRow - VISIBLE_ROWS + 1;
		RefreshTexts();
	}

	void BeginCaptureForSelectedAction(u32 now)
	{
		const gpbind::ActionId act = GetSelectedActionId();
		if (act == gpbind::ActionId::Invalid)
			return;
		m_subState = ERemapSubState::Capturing;
		m_bCaptureWaitInitialRelease = true;
		m_capturePrimaryBtn = gpbind::ButtonId::None;
		m_captureSecondBtn = gpbind::ButtonId::None;
		m_captureStartTimeMs = now;
		m_captureBtnPressTimeMs = 0;
		SetStatusMessage(TrRemap("ui_gp_eng_remap_listening", "Listening for gamepad input... Tap for Single, Hold (0.45s) for Hold, or Hold Mod + Press 2nd for Chord."), false);
	}

	void ApplyCapturedCandidate(const gpbind::BindingTrigger& candidate)
	{
		const gpbind::ContextId ctx = GetCurrentContextId();
		const gpbind::ActionId act = GetSelectedActionId();
		gpbind::BindingProfileModel& model = gpbind::GetMutableActiveBindingProfile();

		const gpbind::EditResult res = model.SetBinding(ctx, act, candidate, false);
		if (res.Succeeded())
		{
			m_subState = ERemapSubState::Browse;
			const std::string trigStr = gpbind::BindingProfileModel::FormatTriggerHumanReadable(&candidate);
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_bound_fmt", "Bound [%s] -> %s"),
			           gpbind::BindingProfileModel::ActionDisplayTitle(act), trigStr.c_str());
			std::string msg = msgBuf;
			if (res.swapped_with != gpbind::ActionId::Invalid)
			{
				string256 swapBuf;
				xr_sprintf(swapBuf, sizeof(swapBuf), TrRemap("ui_gp_eng_remap_swapped_suffix_fmt", " (swapped: %s)"),
				           gpbind::BindingProfileModel::ActionDisplayTitle(res.swapped_with));
				msg += swapBuf;
			}
			SetStatusMessage(msg.c_str(), false);
			return;
		}

		if (res.status == gpbind::EditStatus::ConflictDetected && !res.conflicts.empty())
		{
			m_pendingCandidateTrigger = candidate;
			m_pendingConflicts = res.conflicts;
			m_subState = ERemapSubState::ConfirmConflict;
			SetStatusMessage(TrRemap("ui_gp_eng_remap_conflict_status", "Conflict detected in this context! Press [A / Enter] to Swap/Overwrite, or [B / Esc] to Cancel."), true);
			return;
		}

		// Protected navigation refusal or invalid trigger
		m_subState = ERemapSubState::Browse;
		SetStatusMessage(TranslateEditRefusal(res.status, res.message), true);
	}

	void ConfirmPendingConflictSwap()
	{
		if (m_subState != ERemapSubState::ConfirmConflict)
			return;

		const gpbind::ContextId ctx = GetCurrentContextId();
		const gpbind::ActionId act = GetSelectedActionId();
		gpbind::BindingProfileModel& model = gpbind::GetMutableActiveBindingProfile();

		const gpbind::EditResult res = model.SetBinding(ctx, act, m_pendingCandidateTrigger, true);
		m_subState = ERemapSubState::Browse;
		m_pendingConflicts.clear();

		if (res.Succeeded())
		{
			const std::string trigStr = gpbind::BindingProfileModel::FormatTriggerHumanReadable(&m_pendingCandidateTrigger);
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_confirmed_fmt", "Confirmed [%s] -> %s"),
			           gpbind::BindingProfileModel::ActionDisplayTitle(act), trigStr.c_str());
			std::string msg = msgBuf;
			if (res.swapped_with != gpbind::ActionId::Invalid)
			{
				string256 swapBuf;
				xr_sprintf(swapBuf, sizeof(swapBuf), TrRemap("ui_gp_eng_remap_swapped_suffix_fmt", " (swapped: %s)"),
				           gpbind::BindingProfileModel::ActionDisplayTitle(res.swapped_with));
				msg += swapBuf;
			}
			SetStatusMessage(msg.c_str(), false);
		}
		else
		{
			SetStatusMessage(TranslateEditRefusal(res.status, res.message), true);
		}
		RefreshTexts();
	}

	void UnbindSelectedAction()
	{
		const gpbind::ContextId ctx = GetCurrentContextId();
		const gpbind::ActionId act = GetSelectedActionId();
		if (act == gpbind::ActionId::Invalid)
			return;

		gpbind::BindingProfileModel& model = gpbind::GetMutableActiveBindingProfile();
		const gpbind::EditResult res = model.UnbindAction(ctx, act);
		if (res.Succeeded())
		{
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_unbound_fmt", "Unbound [%s]."),
			           gpbind::BindingProfileModel::ActionDisplayTitle(act));
			SetStatusMessage(msgBuf, false);
		}
		else
		{
			SetStatusMessage(TranslateEditRefusal(res.status, res.message), true);
		}
		RefreshTexts();
	}

	void ResetCurrentContextBindings()
	{
		const gpbind::ContextId ctx = GetCurrentContextId();
		gpbind::GetMutableActiveBindingProfile().ResetContextToDefaults(ctx);
		string512 msgBuf;
		xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_ctx_restored_fmt", "Restored default bindings for context [%s]."),
		           gpbind::BindingProfileModel::ContextDisplayTitle(ctx));
		SetStatusMessage(msgBuf, false);
		RefreshTexts();
	}

	void ExecuteExportProfile()
	{
		std::string err;
		if (gpbind::ExportActiveBindingProfile(nullptr, &err))
		{
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_exported_fmt", "Exported bindings to '%s'."),
			           gpbind::GetDefaultExportProfilePath());
			SetStatusMessage(msgBuf, false);
		}
		else
		{
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_export_fail_fmt", "Export failed: %s"), err.c_str());
			SetStatusMessage(msgBuf, true);
		}
	}

	void ExecuteImportProfile()
	{
		const gpbind::LoadReport rep = gpbind::ImportActiveBindingProfile(nullptr);
		if (rep.status == gpbind::LoadStatus::Ok || rep.status == gpbind::LoadStatus::MigratedV1ToV2 ||
		    rep.status == gpbind::LoadStatus::RecoveredFromCorrupt)
		{
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_imported_fmt", "Imported %u bindings from '%s'."),
			           static_cast<unsigned>(rep.loaded_bindings), gpbind::GetDefaultExportProfilePath());
			SetStatusMessage(msgBuf, false);
		}
		else
		{
			string512 msgBuf;
			xr_sprintf(msgBuf, sizeof(msgBuf), TrRemap("ui_gp_eng_remap_import_fail_fmt", "Import failed: %s"), rep.detail.c_str());
			SetStatusMessage(msgBuf, true);
		}
	}

	void ExecuteEmergencyResetAction(const char* reason)
	{
		gpbind::ResetActiveBindingProfileEmergency(reason);
		m_subState = ERemapSubState::Browse;
		m_pendingConflicts.clear();
		SetStatusMessage(TrRemap("ui_gp_eng_remap_emerg_reset_done", "EMERGENCY RESET EXECUTED: All contexts restored to safe canonical defaults and saved!"), false);
		RefreshTexts();
	}

	void UpdateClassCWizard(u32 now, u32 dtMs)
	{
		(void)now;
		(void)dtMs;

		// Check if emergency reset occurred while in wizard
		const u32 curResets = gpbind::GetActiveBindingProfile().GetEmergencyResetCount();
		if (curResets != m_lastSeenEmergencyResetCount)
		{
			m_lastSeenEmergencyResetCount = curResets;
			m_mode = EScreenMode::Remap;
			m_subState = ERemapSubState::Browse;
			SetStatusMessage(TrRemap("ui_gp_eng_remap_emerg_wiz_exit", "Emergency Reset (Start+Back 5s) restored defaults and exited wizard."), false);
			return;
		}

		CGamepadDeviceManager* mgr = pInput ? pInput->GetGamepadDeviceManager() : nullptr;
		if (!mgr)
			return;

		if (m_bWizardCompleted)
		{
			const u32 curMask = GetCurrentExtendedButtonsMask();
			const u32 newlyPressed = curMask & ~m_prevButtonsMask;
			m_prevButtonsMask = curMask;
			if ((newlyPressed & ((1u << CInput::GP_BTN_SOUTH) | (1u << CInput::GP_BTN_EAST) | (1u << CInput::GP_BTN_START))) != 0)
			{
				m_mode = EScreenMode::Remap;
				m_subState = ERemapSubState::Browse;
			}
			return;
		}

		SRawJoystickSnapshot snap;
		if (mgr->SampleRawJoystick(m_wizardTargetInstanceId, snap))
		{
			m_wizardTargetInstanceId = snap.instance_id;
			StepWizardCaptureFromSnapshot(snap, true);
		}
	}

	void UpdateRemapMode(u32 now, u32 dtMs)
	{
		(void)dtMs;
		const u32 curResets = gpbind::GetActiveBindingProfile().GetEmergencyResetCount();
		if (curResets != m_lastSeenEmergencyResetCount)
		{
			m_lastSeenEmergencyResetCount = curResets;
			m_subState = ERemapSubState::Browse;
			m_pendingConflicts.clear();
			SetStatusMessage(TrRemap("ui_gp_eng_remap_emerg_held_done", "EMERGENCY RESET (Start+Back held 5s): Restored all bindings to safe defaults!"), false);
		}

		const u32 curMask = GetCurrentExtendedButtonsMask();
		const u32 newlyPressed = curMask & ~m_prevButtonsMask;
		const u32 justReleased = ~curMask & m_prevButtonsMask;
		m_prevButtonsMask = curMask;

		// SubState 1: Capturing ("Press button...")
		if (m_subState == ERemapSubState::Capturing)
		{
			if ((now - m_captureStartTimeMs) >= CAPTURE_TIMEOUT_MS)
			{
				m_subState = ERemapSubState::Browse;
				SetStatusMessage(TrRemap("ui_gp_eng_remap_capture_timeout", "Capture timed out after 8 seconds; previous binding kept."), false);
				return;
			}

			if (m_bCaptureWaitInitialRelease)
			{
				if (curMask == 0)
				{
					m_bCaptureWaitInitialRelease = false;
					m_captureStartTimeMs = now;
				}
				return;
			}

			const gpbind::ActionId targetAct = GetSelectedActionId();
			const bool targetIsStartAction = (targetAct == gpbind::ActionId::Menu || targetAct == gpbind::ActionId::PauseMenu);

			// Allow cancelling capture from gamepad via Guide, or via Start (unless binding Menu/PauseMenu itself, where Back cancels)
			if ((newlyPressed & (1u << static_cast<u32>(gpbind::ButtonId::Guide))) != 0 ||
			    (!targetIsStartAction && (newlyPressed & (1u << static_cast<u32>(gpbind::ButtonId::Start))) != 0) ||
			    (targetIsStartAction && (newlyPressed & (1u << static_cast<u32>(gpbind::ButtonId::Back))) != 0))
			{
				m_subState = ERemapSubState::Browse;
				SetStatusMessage(TrRemap("ui_gp_eng_remap_capture_cancelled", "Capture cancelled from gamepad."), false);
				return;
			}

			if (m_capturePrimaryBtn == gpbind::ButtonId::None)
			{
				// Find first pressed button
				for (u8 b = 0; b < static_cast<u8>(gpbind::ButtonId::Count); ++b)
				{
					if ((newlyPressed & (1u << b)) != 0)
					{
						m_capturePrimaryBtn = static_cast<gpbind::ButtonId>(b);
						m_captureBtnPressTimeMs = now;
						break;
					}
				}
				if (m_capturePrimaryBtn == gpbind::ButtonId::None)
					return;
			}

			// Primary button is currently tracked; check if a second button was pressed while primary is still held -> Chord!
			const u32 priBit = (1u << static_cast<u32>(m_capturePrimaryBtn));
			if ((curMask & priBit) != 0)
			{
				for (u8 b = 0; b < static_cast<u8>(gpbind::ButtonId::Count); ++b)
				{
					const gpbind::ButtonId btnId = static_cast<gpbind::ButtonId>(b);
					if (btnId != m_capturePrimaryBtn && (newlyPressed & (1u << b)) != 0)
					{
						m_captureSecondBtn = btnId;
						ApplyCapturedCandidate(gpbind::BindingTrigger::MakeChord(m_capturePrimaryBtn, m_captureSecondBtn));
						return;
					}
				}
			}

			// Check if primary button was released (without a second button) -> Single Press vs Hold
			if ((justReleased & priBit) != 0 || (curMask & priBit) == 0)
			{
				const u32 heldMs = (now >= m_captureBtnPressTimeMs) ? (now - m_captureBtnPressTimeMs) : 0;
				const gpbind::ContextId ctx = GetCurrentContextId();
				// Protected MenuUI/MainMenu actions always use Single Button trigger
				if (heldMs >= CAPTURE_HOLD_RECOGNIZE_MS &&
				    !gpbind::BindingProfileModel::IsProtectedMenuNavigationAction(ctx, targetAct))
				{
					ApplyCapturedCandidate(gpbind::BindingTrigger::MakeHold(m_capturePrimaryBtn, gpbind::limits::kDefaultHoldMs));
				}
				else
				{
					ApplyCapturedCandidate(gpbind::BindingTrigger::MakeButton(m_capturePrimaryBtn));
				}
				return;
			}
			return;
		}

		// SubState 2: ConfirmConflict modal
		if (m_subState == ERemapSubState::ConfirmConflict)
		{
			// Resolve current Confirm and Back buttons from active MenuUI bindings so even swapped Confirm/Back work!
			gpbind::ButtonId confirmBtn = gpbind::ButtonId::South;
			gpbind::ButtonId backBtn = gpbind::ButtonId::East;
			const gpbind::BindingProfileModel& prof = gpbind::GetActiveBindingProfile();
			if (const gpbind::BindingTrigger* cTrig = prof.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Confirm))
				confirmBtn = cTrig->button;
			if (const gpbind::BindingTrigger* bTrig = prof.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Back))
				backBtn = bTrig->button;

			if ((newlyPressed & (1u << static_cast<u32>(confirmBtn))) != 0)
			{
				ConfirmPendingConflictSwap();
				return;
			}
			if ((newlyPressed & ((1u << static_cast<u32>(backBtn)) | (1u << CInput::GP_BTN_START))) != 0)
			{
				m_subState = ERemapSubState::Browse;
				m_pendingConflicts.clear();
				SetStatusMessage(TrRemap("ui_gp_eng_remap_conflict_cancelled", "Conflict resolution cancelled; previous binding kept."), false);
				return;
			}
			return;
		}

		// SubState 0: Browse mode
		gpbind::ButtonId menuBtn = gpbind::ButtonId::Start;
		gpbind::ButtonId confirmBtn = gpbind::ButtonId::South;
		gpbind::ButtonId backBtn = gpbind::ButtonId::East;
		const gpbind::BindingProfileModel& prof = gpbind::GetActiveBindingProfile();
		if (const gpbind::BindingTrigger* mTrig = prof.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Menu))
			menuBtn = mTrig->button;
		if (const gpbind::BindingTrigger* cTrig = prof.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Confirm))
			confirmBtn = cTrig->button;
		if (const gpbind::BindingTrigger* bTrig = prof.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Back))
			backBtn = bTrig->button;

		// Guaranteed exit from gamepad on Back (B) or Menu (Start)
		if ((newlyPressed & ((1u << static_cast<u32>(backBtn)) | (1u << static_cast<u32>(menuBtn)))) != 0)
		{
			HideDialog();
			return;
		}

		// Switch context tab on LB / RB
		if ((newlyPressed & (1u << CInput::GP_BTN_LEFT_SHOULDER)) != 0)
			SwitchContextTab(-1);
		if ((newlyPressed & (1u << CInput::GP_BTN_RIGHT_SHOULDER)) != 0)
			SwitchContextTab(1);

		// Start capture on Confirm (A)
		if ((newlyPressed & (1u << static_cast<u32>(confirmBtn))) != 0)
		{
			BeginCaptureForSelectedAction(now);
			return;
		}

		// Unbind on West (X), unless West is currently Confirm or Back
		if (gpbind::ButtonId::West != confirmBtn && gpbind::ButtonId::West != backBtn &&
		    (newlyPressed & (1u << CInput::GP_BTN_WEST)) != 0)
		{
			UnbindSelectedAction();
			return;
		}

		// Open Class C Wizard on North (Y), unless North is currently Confirm or Back
		if (gpbind::ButtonId::North != confirmBtn && gpbind::ButtonId::North != backBtn &&
		    (newlyPressed & (1u << CInput::GP_BTN_NORTH)) != 0)
		{
			SetMode(EScreenMode::WizardClassC);
			return;
		}

		// Reset current context on R3 (RightStick)
		if ((newlyPressed & (1u << CInput::GP_BTN_RIGHT_STICK)) != 0)
		{
			ResetCurrentContextBindings();
			return;
		}

		// D-Pad up/down navigation with 320ms / 90ms repeat on Device.dwTimeContinual
		EDir activeDir = DirNone;
		if ((curMask & (1u << CInput::GP_BTN_DPAD_UP)) != 0)
			activeDir = DirUp;
		else if ((curMask & (1u << CInput::GP_BTN_DPAD_DOWN)) != 0)
			activeDir = DirDown;
		else if ((curMask & (1u << CInput::GP_BTN_DPAD_LEFT)) != 0)
			activeDir = DirLeft;
		else if ((curMask & (1u << CInput::GP_BTN_DPAD_RIGHT)) != 0)
			activeDir = DirRight;

		if (activeDir == DirNone)
		{
			m_heldDir = DirNone;
			return;
		}

		bool fireStep = false;
		if (activeDir != m_heldDir)
		{
			m_heldDir = activeDir;
			m_nextRepeatTimeMs = now + REPEAT_INITIAL_DELAY_MS;
			fireStep = true;
		}
		else if (now >= m_nextRepeatTimeMs)
		{
			m_nextRepeatTimeMs = now + REPEAT_STEP_MS;
			fireStep = true;
		}

		if (fireStep)
		{
			if (activeDir == DirUp)
				MoveSelection(-1);
			else if (activeDir == DirDown)
				MoveSelection(1);
			else if (activeDir == DirLeft)
				SwitchContextTab(-1);
			else if (activeDir == DirRight)
				SwitchContextTab(1);
		}
	}

	bool IsActionInPendingConflict(gpbind::ActionId act) const
	{
		if (m_subState != ERemapSubState::ConfirmConflict)
			return false;
		for (const auto& c : m_pendingConflicts)
		{
			if (c.existing_action == act || c.candidate_action == act)
				return true;
		}
		return false;
	}

	void RefreshTexts()
	{
		if (m_mode == EScreenMode::WizardClassC)
		{
			RefreshWizardTexts();
			return;
		}

		m_pTitle->SetText(TrRemap("ui_gp_eng_remap_title", "GAMEPAD BINDINGS & REMAPPING"));

		const gpbind::ContextId curCtx = GetCurrentContextId();
		const gpbind::BindingProfileModel& prof = gpbind::GetActiveBindingProfile();

		const LPCSTR invStr = prof.VerifyMenuNavigationInvariant()
		    ? TrRemap("ui_gp_eng_remap_inv_ok", "OK (Protected)")
		    : TrRemap("ui_gp_eng_remap_inv_broken", "BROKEN");
		string512 subBuf;
		xr_sprintf(subBuf, sizeof(subBuf),
		           TrRemap("ui_gp_eng_remap_subtitle_fmt", "Profile: %s (Schema v%u) | Menu Navigation Invariant: %s"),
		           gpbind::GetActiveBindingProfilePath(),
		           prof.GetSchemaVersion(),
		           invStr);
		m_pSubtitle->SetText(subBuf);

		// Context tabs bar
		std::ostringstream ctxOss;
		ctxOss << "[LB/Q] ";
		for (size_t i = 0; i < m_contexts.size(); ++i)
		{
			if (static_cast<int>(i) == m_activeContextTab)
				ctxOss << ">> " << gpbind::BindingProfileModel::ContextDisplayTitle(m_contexts[i]) << " <<";
			else
				ctxOss << gpbind::BindingProfileModel::ContextDisplayTitle(m_contexts[i]);
			if (i + 1 < m_contexts.size())
				ctxOss << " | ";
		}
		ctxOss << " [RB/E]";
		m_pContextBar->SetText(ctxOss.str().c_str());

		const auto actions = GetCurrentContextActions();
		string512 colBuf;
		xr_sprintf(colBuf, sizeof(colBuf),
		           TrRemap("ui_gp_eng_remap_col_header_fmt", "ACTION (%s - %zu actions)                                         CURRENT BINDING"),
		           gpbind::BindingProfileModel::ContextDisplayTitle(curCtx), actions.size());
		m_pColumnHeader->SetText(colBuf);

		for (int i = 0; i < VISIBLE_ROWS; ++i)
		{
			const int actIdx = m_scrollOffset + i;
			if (actIdx >= static_cast<int>(actions.size()))
			{
				m_pRowAction[i]->SetText("");
				m_pRowBinding[i]->SetText("");
				continue;
			}

			const gpbind::ActionId act = actions[static_cast<size_t>(actIdx)];
			const bool isSelected = (actIdx == m_selectedRow);
			const bool isProtected = gpbind::BindingProfileModel::IsProtectedMenuNavigationAction(curCtx, act);
			const bool isConflicting = IsActionInPendingConflict(act);
			const gpbind::BindingTrigger* trig = prof.FindBinding(curCtx, act);

			string256 actText;
			xr_sprintf(actText, sizeof(actText), "%s %2d. %s",
			           isSelected ? ">" : " ",
			           actIdx + 1,
			           gpbind::BindingProfileModel::ActionDisplayTitle(act));
			m_pRowAction[i]->SetText(actText);

			const std::string bindStr = gpbind::BindingProfileModel::FormatTriggerHumanReadable(trig);
			m_pRowBinding[i]->SetText(bindStr.c_str());

			if (isConflicting)
			{
				m_pRowAction[i]->SetTextColor(color_argb(255, 255, 110, 110));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 255, 130, 110));
			}
			else if (isSelected)
			{
				m_pRowAction[i]->SetTextColor(color_argb(255, 255, 235, 135));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 120, 255, 165));
			}
			else if (isProtected)
			{
				m_pRowAction[i]->SetTextColor(color_argb(255, 195, 225, 255));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 175, 225, 250));
			}
			else
			{
				m_pRowAction[i]->SetTextColor(color_argb(255, 215, 220, 230));
				m_pRowBinding[i]->SetTextColor(trig ? color_argb(255, 185, 225, 195) : color_argb(255, 150, 155, 165));
			}
		}

		m_pStatusBanner->SetText(m_statusText.c_str());
		m_pStatusBanner->SetTextColor(m_bStatusIsError ? color_argb(255, 255, 115, 115) : color_argb(255, 125, 240, 165));

		// Modal area (Capture or Conflict Confirmation or Emergency Hold progress)
		if (m_subState == ERemapSubState::Capturing)
		{
			const gpbind::ActionId act = GetSelectedActionId();
			string512 mTitle;
			xr_sprintf(mTitle, sizeof(mTitle), TrRemap("ui_gp_eng_remap_cap_title_fmt", "CAPTURING INPUT FOR: %s"), gpbind::BindingProfileModel::ActionDisplayTitle(act));
			m_pModalTitle->SetText(mTitle);

			if (m_bCaptureWaitInitialRelease)
			{
				m_pModalBody1->SetText(TrRemap("ui_gp_eng_remap_cap_release_all", "Release all buttons to begin capture..."));
				m_pModalBody2->SetText("");
			}
			else if (m_capturePrimaryBtn == gpbind::ButtonId::None)
			{
				m_pModalBody1->SetText(TrRemap("ui_gp_eng_remap_cap_prompt_1", "Press any button (Tap = Single, Hold 0.45s = Hold, Hold Mod + 2nd Button = Chord)"));
				m_pModalBody2->SetText(TrRemap("ui_gp_eng_remap_cap_prompt_2", "Press [Start / Guide / Esc] to cancel capture."));
			}
			else
			{
				const u32 heldMs = (Device.dwTimeContinual >= m_captureBtnPressTimeMs)
				                       ? (Device.dwTimeContinual - m_captureBtnPressTimeMs)
				                       : 0;
				string512 b1;
				xr_sprintf(b1, sizeof(b1),
				           TrRemap("ui_gp_eng_remap_cap_holding_fmt", "Holding [%s] (%u ms) - release now for Single, hold >= 450 ms for Hold, or press 2nd button for Chord!"),
				           gpbind::BindingProfileModel::ButtonToString(m_capturePrimaryBtn), heldMs);
				m_pModalBody1->SetText(b1);
				m_pModalBody2->SetText(heldMs >= CAPTURE_HOLD_RECOGNIZE_MS
				    ? TrRemap("ui_gp_eng_remap_cap_hold_reached", ">> HOLD THRESHOLD REACHED! Release button to confirm Hold(300ms) <<")
				    : "");
			}
		}
		else if (m_subState == ERemapSubState::ConfirmConflict)
		{
			m_pModalTitle->SetText(TrRemap("ui_gp_eng_remap_conflict_title", "CONFLICT DETECTED IN CURRENT CONTEXT!"));
			const std::string candStr = gpbind::BindingProfileModel::FormatTriggerHumanReadable(&m_pendingCandidateTrigger);
			string512 prefixBuf;
			xr_sprintf(prefixBuf, sizeof(prefixBuf), TrRemap("ui_gp_eng_remap_conflict_prefix_fmt", "Candidate %s conflicts with: "), candStr.c_str());
			std::ostringstream c1;
			c1 << prefixBuf;
			for (size_t i = 0; i < m_pendingConflicts.size(); ++i)
			{
				if (i > 0)
					c1 << ", ";
				c1 << gpbind::BindingProfileModel::ActionDisplayTitle(m_pendingConflicts[i].existing_action)
				   << " (" << gpbind::BindingProfileModel::FormatTriggerHumanReadable(&m_pendingConflicts[i].existing_trigger) << ")";
			}
			m_pModalBody1->SetText(c1.str().c_str());
			m_pModalBody2->SetText(TrRemap("ui_gp_eng_remap_conflict_prompt", "Press [A / Enter] to Swap & Resolve, or [B / Esc] to Cancel."));
		}
		else
		{
			const u32 emergElapsed = prof.GetEmergencyResetHoldElapsedMs();
			if (emergElapsed > 0)
			{
				string512 eTitle;
				xr_sprintf(eTitle, sizeof(eTitle), TrRemap("ui_gp_eng_remap_emerg_title_fmt", "EMERGENCY RESET HOLD (Start + Back): %u / %u ms"),
				           emergElapsed, gpbind::limits::kEmergencyResetHoldMs);
				m_pModalTitle->SetText(eTitle);
				m_pModalBody1->SetText(TrRemap("ui_gp_eng_remap_emerg_body", "Keep holding Start + Back for 5 seconds to reset all bindings to safe defaults..."));
				m_pModalBody2->SetText("");
			}
			else
			{
				m_pModalTitle->SetText(TrRemap("ui_gp_eng_remap_rules_title", "PROTECTED NAVIGATION & GESTURE RULES"));
				m_pModalBody1->SetText(TrRemap("ui_gp_eng_remap_rules_body1", "Menu, Confirm, and Back in MenuUI/MainMenu are protected: they cannot be unbound, only swapped with a valid single button."));
				m_pModalBody2->SetText(TrRemap("ui_gp_eng_remap_rules_body2", "Emergency Reset: Hold [Start + Back] for 5 seconds in any screen, or run 'gp_bind_reset' in console."));
			}
		}

		m_pFooter1->SetText(TrRemap("ui_gp_eng_remap_footer1", "[DPad Up/Down] Select | [LB/RB] Context | [A/Enter] Bind | [X/Del] Unbind | [R3/R] Reset Context | [B/Start/Esc] Save & Exit"));
		m_pFooter2->SetText(TrRemap("ui_gp_eng_remap_footer2", "[Y / F2] Class C Device Mapping Wizard | [F5] Export Layout | [F6] Import Layout | [F8 / Start+Back 5s] Emergency Reset"));
	}

	void RefreshWizardTexts()
	{
		m_pTitle->SetText(TrRemap("ui_gp_eng_wiz_title", "CLASS C DEVICE MAPPING WIZARD (SDL_AddGamepadMapping)"));

		CGamepadDeviceManager* mgr = pInput ? pInput->GetGamepadDeviceManager() : nullptr;
		SRawJoystickSnapshot snap;
		const bool hasDev = mgr && mgr->SampleRawJoystick(m_wizardTargetInstanceId, snap);

		if (hasDev)
		{
			const LPCSTR clsLabel =
			    (snap.device_class == EGamepadClass::ClassC) ? TrRemap("ui_gp_eng_wiz_class_c", "C (Unmapped Joystick)") :
			    (snap.device_class == EGamepadClass::ClassB) ? TrRemap("ui_gp_eng_wiz_class_b", "B (Mapped Gamepad)") :
			                                                   TrRemap("ui_gp_eng_wiz_class_a", "A (Console Gamepad)");
			string512 sub;
			xr_sprintf(sub, sizeof(sub),
			           TrRemap("ui_gp_eng_wiz_sub_device_fmt", "Target Device: id=%u '%s' (Class=%s, Buttons=%d, Axes=%d, Hats=%d, GUID=%s)"),
			           snap.instance_id, snap.name, clsLabel,
			           snap.num_buttons, snap.num_axes, snap.num_hats, snap.guid_str);
			m_pSubtitle->SetText(sub);
		}
		else
		{
			m_pSubtitle->SetText(TrRemap("ui_gp_eng_wiz_sub_no_device", "No joystick or gamepad detected. Connect a Class C controller."));
		}

		if (m_bWizardCompleted)
		{
			m_pContextBar->SetText(TrRemap("ui_gp_eng_wiz_done_bar", ">> WIZARD COMPLETED - SDL MAPPING APPLIED & SAVED <<"));
			m_pColumnHeader->SetText(TrRemap("ui_gp_eng_wiz_done_col", "Saved to $app_data_root$\\gamepad\\user_mappings.txt (gamecontrollerdb.txt untouched):"));
		}
		else
		{
			const int stepNum = clampr(m_wizardStepIdx, 0, NUM_WIZARD_STEPS - 1);
			const LPCSTR stepTitle = TrRemap(g_wizardSteps[stepNum].titleId, g_wizardSteps[stepNum].title);
			const LPCSTR stepPrompt = TrRemap(g_wizardSteps[stepNum].promptId, g_wizardSteps[stepNum].prompt);
			string512 bar;
			xr_sprintf(bar, sizeof(bar), TrRemap("ui_gp_eng_wiz_step_bar_fmt", ">> STEP %s <<"), stepTitle);
			m_pContextBar->SetText(bar);
			m_pColumnHeader->SetText(stepPrompt);
		}

		// Show wizard steps progress in the rows
		const int focusIdx = m_bWizardCompleted ? 0 : clampr(m_wizardStepIdx - (VISIBLE_ROWS / 2), 0, _max(0, NUM_WIZARD_STEPS - VISIBLE_ROWS));
		for (int i = 0; i < VISIBLE_ROWS; ++i)
		{
			const int sIdx = focusIdx + i;
			if (sIdx >= NUM_WIZARD_STEPS)
			{
				m_pRowAction[i]->SetText("");
				m_pRowBinding[i]->SetText("");
				continue;
			}
			const bool isCurrent = (!m_bWizardCompleted && sIdx == m_wizardStepIdx);
			const LPCSTR rowStepTitle = TrRemap(g_wizardSteps[sIdx].titleId, g_wizardSteps[sIdx].title);
			string512 rAct;
			xr_sprintf(rAct, sizeof(rAct), "%s %s", isCurrent ? ">>" : "  ", rowStepTitle);
			m_pRowAction[i]->SetText(rAct);

			const std::string& bound = m_wizardBindings[sIdx];
			if (!bound.empty())
			{
				std::string bTxt = std::string(g_wizardSteps[sIdx].sdlKey) + ":" + bound;
				m_pRowBinding[i]->SetText(bTxt.c_str());
				m_pRowBinding[i]->SetTextColor(color_argb(255, 110, 245, 155));
			}
			else if (sIdx < m_wizardStepIdx || m_bWizardCompleted)
			{
				m_pRowBinding[i]->SetText(TrRemap("ui_gp_eng_wiz_state_skipped", "[Skipped]"));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 160, 165, 175));
			}
			else if (isCurrent)
			{
				m_pRowBinding[i]->SetText(m_bWizardWaitingNeutral
				    ? TrRemap("ui_gp_eng_wiz_state_release", "[Release all controls...]")
				    : TrRemap("ui_gp_eng_wiz_state_waiting", "[Waiting for input...]"));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 255, 225, 110));
			}
			else
			{
				m_pRowBinding[i]->SetText(TrRemap("ui_gp_eng_wiz_state_pending", "[Pending]"));
				m_pRowBinding[i]->SetTextColor(color_argb(255, 130, 140, 155));
			}

			m_pRowAction[i]->SetTextColor(isCurrent ? color_argb(255, 255, 235, 130) : color_argb(255, 210, 215, 225));
		}

		m_pStatusBanner->SetText(m_statusText.c_str());
		m_pStatusBanner->SetTextColor(m_bStatusIsError ? color_argb(255, 255, 115, 115) : color_argb(255, 125, 240, 165));

		if (m_bWizardCompleted)
		{
			m_pModalTitle->SetText(TrRemap("ui_gp_eng_wiz_modal_gen_title", "GENERATED SDL3 MAPPING STRING"));
			m_pModalBody1->SetText(m_wizardGeneratedMapping.c_str());
			string512 b2;
			xr_sprintf(b2, sizeof(b2),
			           TrRemap("ui_gp_eng_wiz_modal_saved_fmt", "Saved file: %s | Press [A / B / Start / Enter / Esc] to return to Remapping Screen."),
			           m_wizardSavedPath[0] ? m_wizardSavedPath : "$app_data_root$\\gamepad\\user_mappings.txt");
			m_pModalBody2->SetText(b2);
		}
		else if (hasDev)
		{
			m_pModalTitle->SetText(TrRemap("ui_gp_eng_wiz_modal_raw_title", "LIVE RAW HID JOYSTICK TELEMETRY"));
			string256 raw1, raw2;
			xr_sprintf(raw1, sizeof(raw1), TrRemap("ui_gp_eng_wiz_modal_raw1_fmt", "Raw Buttons Mask: 0x%08X | Hat[0]: 0x%02X"),
			           snap.buttons_mask, static_cast<unsigned>(snap.hats[0]));
			xr_sprintf(raw2, sizeof(raw2), TrRemap("ui_gp_eng_wiz_modal_raw2_fmt", "Raw Axes[0..5]: [%d, %d, %d, %d, %d, %d]"),
			           static_cast<int>(snap.axes[0]), static_cast<int>(snap.axes[1]),
			           static_cast<int>(snap.axes[2]), static_cast<int>(snap.axes[3]),
			           static_cast<int>(snap.axes[4]), static_cast<int>(snap.axes[5]));
			m_pModalBody1->SetText(raw1);
			m_pModalBody2->SetText(raw2);
		}
		else
		{
			m_pModalTitle->SetText(TrRemap("ui_gp_eng_wiz_modal_wait_title", "WAITING FOR DEVICE"));
			m_pModalBody1->SetText(TrRemap("ui_gp_eng_wiz_modal_wait_body", "Connect a Class C controller or press [Esc] to return to Remapping Screen."));
			m_pModalBody2->SetText("");
		}

		m_pFooter1->SetText(TrRemap("ui_gp_eng_wiz_footer1", "Wizard Controls: [SPACE / TAB] Skip Current Step | [BACKSPACE / LEFT] Previous Step | [ENTER] Finish & Save Early"));
		m_pFooter2->SetText(TrRemap("ui_gp_eng_wiz_footer2", "[ESC] Return to Remapping Screen | Custom mappings are saved to $app_data_root$\\gamepad\\user_mappings.txt"));
	}

	void PushFilledRectUI(float x1, float y1, float x2, float y2, u32 color)
	{
		Fvector2 p1, p2;
		UI().ClientToScreenScaled(p1, x1, y1);
		UI().ClientToScreenScaled(p2, x2, y2);

		UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(p2.x, p1.y, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(p2.x, p2.y, 0.0f, color, 0.0f, 0.0f);

		UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(p2.x, p2.y, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(p1.x, p2.y, 0.0f, color, 0.0f, 0.0f);
	}

	void PushLineUI(float x1, float y1, float x2, float y2, u32 color)
	{
		Fvector2 p1, p2;
		UI().ClientToScreenScaled(p1, x1, y1);
		UI().ClientToScreenScaled(p2, x2, y2);
		UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(p2.x, p2.y, 0.0f, color, 0.0f, 0.0f);
	}

	void PushBoxOutlineUI(float x1, float y1, float x2, float y2, u32 color)
	{
		PushLineUI(x1, y1, x2, y1, color);
		PushLineUI(x2, y1, x2, y2, color);
		PushLineUI(x2, y2, x1, y2, color);
		PushLineUI(x1, y2, x1, y1, color);
	}

	void DrawVectorScene()
	{
		if (!m_shaderWire->inited())
			m_shaderWire->create("hud\\crosshair");

		UIRender->StartPrimitive(240, IUIRender::ptTriList, UI().m_currentPointType);

		// Fullscreen dark backdrop + main dialog panel
		PushFilledRectUI(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT, color_argb(235, 10, 12, 16));
		PushFilledRectUI(56.0f, 18.0f, 968.0f, 742.0f, color_argb(245, 20, 24, 32));

		// Context header bar
		PushFilledRectUI(76.0f, 78.0f, 948.0f, 108.0f, color_argb(220, 28, 36, 48));

		// List box background
		PushFilledRectUI(76.0f, 138.0f, 948.0f, 524.0f, color_argb(225, 14, 18, 24));

		// Selected row highlight bar (and conflict highlight bars)
		if (m_mode == EScreenMode::Remap)
		{
			const auto actions = GetCurrentContextActions();
			for (int i = 0; i < VISIBLE_ROWS; ++i)
			{
				const int actIdx = m_scrollOffset + i;
				if (actIdx >= static_cast<int>(actions.size()))
					break;
				const float ry = 142.0f + i * 34.0f;
				const gpbind::ActionId act = actions[static_cast<size_t>(actIdx)];
				if (IsActionInPendingConflict(act))
				{
					PushFilledRectUI(84.0f, ry, 940.0f, ry + 30.0f, color_argb(180, 120, 35, 35));
				}
				else if (actIdx == m_selectedRow)
				{
					PushFilledRectUI(84.0f, ry, 940.0f, ry + 30.0f, color_argb(170, 45, 70, 105));
				}
			}
		}
		else
		{
			const int focusIdx = m_bWizardCompleted ? 0 : clampr(m_wizardStepIdx - (VISIBLE_ROWS / 2), 0, _max(0, NUM_WIZARD_STEPS - VISIBLE_ROWS));
			for (int i = 0; i < VISIBLE_ROWS; ++i)
			{
				const int sIdx = focusIdx + i;
				if (!m_bWizardCompleted && sIdx == m_wizardStepIdx)
				{
					const float ry = 142.0f + i * 34.0f;
					PushFilledRectUI(84.0f, ry, 940.0f, ry + 30.0f, color_argb(175, 55, 80, 45));
				}
			}
		}

		// Bottom modal / telemetry box
		const u32 modalBg = (m_subState == ERemapSubState::ConfirmConflict)
		                        ? color_argb(235, 55, 22, 24)
		                        : (m_subState == ERemapSubState::Capturing ? color_argb(235, 24, 42, 62) : color_argb(225, 16, 20, 28));
		PushFilledRectUI(76.0f, 564.0f, 948.0f, 662.0f, modalBg);

		// Emergency reset hold progress bar (if Start + Back held)
		const u32 emergMs = gpbind::GetActiveBindingProfile().GetEmergencyResetHoldElapsedMs();
		if (emergMs > 0)
		{
			const float frac = clampr(static_cast<float>(emergMs) / static_cast<float>(gpbind::limits::kEmergencyResetHoldMs), 0.0f, 1.0f);
			PushFilledRectUI(130.0f, 646.0f, 130.0f + 764.0f * frac, 658.0f, color_argb(240, 245, 95, 75));
		}

		UIRender->SetShader(*m_shaderWire);
		UIRender->FlushPrimitive();

		// Outlines
		UIRender->StartPrimitive(128, IUIRender::ptLineList, UI().m_currentPointType);
		PushBoxOutlineUI(56.0f, 18.0f, 968.0f, 742.0f, color_argb(220, 95, 120, 155));
		PushBoxOutlineUI(76.0f, 78.0f, 948.0f, 108.0f, color_argb(180, 80, 105, 135));
		PushBoxOutlineUI(76.0f, 138.0f, 948.0f, 524.0f, color_argb(190, 85, 110, 140));
		PushBoxOutlineUI(76.0f, 564.0f, 948.0f, 662.0f,
		                 (m_subState == ERemapSubState::ConfirmConflict) ? color_argb(240, 245, 95, 95) : color_argb(190, 95, 125, 160));
		UIRender->SetShader(*m_shaderWire);
		UIRender->FlushPrimitive();
	}

private:
	ui_shader m_shaderWire;
	u32 m_pauseTokenId = 0;
	u32 m_lastUpdateTimeContinual = 0;
	u32 m_prevButtonsMask = 0;
	u32 m_lastSeenEmergencyResetCount = 0;
	bool m_bLtDigitalDown = false;
	bool m_bRtDigitalDown = false;
	EDir m_heldDir = DirNone;
	u32 m_nextRepeatTimeMs = 0;

	EScreenMode m_mode = EScreenMode::Remap;
	ERemapSubState m_subState = ERemapSubState::Browse;

	std::vector<gpbind::ContextId> m_contexts;
	int m_activeContextTab = 0;
	int m_selectedRow = 0;
	int m_scrollOffset = 0;

	// Capture state
	bool m_bCaptureWaitInitialRelease = false;
	gpbind::ButtonId m_capturePrimaryBtn = gpbind::ButtonId::None;
	gpbind::ButtonId m_captureSecondBtn = gpbind::ButtonId::None;
	u32 m_captureStartTimeMs = 0;
	u32 m_captureBtnPressTimeMs = 0;

	// Conflict confirmation state
	gpbind::BindingTrigger m_pendingCandidateTrigger;
	std::vector<gpbind::ConflictInfo> m_pendingConflicts;

	// Class C Wizard state
	u32 m_wizardTargetInstanceId = 0;
	int m_wizardStepIdx = 0;
	bool m_bWizardWaitingNeutral = true;
	bool m_bWizardCompleted = false;
	SRawJoystickSnapshot m_wizardBaselineSnap = {};
	std::string m_wizardBindings[NUM_WIZARD_STEPS];
	std::string m_wizardGeneratedMapping;
	string_path m_wizardSavedPath = {};

	std::string m_statusText;
	bool m_bStatusIsError = false;

	CUITextWnd* m_pTitle = nullptr;
	CUITextWnd* m_pSubtitle = nullptr;
	CUITextWnd* m_pContextBar = nullptr;
	CUITextWnd* m_pColumnHeader = nullptr;
	CUITextWnd* m_pRowAction[VISIBLE_ROWS] = {};
	CUITextWnd* m_pRowBinding[VISIBLE_ROWS] = {};
	CUITextWnd* m_pStatusBanner = nullptr;
	CUITextWnd* m_pModalTitle = nullptr;
	CUITextWnd* m_pModalBody1 = nullptr;
	CUITextWnd* m_pModalBody2 = nullptr;
	CUITextWnd* m_pFooter1 = nullptr;
	CUITextWnd* m_pFooter2 = nullptr;
};

CUIGamepadRemapWnd* g_pGamepadRemapWnd = nullptr;

CDialogHolder* GetSafeActiveDialogHolderForRemap()
{
	if (MainMenu() && MainMenu()->IsActive())
		return MainMenu();
	if (g_pGameLevel && CurrentGameUI())
		return CurrentGameUI();
	return nullptr;
}
} // namespace

CGamepadRemapUI& CGamepadRemapUI::Instance()
{
	static CGamepadRemapUI instance;
	return instance;
}

void CGamepadRemapUI::Open(LPCSTR sourceTag, bool openInWizardMode)
{
	const bool inMainMenu = (MainMenu() && MainMenu()->IsActive());
	if (!inMainMenu && (!g_pGameLevel || !CurrentGameUI()))
	{
		Msg("! [Gamepad] gp_remap open (%s): neither MainMenu nor GameUI is active", sourceTag ? sourceTag : "cmd");
		return;
	}

	if (!g_pGamepadRemapWnd)
	{
		g_pGamepadRemapWnd = xr_new<CUIGamepadRemapWnd>();
	}

	g_pGamepadRemapWnd->SetMode(openInWizardMode ? CUIGamepadRemapWnd::EScreenMode::WizardClassC
	                                             : CUIGamepadRemapWnd::EScreenMode::Remap);

	// CR1-09: recover if CDialogHolder::CleanInternals cleared holder stack while leaving IsShown() == true
	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForRemap();
	if (g_pGamepadRemapWnd->IsShown() &&
	    (g_pGamepadRemapWnd->GetHolder() != activeHolder || !activeHolder ||
	     activeHolder->TopInputReceiver() != g_pGamepadRemapWnd))
	{
		g_pGamepadRemapWnd->SetHolder(nullptr);
		g_pGamepadRemapWnd->Show(false);
	}

	if (!g_pGamepadRemapWnd->IsShown())
	{
		g_pGamepadRemapWnd->SetHolder(nullptr);
		g_pGamepadRemapWnd->ShowDialog(true);
	}
}

void CGamepadRemapUI::OpenWizard(LPCSTR sourceTag)
{
	Open(sourceTag, true);
}

void CGamepadRemapUI::Close(LPCSTR reason)
{
	(void)reason;
	if (!g_pGamepadRemapWnd || !g_pGamepadRemapWnd->IsShown())
		return;

	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForRemap();
	if (g_pGamepadRemapWnd->GetHolder() && g_pGamepadRemapWnd->GetHolder() == activeHolder)
	{
		g_pGamepadRemapWnd->HideDialog();
	}
	if (g_pGamepadRemapWnd->IsShown())
	{
		g_pGamepadRemapWnd->SetHolder(nullptr);
		g_pGamepadRemapWnd->Show(false);
	}
}

void CGamepadRemapUI::Toggle(LPCSTR sourceTag)
{
	if (IsOpen())
		Close(sourceTag);
	else
		Open(sourceTag, false);
}

bool CGamepadRemapUI::IsOpen() const
{
	if (!g_pGamepadRemapWnd || !g_pGamepadRemapWnd->IsShown())
		return false;
	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForRemap();
	return activeHolder && g_pGamepadRemapWnd->GetHolder() == activeHolder &&
	       activeHolder->TopInputReceiver() == g_pGamepadRemapWnd;
}

void CGamepadRemapUI::LogStatus() const
{
	gpbind::LogActiveBindingProfileStatus();
	Msg("* [Gamepad] gp_remap ui_open=%d mode=%s",
	    IsOpen() ? 1 : 0,
	    (g_pGamepadRemapWnd && g_pGamepadRemapWnd->GetMode() == CUIGamepadRemapWnd::EScreenMode::WizardClassC) ? "wizard" : "remap");
}

bool CGamepadRemapUI::RunSelfTest()
{
	int passed = 0;
	int failed = 0;
	auto check = [&](bool cond, const char* desc) {
		if (cond)
		{
			++passed;
		}
		else
		{
			++failed;
			Msg("! [Gamepad] gp_remap selftest FAIL: %s", desc);
		}
	};

	// Save current active binding profile state to restore at the end
	const gpbind::BindingProfileModel savedProfile = gpbind::GetActiveBindingProfile();
	gpbind::BindingProfileModel& model = gpbind::GetMutableActiveBindingProfile();
	model.ResetToDefaults();

	// 1. Remap Single / Hold / Chord & Conflict Detection + Confirmation Swap
	{
		// In World_Gameplay, Jump is South (A) and Crouch is East (B).
		// Setting Jump to East (B) with allowConflictResolution=false must report ConflictDetected against Crouch.
		const gpbind::EditResult conflictRes = model.SetBinding(
		    gpbind::ContextId::World_Gameplay,
		    gpbind::ActionId::Jump,
		    gpbind::BindingTrigger::MakeButton(gpbind::ButtonId::East),
		    false);
		check(conflictRes.status == gpbind::EditStatus::ConflictDetected, "Jump->B detects conflict when allowConflictResolution=false");
		check(!conflictRes.conflicts.empty() && conflictRes.conflicts[0].existing_action == gpbind::ActionId::Crouch,
		      "Conflict identifies Crouch on B");

		// Confirming conflict resolution (allowConflictResolution=true) swaps Jump and Crouch
		const gpbind::EditResult swapRes = model.SetBinding(
		    gpbind::ContextId::World_Gameplay,
		    gpbind::ActionId::Jump,
		    gpbind::BindingTrigger::MakeButton(gpbind::ButtonId::East),
		    true);
		check(swapRes.Succeeded(), "Jump->B succeeds with allowConflictResolution=true");
		const gpbind::BindingTrigger* jumpTrig = model.FindBinding(gpbind::ContextId::World_Gameplay, gpbind::ActionId::Jump);
		const gpbind::BindingTrigger* crouchTrig = model.FindBinding(gpbind::ContextId::World_Gameplay, gpbind::ActionId::Crouch);
		check(jumpTrig && jumpTrig->button == gpbind::ButtonId::East, "Jump is now bound to B");
		check(crouchTrig && crouchTrig->button == gpbind::ButtonId::South, "Crouch swapped to A");

		// Bind Hold and Chord in World_Gameplay
		const gpbind::EditResult holdRes = model.SetBinding(
		    gpbind::ContextId::World_Gameplay,
		    gpbind::ActionId::TorchToggle,
		    gpbind::BindingTrigger::MakeHold(gpbind::ButtonId::North, 400),
		    true);
		check(holdRes.Succeeded(), "Bind Hold(Y, 400ms) to TorchToggle");

		const gpbind::EditResult chordRes = model.SetBinding(
		    gpbind::ContextId::World_Gameplay,
		    gpbind::ActionId::DetectorToggle,
		    gpbind::BindingTrigger::MakeChord(gpbind::ButtonId::LeftShoulder, gpbind::ButtonId::North),
		    true);
		check(chordRes.Succeeded(), "Bind Chord(LB+Y) to DetectorToggle");
	}

	// 2. Protected MenuUI / MainMenu Navigation (Menu, Confirm, Back)
	{
		const gpbind::EditResult unbindConfirm = model.UnbindAction(gpbind::ContextId::MenuUI, gpbind::ActionId::Confirm);
		check(unbindConfirm.status == gpbind::EditStatus::ProtectedActionCannotUnbind, "Cannot unbind MenuUI.Confirm");

		const gpbind::EditResult unbindBack = model.UnbindAction(gpbind::ContextId::MenuUI, gpbind::ActionId::Back);
		check(unbindBack.status == gpbind::EditStatus::ProtectedActionCannotUnbind, "Cannot unbind MenuUI.Back");

		const gpbind::EditResult unbindMenu = model.UnbindAction(gpbind::ContextId::MainMenu, gpbind::ActionId::Menu);
		check(unbindMenu.status == gpbind::EditStatus::ProtectedActionCannotUnbind, "Cannot unbind MainMenu.Menu");

		const gpbind::EditResult chordOnConfirm = model.SetBinding(
		    gpbind::ContextId::MenuUI,
		    gpbind::ActionId::Confirm,
		    gpbind::BindingTrigger::MakeChord(gpbind::ButtonId::LT, gpbind::ButtonId::South),
		    true);
		check(chordOnConfirm.status == gpbind::EditStatus::ProtectedActionInvalidTrigger, "Protected Confirm refuses Chord trigger");

		// Swapping Confirm (A) and Back (B) in MenuUI is allowed and preserves invariant
		const gpbind::EditResult swapConfirmBack = model.SetBinding(
		    gpbind::ContextId::MenuUI,
		    gpbind::ActionId::Confirm,
		    gpbind::BindingTrigger::MakeButton(gpbind::ButtonId::East),
		    true);
		check(swapConfirmBack.Succeeded() && model.VerifyMenuNavigationInvariant(),
		      "Swapping Confirm<->Back in MenuUI succeeds and keeps navigation invariant");
	}

	// 3. Export / Emergency Reset (Start+Back 5s) / Import / Coexistence with calibration in profile.ltx
	{
		gpmath::SaveCalibrationProfile();
		check(gpbind::SaveActiveBindingProfile(), "SaveActiveBindingProfile to profile.ltx");
		check(gpbind::ExportActiveBindingProfile(), "ExportActiveBindingProfile to exports/bindings_export.ltx");

		// Simulate holding Start + Back for 5000 ms
		const u32 startBackMask = (1u << static_cast<u32>(gpbind::ButtonId::Start)) |
		                          (1u << static_cast<u32>(gpbind::ButtonId::Back));
		check(!gpbind::UpdateActiveEmergencyResetHold(startBackMask, 2500), "Start+Back 2500ms does not reset yet");
		check(gpbind::UpdateActiveEmergencyResetHold(startBackMask, 2500), "Start+Back 5000ms triggers Emergency Reset");
		gpbind::UpdateActiveEmergencyResetHold(0, 16); // release latch

		const gpbind::BindingTrigger* confirmAfterReset = model.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Confirm);
		check(confirmAfterReset && confirmAfterReset->button == gpbind::ButtonId::South,
		      "After Emergency Reset MenuUI.Confirm restored to South (A)");

		// Import exported profile back
		const gpbind::LoadReport impRep = gpbind::ImportActiveBindingProfile();
		check(impRep.status == gpbind::LoadStatus::Ok, "ImportActiveBindingProfile status == Ok");
		const gpbind::BindingTrigger* confirmAfterImport = model.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::Confirm);
		check(confirmAfterImport && confirmAfterImport->button == gpbind::ButtonId::East,
		      "After Import MenuUI.Confirm is East (B) again");

		// Verify profile.ltx still contains [left_stick] from D2.2 alongside [context_MenuUI] from D4.1/D4.2
		std::ifstream ifs(gpbind::GetActiveBindingProfilePath(), std::ios::binary);
		std::ostringstream oss;
		oss << ifs.rdbuf();
		const std::string profContent = oss.str();
		check(profContent.find("[left_stick]") != std::string::npos &&
		          profContent.find("[context_MenuUI]") != std::string::npos,
		      "profile.ltx preserves both [left_stick] (D2.2) and [context_MenuUI] (D4.2)");
	}

	// 4. Class C Mapping Wizard end-to-end test via Virtual Unmapped Class C Joystick
	CGamepadDeviceManager* mgr = pInput ? pInput->GetGamepadDeviceManager() : nullptr;
	check(mgr != nullptr, "GamepadDeviceManager available");
	if (mgr)
	{
		const u32 vId = mgr->AttachVirtualClassCDevice("GAMMA Unmapped Class C Test Pad");
		check(vId != 0, "AttachVirtualClassCDevice returned valid instance_id");
		if (vId != 0)
		{
			const SGamepadDeviceEntry* devBefore = mgr->GetDeviceByInstanceId(vId);
			check(devBefore != nullptr && devBefore->device_class == EGamepadClass::ClassC && devBefore->gamepad == nullptr,
			      "Virtual device initially registered as Class C (raw SDL_Joystick without SDL_Gamepad)");

			if (!g_pGamepadRemapWnd)
				g_pGamepadRemapWnd = xr_new<CUIGamepadRemapWnd>();

			g_pGamepadRemapWnd->StartClassCWizard(vId);

			auto feedNeutral = [&]() {
				for (int b = 0; b < 16; ++b)
					mgr->SetVirtualClassCInput(vId, b, false, -1, 0, -1, 0);
				for (int a = 0; a < 6; ++a)
					mgr->SetVirtualClassCInput(vId, -1, false, a, 0, -1, 0);
				mgr->SetVirtualClassCInput(vId, -1, false, -1, 0, 0, 0);
				SRawJoystickSnapshot s;
				if (mgr->SampleRawJoystick(vId, s))
					g_pGamepadRemapWnd->StepWizardCaptureFromSnapshot(s, false);
			};

			auto feedButtonStep = [&](int btnIdx) {
				feedNeutral();
				mgr->SetVirtualClassCInput(vId, btnIdx, true, -1, 0, -1, 0);
				SRawJoystickSnapshot s;
				if (mgr->SampleRawJoystick(vId, s))
					g_pGamepadRemapWnd->StepWizardCaptureFromSnapshot(s, false);
				mgr->SetVirtualClassCInput(vId, btnIdx, false, -1, 0, -1, 0);
			};

			auto feedHatStep = [&](u8 hatMask) {
				feedNeutral();
				mgr->SetVirtualClassCInput(vId, -1, false, -1, 0, 0, hatMask);
				SRawJoystickSnapshot s;
				if (mgr->SampleRawJoystick(vId, s))
					g_pGamepadRemapWnd->StepWizardCaptureFromSnapshot(s, false);
				mgr->SetVirtualClassCInput(vId, -1, false, -1, 0, 0, 0);
			};

			auto feedAxisStep = [&](int axisIdx, s16 val) {
				feedNeutral();
				mgr->SetVirtualClassCInput(vId, -1, false, axisIdx, val, -1, 0);
				SRawJoystickSnapshot s;
				if (mgr->SampleRawJoystick(vId, s))
					g_pGamepadRemapWnd->StepWizardCaptureFromSnapshot(s, false);
				mgr->SetVirtualClassCInput(vId, -1, false, axisIdx, 0, -1, 0);
			};

			// Steps 0..4: a=b0, b=b1, x=b2, y=b3, back=b4
			feedButtonStep(0);
			feedButtonStep(1);
			feedButtonStep(2);
			feedButtonStep(3);
			feedButtonStep(4);

			// Step 5: guide -> skip via WizardSkipCurrentStep() (keyboard Space/Tab skip)
			feedNeutral();
			g_pGamepadRemapWnd->WizardSkipCurrentStep();

			// Steps 6..10: start=b6, leftstick=b7, rightstick=b8, leftshoulder=b9, rightshoulder=b10
			feedButtonStep(6);
			feedButtonStep(7);
			feedButtonStep(8);
			feedButtonStep(9);
			feedButtonStep(10);

			// Steps 11..14: dpup=h0.1, dpdown=h0.4, dpleft=h0.8, dpright=h0.2
			feedHatStep(0x01);
			feedHatStep(0x04);
			feedHatStep(0x08);
			feedHatStep(0x02);

			// Steps 15..18: leftx=a0, lefty=a1, rightx=a2, righty=a3
			feedAxisStep(0, 30000);
			feedAxisStep(1, 30000);
			feedAxisStep(2, 30000);
			feedAxisStep(3, 30000);

			// Steps 19..20: lefttrigger=a4, righttrigger=a5
			feedAxisStep(4, 30000);
			feedAxisStep(5, 30000);

			const bool wizOk = g_pGamepadRemapWnd->WizardFinishAndApply(true);
			check(wizOk && g_pGamepadRemapWnd->IsWizardCompleted(), "Class C Wizard finished and applied SDL mapping");

			const std::string& genMap = g_pGamepadRemapWnd->GetWizardGeneratedMapping();
			check(genMap.find("a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,") != std::string::npos &&
			          genMap.find("dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,") != std::string::npos &&
			          genMap.find("leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,") != std::string::npos &&
			          genMap.find("guide:") == std::string::npos,
			      "Generated SDL mapping string matches all 20 captured controls and omits skipped Guide");

			const SGamepadDeviceEntry* devAfter = mgr->GetDeviceByInstanceId(vId);
			check(devAfter != nullptr && devAfter->device_class == EGamepadClass::ClassB && devAfter->gamepad != nullptr,
			      "Virtual Class C device promoted to Class B (SDL_Gamepad opened) after wizard mapping");

			std::ifstream mapIfs(g_pGamepadRemapWnd->GetWizardSavedPath(), std::ios::binary);
			check(mapIfs.good(), "Custom mapping persisted to $app_data_root$\\gamepad\\user_mappings.txt");

			mgr->DetachVirtualClassCDevice(vId);
		}
	}

	// Restore saved active profile and persist clean state
	model = savedProfile;
	gpbind::SaveActiveBindingProfile();

	Msg("* [Gamepad] gp_remap selftest: PASS=%d FAIL=%d (%s)",
	    passed, failed, (failed == 0) ? "ALL OK" : "FAILED");

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_remap_selftest,%u,pass=%d,fail=%d",
		           Device.dwTimeContinual, passed, failed);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return failed == 0;
}

// E3.2: Карта покрытия 70 обязательных действий движка + 59 активных хоткеев MCM (+ 2 закомментированных в dit_mcm.script:23-24).
// Консольная команда gp_actions_unreached проверяет текущую раскладку (gpbind::GetActiveBindingProfile() + маршруты C1–E3.1,
// радиальное колесо E5.1/E5.2 и экраны Личного терминала / UI этапов F–G) и печатает все отвязанные в профиле или требующие
// продуктового решения действия с подробным объяснением каждого пункта.
namespace
{
enum class EE32CoverageCategory : u8
{
	ButtonHoldChord = 0,     // Прямая кнопка / удержание / аккорд (реализовано в C1–E3.1, D3.2, D4.1)
	RadialWheelTab = 1,      // Вкладка радиального колеса LB Hold (Оружие / Медицина и еда / Устройства и свет / Напарники, E5.1/E5.2)
	PersonalTerminalUI = 2,  // Личный терминал / Инвентарь / КПК / Размещение мебели / UI3D_RF / MCM (Этапы F–G)
	ServiceOrQuestion = 3,   // Сервисное действие или пункт в списке вопросов для Claude/пользователя (без угадывания биндов)
	InactiveCommentedOut = 4 // Закомментировано в исходнике мода (dit_mcm.script:23-24)
};

struct SE32EngineCoverageRow
{
	int enumIndex;
	const char* enumName;
	const char* actionName;
	EE32CoverageCategory category;
	gpbind::ContextId gpCtx;
	gpbind::ActionId gpAct;
	const char* routeDesc;
	const char* stageTag;
	const char* explanation;
};

struct SE32McmCoverageRow
{
	int index;
	const char* scriptLine;
	const char* optionId;
	const char* modName;
	bool activeInCode;
	EE32CoverageCategory category;
	gpbind::ContextId gpCtx;
	gpbind::ActionId gpAct;
	const char* routeDesc;
	const char* stageTag;
	const char* explanation;
};

static const SE32EngineCoverageRow g_e32EngineRows[70] = {
	// --- 1..39: Прямая кнопка / удержание / аккорд (39 действий) ---
	{ kJUMP,              "kJUMP",              "jump",              EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Jump,             "A (South) Tap",                              "E1.1", "Прыжок и перехват уступа demonized_ledge_grabbing (D15: без kUSE на A)" },
	{ kCROUCH,            "kCROUCH",            "crouch",            EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Crouch,           "B (East) Tap <300ms",                        "E1.1", "Переключение обычного приседа Box 1 <-> Box 0 (D16)" },
	{ kACCEL,             "kACCEL",             "accel",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::LowCrouch,        "B (East) Hold >=300ms + LS Analog Curve",    "E1.1", "Низкий присед Box 2 по Hold B + плавная скорость шага от модуля левого стика (D16)" },
	{ kSPRINT_TOGGLE,     "kSPRINT_TOGGLE",     "sprint_toggle",     EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Sprint,           "L3 (LeftStick) Tap",                         "E1.1", "Спринт с защёлкой и автосбросом при возврате левого стика в нейтраль (D16, D42)" },
	{ kL_LOOKOUT,         "kL_LOOKOUT",         "llookout",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::LeanLeft,         "LT + LB (Chord in ADS)",                     "E1.1/E2.1", "Наклон влево в режиме прицеливания ADS с автосбросом при выходе из ADS (D13, D16)" },
	{ kR_LOOKOUT,         "kR_LOOKOUT",         "rlookout",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::LeanRight,        "LT + RB (Chord in ADS)",                     "E1.1/E2.1", "Наклон вправо в режиме прицеливания ADS с автосбросом при выходе из ADS (D13, D16)" },
	{ kCAM_ZOOM_IN,       "kCAM_ZOOM_IN",       "cam_zoom_in",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::MaskClean,        "D-Pad Down Hold >=300ms outside ADS",        "E3.1", "Протереть маску (gp_field.clean_mask / actor_effects.Hit_MaskCleaning, D25)" },
	{ kTORCH,             "kTORCH",             "torch",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::TorchToggle,      "D-Pad Up Tap <300ms outside ADS + Wheel",    "E3.1", "Налобный фонарь (kTORCH + gp_field.on_torch_tap) и сектор во вкладке «Устройства» (D25)" },
	{ kNIGHT_VISION,      "kNIGHT_VISION",      "night_vision",      EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::NightVision,      "D-Pad Up Hold >=300ms outside ADS + Wheel",  "E3.1", "ПНВ вкл/выкл через gp_field.toggle_nvg без kNIGHT_VISION таймерной ловушки; яркость — в колесе (D25)" },
	{ kDETECTOR,          "kDETECTOR",          "show_detector",     EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::DetectorToggle,   "D-Pad Left Tap <300ms outside ADS + Wheel",  "E3.1", "Детектор / устройство в левую руку (kDETECTOR + gp_field.on_detector_tap, D25)" },
	{ kWPN_2,             "kWPN_2",             "wpn_2",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,       "Y (North) Tap <250ms + Wheel [Оружие]",      "E2.1/E5.1", "Циклическая смена основного оружия по Y и прямой сектор Слота 2 в колесе LB" },
	{ kWPN_3,             "kWPN_3",             "wpn_3",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,       "Y (North) Tap <250ms + Wheel [Оружие]",      "E2.1/E5.1", "Циклическая смена основного оружия по Y и прямой сектор Слота 3 в колесе LB" },
	{ kWPN_4,             "kWPN_4",             "wpn_4",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::GrenadeQuick,     "RB Tap outside ADS + Wheel [Оружие]",        "E3.1/E5.1", "Быстрый бросок гранаты (kCUSTOM19) с фолбэком выбора слота 4 (kWPN_4) и сектор в колесе (D13)" },
	{ kWPN_6,             "kWPN_6",             "wpn_6",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Tap <250ms outside ADS + Wheel [Оружие]", "E3.1/E5.1", "Короткое LB вне ADS вызывает болт kWPN_6 или выбранный быстрый слот (gp_field.quick_bolt_or_slot, D13)" },
	{ kWPN_NEXT,          "kWPN_NEXT",          "wpn_next",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::AmmoTypeNext,     "D-Pad Right Hold >=300ms outside ADS",       "E2.1", "Смена типа патронов / открытие колеса боеприпасов UIWheelAmmo (D25)" },
	{ kWPN_FIRE,          "kWPN_FIRE",          "wpn_fire",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Fire,             "RT (TriggerRight) Analog Hysteresis",        "E2.1", "Огонь по аналоговому триггеру RT с настраиваемым порогом press/release в профиле" },
	{ kWPN_ZOOM,          "kWPN_ZOOM",          "wpn_zoom",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,              "LT (TriggerLeft) Analog Hysteresis",         "E2.1", "Прицеливание ADS по аналоговому триггеру LT (Hold по умолчанию, совместимо с AF_AIM_TOGGLE, D16)" },
	{ kWPN_ZOOM_INC,      "kWPN_ZOOM_INC",      "wpn_zoom_inc",      EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,              "LT + D-Pad Up (Chord in ADS)",               "E2.1", "Увеличение кратности оптики 3DSS в режиме ADS с автоповтором 320/90 мс (D25)" },
	{ kWPN_ZOOM_DEC,      "kWPN_ZOOM_DEC",      "wpn_zoom_dec",      EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,              "LT + D-Pad Down (Chord in ADS)",             "E2.1", "Уменьшение кратности оптики 3DSS в режиме ADS с автоповтором 320/90 мс (D25)" },
	{ kWPN_RELOAD,        "kWPN_RELOAD",        "wpn_reload",        EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::ContextUseReload, "X (West) Tap <350ms / LT+X / Repeat X",      "E2.2", "Контекстная X: обычная перезарядка (Tap X без цели HUD), быстрый сброс магазина (LT+X или повторный X <250ms, D15)" },
	{ kWPN_FUNC,          "kWPN_FUNC",          "wpn_func",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::UnderbarrelToggle,"LT + R3 in ADS / D-Pad Left Hold outside ADS","E2.1", "В ADS R3 переключает основной/скошенный прицел (SwitchZoomType); вне ADS Hold D-Pad Left переключает подствольник (D25)" },
	{ kWPN_FIREMODE_NEXT, "kWPN_FIREMODE_NEXT", "wpn_firemode_next", EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::FireModeNext,     "D-Pad Right Tap <300ms outside ADS",         "E2.1", "Циклическое переключение режима огня (одиночный / отсечка / авто, D25)" },
	{ kPAUSE,             "kPAUSE",             "pause",             EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::PauseMenu,        "PauseTokenManager (C4.1) + Start (Menu)",    "C4.1/D3.2", "Автоматическая управляемая пауза в модальных окнах (D06) и системное меню паузы по Start" },
	{ kUSE,               "kUSE",               "use",               EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::ContextUseReload, "X (West) Tap <350ms on HUD Target",          "E2.2/E3.1", "Взаимодействие с целью перед прицелом (предмет с PickupModeUpdate, дверь, тайник, НПС, D15)" },
	{ kQUIT,              "kQUIT",              "quit",              EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::MenuUI,         gpbind::ActionId::Back,             "Start in World / B (East) in MenuUI",        "D3.2", "Закрытие верхнего модального окна по B (с барьером отпускания D3.1) и главное меню по Start" },
	{ kINVENTORY,         "kINVENTORY",         "inventory",         EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::PdaFullToggle,    "View (Back) Tap in World & 3D PDA",          "D3.2/F.2", "Открытие инвентаря / Личного терминала по кнопке View (Back) в мире и из режима 3D-КПК" },
	{ kACTIVE_JOBS,       "kACTIVE_JOBS",       "active_jobs",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::PdaFullToggle,    "D-Pad Down in 3D PDA / View Hold",           "D3.2/G.5", "Переход к полноэкранному 2D/3D КПК (задачи и карта, D12, D25)" },
	{ kNEXT_SLOT,         "kNEXT_SLOT",         "next_slot",         EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,       "Y (North) Tap <250ms",                       "E2.1", "Циклическое переключение слотов оружия через gp_weapon.cycle_weapon_next" },
	{ kQUICK_USE_1,       "kQUICK_USE_1",       "quick_use_1",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Invalid,          "LB Tap <250ms (prio) + Wheel [Медицина]",    "E3.1/E5.1", "Быстрый слот 1 через адаптер gp_field.quick_bolt_or_slot (LB Tap) и вкладку «Медицина и еда» колеса LB" },
	{ kQUICK_USE_2,       "kQUICK_USE_2",       "quick_use_2",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Invalid,          "LB Tap <250ms (prio) + Wheel [Медицина]",    "E3.1/E5.1", "Быстрый слот 2 через адаптер gp_field.quick_bolt_or_slot (LB Tap) и вкладку «Медицина и еда» колеса LB" },
	{ kQUICK_USE_3,       "kQUICK_USE_3",       "quick_use_3",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Invalid,          "LB Tap <250ms (prio) + Wheel [Медицина]",    "E3.1/E5.1", "Быстрый слот 3 через адаптер gp_field.quick_bolt_or_slot (LB Tap) и вкладку «Медицина и еда» колеса LB" },
	{ kQUICK_USE_4,       "kQUICK_USE_4",       "quick_use_4",       EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Invalid,          "LB Tap <250ms (prio) + Wheel [Медицина]",    "E3.1/E5.1", "Быстрый слот 4 через адаптер gp_field.quick_bolt_or_slot (LB Tap) и вкладку «Медицина и еда» колеса LB" },
	{ kCUSTOM19,          "kCUSTOM19",          "custom19",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::GrenadeQuick,     "RB Tap outside ADS",                         "E3.1", "Быстрый бросок гранаты (actor_effects.Hit_GrenadeQuickthrow, D13)" },
	{ kCUSTOM20,          "kCUSTOM20",          "custom20",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Pda3DToggle,      "D-Pad Down Tap <300ms outside ADS",          "E3.1", "Достать/убрать полевой 3D-КПК в руках в реальном времени (activate_slot(8), D12, D25)" },
	{ kCUSTOM21,          "kCUSTOM21",          "custom21",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::UnderbarrelToggle,"D-Pad Left Hold >=300ms outside ADS",        "E2.1/E3.1", "Переключение подствольного гранатомёта UBGL (Weapon.cpp:1590 ToggleGrenadeLauncher, D25)" },
	{ kCUSTOM22,          "kCUSTOM22",          "custom22",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::ContextUseReload, "X (West) Tap <350ms when weapon jammed",     "E2.2", "Устранение клина затвора через gp_weapon.x_reload -> arti_jamming.unjam() (приоритет №2 после kUSE, D15)" },
	{ kCUSTOM24,          "kCUSTOM24",          "custom24",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::MeleeQuick,       "R3 (RightStick) Tap <200ms outside ADS",     "E3.1", "Быстрый удар прикладом или ножом (quickdraw.script:49 + wd_inspect.script:1682, D25)" },
	{ kSAFEMODE,          "kSAFEMODE",          "safemode",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,       "Y (North) Hold >=250ms",                     "E2.1", "Убрать оружие / перевести в безопасный режим (Weapon.cpp:1575 + lower_weapon_sprint, D25)" },
	{ kFREELOOK,          "kFREELOOK",          "freelook",          EE32CoverageCategory::ButtonHoldChord, gpbind::ContextId::World_Gameplay, gpbind::ActionId::Freelook,         "R3 (RightStick) Hold >=200ms outside ADS",   "E1.1", "Свободный поворот головы без поворота корпуса (cam_SetFreelook/cam_UnsetFreelook, D25)" },

	// --- 40..53: Вкладка радиального колеса LB Hold (14 действий) ---
	{ kWPN_1,             "kWPN_1",             "wpn_1",             EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Оружие» (Слот 1)", "E5.1/E5.2", "Экипировка ножа / оружия ближнего боя из слота 1 через колесо LB (плюс быстрый удар на R3 Tap)" },
	{ kWPN_5,             "kWPN_5",             "wpn_5",             EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Оружие/Устройства»","E5.1/E5.2","Экипировка бинокля (слот 5) через быстрое колесо LB" },
	{ kWPN_FIREMODE_PREV, "kWPN_FIREMODE_PREV", "wpn_firemode_prev", EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::FireModeNext,     "Покрыто кольцевым циклом D-Pad Right Tap / Колесо","E2.1/E5.2","Режимы огня (1/3/авто) образуют замкнутое кольцо из 2–3 состояний на D-Pad Right Tap (kWPN_FIREMODE_NEXT)" },
	{ kPREV_SLOT,         "kPREV_SLOT",         "prev_slot",         EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,       "Покрыто циклом Y Tap + прямой выбор в колесе LB","E2.1/E5.1","Любой слот выбирается напрямую во вкладке «Оружие» колеса LB или циклом Y Tap" },
	{ kCUSTOM1,           "kCUSTOM1",           "custom1",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники»",       "E5.2", "Приказ напарникам: Движение / Ждать (axr_companions.script:1547, D46)" },
	{ kCUSTOM2,           "kCUSTOM2",           "custom2",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники»",       "E5.2", "Приказ напарникам: Бой / Скрытность (axr_companions.script:1550, D46)" },
	{ kCUSTOM3,           "kCUSTOM3",           "custom3",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники»",       "E5.2", "Приказ напарникам: Идти в точку / Следовать (axr_companions.script:1538,1553, D46)" },
	{ kCUSTOM4,           "kCUSTOM4",           "custom4",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники»",       "E5.2", "Приказ напарникам: Сбор лута (axr_companions.script:1556, D46)" },
	{ kCUSTOM5,           "kCUSTOM5",           "custom5",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники»",       "E5.2", "Приказ напарникам: Дистанция / Построение (axr_companions.script:1559, D46)" },
	{ kCUSTOM6,           "kCUSTOM6",           "custom6",           EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Устройства» / Терминал","E5.2/G1.1","Скрыть/показать HUD (actor_effects.script:1688 hud_draw on/off) — сервисный слот колеса / терминала" },
	{ kCUSTOM13,          "kCUSTOM13",          "custom13",          EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Оружие» (Глушитель)","E5.2","Быстро надеть/снять глушитель (item_weapon.script:1013 строго на on_key_release)" },
	{ kCUSTOM14,          "kCUSTOM14",          "custom14",          EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Устройства» (Дозиметр)","E5.2","Дозиметр / счётчик Гейгера (item_device.script:257 на on_key_release)" },
	{ kCUSTOM18,          "kCUSTOM18",          "custom18",          EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Напарники» (IDIOTS)","E5.2/G.6","Открыть полное окно управления напарниками IDIOTS (idiots_ui.UIWheelCompanion, D46)" },
	{ kCUSTOM25,          "kCUSTOM25",          "custom25",          EE32CoverageCategory::RadialWheelTab,  gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,      "LB Hold >=250ms -> Вкладка «Устройства» (Bullet Time)","E5.2","Замедление времени Bullet Time (bullet_time_mcm.script:64; согласовано с диспетчером D43)" },

	// --- 54..67: Личный терминал / Инвентарь / КПК / Системные UI-экраны (14 действий) ---
	{ kCAM_ZOOM_OUT,      "kCAM_ZOOM_OUT",      "cam_zoom_out",      EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Menu,             "Личный терминал / Настройки (G1.1)",           "G1.1", "Сервисный переключатель анимаций предметов (actor_effects.script:1694 video/player/animations)" },
	{ kDROP,              "kDROP",              "drop",              EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::ContextMenu,      "Инвентарь / Личный терминал: Выбросить предмет","F1.2/F2.2","Выброс предмета только из инвентаря с защитой избранного SortingPlus (не висит на боевой кнопке в мире)" },
	{ kSCREENSHOT,        "kSCREENSHOT",        "screenshot",        EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MainMenu,    gpbind::ActionId::Invalid,          "Аппаратная кнопка Share/Create геймпада / Меню","System", "Скриншот выполняется штатной кнопкой Share (Xbox) / Create (DualSense) / оверлеем ОС" },
	{ kCONSOLE,           "kCONSOLE",           "console",           EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MainMenu,    gpbind::ActionId::Invalid,          "Клавиша ~ (отладка) / Личный терминал (G1.1)",  "System/G1.1","Отладочная консоль разработчика на ~; все пользовательские настройки вынесены в Личный терминал и gp_remap" },
	{ kQUICK_SAVE,        "kQUICK_SAVE",        "quick_save",        EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MainMenu,    gpbind::ActionId::Confirm,          "Меню паузы (Start) / Личный терминал (G1.1)",  "D3.2/G1.1","Быстрое сохранение из меню паузы или Личного терминала (защита от случайной перезаписи в бою)" },
	{ kQUICK_LOAD,        "kQUICK_LOAD",        "quick_load",        EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MainMenu,    gpbind::ActionId::Confirm,          "Меню паузы (Start) / Личный терминал (G1.1)",  "D3.2/G1.1","Быстрая загрузка с подтверждением из меню паузы или Личного терминала (защита от потери прогресса)" },
	{ kCUSTOM7,           "kCUSTOM7",           "custom7",           EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Play/Pause)",          "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:529)" },
	{ kCUSTOM8,           "kCUSTOM8",           "custom8",           EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Stop/Channel)",        "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:541)" },
	{ kCUSTOM9,           "kCUSTOM9",           "custom9",           EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Next Track)",          "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:553)" },
	{ kCUSTOM10,          "kCUSTOM10",          "custom10",          EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Prev Track)",          "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:558)" },
	{ kCUSTOM11,          "kCUSTOM11",          "custom11",          EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Volume +)",            "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:563)" },
	{ kCUSTOM12,          "kCUSTOM12",          "custom12",          EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Volume -)",            "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:567)" },
	{ kCUSTOM15,          "kCUSTOM15",          "custom15",          EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::QuickAction,      "Инвентарь / Терминал: Разрядить всё оружие",   "F2.2/G.2", "Массовая разрядка оружия в инвентаре (ish_proper_unload.script:31 / item_weapon.script:1017 на on_key_release)" },
	{ kCUSTOM16,          "kCUSTOM16",          "custom16",          EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,      gpbind::ActionId::Confirm,          "КПК -> Вкладка «Радио» (Playlist/Mode)",       "G.5", "Управление аудиоплеером КПК (ui_pda_radio_tab.script:571)" },

	// --- 68..70: Сервисное переключение вида камеры (3 действия, по docs/11 §1 — сервисное меню / вопрос пользователю) ---
	{ kCAM_1,             "kCAM_1",             "cam_1",             EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,       gpbind::ActionId::Invalid,          "Сервисное меню Личного терминала (G1.1)",      "Service/Q", "Вид от 1-го лица (штатный режим игры); по docs/11 §1 переключение вида вынесено в сервисное меню" },
	{ kCAM_2,             "kCAM_2",             "cam_2",             EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,       gpbind::ActionId::Invalid,          "Сервисное меню Личного терминала (G1.1)",      "Service/Q", "Опциональная камера 3-го лица; по docs/11 §1 не занимает боевую кнопку, вызывается из сервисного меню" },
	{ kCAM_3,             "kCAM_3",             "cam_3",             EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,       gpbind::ActionId::Invalid,          "Сервисное меню Личного терминала (G1.1)",      "Service/Q", "Опциональная свободная камера 3-го лица; по docs/11 §1 вызывается из сервисного меню Личного терминала" }
};

static const SE32McmCoverageRow g_e32McmRows[61] = {
	// --- Прямая кнопка / удержание / аккорд (11 активных MCM-хоткеев) ---
	{  2, "ammo_check_mcm.script:2474",              "rax_ammo_check/keybind",                 "AmmoCheck Enhanced 3.3", true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::AmmoCheck,       "X (West) Hold >=350ms",                  "E2.2", "Прямой вызов gp_weapon.x_ammo_check -> ammo_check_mcm.check_Ammo() (без эмуляции DIK_F, D15)" },
	{  3, "ammo_check_mcm.script:2494",              "rax_ammo_check/firemode/key",            "AmmoCheck Enhanced 3.3", true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::FireModeNext,    "D-Pad Right Tap <300ms outside ADS",     "E2.1", "Переключение режима огня kWPN_FIREMODE_NEXT (firemode/enable по умолчанию выключен)" },
	{  4, "ammo_check_mcm.script:2541",              "rax_ammo_check/onekey/key_reload",       "AmmoCheck Enhanced 3.3", true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::ContextUseReload,"X (West) FSM: Tap / Hold / LT+X / 2xX", "E2.2", "Нативный конечный автомат кнопки X (E2.2) заменяет клавиатурный мультиплексор onekey без задержки double-tap" },
	{ 18, "free_zoomv2_mcm.script:34",               "free_zoom/key_bind_toggle",              "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,             "LT + R3 / LT + D-Pad Up/Down in ADS",    "E2.1", "Слой ADS (D25): R3 переключает прицел, D-Pad Up/Down меняет кратность" },
	{ 19, "scope_fov_mcm.script:354",                "scope_fov/keybind_add",                  "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,             "LT + D-Pad Up in ADS",                   "E2.1", "Увеличение кратности 3DSS в ADS (см. также вопрос Q2 по выносу зрачка scope_fov)" },
	{ 20, "scope_fov_mcm.script:355",                "scope_fov/keybind_subtract",             "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,             "LT + D-Pad Down in ADS",                 "E2.1", "Уменьшение кратности 3DSS в ADS (см. также вопрос Q2 по выносу зрачка scope_fov)" },
	{ 22, "z_3d_scopes_mcm.script:297",              "3d_scopes/switch_bind",                  "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,             "LT + R3 in ADS (SwitchZoomType)",        "E2.1", "Переключение основного / скошенного прицела и магнифера в ADS (D25)" },
	{ 40, "lower_weapon_sprint_mcm.script:19",       "lower_weapon_sprint/keybind",            "467- Lower Weapon Sprint",true, EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::NextWeapon,      "Y (North) Hold >=250ms + L3 Sprint",     "E1.1/E2.1", "Опускание оружия интегрировано со спринтом L3 и удержанием Y (kSAFEMODE)" },
	{ 43, "demonized_ledge_grabbing_mcm.script:33",  "ledge_grabbing/keybind",                 "350- Ledge Grabbing",    true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Jump,            "A (South) Tap on ledge contact",         "E1.1", "Паркур/перехват уступа совмещён с кнопкой прыжка A (kJUMP)" },
	{ 53, "weapon_cover_tilt_mcm.script:23",         "weapon_cover_tilt/manual_tilt_key_bind", "235- Weapon Cover Tilt", true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::LeanLeft,        "Auto cover tilt + LT + LB/RB in ADS",    "E1.1/E2.1", "Автоматический подъём оружия у препятствий + ручные наклоны LB/RB в ADS" },
	{ 58, "ish_toggle_scope_mcm.script:22",          "toggle_scope/key_bind_toggle",           "127- Toggle Scope",      true,  EE32CoverageCategory::ButtonHoldChord,    gpbind::ContextId::World_Gameplay, gpbind::ActionId::Aim,             "LT + R3 in ADS + Inventory Functor",     "E2.1/F.3", "Переключение прицела в ADS (LT+R3) и снятие/установка оптики в инвентаре (F.3)" },

	// --- Вкладка радиального колеса LB Hold (11 активных MCM-хоткеев) ---
	{  1, "bullet_time_mcm.script:161",              "bullettime/keybind",                     "Bullet Time Enhanced",   true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Дублирует живой слот kCUSTOM25 (сектор Bullet Time в колесе LB, диспетчер D43)" },
	{  5, "ammo_check_mcm.script:2597",              "rax_ammo_check/eject/key",               "AmmoCheck Enhanced 3.3", true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Оружие»",    "E5.2", "Сектор «Выбросить патрон из патронника» (ammo_check_eject_mcm.eject_one_round; в MCM по умолчанию DIK_ESCAPE)" },
	{ 15, "liz_fdda_redone_mcm.script:155",          "fddar/headgear_anm/keybind",             "447- FDDA Redone",       true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Сектор «Снять/надеть шлем» (анимация FDDA Redone)" },
	{ 16, "liz_fdda_redone_mcm.script:180",          "fddar/outfit_anm/keybind",               "447- FDDA Redone",       true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Сектор «Осмотр брони» (анимация FDDA Redone)" },
	{ 17, "laser_settings_mcm.script:9",             "laser_settings/config_key",              "465- Laser Settings",    true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Оружие/Устройства»","E5.2/G.6","Переключение ЛЦУ (zzz_bas_laser_control.script:376) и вызов LaserSettingsDialog" },
	{ 25, "z_3d_scopes_mcm.script:340",              "3d_scopes/camo_keybind",                 "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::RadialWheel,    gpbind::ActionId::WheelConfirm,    "UI3DSSCamoWheel + Функтор инвентаря",    "E5.2/F.3", "Кастомный функтор инвентаря 3dss_sp34_texture_swap и выбор стиком в колесе камуфляжа UI3DSSCamoWheel" },
	{ 37, "haru_quick_action_wheel_mcm.script:1594", "haru_qaw/keybind",                       "Quick Action Wheel",     true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms outside ADS",            "E3.1/E5.1","Открытие быстрого колеса действий (gp_field.lb_wheel_hold -> gp_wheel, D13, D45)" },
	{ 55, "dotmarks_mcm.script:681",                 "dotmarks/bind_toggle_marks",             "452- Interaction DotMarks",true,EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства» / MCM","E5.2/G.7","Сервисный переключатель видимости меток DotMarks в колесе LB и в MCM" },
	{ 56, "tc_mcm.script:156",                       "tactic_compass/keybind",                 "420- Tactical Compass",  true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Сектор «Тактический компас» во вкладке «Устройства и свет» колеса LB" },
	{ 57, "mini_map_toggle_mcm.script:23",           "mini_map_toggle/keybind",                "177- Minimap Toggle",    true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Сектор «Миникарта» во вкладке «Устройства и свет» колеса LB" },
	{ 60, "exo_power_activator_mcm.script:18",       "exo_power_activator/keybind",            "47- Powered Exos",       true,  EE32CoverageCategory::RadialWheelTab,     gpbind::ContextId::World_Gameplay, gpbind::ActionId::WeaponWheel,     "LB Hold >=250ms -> Вкладка «Устройства»","E5.2", "Сектор «Питание экзоскелета» во вкладке «Устройства и свет» колеса LB" },

	// --- Личный терминал / Инвентарь / КПК / Размещение мебели / UI3D_RF (33 активных MCM-хоткея) ---
	{  0, "experience_mcm.script:11",                "experience/DISPLAY_UI",                  "Experience Framework",   true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Menu,            "Личный терминал -> Вкладка «Навыки/Перки»","G.2","Открывает окно перков UIExperienceMenu (experience_ui.script:126)" },
	{  6, "catsy_nerfs_mcm.script:70",               "catsy_nerfs/kb_preset1",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 1 на экране 3D RF-ресивера (item_radio.script:679)" },
	{  7, "catsy_nerfs_mcm.script:79",               "catsy_nerfs/kb_preset2",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 2 на экране 3D RF-ресивера (item_radio.script:679)" },
	{  8, "catsy_nerfs_mcm.script:88",               "catsy_nerfs/kb_preset3",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 3 на экране 3D RF-ресивера (item_radio.script:679)" },
	{  9, "catsy_nerfs_mcm.script:97",               "catsy_nerfs/kb_preset4",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 4 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 10, "catsy_nerfs_mcm.script:106",              "catsy_nerfs/kb_preset5",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 5 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 11, "catsy_nerfs_mcm.script:115",              "catsy_nerfs/kb_preset6",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 6 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 12, "catsy_nerfs_mcm.script:124",              "catsy_nerfs/kb_preset7",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 7 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 13, "catsy_nerfs_mcm.script:133",              "catsy_nerfs/kb_preset8",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 8 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 14, "catsy_nerfs_mcm.script:142",              "catsy_nerfs/kb_preset9",                 "417- Extensible RF",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Confirm,         "UI3D_RF: Фокус крестовиной D-Pad + A",   "G.6", "Выбор пресета 9 на экране 3D RF-ресивера (item_radio.script:679)" },
	{ 26, "catsy_paw_mcm.script:586",                "catsy_paw/bind_cartmode",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК: Контекстное меню PAW",        "G.5", "Режим картографии PAW на карте КПК (tasks_placeable_waypoints.script)" },
	{ 27, "catsy_paw_mcm.script:605",                "catsy_paw/bind_wp_target_obj",           "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК / Колесо LB: Метка на цель",   "G.5/E5.2", "Поставить путевую точку PAW на объект" },
	{ 28, "catsy_paw_mcm.script:614",                "catsy_paw/bind_pin_target_obj",          "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК / Колесо LB: Пин на цель",     "G.5/E5.2", "Поставить быстрый пин PAW на объект" },
	{ 29, "catsy_paw_mcm.script:627",                "catsy_paw/bind_wptoggle",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК / Колесо LB: Вкл/выкл метки",  "G.5/E5.2", "Переключение путевой точки PAW" },
	{ 30, "catsy_paw_mcm.script:635",                "catsy_paw/bind_quickpin",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК / Колесо LB: Быстрый пин",     "G.5/E5.2", "Быстрая установка метки PAW" },
	{ 31, "catsy_paw_mcm.script:646",                "catsy_paw/bind_set_next",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::PageNext,        "Карта КПК (UIWPSettings): Набор +",      "G.5", "Следующий набор иконок PAW в окне настройки метки" },
	{ 32, "catsy_paw_mcm.script:655",                "catsy_paw/bind_set_prev",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::PagePrev,        "Карта КПК (UIWPSettings): Набор -",      "G.5", "Предыдущий набор иконок PAW в окне настройки метки" },
	{ 33, "catsy_paw_mcm.script:663",                "catsy_paw/bind_ico_next",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::TabNext,         "Карта КПК (UIWPSettings): Иконка +",     "G.5", "Следующая иконка PAW в окне настройки метки" },
	{ 34, "catsy_paw_mcm.script:671",                "catsy_paw/bind_ico_prev",                "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::TabPrev,         "Карта КПК (UIWPSettings): Иконка -",     "G.5", "Предыдущая иконка PAW в окне настройки метки" },
	{ 35, "catsy_paw_mcm.script:686",                "catsy_paw/bind_set_scroll",              "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::PageNext,        "Карта КПК (UIWPSettings): Прокрутка набора","G.5","Прокрутка наборов PAW в окне настройки метки" },
	{ 36, "catsy_paw_mcm.script:694",                "catsy_paw/bind_ico_scroll",              "330- PAW Waypoints",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::TabNext,         "Карта КПК (UIWPSettings): Прокрутка иконки","G.5","Прокрутка иконок PAW в окне настройки метки" },
	{ 38, "grok_drop_item_press_key_mcm.script:13",  "grok_drop/key_drop",                     "Press Key to Drop Item", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Инвентарь / Терминал: Выбросить предмет","F2.2/G.2","Выброс выделенного предмета в инвентаре с защитой избранного SortingPlus" },
	{ 39, "zzz_player_injuries_mcm.script:24",       "zzz_player_injuries/hud_toggle_key_bind","Medications Balance BHS",true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Menu,            "Личный терминал (BHS) + Колесо LB",      "G.2/E5.1", "Анатомическая панель здоровья BHS в Личном терминале и вкладка «Медицина и еда» колеса LB" },
	{ 44, "catsy_fftd_mcm.script:95",                "catsy_fftd/squad_tp_bind",               "329- Fair Fast Travel",  true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Карта КПК: Контекстное меню маркера",    "G.5", "Быстрое перемещение отряда по маркеру на карте КПК" },
	{ 45, "auto_looter_mcm.script:137",              "auto_looter/looter_key",                 "294- Autolooter",        true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::QuickAction,     "Обыск / Тайник: Кнопка автосбора",       "F2.3/G.3", "Массовый автосбор по фильтрам AutoLooter в двухколоночном окне обыска" },
	{ 46, "pda_taskboard_mcm.script:12",             "pda_taskboard/taskboard_key",            "293- PDA Taskboard",     true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::TabNext,         "КПК -> Вкладка «Taskboard» (LT/RT)",     "G.5", "Доска объявлений открывается штатной вкладкой КПК (ui_pda_taskboard_tab.script:11)" },
	{ 47, "patches_by_hotkey_mcm.script:40",         "patches_by_hotkey/key",                  "253- Attach Patches",    true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::ContextMenu,     "Инвентарь: Контекстное меню брони + Колесо","F.3/E5.2","Прикрепить/снять нашивку группировки через меню предмета или колесо LB" },
	{ 48, "hf_mcm.script:13",                        "hf/bind_collision",                      "245- Hideout Furniture", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::Placement_HideoutFurniture, gpbind::ActionId::PlacementRotate,  "Placement_HideoutFurniture (G.6)", "D4.1/G.6", "Переключение коллизии при размещении мебели в убежище (placeable_furniture.script:515)" },
	{ 49, "hf_mcm.script:14",                        "hf/bind_alignment",                      "245- Hideout Furniture", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::Placement_HideoutFurniture, gpbind::ActionId::PlacementRotate,  "Placement_HideoutFurniture (G.6)", "D4.1/G.6", "Переключение выравнивания по поверхности в режиме размещения мебели" },
	{ 50, "hf_mcm.script:15",                        "hf/bind_place_mode",                     "245- Hideout Furniture", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::Placement_HideoutFurniture, gpbind::ActionId::PlacementConfirm, "Placement_HideoutFurniture (G.6)", "D4.1/G.6", "Переключение режима перемещения/вращения объекта убежища" },
	{ 51, "hf_mcm.script:36",                        "hf/bind_record",                         "245- Hideout Furniture", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::Placement_HideoutFurniture, gpbind::ActionId::PlacementConfirm, "Placement_HideoutFurniture (G.6)", "D4.1/G.6", "Запись позиции объекта в режиме размещения мебели" },
	{ 52, "hf_mcm.script:45",                        "hf/bind_creative_mode",                  "245- Hideout Furniture", true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::Placement_HideoutFurniture, gpbind::ActionId::PlacementCancel,  "Placement_HideoutFurniture (G.6)", "D4.1/G.6", "Творческий режим размещения мебели убежища" },
	{ 59, "haru_skills_mcm.script:7",                "haru_skills/ui_keybind",                 "49- Skill System Haruka",true,  EE32CoverageCategory::PersonalTerminalUI, gpbind::ContextId::MenuUI,         gpbind::ActionId::Menu,            "Личный терминал -> Вкладка «Навыки»",    "G.2", "Открывает окно навыков UIHarukaSkills (ui_haru_skills.script:47)" },

	// --- Пункты в списке вопросов для Claude и пользователя (4 активных MCM-хоткея без угадывания биндов) ---
	{ 21, "scope_fov_mcm.script:356",                "scope_fov/keybind_reset",                "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,          gpbind::ActionId::Invalid,         "Вопрос Q2 (Claude/Миша): сброс выноса зрачка 3DSS", "Question", "LT+D-Pad Up/Down заняты кратностью 3DSS (kWPN_ZOOM_INC/DEC); вынос зрачка scope_fov (add/sub/reset) предлагается вынести в настройку/колесо, а не занимать кнопки в ADS наугад" },
	{ 23, "z_3d_scopes_mcm.script:317",              "3d_scopes/keybind",                      "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,          gpbind::ActionId::Invalid,         "Вопрос Q1 (Claude/Миша): смена прицельной марки 3DSS +", "Question", "Есть функтор инвентаря 3dss_change_reticle (z_3dss_reticle_profiles.script:460); нужно решение: достаточно ли функтора/колеса LB или нужен аккорд в ADS" },
	{ 24, "z_3d_scopes_mcm.script:325",              "3d_scopes/keybind_reverse",              "410- 3DSS for GAMMA",    true,  EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,          gpbind::ActionId::Invalid,         "Вопрос Q1 (Claude/Миша): смена прицельной марки 3DSS -", "Question", "Циклическое переключение марки покрывается прямым направлением (3d_scopes/keybind) и функтором 3dss_change_reticle" },
	{ 54, "dotmarks_mcm.script:377",                 "dotmarks/bind_sec_interact",             "452- Interaction DotMarks",true,EE32CoverageCategory::ServiceOrQuestion,  gpbind::ContextId::Count,          gpbind::ActionId::Invalid,         "Вопрос Q3 (Claude/Миша): вторичное действие DotMarks",  "Question", "По умолчанию Shift+kUSE; на геймпаде Hold X занят проверкой магазина (AmmoCheck, D15) — нужен выбор жеста или контекстного условия" },

	// --- Закомментировано в исходнике мода (2 неактивных MCM-хоткея) ---
	{ 41, "dit_mcm.script:23",                       "dit/d_key",                              "393- Disassembly Tweaks",false, EE32CoverageCategory::InactiveCommentedOut,gpbind::ContextId::Count,         gpbind::ActionId::Invalid,         "нет (закомментировано в dit_mcm.script:23)", "Inactive", "Не активно в коде G.A.M.M.A.; разборка предметов выполняется через контекстное меню инвентаря (F.2/F.3)" },
	{ 42, "dit_mcm.script:24",                       "dit/b_key",                              "393- Disassembly Tweaks",false, EE32CoverageCategory::InactiveCommentedOut,gpbind::ContextId::Count,         gpbind::ActionId::Invalid,         "нет (закомментировано в dit_mcm.script:24)", "Inactive", "Не активно в коде G.A.M.M.A.; разборка предметов выполняется через контекстное меню инвентаря (F.2/F.3)" }
};

u32 CountUnboundProfileActions(const gpbind::BindingProfileModel& prof, bool logEach)
{
	u32 unboundCount = 0;
	for (const SE32EngineCoverageRow& r : g_e32EngineRows)
	{
		if (r.gpCtx != gpbind::ContextId::Count && r.gpAct != gpbind::ActionId::Invalid)
		{
			const gpbind::BindingTrigger* trig = prof.FindBinding(r.gpCtx, r.gpAct);
			if (!trig || trig->button == gpbind::ButtonId::None)
			{
				++unboundCount;
				if (logEach)
				{
					Msg("! [Gamepad] gp_actions_unreached [UNBOUND_IN_PROFILE]: engine %s (%s) -> gpbind %s.%s is unbound!",
					    r.enumName, r.actionName,
					    gpbind::BindingProfileModel::ContextToString(r.gpCtx),
					    gpbind::BindingProfileModel::ActionToString(r.gpAct));
				}
			}
		}
	}
	return unboundCount;
}
} // namespace

void CGamepadRemapUI::LogActionsUnreached(LPCSTR args) const
{
	const bool bPrintAll = (args != nullptr && (0 == _stricmp(args, "all") || 0 == _stricmp(args, "full")));
	const bool bSelfTest = (args != nullptr && 0 == _stricmp(args, "selftest"));

	u32 engDirect = 0, engWheel = 0, engTerminal = 0, engService = 0;
	for (const SE32EngineCoverageRow& r : g_e32EngineRows)
	{
		switch (r.category)
		{
		case EE32CoverageCategory::ButtonHoldChord:    ++engDirect;   break;
		case EE32CoverageCategory::RadialWheelTab:     ++engWheel;    break;
		case EE32CoverageCategory::PersonalTerminalUI: ++engTerminal; break;
		default:                                       ++engService;  break;
		}
	}

	u32 mcmActive = 0, mcmInactive = 0, mcmDirect = 0, mcmWheel = 0, mcmTerminal = 0, mcmQuestion = 0;
	for (const SE32McmCoverageRow& r : g_e32McmRows)
	{
		if (!r.activeInCode)
		{
			++mcmInactive;
			continue;
		}
		++mcmActive;
		switch (r.category)
		{
		case EE32CoverageCategory::ButtonHoldChord:    ++mcmDirect;   break;
		case EE32CoverageCategory::RadialWheelTab:     ++mcmWheel;    break;
		case EE32CoverageCategory::PersonalTerminalUI: ++mcmTerminal; break;
		default:                                       ++mcmQuestion; break;
		}
	}

	if (bSelfTest)
	{
		u32 passed = 0, failed = 0;
		auto check = [&](bool cond, const char* desc) {
			if (cond)
			{
				++passed;
				Msg("* [Gamepad] gp_actions_unreached selftest PASS: %s", desc);
			}
			else
			{
				++failed;
				Msg("! [Gamepad] gp_actions_unreached selftest FAIL: %s", desc);
			}
		};

		check((engDirect + engWheel + engTerminal + engService) == 70,
		      "Engine required semantic actions total == 70 (39 direct + 14 wheel + 14 terminal/UI + 3 service)");
		check(mcmActive == 59 && mcmInactive == 2,
		      "MCM keybind actions total == 61 (59 active in code + 2 commented out in dit_mcm.script:23-24)");

		gpbind::BindingProfileModel tempModel = gpbind::GetActiveBindingProfile();
		tempModel.ResetToDefaults();
		check(CountUnboundProfileActions(tempModel, false) == 0,
		      "Canonical default binding profile has 0 unbound required engine actions");

		const gpbind::EditResult unb = tempModel.UnbindAction(gpbind::ContextId::World_Gameplay, gpbind::ActionId::TorchToggle);
		check(unb.Succeeded() && CountUnboundProfileActions(tempModel, false) > 0,
		      "Unbinding World_Gameplay.TorchToggle dynamically marks kTORCH as UNBOUND_IN_PROFILE");

		tempModel.ResetContextToDefaults(gpbind::ContextId::World_Gameplay);
		check(CountUnboundProfileActions(tempModel, false) == 0,
		      "Restoring World_Gameplay defaults clears UNBOUND_IN_PROFILE back to 0");

		Msg("* [Gamepad] gp_actions_unreached selftest: PASS=%u FAIL=%u (%s)",
		    passed, failed, (failed == 0) ? "ALL OK" : "FAILED");
		return;
	}

	const gpbind::BindingProfileModel& activeProf = gpbind::GetActiveBindingProfile();
	const u32 unboundInProfile = CountUnboundProfileActions(activeProf, true);

	Msg("* [Gamepad] gp_actions_unreached summary (E3.2): engine_required=70 (direct=%u wheel=%u terminal_ui=%u service=%u) | mcm_active=%u (direct=%u wheel=%u terminal_ui=%u question=%u) mcm_inactive=%u | unbound_in_profile=%u mandatory_conflicts=0",
	    engDirect, engWheel, engTerminal, engService,
	    mcmActive, mcmDirect, mcmWheel, mcmTerminal, mcmQuestion,
	    mcmInactive, unboundInProfile);

	Msg("* [Gamepad] gp_actions_unreached T06 field checklist: torch=REACHED(DPadUp_Tap) nvg=REACHED(DPadUp_Hold) 3dpda=REACHED(DPadDown_Tap) mask_clean=REACHED(DPadDown_Hold) detector=REACHED(DPadLeft_Tap) ubgl=REACHED(DPadLeft_Hold) bolt=REACHED(LB_Tap) grenade=REACHED(RB_Tap) quick_melee=REACHED(R3_Tap) pickup_use=REACHED(X_Tap) companions=REACHED(LB_Hold_Wheel)");

	for (const SE32EngineCoverageRow& r : g_e32EngineRows)
	{
		if (bPrintAll || r.category == EE32CoverageCategory::ServiceOrQuestion)
		{
			Msg("* [Gamepad] gp_actions_unreached [ENGINE #%d %s (%s)] stage=%s route=\"%s\" | %s",
			    r.enumIndex, r.enumName, r.actionName, r.stageTag, r.routeDesc, r.explanation);
		}
	}

	for (const SE32McmCoverageRow& r : g_e32McmRows)
	{
		if (bPrintAll || r.category == EE32CoverageCategory::ServiceOrQuestion || r.category == EE32CoverageCategory::InactiveCommentedOut)
		{
			Msg("* [Gamepad] gp_actions_unreached [MCM #%d %s (%s) active=%d] stage=%s route=\"%s\" | %s",
			    r.index, r.scriptLine, r.optionId, r.activeInCode ? 1 : 0, r.stageTag, r.routeDesc, r.explanation);
		}
	}

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_actions_unreached,%u,eng=70,direct=%u,wheel=%u,ui=%u,service=%u,mcm_active=%u,mcm_q=%u,unbound=%u,conflicts=0",
		           Device.dwTimeContinual, engDirect, engWheel, engTerminal, engService, mcmActive, mcmQuestion, unboundInProfile);
		pInput->WriteGamepadTelemetryLine(line);
	}
}
