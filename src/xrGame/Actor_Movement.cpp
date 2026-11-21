#include "stdafx.h"

#include "actor.h"
#include "inventory.h"
#include "weapon.h"
#include "../xrEngine/CameraBase.h"
#include "xrMessages.h"

#include "level.h"
#include "UIGameCustom.h"
#include "string_table.h"
#include "actorcondition.h"
#include "game_cl_base.h"
#include "WeaponMagazined.h"
#include "CharacterPhysicsSupport.h"
#include "actoreffector.h"
#include "static_cast_checked.hpp"
#include "player_hud.h"
#include "bodycam_settings.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/GamepadMath.h"
#include "../xrEngine/GamepadErrorGuard.h"
#include "../xrEngine/XR_IOConsole.h"
#include "UI/UIDialogWnd.h"
#include "GamepadContextDispatcher.h"

#ifdef DEBUG
#include "phdebug.h"
#endif
BOOL disableActorBodyRotationDelay = TRUE;

// verdatim: flag to allow 'running' speed during left or right lookouts
BOOL AllowAccelDuringLookOut = FALSE; 

// C2.1 + D2.2: конвейер мёртвой зоны, насыщения и кривой из профиля калибровки (GamepadMath)
constexpr float GAMEPAD_MOVE_FLAG_THRESHOLD = 0.30f;
constexpr float GAMEPAD_SPRINT_BACK_K = 0.35f; // FIX-LIVE2 п.14: бег снимает стик назад, угол > ~110° от «вперёд»
constexpr float GAMEPAD_STUCK_VEL_BASE = 0.2f;

extern bool g_bDisableAllInput;
bool g_gp_synth_move_active = false;
// FIX-LIVE1 п.1: множитель скорости анимации �
// Читают: ноги/шаги (ActorAnimation.cpp), покачивание камеры (EffectorBobbing.cpp), слои движения рук (player_hud.cpp).
float g_gp_move_anim_k = 1.f;
// FIX-LIVE3 п.5: скорость �
// нижняя граница 0.35 давала у слабого наклона почти обычный темп при едва ползущем персонаже.
constexpr float GAMEPAD_MOVE_ANIM_MIN_K = 0.15f;
// FIX-LIVE3 п.7: окно, в которое отпущенный на бегу стик может вернуться и продолжить бег (переброс через центр)
constexpr u32 GAMEPAD_SPRINT_PENDING_MS = 200;
// FIX-LIVE4: бег держится, пока наклон LS не ослаблен; ниже порога дольше 150 мс — бег снят.
// FIX-LIVE5 (Миша 2026-11-16): порог — по ФИЗИЧЕСКОМУ наклону стика (до мёртвой зоны и кривой): отпустил
// примерно на 30% от полного — персонаж пере�
// соответствовали ~0.65-0.75 и зависели от калибровки, поэтому бег лишь замедлялся.
constexpr float GAMEPAD_SPRINT_KEEP_MAG = 0.7f;
constexpr u32 GAMEPAD_SPRINT_LOW_MS = 150;
float g_gp_synth_move_x = 0.f;
float g_gp_synth_move_y = 0.f;

void CActor::UpdateGamepadMoveInput()
{
	m_vGamepadMove.set(0.f, 0.f);
	m_fGamepadMoveRawMag = 0.f;

	u32 sehCode = 0;
	u64 sehAddr = 0;
	const bool stepOk = gperr::ExecuteProtectedGamepadStepSEH(
	    +[](void* rawSelf)
	    {
		    CActor* self = static_cast<CActor*>(rawSelf);
		    if (!self)
			    return;

		    CGamepadContextDispatcher::Instance().OnFrame();
		    if ((Console && Console->bVisible) || !CGamepadContextDispatcher::Instance().AllowsWorldMove())
		    {
			    if (self->m_bGamepadMoveActive)
			    {
				    self->m_bGamepadMoveActive = false;
				    self->StopAnyMove();
			    }
			    return;
		    }

		    if (Device.Paused() || g_bDisableAllInput || !self->g_Alive() || self->Remote() || self->IsTalking() || self->m_holder != nullptr)
			    return;

		    if (CurrentGameUI())
		    {
			    CUIDialogWnd* pTIR = CurrentGameUI()->TopInputReceiver();
			    if (pTIR && pTIR->IR_process() && pTIR->StopAnyMove())
				    return;
		    }

		    if (g_gp_synth_move_active)
		    {
			    Fvector2 synth = { g_gp_synth_move_x, g_gp_synth_move_y };
			    const float mag = synth.magnitude();
			    if (mag > 1.f)
				    synth.mul(1.f / mag);
			    if (mag > EPS_S)
			    {
				    self->m_vGamepadMove = synth;
				    self->m_fGamepadMoveRawMag = _min(mag, 1.f);
			    }
			    return;
		    }

		    if (!pInput)
			    return;

		    const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		    if (!gp.connected || !gp.has_focus)
			    return;

		    const float rawX = gpmath::NormalizeStickAxis(gp.axes[0]);
		    const float rawY = -gpmath::NormalizeStickAxis(gp.axes[1]);
		    const gpmath::Vec2 mv = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick, rawX, rawY);
		    if (_abs(mv.x) <= EPS_S && _abs(mv.y) <= EPS_S)
			    return;

		    self->m_vGamepadMove.set(mv.x, mv.y);
		    self->m_fGamepadMoveRawMag = _min(_sqrt(rawX * rawX + rawY * rawY), 1.f);
	    },
	    this,
	    sehCode,
	    sehAddr);

	if (!stepOk)
	{
		g_gp_synth_move_active = false;
		m_vGamepadMove.set(0.f, 0.f);
		CGamepadContextDispatcher::Instance().ConsumeAllCurrentlyHeldButtons();
		const LPCSTR ctxName = CGamepadContextDispatcher::GetContextName(CGamepadContextDispatcher::Instance().GetCurrentContext());
		if (pInput)
		{
			pInput->HandleGamepadFrameError("CActor::UpdateGamepadMoveInput", nullptr, sehCode, sehAddr, ctxName);
		}
		else
		{
			gperr::HandleProtectedFrameFailure("CActor::UpdateGamepadMoveInput", nullptr, sehCode, sehAddr);
		}
	}
}

