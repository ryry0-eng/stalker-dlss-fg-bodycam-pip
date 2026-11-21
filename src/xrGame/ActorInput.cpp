#include "stdafx.h"
#include <dinput.h>
#include "Actor.h"
#include "Torch.h"
#include "trade.h"
#include "../xrEngine/CameraBase.h"

#ifdef DEBUG
#	include "PHDebug.h"
#endif

#include "hit.h"
#include "PHDestroyable.h"
#include "UIGameSP.h"
#include "inventory.h"
#include "level.h"
#include "game_cl_base.h"
#include "xr_level_controller.h"
#include "UsableScriptObject.h"
#include "actorcondition.h"
#include "actor_input_handler.h"
#include "string_table.h"
#include "UI/UIStatic.h"
#include "UI/UIActorMenu.h"
#include "UI/UIDragDropReferenceList.h"
#include "CharacterPhysicsSupport.h"
#include "InventoryBox.h"
#include "player_hud.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/GamepadMath.h"
#include "../xrEngine/GamepadErrorGuard.h"
#include "../xrEngine/XR_IOConsole.h"
#include "GamepadContextDispatcher.h"
#include "GamepadAimAssist.h"
#include "flare.h"
#include "CustomDetector.h"
#include "clsid_game.h"
#include "hudmanager.h"
#include "Weapon.h"
#include "Flashlight.h"
#include "../xrPhysics/IElevatorState.h"
#include "holder_custom.h"
#include "Weapon.h"
#include "CustomOutfit.h"

extern u32 hud_adj_mode;

void CActor::IR_OnKeyboardPress(int cmd)
{
	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT)) return;

	if (Remote()) return;

	if (IsTalking()) return;
	if (m_input_external_handler && !m_input_external_handler->authorized(cmd)) return;

	switch (cmd)
	{
	case kWPN_FIRE:
		{
			if ((mstate_wishful & mcLookout) && !IsGameTypeSingle()) return;

			// Tronex: export to allow/prevent weapon fire if returned false
			::luabind::functor<bool> funct;
			if (ai().script_engine().functor("_G.CActor_Fire", funct))
			{
				if (!funct())
				{
					return;
				}
			}

			//-----------------------------
			if (OnServer())
			{
				NET_Packet P;
				P.w_begin(M_PLAYER_FIRE);
				P.w_u16(ID());
				u_EventSend(P);
			}
		}
		break;
	default:
		{
		}
		break;
	}

	if (!g_Alive()) return;

	if (m_holder && kUSE != cmd)
	{
		m_holder->OnKeyboardPress(cmd);
		if (m_holder->allowWeapon() && inventory().Action((u16)cmd, CMD_START)) return;
		return;
	}
	else
		if (inventory().Action((u16)cmd, CMD_START)) return;

#ifdef DEBUG
	if(psActorFlags.test(AF_NO_CLIP))
	{
		NoClipFly(cmd);
		return;
	}
