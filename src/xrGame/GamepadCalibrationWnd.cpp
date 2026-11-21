#include "stdafx.h"
#include "GamepadCalibrationWnd.h"
#include "PauseTokenManager.h"
#include "GamepadContextDispatcher.h"
#include "../xrEngine/GamepadMath.h"
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
#include <cmath>

extern ENGINE_API int ps_gp_telemetry;

namespace
{
class CUIGamepadCalibrationWnd : public CUIDialogWnd
{
	typedef CUIDialogWnd inherited;

public:
	static constexpr int NUM_ROWS = 9;
	static constexpr int NUM_TABS = 4; // 0=LeftStick, 1=RightStick, 2=UIStick, 3=Triggers
	static constexpr u32 REPEAT_INITIAL_DELAY_MS = 320;
	static constexpr u32 REPEAT_STEP_MS = 90;

	CUIGamepadCalibrationWnd()
	{
		AllowWorkInPause(true);
		AllowMovement(false);
		AllowCursor(false);
		AllowCenterCursor(false);

		SetWndRect(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT));

		m_shaderWire->create("hud\\crosshair");

		m_pTitle = CreateLabel(112.0f, 28.0f, 800.0f, 32.0f, UI().Font().pFontLetterica18Russian,
		                       color_argb(255, 245, 215, 115), CGameFont::alCenter);
		m_pSubtitle = CreateLabel(112.0f, 56.0f, 800.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                          color_argb(255, 185, 215, 245), CGameFont::alCenter);

		m_pLeftStickTitle = CreateLabel(100.0f, 92.0f, 260.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                                color_argb(255, 220, 235, 255), CGameFont::alCenter);
		m_pLeftStickReadout = CreateLabel(70.0f, 304.0f, 320.0f, 44.0f, UI().Font().pFontLetterica16Russian,
		                                  color_argb(255, 195, 225, 200), CGameFont::alCenter);

		m_pCurveTitle = CreateLabel(392.0f, 92.0f, 240.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                            color_argb(255, 220, 235, 255), CGameFont::alCenter);
		m_pCurveReadout = CreateLabel(382.0f, 304.0f, 260.0f, 44.0f, UI().Font().pFontLetterica16Russian,
		                              color_argb(255, 215, 205, 175), CGameFont::alCenter);

		m_pRightStickTitle = CreateLabel(664.0f, 92.0f, 260.0f, 24.0f, UI().Font().pFontLetterica16Russian,
		                                 color_argb(255, 220, 235, 255), CGameFont::alCenter);
		m_pRightStickReadout = CreateLabel(634.0f, 304.0f, 320.0f, 44.0f, UI().Font().pFontLetterica16Russian,
		                                   color_argb(255, 195, 225, 200), CGameFont::alCenter);

		m_pLtLabel = CreateLabel(92.0f, 354.0f, 380.0f, 22.0f, UI().Font().pFontLetterica16Russian,
		                         color_argb(255, 225, 225, 235), CGameFont::alLeft);
		m_pRtLabel = CreateLabel(552.0f, 354.0f, 380.0f, 22.0f, UI().Font().pFontLetterica16Russian,
		                         color_argb(255, 225, 225, 235), CGameFont::alLeft);

		const float rowStartY = 418.0f;
		const float rowStepY = 29.0f;
		for (int i = 0; i < NUM_ROWS; ++i)
		{
			m_pRows[i] = CreateLabel(140.0f, rowStartY + i * rowStepY, 744.0f, 26.0f,
			                         UI().Font().pFontLetterica16Russian,
			                         color_argb(255, 210, 215, 225), CGameFont::alLeft);
		}

		m_pFooter = CreateLabel(80.0f, 695.0f, 864.0f, 40.0f, UI().Font().pFontLetterica16Russian,
		                        color_argb(255, 240, 210, 130), CGameFont::alCenter);

		RefreshTexts();
	}

	virtual ~CUIGamepadCalibrationWnd()
	{
		ReleasePauseTokenIfHeld();
	}