bool CActor::EvaluateGamepadSprintCone(float gx, float gy, float gpMag, float& outVx, float& outVz)
{
	// FIX-LIVE2 п.14 + п.22 (Миша 2026-11-16): бег снимает только стик назад (угол больше ~110° от «вперёд»,
	// gy < -0.35 * |LS|); вперёд, по диагонали и строго вбок бег держится, боковая составляющая не урезается
	// (вбок с той же скоростью, что вперёд: f_strafe = 1 в расчёте скорости ниже).
	// Было (E1.1, D42): конус ±60° и боковая составляющая * 0.5.
	if (gpMag <= EPS_S || gy < -GAMEPAD_SPRINT_BACK_K * gpMag)
	{
		outVx = gx;
		outVz = gy;
		return false;
	}
	outVx = gx;
	outVz = gy;
	return true;
}

void CActor::EvaluateGamepadLadderStick(float gx, float gy, float& outVx, float& outVz, u32& inOutGpFlags)
{
	// E1.1 (4, 5): на лестнице (mcClimb) при отклонении стика преимущественно ввер�
	// обнуляем случайный боковой дрейф пальца, чтобы CElevatorState::UpdateStClimbing* не сбрасывал в clbDepart;
	// при намеренном отклонении стика вбок (|gx| > |gy|) со�
	if (_abs(gy) >= _abs(gx))
	{
		outVx = 0.f;
		outVz = gy;
		inOutGpFlags &= ~(mcLStrafe | mcRStrafe);
		inOutGpFlags |= (gy >= 0.f) ? mcFwd : mcBack;
	}
	else
	{
		outVx = gx;
		outVz = gy;
	}
}

void CActor::StepGamepadPostureState(bool isHoldAction,
                                     bool isSprintAction,
                                     bool& inOutCrouch,
                                     bool& inOutLowCrouch,
                                     bool& inOutSprint,
                                     u32& inOutWishful)
{
	if (isSprintAction)
	{
		// L3 (D16): Toggle спринта; если были в приседе / низком приседе — встаём и включаем спринт.
		// E1.2b: решение принимается только по флагам геймпада (+ mstate_real, см. обёртки Set*), НЕ по mstate_wishful:
		// диспетчер перед вызовом инъецирует kSPRINT_TOGGLE, и при g_sprint_toggle=on движок уже сам переключил
		// mcSprint в mstate_wishful — повторный toggle по этому биту сразу выключал бы только что включённый спринт.
		if (inOutSprint)
		{
			inOutSprint = false;
			inOutWishful &= ~mcSprint;
		}
		else
		{
			inOutCrouch = false;
			inOutLowCrouch = false;
			inOutSprint = true;
			inOutWishful &= ~(mcCrouch | mcAccel);
			inOutWishful |= mcSprint;
		}
		return;
	}

	if (!isHoldAction)
	{
		// Короткое нажатие B (Tap < 300 мс): Toggle приседа (стоя -> присед Box 1; присед или низкий присед -> встать Box 0).
		// E1.2b: решение — только по флагам геймпада (+ mstate_real, см. обёртки Set*), НЕ по mstate_wishful.
		// Диспетчер перед вызовом инъецирует kCROUCH (press+release), и при g_crouch_toggle=on движок уже сам сделал
		// mstate_wishful ^= mcCrouch (ActorInput.cpp, case kCROUCH). Раньше этот бит читался как «уже в приседе»
		// и короткое B тут же выключало присед (персонаж оставался стоять). Явная запись wishful ниже
		// переопределяет результат инъекции в обе стороны.
		if (inOutCrouch || inOutLowCrouch)
		{
			inOutCrouch = false;
			inOutLowCrouch = false;
			inOutWishful &= ~(mcCrouch | mcAccel);
		}
		else
		{
			inOutCrouch = true;
			inOutLowCrouch = false;
			inOutSprint = false;
			inOutWishful &= ~(mcSprint | mcAccel);
			inOutWishful |= mcCrouch;
		}
	}
	else
	{
		// Удержание B (Hold >= 300 мс): стоя или в обычном приседе -> низкий присед Box 2 (mcCrouch | mcAccel);
		// повторное удержание B из низкого приседа -> подъём в обычный присед Box 1 (mcCrouch без mcAccel)
		if (inOutLowCrouch || ((inOutWishful & (mcCrouch | mcAccel)) == (mcCrouch | mcAccel)))
		{
			inOutCrouch = true;
			inOutLowCrouch = false;
			inOutWishful &= ~mcAccel;
			inOutWishful |= mcCrouch;
		}
		else
		{
			inOutCrouch = true;
			inOutLowCrouch = true;
			inOutSprint = false;
			inOutWishful &= ~mcSprint;
			inOutWishful |= (mcCrouch | mcAccel);
		}
	}
}