#endif //DEBUG
	switch (cmd)
	{
	case kJUMP:
		{
			mstate_wishful |= mcJump;
		}
		break;
	case kSPRINT_TOGGLE:
		{
			if (psActorFlags.test(AF_SPRINT_TOGGLE))
			{
				if (psActorFlags.test(AF_WALK_TOGGLE)) mstate_wishful &= ~mcAccel;
				if (psActorFlags.test(AF_CROUCH_TOGGLE)) mstate_wishful &= ~mcCrouch;
				mstate_wishful ^= mcSprint;
			}
		}
		break;
	case kFREELOOK:
	{
		if (psActorFlags.test(AF_FREELOOK_TOGGLE))
		{
			if (cam_freelook == eflDisabled && CanUseFreelook())
			{
				cam_SetFreelook();
			}
			else if (cam_freelook == eflEnabled)
			{
				cam_UnsetFreelook();
			}
		}
	}
	break;
	case kCROUCH:
		{
			if (psActorFlags.test(AF_CROUCH_TOGGLE))
				mstate_wishful ^= mcCrouch;
		}
		break;
	case kACCEL:
		{
			if (psActorFlags.test(AF_WALK_TOGGLE))
				mstate_wishful ^= mcAccel;
		}
		break;
	case kL_LOOKOUT:
		{
			if (psActorFlags.test(AF_LOOKOUT_TOGGLE))
			{
				mstate_wishful &= ~mcRLookout;
				mstate_wishful ^= mcLLookout;
			}
		}
		break;
	case kR_LOOKOUT:
		{
			if (psActorFlags.test(AF_LOOKOUT_TOGGLE))
			{
				mstate_wishful &= ~mcLLookout;
				mstate_wishful ^= mcRLookout;
			}
		}
		break;
	case kCAM_1: cam_Set(eacFirstEye);
		break;
	case kCAM_2: cam_Set(eacLookAt);
		break;
	case kCAM_3: cam_Set(eacFreeLook);
		break;
	case kNIGHT_VISION:
		{
			//SwitchNightVision(); //Rezy: now it's controlled via LUA scripts for timing and animations
			break;
		}
	case kTORCH:
		{
			//SwitchTorch(); //Tronex: now it's controlled via LUA scripts for timing and animations
			break;
		}

	case kDETECTOR:
		{
			PIItem dev_active = inventory().ItemFromSlot(DETECTOR_SLOT);
			if (dev_active)
			{
				CCustomDevice* dev = smart_cast<CCustomDevice*>(dev_active);
				if (dev)
					dev->ToggleDevice(g_player_hud->attached_item(0) != NULL);
			}
		}
		break;
		/*
			case kFLARE:{
					PIItem fl_active = inventory().ItemFromSlot(FLARE_SLOT);
					if(fl_active)
					{
						CFlare* fl			= smart_cast<CFlare*>(fl_active);
						fl->DropFlare		();
						return				;
					}
		
					PIItem fli = inventory().Get(CLSID_DEVICE_FLARE, true);
					if(!fli)			return;
		
					CFlare* fl			= smart_cast<CFlare*>(fli);
					
					if(inventory().Slot(fl))
						fl->ActivateFlare	();
				}break;
		*/
	case kUSE:
		ActorUse();
		break;
	case kDROP:
		b_DropActivated = TRUE;
		f_DropPower = 0;
		break;
	case kNEXT_SLOT:
		{
			OnNextWeaponSlot();
		}
		break;
	case kPREV_SLOT:
		{
			OnPrevWeaponSlot();
		}
		break;

	case kQUICK_USE_1:
	case kQUICK_USE_2:
	case kQUICK_USE_3:
	case kQUICK_USE_4:
		{
			const shared_str& item_name = g_quick_use_slots[cmd - kQUICK_USE_1];
			if (item_name.size())
			{
				PIItem itm = inventory().GetAny(item_name.c_str());

				::luabind::functor<bool> funct;
				if (itm && ai().script_engine().functor("_G.CInventory__eat", funct))
				{
					CGameObject* GO = itm->cast_game_object();
					if (GO && funct(GO->lua_game_object()))
					{
						if (IsGameTypeSingle())
						{
							inventory().Eat(itm);
						}
						else
						{
							inventory().ClientEat(itm);
						}

						StaticDrawableWrapper* _s = CurrentGameUI()->AddCustomStatic("item_used", true);
						string1024 str;
						strconcat(sizeof(str), str, *CStringTable().translate("st_item_used"), ": ", itm->NameItem());
						_s->wnd()->TextItemControl()->SetText(str);

						//CurrentGameUI()->GetActorMenu().m_pQuickSlot->ReloadReferences(this);
					}
				}
			}
		}
		break;
	}
}

// demonized: switch to disable mouse wheel weapon change
BOOL mouseWheelChangeWeapon = TRUE;

// demonized: switch to invert mouse wheel weapon change
BOOL mouseWheelInvertChangeWeapons = FALSE;

// mbehm: switch to allow inverting mouse wheel zoom direction
BOOL mouseWheelInvertZoom = FALSE;
void CActor::IR_OnMouseWheel(int direction)
{
	if (hud_adj_mode)
	{
		g_player_hud->tune(Ivector().set(0, 0, direction));
		return;
	}

#ifdef CAR_NEW
    if (m_holder)
    {
        CCar* car = smart_cast<CCar*>(m_holder);
        if (car && car->IsScopeEnable())
        {
            car->ScopeOnMouseWheel(direction);
            return;
        }
    }
#endif

	if (mouseWheelInvertZoom) {
		if (inventory().Action((direction > 0) ? (u16)kWPN_ZOOM_DEC : (u16)kWPN_ZOOM_INC, CMD_START)) return;
	} else {
		if (inventory().Action((direction > 0) ? (u16)kWPN_ZOOM_INC : (u16)kWPN_ZOOM_DEC, CMD_START)) return;
	}

	if (mouseWheelChangeWeapon) {
		if (mouseWheelInvertChangeWeapons) {
			if (direction < 0)
				OnNextWeaponSlot();
			else
				OnPrevWeaponSlot();
		} else {
			if (direction > 0)
				OnNextWeaponSlot();
			else
				OnPrevWeaponSlot();
		}
	}
}

void CActor::IR_OnKeyboardRelease(int cmd)
{
	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT)) return;

	if (Remote()) return;

	if (m_input_external_handler && !m_input_external_handler->authorized(cmd)) return;

	if (g_Alive())
	{
		if (cmd == kUSE && !psActorFlags.test(AF_MULTI_ITEM_PICKUP))
			m_bPickupMode = false;

		if (m_holder)
		{
			m_holder->OnKeyboardRelease(cmd);

			if (m_holder->allowWeapon() && inventory().Action((u16)cmd, CMD_STOP)) return;
			return;
		}
		else
			if (inventory().Action((u16)cmd, CMD_STOP)) return;


		switch (cmd)
		{
		case kJUMP: mstate_wishful &= ~mcJump;
			break;
		case kDROP: if (GAME_PHASE_INPROGRESS == Game().Phase()) g_PerformDrop();
			break;
		}
	}
}