	virtual bool WorkInPause() const override { return true; }
	virtual bool NeedCursor() const override { return false; }

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
					m_pauseTokenId = CPauseTokenManager::Instance().Acquire("gp_calibration");
				}
			}
			m_lastUpdateTimeContinual = Device.dwTimeContinual;
			m_heldDir = DirNone;
			m_prevButtonsMask = GetCurrentButtonsMask();
			m_bLtPressed = false;
			m_bRtPressed = false;
			RefreshTexts();

			Msg("* [Gamepad] CUIGamepadCalibrationWnd::Show(true): token=%u dwTimeContinual=%u Paused=%d mainMenu=%d",
			    m_pauseTokenId, Device.dwTimeContinual, Device.Paused() ? 1 : 0,
			    (MainMenu() && MainMenu()->IsActive()) ? 1 : 0);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_calib_window,%u,open,token=%u,paused=%d",
				           Device.dwTimeContinual, m_pauseTokenId, Device.Paused() ? 1 : 0);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
		else if (!status && wasShown)
		{
			// Save calibration profile to $app_data_root$\gamepad\profile.ltx on exit (D37 / D2.2)
			gpmath::SaveCalibrationProfile();

			const u32 releasedToken = m_pauseTokenId;
			ReleasePauseTokenIfHeld();
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
			CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_START);

			Msg("* [Gamepad] CUIGamepadCalibrationWnd::Show(false): saved profile, released_token=%u dwTimeContinual=%u",
			    releasedToken, Device.dwTimeContinual);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_calib_window,%u,close_and_save,token=%u,paused=%d",
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
		m_lastUpdateTimeContinual = now;

		SampleLiveGamepadAxes();
		PollGamepadControls(now);
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
			if (dik == DIK_ESCAPE)
			{
				HideDialog();
				return true;
			}
			if (dik == DIK_UP || dik == DIK_W)
			{
				MoveRow(-1);
				return true;
			}
			if (dik == DIK_DOWN || dik == DIK_S)
			{
				MoveRow(1);
				return true;
			}
			if (dik == DIK_LEFT || dik == DIK_A)
			{
				AdjustCurrentRow(-1);
				return true;
			}
			if (dik == DIK_RIGHT || dik == DIK_D)
			{
				AdjustCurrentRow(1);
				return true;
			}
			if (dik == DIK_Q || dik == DIK_PRIOR)
			{
				SwitchTab(-1);
				return true;
			}
			if (dik == DIK_E || dik == DIK_NEXT || dik == DIK_TAB)
			{
				SwitchTab(1);
				return true;
			}
			if (dik == DIK_R)
			{
				ResetCurrentTabToDefaults();
				return true;
			}
			if (dik == DIK_RETURN || dik == DIK_SPACE)
			{
				ActivateCurrentRow();
				return true;
			}
		}
		return inherited::OnKeyboardAction(dik, keyboard_action);
	}

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

	u32 GetCurrentButtonsMask() const
	{
		if (!pInput)
			return 0;
		const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		if (!gp.connected || !gp.has_focus)
			return 0;
		return gp.buttons_mask;
	}

	void SampleLiveGamepadAxes()
	{
		m_rawLX = 0.0f;
		m_rawLY = 0.0f;
		m_rawRX = 0.0f;
		m_rawRY = 0.0f;
		m_rawLT = 0.0f;
		m_rawRT = 0.0f;

		if (pInput)
		{
			const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
			if (gp.connected && gp.has_focus)
			{
				m_rawLX = gpmath::NormalizeStickAxis(gp.axes[0]);
				m_rawLY = -gpmath::NormalizeStickAxis(gp.axes[1]);
				m_rawRX = gpmath::NormalizeStickAxis(gp.axes[2]);
				m_rawRY = -gpmath::NormalizeStickAxis(gp.axes[3]);
				m_rawLT = gpmath::NormalizeTriggerAxis(gp.axes[4]);
				m_rawRT = gpmath::NormalizeTriggerAxis(gp.axes[5]);
			}
		}

		const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
		m_procL = gpmath::ProcessStick(prof.left_stick, m_rawLX, m_rawLY);
		m_procR = gpmath::ProcessStick(prof.right_stick, m_rawRX, m_rawRY);
		m_procLT = gpmath::ProcessTrigger(prof.left_trigger, m_rawLT);
		m_procRT = gpmath::ProcessTrigger(prof.right_trigger, m_rawRT);
		m_bLtPressed = gpmath::UpdateTriggerButton(prof.left_trigger, m_bLtPressed, m_rawLT);
		m_bRtPressed = gpmath::UpdateTriggerButton(prof.right_trigger, m_bRtPressed, m_rawRT);
	}

	void PollGamepadControls(u32 now)
	{
		const u32 curMask = GetCurrentButtonsMask();
		const u32 newlyPressed = curMask & ~m_prevButtonsMask;
		m_prevButtonsMask = curMask;

		// Close & Save on B or Start
		if ((newlyPressed & ((1u << CInput::GP_BTN_EAST) | (1u << CInput::GP_BTN_START))) != 0)
		{
			HideDialog();
			return;
		}

		// Switch target tab on LB / RB
		if ((newlyPressed & (1u << CInput::GP_BTN_LEFT_SHOULDER)) != 0)
			SwitchTab(-1);
		if ((newlyPressed & (1u << CInput::GP_BTN_RIGHT_SHOULDER)) != 0)
			SwitchTab(1);

		// Reset current tab on X, reset all on Y
		if ((newlyPressed & (1u << CInput::GP_BTN_WEST)) != 0)
			ResetCurrentTabToDefaults();
		if ((newlyPressed & (1u << CInput::GP_BTN_NORTH)) != 0)
		{
			gpmath::ResetCalibrationProfileToDefaults();
			RefreshTexts();
		}

		// Activate on A
		if ((newlyPressed & (1u << CInput::GP_BTN_SOUTH)) != 0)
			ActivateCurrentRow();

		// D-Pad navigation with 320ms / 90ms repeat on Device.dwTimeContinual
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

		if (activeDir != m_heldDir)
		{
			m_heldDir = activeDir;
			m_nextRepeatTimeMs = now + REPEAT_INITIAL_DELAY_MS;
			StepDirection(activeDir);
		}
		else if (now >= m_nextRepeatTimeMs)
		{
			m_nextRepeatTimeMs = now + REPEAT_STEP_MS;
			StepDirection(activeDir);
		}
	}

	void StepDirection(EDir dir)
	{
		switch (dir)
		{
		case DirUp: MoveRow(-1); break;
		case DirDown: MoveRow(1); break;
		case DirLeft: AdjustCurrentRow(-1); break;
		case DirRight: AdjustCurrentRow(1); break;
		default: break;
		}
	}

	void SwitchTab(int delta)
	{
		m_activeTab = (m_activeTab + delta + NUM_TABS) % NUM_TABS;
	}

	void MoveRow(int delta)
	{
		m_selectedRow = (m_selectedRow + delta + NUM_ROWS) % NUM_ROWS;
	}

	gpmath::StickConfig& GetActiveStickConfig(gpmath::CalibrationProfile& prof)
	{
		if (m_activeTab == 1)
			return prof.right_stick;
		if (m_activeTab == 2)
			return prof.ui_stick;
		return prof.left_stick;
	}

	const gpmath::StickConfig& GetActiveStickConfig(const gpmath::CalibrationProfile& prof) const
	{
		if (m_activeTab == 1)
			return prof.right_stick;
		if (m_activeTab == 2)
			return prof.ui_stick;
		return prof.left_stick;
	}

	void ResetCurrentTabToDefaults()
	{
		gpmath::CalibrationProfile& prof = gpmath::GetMutableCalibrationProfile();
		if (m_activeTab == 0)
			prof.left_stick = gpmath::StickConfig{};
		else if (m_activeTab == 1)
		{
			prof.right_stick = gpmath::StickConfig{};
			prof.omega_horz = 4.0f;
			prof.omega_vert = 2.5f;
		}
		else if (m_activeTab == 2)
			prof.ui_stick = gpmath::StickConfig{};
		else
		{
			prof.left_trigger = gpmath::TriggerConfig{};
			prof.right_trigger = gpmath::TriggerConfig{};
		}
		gpmath::SanitizeProfile(prof, nullptr);
	}

	void ActivateCurrentRow()
	{
		if (m_selectedRow == 0)
		{
			SwitchTab(1);
		}
		else if (m_selectedRow == 8 && m_activeTab < 3)
		{
			ResetCurrentTabToDefaults();
		}
		else
		{
			AdjustCurrentRow(1);
		}
	}

	void AdjustCurrentRow(int dir)
	{
		const float step = 0.01f * static_cast<float>(dir);
		gpmath::CalibrationProfile& prof = gpmath::GetMutableCalibrationProfile();

		if (m_selectedRow == 0)
		{
			SwitchTab(dir);
			return;
		}

		if (m_activeTab < 3)
		{
			gpmath::StickConfig& stk = GetActiveStickConfig(prof);
			switch (m_selectedRow)
			{
			case 1: // Deadzone Shape
				stk.shape = (stk.shape == gpmath::DeadzoneShape::Radial)
				                ? gpmath::DeadzoneShape::Axial
				                : gpmath::DeadzoneShape::Radial;
				break;
			case 2: // Inner Deadzone
				stk.deadzone = clampr(stk.deadzone + step, gpmath::limits::kDeadzoneMin, gpmath::limits::kDeadzoneMax);
				if (stk.saturation < stk.deadzone + gpmath::limits::kMinSpan)
					stk.saturation = clampr(stk.deadzone + gpmath::limits::kMinSpan, 0.05f, 1.0f);
				break;
			case 3: // Outer Saturation
				stk.saturation = clampr(stk.saturation + step, stk.deadzone + gpmath::limits::kMinSpan, gpmath::limits::kSaturationMax);
				break;
			case 4: // Curve Type
			{
				const int cur = static_cast<int>(stk.curve.type);
				const int next = (cur + dir + 4) % 4;
				stk.curve.type = static_cast<gpmath::CurveType>(next);
				break;
			}
			case 5: // Curve Parameter 1 (gamma / s_exponent / knee_in)
				if (stk.curve.type == gpmath::CurveType::Power)
					stk.curve.gamma = clampr(stk.curve.gamma + step * 5.0f, gpmath::limits::kExpMin, gpmath::limits::kExpMax);
				else if (stk.curve.type == gpmath::CurveType::SCurve)
					stk.curve.s_exponent = clampr(stk.curve.s_exponent + step * 5.0f, gpmath::limits::kExpMin, gpmath::limits::kExpMax);
				else if (stk.curve.type == gpmath::CurveType::TwoZone)
					stk.curve.knee_in = clampr(stk.curve.knee_in + step * 2.0f, gpmath::limits::kKneeMin, gpmath::limits::kKneeMax);
				break;
			case 6: // Curve Parameter 2 (knee_out or Right Stick omega)
				if (stk.curve.type == gpmath::CurveType::TwoZone)
					stk.curve.knee_out = clampr(stk.curve.knee_out + step * 2.0f, gpmath::limits::kKneeMin, gpmath::limits::kKneeMax);
				else if (m_activeTab == 1)
				{
					prof.omega_horz = clampr(prof.omega_horz + step * 10.0f, gpmath::limits::kOmegaMin, gpmath::limits::kOmegaMax);
					prof.omega_vert = clampr(prof.omega_vert + step * 6.25f, gpmath::limits::kOmegaMin, gpmath::limits::kOmegaMax);
				}
				break;
			case 7: // Invert Y / Invert X
				if (dir > 0)
					stk.invert_y = !stk.invert_y;
				else
					stk.invert_x = !stk.invert_x;
				break;
			case 8:
				ResetCurrentTabToDefaults();
				break;
			default:
				break;
			}
		}
		else
		{
			// Triggers tab (LT & RT)
			switch (m_selectedRow)
			{
			case 1:
				prof.left_trigger.deadzone = clampr(prof.left_trigger.deadzone + step, gpmath::limits::kDeadzoneMin, gpmath::limits::kDeadzoneMax);
				break;
			case 2:
				prof.left_trigger.saturation = clampr(prof.left_trigger.saturation + step, prof.left_trigger.deadzone + gpmath::limits::kMinSpan, 1.0f);
				break;
			case 3:
				prof.left_trigger.press = clampr(prof.left_trigger.press + step, gpmath::limits::kTriggerMinGap, 1.0f);
				if (prof.left_trigger.release > prof.left_trigger.press - gpmath::limits::kTriggerMinGap)
					prof.left_trigger.release = clampr(prof.left_trigger.press - gpmath::limits::kTriggerMinGap, 0.0f, 0.95f);
				break;
			case 4:
				prof.left_trigger.release = clampr(prof.left_trigger.release + step, 0.0f, prof.left_trigger.press - gpmath::limits::kTriggerMinGap);
				break;
			case 5:
				prof.right_trigger.deadzone = clampr(prof.right_trigger.deadzone + step, gpmath::limits::kDeadzoneMin, gpmath::limits::kDeadzoneMax);
				break;
			case 6:
				prof.right_trigger.saturation = clampr(prof.right_trigger.saturation + step, prof.right_trigger.deadzone + gpmath::limits::kMinSpan, 1.0f);
				break;
			case 7:
				prof.right_trigger.press = clampr(prof.right_trigger.press + step, gpmath::limits::kTriggerMinGap, 1.0f);
				if (prof.right_trigger.release > prof.right_trigger.press - gpmath::limits::kTriggerMinGap)
					prof.right_trigger.release = clampr(prof.right_trigger.press - gpmath::limits::kTriggerMinGap, 0.0f, 0.95f);
				break;
			case 8:
				prof.right_trigger.release = clampr(prof.right_trigger.release + step, 0.0f, prof.right_trigger.press - gpmath::limits::kTriggerMinGap);
				break;
			default:
				break;
			}
		}

		gpmath::SanitizeProfile(prof, nullptr);
	}

	static LPCSTR TrCal(LPCSTR id, LPCSTR fallback)
	{
		if (CStringTable::getPData())
		{
			const shared_str s = CStringTable().translate(id);
			if (s.size() && 0 != xr_strcmp(s.c_str(), id))
				return s.c_str();
		}
		return fallback;
	}

	static LPCSTR GetLocalizedDeadzoneShort(gpmath::DeadzoneShape shape)
	{
		return (shape == gpmath::DeadzoneShape::Radial)
		    ? TrCal("ui_gp_eng_cal_shape_short_radial", "radial")
		    : TrCal("ui_gp_eng_cal_shape_short_axial", "axial");
	}

	static LPCSTR GetLocalizedCurveShort(gpmath::CurveType type)
	{
		switch (type)
		{
		case gpmath::CurveType::Power:   return TrCal("ui_gp_eng_cal_curve_short_power", "power");
		case gpmath::CurveType::SCurve:  return TrCal("ui_gp_eng_cal_curve_short_scurve", "s_curve");
		case gpmath::CurveType::TwoZone: return TrCal("ui_gp_eng_cal_curve_short_twozone", "two_zone");
		case gpmath::CurveType::Linear:
		default:                         return TrCal("ui_gp_eng_cal_curve_short_linear", "linear");
		}
	}

	void RefreshTexts()
	{
		const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();

		m_pTitle->SetText(TrCal("ui_gp_eng_cal_title", "GAMEPAD STICK & TRIGGER CALIBRATION"));

		string512 subBuf;
		xr_sprintf(subBuf, sizeof(subBuf),
		           TrCal("ui_gp_eng_cal_subtitle_fmt", "Profile v%u | Source Arb DZ=%.2f | LB/RB (Q/E): Switch Target | D-Pad: Select/Adjust | X (R): Reset Tab | B (Esc): Save & Exit"),
		           prof.version, gpmath::GetCalibrationStickDeadzone());
		m_pSubtitle->SetText(subBuf);

		string256 lsTitle;
		xr_sprintf(lsTitle, sizeof(lsTitle), TrCal("ui_gp_eng_cal_ls_title_fmt", "Left Stick (Move) [%s, %s]"),
		           GetLocalizedDeadzoneShort(prof.left_stick.shape),
		           GetLocalizedCurveShort(prof.left_stick.curve.type));
		m_pLeftStickTitle->SetText(lsTitle);

		const float rawMagL = std::sqrt(m_rawLX * m_rawLX + m_rawLY * m_rawLY);
		const float procMagL = std::sqrt(m_procL.x * m_procL.x + m_procL.y * m_procL.y);
		string512 lsReadout;
		xr_sprintf(lsReadout, sizeof(lsReadout),
		           TrCal("ui_gp_eng_cal_stick_readout_fmt", "Raw: (%+.2f, %+.2f) |v|=%.2f  ->  Out: (%+.2f, %+.2f) |v|=%.2f   [DZ=%.2f Sat=%.2f]"),
		           m_rawLX, m_rawLY, rawMagL, m_procL.x, m_procL.y, procMagL,
		           prof.left_stick.deadzone, prof.left_stick.saturation);
		m_pLeftStickReadout->SetText(lsReadout);

		const gpmath::StickConfig& curStk = GetActiveStickConfig(prof);
		string256 crvTitle;
		xr_sprintf(crvTitle, sizeof(crvTitle), TrCal("ui_gp_eng_cal_curve_title_fmt", "Curve: %s"), GetLocalizedCurveShort(curStk.curve.type));
		m_pCurveTitle->SetText(crvTitle);

		string256 crvReadout;
		xr_sprintf(crvReadout, sizeof(crvReadout),
		           TrCal("ui_gp_eng_cal_curve_readout_fmt", "gamma=%.2f  s_exp=%.2f  knee=(%.2f -> %.2f)"),
		           curStk.curve.gamma, curStk.curve.s_exponent,
		           curStk.curve.knee_in, curStk.curve.knee_out);
		m_pCurveReadout->SetText(crvReadout);

		string256 rsTitle;
		xr_sprintf(rsTitle, sizeof(rsTitle), TrCal("ui_gp_eng_cal_rs_title_fmt", "Right Stick (Look) [%s, %s]"),
		           GetLocalizedDeadzoneShort(prof.right_stick.shape),
		           GetLocalizedCurveShort(prof.right_stick.curve.type));
		m_pRightStickTitle->SetText(rsTitle);

		const float rawMagR = std::sqrt(m_rawRX * m_rawRX + m_rawRY * m_rawRY);
		const float procMagR = std::sqrt(m_procR.x * m_procR.x + m_procR.y * m_procR.y);
		string512 rsReadout;
		xr_sprintf(rsReadout, sizeof(rsReadout),
		           TrCal("ui_gp_eng_cal_stick_readout_fmt", "Raw: (%+.2f, %+.2f) |v|=%.2f  ->  Out: (%+.2f, %+.2f) |v|=%.2f   [DZ=%.2f Sat=%.2f]"),
		           m_rawRX, m_rawRY, rawMagR, m_procR.x, m_procR.y, procMagR,
		           prof.right_stick.deadzone, prof.right_stick.saturation);
		m_pRightStickReadout->SetText(rsReadout);

		const LPCSTR strPressed = TrCal("ui_gp_eng_cal_trig_pressed", "PRESSED");
		const LPCSTR strReleased = TrCal("ui_gp_eng_cal_trig_released", "RELEASED");

		string512 ltBuf, rtBuf;
		xr_sprintf(ltBuf, sizeof(ltBuf),
		           TrCal("ui_gp_eng_cal_lt_readout_fmt", "LT: Raw=%.2f Out=%.2f [%s] (DZ=%.2f Rel=%.2f Press=%.2f Sat=%.2f)"),
		           m_rawLT, m_procLT, m_bLtPressed ? strPressed : strReleased,
		           prof.left_trigger.deadzone, prof.left_trigger.release,
		           prof.left_trigger.press, prof.left_trigger.saturation);
		m_pLtLabel->SetText(ltBuf);

		xr_sprintf(rtBuf, sizeof(rtBuf),
		           TrCal("ui_gp_eng_cal_rt_readout_fmt", "RT: Raw=%.2f Out=%.2f [%s] (DZ=%.2f Rel=%.2f Press=%.2f Sat=%.2f)"),
		           m_rawRT, m_procRT, m_bRtPressed ? strPressed : strReleased,
		           prof.right_trigger.deadzone, prof.right_trigger.release,
		           prof.right_trigger.press, prof.right_trigger.saturation);
		m_pRtLabel->SetText(rtBuf);

		const LPCSTR tabNames[NUM_TABS] = {
			TrCal("ui_gp_eng_cal_tab_ls", "Left Stick (Movement)"),
			TrCal("ui_gp_eng_cal_tab_rs", "Right Stick (Camera Look)"),
			TrCal("ui_gp_eng_cal_tab_ui", "UI Stick (Menu Navigation)"),
			TrCal("ui_gp_eng_cal_tab_trig", "Triggers LT/RT (Analog & Thresholds)")
		};

		string512 rowStr[NUM_ROWS];
		xr_sprintf(rowStr[0], sizeof(rowStr[0]), TrCal("ui_gp_eng_cal_row_target_fmt", "Target Group               :  <  %s  >"), tabNames[m_activeTab]);

		if (m_activeTab < 3)
		{
			const gpmath::StickConfig& s = GetActiveStickConfig(prof);
			const LPCSTR shapeFull = (s.shape == gpmath::DeadzoneShape::Radial)
			    ? TrCal("ui_gp_eng_cal_shape_radial_full", "Radial (Vector Length)")
			    : TrCal("ui_gp_eng_cal_shape_axial_full", "Axial (Per-Axis Cross)");
			xr_sprintf(rowStr[1], sizeof(rowStr[1]), TrCal("ui_gp_eng_cal_row_shape_fmt", "Deadzone Shape             :  <  %s  >"), shapeFull);
			xr_sprintf(rowStr[2], sizeof(rowStr[2]), TrCal("ui_gp_eng_cal_row_inner_dz_fmt", "Inner Deadzone (ring)      :  <  %.2f  >   (min %.2f .. max %.2f)"),
			           s.deadzone, gpmath::limits::kDeadzoneMin, gpmath::limits::kDeadzoneMax);
			xr_sprintf(rowStr[3], sizeof(rowStr[3]), TrCal("ui_gp_eng_cal_row_outer_sat_fmt", "Outer Saturation (ring)    :  <  %.2f  >   (min %.2f .. max %.2f)"),
			           s.saturation, s.deadzone + gpmath::limits::kMinSpan, gpmath::limits::kSaturationMax);
			xr_sprintf(rowStr[4], sizeof(rowStr[4]), TrCal("ui_gp_eng_cal_row_curve_fmt", "Response Curve             :  <  %s  >"),
			           GetLocalizedCurveShort(s.curve.type));

			if (s.curve.type == gpmath::CurveType::Power)
				xr_sprintf(rowStr[5], sizeof(rowStr[5]), TrCal("ui_gp_eng_cal_row_gamma_fmt", "Curve Exponent (gamma)     :  <  %.2f  >   (0.50 .. 4.00)"), s.curve.gamma);
			else if (s.curve.type == gpmath::CurveType::SCurve)
				xr_sprintf(rowStr[5], sizeof(rowStr[5]), TrCal("ui_gp_eng_cal_row_sexp_fmt", "S-Curve Exponent           :  <  %.2f  >   (0.50 .. 4.00)"), s.curve.s_exponent);
			else if (s.curve.type == gpmath::CurveType::TwoZone)
				xr_sprintf(rowStr[5], sizeof(rowStr[5]), TrCal("ui_gp_eng_cal_row_knee_in_fmt", "TwoZone Knee Input (X)     :  <  %.2f  >   (0.05 .. 0.95)"), s.curve.knee_in);
			else
				xr_strcpy(rowStr[5], sizeof(rowStr[5]), TrCal("ui_gp_eng_cal_row_linear_param", "Curve Parameter            :  (Linear f(t) = t)"));

			if (s.curve.type == gpmath::CurveType::TwoZone)
				xr_sprintf(rowStr[6], sizeof(rowStr[6]), TrCal("ui_gp_eng_cal_row_knee_out_fmt", "TwoZone Knee Output (Y)    :  <  %.2f  >   (0.05 .. 0.95)"), s.curve.knee_out);
			else if (m_activeTab == 1)
				xr_sprintf(rowStr[6], sizeof(rowStr[6]), TrCal("ui_gp_eng_cal_row_omega_fmt", "Look Speed (Omega H / V)   :  <  %.2f / %.2f rad/s  >"), prof.omega_horz, prof.omega_vert);
			else
				xr_strcpy(rowStr[6], sizeof(rowStr[6]), TrCal("ui_gp_eng_cal_row_sec_none", "Secondary Curve Param      :  ---"));

			const LPCSTR strOn = TrCal("ui_gp_eng_cal_on", "ON");
			const LPCSTR strOff = TrCal("ui_gp_eng_cal_off", "OFF");
			xr_sprintf(rowStr[7], sizeof(rowStr[7]), TrCal("ui_gp_eng_cal_row_invert_fmt", "Axis Inversion (X / Y)     :  Left: Invert X [%s]  |  Right: Invert Y [%s]"),
			           s.invert_x ? strOn : strOff, s.invert_y ? strOn : strOff);
			xr_sprintf(rowStr[8], sizeof(rowStr[8]), TrCal("ui_gp_eng_cal_row_reset_fmt", "[ Action: Press A / Enter or X to Reset %s to Default ]"), tabNames[m_activeTab]);
		}
		else
		{
			xr_sprintf(rowStr[1], sizeof(rowStr[1]), TrCal("ui_gp_eng_cal_row_lt_dz_fmt", "LT Inner Deadzone          :  <  %.2f  >"), prof.left_trigger.deadzone);
			xr_sprintf(rowStr[2], sizeof(rowStr[2]), TrCal("ui_gp_eng_cal_row_lt_sat_fmt", "LT Outer Saturation        :  <  %.2f  >"), prof.left_trigger.saturation);
			xr_sprintf(rowStr[3], sizeof(rowStr[3]), TrCal("ui_gp_eng_cal_row_lt_press_fmt", "LT Digital Press Thresh    :  <  %.2f  >   (hysteresis press)"), prof.left_trigger.press);
			xr_sprintf(rowStr[4], sizeof(rowStr[4]), TrCal("ui_gp_eng_cal_row_lt_rel_fmt", "LT Digital Release Thresh  :  <  %.2f  >   (<= press - 0.05)"), prof.left_trigger.release);
			xr_sprintf(rowStr[5], sizeof(rowStr[5]), TrCal("ui_gp_eng_cal_row_rt_dz_fmt", "RT Inner Deadzone          :  <  %.2f  >"), prof.right_trigger.deadzone);
			xr_sprintf(rowStr[6], sizeof(rowStr[6]), TrCal("ui_gp_eng_cal_row_rt_sat_fmt", "RT Outer Saturation        :  <  %.2f  >"), prof.right_trigger.saturation);
			xr_sprintf(rowStr[7], sizeof(rowStr[7]), TrCal("ui_gp_eng_cal_row_rt_press_fmt", "RT Digital Press Thresh    :  <  %.2f  >   (hysteresis press)"), prof.right_trigger.press);
			xr_sprintf(rowStr[8], sizeof(rowStr[8]), TrCal("ui_gp_eng_cal_row_rt_rel_fmt", "RT Digital Release Thresh  :  <  %.2f  >   (<= press - 0.05)"), prof.right_trigger.release);
		}

		for (int i = 0; i < NUM_ROWS; ++i)
		{
			const bool sel = (i == m_selectedRow);
			string512 fullRow;
			xr_sprintf(fullRow, sizeof(fullRow), "%s %s", sel ? ">>" : "  ", rowStr[i]);
			m_pRows[i]->SetText(fullRow);
			m_pRows[i]->SetTextColor(sel ? color_argb(255, 255, 225, 95) : color_argb(255, 205, 215, 225));
		}

		string512 footBuf;
		xr_sprintf(footBuf, sizeof(footBuf),
		           TrCal("ui_gp_eng_cal_footer_fmt", "File: %s | Yellow=Raw Stick, Green=Processed Stick, Orange=Deadzone Ring, Cyan=Saturation Ring"),
		           gpmath::GetCalibrationProfilePath());
		m_pFooter->SetText(footBuf);
	}

	// Screen-scaled primitive drawing helpers using UIRender and m_shaderWire ("hud\crosshair")
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

	void PushCirclePx(const Fvector2& centerPx, float radiusPx, int segments, u32 color)
	{
		if (radiusPx <= 0.5f)
			return;
		const float stepRad = (2.0f * PI) / static_cast<float>(segments);
		for (int i = 0; i < segments; ++i)
		{
			const float a0 = stepRad * static_cast<float>(i);
			const float a1 = stepRad * static_cast<float>(i + 1);
			const float x0 = centerPx.x + std::cos(a0) * radiusPx;
			const float y0 = centerPx.y + std::sin(a0) * radiusPx;
			const float x1 = centerPx.x + std::cos(a1) * radiusPx;
			const float y1 = centerPx.y + std::sin(a1) * radiusPx;
			UIRender->PushPoint(x0, y0, 0.0f, color, 0.0f, 0.0f);
			UIRender->PushPoint(x1, y1, 0.0f, color, 0.0f, 0.0f);
		}
	}

	void PushDotPx(float cxPx, float cyPx, float halfSizePx, u32 color)
	{
		UIRender->PushPoint(cxPx - halfSizePx, cyPx - halfSizePx, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(cxPx + halfSizePx, cyPx - halfSizePx, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(cxPx + halfSizePx, cyPx + halfSizePx, 0.0f, color, 0.0f, 0.0f);

		UIRender->PushPoint(cxPx - halfSizePx, cyPx - halfSizePx, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(cxPx + halfSizePx, cyPx + halfSizePx, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(cxPx - halfSizePx, cyPx + halfSizePx, 0.0f, color, 0.0f, 0.0f);
	}

	void DrawStickVisualizerLines(float uiCenterX, float uiCenterY, float uiRadius,
	                              const gpmath::StickConfig& cfg,
	                              float rawX, float rawY,
	                              const gpmath::Vec2& proc)
	{
		Fvector2 centerPx;
		UI().ClientToScreenScaled(centerPx, uiCenterX, uiCenterY);
		float rPx = uiRadius;
		UI().ClientToScreenScaledHeight(rPx);

		// Crosshairs
		UIRender->PushPoint(centerPx.x - rPx, centerPx.y, 0.0f, color_argb(110, 110, 125, 145), 0.0f, 0.0f);
		UIRender->PushPoint(centerPx.x + rPx, centerPx.y, 0.0f, color_argb(110, 110, 125, 145), 0.0f, 0.0f);
		UIRender->PushPoint(centerPx.x, centerPx.y - rPx, 0.0f, color_argb(110, 110, 125, 145), 0.0f, 0.0f);
		UIRender->PushPoint(centerPx.x, centerPx.y + rPx, 0.0f, color_argb(110, 110, 125, 145), 0.0f, 0.0f);

		// Outer unit circle (|v| = 1.0)
		PushCirclePx(centerPx, rPx, 36, color_argb(220, 150, 170, 200));

		// Saturation ring (|v| = cfg.saturation)
		const float satPx = rPx * clampr(cfg.saturation, 0.05f, 1.0f);
		PushCirclePx(centerPx, satPx, 36, color_argb(215, 70, 205, 235));

		// Deadzone ring (radial circle or axial cross-band)
		const float dzPx = rPx * clampr(cfg.deadzone, 0.0f, 0.90f);
		const u32 dzColor = color_argb(235, 240, 145, 55);
		if (cfg.shape == gpmath::DeadzoneShape::Axial)
		{
			// Axial deadzone bands along X and Y axes
			UIRender->PushPoint(centerPx.x - dzPx, centerPx.y - satPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x - dzPx, centerPx.y + satPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x + dzPx, centerPx.y - satPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x + dzPx, centerPx.y + satPx, 0.0f, dzColor, 0.0f, 0.0f);

			UIRender->PushPoint(centerPx.x - satPx, centerPx.y - dzPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x + satPx, centerPx.y - dzPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x - satPx, centerPx.y + dzPx, 0.0f, dzColor, 0.0f, 0.0f);
			UIRender->PushPoint(centerPx.x + satPx, centerPx.y + dzPx, 0.0f, dzColor, 0.0f, 0.0f);
		}
		else
		{
			PushCirclePx(centerPx, dzPx, 32, dzColor);
		}

		// Raw stick vector (note: screen Y goes down, stick Y > 0 goes up)
		const float rawEndX = centerPx.x + clampr(rawX, -1.0f, 1.0f) * rPx;
		const float rawEndY = centerPx.y - clampr(rawY, -1.0f, 1.0f) * rPx;
		UIRender->PushPoint(centerPx.x, centerPx.y, 0.0f, color_argb(220, 245, 215, 70), 0.0f, 0.0f);
		UIRender->PushPoint(rawEndX, rawEndY, 0.0f, color_argb(220, 245, 215, 70), 0.0f, 0.0f);

		// Processed stick vector
		const float procEndX = centerPx.x + clampr(proc.x, -1.0f, 1.0f) * rPx;
		const float procEndY = centerPx.y - clampr(proc.y, -1.0f, 1.0f) * rPx;
		UIRender->PushPoint(centerPx.x, centerPx.y, 0.0f, color_argb(255, 75, 250, 130), 0.0f, 0.0f);
		UIRender->PushPoint(procEndX, procEndY, 0.0f, color_argb(255, 75, 250, 130), 0.0f, 0.0f);
	}

	void DrawVectorScene()
	{
		if (!m_shaderWire->inited())
			m_shaderWire->create("hud\\crosshair");

		const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
		const gpmath::StickConfig& curStk = GetActiveStickConfig(prof);

		// Pass 1: Filled Triangles (backdrop, panels, trigger bar fills, selected row bar, stick/curve dots)
		UIRender->StartPrimitive(240, IUIRender::ptTriList, UI().m_currentPointType);

		// Fullscreen dark backdrop + main dialog panel
		PushFilledRectUI(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT, color_argb(235, 10, 12, 16));
		PushFilledRectUI(56.0f, 20.0f, 968.0f, 744.0f, color_argb(245, 20, 24, 32));

		// Curve preview box background
		const float crvX1 = 432.0f, crvY1 = 126.0f, crvX2 = 592.0f, crvY2 = 286.0f;
		PushFilledRectUI(crvX1, crvY1, crvX2, crvY2, color_argb(220, 14, 18, 24));

		// LT & RT Trigger bar backgrounds + raw/processed fills
		const float ltBarX1 = 92.0f, ltBarY1 = 378.0f, ltBarW = 380.0f, ltBarH = 22.0f;
		const float rtBarX1 = 552.0f, rtBarY1 = 378.0f, rtBarW = 380.0f, rtBarH = 22.0f;

		PushFilledRectUI(ltBarX1, ltBarY1, ltBarX1 + ltBarW, ltBarY1 + ltBarH, color_argb(230, 14, 16, 22));
		PushFilledRectUI(ltBarX1, ltBarY1 + 2.0f, ltBarX1 + ltBarW * clampr(m_rawLT, 0.0f, 1.0f), ltBarY1 + 10.0f,
		                 color_argb(200, 220, 190, 70));
		PushFilledRectUI(ltBarX1, ltBarY1 + 11.0f, ltBarX1 + ltBarW * clampr(m_procLT, 0.0f, 1.0f), ltBarY1 + ltBarH - 2.0f,
		                 m_bLtPressed ? color_argb(240, 75, 245, 130) : color_argb(220, 65, 180, 235));

		PushFilledRectUI(rtBarX1, rtBarY1, rtBarX1 + rtBarW, rtBarY1 + rtBarH, color_argb(230, 14, 16, 22));
		PushFilledRectUI(rtBarX1, rtBarY1 + 2.0f, rtBarX1 + rtBarW * clampr(m_rawRT, 0.0f, 1.0f), rtBarY1 + 10.0f,
		                 color_argb(200, 220, 190, 70));
		PushFilledRectUI(rtBarX1, rtBarY1 + 11.0f, rtBarX1 + rtBarW * clampr(m_procRT, 0.0f, 1.0f), rtBarY1 + rtBarH - 2.0f,
		                 m_bRtPressed ? color_argb(240, 75, 245, 130) : color_argb(220, 65, 180, 235));

		// Selected parameter row highlight bar
		const float selRowY = 416.0f + m_selectedRow * 29.0f;
		PushFilledRectUI(128.0f, selRowY, 896.0f, selRowY + 25.0f, color_argb(160, 45, 65, 95));

		// Stick vector endpoint dots (Left & Right sticks)
		{
			Fvector2 cLPx, cRPx;
			UI().ClientToScreenScaled(cLPx, 230.0f, 206.0f);
			UI().ClientToScreenScaled(cRPx, 794.0f, 206.0f);
			float rPx = 86.0f;
			UI().ClientToScreenScaledHeight(rPx);
			float dotScaled = 3.5f;
			UI().ClientToScreenScaledHeight(dotScaled);
			const float dotR = _max(2.5f, dotScaled);

			PushDotPx(cLPx.x + m_rawLX * rPx, cLPx.y - m_rawLY * rPx, dotR, color_argb(240, 250, 215, 65));
			PushDotPx(cLPx.x + m_procL.x * rPx, cLPx.y - m_procL.y * rPx, dotR + 1.0f, color_argb(255, 75, 255, 135));

			PushDotPx(cRPx.x + m_rawRX * rPx, cRPx.y - m_rawRY * rPx, dotR, color_argb(240, 250, 215, 65));
			PushDotPx(cRPx.x + m_procR.x * rPx, cRPx.y - m_procR.y * rPx, dotR + 1.0f, color_argb(255, 75, 255, 135));
		}

		UIRender->SetShader(*m_shaderWire);
		UIRender->FlushPrimitive();

		// Pass 2: Vector Lines (stick circles, deadzone/saturation rings, vectors, curve graph, trigger threshold ticks)
		UIRender->StartPrimitive(1024, IUIRender::ptLineList, UI().m_currentPointType);

		PushBoxOutlineUI(56.0f, 20.0f, 968.0f, 744.0f, color_argb(220, 95, 120, 155));
		PushBoxOutlineUI(crvX1, crvY1, crvX2, crvY2, color_argb(180, 95, 115, 140));
		PushLineUI(crvX1, crvY2, crvX2, crvY1, color_argb(90, 120, 130, 145)); // Linear reference diagonal

		// Plot active stick response curve y = ApplyCurve(curStk.curve, t)
		constexpr int CURVE_SEGMENTS = 24;
		for (int i = 0; i < CURVE_SEGMENTS; ++i)
		{
			const float t0 = static_cast<float>(i) / static_cast<float>(CURVE_SEGMENTS);
			const float t1 = static_cast<float>(i + 1) / static_cast<float>(CURVE_SEGMENTS);
			const float y0 = gpmath::ApplyCurve(curStk.curve, t0);
			const float y1 = gpmath::ApplyCurve(curStk.curve, t1);
			PushLineUI(crvX1 + t0 * (crvX2 - crvX1), crvY2 - y0 * (crvY2 - crvY1),
			           crvX1 + t1 * (crvX2 - crvX1), crvY2 - y1 * (crvY2 - crvY1),
			           color_argb(255, 95, 235, 165));
		}

		// Left & Right Stick circles + deadzone/saturation rings + raw/processed vectors
		DrawStickVisualizerLines(230.0f, 206.0f, 86.0f, prof.left_stick, m_rawLX, m_rawLY, m_procL);
		DrawStickVisualizerLines(794.0f, 206.0f, 86.0f, prof.right_stick, m_rawRX, m_rawRY, m_procR);

		// Trigger bars outline + threshold ticks (Deadzone, Release, Press, Saturation)
		auto drawTriggerTicks = [&](float bx, float by, float bw, float bh, const gpmath::TriggerConfig& tc)
		{
			PushBoxOutlineUI(bx, by, bx + bw, by + bh, color_argb(200, 120, 140, 165));
			const float xDz = bx + bw * clampr(tc.deadzone, 0.0f, 1.0f);
			const float xRel = bx + bw * clampr(tc.release, 0.0f, 1.0f);
			const float xPrs = bx + bw * clampr(tc.press, 0.0f, 1.0f);
			const float xSat = bx + bw * clampr(tc.saturation, 0.0f, 1.0f);

			PushLineUI(xDz, by - 3.0f, xDz, by + bh + 3.0f, color_argb(245, 240, 145, 55));   // Deadzone (orange)
			PushLineUI(xRel, by - 3.0f, xRel, by + bh + 3.0f, color_argb(245, 80, 210, 250)); // Release (cyan)
			PushLineUI(xPrs, by - 4.0f, xPrs, by + bh + 4.0f, color_argb(255, 255, 95, 95));  // Press (red)
			PushLineUI(xSat, by - 3.0f, xSat, by + bh + 3.0f, color_argb(245, 90, 245, 130)); // Saturation (green)
		};

		drawTriggerTicks(ltBarX1, ltBarY1, ltBarW, ltBarH, prof.left_trigger);
		drawTriggerTicks(rtBarX1, rtBarY1, rtBarW, rtBarH, prof.right_trigger);

		UIRender->SetShader(*m_shaderWire);
		UIRender->FlushPrimitive();
	}

	ui_shader m_shaderWire;
	u32 m_pauseTokenId = 0;
	u32 m_lastUpdateTimeContinual = 0;
	u32 m_prevButtonsMask = 0;
	EDir m_heldDir = DirNone;
	u32 m_nextRepeatTimeMs = 0;

	int m_activeTab = 0;
	int m_selectedRow = 2; // Start on Inner Deadzone row

	float m_rawLX = 0.0f;
	float m_rawLY = 0.0f;
	float m_rawRX = 0.0f;
	float m_rawRY = 0.0f;
	float m_rawLT = 0.0f;
	float m_rawRT = 0.0f;
	gpmath::Vec2 m_procL{ 0.0f, 0.0f };
	gpmath::Vec2 m_procR{ 0.0f, 0.0f };
	float m_procLT = 0.0f;
	float m_procRT = 0.0f;
	bool m_bLtPressed = false;
	bool m_bRtPressed = false;

	CUITextWnd* m_pTitle = nullptr;
	CUITextWnd* m_pSubtitle = nullptr;
	CUITextWnd* m_pLeftStickTitle = nullptr;
	CUITextWnd* m_pLeftStickReadout = nullptr;
	CUITextWnd* m_pCurveTitle = nullptr;
	CUITextWnd* m_pCurveReadout = nullptr;
	CUITextWnd* m_pRightStickTitle = nullptr;
	CUITextWnd* m_pRightStickReadout = nullptr;
	CUITextWnd* m_pLtLabel = nullptr;
	CUITextWnd* m_pRtLabel = nullptr;
	CUITextWnd* m_pRows[NUM_ROWS] = {};
	CUITextWnd* m_pFooter = nullptr;
};

CUIGamepadCalibrationWnd* g_pGamepadCalibWnd = nullptr;

CDialogHolder* GetSafeActiveDialogHolderForCalib()
{
	if (MainMenu() && MainMenu()->IsActive())
		return MainMenu();
	if (g_pGameLevel && CurrentGameUI())
		return CurrentGameUI();
	return nullptr;
}
} // namespace

CGamepadCalibrationUI& CGamepadCalibrationUI::Instance()
{
	static CGamepadCalibrationUI instance;
	return instance;
}

void CGamepadCalibrationUI::Open(LPCSTR sourceTag)
{
	const bool inMainMenu = (MainMenu() && MainMenu()->IsActive());
	if (!inMainMenu && (!g_pGameLevel || !CurrentGameUI()))
	{
		Msg("! [Gamepad] gp_calib open (%s): neither MainMenu nor GameUI is active", sourceTag ? sourceTag : "cmd");
		return;
	}

	if (!g_pGamepadCalibWnd)
	{
		g_pGamepadCalibWnd = xr_new<CUIGamepadCalibrationWnd>();
	}

	// CR1-09: recover if CDialogHolder::CleanInternals cleared holder stack while leaving IsShown() == true
	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForCalib();
	if (g_pGamepadCalibWnd->IsShown() &&
	    (g_pGamepadCalibWnd->GetHolder() != activeHolder || !activeHolder ||
	     activeHolder->TopInputReceiver() != g_pGamepadCalibWnd))
	{
		g_pGamepadCalibWnd->SetHolder(nullptr);
		g_pGamepadCalibWnd->Show(false);
	}

	if (!g_pGamepadCalibWnd->IsShown())
	{
		g_pGamepadCalibWnd->SetHolder(nullptr);
		g_pGamepadCalibWnd->ShowDialog(true);
	}
}

void CGamepadCalibrationUI::Close(LPCSTR reason)
{
	(void)reason;
	if (!g_pGamepadCalibWnd || !g_pGamepadCalibWnd->IsShown())
		return;

	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForCalib();
	if (g_pGamepadCalibWnd->GetHolder() && g_pGamepadCalibWnd->GetHolder() == activeHolder)
	{
		g_pGamepadCalibWnd->HideDialog();
	}
	if (g_pGamepadCalibWnd->IsShown())
	{
		g_pGamepadCalibWnd->SetHolder(nullptr);
		g_pGamepadCalibWnd->Show(false);
	}
}

void CGamepadCalibrationUI::Toggle(LPCSTR sourceTag)
{
	if (IsOpen())
		Close(sourceTag);
	else
		Open(sourceTag);
}

bool CGamepadCalibrationUI::IsOpen() const
{
	if (!g_pGamepadCalibWnd || !g_pGamepadCalibWnd->IsShown())
		return false;
	CDialogHolder* activeHolder = GetSafeActiveDialogHolderForCalib();
	return activeHolder && g_pGamepadCalibWnd->GetHolder() == activeHolder &&
	       activeHolder->TopInputReceiver() == g_pGamepadCalibWnd;
}

void CGamepadCalibrationUI::LogStatus() const
{
	gpmath::LogCalibrationProfileStatus();
	Msg("* [Gamepad] gp_calib ui_open=%d", IsOpen() ? 1 : 0);
}

bool CGamepadCalibrationUI::RunSelfTest()
{
	return gpmath::RunCalibrationSelfTest();
}