void CActor::SetGamepadSprintToggle()
{
	// FIX-LIVE2 п.14: повторное нажатие LS при отпущенном стике снимает со�
	if (m_bGamepadSprintPending && !m_bGamepadSprint)
	{
		m_bGamepadSprintPending = false;
		mstate_wishful &= ~mcSprint;
		return;
	}
	m_bGamepadSprintPending = false;
	m_bGamepadCrouch = m_bGamepadCrouch || ((mstate_real & mcCrouch) != 0);
	m_bGamepadLowCrouch = m_bGamepadLowCrouch || ((mstate_real & (mcCrouch | mcAccel)) == (mcCrouch | mcAccel));
	m_bGamepadSprint = m_bGamepadSprint || ((mstate_real & mcSprint) != 0);
	StepGamepadPostureState(false, true, m_bGamepadCrouch, m_bGamepadLowCrouch, m_bGamepadSprint, mstate_wishful);
}

void CActor::SetGamepadCrouchToggle()
{
	m_bGamepadCrouch = m_bGamepadCrouch || ((mstate_real & mcCrouch) != 0);
	m_bGamepadLowCrouch = m_bGamepadLowCrouch || ((mstate_real & (mcCrouch | mcAccel)) == (mcCrouch | mcAccel));
	m_bGamepadSprint = m_bGamepadSprint || ((mstate_real & mcSprint) != 0);
	StepGamepadPostureState(false, false, m_bGamepadCrouch, m_bGamepadLowCrouch, m_bGamepadSprint, mstate_wishful);
}

void CActor::SetGamepadLowCrouchHold()
{
	m_bGamepadCrouch = m_bGamepadCrouch || ((mstate_real & mcCrouch) != 0);
	m_bGamepadLowCrouch = m_bGamepadLowCrouch || ((mstate_real & (mcCrouch | mcAccel)) == (mcCrouch | mcAccel));
	m_bGamepadSprint = m_bGamepadSprint || ((mstate_real & mcSprint) != 0);
	StepGamepadPostureState(true, false, m_bGamepadCrouch, m_bGamepadLowCrouch, m_bGamepadSprint, mstate_wishful);
}

void CActor::ApplyGamepadPostureToWishful()
{
	if (m_bGamepadCrouch)
	{
		mstate_wishful |= mcCrouch;
		if (m_bGamepadLowCrouch)
			mstate_wishful |= mcAccel;
		else
			mstate_wishful &= ~mcAccel;
	}
	else
	{
		mstate_wishful &= ~mcCrouch;
		if (!m_bGamepadLowCrouch)
			mstate_wishful &= ~mcAccel;
	}

	if (m_bGamepadSprint)
		mstate_wishful |= mcSprint;
	else
		mstate_wishful &= ~mcSprint;
}

void CActor::ResetGamepadMovementLatches()
{
	m_bGamepadSprint = false;
	m_bGamepadSprintPending = false;
	m_bGamepadCrouch = false;
	m_bGamepadLowCrouch = false;
}

static const float s_fLandingTime1 = 0.1f; // через сколько снять флаг Landing1 (т.е. включить следующую анимацию)
static const float s_fLandingTime2 = 0.3f; // через сколько снять флаг Landing2 (т.е. включить следующую анимацию)
static const float s_fJumpTime = 0.3f;
static const float s_fJumpGroundTime = 0.1f; // для снятия флажка Jump если на земле
const float s_fFallTime = 0.2f;

IC static void generate_orthonormal_basis1(const Fvector& dir, Fvector& updir, Fvector& right)
{
	right.crossproduct(dir, updir); //. <->
	right.normalize();
	updir.crossproduct(right, dir);
}


void CActor::g_cl_ValidateMState(float dt, u32 mstate_wf)
{
	// Lookout
	if (((mstate_wf & mcLLookout) && (mstate_wf & mcRLookout)) || ((mstate_real & mcLLookout) && (mstate_real & mcRLookout)))
	{
		// It's impossible to perform right and left lookouts in the same time
		mstate_real &= ~mcLookout;
	}
	else if (mstate_wf & mcLookout)
	{
		// Activate one of lookouts
		mstate_real |= mstate_wf & mcLookout;
	}
	else
	{
		// No lookouts needed
		mstate_real &= ~mcLookout;
	}

	if (mstate_real & (mcJump | mcFall | mcLanding | mcLanding2))
		mstate_real &= ~mcLookout;

	// закончить приземление
	if (mstate_real & (mcLanding | mcLanding2))
	{
		m_fLandingTime -= dt;
		if (m_fLandingTime <= 0.f)
		{
			mstate_real &= ~ (mcLanding | mcLanding2);
			mstate_real &= ~ (mcFall | mcJump);
		}
	}
	// закончить падение
	if (character_physics_support()->movement()->gcontact_Was)
	{
		if (mstate_real & mcFall)
		{
			if (character_physics_support()->movement()->GetContactSpeed() > 4.f)
			{
				if (fis_zero(character_physics_support()->movement()->gcontact_HealthLost))
				{
					m_fLandingTime = s_fLandingTime1;
					mstate_real |= mcLanding;
				}
				else
				{
					m_fLandingTime = s_fLandingTime2;
					mstate_real |= mcLanding2;
				}
			}

			::luabind::functor<bool> on_land;
			if (ai().script_engine().functor("_G.CActor_on_land", on_land))
				on_land(character_physics_support()->movement()->GetContactSpeed());
		}
		m_bJumpKeyPressed = TRUE;
		m_fJumpTime = s_fJumpTime;
		mstate_real &= ~ (mcFall | mcJump);
	}
	if ((mstate_wf & mcJump) == 0)
		m_bJumpKeyPressed = FALSE;

	// Зажало-ли меня/уперся - не двигаюсь
	const float stuckVelThreshold = m_bGamepadMoveActive
		? GAMEPAD_STUCK_VEL_BASE * clampr(m_vGamepadMove.magnitude(), 0.f, 1.f) // C2.1 fix: порог пропорционален |v| (скорость тоже ~|v|); полный стик = клавиатура 0.2
		: GAMEPAD_STUCK_VEL_BASE;
	if (((character_physics_support()->movement()->GetVelocityActual() < stuckVelThreshold) && (!(mstate_real & (mcFall | mcJump | mcLanding | mcLanding2)) || (!(mstate_real & mcClimb) && character_physics_support()->movement()->Environment() == CPHMovementControl::peAtWall)))
		|| character_physics_support()->movement()->bSleep)
	{
		mstate_real &= ~ mcAnyMove;
	}
	if (character_physics_support()->movement()->Environment() == CPHMovementControl::peOnGround ||
		character_physics_support()->movement()->Environment() == CPHMovementControl::peAtWall)
	{
		// если на земле гарантированно снимать флажок Jump
		if (((s_fJumpTime - m_fJumpTime) > s_fJumpGroundTime) && (mstate_real & mcJump))
		{
			mstate_real &= ~ mcJump;
			m_fJumpTime = s_fJumpTime;
		}
	}
	if (character_physics_support()->movement()->Environment() == CPHMovementControl::peAtWall)
	{
		if (!(mstate_real & mcClimb))
		{
			mstate_real |= mcClimb;
			mstate_real &= ~mcSprint;
			m_bGamepadSprint = false;
			m_bGamepadCrouch = false;
			m_bGamepadLowCrouch = false;
			cam_SetLadder();
		}
	}
	else
	{
		if (mstate_real & mcClimb)
		{
			cam_UnsetLadder();
		}
		mstate_real &= ~mcClimb;
	};

	if (mstate_wf != mstate_real)
	{
		if ((mstate_real & mcCrouch) && ((0 == (mstate_wf & mcCrouch)) || mstate_real & mcClimb))
		{
			if (character_physics_support()->movement()->ActivateBoxDynamic(0))
			{
				mstate_real &= ~mcCrouch;
			}
		}
	}

	if (!CanAccelerate() && isActorAccelerated(mstate_real, IsZoomAimingMode()) && !(mstate_real & mcCrouch) && !m_bGamepadMoveActive)
	{
		mstate_real ^= mcAccel;
	};

	if (this == Level().CurrentControlEntity())
	{
		bool bOnClimbNow = !!(mstate_real & mcClimb);
		bool bOnClimbOld = !!(mstate_old & mcClimb);

		if (bOnClimbNow != bOnClimbOld)
		{
			SetWeaponHideState(/*INV_STATE_LADDER*/ INV_STATE_BLOCK_ALL, bOnClimbNow);
		};
	};
};