void CActor::IR_OnKeyboardHold(int cmd)
{
	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT)) return;

	if (Remote() || !g_Alive()) return;
	if (m_input_external_handler && !m_input_external_handler->authorized(cmd)) return;
	if (IsTalking()) return;

	if (m_holder)
	{
		m_holder->OnKeyboardHold(cmd);
		return;
	}

#ifdef DEBUG
	if(psActorFlags.test(AF_NO_CLIP) && (cmd==kFWD || cmd==kBACK || cmd==kL_STRAFE || cmd==kR_STRAFE 
		|| cmd==kJUMP || cmd==kCROUCH))
	{
		NoClipFly(cmd);
		return;
	}
#endif //DEBUG
	float LookFactor = GetLookFactor();
	switch (cmd)
	{
	case kUP:
	case kDOWN:
		if(cam_freelook != eflEnabling && cam_freelook != eflDisabling) cam_Active()->Move((cmd == kUP) ? kDOWN : kUP, 0, LookFactor);
		break;
	case kCAM_ZOOM_IN:
	case kCAM_ZOOM_OUT:
		cam_Active()->Move(cmd);
		break;
	case kLEFT:
	case kRIGHT:
		if (eacFreeLook != cam_active && cam_freelook != eflEnabling && cam_freelook != eflDisabling) cam_Active()->Move(cmd, 0, LookFactor);
		break;
	case kL_STRAFE: mstate_wishful |= mcLStrafe;
		break;
	case kR_STRAFE: mstate_wishful |= mcRStrafe;
		break;
	case kL_LOOKOUT:
		{
			if (!psActorFlags.test(AF_LOOKOUT_TOGGLE) && cam_freelook == eflDisabled)
				mstate_wishful |= mcLLookout;
		}
		break;
	case kR_LOOKOUT:
		{
			if (!psActorFlags.test(AF_LOOKOUT_TOGGLE) && cam_freelook == eflDisabled)
				mstate_wishful |= mcRLookout;
		}
		break;
	case kFWD: mstate_wishful |= mcFwd;
		break;
	case kBACK: mstate_wishful |= mcBack;
		break;
	case kCROUCH:
		{
			if (!psActorFlags.test(AF_CROUCH_TOGGLE))
				mstate_wishful |= mcCrouch;
		}
		break;
	case kACCEL:
		{
			if (!psActorFlags.test(AF_WALK_TOGGLE))
				mstate_wishful |= mcAccel;
		}
	break;
	case kFREELOOK:
	{
		if (!psActorFlags.test(AF_FREELOOK_TOGGLE))
		{
			if (cam_freelook == eflDisabled && CanUseFreelook())
			{
				cam_SetFreelook();
			}
		}
	}
	break;
	case kSPRINT_TOGGLE:
		{
			if (!psActorFlags.test(AF_SPRINT_TOGGLE))
			{
				if (psActorFlags.test(AF_WALK_TOGGLE)) mstate_wishful &= ~mcAccel;
				if (psActorFlags.test(AF_CROUCH_TOGGLE)) mstate_wishful &= ~mcCrouch;
				mstate_wishful |= mcSprint;
			}
		}
		break;
	}
}

void CActor::IR_OnMouseMove(int dx, int dy)
{
	if (hud_adj_mode)
	{
		g_player_hud->tune(Ivector().set(dx, dy, 0));
		return;
	}

	PIItem iitem = inventory().ActiveItem();
	if (iitem && iitem->cast_hud_item())
		iitem->cast_hud_item()->ResetSubStateTime();

	if (Remote()) return;

	if (m_holder)
	{
		m_holder->OnMouseMove(dx, dy);
		return;
	}

	if (cam_freelook == eflEnabling || cam_freelook == eflDisabling)
		return;

	if (cam_active == eacFirstEye && cam_freelook == eflDisabled &&
		IsFocused() && g_player_hud && g_player_hud->ReferenceMouse(dx, dy))
		return;

	const float LookFactor = GetLookFactor();

	CCameraBase* C = cameras[cam_active];
	// pip when the SVP is active the main view stays wide, so first-eye sensitivity must track the SVP
	// zoom fov instead of C->f_fov, off (no SVP) keeps the stock C->f_fov so mouse feel is unchanged
	float ratio = C->f_fov / g_fov;
	if (cam_active == eacFirstEye && Device.true_pip_on && Device.m_SecondViewport.IsSVPActive())
	{
		// the curve exponent shapes the zoom response, 1 rides the svp fov, 0 goes flat,
		// the multiplier trims the whole scoped feel
		extern float g_svp_sens, g_svp_sens_curve;
		ratio = powf(currentFOV(true) / g_fov, g_svp_sens_curve) * g_svp_sens;
	}
	float scale = ratio * (psMouseSens * sens_multiple) * psMouseSensScale / 50.f / LookFactor;
	float bodycam_yaw_delta = 0.f;
	float bodycam_pitch_delta = 0.f;
	if (dx)
	{
		float d = float(dx) * scale;
		cam_Active()->Move((d < 0) ? kLEFT : kRIGHT, _abs(d));
		bodycam_yaw_delta = d;
	}
	if (dy)
	{
		float d = ((psMouseInvert.test(1)) ? -1 : 1) * float(dy) * scale * psMouseSensVerticalK * 3.f / 4.f;
		cam_Active()->Move((d > 0) ? kUP : kDOWN, _abs(d));
		bodycam_pitch_delta = d;
	}
	m_bodycam.AddMouseLookDelta(bodycam_yaw_delta, bodycam_pitch_delta);
}

// C3.1 + D2.2: конвейер мёртвой зоны, насыщения и кривой камеры из профиля калибровки (GamepadMath)
constexpr float GAMEPAD_LOOK_OMEGA_HORZ = 4.0f;
constexpr float GAMEPAD_LOOK_OMEGA_VERT = 2.5f;
constexpr float GAMEPAD_LOOK_MIN_DELTA = 1e-6f;

extern bool g_bDisableAllInput;
bool g_gp_synth_look_active = false;
float g_gp_synth_look_rx = 0.f;
float g_gp_synth_look_ry = 0.f;
float g_gp_synth_look_time_left = 0.f;
float g_gp_synth_look_duration = 0.f;
float g_gp_synth_look_accum_yaw = 0.f;
float g_gp_synth_look_accum_pitch = 0.f;
u32 g_gp_synth_look_frames = 0;
static float g_gp_turn_accel_ramp = 0.f; // K6b: плавный множитель ускорения поворота у края правого стика (0..1)

void CActor::IR_OnGamepadLook(float rx, float ry, float dt)
{
	if (dt <= 0.f || hud_adj_mode)
		return;

	PIItem iitem = inventory().ActiveItem();
	if (iitem && iitem->cast_hud_item())
		iitem->cast_hud_item()->ResetSubStateTime();

	if (Remote() || m_holder != nullptr)
		return;

	if (cam_freelook == eflEnabling || cam_freelook == eflDisabling)
		return;

	const float LookFactor = GetLookFactor();
	if (LookFactor <= EPS_S)
		return;

	CCameraBase* C = cameras[cam_active];
	if (!C)
		return;

	const gpmath::CalibrationProfile& calib = gpmath::GetCalibrationProfile();
	const float omegaH = g_gp_synth_look_active ? GAMEPAD_LOOK_OMEGA_HORZ : calib.omega_horz;
	const float omegaV = g_gp_synth_look_active ? GAMEPAD_LOOK_OMEGA_VERT : calib.omega_vert;

	const bool isAds = CGamepadContextDispatcher::Instance().IsGamepadAdsHeld() || IsZoomAimingMode();
	const float k6bScale = g_gp_synth_look_active ? 1.0f : gpmath::ComputeLookSpeedScale(calib.ads_sens_mult, isAds, calib.turn_accel, g_gp_turn_accel_ramp);
	float ratio = C->f_fov / g_fov;
	if (cam_active == eacFirstEye && Device.true_pip_on && Device.m_SecondViewport.IsSVPActive())
	{
		extern float g_svp_sens, g_svp_sens_curve;
		ratio = powf(currentFOV(true) / g_fov, g_svp_sens_curve) * g_svp_sens;
	}
	const float fovScale = ratio * sens_multiple / LookFactor * k6bScale;
	const float dYaw = rx * omegaH * fovScale * dt;
	const float invY = psMouseInvert.test(1) ? -1.f : 1.f;
	const float dPitch = invY * ry * omegaV * fovScale * dt;

	if (_abs(dYaw) >= GAMEPAD_LOOK_MIN_DELTA)
		cam_Active()->Move((dYaw < 0.f) ? kLEFT : kRIGHT, _abs(dYaw));

	if (_abs(dPitch) >= GAMEPAD_LOOK_MIN_DELTA)
		// C3.1b: ry > 0 = стик вверх; в CCameraFirstEye::Move kDOWN поднимает взгляд (как dy < 0 в IR_OnMouseMove:479)
		cam_Active()->Move((dPitch > 0.f) ? kDOWN : kUP, _abs(dPitch));

	// Feed Bodycam sway/inertia identically to IR_OnMouseMove (dPitch > 0 looks up = negative pitch delta)
	m_bodycam.AddMouseLookDelta(
		(_abs(dYaw) >= GAMEPAD_LOOK_MIN_DELTA) ? dYaw : 0.f,
		(_abs(dPitch) >= GAMEPAD_LOOK_MIN_DELTA) ? -dPitch : 0.f);

	if (g_gp_synth_look_active)
	{
		g_gp_synth_look_accum_yaw += dYaw;
		g_gp_synth_look_accum_pitch += dPitch;
		++g_gp_synth_look_frames;
	}
}