void CActor::g_cl_CheckControls(u32 mstate_wf, Fvector& vControlAccel, float& Jump, float dt)
{
	const bool bodycam_reference = BodycamReferenceMovement();
	float cam_eff_factor = 0.0f;
	mstate_old = mstate_real;
	vControlAccel.set(0, 0, 0);

	if (!(mstate_real & mcFall) && (character_physics_support()->movement()->Environment() == CPHMovementControl::
		peInAir))
	{
		m_fFallTime -= dt;
		if (m_fFallTime <= 0.f)
		{
			m_fFallTime = s_fFallTime;
			mstate_real |= mcFall;
			mstate_real &= ~ mcJump;
		}
	}

	if (!CanMove())
	{
		if ((mstate_wf & mcAnyMove) || (mstate_real & mcAnyMove) || m_bGamepadSprint)
		{
			StopAnyMove();
			mstate_wf &= ~mcAnyMove;
			mstate_wf &= ~mcJump;
			mstate_wf &= ~mcSprint;
		}
	}
	if (bodycam_reference) BodycamReferenceGait(mstate_wf);

	const bool bKbMove = (mstate_wf & mcAnyMove) != 0;
	const float gpMagSq = m_vGamepadMove.square_magnitude();
	const bool bGpMove = !bKbMove && CanMove() && (gpMagSq > EPS_S);
	m_bGamepadMoveActive = bGpMove;
	g_gp_move_anim_k = bGpMove ? _max(GAMEPAD_MOVE_ANIM_MIN_K, clampr(_sqrt(gpMagSq), 0.f, 1.f)) : 1.f;

	// E1.1 (2): медленный шаг стиком никогда не ставит mcAccel и не включает ActivateBoxDynamic(2) сам.
	if ((bGpMove || m_bGamepadCrouch) && !m_bGamepadLowCrouch)
	{
		mstate_wf &= ~mcAccel;
		mstate_wishful &= ~mcAccel;
	}
	else
	{
		// update player accel
		if (mstate_wf & mcFwd) vControlAccel.z += 1;
		if (mstate_wf & mcBack) vControlAccel.z += -1;
		if (mstate_wf & mcLStrafe) !bodycam_reference && (mstate_wf & mcSprint) ? vControlAccel.x += -0.5 : vControlAccel.x += -1;
		if (mstate_wf & mcRStrafe) !bodycam_reference && (mstate_wf & mcSprint) ? vControlAccel.x += 0.5 : vControlAccel.x += 1;
	}

	CPHMovementControl::EEnvironment curr_env = character_physics_support()->movement()->Environment();
	if (curr_env == CPHMovementControl::peOnGround || curr_env == CPHMovementControl::peAtWall)
	{
		// crouch
		if ((0 == (mstate_real & mcCrouch)) && (mstate_wf & mcCrouch))
		{
			if (mstate_real & mcClimb)
			{
				mstate_wf &= ~mcCrouch;
				m_bGamepadCrouch = false;
				m_bGamepadLowCrouch = false;
			}
			else
			{
				character_physics_support()->movement()->EnableCharacter();
				bool Crouched = false;
				if (isActorAccelerated(mstate_wf, IsZoomAimingMode()))
					Crouched = character_physics_support()->movement()->ActivateBoxDynamic(1);
				else
					Crouched = character_physics_support()->movement()->ActivateBoxDynamic(2);

				if (Crouched)
					mstate_real |= mcCrouch;
			}
		}
		// jump
		m_fJumpTime -= dt;

		if (CanJump() && (mstate_wf & mcJump))
		{
			mstate_real |= mcJump;
			m_bJumpKeyPressed = TRUE;
			Jump = m_fJumpSpeed;
			m_fJumpTime = s_fJumpTime;

			::luabind::functor<bool> on_jump;
			if (ai().script_engine().functor("_G.CActor_on_jump", on_jump))
				on_jump();

			//уменьшить силу игрока из-за выполненого прыжка
			if (!GodMode())
				conditions().ConditionJump(inventory().TotalWeight() / MaxCarryWeight());
		}

		// mask input into "real" state
		u32 move = mcAnyMove | mcAccel;

		if (mstate_real & mcCrouch)
		{
			if (!isActorAccelerated(mstate_real, IsZoomAimingMode()) && isActorAccelerated(
				mstate_wf, IsZoomAimingMode()))
			{
				character_physics_support()->movement()->EnableCharacter();
				if (!character_physics_support()->movement()->ActivateBoxDynamic(1))move &= ~mcAccel;
			}

			if (isActorAccelerated(mstate_real, IsZoomAimingMode()) && !isActorAccelerated(
				mstate_wf, IsZoomAimingMode()))
			{
				character_physics_support()->movement()->EnableCharacter();
				if (character_physics_support()->movement()->ActivateBoxDynamic(2))mstate_real &= ~mcAccel;
			}
		}

		if ((mstate_wf & mcSprint) && !CanSprint())
		{
			mstate_wf &= ~mcSprint;
			if (!((mstate_real & mcCrouch) && !(mstate_wf & mcCrouch)))
			{
				mstate_wishful &= ~mcSprint;
				m_bGamepadSprint = false;
			}
		}

		mstate_real &= (~move);
		mstate_real |= (mstate_wf & move);

		if (mstate_wf & mcSprint)
			mstate_real |= mcSprint;
		else
			mstate_real &= ~mcSprint;
		const bool bGpSprintDir = bGpMove && m_bGamepadSprint && (mstate_real & (mcFwd | mcLStrafe | mcRStrafe)) &&
		                          !(mstate_real & mcBack);
		if ((bodycam_reference ? !BodycamReferenceSprintDirection() : (!(mstate_real & mcFwd) && !bGpSprintDir)) || mstate_real & (mcCrouch | mcClimb) || !isActorAccelerated(
			mstate_wf, IsZoomAimingMode()))
		{
			mstate_real &= ~mcSprint;
			if (!(mstate_real & (mcCrouch) && !(mstate_wf & mcCrouch)))
			{
				mstate_wishful &= ~mcSprint;
				m_bGamepadSprint = false;
			}
		}

		if (bodycam_reference) BodycamReferenceFallbackGait();

		// check player move state
		if (mstate_real & mcAnyMove)
		{
			BOOL bAccelerated = isActorAccelerated(mstate_real, IsZoomAimingMode()) && CanAccelerate();

			// correct "mstate_real" if opposite keys pressed
			if (_abs(vControlAccel.z) < EPS) mstate_real &= ~(mcFwd + mcBack);
			if (_abs(vControlAccel.x) < EPS) mstate_real &= ~(mcLStrafe + mcRStrafe);

			// normalize and analyze crouch and run
			float scale = vControlAccel.magnitude();
			if (scale > EPS)
			{
				if (bGpMove)
				{
					const float gpMag = clampr(_sqrt(gpMagSq), 0.f, 1.f);
					vControlAccel.mul(1.f / scale);

					const float cosTheta = vControlAccel.z;
					const float sinTheta = vControlAccel.x;
					const float cos2 = cosTheta * cosTheta;
					const float sin2 = sinTheta * sinTheta;

					const bool bRunScale = bAccelerated && !IsZoomAimingMode() && (m_fRunFactor > EPS);
					float f_fb = 1.f;
					if (cosTheta < 0.f)
					{
						if (bRunScale)
							f_fb = m_fRunBackFactor / m_fRunFactor;
						else if (bAccelerated && !IsZoomAimingMode())
							f_fb = m_fRunBackFactor;
						else
							f_fb = m_fWalkBackFactor;
					}

					float f_strafe = 1.f;
					if (!(mstate_real & mcCrouch))
					{
						if (mstate_real & mcSprint)
							f_strafe = m_bGamepadSprint ? 1.f : m_fSprint_StrafeFactor;
						else if (bAccelerated)
							f_strafe = m_fRun_StrafeFactor;
						else
							f_strafe = m_fWalk_StrafeFactor;
					}

					const float dirFactor = cos2 * f_fb + sin2 * f_strafe;

					scale = m_fWalkAccel;
					if (bRunScale)
						scale *= m_fRunFactor;

					if (mstate_real & mcCrouch) scale *= m_fCrouchFactor;
					if (mstate_real & mcClimb) scale *= m_fClimbFactor;
					if (mstate_real & mcSprint) scale *= m_fSprintFactor;

					scale *= dirFactor * gpMag;
					if (bodycam_reference) scale = BodycamReferenceMoveScale(scale, vControlAccel, !!bAccelerated);
					vControlAccel.mul(scale);
					cam_eff_factor = scale;
				}
				else
				{
					scale = m_fWalkAccel / scale;
					if (bAccelerated && !IsZoomAimingMode())
						if (mstate_real & mcBack)
							scale *= m_fRunBackFactor;
						else
							scale *= m_fRunFactor;
					else if (mstate_real & mcBack)
						scale *= m_fWalkBackFactor;


					if (mstate_real & mcCrouch) scale *= m_fCrouchFactor;
					if (mstate_real & mcClimb) scale *= m_fClimbFactor;
					if (mstate_real & mcSprint) scale *= m_fSprintFactor;

					if (mstate_real & (mcLStrafe | mcRStrafe) && !(mstate_real & mcCrouch))
					{
						if (mstate_real & mcSprint)
							scale *= m_fSprint_StrafeFactor;
						else if (bAccelerated)
							scale *= m_fRun_StrafeFactor;
						else
							scale *= m_fWalk_StrafeFactor;
					}

					if (bodycam_reference) scale = BodycamReferenceMoveScale(scale, vControlAccel, !!bAccelerated);
					vControlAccel.mul(scale);
					cam_eff_factor = scale;
				}
			} //scale>EPS
		} //(mstate_real&mcAnyMove)
	} //peOnGround || peAtWall

	const bool classic = Local() && Bodycam::ClassicActive();
	if (classic && g_Alive())
		BodycamUpdateMovement(vControlAccel, dt);
	else
		BodycamResetMovement();

	// The Classic footstep camera replaces the movement-entry camera effectors.
	const bool footstep_camera = classic && IsFocused() && Bodycam::FootstepCameraEnabled();
	if (!bodycam_reference && !footstep_camera && IsGameTypeSingle() && cam_eff_factor > EPS)
	{
		LPCSTR state_anm = NULL;

		if (mstate_real & mcSprint && !(mstate_old & mcSprint))
			state_anm = "sprint";
		else if (mstate_real & mcLStrafe && !(mstate_old & mcLStrafe))
			state_anm = "strafe_left";
		else if (mstate_real & mcRStrafe && !(mstate_old & mcRStrafe))
			state_anm = "strafe_right";
		else if (mstate_real & mcFwd && !(mstate_old & mcFwd))
			state_anm = "move_fwd";
		else if (mstate_real & mcBack && !(mstate_old & mcBack))
			state_anm = "move_back";

		if (state_anm)
		{
			//play moving cam effect
			CActor* control_entity = static_cast_checked<CActor*>(Level().CurrentControlEntity());
			if (control_entity)
			{
				R_ASSERT2(control_entity, "current control entity is NULL");
				CEffectorCam* ec = control_entity->Cameras().GetCamEffector(eCEActorMoving);
				if (NULL == ec)
				{
					string_path			eff_name;
					xr_sprintf(eff_name, sizeof(eff_name), "%s.anm", state_anm);
					string_path			ce_path;
					string_path			anm_name;
					strconcat(sizeof(anm_name), anm_name, "camera_effects\\actor_move\\", eff_name);
					if (FS.exist(ce_path, "$game_anims$", anm_name))
					{
						CAnimatorCamLerpEffectorConst* e = xr_new<CAnimatorCamLerpEffectorConst>();
						float max_scale = 70.0f;
						float factor = cam_eff_factor / max_scale;
						e->SetFactor(factor);
						e->SetType(eCEActorMoving);
						e->SetHudAffect(false);
						e->SetCyclic(false);
						e->Start(anm_name);
						control_entity->Cameras().AddCamEffector(e);
					}
				}
			}
		}
	}
	//transform local dir to world dir
	Fmatrix mOrient;
	mOrient.rotateY(-r_model_yaw);
	mOrient.transform_dir(vControlAccel);
}