// FIX-LIVE5 (Миша 2026-11-16): после колеса / выбора оружия / окна правый стик часто ещё отклонён, и камера
// «улетала» в первый же кадр. Пока поворот камеры заблокирован, взводится барьер; после разблокировки камера ждёт
// возврата RS почти в центр (физический наклон < GP_LOOK_BARRIER_MAG), и только потом снова вращается.
static bool s_gpLookBarrier = false;
constexpr float GP_LOOK_BARRIER_MAG = 0.3f;

void CActor::UpdateGamepadLookInput(float dt)
{
	struct SLookGuardCtx
	{
		CActor* self;
		float dt;
	};
	SLookGuardCtx stepCtx = { this, dt };
	u32 sehCode = 0;
	u64 sehAddr = 0;
	const bool stepOk = gperr::ExecuteProtectedGamepadStepSEH(
	    +[](void* rawCtx)
	    {
		    SLookGuardCtx* c = static_cast<SLookGuardCtx*>(rawCtx);
		    if (!c || !c->self)
			    return;
		    CActor* self = c->self;
		    const float stepDtVal = c->dt;

		    CGamepadContextDispatcher::Instance().OnFrame();
		    if ((Console && Console->bVisible) || !CGamepadContextDispatcher::Instance().AllowsWorldLook())
		    {
			    s_gpLookBarrier = true;
			    return;
		    }

		    if (Device.Paused() || stepDtVal <= 0.f || g_bDisableAllInput || !self->g_Alive() || self->Remote() || self->IsTalking() || self->m_holder != nullptr || hud_adj_mode != 0)
			    return;

		    if (CurrentGameUI())
		    {
			    CUIDialogWnd* pTIR = CurrentGameUI()->TopInputReceiver();
			    if (pTIR && pTIR->IR_process())
			    {
				    s_gpLookBarrier = true;
				    return;
			    }
		    }

		    if (g_gp_synth_look_active)
		    {
			    Fvector2 synth = { g_gp_synth_look_rx, g_gp_synth_look_ry };
			    const float mag = synth.magnitude();
			    if (mag > 1.f)
				    synth.mul(1.f / mag);

			    const float stepDt = (stepDtVal < g_gp_synth_look_time_left) ? stepDtVal : g_gp_synth_look_time_left;
			    if (mag > EPS_S && stepDt > 0.f)
				    self->IR_OnGamepadLook(synth.x, synth.y, stepDt);

			    g_gp_synth_look_time_left -= stepDtVal;
			    if (g_gp_synth_look_time_left <= 0.f)
			    {
				    g_gp_synth_look_active = false;
				    g_gp_synth_look_time_left = 0.f;
				    Msg("* [Gamepad] gp_synth_look done: rx=%.4f ry=%.4f sec=%.4f yaw_rad=%.6f yaw_deg=%.2f pitch_rad=%.6f pitch_deg=%.2f frames=%u sens_multiple=%.4f",
				        g_gp_synth_look_rx, g_gp_synth_look_ry, g_gp_synth_look_duration,
				        g_gp_synth_look_accum_yaw, rad2deg(g_gp_synth_look_accum_yaw),
				        g_gp_synth_look_accum_pitch, rad2deg(g_gp_synth_look_accum_pitch),
				        g_gp_synth_look_frames, sens_multiple);
			    }
			    return;
		    }

		    if (!pInput)
			    return;

		    const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		    if (!gp.connected || !gp.has_focus)
			    return;

		    const float rawX = gpmath::NormalizeStickAxis(gp.axes[2]);
		    const float rawY = -gpmath::NormalizeStickAxis(gp.axes[3]);
		    const gpmath::CalibrationProfile& calibProf = gpmath::GetCalibrationProfile();
		    if (s_gpLookBarrier)
		    {
			    if (rawX * rawX + rawY * rawY >= GP_LOOK_BARRIER_MAG * GP_LOOK_BARRIER_MAG)
			    {
				    gpmath::StepTurnAcceleration(g_gp_turn_accel_ramp, 0.f, calibProf.turn_accel, stepDtVal);
				    return;
			    }
			    s_gpLookBarrier = false;
		    }
		    gpmath::Vec2 lk = gpmath::ProcessStick(calibProf.right_stick, rawX, rawY);
		    const float procMag = _sqrt(lk.x * lk.x + lk.y * lk.y);
		    gpmath::StepTurnAcceleration(g_gp_turn_accel_ramp, procMag, calibProf.turn_accel, stepDtVal);

		    // E4.1: помощь прицеливания. D05: при gp_aa_level=off модуль не вызывается вообще.
		    if (gpaa::g_level != 0)
		    {
			    const gpmath::Vec2 mv = CGamepadContextDispatcher::Instance().AllowsWorldMove()
			                                ? gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick,
			                                                       gpmath::NormalizeStickAxis(gp.axes[0]), -gpmath::NormalizeStickAxis(gp.axes[1]))
			                                : gpmath::Vec2{ 0.f, 0.f };

			    gpaa::FrameInput in;
			    in.dt = stepDtVal;
			    in.lookX = lk.x;
			    in.lookY = lk.y;
			    in.moveMag = _sqrt(mv.x * mv.x + mv.y * mv.y);
			    in.ads = CGamepadContextDispatcher::Instance().IsGamepadAdsHeld() || self->IsZoomAimingMode();
			    in.gamepadSource = pInput->IsGamepadActiveSource();
			    in.actor = self;
			    in.cam = self->cam_Active();
			    in.weapon = smart_cast<CWeapon*>(self->inventory().ActiveItem());
			    in.eligible = self->cam_active == eacFirstEye && self->cam_freelook == eflDisabled && self->m_holder == nullptr && hud_adj_mode == 0;

			    gpaa::FrameOutput out;
			    gpaa::Process(in, out);
			    lk.x = out.lookX;
			    lk.y = out.lookY;
			    self->ApplyGamepadAimAssistDelta(out.addYaw, out.addPitch);
		    }

		    if (_abs(lk.x) <= EPS_S && _abs(lk.y) <= EPS_S)
			    return;

		    self->IR_OnGamepadLook(lk.x, lk.y, stepDtVal);
	    },
	    &stepCtx,
	    sehCode,
	    sehAddr);

	if (!stepOk)
	{
		g_gp_synth_look_active = false;
		g_gp_synth_look_time_left = 0.f;
		m_vGamepadMove.set(0.f, 0.f);
		CGamepadContextDispatcher::Instance().ConsumeAllCurrentlyHeldButtons();
		const LPCSTR ctxName = CGamepadContextDispatcher::GetContextName(CGamepadContextDispatcher::Instance().GetCurrentContext());
		if (pInput)
		{
			pInput->HandleGamepadFrameError("CActor::UpdateGamepadLookInput", nullptr, sehCode, sehAddr, ctxName);
		}
		else
		{
			gperr::HandleProtectedFrameFailure("CActor::UpdateGamepadLookInput", nullptr, sehCode, sehAddr);
		}
	}
}