#define ACTOR_ANIM_SECT "actor_animation"

#define ACTOR_LLOOKOUT_ANGLE	PI_DIV_4
#define ACTOR_RLOOKOUT_ANGLE	PI_DIV_4

void CActor::g_Orientate(u32 mstate_rl, float dt)
{
	static float fwd_l_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "fwd_l_strafe_yaw"));
	static float back_l_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "back_l_strafe_yaw"));
	static float fwd_r_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "fwd_r_strafe_yaw"));
	static float back_r_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "back_r_strafe_yaw"));
	static float l_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "l_strafe_yaw"));
	static float r_strafe_yaw = deg2rad(pSettings->r_float(ACTOR_ANIM_SECT, "r_strafe_yaw"));

	if (!g_Alive())return;
	// visual effect of "fwd+strafe" like motion
	float calc_yaw = 0;
	if (mstate_real & mcClimb)
	{
		if (g_LadderOrient()) return;
	}
	if (m_bGamepadMoveActive && (mstate_rl & mcAnyMove))
	{
		const float gx = m_vGamepadMove.x;
		const float gy = m_vGamepadMove.y;
		if (gy >= 0.f)
			calc_yaw = -atan2f(gx, gy);
		else
			calc_yaw = -atan2f(-gx, -gy);
	}
	else
	{
		switch (mstate_rl & mcAnyMove)
		{
		case mcFwd + mcLStrafe:
			calc_yaw = +fwd_l_strafe_yaw; //+PI_DIV_4; 
			break;
		case mcBack + mcRStrafe:
			calc_yaw = +back_r_strafe_yaw; //+PI_DIV_4; 
			break;
		case mcFwd + mcRStrafe:
			calc_yaw = -fwd_r_strafe_yaw; //-PI_DIV_4; 
			break;
		case mcBack + mcLStrafe:
			calc_yaw = -back_l_strafe_yaw; //-PI_DIV_4; 
			break;
		case mcLStrafe:
			calc_yaw = +l_strafe_yaw; //+PI_DIV_3-EPS_L; 
			break;
		case mcRStrafe:
			calc_yaw = -r_strafe_yaw; //-PI_DIV_4+EPS_L; 
			break;
		}
	}
	m_fGamepadLastCalcYaw = calc_yaw;

	// lerp angle for "effect" and capture torso data from camera
	float scale_yaw_offset = 1.0f;

	if (cam_active == eacFirstEye && g_player_hud && m_legs_controller.is_active()) {
		scale_yaw_offset = 0.15f;
	}

	angle_lerp(r_model_yaw_delta, calc_yaw * scale_yaw_offset, PI_MUL_4 * scale_yaw_offset, dt);

	// build matrix
	Fmatrix mXFORM;
	mXFORM.rotateY(-(r_model_yaw + r_model_yaw_delta));
	mXFORM.c.set(Position());
	XFORM().set(mXFORM);
	VERIFY(_valid(XFORM()));

	//-------------------------------------------------

	float tgt_roll = 0.f;
	if (mstate_rl & mcLookout)
	{
		tgt_roll = (mstate_rl & mcLLookout) ? -ACTOR_LLOOKOUT_ANGLE : ACTOR_RLOOKOUT_ANGLE;

		// demonized: add lookout modifier
		tgt_roll *= m_fLookoutFactor;

		if ((mstate_rl & mcLLookout) && (mstate_rl & mcRLookout))
			tgt_roll = 0.0f;
	}
	if (!fsimilar(tgt_roll, r_torso_tgt_roll, EPS))
	{
		r_torso_tgt_roll = angle_inertion_var(r_torso_tgt_roll, tgt_roll, 0.f, CurrentHeight * PI_MUL_2, PI_DIV_2, dt);
		r_torso_tgt_roll = angle_normalize_signed(r_torso_tgt_roll);
	}
}

bool CActor::g_LadderOrient()
{
	Fvector leader_norm;
	character_physics_support()->movement()->GroundNormal(leader_norm);
	if (_abs(leader_norm.y) > M_SQRT1_2) return false;
	//leader_norm.y=0.f;
	float mag = leader_norm.magnitude();
	if (mag < EPS_L) return false;
	leader_norm.div(mag);
	leader_norm.invert();
	Fmatrix M;
	M.set(Fidentity);
	M.k.set(leader_norm);
	M.j.set(0.f, 1.f, 0.f);
	generate_orthonormal_basis1(M.k, M.j, M.i);
	M.i.invert();
	//M.j.invert();


	//Fquaternion q1,q2,q3;
	//q1.set(XFORM());
	//q2.set(M);
	//q3.slerp(q1,q2,dt);
	//Fvector angles1,angles2,angles3;
	//XFORM().getHPB(angles1.x,angles1.y,angles1.z);
	//M.getHPB(angles2.x,angles2.y,angles2.z);
	////angle_lerp(angles3.x,angles1.x,angles2.x,dt);
	////angle_lerp(angles3.y,angles1.y,angles2.y,dt);
	////angle_lerp(angles3.z,angles1.z,angles2.z,dt);

	//angles3.lerp(angles1,angles2,dt);
	////angle_lerp(angles3.y,angles1.y,angles2.y,dt);
	////angle_lerp(angles3.z,angles1.z,angles2.z,dt);
	//angle_lerp(angles3.x,angles1.x,angles2.x,dt);
	//XFORM().setHPB(angles3.x,angles3.y,angles3.z);
	Fvector position;
	position.set(Position());
	//XFORM().rotation(q3);
	VERIFY2(_valid(M), "Invalide matrix in g_LadderOrient");
	XFORM().set(M);
	VERIFY2(_valid(position), "Invalide position in g_LadderOrient");
	Position().set(position);
	VERIFY(_valid(XFORM()));
	return true;
}