// E4.1: прямой поворот камеры от помощи прицеливания (рад), без чувствительности, инверсии и FOV-масштаба стика.
void CActor::ApplyGamepadAimAssistDelta(float yaw, float pitch)
{
	if (cam_active != eacFirstEye || cam_freelook != eflDisabled || m_holder != nullptr || hud_adj_mode != 0)
		return;
	CCameraBase* C = cam_Active();
	if (!C || !_valid(yaw) || !_valid(pitch))
		return;
	if (_abs(yaw) >= GAMEPAD_LOOK_MIN_DELTA)
		C->Move((yaw > 0.f) ? kRIGHT : kLEFT, _abs(yaw));
	if (_abs(pitch) >= GAMEPAD_LOOK_MIN_DELTA)
		// C3.1b: kDOWN поднимает взгляд в CCameraFirstEye::Move
		C->Move((pitch > 0.f) ? kDOWN : kUP, _abs(pitch));
}


#include "HudItem.h"

bool CActor::use_Holder(CHolderCustom* holder)
{
	if (m_holder)
	{
		bool b = use_HolderEx(0, false);

		if (inventory().ActiveItem())
		{
			CHudItem* hi = smart_cast<CHudItem*>(inventory().ActiveItem());
			if (hi) hi->OnAnimationEnd(hi->GetState());
		}

		return b;
	}
	else
	{
		bool b = use_HolderEx(holder, false);

		if (b)
		{
			//used succesfully
			// switch off torch...
			CAttachableItem* I = CAttachmentOwner::attachedItem(CLSID_DEVICE_TORCH);
			if (I)
			{
				CTorch* torch = smart_cast<CTorch*>(I);
				if (torch) torch->Switch(false);
			}
		}

		if (inventory().ActiveItem())
		{
			CHudItem* hi = smart_cast<CHudItem*>(inventory().ActiveItem());
			if (hi) hi->OnAnimationEnd(hi->GetState());
		}

		return b;
	}
}