// ****************************** Update actor orientation according to camera orientation
void CActor::g_cl_Orientate(u32 mstate_rl, float dt)
{
	// capture camera into torso (only for FirstEye & LookAt cameras)
	if (eacFreeLook != cam_active)
	{
		if (cam_freelook == eflDisabled)
		{
			r_torso.yaw = cam_Active()->GetWorldYaw();
			r_torso.pitch = cam_Active()->GetWorldPitch();
		}
		else
		{
			CCameraBase* C = cam_Active();
			r_torso.yaw = angle_lerp(cam_Active()->GetWorldYaw(), -old_torso_yaw, freelook_cam_control);
			float old_pitch = cam_Active()->GetWorldPitch();
			float new_pitch = old_pitch > 0.f ? old_pitch * .6f : old_pitch *.8f;
			r_torso.pitch = angle_lerp(old_pitch, new_pitch, freelook_cam_control);
		}
	}
	else
	{
		r_torso.yaw = cam_FirstEye()->GetWorldYaw();
		r_torso.pitch = cam_FirstEye()->GetWorldPitch();
	}

	unaffected_r_torso.yaw = r_torso.yaw;
	unaffected_r_torso.pitch = r_torso.pitch;
	unaffected_r_torso.roll = r_torso.roll;

	CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*>(inventory().GetActiveSlot() != NO_ACTIVE_SLOT
		                                                      ? inventory().ItemFromSlot(inventory().GetActiveSlot())
		                                                      : NULL);
	if (pWM && pWM->GetCurrentFireMode() == 1 && eacFirstEye != cam_active)
	{
		Fvector dangle = weapon_recoil_last_delta();
		r_torso.yaw = unaffected_r_torso.yaw + dangle.y;
		r_torso.pitch = unaffected_r_torso.pitch + dangle.x;
	}

    if (disableActorBodyRotationDelay || Device.time_factor() < 1)
    {
        r_model_yaw = angle_normalize(r_torso.yaw);
        r_model_yaw_dest = r_model_yaw;
        mstate_real &= ~mcTurn;
    }
    else
    {
        // если есть движение - выровнять модель по камере
        if (mstate_rl & mcAnyMove)
        {
            r_model_yaw = angle_normalize(r_torso.yaw);
            mstate_real &= ~mcTurn;
        }
        else
        {
            // if camera rotated more than 45 degrees - align model with it
            float ty = angle_normalize(r_torso.yaw);
            if (_abs(r_model_yaw - ty) > PI_DIV_4 - 30)
            {
                r_model_yaw_dest = ty;
                // 
                mstate_real |= mcTurn;
            }
            if (_abs(r_model_yaw - r_model_yaw_dest) < EPS_L)
            {
                mstate_real &= ~mcTurn;
            }
            if (mstate_rl & mcTurn)
            {
                angle_lerp(r_model_yaw, r_model_yaw_dest, PI_MUL_2, dt);
            }
        }
    }
}

void CActor::g_sv_Orientate(u32 /**mstate_rl/**/, float /**dt/**/)
{
	r_model_yaw = NET_Last.o_model;

	r_torso.yaw = unaffected_r_torso.yaw;
	r_torso.pitch = unaffected_r_torso.pitch;
	r_torso.roll = unaffected_r_torso.roll;

	CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*>(inventory().GetActiveSlot() != NO_ACTIVE_SLOT
		                                                      ? inventory().ItemFromSlot(inventory().GetActiveSlot())
		                                                      : NULL);
	if (pWM && pWM->GetCurrentFireMode() == 1/* && eacFirstEye != cam_active*/)
	{
		Fvector dangle = weapon_recoil_last_delta();
		r_torso.yaw += dangle.y;
		r_torso.pitch += dangle.x;
		r_torso.roll += dangle.z;
	}
}

bool isActorAccelerated(u32 mstate, bool ZoomMode)
{
	bool res = false;
	if (mstate & mcAccel)
		res = false;
	else
		res = true;

	if (mstate & (mcCrouch | mcClimb | mcJump | mcLanding | mcLanding2))
		return res;
	if ((mstate & mcLookout && !AllowAccelDuringLookOut) || ZoomMode)
		return false;
	return res;
}

bool CActor::CanAccelerate()
{
	bool can_accel = !conditions().IsLimping() &&
		!character_physics_support()->movement()->PHCapture() &&
		(m_time_lock_accel < Device.dwTimeGlobal);

	return can_accel;
}

bool CActor::CanRun()
{
	bool can_run = !IsZoomAimingMode() && !(mstate_real & mcLookout);
	return can_run;
}

bool CActor::CanSprint()
{
	bool can_Sprint = CanAccelerate() && !conditions().IsCantSprint() &&
		Game().PlayerCanSprint(this)
		&& CanRun()
		/*&& !(mstate_real&mcLStrafe || mstate_real&mcRStrafe)*/
		&& InventoryAllowSprint();

	if (!Bodycam::ReferenceMovementActive() && (mstate_real & mcLStrafe || mstate_real & mcRStrafe) && !(mstate_real & mcFwd) &&
	    !(m_bGamepadMoveActive && m_bGamepadSprint))
		can_Sprint = false;

	return can_Sprint && (m_block_sprint_counter <= 0);
}

bool CActor::CanJump()
{
	bool can_Jump =
		!conditions().IsCantSprint() && !character_physics_support()->movement()->PHCapture() && ((mstate_real & mcJump)
			== 0) && (m_fJumpTime <= 0.f)
		&& !m_bJumpKeyPressed && !IsZoomAimingMode();

	return can_Jump;
}

bool CActor::CanMove()
{
	const bool bTryingToMove = ((mstate_wishful & mcAnyMove) != 0) || (m_vGamepadMove.square_magnitude() > EPS_S);
	if (conditions().IsCantWalk())
	{
		if (bTryingToMove && CurrentGameUI())
		{
			CurrentGameUI()->AddCustomStatic("cant_walk", true);
		}
		return false;
	}
	else if (conditions().IsCantWalkWeight())
	{
		if (bTryingToMove && CurrentGameUI())
		{
			CurrentGameUI()->AddCustomStatic("cant_walk_weight", true);
		}
		return false;
	}

	if (IsTalking())
		return false;
	else
		return true;
}

void CActor::StopAnyMove()
{
	mstate_wishful &= ~mcAnyMove;
	mstate_real &= ~mcAnyMove;
	m_vGamepadMove.set(0.f, 0.f);
	m_bGamepadMoveActive = false;
	m_bGamepadSprint = false;

	if (this == Level().CurrentViewEntity())
	{
		g_player_hud->OnMovementChanged((EMoveCommand)0);
	}
}


bool CActor::is_jump()
{
	return ((mstate_real & (mcJump | mcFall | mcLanding | mcLanding2)) != 0);
}

//максимальный переносимы вес
#include "CustomOutfit.h"

float CActor::MaxCarryWeight() const
{
	float res = inventory().GetMaxWeight();
	res += get_additional_weight();
	return res;
}

float CActor::MaxWalkWeight() const
{
	float max_w = CActor::conditions().MaxWalkWeight();
	max_w += get_additional_weight();
	return max_w;
}

#include "artefact.h"
#include "ActorBackpack.h"

float CActor::get_additional_weight() const
{
	float res = conditions().GetCarryWeightBoost();

	CCustomOutfit* outfit = GetOutfit();
	if (outfit)

		res += outfit->m_additional_weight;


	CBackpack* pBackpack = smart_cast<CBackpack*>(inventory().ItemFromSlot(BACKPACK_SLOT));
	if (pBackpack)
		res += pBackpack->m_additional_weight;

	for (TIItemContainer::const_iterator it = inventory().m_belt.begin();
	     inventory().m_belt.end() != it; ++it)
	{
		CArtefact* artefact = smart_cast<CArtefact*>(*it);
		if (artefact)
			res += (artefact->AdditionalInventoryWeight() * artefact->GetCondition());
	}

	return res;
}