void CActor::ActorUse()
{
	if (m_holder)
	{
		CGameObject* GO = smart_cast<CGameObject*>(m_holder);
		NET_Packet P;
		CGameObject::u_EventGen(P, GEG_PLAYER_DETACH_HOLDER, ID());
		P.w_u16(GO->ID());
		CGameObject::u_EventSend(P);
		return;
	}

	if (!psActorFlags.test(AF_MULTI_ITEM_PICKUP))
		m_bPickupMode = true;

	if (character_physics_support()->movement()->PHCapture())
		character_physics_support()->movement()->PHReleaseObject();


	if (m_pUsableObject && NULL == m_pObjectWeLookingAt->cast_inventory_item())
	{
		m_pUsableObject->use(this);
	}

	if (m_pInvBoxWeLookingAt && m_pInvBoxWeLookingAt->nonscript_usable())
	{
		CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
		if (pGameSP) //single
		{
			if (!m_pInvBoxWeLookingAt->closed())
			{
				pGameSP->StartCarBody(this, m_pInvBoxWeLookingAt);
			}
		}
		return;
	}

	if (!m_pUsableObject || m_pUsableObject->nonscript_usable())
	{
		bool bCaptured = false;

		collide::rq_result& RQ = HUD().GetRQ();
		CPhysicsShellHolder* object = smart_cast<CPhysicsShellHolder*>(RQ.O);
		u16 element = BI_NONE;
		if (object)
			element = (u16)RQ.element;

		if (object && Level().IR_GetKeyState(DIK_LSHIFT))
		{
			bool b_allow = !!pSettings->line_exist("ph_capture_visuals", object->cNameVisual());
			::luabind::functor<bool> funct;
			if (ai().script_engine().functor("_G.CActor__OnBeforePHCapture", funct))
				b_allow = funct(object->lua_game_object(), b_allow);

			if (b_allow && !character_physics_support()->movement()->PHCapture())
			{
				character_physics_support()->movement()->PHCaptureObject(object, element);
				bCaptured = true;
			}
		}
		else
		{
			if (object && smart_cast<CHolderCustom*>(object))
			{
				NET_Packet P;
				CGameObject::u_EventGen(P, GEG_PLAYER_ATTACH_HOLDER, ID());
				P.w_u16(object->ID());
				CGameObject::u_EventSend(P);
				return;
			}
		}

		if (m_pPersonWeLookingAt)
		{
			CEntityAlive* pEntityAliveWeLookingAt =
				smart_cast<CEntityAlive*>(m_pPersonWeLookingAt);

			VERIFY(pEntityAliveWeLookingAt);

			if (IsGameTypeSingle())
			{
				if (pEntityAliveWeLookingAt->g_Alive())
				{
					TryToTalk();
				}
				else if (!bCaptured)
				{
					//только если находимся в режиме single
					CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
					if (pGameSP)
					{
						if (!m_pPersonWeLookingAt->deadbody_closed_status())
						{
							if (pEntityAliveWeLookingAt->AlreadyDie() &&
								pEntityAliveWeLookingAt->GetLevelDeathTime() + 3000 < Device.dwTimeGlobal)
								// 99.9% dead
								pGameSP->StartCarBody(this, m_pPersonWeLookingAt);
						}
					}
				}
			}
		}
	}
}
extern BOOL firstPersonDeath;
BOOL CActor::HUDview() const
{
	return IsFocused() && (cam_active == eacFirstEye) &&
		((!m_holder) || (m_holder && m_holder->allowWeapon() && m_holder->HUDView())) && (firstPersonDeath ? g_Alive() : true);
}

static u16 SlotsToCheck [] = {
	KNIFE_SLOT, // 0
	INV_SLOT_2, // 1
	INV_SLOT_3, // 2
	GRENADE_SLOT, // 3
	ARTEFACT_SLOT, // 10
	PDA_SLOT
};

void CActor::OnNextWeaponSlot()
{
	u32 ActiveSlot = inventory().GetActiveSlot();
	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = inventory().GetPrevActiveSlot();

	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = KNIFE_SLOT;

	u32 NumSlotsToCheck = sizeof(SlotsToCheck) / sizeof(SlotsToCheck[0]);

	u32 CurSlot = 0;
	for (; CurSlot < NumSlotsToCheck; CurSlot++)
	{
		if (SlotsToCheck[CurSlot] == ActiveSlot) break;
	};

	if (CurSlot >= NumSlotsToCheck)
		return;

	for (u32 i = CurSlot + 1; i < NumSlotsToCheck; i++)
	{
		if (inventory().ItemFromSlot(SlotsToCheck[i]))
		{
			if (SlotsToCheck[i] == ARTEFACT_SLOT)
			{
				IR_OnKeyboardPress(kARTEFACT);
			}
			else if (SlotsToCheck[i] == PDA_SLOT)
			{
				IR_OnKeyboardPress(kACTIVE_JOBS);
			}
			else
				IR_OnKeyboardPress(kWPN_1 + i);
			return;
		}
	}
};

void CActor::OnPrevWeaponSlot()
{
	u32 ActiveSlot = inventory().GetActiveSlot();
	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = inventory().GetPrevActiveSlot();

	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = KNIFE_SLOT;

	u32 NumSlotsToCheck = sizeof(SlotsToCheck) / sizeof(SlotsToCheck[0]);
	u32 CurSlot = 0;

	for (; CurSlot < NumSlotsToCheck; CurSlot++)
	{
		if (SlotsToCheck[CurSlot] == ActiveSlot) break;
	};

	if (CurSlot >= NumSlotsToCheck)
		CurSlot = NumSlotsToCheck - 1; //last in row

	for (s32 i = s32(CurSlot - 1); i >= 0; i--)
	{
		if (inventory().ItemFromSlot(SlotsToCheck[i]))
		{
			if (SlotsToCheck[i] == ARTEFACT_SLOT)
			{
				IR_OnKeyboardPress(kARTEFACT);
			}
			else if (SlotsToCheck[i] == PDA_SLOT)
			{
				IR_OnKeyboardPress(kACTIVE_JOBS);
			}
			else
				IR_OnKeyboardPress(kWPN_1 + i);
			return;
		}
	}
};

extern float g_AimLookFactor;

// momopate: Optional restoration of the handling gimmick from the original games
BOOL g_allow_weapon_control_inertion_factor = 0;
BOOL g_allow_outfit_control_inertion_factor = 0;

float CActor::GetLookFactor()
{
	if (m_input_external_handler)
		return m_input_external_handler->mouse_scale_factor();

	float factor = 1.f;

	if (m_bZoomAimingMode)
		factor /= g_AimLookFactor;

	if (g_allow_weapon_control_inertion_factor)
	{
		PIItem pItem = inventory().ActiveItem();
		if (pItem)
			factor *= pItem->GetControlInertionFactor();
	}

	if (g_allow_outfit_control_inertion_factor)
	{
		CCustomOutfit* outfit = GetOutfit();
		if (outfit)
			factor *= outfit->GetControlInertionFactor();
	}

    VERIFY(!fis_zero(factor));

	if (cam_freelook != eflDisabled)
		return 1.5f;

	return factor;
}

void CActor::set_input_external_handler(CActorInputHandler* handler)
{
	// clear state
	if (handler)
		mstate_wishful = 0;

	// release fire button
	if (handler)
		IR_OnKeyboardRelease(kWPN_FIRE);

	// set handler
	m_input_external_handler = handler;
}

void CActor::SwitchNightVision()
{
	SwitchNightVision(!m_bNightVisionOn);
}

void CActor::SwitchTorch()
{
	CTorch* pTorch = smart_cast<CTorch*>(inventory().ItemFromSlot(TORCH_SLOT));
	if (pTorch)
		pTorch->Switch();
}

#ifdef DEBUG
void CActor::NoClipFly(int cmd)
{
	Fvector cur_pos;// = Position();
	cur_pos.set(0,0,0);
	float scale = 1.0f;
	if(pInput->iGetAsyncKeyState(DIK_LSHIFT))
		scale = 0.25f;
	else if(pInput->iGetAsyncKeyState(DIK_LMENU))
		scale = 4.0f;

	switch(cmd)
	{
	case kJUMP:		
		cur_pos.y += 0.1f;
		break;
	case kCROUCH:	
		cur_pos.y -= 0.1f;
		break;
	case kFWD:	
		cur_pos.z += 0.1f;
		break;
	case kBACK:
		cur_pos.z -= 0.1f;
		break;
	case kL_STRAFE:
		cur_pos.x -= 0.1f;
		break;
	case kR_STRAFE:
		cur_pos.x += 0.1f;
		break;
	case kCAM_1:	
		cam_Set(eacFirstEye);				
		break;
	case kCAM_2:	
		cam_Set(eacLookAt);				
		break;
	case kCAM_3:	
		cam_Set(eacFreeLook);
		break;
	case kNIGHT_VISION:
		SwitchNightVision();
		break;
	case kTORCH:
		SwitchTorch();
		break;
	case kDETECTOR:
		{
			PIItem det_active = inventory().ItemFromSlot(DETECTOR_SLOT);
			if(det_active)
			{
				CCustomDetector* det = smart_cast<CCustomDetector*>(det_active);
					det->ToggleDetector(g_player_hud->attached_item(0)!=NULL);
				return;
			}
		}
		break;
	case kUSE:
		ActorUse();
		break;
	}
	cur_pos.mul(scale);
	Fmatrix	mOrient;
	mOrient.rotateY(-(cam_Active()->GetWorldYaw()));
	mOrient.transform_dir(cur_pos);
	Position().add(cur_pos);
	character_physics_support()->movement()->SetPosition(Position());
}
#endif //DEBUG
