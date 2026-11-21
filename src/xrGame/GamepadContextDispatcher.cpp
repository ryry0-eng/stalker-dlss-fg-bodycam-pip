#include "stdafx.h"
#include "GamepadContextDispatcher.h"
#include "PauseTokenManager.h"
#include "../xrEngine/device.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/GamepadHaptics.h"
#include "../xrEngine/XR_IOConsole.h"
#include "Level.h"
#include "Actor.h"
#include "CameraFirstEye.h"
#include "MainMenu.h"
#include "string_table.h"
#include <array>
#include "UIGameCustom.h"
#include "UIGameSP.h"
#include "UICursor.h"
#include "xr_level_controller.h"
#include "ui/UIDialogWnd.h"
#include "ui/UICustomEdit.h"
#include "ui/UIEditKeyBind.h"
#include "ui/UI3tButton.h"
#include "ui/UIButton.h"
#include "ui/UICheckButton.h"
#include "ui/UIRadioButton.h"
#include "ui/UIMessageBox.h"
#include "ui/UIMessageBoxEx.h"
#include "ui/UIComboBox.h"
#include "ui/UITrackBar.h"
#include "ui/UIListBox.h"
#include "ui/UIListBoxItem.h"
#include "ui/UITabControl.h"
#include "ui/UITabButton.h"
#include "ui/UIMMShniaga.h"
#include "ui/UIScrollView.h"
#include "ui/UIScrollBar.h"
#include "GamepadOnScreenKeyboard.h"
#include "GamepadCalibrationWnd.h"
#include "GamepadRemapWnd.h"
#include "ui/UIPdaWnd.h"
#include "PDA.h"
#include "ui/UITaskWnd.h"
#include "ui/UIMapWnd.h"
#include "ui/UIMap.h" // FIX-LIVE4: CUIGlobalMap::GetAbsolutePos (метка у края карты)
#include "ui/UIMapLegend.h"
#include "ui/UIPropertiesBox.h"
#include "map_spot.h"
#include "ui/UITalkWnd.h"
#include "ui/UITalkDialogWnd.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIGameTutorial.h"
#include "UsableScriptObject.h"
#include "InventoryBox.h"
#include "InventoryOwner.h"
#include "holder_custom.h"
#include "entity_alive.h"
#include "ai_space.h"
#include "Inventory.h"
#include "Weapon.h"
#include "WeaponMagazined.h"
#include "WeaponMagazinedWGrenade.h"
#include "Actor_Flags.h"
#include "../xrEngine/GamepadMath.h"
#include "../xrEngine/GamepadBindings.h"
#include "../xrServerEntities/script_engine.h"
#include <luabind/luabind.hpp>
#include <dinput.h>

extern bool g_bDisableAllInput;
extern CUISequencer* g_tutorial; // level_script.cpp: текущий туториал game.start_tutorial (E2.2c)

// E2.2d / R1-Engine: "радиус подсказок" для короткого X и "липкого" прицела -- угол (градусы) вокруг прицела, в котором
// интерактивный объект (тайник, труп, говорящий НПС, предмет, usable-объект) подхватывается как цель взгляда и использования.
// 0 -- выключено (только точное наведение, как в E2.2). Консоль gp_x_target_angle.
float ps_gp_x_target_angle = 14.f;
extern bool g_block_all_except_movement;
extern ENGINE_API int ps_gp_telemetry;
extern int ps_gp_inv_focus_frame; // UICursor.cpp (INV-REWORK-1)
// FIX-LIVE3 п.19/20 (Миша 2026-11-16): удержание View для КПК — 0,4 с (было 0,7). Мировой View открывает рюкзак
// сразу на нажатии (FDDA backpack_open играет уже при открытом окне), поэтому удержание отслеживается по сырой
// маске независимо от контекста: если View всё ещё зажат через 0,4 с — рюкзак закрывается и открывается КПК.
constexpr u32 GP_VIEW_PDA_HOLD_MS = 400;
constexpr u32 GP_VIEW_PDA_HOLD_PAUSE_MS = 150;
static bool s_gpViewLateArmed = false;
static u32 s_gpViewLateStart = 0;
extern int ps_gp_pda_reticle_size; // UICursor.cpp (FIX-LIVE3 п.13)
extern float ps_gp_cursor_size;   // UICursor.cpp (FG.5c / CURSOR1)
extern float ps_gp_cursor_speed;  // UICursor.cpp (CURSOR1)

namespace
{
	// Доступ к protected-полю CUIWindow::m_pKeyboardCapturer без модификации заголовка UIWindow.h
	struct CUIWindowKeyboardCapturerProbe : public CUIWindow
	{
		static CUIWindow* GetCapturer(CUIWindow* w)
		{
			return w ? static_cast<CUIWindowKeyboardCapturerProbe*>(w)->m_pKeyboardCapturer : nullptr;
		}
	};

	// Доступ к protected-полям CUIMMShniaga (списки кнопок текущей страницы и текущий выбранный пункт m_selected)
	struct CUIMMShniagaButtonsProbe : public CUIMMShniaga
	{
		static void GatherButtons(CUIMMShniaga* shn, xr_vector<CUIWindow*>& outBtns)
		{
			if (!shn)
				return;
			const auto* probe = static_cast<const CUIMMShniagaButtonsProbe*>(shn);
			const xr_vector<CUITextWnd*>* src = &probe->m_buttons;
			if (probe->m_page == epi_new_game)
				src = &probe->m_buttons_new;
			else if (probe->m_page == epi_new_network_game)
				src = &probe->m_buttons_new_network;
			for (CUITextWnd* btn : *src)
			{
				if (btn && btn->IsShown())
					outBtns.push_back(btn);
			}
		}

		static CUIWindow* GetSelected(CUIMMShniaga* shn)
		{
			if (!shn)
				return nullptr;
			CUIWindow* sel = static_cast<const CUIMMShniagaButtonsProbe*>(shn)->m_selected;
			return (sel && sel->IsShown()) ? sel : nullptr;
		}

		static CUIMMShniaga* FindShownIn(CUIWindow* root, int depth = 0)
		{
			if (!root || !root->IsShown() || depth > 4)
				return nullptr;
			if (CUIMMShniaga* shn = smart_cast<CUIMMShniaga*>(root))
				return shn;
			for (auto it = root->GetChildWndList().begin(); it != root->GetChildWndList().end(); ++it)
			{
				if (CUIMMShniaga* found = FindShownIn(*it, depth + 1))
					return found;
			}
			return nullptr;
		}
	};

	// MENU-FOCUS-1: доступ к protected-полям CUIComboBox (m_list_frame и m_frameLine) без правки UIComboBox.h
	struct CUIComboBoxProbe : public CUIComboBox
	{
		static bool IsExpanded(CUIComboBox* cb)
		{
			return cb && (cb->*(&CUIComboBoxProbe::m_list_frame)).IsShown();
		}

		static void Close(CUIComboBox* combo)
		{
			(combo->*(&CUIComboBoxProbe::ShowList))(false);
		}

		static void StepClosed(CUIComboBox* combo, int direction)
		{
			auto& list = combo->m_list_box;
			const int count = static_cast<int>(list.GetSize());
			const auto selected = list.GetSelectedIDX();
			int index = selected < static_cast<u32>(count) ? static_cast<int>(selected) : (direction > 0 ? -1 : 0);
			for (int i = 0; i < count; ++i)
			{
				index = (index + direction + count) % count;
				auto* item = list.GetItemByIDX(index);
				if (item && item->IsEnabled() && item->IsShown() && item != list.GetSelectedItem())
				{
					list.SetSelected(item);
					(combo->*(&CUIComboBoxProbe::OnListItemSelect))();
					return;
				}
			}
		}

		static void GetHeaderRect(CUIComboBox* cb, Frect& outRect)
		{
			if (!cb)
			{
				outRect.set(0.0f, 0.0f, 0.0f, 0.0f);
				return;
			}
			(cb->*(&CUIComboBoxProbe::m_frameLine)).GetAbsoluteRect(outRect);
		}
	};

	// MENU-FOCUS-1: доступ к protected-отступам CUIScrollView::m_upIndent / m_downIndent для точной области отсечения
	struct CUIScrollViewProbe : public CUIScrollView
	{
		static void GetVisibleRect(CUIScrollView* sv, Frect& outRect)
		{
			if (!sv)
			{
				outRect.set(0.0f, 0.0f, 0.0f, 0.0f);
				return;
			}
			sv->GetAbsoluteRect(outRect);
			outRect.y1 += sv->*(&CUIScrollViewProbe::m_upIndent);
			outRect.y2 -= sv->*(&CUIScrollViewProbe::m_downIndent);
		}
	};

	// Доступ к protected-полю CUIMainIngameWnd::m_pPickUpItem без модификации заголовка UIMainIngameWnd.h
	struct CUIMainIngameWndPickupProbe : public CUIMainIngameWnd
	{
		static CInventoryItem* GetPickUpItem(CUIMainIngameWnd* w)
		{
			return w ? static_cast<CUIMainIngameWndPickupProbe*>(w)->m_pPickUpItem : nullptr;
		}
	};

	// E3.1 + R1-Engine: доступ к protected-полям CActor (m_pObjectWeLookingAt, m_pPersonWeLookingAt, m_pInvBoxWeLookingAt,
	// m_pUsableObject, m_bPickupMode, m_fPickupInfoRadius) и методам PickupModeUpdate/PickupModeUpdate_COD без модификации Actor.h.
	// В G.A.M.M.A. по умолчанию включён g_multi_item_pickup on (AF_MULTI_ITEM_PICKUP), при котором CActor::ActorUse()
	// не взводит m_bPickupMode, а CActor::UpdateCL() опрашивает только физический массив клавиатуры pInput->iGetAsyncKeyState(dik).
	struct CActorPickupProbe : public CActor
	{
		static float GetPickupRadius(CActor* pActor)
		{
			if (!pActor)
				return 2.8f;
			const float r = static_cast<CActorPickupProbe*>(pActor)->m_fPickupInfoRadius;
			return _max(r, 2.8f);
		}

		static void SyncLookTargetFields(CActor* pActor, CObject* pTarget)
		{
			if (!pActor || !pTarget)
				return;
			CActorPickupProbe* probe = static_cast<CActorPickupProbe*>(pActor);
			CGameObject* go = smart_cast<CGameObject*>(pTarget);
			if (!go)
				return;
			probe->m_pObjectWeLookingAt = go;
			probe->m_pUsableObject = smart_cast<CUsableScriptObject*>(go);
			probe->m_pInvBoxWeLookingAt = smart_cast<CInventoryBox*>(go);
			probe->m_pPersonWeLookingAt = smart_cast<CInventoryOwner*>(go);
			probe->m_pVehicleWeLookingAt = smart_cast<CHolderCustom*>(go);
		}

		static bool TriggerGamepadWorldItemPickup(CActor* pActor, LPCSTR* outItemSect = nullptr)
		{
			if (!pActor || !pActor->g_Alive())
				return false;

			CGameObject* pLookObj = pActor->ObjectWeLookingAt();
			// Если взгляд направлен на не-предмет (дверь, рычаг, тайник) или персонажа/труп — подбор с земли не запускаем
			if ((pLookObj != nullptr && pLookObj->cast_inventory_item() == nullptr) || pActor->PersonWeLookingAt() != nullptr)
				return false;

			CInventoryItem* pLookItem = pLookObj ? pLookObj->cast_inventory_item() : nullptr;
			CInventoryItem* pCodItem = (CurrentGameUI() && CurrentGameUI()->UIMainIngameWnd)
			                               ? CUIMainIngameWndPickupProbe::GetPickUpItem(CurrentGameUI()->UIMainIngameWnd)
			                               : nullptr;
			CInventoryItem* pTargetItem = pLookItem ? pLookItem : pCodItem;
			if (!pTargetItem)
				return false;

			if (outItemSect)
				*outItemSect = pTargetItem->object().cNameSect().c_str();

			CActorPickupProbe* probe = static_cast<CActorPickupProbe*>(pActor);
			// Синхронизируем m_pObjectWeLookingAt / m_pUsableObject на время подбора, чтобы PickupModeUpdate() и колбэки видели предмет
			CGameObject* prevLookObj = probe->m_pObjectWeLookingAt;
			CUsableScriptObject* prevUsable = probe->m_pUsableObject;
			if (!pLookItem && pTargetItem->cast_game_object())
			{
				probe->m_pObjectWeLookingAt = pTargetItem->cast_game_object();
				probe->m_pUsableObject = smart_cast<CUsableScriptObject*>(probe->m_pObjectWeLookingAt);
			}
			const bool prevPickup = probe->m_bPickupMode;
			probe->m_bPickupMode = true;
			const pickup_result_t res = probe->PickupModeUpdate();
			probe->PickupModeUpdate_COD(res);
			probe->m_bPickupMode = prevPickup;
			if (!pLookItem)
			{
				probe->m_pObjectWeLookingAt = prevLookObj;
				probe->m_pUsableObject = prevUsable;
			}
			return true;
		}
	};

	// E2.2c: костёр G.A.M.M.A. -- зона без use-цели (ObjectWeLookingAt/DefaultAction пусты). Поджог и тушение идут через
	// туториал: bind_campfire.script actor_on_update запускает game.start_tutorial("tutorial_campfire_ignite|extinguish"),
	// когда актор ближе dist_cf к костру и не смотрит на объект; пункт туториала по kUSE вызывает bind_campfire.use_campfire
	// (комментарий "called from game_tutorials.xml"). Туториал захватывает ввод (CUISequencer::Start: IR_Capture), поэтому
	// физическая F сначала попадает в CUISequencer::IR_OnKeyboardPress -> CUISequenceSimpleItem::OnKeyboardPress -> функтор.
	// Доступ к protected-очереди пунктов без изменения UIGameTutorial.h.
	struct CUISequencerItemsProbe : public CUISequencer
	{
		static CUISequenceItem* Front(CUISequencer* s)
		{
			if (!s)
				return nullptr;
			CUISequencerItemsProbe* p = static_cast<CUISequencerItemsProbe*>(s);
			return p->m_sequencer_items.empty() ? nullptr : p->m_sequencer_items.front();
		}
	};

	// Чистая функция (selftest): есть ли в списке действий пункта туториала (m_actions) действие actionId
	template <class TIt>
	bool TutorialActionItemsContain(TIt first, TIt last, int actionId)
	{
		if (actionId < 0)
			return false;
		for (; first != last; ++first)
			if (static_cast<int>(first->m_action) == actionId)
				return true;
		return false;
	}

	// Активный туториал ждёт нажатия actionId (его текущий пункт вызовет свой Lua-функтор)
	bool ActiveTutorialListensForAction(int actionId)
	{
		if (actionId < 0 || g_tutorial == nullptr || !g_tutorial->IsActive())
			return false;
		CUISequenceSimpleItem* item = smart_cast<CUISequenceSimpleItem*>(CUISequencerItemsProbe::Front(g_tutorial));
		if (!item)
			return false;
		return TutorialActionItemsContain(item->m_actions.begin(), item->m_actions.end(), actionId);
	}

	// Чистая функция (selftest): короткое X -- kUSE, если есть HUD-цель или туториал ждёт kUSE; иначе перезарядка
	bool XTapChoosesUse(bool hasHudTarget, bool tutorialWantsUse)
	{
		return hasHudTarget || tutorialWantsUse;
	}

	bool HasHudTargetForUse()
	{
		CActor* pActor = g_actor;
		if (!pActor || !pActor->g_Alive())
			return false;

		if (pActor->ObjectWeLookingAt() != nullptr || pActor->PersonWeLookingAt() != nullptr)
			return true;

		LPCSTR defaultAct = pActor->GetDefaultActionForObject();
		if (defaultAct && defaultAct[0] != 0)
			return true;

		if (CurrentGameUI() && CurrentGameUI()->UIMainIngameWnd)
		{
			if (CUIMainIngameWndPickupProbe::GetPickUpItem(CurrentGameUI()->UIMainIngameWnd) != nullptr)
				return true;
		}
		return false;
	}

	// E2.2d: цели, о которых знает только Lua (скрипт ловит F рядом с чем-то без HUD-цели): gp_weapon.x_script_use_target()
	// возвращает "none" или id сработавшей проверки. Вызывается только в момент решения короткого X.
	bool QueryLuaScriptUseTarget(string128& outReason)
	{
		outReason[0] = 0;
		if (!g_ai_space)
			return false;
		::luabind::functor<LPCSTR> fn;
		if (!ai().script_engine().functor("gp_weapon.x_script_use_target", fn))
			return false;
		try
		{
			LPCSTR r = fn();
			if (r && r[0] && 0 != xr_strcmp(r, "none"))
			{
				strncpy_s(outReason, sizeof(outReason), r, _TRUNCATE);
				return true;
			}
		}
		catch (...)
		{
			Msg("! [Gamepad] Exception in gp_weapon.x_script_use_target");
		}
		return false;
	}

	// Чистая функция (selftest): точка center в конусе angleDeg вокруг camDir (нормализован) и не дальше maxDist
	bool XPointInTargetCone(const Fvector& camPos, const Fvector& camDir, const Fvector& center, float maxDist, float angleDeg)
	{
		if (angleDeg <= 0.f || maxDist <= 0.f)
			return false;
		Fvector d;
		d.sub(center, camPos);
		const float dist = d.magnitude();
		if (dist > maxDist)
			return false;
		if (dist < EPS_L)
			return true;
		const float cosA = d.dotproduct(camDir) / dist;
		return cosA >= _cos(deg2rad(_min(angleDeg, 89.f)));
	}

	// Объект, у которого штатно есть подсказка использования (те же классы, что в CActor::shedule_Update, Actor.cpp:2066-2131),
	// включая ящики/тайники, трупы, говорящих НПС, интерактивные объекты с tip_text и подбираемые предметы в мире.
	bool IsHintObjectForXCone(CObject* o, CActor* pActor)
	{
		CGameObject* go = smart_cast<CGameObject*>(o);
		if (!go || go == pActor || !go->getVisible() || go->getDestroy())
			return false;
		if (CInventoryItem* ii = go->cast_inventory_item())
		{
			if (go->H_Parent() == nullptr && ii->CanTake() && ii->Useful() && !Level().m_feel_deny.is_object_denied(go))
				return true;
		}
		if (CUsableScriptObject* u = smart_cast<CUsableScriptObject*>(go))
		{
			LPCSTR tip = u->tip_text();
			if (tip && tip[0])
				return true;
		}
		// транспорт не берём: посадка (ActorUse) идёт по точному лучу HUD().GetRQ(), а не по полям цели взгляда
		if (smart_cast<CInventoryBox*>(go))
			return true;
		if (CEntityAlive* ea = smart_cast<CEntityAlive*>(go))
		{
			if (!ea->g_Alive())
				return true; // труп человека или мутанта
			CInventoryOwner* io = smart_cast<CInventoryOwner*>(go);
			if (io && io->IsTalkEnabled())
				return true;
		}
		return false;
	}

	// Вычисляет приподнятую целевую точку объекта (и опционально нижнюю/базовую), чтобы лежащие на полу трупы,
	// ящики и предметы не отсекались ложным попаданием луча в пол и корректно попадали в конус камеры.
	void GetStickyTargetProbePoints(CObject* o, Fvector& outPrimary, Fvector& outSecondary)
	{
		Fvector c;
		o->Center(c);
		outSecondary = c;
		outPrimary = c;
		CGameObject* go = smart_cast<CGameObject*>(o);
		if (!go)
		{
			outPrimary.y += 0.20f;
			return;
		}
		if (CEntityAlive* ea = smart_cast<CEntityAlive*>(go))
		{
			if (!ea->g_Alive())
			{
				// Для лежащего трупа o->Center(c) может быть у самой земли или смещён: проверяем и приподнятую точку
				outPrimary.y += 0.35f;
				outSecondary.y += 0.12f;
				return;
			}
		}
		if (smart_cast<CInventoryBox*>(go))
		{
			outPrimary.y += 0.25f;
			outSecondary.y += 0.08f;
			return;
		}
		if (go->cast_inventory_item())
		{
			outPrimary.y += 0.15f;
			return;
		}
		outPrimary.y += 0.20f;
	}

	// Проверка видимости от камеры до целевой точки без ложного отсечения полом у самого объекта
	bool HasLineOfSightToStickyTarget(const Fvector& camPos, const Fvector& targetPt, float objRadius, CActor* pActor)
	{
		Fvector d;
		d.sub(targetPt, camPos);
		const float dist = d.magnitude();
		if (dist <= 0.20f)
			return true;
		d.div(dist);
		// Не доводим луч до самой поверхности пола/объекта на 0.30 м (или радиус объекта), чтобы не упереться в пол под трупом/ящиком
		const float traceDist = _max(dist - _max(objRadius, 0.30f), 0.05f);
		return !Level().ObjectSpace.RayTest(camPos, d, traceDist, collide::rqtStatic, nullptr, pActor);
	}

	// Объект с подсказкой рядом с прицелом (конус ps_gp_x_target_angle, рабочая 3D-дистанция от глаз ~3.0 м, без стены между)
	CObject* FindNearHintObject(CActor* pActor)
	{
		if (!pActor || ps_gp_x_target_angle <= 0.f || !g_SpatialSpace || !g_pGameLevel)
			return nullptr;
		const Fvector camPos = Device.vCameraPosition;
		Fvector camDir = Device.vCameraDirection;
		camDir.normalize_safe();
		// Камера на высоте глаз (~1.7-1.8 м): до трупа/ящика на земле в 1.8-2.2 м по горизонтали 3D-дистанция от глаз составляет ~2.5-3.0 м
		// INV-REWORK-24 (Misha 2026-11-12): same reach as the vanilla use key (Actor.cpp kMaxLookRange = 2 m to the
		// surface); the probe point is the object's centre, so maxDist below adds the object radius.
		const float kUseRange = 2.0f;
		xr_vector<ISpatialShared> found;
		g_SpatialSpace->q_sphere(found, 0, STYPE_COLLIDEABLE, camPos, kUseRange + 1.2f);
		float bestScore = -2.f;
		CObject* best = nullptr;
		for (ISpatialShared sp : found)
		{
			CObject* o = sp ? sp->dcast_CObject() : nullptr;
			if (!o || !IsHintObjectForXCone(o, pActor))
				continue;
			Fvector ptPrimary, ptSecondary;
			GetStickyTargetProbePoints(o, ptPrimary, ptSecondary);
			const float r = clampr(o->Radius(), 0.25f, 1.2f);
			const float maxDist = kUseRange + r;

			Fvector chosenPt = ptPrimary;
			bool inCone = XPointInTargetCone(camPos, camDir, ptPrimary, maxDist, ps_gp_x_target_angle);
			if (!inCone && XPointInTargetCone(camPos, camDir, ptSecondary, maxDist, ps_gp_x_target_angle))
			{
				chosenPt = ptSecondary;
				inCone = true;
			}
			if (!inCone)
			{
				Fvector rawCenter;
				o->Center(rawCenter);
				if (XPointInTargetCone(camPos, camDir, rawCenter, maxDist, ps_gp_x_target_angle))
				{
					chosenPt = rawCenter;
					inCone = true;
				}
			}
			if (!inCone)
				continue;

			// Проверяем видимость до приподнятой или резервной точки, чтобы пол под трупом/ящиком не блокировал луч
			if (!HasLineOfSightToStickyTarget(camPos, ptPrimary, r, pActor) &&
			    !HasLineOfSightToStickyTarget(camPos, ptSecondary, r, pActor))
			{
				continue;
			}

			Fvector d;
			d.sub(chosenPt, camPos);
			const float dist = d.magnitude();
			const float cosA = (dist > EPS_L) ? (d.dotproduct(camDir) / dist) : 1.f;
			// Приоритет контейнерам/трупам/usable перед мелкими предметами при почти равном угле
			CGameObject* go = smart_cast<CGameObject*>(o);
			const bool isItem = (go && go->cast_inventory_item() != nullptr);
			const float score = cosA - (isItem ? 0.015f : 0.0f);
			if (score > bestScore)
			{
				bestScore = score;
				best = o;
			}
		}
		return best;
	}

	bool FindNearHintObjectForX(LPCSTR* outSect, CObject** outObj = nullptr)
	{
		CObject* best = FindNearHintObject(g_actor);
		if (best && outSect)
			*outSect = best->cNameSect().c_str();
		if (outObj)
			*outObj = best;
		return best != nullptr;
	}

	// Чистая функция (selftest): "липкая" цель взгляда разрешена только при активном геймпаде и угле > 0
	bool StickyLookTargetAllowed(bool gamepadActiveSource, float angleDeg, bool externalInputHandler)
	{
		return gamepadActiveSource && angleDeg > 0.f && !externalInputHandler;
	}

} // namespace

// E2.2e: "липкая" цель взгляда для геймпада. Вызывается из CActor::shedule_Update (Actor.cpp), когда точно под прицелом
// ближе 2 м ничего нет. Возвращает объект с подсказкой в конусе gp_x_target_angle -- от него игра показывает подсказку
// использования (m_sDefaultObjAction), Dot Marks берёт цель (get_actor_object_looking_at), F/X используют его (ActorUse).
// Мышь/клавиатура (IsGamepadActiveSource() == false) -- nullptr, поведение без изменений.
CObject* GamepadFindStickyUseTarget(CActor* pActor)
{
	if (!pActor || !pInput)
		return nullptr;
	if (!StickyLookTargetAllowed(pInput->IsGamepadActiveSource(), ps_gp_x_target_angle, pActor->input_external_handler_installed()))
		return nullptr;
	return FindNearHintObject(pActor);
}

namespace
{
	enum class EXTapTarget
	{
		None,     // перезарядка
		Hud,      // объект под прицелом (E2.2)
		Tutorial, // туториал ждёт kUSE (E2.2c, костёр)
		Script,   // Lua-проверка gp_weapon.x_script_use_target (E2.2d)
		Near      // объект с подсказкой рядом с прицелом (E2.2d / R1-Engine, gp_x_target_angle): синхронизирует цель взгляда и выполняет kUSE
	};

	// Чистая функция (selftest): порядок приоритета целей короткого X
	EXTapTarget XTapTargetFromFlags(bool hud, bool tutorial, bool script, bool nearHint)
	{
		if (hud)
			return EXTapTarget::Hud;
		if (tutorial)
			return EXTapTarget::Tutorial;
		if (script)
			return EXTapTarget::Script;
		if (nearHint)
			return EXTapTarget::Near;
		return EXTapTarget::None;
	}

	// Проверки идут по порядку и останавливаются на первой сработавшей (Lua и конус -- только если нет HUD-цели/туториала)
	EXTapTarget ResolveXTapTarget(string128& outDetail)
	{
		outDetail[0] = 0;
		CActor* pActor = g_actor;
		if (!pActor || !pActor->g_Alive())
			return EXTapTarget::None;
		if (HasHudTargetForUse())
		{
			xr_strcpy(outDetail, "hud_target");
			return EXTapTarget::Hud;
		}
		if (ActiveTutorialListensForAction(kUSE))
		{
			xr_strcpy(outDetail, "tutorial_kUSE");
			return EXTapTarget::Tutorial;
		}
		string128 reason;
		if (QueryLuaScriptUseTarget(reason))
		{
			xr_sprintf(outDetail, sizeof(outDetail), "script:%s", reason);
			return EXTapTarget::Script;
		}
		LPCSTR sect = nullptr;
		CObject* nearObj = nullptr;
		if (FindNearHintObjectForX(&sect, &nearObj))
		{
			// R1-Engine: если объект найден в конусе между тиками shedule_Update, сразу синхронизируем поля цели взгляда CActor,
			// чтобы последующий kUSE -> ActorUse() / TriggerGamepadWorldItemPickup() открыл ящик/труп или подобрал предмет
			if (nearObj && !pActor->input_external_handler_installed())
				CActorPickupProbe::SyncLookTargetFields(pActor, nearObj);
			xr_sprintf(outDetail, sizeof(outDetail), "near_target:%s", sect ? sect : "object");
			return EXTapTarget::Near;
		}
		return EXTapTarget::None;
	}

	bool HasInteractiveTargetForUse()
	{
		string128 detail;
		return ResolveXTapTarget(detail) != EXTapTarget::None;
	}

	bool IsWorldGameplayLikeContext(EGamepadContext ctx)
	{
		return ctx == EGamepadContext::World_Gameplay ||
		       ctx == EGamepadContext::World_VehicleOrHolder ||
		       ctx == EGamepadContext::Cutscene_InputBlock;
	}

	bool HasActiveCustomEditInHolder(CDialogHolder* holder)
	{
		if (CGamepadOnScreenKeyboardUI::Instance().IsOpen())
			return true;

		if (!holder)
			return false;

		CUIDialogWnd* top = holder->TopInputReceiver();
		if (!top || !top->IsShown() || !top->IR_process())
			return false;

		CUIWindow* cur = top;
		for (int depth = 0; cur != nullptr && depth < 16; ++depth)
		{
			if (CUICustomEdit* edit = smart_cast<CUICustomEdit*>(cur))
			{
				if (edit->IsShown() && edit->IsEnabled())
					return true;
			}
			cur = CUIWindowKeyboardCapturerProbe::GetCapturer(cur);
		}

		if (CGamepadOnScreenKeyboardUI::FindFocusedEditBoxInTree(top) != nullptr)
			return true;

		return false;
	}

	bool IsActorLookAtCameraActive(CActor* pActor)
	{
		if (!pActor)
			return false;

		if (CCameraBase* camFE = pActor->cam_FirstEye())
		{
			if (CCameraFirstEye* pFirstEye = dynamic_cast<CCameraFirstEye*>(camFE))
			{
				if (pFirstEye->IsLookAtActive())
					return true;
			}
		}
		return false;
	}
} // namespace

CGamepadContextDispatcher& CGamepadContextDispatcher::Instance()
{
	static CGamepadContextDispatcher s_instance;
	return s_instance;
}

LPCSTR CGamepadContextDispatcher::GetContextName(EGamepadContext ctx)
{
	switch (ctx)
	{
	case EGamepadContext::Console:                   return "Console";
	case EGamepadContext::TextEntry:                 return "TextEntry";
	case EGamepadContext::MainMenu:                  return "MainMenu";
	case EGamepadContext::Cutscene_InputBlock:       return "Cutscene_InputBlock";
	case EGamepadContext::Sleep:                     return "Sleep";
	case EGamepadContext::Dialog_NPC:                return "Dialog_NPC";
	case EGamepadContext::RadialWheel:               return "RadialWheel";
	case EGamepadContext::Placement_HideoutFurniture: return "Placement_HideoutFurniture";
	case EGamepadContext::World_3DPDA:               return "World_3DPDA";
	case EGamepadContext::MenuUI_Modal:              return "MenuUI_Modal";
	case EGamepadContext::World_VehicleOrHolder:     return "World_VehicleOrHolder";
	case EGamepadContext::World_Gameplay:            return "World_Gameplay";
	default:                                         return "Unknown";
	}
}

u32 CGamepadContextDispatcher::GetContextPriorityNumber(EGamepadContext ctx)
{
	return static_cast<u32>(ctx) + 1u;
}

bool CGamepadContextDispatcher::IsContextLuaAssistedByDesign(EGamepadContext ctx)
{
	// D3.1 Стоп-условие: список контекстов из R3, полное или частичное определение которых требует Lua-моста D3.2:
	// - TextEntry: CUICustomEdit определяется из C++, но MCM keybind capture (ui_mcm.GUI.k_binder) — из Lua (D3.2)
	// - Cutscene_InputBlock: g_bDisableAllInput / m_FPCam / only_allow_movekeys из C++, но liz_fdda_input_manager — из Lua (D3.2)
	// - Sleep: UISleep (CUIScriptWnd) без имени окна не отличим от общего MenuUI_Modal без флага из Lua _G._GUIs["UISleep"] (D3.2)
	// - RadialWheel: !StopAnyMove() определяется из C++, но конкретный вид колеса (Haru/Pie/Camo/Ammo/Companion) — из Lua (D3.2)
	// - Placement_HideoutFurniture: состояния HOLDING(2) и PREVIEW(4) не имеют CUIDialogWnd и живут только в Lua placeable_furniture (D3.2)
	switch (ctx)
	{
	case EGamepadContext::TextEntry:
	case EGamepadContext::Cutscene_InputBlock:
	case EGamepadContext::Sleep:
	case EGamepadContext::RadialWheel:
	case EGamepadContext::Placement_HideoutFurniture:
		return true;
	default:
		return false;
	}
}

LPCSTR CGamepadContextDispatcher::GetNavDirName(ENavDir dir)
{
	switch (dir)
	{
	case ENavDir::Up:    return "up";
	case ENavDir::Down:  return "down";
	case ENavDir::Left:  return "left";
	case ENavDir::Right: return "right";
	default:             return "none";
	}
}

LPCSTR CGamepadContextDispatcher::GetRadialWheelTypeName(ERadialWheelType type)
{
	switch (type)
	{
	case ERadialWheelType::QuickAction_Haru: return "QuickAction_Haru";
	case ERadialWheelType::Interactive_Pie:  return "Interactive_Pie";
	case ERadialWheelType::Camo_3DSS:        return "Camo_3DSS";
	case ERadialWheelType::Ammo:             return "Ammo";
	case ERadialWheelType::Companion_Flat:   return "Companion_Flat";
	case ERadialWheelType::CppHeuristic:     return "CppHeuristic";
	default:                                 return "None";
	}
}

LPCSTR CGamepadContextDispatcher::GetPlacementStateName(EPlacementState state)
{
	switch (state)
	{
	case EPlacementState::Holding: return "Holding(2)";
	case EPlacementState::AdvCtrl: return "AdvCtrl(3)";
	case EPlacementState::Preview: return "Preview(4)";
	default:                       return "None(0)";
	}
}

static CUIDialogWnd* NewGameDialog(); // FIX-LIVE1 п.2: defined below, used by the TextEntry rule

EGamepadContext CGamepadContextDispatcher::EvaluateContextWaterfall(SContextSubFlags& outSubFlags, LPCSTR& outReason) const
{
	outSubFlags = {};
	outReason = "default_gameplay";

	const bool bMainMenuActive = (MainMenu() != nullptr && MainMenu()->IsActive());
	CUIGameCustom* pGameUI = CurrentGameUI();
	CUIDialogWnd* pGameTIR = pGameUI ? pGameUI->TopInputReceiver() : nullptr;
	if (pGameTIR && (!pGameTIR->IsShown() || !pGameTIR->IR_process()))
	{
		pGameTIR = nullptr;
	}

	// 1. Console (Приоритет 1 — закрывает C4.3-D6)
	if (Console != nullptr && Console->bVisible)
	{
		outReason = "console_visible";
		return EGamepadContext::Console;
	}

	// 2. TextEntry (Приоритет 2 — CUICustomEdit в активном CDialogHolder или MCM k_binder из D3.2)
	CDialogHolder* activeHolder = bMainMenuActive ? static_cast<CDialogHolder*>(MainMenu()) : static_cast<CDialogHolder*>(pGameUI);
	// FIX-LIVE1 п.2 (Миша 2026-11-15: в «Создании персонажа» не работает ни рамка, ни переходы, ни A):
	// имя в этом окне вводится только экранной клавиатурой (gp_new_game, A на поле имени), поэтому поле ввода
	// UINewGame (в т.ч. «залипший» захват клавиатуры после клика мышью) не должно уводить окно в TextEntry.
	if (HasActiveCustomEditInHolder(activeHolder) &&
	    !(bMainMenuActive && NewGameDialog() != nullptr && !CGamepadOnScreenKeyboardUI::Instance().IsOpen()))
	{
		outReason = bMainMenuActive ? "custom_edit_main_menu" : "custom_edit_game_ui";
		return EGamepadContext::TextEntry;
	}
	if (m_luaHints.bMcmKeybindCapture)
	{
		outSubFlags.bDetectedViaLuaHint = true;
		outReason = "lua_mcm_keybind_capture";
		return EGamepadContext::TextEntry;
	}

	// 3. MainMenu (Приоритет 3 — главное меню или уровень ещё не загружен)
	if (bMainMenuActive || g_pGameLevel == nullptr)
	{
		if (bMainMenuActive && MainMenu()->TopInputReceiver() != nullptr)
		{
			outSubFlags.bMainMenuHasModalSubDialog = true;
		}
		outReason = bMainMenuActive ? "main_menu_active" : "no_game_level";
		return EGamepadContext::MainMenu;
	}

	CActor* pActor = g_actor;

	// 4. Cutscene_InputBlock (Приоритет 4 — полная или частичная блокировка ввода)
	const bool bFullCutsceneBlock =
		g_bDisableAllInput ||
		m_luaHints.bActorSleepingInfo ||
		(pActor == nullptr) ||
		(!pActor->g_Alive()) ||
		(pActor->m_FPCam != nullptr) ||
		pActor->input_external_handler_installed() ||
		// FIX-LIVE1 (п.3/п.5): G.A.M.M.A. dialog_fov.script держит actor_look_at_point всё время разговора
		// (и торговли поверх него). Поворот камеры к собеседнику — не катсцена: при открытом окне
		// (TalkMenu / торговля / любой TopInputReceiver) он не должен перебивать Dialog_NPC / MenuUI_Modal.
		(IsActorLookAtCameraActive(pActor) && pGameTIR == nullptr && !pActor->IsTalking());

	if (bFullCutsceneBlock)
	{
		outSubFlags.bCutsceneFullBlock = true;
		outSubFlags.bDetectedViaLuaHint = m_luaHints.bActorSleepingInfo;
		if (g_bDisableAllInput)
			outReason = "g_bDisableAllInput";
		else if (m_luaHints.bActorSleepingInfo)
			outReason = "lua_actor_is_sleeping";
		else if (!pActor || !pActor->g_Alive())
			outReason = "actor_dead_or_null";
		else if (pActor->m_FPCam != nullptr)
			outReason = "actor_fpcam_cutscene";
		else if (pActor->input_external_handler_installed())
			outReason = "actor_external_input_handler";
		else
			outReason = "camera_first_eye_lookat";
		return EGamepadContext::Cutscene_InputBlock;
	}

	// Частичная блокировка (level.only_allow_movekeys / liz_fdda_input_manager):
	// срабатывает, когда поверх мира не открыто модальное диалоговое окно (в CLevel::IR_OnKeyboardPress UI опрашивается до g_block_all_except_movement)
	const bool bFddaActive = m_luaHints.bFddaInputDisabled &&
	                         (m_bInSelfTest || m_fddaDisabledSinceMs == 0 ||
	                          Device.dwTimeContinual <= m_fddaDisabledSinceMs ||
	                          (Device.dwTimeContinual - m_fddaDisabledSinceMs) <= 10000);
	if ((g_block_all_except_movement || bFddaActive) && pGameTIR == nullptr)
	{
		outSubFlags.bPartialMoveOnlyBlock = true;
		outSubFlags.bDetectedViaLuaHint = (!g_block_all_except_movement && bFddaActive);
		outReason = g_block_all_except_movement ? "only_allow_movekeys" : "lua_fdda_input_disabled";
		return EGamepadContext::Cutscene_InputBlock;
	}

	// 5. Sleep (Приоритет 5 — окно сна UISleep, помечено для D3.2 Lua-моста + проверка WindowName)
	const bool bWindowNamedSleep = (pGameTIR != nullptr && pGameTIR->WindowName().size() > 0 &&
	                                0 == xr_strcmp(pGameTIR->WindowName().c_str(), "UISleep"));
	if (bWindowNamedSleep || (m_luaHints.bSleepDialogShown && !pGameTIR))
	{
		outSubFlags.bDetectedViaLuaHint = m_luaHints.bSleepDialogShown;
		outReason = m_luaHints.bSleepDialogShown ? "lua_sleep_dialog" : "window_name_UISleep";
		return EGamepadContext::Sleep;
	}

	// 6. Dialog_NPC (Приоритет 6 — окно диалога с НПС CUITalkWnd)
	CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(pGameUI);
	const bool bTalkMenuShown = (pGameSP != nullptr && pGameSP->TalkMenu != nullptr && pGameSP->TalkMenu->IsShown());
	const bool bTalkIsTopReceiver = (bTalkMenuShown && (pGameTIR == pGameSP->TalkMenu || pGameTIR == nullptr));
	const bool bActorTalkingWithoutOtherModal = (pActor != nullptr && pActor->IsTalking() &&
	                                             (pGameTIR == nullptr || (pGameSP && pGameTIR == pGameSP->TalkMenu)));
	if (bTalkIsTopReceiver || bActorTalkingWithoutOtherModal)
	{
		outReason = bTalkIsTopReceiver ? "talk_menu_shown" : "actor_is_talking";
		return EGamepadContext::Dialog_NPC;
	}

	// 7. RadialWheel (Приоритет 7 — радиальные колёса / колесо напарника)
	if (m_luaHints.radialWheelType != ERadialWheelType::None)
	{
		outSubFlags.bDetectedViaLuaHint = true;
		outSubFlags.bModalAllowsMovement = (pGameTIR == nullptr || !pGameTIR->StopAnyMove());
		outReason = "lua_radial_wheel";
		return EGamepadContext::RadialWheel;
	}
	// C++ эвристика для скриптовых оверлеев с AllowMovement(true), кроме 3D КПК, режима мебели ADV_CTRL
	// и модальных окон, уже отслеживаемых мостом Lua (например, UIInventory:inventory с AllowMovement(true))
	const bool bIsPdaTopReceiver = (pGameUI != nullptr && pGameTIR == &pGameUI->GetPdaMenu());
	if (!m_bGamepadMouseMode && pGameTIR != nullptr && !pGameTIR->StopAnyMove() && !bIsPdaTopReceiver &&
	    m_luaHints.placementState != EPlacementState::AdvCtrl &&
	    m_luaHints.modalMenuTag.size() == 0 && m_luaHints.luaHintSyncCount == 0)
	{
		outSubFlags.bModalAllowsMovement = true;
		outReason = "cpp_overlay_allow_movement";
		return EGamepadContext::RadialWheel;
	}

	// 8. Placement_HideoutFurniture (Приоритет 8 — размещение мебели Hideout Furniture, помечено для D3.2)
	if (m_luaHints.placementState != EPlacementState::None)
	{
		outSubFlags.bDetectedViaLuaHint = true;
		outSubFlags.bModalAllowsMovement = (m_luaHints.placementState == EPlacementState::Holding);
		outReason = "lua_placement_hideout_furniture";
		return EGamepadContext::Placement_HideoutFurniture;
	}

	// 9. World_3DPDA (Приоритет 9 — 3D КПК в руках: активный экран или опущенный freelook)
	if (pGameUI != nullptr && pGameUI->GetPdaMenu().IsShown())
	{
		// Если поверх 3D КПК не открыто другое модальное окно (например, инвентарь или MessageBox)
		if (pGameTIR == nullptr || pGameTIR == &pGameUI->GetPdaMenu())
		{
			outSubFlags.b3dPdaInteractive = pGameUI->GetPdaMenu().IsEnabled() && (pGameTIR == &pGameUI->GetPdaMenu());
			outSubFlags.bModalAllowsMovement = !pGameUI->GetPdaMenu().StopAnyMove();
			outReason = outSubFlags.b3dPdaInteractive ? "3d_pda_interactive" : "3d_pda_freelook";
			return EGamepadContext::World_3DPDA;
		}
	}

	// 10. MenuUI_Modal (Приоритет 10 — любое модальное окно CUIDialogWnd на CurrentGameUI()->TopInputReceiver())
	if (pGameTIR != nullptr)
	{
		outSubFlags.bModalAllowsMovement = !pGameTIR->StopAnyMove();
		outSubFlags.bDetectedViaLuaHint = (m_luaHints.modalMenuTag.size() > 0);
		outReason = (m_luaHints.modalMenuTag.size() > 0) ? m_luaHints.modalMenuTag.c_str() : "top_input_receiver_modal";
		return EGamepadContext::MenuUI_Modal;
	}

	// 11. World_VehicleOrHolder (Приоритет 11 — транспорт CCar или станковое оружие CWeaponStatMgun)
	if (pActor != nullptr && pActor->Holder() != nullptr)
	{
		outReason = "actor_holder_active";
		return EGamepadContext::World_VehicleOrHolder;
	}

	// 12. World_Gameplay (Приоритет 12 — штатный геймплей в мире от первого лица)
	outReason = "world_gameplay";
	return EGamepadContext::World_Gameplay;
}

u32 CGamepadContextDispatcher::SampleRawButtonsAndTriggersMask()
{
	if (!pInput)
	{
		m_bLtDigitalDown = false;
		m_bRtDigitalDown = false;
		return 0;
	}

	const auto& gp = pInput->GetGamepadState();
	if (!gp.connected || !gp.has_focus || gp.need_release_all)
	{
		m_bLtDigitalDown = false;
		m_bRtDigitalDown = false;
		return 0;
	}

	u32 mask = gp.buttons_mask & ((1u << VBTN_PHYS_COUNT) - 1u);

	// E2.1 (1): кнопочный порог курков с гистерезисом GamepadMath (press/release из профиля калибровки D2.2/D37).
	// Значения уже санированы (press - release >= kTriggerMinGap), поэтому дребезга на границе нет.
	const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
	m_bLtDigitalDown = gpmath::UpdateTriggerButton(prof.left_trigger, m_bLtDigitalDown, gpmath::NormalizeTriggerAxis(gp.axes[4]));
	m_bRtDigitalDown = gpmath::UpdateTriggerButton(prof.right_trigger, m_bRtDigitalDown, gpmath::NormalizeTriggerAxis(gp.axes[5]));

	if (m_bLtDigitalDown)
		mask |= (1u << VBTN_LT);
	if (m_bRtDigitalDown)
		mask |= (1u << VBTN_RT);

	return mask;
}

CGamepadContextDispatcher::ENavDir CGamepadContextDispatcher::SampleStickCardinalDirection(s16 lxRaw, s16 lyRaw)
{
	const float lx = static_cast<float>(lxRaw) / 32767.0f;
	const float ly = static_cast<float>(lyRaw) / 32767.0f;
	// MENU-FOCUS-1: в меню порог отклонения стика для прыжка 50%, в остальных контекстах прежние 55%
	const float ENTER_THRESHOLD = (m_currentContext == EGamepadContext::MainMenu) ? MENU_FOCUS_STICK_ENTER_THRESHOLD : 0.55f;
	constexpr float EXIT_THRESHOLD = 0.35f;

	if (m_stickCardinalDir == ENavDir::Up && ly > -EXIT_THRESHOLD)
		m_stickCardinalDir = ENavDir::None;
	else if (m_stickCardinalDir == ENavDir::Down && ly < EXIT_THRESHOLD)
		m_stickCardinalDir = ENavDir::None;
	else if (m_stickCardinalDir == ENavDir::Left && lx > -EXIT_THRESHOLD)
		m_stickCardinalDir = ENavDir::None;
	else if (m_stickCardinalDir == ENavDir::Right && lx < EXIT_THRESHOLD)
		m_stickCardinalDir = ENavDir::None;

	if (m_stickCardinalDir == ENavDir::None)
	{
		if (_abs(lx) >= _abs(ly))
		{
			if (lx <= -ENTER_THRESHOLD)
				m_stickCardinalDir = ENavDir::Left;
			else if (lx >= ENTER_THRESHOLD)
				m_stickCardinalDir = ENavDir::Right;
		}
		else
		{
			if (ly <= -ENTER_THRESHOLD)
				m_stickCardinalDir = ENavDir::Up;
			else if (ly >= ENTER_THRESHOLD)
				m_stickCardinalDir = ENavDir::Down;
		}
	}

	return m_stickCardinalDir;
}

namespace
{
u32 MenuRepeatStepMs(u32 heldMs, bool trackbar)
{
	if (heldMs >= 1000)
		return trackbar ? 40 : 63;
	if (trackbar && heldMs >= 600)
		return 80;
	return CGamepadContextDispatcher::MENU_FOCUS_REPEAT_STEP_MS;
}
}

static CUIDialogWnd* NewGameDialog()
{
    auto* root = MainMenu() && MainMenu()->IsActive() ? MainMenu()->TopInputReceiver() : nullptr;
    return root && root->IsShown() && root->WindowName() == "GACreateCharacter" ? root : nullptr;
}

static bool IsLightingDialog(CUIDialogWnd* root)
{
    return root && root->IsShown() && root->WindowName() == "UILightControl";
}

void CGamepadContextDispatcher::StepStateMachine(EGamepadContext evaluatedCtx,
                                                 const SContextSubFlags& evaluatedSubFlags,
                                                 LPCSTR evalReason,
                                                 u32 rawButtonsMask,
                                                 s16 lxRaw,
                                                 s16 lyRaw,
                                                 u32 nowContinual,
                                                 bool emitTelemetry)
{
	m_rawDownMask = rawButtonsMask;
	// FIX-LIVE2 п.23 (Миша 2026-11-16): LS, отклонённый ещё при входе в модальное окно (обыск ящика/трупа по X на ходу)
	// и ни разу не отпущенный внутри окна, после закрытия окна продолжает движение без барьера «до нейтрали».
	static bool s_lsCarriedIntoModal = false;

	// 1. Снимаем барьер consumed_until_release с тех кнопок, которые физически отпущены в этом кадре
	m_consumedUntilReleaseMask &= m_rawDownMask;

	// 2. Проверяем смену контекста (каскад приоритетов оценивается 1 раз за кадр)
	// F1.2: каждое модальное окно в стеке — отдельный контекст диспетчера D3.1 с барьером consumed_until_release
	const LPCSTR newModalLayerTag = (evaluatedCtx == EGamepadContext::MenuUI_Modal && m_luaHints.modalMenuTag.size() > 0)
	                                ? m_luaHints.modalMenuTag.c_str()
	                                : "";
	const LPCSTR prevModalLayerTag = m_activeModalLayerTag.size() ? m_activeModalLayerTag.c_str() : "";
	// FIX-LIVE3 п.12 (Миша 2026-11-16): после закрытия обыска (ящик/труп/тайник/мутант) игрок сразу идёт
	// по текущему положению LS — без барьера «до нейтрали», даже если стик внутри окна отпускали.
	const bool bPrevLayerWasSearch = 0 == strncmp(prevModalLayerTag, "UIInventory:loot", 16) ||
	                                 0 == xr_strcmp(prevModalLayerTag, "UIMutantLoot");
	const bool bModalLayerChanged = (evaluatedCtx == EGamepadContext::MenuUI_Modal &&
	                                 m_currentContext == EGamepadContext::MenuUI_Modal &&
	                                 0 != xr_strcmp(prevModalLayerTag, newModalLayerTag));

	if (!m_bInitialized)
	{
		m_bInitialized = true;
		m_currentContext = evaluatedCtx;
		m_prevContext = evaluatedCtx;
		m_subFlags = evaluatedSubFlags;
		m_activeModalLayerTag = newModalLayerTag;
		m_lastTransitionReason = evalReason ? evalReason : "init";
		m_lastTransitionTimeContinual = nowContinual;
		m_bContextChangedThisFrame = false;
		m_releasedOnContextExitMask = 0;
		// При первом кадре инициализации маскируем уже зажатые кнопки до отпускания
		m_consumedUntilReleaseMask |= m_rawDownMask;
	}
	else if (evaluatedCtx != m_currentContext || bModalLayerChanged)
	{
		m_prevContext = m_currentContext;
		m_currentContext = evaluatedCtx;
		m_subFlags = evaluatedSubFlags;
		m_activeModalLayerTag = newModalLayerTag;
		m_lastTransitionReason = evalReason ? evalReason : (bModalLayerChanged ? "modal_layer_transition" : "transition");
		m_lastTransitionTimeContinual = nowContinual;
		++m_transitionCount;
		m_bContextChangedThisFrame = true;

		// REC-2: старый контекст (или предыдущее модальное окно в стеке F1.2) получает чистый синтетический Released
		// для всех кнопок, которые были эффективно зажаты в нём, а все физически зажатые в момент перехода кнопки
		// блокируются в новом контексте/слое до физического отпускания!
		m_releasedOnContextExitMask = m_prevEffectiveDownMask;
		m_consumedUntilReleaseMask |= m_rawDownMask;
		m_prevEffectiveDownMask = 0;
		m_bLbPending = false;
		m_bLbHoldCommitted = false;
		m_bEastDualModePending = false;
		m_bEastHoldCommitted = false;
		m_bR3DualModePending = false;
		m_bXPending = false;
		m_bXHoldCommitted = false;
		ZeroMemory(m_worldDualPending, sizeof(m_worldDualPending));
		ZeroMemory(m_worldDualHoldCommitted, sizeof(m_worldDualHoldCommitted));

		// Также сбрасываем автоповтор навигации и блокируем отклонённый левый стик до возврата в нейтраль
		m_activeNavDir = ENavDir::None;
		m_activeNavSource = "none";
		m_bNavTriggeredThisFrame = false;
		m_bNavTriggeredIsRepeat = false;
		const ENavDir rawStickDir = SampleStickCardinalDirection(lxRaw, lyRaw);
		m_bStickNavConsumedUntilNeutral = (rawStickDir != ENavDir::None);

		// FG.6h: при переходе между контекстами (или переключении interactive <-> freelook в 3D-КПК)
		// сбрасываем вектор движения актора, а при выходе из UI/модального контекста с отклонённым левым стиком
		// взводим барьер m_bStickMoveConsumedUntilNeutral до возврата стика в нейтраль, чтобы актор не «уехал».
		const float moveRawX = gpmath::NormalizeStickAxis(lxRaw);
		const float moveRawY = -gpmath::NormalizeStickAxis(lyRaw);
		const gpmath::Vec2 moveStick = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick, moveRawX, moveRawY);
		const bool bStickDeflectedForMove = (_abs(moveStick.x) > EPS_S || _abs(moveStick.y) > EPS_S);
		m_bStickMoveConsumedUntilNeutral = false;
		if (!AllowsWorldMove())
		{
			if (CActor* pActor = g_actor)
			{
				pActor->m_vGamepadMove.set(0.f, 0.f);
				pActor->m_bGamepadMoveActive = false;
				pActor->StopAnyMove();
			}
		}
		else if (m_prevContext == EGamepadContext::MenuUI_Modal ||
		         m_prevContext == EGamepadContext::World_3DPDA ||
		         m_prevContext == EGamepadContext::Dialog_NPC ||
		         m_prevContext == EGamepadContext::Sleep ||
		         m_prevContext == EGamepadContext::MainMenu ||
		         m_prevContext == EGamepadContext::TextEntry ||
		         m_prevContext == EGamepadContext::Console)
		{
			const bool carried = (s_lsCarriedIntoModal || bPrevLayerWasSearch) && m_prevContext == EGamepadContext::MenuUI_Modal;
			m_bStickMoveConsumedUntilNeutral = bStickDeflectedForMove && !carried;
			s_lsCarriedIntoModal = false;
			if (carried)
				m_lastTransitionReason = "modal_exit_ls_carried";
			if (CActor* pActor = g_actor)
			{
				pActor->m_vGamepadMove.set(0.f, 0.f);
				pActor->m_bGamepadMoveActive = false;
				pActor->StopAnyMove();
			}
		}

		if (emitTelemetry && ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line),
			           "gp_ctx_changed,%u,from=%s,to=%s,reason=%s,raw_mask=0x%05X,barrier_mask=0x%05X,exit_released=0x%05X",
			           nowContinual,
			           GetContextName(m_prevContext),
			           GetContextName(m_currentContext),
			           m_lastTransitionReason,
			           m_rawDownMask,
			           m_consumedUntilReleaseMask,
			           m_releasedOnContextExitMask);
			pInput->WriteGamepadTelemetryLine(line);
		}
	}
	else
	{
		const bool bPrev3dPdaInteractive = (m_currentContext == EGamepadContext::World_3DPDA && m_subFlags.b3dPdaInteractive);
		m_subFlags = evaluatedSubFlags;
		m_lastTransitionReason = evalReason ? evalReason : m_lastTransitionReason;
		m_bContextChangedThisFrame = false;
		m_releasedOnContextExitMask = 0;

		const bool bNow3dPdaInteractive = (m_currentContext == EGamepadContext::World_3DPDA && m_subFlags.b3dPdaInteractive);
		if (bPrev3dPdaInteractive != bNow3dPdaInteractive)
		{
			m_consumedUntilReleaseMask |= m_rawDownMask;
			if (bNow3dPdaInteractive)
			{
				m_bStickMoveConsumedUntilNeutral = false;
				if (CActor* pActor = g_actor)
				{
					pActor->m_vGamepadMove.set(0.f, 0.f);
					pActor->m_bGamepadMoveActive = false;
					pActor->StopAnyMove();
				}
			}
			else
			{
				const float moveRawX = gpmath::NormalizeStickAxis(lxRaw);
				const float moveRawY = -gpmath::NormalizeStickAxis(lyRaw);
				const gpmath::Vec2 moveStick = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick, moveRawX, moveRawY);
				m_bStickMoveConsumedUntilNeutral = (_abs(moveStick.x) > EPS_S || _abs(moveStick.y) > EPS_S);
				if (CActor* pActor = g_actor)
				{
					pActor->m_vGamepadMove.set(0.f, 0.f);
					pActor->m_bGamepadMoveActive = false;
					pActor->StopAnyMove();
				}
			}
		}
	}

	// 3. В контексте Console (Приоритет 1, C4.3-D6) все кнопки геймпада также подавляются (и при нажатии в консоли уходят в барьер)
	if (m_currentContext == EGamepadContext::Console)
	{
		m_consumedUntilReleaseMask |= m_rawDownMask;
	}

	// 4. Вычисляем эффективную маску и состояния Pressed / Held / Released в текущем контексте
	m_effectiveDownMask = m_rawDownMask & ~m_consumedUntilReleaseMask;
	m_pressedMask = m_effectiveDownMask & ~m_prevEffectiveDownMask;
	m_heldMask = m_effectiveDownMask;
	m_releasedMask = (~m_effectiveDownMask) & m_prevEffectiveDownMask;
	m_prevEffectiveDownMask = m_effectiveDownMask;

	// 5. Навигация D-Pad / левый стик с автоповтором на Device.dwTimeContinual (320 мс / 90 мс)
	m_bNavTriggeredThisFrame = false;
	m_bNavTriggeredIsRepeat = false;
	// MENU-FOCUS-1: в меню (MainMenu) автоповтор 350 мс / 125 мс (8 шагов/с), одинаково для крестовины и левого стика
	const bool bMenuFocusNav = (m_currentContext == EGamepadContext::MainMenu);
	const u32 navInitialDelayMs = bMenuFocusNav ? MENU_FOCUS_REPEAT_INITIAL_DELAY_MS : REPEAT_INITIAL_DELAY_MS;

	const ENavDir stickDir = SampleStickCardinalDirection(lxRaw, lyRaw);
	if (stickDir == ENavDir::None)
	{
		m_bStickNavConsumedUntilNeutral = false;
	}

	if (m_bContextChangedThisFrame)
	{
		if (m_currentContext == EGamepadContext::MenuUI_Modal && m_prevContext == EGamepadContext::World_Gameplay)
		{
			const gpmath::Vec2 mv = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick,
			                                             gpmath::NormalizeStickAxis(lxRaw), -gpmath::NormalizeStickAxis(lyRaw));
			s_lsCarriedIntoModal = (_abs(mv.x) > EPS_S || _abs(mv.y) > EPS_S);
		}
		else if (m_currentContext != EGamepadContext::MenuUI_Modal)
			s_lsCarriedIntoModal = false;
	}
	else if (s_lsCarriedIntoModal && m_currentContext == EGamepadContext::MenuUI_Modal)
	{
		const gpmath::Vec2 mv = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick,
		                                             gpmath::NormalizeStickAxis(lxRaw), -gpmath::NormalizeStickAxis(lyRaw));
		if (_abs(mv.x) <= EPS_S && _abs(mv.y) <= EPS_S)
			s_lsCarriedIntoModal = false;
	}

	if (m_bStickMoveConsumedUntilNeutral)
	{
		const float moveRawX = gpmath::NormalizeStickAxis(lxRaw);
		const float moveRawY = -gpmath::NormalizeStickAxis(lyRaw);
		const gpmath::Vec2 moveStick = gpmath::ProcessStick(gpmath::GetCalibrationProfile().left_stick, moveRawX, moveRawY);
		if (_abs(moveStick.x) <= EPS_S && _abs(moveStick.y) <= EPS_S)
		{
			m_bStickMoveConsumedUntilNeutral = false;
		}
	}

	if (m_currentContext == EGamepadContext::Console)
	{
		m_activeNavDir = ENavDir::None;
		m_activeNavSource = "none";
		if (stickDir != ENavDir::None)
			m_bStickNavConsumedUntilNeutral = true;
		return;
	}

	ENavDir candidateDir = ENavDir::None;
	LPCSTR candidateSource = "none";

	if (m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_UP))
	{
		candidateDir = ENavDir::Up;
		candidateSource = "dpad_up";
	}
	else if (m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_DOWN))
	{
		candidateDir = ENavDir::Down;
		candidateSource = "dpad_down";
	}
	else if (m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_LEFT))
	{
		candidateDir = ENavDir::Left;
		candidateSource = "dpad_left";
	}
	else if (m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_RIGHT))
	{
		candidateDir = ENavDir::Right;
		candidateSource = "dpad_right";
	}
	else if (!m_bStickNavConsumedUntilNeutral && stickDir != ENavDir::None)
	{
		candidateDir = stickDir;
		switch (stickDir)
		{
		case ENavDir::Up:    candidateSource = "lstick_up"; break;
		case ENavDir::Down:  candidateSource = "lstick_down"; break;
		case ENavDir::Left:  candidateSource = "lstick_left"; break;
		case ENavDir::Right: candidateSource = "lstick_right"; break;
		default: break;
		}
	}

	const bool vertical = candidateDir == ENavDir::Up || candidateDir == ENavDir::Down;
	const bool blockVertical = vertical && bMenuFocusNav && MainMenu() && MainMenu()->IsActive() &&
		GetUICursor().IsMenuFocusTransitionActive(MainMenu()->TopInputReceiver(), nowContinual);
	const bool resumeVertical = vertical && bMenuFocusNav && m_menuVerticalBlocked &&
		candidateDir == m_activeNavDir && !blockVertical;
	m_menuVerticalBlocked = blockVertical;

	if (candidateDir == ENavDir::None)
	{
		m_activeNavDir = ENavDir::None;
		m_activeNavSource = "none";
	}
	else if (candidateDir != m_activeNavDir || (bMenuFocusNav && xr_strcmp(candidateSource, m_activeNavSource) != 0))
	{
		m_activeNavDir = candidateDir;
		m_activeNavSource = candidateSource;
		m_navPressTimeContinual = nowContinual;
		m_navNextRepeatTimeContinual = nowContinual + navInitialDelayMs;
		m_bNavTriggeredThisFrame = true;
		m_bNavTriggeredIsRepeat = false;
	}
	else if (resumeVertical || static_cast<s32>(nowContinual - m_navNextRepeatTimeContinual) >= 0)
	{
		if (resumeVertical)
			m_navNextRepeatTimeContinual = nowContinual;
		const bool horizontal = candidateDir == ENavDir::Left || candidateDir == ENavDir::Right;
		const u32 navStepMs = bMenuFocusNav
			? MenuRepeatStepMs(nowContinual - m_navPressTimeContinual, horizontal && m_menuFocusOnTrackbar)
			: REPEAT_STEP_MS;
		u32 steps = 0;
		while (static_cast<s32>(nowContinual - m_navNextRepeatTimeContinual) >= 0 && steps < 8)
		{
			m_navNextRepeatTimeContinual += navStepMs;
			++steps;
		}
		if (static_cast<s32>(nowContinual - m_navNextRepeatTimeContinual) >= 0)
		{
			m_navNextRepeatTimeContinual = nowContinual + navStepMs;
		}
		m_activeNavSource = candidateSource;
		m_bNavTriggeredThisFrame = true;
		m_bNavTriggeredIsRepeat = true;
	}
	if (blockVertical)
	{
		m_bNavTriggeredThisFrame = false;
		m_bNavTriggeredIsRepeat = false;
	}
}

void CGamepadContextDispatcher::OnFrame()
{
	if (m_bInitialized && Device.dwFrame == m_lastUpdateFrame)
	{
		// Если внутри того же кадра открылась или закрылась консоль ~ (например, через Console->Show()/Hide()),
		// немедленно пересчитываем контекст, чтобы барьер и блокировка стиков сработали без задержки в 1 кадр.
		const bool bConsoleVisibleNow = (Console != nullptr && Console->bVisible);
		const bool bWasConsoleCtx = (m_currentContext == EGamepadContext::Console);
		if (bConsoleVisibleNow == bWasConsoleCtx)
			return;
	}
	m_lastUpdateFrame = Device.dwFrame;

	// D3.2 (REC-4): опрашиваем наблюдатель Lua-состояний в gp_bridge.script перед расчётом каскада контекстов.
	// Работает в том числе на паузе и при открытых модальных окнах (где actor_on_update не вызывается).
	// FR.1 №3: опрос Lua-моста не чаще раза в 50 мс (Device.dwTimeContinual). Прямые вызовы
	// PollLuaBridgeHints из мест смены состояния (консоль, модальные окна, нажатия) не ограничиваются.
	{
		static u32 s_lastLuaPollTimeMs = 0;
		static bool s_luaPollEverRan = false;
		const u32 nowMs = Device.dwTimeContinual;
		if (!s_luaPollEverRan || nowMs - s_lastLuaPollTimeMs >= 50u)
		{
			s_luaPollEverRan = true;
			s_lastLuaPollTimeMs = nowMs;
			PollLuaBridgeHints();
		}
	}
	if (!m_bInSelfTest && m_luaHints.bFddaInputDisabled && !g_block_all_except_movement &&
	    m_fddaDisabledSinceMs != 0 && Device.dwTimeContinual > m_fddaDisabledSinceMs &&
	    (Device.dwTimeContinual - m_fddaDisabledSinceMs) > 10000)
	{
		// Защита от «залипшего» флага FDDA после прерванной анимации
		m_luaHints.bFddaInputDisabled = false;
		m_fddaDisabledSinceMs = 0;
	}

	SContextSubFlags evalSubFlags = {};
	LPCSTR evalReason = "world_gameplay";
	const EGamepadContext evaluatedCtx = EvaluateContextWaterfall(evalSubFlags, evalReason);

	const u32 rawButtonsMask = SampleRawButtonsAndTriggersMask();
	s16 lxRaw = 0;
	s16 lyRaw = 0;
	if (pInput)
	{
		const auto& gp = pInput->GetGamepadState();
		if (gp.connected && gp.has_focus && !gp.need_release_all)
		{
			lxRaw = gp.axes[0];
			lyRaw = gp.axes[1];
		}
	}

	// FIX-LOADSCREEN: экран «Press Any Key / Enter the Zone» после загрузки — CGamePersistent::game_loaded()
	// стартует CUISequencer "game_loaded" (m_intro), который через IR_Capture перехватывает ввод CInput.
	// Геймпад же инжектит нажатия напрямую в Level()/MainMenu() (InjectKeyboardEvent), минуя cbStack,
	// поэтому CUISequenceSimpleItem с <guard_key>any</guard_key> никогда не получал OnKeyboardPress от геймпада.
	// Здесь: любая свежая кнопка/курок геймпада = «любая клавиша». Нажатие поглощается до отпускания,
	// чтобы не провалиться в мир (A = прыжок) в том же кадре. Выполняется ДО StepStateMachine.
	{
		// m_intro у CGamePersistent private: берём захвативший ввод приёмник CInput (CUISequencer::Start -> IR_Capture).
		// g_tutorial (туториалы уровня, экран смерти) обрабатывается отдельно; видео-пункты intro_game -- не SimpleItem.
		CUISequencer* const loadIntro = (!m_bInSelfTest && pInput && g_pGameLevel)
			? dynamic_cast<CUISequencer*>(pInput->CurrentIR())
			: nullptr;
		CUISequenceSimpleItem* const loadItem = (loadIntro && loadIntro != g_tutorial && loadIntro->IsActive())
			? smart_cast<CUISequenceSimpleItem*>(CUISequencerItemsProbe::Front(loadIntro))
			: nullptr;
		static CUISequenceSimpleItem* s_loadItem = nullptr;
		if (loadItem != s_loadItem)
		{
			s_loadItem = loadItem;
			if (loadItem)
				ConsumeAllCurrentlyHeldButtons(); // кнопки, зажатые ещё на «Загрузить», не закрывают экран
		}
		if (loadItem && loadItem->m_continue_dik_guard != -1)
		{
			const u32 freshMask = rawButtonsMask & ~m_rawDownMask & ~m_consumedUntilReleaseMask;
			if (freshMask != 0)
			{
				ConsumeAllCurrentlyHeldButtons();
				const int guardDik = (loadItem->m_continue_dik_guard > 0 && loadItem->m_continue_dik_guard != 9999)
					? loadItem->m_continue_dik_guard
					: DIK_SPACE;
				loadItem->OnKeyboardPress(guardDik);
				Msg("* [Gamepad] FIX-LOADSCREEN: load prompt dismissed by gamepad (mask=0x%X, dik=%d)", freshMask, guardDik);
			}
		}
	}

	// FIX-LIVE3 п.15: маска прошлого кадра для «свежих» A/Start на экране смерти. Раньше сравнивали с
	// m_rawDownMask уже ПОСЛЕ StepStateMachine (= маска этого же кадра), и свежего нажатия не было никогда.
	const u32 prevFrameRawMask = m_rawDownMask;
	StepStateMachine(evaluatedCtx, evalSubFlags, evalReason, rawButtonsMask, lxRaw, lyRaw, Device.dwTimeContinual, true);

	// FIX-LIVE3 п.19: View, нажатый в мире и удержанный 0,4 с (рюкзак уже открылся, идёт анимация) -> КПК.
	if (s_gpViewLateArmed)
	{
		if (m_bInSelfTest || !(rawButtonsMask & (1u << CInput::GP_BTN_BACK)) || !g_actor || !g_actor->g_Alive())
			s_gpViewLateArmed = false;
		else if (Device.dwTimeContinual - s_gpViewLateStart >= GP_VIEW_PDA_HOLD_MS && g_ai_space)
		{
			s_gpViewLateArmed = false;
			ConsumeButtonUntilRelease(CInput::GP_BTN_BACK);
			::luabind::functor<bool> fnLate;
			if (ai().script_engine().functor("gp_inv_rework.view_world_hold_late", fnLate))
			{
				try
				{
					if (fnLate() && pInput && pInput->GetGamepadHaptics())
						pInput->GetGamepadHaptics()->PulseSoftClick("world_view_hold_pda");
				}
				catch (...)
				{
				}
			}
		}
	}

	if (!m_bInSelfTest && m_currentContext == EGamepadContext::Cutscene_InputBlock && (!g_actor || !g_actor->g_Alive()))
	{
		CActor* const pActor = g_actor;
		// W5-MENU В6 [решено без Миши]: экран смерти (<game_over> в game_tutorials.xml) — защитная пауза 1,25 с
		// со сбросом зажатых в момент гибели кнопок, затем свежие A или Start переводят в главное меню.
		static CUISequenceSimpleItem* s_deathItem = nullptr;
		static u32 s_deathStartedMs = 0;
		CUISequenceSimpleItem* deathItem = (!m_bInSelfTest && pActor && g_tutorial && g_tutorial->IsActive())
			? smart_cast<CUISequenceSimpleItem*>(CUISequencerItemsProbe::Front(g_tutorial))
			: nullptr;
		if (deathItem != s_deathItem)
		{
			s_deathItem = deathItem;
			s_deathStartedMs = deathItem ? Device.dwTimeContinual : 0;
			if (deathItem)
				ConsumeAllCurrentlyHeldButtons();
		}
		if (deathItem && pInput && pInput->IsGamepadActiveSource() &&
		    Device.dwTimeContinual >= s_deathStartedMs &&
		    (Device.dwTimeContinual - s_deathStartedMs) >= 1250 &&
		    ((Device.dwTimeContinual / 1000.0f) - deathItem->m_time_start) >= 1.25f)
		{
			constexpr u32 skipMask = (1u << CInput::GP_BTN_SOUTH) | (1u << CInput::GP_BTN_START);
			if ((rawButtonsMask & skipMask & ~prevFrameRawMask & ~m_consumedUntilReleaseMask) != 0)
			{
				ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
				ConsumeButtonUntilRelease(CInput::GP_BTN_START);
				const int guardDik = (deathItem->m_continue_dik_guard > 0 && deathItem->m_continue_dik_guard != 9999)
					? deathItem->m_continue_dik_guard
					: DIK_SPACE;
				deathItem->OnKeyboardPress(guardDik);
				deathItem->m_time_length = 0.0f;
			}
		}
		// FIX-LIVE2 п.26 (Миша 2026-11-16, 26а): без карточки game_over (она закончилась, Soulslike и т. п.)
		// свежие A и Start после 1,25 с открывают одно и то же главное меню (пункт «Загрузить»), как Esc.
		static u32 s_deadSinceMs = 0;
		static CActor* s_deadActor = nullptr;
		if (s_deadActor != pActor)
		{
			s_deadActor = pActor;
			s_deadSinceMs = 0;
		}
		if (!deathItem)
		{
			if (!s_deadSinceMs)
			{
				s_deadSinceMs = Device.dwTimeContinual ? Device.dwTimeContinual : 1;
				ConsumeAllCurrentlyHeldButtons();
			}
			CUIDialogWnd* const topDlg = CurrentGameUI() ? CurrentGameUI()->TopInputReceiver() : nullptr;
			const bool menuActive = MainMenu() && MainMenu()->IsActive();
			if (!menuActive && !(topDlg && topDlg->IsShown()) && pInput && pInput->IsGamepadActiveSource() &&
			    Device.dwTimeContinual - s_deadSinceMs >= 1250)
			{
				constexpr u32 openMask = (1u << CInput::GP_BTN_SOUTH) | (1u << CInput::GP_BTN_START);
				if ((rawButtonsMask & openMask & ~prevFrameRawMask & ~m_consumedUntilReleaseMask) != 0)
				{
					ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
					ConsumeButtonUntilRelease(CInput::GP_BTN_START);
					Console->Execute("main_menu on");
				}
			}
		}
		else
			s_deadSinceMs = 0;
	}

	if (!m_bInSelfTest && m_currentContext == EGamepadContext::Placement_HideoutFurniture &&
	    pInput && pInput->IsGamepadActiveSource() && g_ai_space)
	{
		static u32 s_lastPlacementTickMs = 0;
		const u32 nowTickMs = Device.dwTimeContinual;
		const u32 dtMs = (s_lastPlacementTickMs != 0 && nowTickMs >= s_lastPlacementTickMs)
		                     ? _min(nowTickMs - s_lastPlacementTickMs, 100u)
		                     : 16u;
		s_lastPlacementTickMs = nowTickMs;

		const auto& gp = pInput->GetGamepadState();
		if (gp.connected && gp.has_focus && !gp.need_release_all)
		{
			const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
			const gpmath::Vec2 ls = gpmath::ProcessStick(prof.left_stick, gpmath::NormalizeStickAxis(gp.axes[0]), -gpmath::NormalizeStickAxis(gp.axes[1]));
			const gpmath::Vec2 rs = gpmath::ProcessStick(prof.right_stick, gpmath::NormalizeStickAxis(gp.axes[2]), -gpmath::NormalizeStickAxis(gp.axes[3]));
			::luabind::functor<bool> tickFn;
			if (ai().script_engine().functor("gp_hideout.placement_tick", tickFn))
			{
				try
				{
					tickFn(ls.x, ls.y, rs.x, rs.y, m_bLtDigitalDown, m_effectiveDownMask, dtMs);
				}
				catch (...)
				{
				}
			}
		}
	}
}

bool CGamepadContextDispatcher::AllowsWorldMove() const
{
	if (m_bGamepadMouseMode)
		return false;
	// C4.3-D6: открытая консоль ~ безусловно блокирует передачу левого стика в движение персонажа
	// (D3.3b: вне RunSelfTest(), где тестируется чистый автомат m_currentContext при открытой консоли)
	if (!m_bInSelfTest && Console != nullptr && Console->bVisible)
		return false;

	switch (m_currentContext)
	{
	case EGamepadContext::Console:
	case EGamepadContext::TextEntry:
	case EGamepadContext::MainMenu:
	case EGamepadContext::Sleep:
	case EGamepadContext::Dialog_NPC:
	case EGamepadContext::World_VehicleOrHolder:
		return false;

	case EGamepadContext::Cutscene_InputBlock:
		return m_subFlags.bPartialMoveOnlyBlock && !m_subFlags.bCutsceneFullBlock;

	case EGamepadContext::RadialWheel:
		return m_subFlags.bModalAllowsMovement;

	case EGamepadContext::Placement_HideoutFurniture:
		return (m_luaHints.placementState == EPlacementState::Holding) && !m_bStickMoveConsumedUntilNeutral;

	case EGamepadContext::World_3DPDA:
		// FG.6h: пока экран 3D-КПК активен (b3dPdaInteractive), левый стик двигает курсор/карту КПК и НЕ должен двигать актора;
		// движение актора с 3D-КПК в руках разрешено только в опущенном подрежиме freelook (!b3dPdaInteractive).
		return !m_subFlags.b3dPdaInteractive && m_subFlags.bModalAllowsMovement && !m_bStickMoveConsumedUntilNeutral;

	case EGamepadContext::MenuUI_Modal:
		// FG.6h: пока открыто любое модальное окно MenuUI_Modal (инвентарь без паузы, 2D-КПК, торговля, обыск, ремонт и др.),
		// левый стик управляет виртуальным курсором UI и НЕ должен двигать персонажа в мире (даже если окно поставило AllowMovement(true)).
		return false;

	case EGamepadContext::World_Gameplay:
		return !m_bStickMoveConsumedUntilNeutral;

	default:
		return false;
	}
}

bool CGamepadContextDispatcher::AllowsWorldLook() const
{
	if (m_bGamepadMouseMode || m_weaponSelectorOpen || m_weaponSelectorLookGate)
		return false;
	// C4.3-D6: открытая консоль ~ безусловно блокирует передачу правого стика во вращение камеры
	// (D3.3b: вне RunSelfTest(), где тестируется чистый автомат m_currentContext при открытой консоли)
	if (!m_bInSelfTest && Console != nullptr && Console->bVisible)
		return false;

	switch (m_currentContext)
	{
	case EGamepadContext::Console:
	case EGamepadContext::TextEntry:
	case EGamepadContext::MainMenu:
	case EGamepadContext::Sleep:
	case EGamepadContext::Dialog_NPC:
	case EGamepadContext::RadialWheel:
	case EGamepadContext::MenuUI_Modal:
	case EGamepadContext::World_VehicleOrHolder:
		return false;

	case EGamepadContext::Cutscene_InputBlock:
		return m_subFlags.bPartialMoveOnlyBlock && !m_subFlags.bCutsceneFullBlock;

	case EGamepadContext::Placement_HideoutFurniture:
		return (m_luaHints.placementState == EPlacementState::Holding);

	case EGamepadContext::World_3DPDA:
		// В подрежиме активного экрана 3D КПК (b3dPdaInteractive) правый стик управляет курсором КПК, а не камерой мира;
		// в опущенном подрежиме freelook (!b3dPdaInteractive) правый стик вращает камеру мира.
		return !m_subFlags.b3dPdaInteractive;

	case EGamepadContext::World_Gameplay:
		return true;

	default:
		return false;
	}
}

bool CGamepadContextDispatcher::AllowsGamepadButtons() const
{
	if (!m_bInSelfTest && Console != nullptr && Console->bVisible)
		return false;
	return m_currentContext != EGamepadContext::Console;
}

bool CGamepadContextDispatcher::IsButtonPressed(u32 btnBit) const
{
	if (btnBit >= VBTN_TOTAL_COUNT || !AllowsGamepadButtons())
		return false;
	return (m_pressedMask & (1u << btnBit)) != 0;
}

bool CGamepadContextDispatcher::IsButtonHeld(u32 btnBit) const
{
	if (btnBit >= VBTN_TOTAL_COUNT || !AllowsGamepadButtons())
		return false;
	return (m_heldMask & (1u << btnBit)) != 0;
}

bool CGamepadContextDispatcher::IsButtonReleased(u32 btnBit) const
{
	if (btnBit >= VBTN_TOTAL_COUNT || !AllowsGamepadButtons())
		return false;
	return (m_releasedMask & (1u << btnBit)) != 0;
}

bool CGamepadContextDispatcher::WasButtonReleasedOnContextExit(u32 btnBit) const
{
	if (btnBit >= VBTN_TOTAL_COUNT)
		return false;
	return (m_releasedOnContextExitMask & (1u << btnBit)) != 0;
}

bool CGamepadContextDispatcher::IsButtonConsumedUntilRelease(u32 btnBit) const
{
	if (btnBit >= VBTN_TOTAL_COUNT)
		return false;
	return (m_consumedUntilReleaseMask & (1u << btnBit)) != 0;
}

bool CGamepadContextDispatcher::IsNavDirTriggeredThisFrame(ENavDir* outDir, bool* outIsRepeat, LPCSTR* outSource) const
{
	if (!AllowsGamepadButtons() || !m_bNavTriggeredThisFrame || m_activeNavDir == ENavDir::None)
		return false;

	if (outDir)
		*outDir = m_activeNavDir;
	if (outIsRepeat)
		*outIsRepeat = m_bNavTriggeredIsRepeat;
	if (outSource)
		*outSource = m_activeNavSource;
	return true;
}

void CGamepadContextDispatcher::ConsumeButtonUntilRelease(u32 btnBit)
{
	if (btnBit >= VBTN_TOTAL_COUNT)
		return;
	const u32 bit = (1u << btnBit);
	m_worldDualPending[btnBit] = false;
	m_worldDualHoldCommitted[btnBit] = false;
	if (btnBit == m_freelookBtnBit)
		m_bR3DualModePending = false;
	if (m_activeInjectedDik[btnBit] > 0)
	{
		if (pInput)
			pInput->SetGamepadInjectedKeyState(m_activeInjectedDik[btnBit], false);
		m_activeInjectedDik[btnBit] = 0;
		m_activeInjectedAction[btnBit] = kNOTBINDED;
	}
	if (m_rawDownMask & bit)
	{
		m_consumedUntilReleaseMask |= bit;
		m_effectiveDownMask &= ~bit;
		m_pressedMask &= ~bit;
		m_heldMask &= ~bit;
		m_prevEffectiveDownMask &= ~bit;
	}
}

void CGamepadContextDispatcher::ConsumeAllCurrentlyHeldButtons()
{
	const u32 liveMask = m_rawDownMask | SampleRawButtonsAndTriggersMask();
	m_consumedUntilReleaseMask |= liveMask;
	m_effectiveDownMask = 0;
	m_pressedMask = 0;
	m_heldMask = 0;
	m_prevEffectiveDownMask = 0;
	m_activeNavDir = ENavDir::None;
	m_bNavTriggeredThisFrame = false;
	if (m_stickCardinalDir != ENavDir::None)
		m_bStickNavConsumedUntilNeutral = true;

	m_bLbPending = false;
	m_bLbHoldCommitted = false;
	m_bEastDualModePending = false;
	m_bEastHoldCommitted = false;
	m_bR3DualModePending = false;
	m_bXPending = false;
	m_bXHoldCommitted = false;
	m_bWheelLbWasPhysicallyDown = false;
	if (m_bChordFreelookOn)
	{
		m_bChordFreelookOn = false;
		m_chordFreelookDik = 0;
		if (CActor* pActor = g_actor)
			pActor->cam_UnsetFreelook();
	}
	ZeroMemory(m_worldDualPending, sizeof(m_worldDualPending));
	ZeroMemory(m_worldDualHoldCommitted, sizeof(m_worldDualHoldCommitted));
	if (pInput)
		pInput->ClearAllGamepadInjectedKeys();
	for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
	{
		m_activeInjectedDik[btn] = 0;
		m_activeInjectedAction[btn] = kNOTBINDED;
	}
}

void CGamepadContextDispatcher::SetLuaMcmKeybindCapture(bool active)
{
	m_luaHints.bMcmKeybindCapture = active;
}

void CGamepadContextDispatcher::SetLuaFddaInputDisabled(bool active)
{
	if (active && !m_luaHints.bFddaInputDisabled)
		m_fddaDisabledSinceMs = Device.dwTimeContinual ? Device.dwTimeContinual : 1;
	else if (!active)
		m_fddaDisabledSinceMs = 0;
	m_luaHints.bFddaInputDisabled = active;
}

void CGamepadContextDispatcher::SetLuaActorSleepingInfo(bool active)
{
	m_luaHints.bActorSleepingInfo = active;
}

void CGamepadContextDispatcher::SetLuaSleepDialogShown(bool active)
{
	m_luaHints.bSleepDialogShown = active;
}

void CGamepadContextDispatcher::SetLuaRadialWheelType(ERadialWheelType wheelType)
{
	m_luaHints.radialWheelType = wheelType;
}

void CGamepadContextDispatcher::SetLuaPlacementState(EPlacementState state)
{
	m_luaHints.placementState = state;
}

void CGamepadContextDispatcher::SetLuaModalMenuTag(LPCSTR tag)
{
	m_luaHints.modalMenuTag = (tag && tag[0]) ? tag : "";
}

void CGamepadContextDispatcher::NotifyModalLayerTransition(LPCSTR newModalTag, LPCSTR reason)
{
	const LPCSTR safeTag = (newModalTag && newModalTag[0]) ? newModalTag : "";
	const LPCSTR safeReason = (reason && reason[0]) ? reason : "modal_layer_transition";
	m_luaHints.modalMenuTag = safeTag;
	m_activeModalLayerTag = safeTag;
	m_lastTransitionReason = safeReason;
	m_lastTransitionTimeContinual = Device.dwTimeContinual;
	++m_transitionCount;
	m_bContextChangedThisFrame = true;
	m_releasedOnContextExitMask = m_prevEffectiveDownMask;
	ConsumeAllCurrentlyHeldButtons();

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_ctx_modal_layer,%u,tag=%s,reason=%s,raw_mask=0x%05X,barrier_mask=0x%05X,exit_released=0x%05X",
		           Device.dwTimeContinual,
		           safeTag[0] ? safeTag : "none",
		           safeReason,
		           m_rawDownMask,
		           m_consumedUntilReleaseMask,
		           m_releasedOnContextExitMask);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

void CGamepadContextDispatcher::SetLuaBridgeHintsBatch(bool mcmBind,
                                                       bool fddaDis,
                                                       bool sleepInfo,
                                                       bool sleepDlg,
                                                       int wheelType,
                                                       int placementState,
                                                       LPCSTR modalTag)
{
	m_luaHints.bMcmKeybindCapture = mcmBind;
	if (fddaDis && !m_luaHints.bFddaInputDisabled)
		m_fddaDisabledSinceMs = Device.dwTimeContinual ? Device.dwTimeContinual : 1;
	else if (!fddaDis)
		m_fddaDisabledSinceMs = 0;
	m_luaHints.bFddaInputDisabled = fddaDis;
	m_luaHints.bActorSleepingInfo = sleepInfo;
	m_luaHints.bSleepDialogShown = sleepDlg;

	const int clampedWheel = clampr(wheelType, 0, 5);
	m_luaHints.radialWheelType = static_cast<ERadialWheelType>(clampedWheel);

	EPlacementState st = EPlacementState::None;
	if (placementState == 2)
		st = EPlacementState::Holding;
	else if (placementState == 3)
		st = EPlacementState::AdvCtrl;
	else if (placementState == 4)
		st = EPlacementState::Preview;
	m_luaHints.placementState = st;

	m_luaHints.modalMenuTag = (modalTag && modalTag[0]) ? modalTag : "";
	++m_luaHints.luaHintSyncCount;
}

void CGamepadContextDispatcher::SetLuaPlacementKeys(int dikPlace, int dikCollision, int dikAlign, int dikControls)
{
	if (dikPlace >= 0)
		m_luaHints.placementDikPlace = dikPlace;
	if (dikCollision > 0)
		m_luaHints.placementDikCollision = dikCollision;
	if (dikAlign > 0)
		m_luaHints.placementDikAlign = dikAlign;
	if (dikControls > 0)
		m_luaHints.placementDikControls = dikControls;
}

void CGamepadContextDispatcher::PollLuaBridgeHints()
{
	if (!g_ai_space)
		return;

	::luabind::functor<bool> fn;
	if (!ai().script_engine().functor("gp_bridge.sync_context_hints", fn))
		return;

	try
	{
		(void)fn(false);
	}
	catch (...)
	{
		Msg("! [Gamepad] Exception in gp_bridge.sync_context_hints");
	}
}

void CGamepadContextDispatcher::ResetLuaHints(LPCSTR reason)
{
	(void)reason;
	m_luaHints = {};
}

int CGamepadContextDispatcher::ResolveActionDik(int actionId, bool* outUsedSynthetic) const
{
	if (outUsedSynthetic)
		*outUsedSynthetic = false;

	if (actionId < 0 || actionId >= kLASTACTION)
		return 0;

	const EGameActions act = static_cast<EGameActions>(actionId);
	const int boundDik = get_action_dik(act, -1);
	if (boundDik > 0)
		return boundDik;

	// Для kJUMP (и слотов оружия 1..4) при отсутствии привязки в user.ltx возвращаем стандартный DIK (DIK_SPACE для прыжка,
	// чтобы demonized_ledge_grabbing.script видел реальный пробел в on_key_press и key_state(DIK_SPACE)).
	if (outUsedSynthetic)
		*outUsedSynthetic = true;
	if (actionId == kJUMP)
		return DIK_SPACE;
	if (actionId == kWPN_1)
		return DIK_1;
	if (actionId == kWPN_2)
		return DIK_2;
	if (actionId == kWPN_3)
		return DIK_3;
	if (actionId == kWPN_4)
		return DIK_4;
	return 0xFF;
}

bool CGamepadContextDispatcher::ResolveContextualButtonInjection(u32 btnBit,
                                                                 EGamepadContext ctx,
                                                                 const SContextSubFlags& subFlags,
                                                                 int& outActionId,
                                                                 int& outExplicitDik,
                                                                 LPCSTR& outReason) const
{
	outActionId = kNOTBINDED;
	outExplicitDik = 0;
	outReason = "unmapped";

	if (btnBit >= VBTN_TOTAL_COUNT)
	{
		outReason = "invalid_btn";
		return false;
	}

	// 1. Console (Приоритет 1 — C4.3-D6: полное подавление кнопок геймпада)
	if (ctx == EGamepadContext::Console)
	{
		outReason = "blocked_console";
		return false;
	}

	// 2. TextEntry (Приоритет 2 — поле ввода CUICustomEdit или захват клавиши ui_mcm.GUI.k_binder)
	if (ctx == EGamepadContext::TextEntry)
	{
		// F5.1 (D51): когда открыта экранная клавиатура геймпада, все нажатия обрабатывает её модальное окно
		// CUIGamepadOnScreenKeyboardWnd::Update() без просачивания DIK_ESCAPE / DIK_RETURN в родительский диалог.
		if (!m_bInSelfTest && CGamepadOnScreenKeyboardUI::Instance().IsOpen())
		{
			outReason = "handled_by_osk";
			return false;
		}
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kQUIT;
			outExplicitDik = DIK_ESCAPE;
			outReason = "text_entry_cancel_escape";
			return true;
		}
		if (!m_luaHints.bMcmKeybindCapture && btnBit == CInput::GP_BTN_SOUTH)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = DIK_RETURN;
			outReason = "text_entry_confirm_return";
			return true;
		}
		outReason = m_luaHints.bMcmKeybindCapture ? "blocked_mcm_keybind" : "blocked_text_entry";
		return false;
	}

	// CURSOR1: режим виртуальной мыши (m_bGamepadMouseMode) поверх любого неподдерживаемого модового окна
	if (m_bGamepadMouseMode)
	{
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == VBTN_RT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "mouse_mode_lclick";
			return true;
		}
		if (btnBit == CInput::GP_BTN_WEST)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_2;
			outReason = "mouse_mode_rclick";
			return true;
		}
		outReason = "blocked_mouse_mode";
		return false;
	}

	// 3. MainMenu (Приоритет 3 — главное меню / пауза / MCM)
	if (ctx == EGamepadContext::MainMenu)
	{
		if (btnBit == CInput::GP_BTN_EAST || btnBit == CInput::GP_BTN_START)
		{
			outActionId = kQUIT;
			outExplicitDik = DIK_ESCAPE;
			outReason = "main_menu_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_SOUTH)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "main_menu_lclick";
			return true;
		}
		outReason = "blocked_main_menu";
		return false;
	}

	// 4. Cutscene_InputBlock (Приоритет 4)
	if (ctx == EGamepadContext::Cutscene_InputBlock)
	{
		if (subFlags.bCutsceneFullBlock)
		{
			outReason = "blocked_cutscene_full";
			return false;
		}
		// При частичной блокировке (only_allow_movekeys / liz_fdda_input_manager) дискретные события
		// идут через CLevel::IR_OnKeyboard*, где фильтр liz_fdda_input_manager.on_before_key_press
		// сам пропускает разрешённые бинды и по kWPN_RELOAD (X) вызывает lam2.stop_all_actions().
	}

	// 5. Sleep (Приоритет 5 — окно сна UISleep)
	if (ctx == EGamepadContext::Sleep)
	{
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kQUIT;
			outExplicitDik = ResolveActionDik(kQUIT);
			outReason = "sleep_ui_close_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == VBTN_RT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "sleep_ui_lclick";
			return true;
		}
		outReason = "blocked_sleep_ui";
		return false;
	}

	// 6. Dialog_NPC (Приоритет 6 — CUITalkWnd)
	if (ctx == EGamepadContext::Dialog_NPC)
	{
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kQUIT;
			outExplicitDik = ResolveActionDik(kQUIT);
			outReason = "dialog_npc_close_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_WEST)
		{
			outActionId = kSPRINT_TOGGLE;
			outExplicitDik = ResolveActionDik(kSPRINT_TOGGLE);
			outReason = "dialog_npc_switch_trade";
			return true;
		}
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == VBTN_RT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "dialog_npc_lclick";
			return true;
		}
		// FG.6e / FG.6h: разрешаем проброс D-Pad (Up/Down/Left/Right -> DIK_UP/DOWN/LEFT/RIGHT) в окне диалога НПС
		if (btnBit == CInput::GP_BTN_DPAD_UP)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = 200; // DIK_UP
			outReason = "dialog_npc_dpad_up";
			return true;
		}
		if (btnBit == CInput::GP_BTN_DPAD_DOWN)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = 208; // DIK_DOWN
			outReason = "dialog_npc_dpad_down";
			return true;
		}
		if (btnBit == CInput::GP_BTN_DPAD_LEFT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = 203; // DIK_LEFT
			outReason = "dialog_npc_dpad_left";
			return true;
		}
		if (btnBit == CInput::GP_BTN_DPAD_RIGHT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = 205; // DIK_RIGHT
			outReason = "dialog_npc_dpad_right";
			return true;
		}
		outReason = "blocked_dialog_npc";
		return false;
	}

	// 7. RadialWheel (Приоритет 7 — колёса быстрого доступа / патронов / напарника)
	if (ctx == EGamepadContext::RadialWheel)
	{
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kQUIT;
			outExplicitDik = ResolveActionDik(kQUIT);
			outReason = "radial_wheel_close_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == VBTN_RT)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "radial_wheel_lclick";
			return true;
		}
		if (btnBit == CInput::GP_BTN_WEST && m_luaHints.radialWheelType == ERadialWheelType::Ammo)
		{
			outActionId = kWPN_FIREMODE_NEXT;
			outExplicitDik = ResolveActionDik(kWPN_FIREMODE_NEXT);
			outReason = "ammo_wheel_firemode";
			return true;
		}
		outReason = "blocked_radial_wheel";
		return false;
	}

	// 8. Placement_HideoutFurniture (Приоритет 8 — защита от сброса установки в placeable_furniture.script:397-402)
	if (ctx == EGamepadContext::Placement_HideoutFurniture)
	{
		if (m_luaHints.placementState == EPlacementState::AdvCtrl)
		{
			if (btnBit == CInput::GP_BTN_SOUTH)
			{
				outActionId = kUSE;
				outExplicitDik = (m_luaHints.placementDikPlace > 0) ? m_luaHints.placementDikPlace : ResolveActionDik(kUSE);
				outReason = "placement_advctrl_place";
				return true;
			}
			if (btnBit == CInput::GP_BTN_EAST)
			{
				outActionId = kQUIT;
				outExplicitDik = ResolveActionDik(kQUIT);
				outReason = "placement_advctrl_close";
				return true;
			}
			if (btnBit == CInput::GP_BTN_WEST)
			{
				outActionId = kNOTBINDED;
				outExplicitDik = (m_luaHints.placementDikCollision > 0) ? m_luaHints.placementDikCollision : 15;
				outReason = "placement_advctrl_collision";
				return true;
			}
			if (btnBit == CInput::GP_BTN_NORTH)
			{
				outActionId = kNOTBINDED;
				outExplicitDik = (m_luaHints.placementDikAlign > 0) ? m_luaHints.placementDikAlign : 58;
				outReason = "placement_advctrl_align";
				return true;
			}
			if (btnBit == CInput::GP_BTN_BACK)
			{
				outActionId = kNOTBINDED;
				outExplicitDik = (m_luaHints.placementDikControls > 0) ? m_luaHints.placementDikControls : 199;
				outReason = "placement_toggle_mode";
				return true;
			}
			outReason = "blocked_placement_guard";
			return false;
		}

		// Состояния HOLDING(2) и PREVIEW(4): пропускаем ТОЛЬКО клавиши управления установкой мебели!
		if (btnBit == CInput::GP_BTN_SOUTH)
		{
			outActionId = kUSE;
			outExplicitDik = (m_luaHints.placementDikPlace > 0) ? m_luaHints.placementDikPlace : ResolveActionDik(kUSE);
			outReason = "placement_confirm_place";
			return true;
		}
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kQUIT;
			outExplicitDik = ResolveActionDik(kQUIT);
			outReason = "placement_cancel_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_WEST)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = (m_luaHints.placementDikCollision > 0) ? m_luaHints.placementDikCollision : 15; // DIK_TAB
			outReason = "placement_toggle_collision";
			return true;
		}
		if (btnBit == CInput::GP_BTN_NORTH)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = (m_luaHints.placementDikAlign > 0) ? m_luaHints.placementDikAlign : 58; // DIK_CAPITAL
			outReason = "placement_toggle_align";
			return true;
		}
		if (btnBit == CInput::GP_BTN_BACK)
		{
			outActionId = kNOTBINDED;
			outExplicitDik = (m_luaHints.placementDikControls > 0) ? m_luaHints.placementDikControls : 199; // DIK_HOME
			outReason = "placement_toggle_controls";
			return true;
		}

		outReason = "blocked_placement_guard";
		return false;
	}

	// 9. World_3DPDA (Приоритет 9, PDA1-E §4.1)
	if (ctx == EGamepadContext::World_3DPDA)
	{
		if (subFlags.b3dPdaInteractive)
		{
			if (btnBit == CInput::GP_BTN_EAST)
			{
				outActionId = kQUIT;
				outExplicitDik = ResolveActionDik(kQUIT);
				outReason = "3dpda_close_escape";
				return true;
			}
			if (btnBit == CInput::GP_BTN_RIGHT_STICK)
			{
				outActionId = kWPN_FUNC;
				outExplicitDik = ResolveActionDik(kWPN_FUNC);
				outReason = "3dpda_lower_freelook";
				return true;
			}
			if (btnBit == CInput::GP_BTN_SOUTH)
			{
				outActionId = kNOTBINDED;
				outExplicitDik = MOUSE_1;
				outReason = "3dpda_ui_lclick";
				return true;
			}
			outReason = "blocked_3dpda_ui";
			return false;
		}
		// Предпросмотр / опущенный экран 3D КПК (§4.1):
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == CInput::GP_BTN_RIGHT_STICK)
		{
			CPda* pdaItem = (g_actor != nullptr) ? smart_cast<CPda*>(g_actor->inventory().ActiveItem()) : nullptr;
			const bool bZoomed = (pdaItem != nullptr && pdaItem->m_bZoomed);
			outActionId = bZoomed ? kWPN_FUNC : kWPN_RELOAD;
			outExplicitDik = ResolveActionDik(outActionId);
			outReason = bZoomed ? "3dpda_preview_raise_screen" : "3dpda_preview_zoom_in";
			return true;
		}
		if (btnBit == CInput::GP_BTN_EAST)
		{
			outActionId = kACTIVE_JOBS;
			outExplicitDik = ResolveActionDik(kACTIVE_JOBS);
			outReason = "3dpda_preview_holster";
			return true;
		}
		if (btnBit == CInput::GP_BTN_BACK)
		{
			outActionId = kINVENTORY;
			outExplicitDik = ResolveActionDik(kINVENTORY);
			outReason = "3dpda_preview_inventory";
			return true;
		}
		if (btnBit == VBTN_LT || btnBit == VBTN_RT)
		{
			outReason = "blocked_3dpda_preview_triggers";
			return false;
		}
	}

	// 10. MenuUI_Modal (Приоритет 10 — инвентарь, торговля, ремонт, MessageBox и др.)
	if (ctx == EGamepadContext::MenuUI_Modal)
	{
		if (CPauseTokenManager::Instance().IsFocusTestWindowOpen())
		{
			outReason = "handled_by_focus_test";
			return false;
		}
		if (btnBit == CInput::GP_BTN_EAST)
		{
			if (CPauseTokenManager::Instance().Count() > 0)
			{
				outReason = "handled_by_pause_token_B";
				return false;
			}
			outActionId = kQUIT;
			outExplicitDik = ResolveActionDik(kQUIT);
			outReason = "modal_ui_close_escape";
			return true;
		}
		if (btnBit == CInput::GP_BTN_SOUTH || btnBit == VBTN_RT)
		{
			if (btnBit == VBTN_RT && (IsInventoryModalActive() || IsPdaInteractiveActive()))
			{
				outReason = "handled_by_inv_sort_tabs";
				return false;
			}
			if (m_bVcLocked)
			{
				outReason = "blocked_vc_locked";
				return false;
			}
			outActionId = kNOTBINDED;
			outExplicitDik = MOUSE_1;
			outReason = "modal_ui_lclick";
			return true;
		}
		outReason = "blocked_modal_ui";
		return false;
	}

	// 11. World_VehicleOrHolder (Приоритет 11 — транспорт CCar / станковое оружие)
	if (ctx == EGamepadContext::World_VehicleOrHolder)
	{
		switch (btnBit)
		{
		case CInput::GP_BTN_WEST:       outActionId = kUSE; break;
		case CInput::GP_BTN_SOUTH:      outActionId = kJUMP; break;
		case CInput::GP_BTN_DPAD_UP:    outActionId = kTORCH; break;
		case CInput::GP_BTN_NORTH:      outActionId = kWPN_NEXT; break;
		case CInput::GP_BTN_START:      outActionId = kQUIT; break;
		case VBTN_RT:                   outActionId = kWPN_FIRE; break;
		default:
			outReason = "blocked_vehicle_holder";
			return false;
		}
		outExplicitDik = ResolveActionDik(outActionId);
		outReason = "vehicle_holder_bind";
		return outExplicitDik > 0;
	}

	// 12. World_Gameplay (и Cutscene_InputBlock с частичной блокировкой / World_3DPDA freelook)
	// K2 (D61): разрешение действий из активного профиля BindingProfileModel (appdata\gamepad\profile.ltx)
	if (btnBit >= VBTN_TOTAL_COUNT)
	{
		outReason = "unmapped_world_button";
		return false;
	}

	const gpbind::ButtonId targetBtn = static_cast<gpbind::ButtonId>(btnBit);
	const gpbind::BindingProfileModel& bindProf = gpbind::GetActiveBindingProfile();
	gpbind::ActionId boundButtonAct = gpbind::ActionId::Invalid;
	for (const auto& entry : bindProf.GetAllBindings())
	{
		if (entry.context == gpbind::ContextId::World_Gameplay &&
		    entry.trigger.type == gpbind::TriggerType::Button &&
		    entry.trigger.button == targetBtn)
		{
			boundButtonAct = entry.action;
			break;
		}
	}

	if (m_luaHints.bFddaInputDisabled &&
	    (boundButtonAct == gpbind::ActionId::ContextUseReload ||
	     (boundButtonAct == gpbind::ActionId::Invalid && btnBit == CInput::GP_BTN_WEST)))
	{
		// В анимации FDDA нажатие кнопки ContextUseReload (X) отправляет kWPN_RELOAD (34), который в liz_fdda_input_manager
		// входит в skip_bidings[34] и штатно вызывает lam2.stop_all_actions().
		outActionId = kWPN_RELOAD;
		outExplicitDik = ResolveActionDik(outActionId);
		outReason = "world_gameplay_bind";
		return outExplicitDik > 0;
	}

	switch (boundButtonAct)
	{
	case gpbind::ActionId::Jump:
		outActionId = kJUMP;
		break;
	case gpbind::ActionId::Crouch:
	case gpbind::ActionId::LowCrouch:
		outActionId = kCROUCH;
		break;
	case gpbind::ActionId::ContextUseReload:
		outActionId = HasInteractiveTargetForUse() ? kUSE : kWPN_RELOAD;
		break;
	case gpbind::ActionId::NextWeapon:
		outActionId = const_cast<CGamepadContextDispatcher*>(this)->ResolveNextWeaponSlotAction();
		break;
	case gpbind::ActionId::AmmoTypeNext:
		outActionId = kWPN_NEXT;
		break;
	case gpbind::ActionId::PdaFullToggle:
		outActionId = kINVENTORY;
		break;
	case gpbind::ActionId::PauseMenu:
		outActionId = kQUIT;
		break;
	case gpbind::ActionId::Sprint:
		outActionId = kSPRINT_TOGGLE;
		break;
	case gpbind::ActionId::MeleeQuick:
		outActionId = kCUSTOM24;
		break;
	case gpbind::ActionId::WeaponLower:
		outActionId = kSAFEMODE;
		break;
	case gpbind::ActionId::Freelook:
		outActionId = kFREELOOK;
		break;
	case gpbind::ActionId::QuickBoltOrSlot:
		outActionId = kWPN_6;
		break;
	case gpbind::ActionId::GrenadeQuick:
		outActionId = kCUSTOM19;
		break;
	case gpbind::ActionId::LeanLeft:
		outActionId = kL_LOOKOUT;
		break;
	case gpbind::ActionId::LeanRight:
		outActionId = kR_LOOKOUT;
		break;
	case gpbind::ActionId::TorchToggle:
		outActionId = kTORCH;
		break;
	case gpbind::ActionId::NightVision:
		outActionId = kNIGHT_VISION;
		break;
	case gpbind::ActionId::Pda3DToggle:
		outActionId = (ctx == EGamepadContext::World_3DPDA) ? kACTIVE_JOBS : kCUSTOM20;
		break;
	case gpbind::ActionId::MaskClean:
		outActionId = kCAM_ZOOM_IN;
		break;
	case gpbind::ActionId::DetectorToggle:
		outActionId = kDETECTOR;
		break;
	case gpbind::ActionId::UnderbarrelToggle:
	{
		extern BOOL useSeparateUBGLKeybind;
		outActionId = useSeparateUBGLKeybind ? kCUSTOM21 : kWPN_FUNC;
		break;
	}
	case gpbind::ActionId::FireModeNext:
		outActionId = kWPN_FIREMODE_NEXT;
		break;
	case gpbind::ActionId::Aim:
		outActionId = kWPN_ZOOM;
		break;
	case gpbind::ActionId::Fire:
		outActionId = kWPN_FIRE;
		break;
	default:
		outReason = "unmapped_world_button";
		return false;
	}

	outExplicitDik = ResolveActionDik(outActionId);
	outReason = "world_gameplay_bind";
	return outExplicitDik > 0;
}

bool CGamepadContextDispatcher::InjectKeyboardEvent(int dik, int actionId, LPCSTR phase, EGamepadContext ctx, bool emitLog)
{
	if (dik <= 0 || !phase || !phase[0])
		return false;

	// Синхронизируем массив состояния клавиш CInput (для pInput->iGetAsyncKeyState / Lua key_state(dik)),
	// чтобы моды вроде demonized_ledge_grabbing видели реальное удержание DIK_SPACE / kJUMP всё время зажатия кнопки на геймпаде.
	if (pInput)
	{
		if (0 == _stricmp(phase, "press") || 0 == _stricmp(phase, "hold"))
		{
			pInput->SetGamepadInjectedKeyState(dik, true);
			if (actionId == kJUMP && dik != DIK_SPACE)
				pInput->SetGamepadInjectedKeyState(DIK_SPACE, true);
		}
		else if (0 == _stricmp(phase, "release"))
		{
			pInput->SetGamepadInjectedKeyState(dik, false);
			if (actionId == kJUMP && dik != DIK_SPACE)
				pInput->SetGamepadInjectedKeyState(DIK_SPACE, false);
		}
	}

	// Если действие не привязано в user.ltx и разрешено через синтетический/фолбэк DIK —
	// временно подключаем дескриптор клавиши в свободный слот g_key_bindings[actionId] на время вызова.
	static _keyboard s_synthKeyboard = { "kGAMEPAD_SYNTH", 0xFF, "GamepadSynth" };
	_binding* tempBinding = nullptr;
	if (actionId >= 0 && actionId < kLASTACTION)
	{
		if (g_key_bindings[actionId].m_keyboard[0] == nullptr && g_key_bindings[actionId].m_keyboard[1] == nullptr)
		{
			s_synthKeyboard.dik = dik;
			tempBinding = &g_key_bindings[actionId];
			tempBinding->m_keyboard[1] = &s_synthKeyboard;
		}
	}

	const bool bMainMenuTarget = (ctx == EGamepadContext::MainMenu) ||
	                             (MainMenu() != nullptr && MainMenu()->IsActive()) ||
	                             (g_pGameLevel == nullptr);

	bool dispatched = false;
	if (bMainMenuTarget)
	{
		if (MainMenu() != nullptr && MainMenu()->IsActive())
		{
			if (0 == _stricmp(phase, "press"))
			{
				MainMenu()->IR_OnKeyboardPress(dik);
				++m_injectionStats.totalPressInjected;
				dispatched = true;
			}
			else if (0 == _stricmp(phase, "hold"))
			{
				MainMenu()->IR_OnKeyboardHold(dik);
				++m_injectionStats.totalHoldInjected;
				dispatched = true;
			}
			else if (0 == _stricmp(phase, "release"))
			{
				MainMenu()->IR_OnKeyboardRelease(dik);
				++m_injectionStats.totalReleaseInjected;
				dispatched = true;
			}
		}
	}
	else if (g_pGameLevel != nullptr)
	{
		// CURSOR1: в режиме виртуальной мыши клик MOUSE_1 / MOUSE_2 по пустому фону чужого окна с AllowMovement(true)
		// НЕ должен проваливаться из CDialogHolder::IR_UIOnKeyboard* в Actor (kWPN_FIRE / kWPN_ZOOM).
		CUIDialogWnd* guardTir = nullptr;
		bool restoreAllowMove = false;
		bool directMouseToTirOnly = false;
		if (m_bGamepadMouseMode && (dik == MOUSE_1 || dik == MOUSE_2) && CurrentGameUI() != nullptr)
		{
			guardTir = CurrentGameUI()->TopInputReceiver();
			if (guardTir != nullptr && !guardTir->StopAnyMove())
			{
				guardTir->AllowMovement(false);
				restoreAllowMove = true;
				if (!guardTir->StopAnyMove())
					directMouseToTirOnly = true;
			}
		}

		if (directMouseToTirOnly && guardTir != nullptr && guardTir->IR_process())
		{
			const Fvector2 cp = GetUICursor().GetCursorPosition();
			if (0 == _stricmp(phase, "press"))
			{
				const EUIMessages msg = (dik == MOUSE_1) ? WINDOW_LBUTTON_DOWN : WINDOW_RBUTTON_DOWN;
				if (!guardTir->OnMouseAction(cp.x, cp.y, msg))
					guardTir->OnKeyboardAction(dik, WINDOW_KEY_PRESSED);
				++m_injectionStats.totalPressInjected;
				dispatched = true;
			}
			else if (0 == _stricmp(phase, "hold"))
			{
				guardTir->OnKeyboardHold(dik);
				++m_injectionStats.totalHoldInjected;
				dispatched = true;
			}
			else if (0 == _stricmp(phase, "release"))
			{
				const EUIMessages msg = (dik == MOUSE_1) ? WINDOW_LBUTTON_UP : WINDOW_RBUTTON_UP;
				if (!guardTir->OnMouseAction(cp.x, cp.y, msg))
					guardTir->OnKeyboardAction(dik, WINDOW_KEY_RELEASED);
				++m_injectionStats.totalReleaseInjected;
				dispatched = true;
			}
		}
		else if (0 == _stricmp(phase, "press"))
		{
			// E2.2c: если активный туториал ждёт это действие (костёр: kUSE -> bind_campfire.use_campfire), нажатие идёт
			// через сам туториал, как физическая клавиша: он вызовет функтор и, если не держит ввод (GrabInput),
			// сам передаст нажатие в m_pStoredInputReceiver (Level). Отпускание по-прежнему сразу в Level.
			if (ActiveTutorialListensForAction(actionId))
			{
				g_tutorial->IR_OnKeyboardPress(dik);
				++m_tutorialRoutedPressCount;
			}
			else
			{
				Level().IR_OnKeyboardPress(dik);
			}
			++m_injectionStats.totalPressInjected;
			dispatched = true;
		}
		else if (0 == _stricmp(phase, "hold"))
		{
			Level().IR_OnKeyboardHold(dik);
			++m_injectionStats.totalHoldInjected;
			dispatched = true;
		}
		else if (0 == _stricmp(phase, "release"))
		{
			Level().IR_OnKeyboardRelease(dik);
			++m_injectionStats.totalReleaseInjected;
			dispatched = true;
		}

		if (restoreAllowMove && CurrentGameUI() != nullptr && CurrentGameUI()->TopInputReceiver() == guardTir)
		{
			guardTir->AllowMovement(true);
		}
	}

	if (tempBinding)
	{
		tempBinding->m_keyboard[1] = nullptr;
	}

	if (dispatched)
	{
		m_injectionStats.lastInjectedDik = dik;
		m_injectionStats.lastInjectedAction = actionId;
		m_injectionStats.lastInjectedContext = ctx;
		m_injectionStats.lastInjectedPhase = phase;

		if (ps_gp_telemetry && pInput && (0 != _stricmp(phase, "hold")))
		{
			string256 line;
			xr_sprintf(line, sizeof(line),
			           "gp_ctx_inject,%u,phase=%s,ctx=%s,dik=%d,action=%d",
			           Device.dwTimeContinual,
			           phase,
			           GetContextName(ctx),
			           dik,
			           actionId);
			pInput->WriteGamepadTelemetryLine(line);
		}

		if (emitLog)
		{
			Msg("* [Gamepad] gp_ctx inject: phase=%s ctx=%s dik=%d action=%d (%s)",
			    phase,
			    GetContextName(ctx),
			    dik,
			    actionId,
			    (actionId >= 0 && actionId < kLASTACTION) ? id_to_action_name(static_cast<EGameActions>(actionId)) : "raw_dik");
		}
	}

	return dispatched;
}

namespace
{
const gpbind::BindingTrigger* FindWorldBinding(gpbind::ActionId act)
{
	return gpbind::GetActiveBindingProfile().FindBinding(gpbind::ContextId::World_Gameplay, act);
}

u32 GetWorldSingleButtonBit(gpbind::ActionId act, u32 fallbackBit = 0xFFu)
{
	const gpbind::BindingTrigger* t = FindWorldBinding(act);
	if (!t || t->button == gpbind::ButtonId::None)
		return fallbackBit;
	if (t->type == gpbind::TriggerType::Button)
		return static_cast<u32>(t->button);
	return 0xFFu;
}

void QueryWorldButtonAndHoldActions(u32 btnBit,
                                    gpbind::ActionId& outButtonAct,
                                    gpbind::ActionId& outHoldAct,
                                    u32& outHoldMs)
{
	outButtonAct = gpbind::ActionId::Invalid;
	outHoldAct = gpbind::ActionId::Invalid;
	outHoldMs = gpbind::limits::kDefaultHoldMs;
	if (btnBit >= CGamepadContextDispatcher::VBTN_TOTAL_COUNT)
		return;
	const gpbind::ButtonId targetBtn = static_cast<gpbind::ButtonId>(btnBit);
	const gpbind::BindingProfileModel& prof = gpbind::GetActiveBindingProfile();
	for (const auto& entry : prof.GetAllBindings())
	{
		if (entry.context != gpbind::ContextId::World_Gameplay || entry.trigger.button != targetBtn)
			continue;
		if (entry.trigger.type == gpbind::TriggerType::Button && outButtonAct == gpbind::ActionId::Invalid)
		{
			outButtonAct = entry.action;
		}
		else if (entry.trigger.type == gpbind::TriggerType::Hold && outHoldAct == gpbind::ActionId::Invalid)
		{
			outHoldAct = entry.action;
			outHoldMs = (entry.trigger.hold_ms > 0) ? entry.trigger.hold_ms : gpbind::limits::kDefaultHoldMs;
		}
	}
}
} // namespace

void CGamepadContextDispatcher::StepWorldMovementButtons(u32 nowContinual,
                                                         bool& outHandledL3,
                                                         bool& outHandledB,
                                                         bool& outHandledR3)
{
	outHandledL3 = false;
	outHandledB = false;
	outHandledR3 = false;

	// K2 item 4: перечитывание профиля без перезапуска игры при изменении файла на диске
	if (!m_bInSelfTest && (nowContinual - m_lastProfileDiskCheckTime >= 500 || m_bContextChangedThisFrame))
	{
		m_lastProfileDiskCheckTime = nowContinual;
		if (gpbind::ReloadActiveBindingProfileIfModifiedOnDisk())
		{
			m_bR3DualModePending = false;
			ZeroMemory(m_worldDualPending, sizeof(m_worldDualPending));
			ZeroMemory(m_worldDualHoldCommitted, sizeof(m_worldDualHoldCommitted));
		}
	}

	const bool bAllowBtns = AllowsGamepadButtons();
	const bool bInWorldGameplay = (m_currentContext == EGamepadContext::World_Gameplay) && bAllowBtns;
	const bool bInWorldOr3dPda = bInWorldGameplay;

	// Crouch / LowCrouch / Sprint теперь разрешаются в едином профильном диспетчере StepWorldWeaponButtons (K2).
	if (!bInWorldGameplay)
	{
		m_bEastDualModePending = false;
		m_bEastHoldCommitted = false;
	}

	// E1.1 (3) + K2: Freelook (по умолчанию Hold R3 >= 200 мс) и короткое действие на той же кнопке вне ADS
	const gpbind::BindingTrigger* flTrig = FindWorldBinding(gpbind::ActionId::Freelook);
	const u32 flBtn = (flTrig && flTrig->button != gpbind::ButtonId::None &&
	                   flTrig->type != gpbind::TriggerType::Chord) // GAYS v0.2: chord freelook = toggle in ExecuteWorldGameplayAction
	                      ? static_cast<u32>(flTrig->button)
	                      : 0xFFu;
	const u32 flHoldMs = (flTrig && flTrig->type == gpbind::TriggerType::Hold && flTrig->hold_ms > 0)
	                         ? flTrig->hold_ms
	                         : GAMEPAD_FREELOOK_HOLD_MS;
	const bool flIsHold = (!flTrig || flTrig->type == gpbind::TriggerType::Hold);

	if (!bInWorldOr3dPda || flBtn >= VBTN_TOTAL_COUNT || WasButtonReleasedOnContextExit(m_freelookBtnBit))
	{
		if (m_bR3FreelookActive)
		{
			m_bR3FreelookActive = false;
			if (m_r3FreelookDik > 0)
			{
				const bool bModalOpenedNow = (CurrentGameUI() != nullptr && CurrentGameUI()->TopInputReceiver() != nullptr) ||
				                             (MainMenu() != nullptr && MainMenu()->IsActive()) ||
				                             (Console != nullptr && Console->bVisible);
				if (bModalOpenedNow)
				{
					if (g_pGameLevel != nullptr && Level().CurrentEntity() != nullptr)
					{
						if (IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(Level().CurrentEntity())))
						{
							IR->IR_OnKeyboardRelease(kFREELOOK);
							++m_injectionStats.totalReleaseInjected;
						}
					}
				}
				else
				{
					InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "release", EGamepadContext::World_Gameplay, false);
				}
				m_r3FreelookDik = 0;
			}
			if (CActor* pActor = g_actor)
			{
				if (!psActorFlags.test(AF_FREELOOK_TOGGLE))
					pActor->cam_UnsetFreelook();
			}
		}
		m_bR3DualModePending = false;
	}
	else
	{
		const u32 aimBtn = GetWorldSingleButtonBit(gpbind::ActionId::Aim, VBTN_LT);
		const bool bInAds = (m_currentContext == EGamepadContext::World_Gameplay) &&
		                    ((g_actor != nullptr && g_actor->IsZoomAimingMode()) ||
		                     (aimBtn < VBTN_TOTAL_COUNT && IsButtonHeld(aimBtn)));

		if (IsButtonReleased(flBtn) && (m_bR3DualModePending || m_bR3FreelookActive))
		{
			if (flBtn == CInput::GP_BTN_RIGHT_STICK)
				outHandledR3 = true;
			if (m_bR3FreelookActive)
			{
				m_bR3FreelookActive = false;
				m_bR3DualModePending = false;
				if (m_r3FreelookDik > 0)
				{
					InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "release", m_currentContext, false);
					m_r3FreelookDik = 0;
				}
				if (CActor* pActor = g_actor)
				{
					if (!psActorFlags.test(AF_FREELOOK_TOGGLE))
						pActor->cam_UnsetFreelook();
				}
			}
			else if (m_bR3DualModePending)
			{
				m_bR3DualModePending = false;
				if (m_currentContext == EGamepadContext::World_Gameplay &&
				    (nowContinual - m_r3PressTimeContinual) < flHoldMs)
				{
					gpbind::ActionId tapAct = gpbind::ActionId::Invalid;
					gpbind::ActionId dummyHold = gpbind::ActionId::Invalid;
					u32 dummyMs = 0;
					QueryWorldButtonAndHoldActions(flBtn, tapAct, dummyHold, dummyMs);
					if (tapAct != gpbind::ActionId::Invalid)
					{
						ExecuteWorldGameplayAction(static_cast<int>(tapAct),
						                           flBtn,
						                           static_cast<int>(gpbind::TriggerType::Button),
						                           nowContinual);
					}
				}
			}
		}
		else if (!bInAds || m_bR3DualModePending || m_bR3FreelookActive)
		{
			if (flBtn == CInput::GP_BTN_RIGHT_STICK)
				outHandledR3 = true;
			if (IsButtonPressed(flBtn))
			{
				m_freelookBtnBit = flBtn;
				if (flIsHold)
				{
					m_bR3DualModePending = true;
					m_bR3FreelookActive = false;
					m_r3PressTimeContinual = nowContinual;
				}
				else
				{
					m_bR3DualModePending = false;
					m_bR3FreelookActive = true;
					m_r3FreelookDik = ResolveActionDik(kFREELOOK);
					if (m_r3FreelookDik > 0)
						InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "press", m_currentContext, false);
					if (CActor* pActor = g_actor)
					{
						if (pActor->CanUseFreelook())
							pActor->cam_SetFreelook();
					}
				}
			}
			else if (IsButtonHeld(flBtn))
			{
				if (m_bR3DualModePending && !m_bR3FreelookActive &&
				    (nowContinual - m_r3PressTimeContinual >= flHoldMs))
				{
					m_bR3FreelookActive = true;
					m_r3FreelookDik = ResolveActionDik(kFREELOOK);
					if (m_r3FreelookDik > 0)
					{
						InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "press", m_currentContext, false);
					}
					if (CActor* pActor = g_actor)
					{
						if (pActor->CanUseFreelook())
							pActor->cam_SetFreelook();
					}
				}
				else if (m_bR3FreelookActive)
				{
					if (m_r3FreelookDik > 0)
					{
						InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "hold", m_currentContext, false);
					}
					if (CActor* pActor = g_actor)
					{
						if (pActor->CanUseFreelook() && pActor->cam_freelook == eflDisabled)
							pActor->cam_SetFreelook();
					}
				}
			}
			else
			{
				// Если кнопка freelook не зажата (например, была поглощена барьером), снимаем зависший pending/active
				if (m_bR3FreelookActive)
				{
					m_bR3FreelookActive = false;
					if (m_r3FreelookDik > 0)
					{
						InjectKeyboardEvent(m_r3FreelookDik, kFREELOOK, "release", m_currentContext, false);
						m_r3FreelookDik = 0;
					}
					if (CActor* pActor = g_actor)
					{
						if (!psActorFlags.test(AF_FREELOOK_TOGGLE))
							pActor->cam_UnsetFreelook();
					}
				}
				m_bR3DualModePending = false;
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// E2.1: оружейные кнопки World_Gameplay. Все действия идут штатными путями игры:
// инъекцией привязок через CLevel::IR_OnKeyboard* (D3.2), т.е. через те же CWeapon::Action и
// Lua-колбэки on_key_press/on_key_release, что и с клавиатуры. Прямых вызовов CWeapon нет.
// ---------------------------------------------------------------------------------------------
extern BOOL useSeparateUBGLKeybind; // Weapon.cpp
extern float sens_multiple;         // Weapon.cpp

namespace
{
CWeapon* GetGamepadActorWeapon()
{
	CActor* pActor = g_actor;
	if (!pActor)
		return nullptr;
	return smart_cast<CWeapon*>(pActor->inventory().ActiveItem());
}

bool IsModalOverWorldNow()
{
	return (CurrentGameUI() != nullptr && CurrentGameUI()->TopInputReceiver() != nullptr) ||
	       (MainMenu() != nullptr && MainMenu()->IsActive()) ||
	       (Console != nullptr && Console->bVisible);
}
} // namespace

void CGamepadContextDispatcher::InjectWeaponTap(int actionId, LPCSTR tag)
{
	const int dik = ResolveActionDik(actionId);
	bool ok = false;
	if (dik > 0)
	{
		ok = InjectKeyboardEvent(dik, actionId, "press", m_currentContext, false);
		InjectKeyboardEvent(dik, actionId, "release", m_currentContext, false);
	}

	m_lastWeaponAction = tag ? tag : "tap";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;

	if (ps_gp_telemetry && pInput)
	{
		const CWeapon* pWpn = GetGamepadActorWeapon();
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_weapon,%u,action=%s,act=%d,dik=%d,ok=%d,ads=%d,lean=%d,wpn=%s",
		           Device.dwTimeContinual, m_lastWeaponAction, actionId, dik, ok ? 1 : 0, m_bLtAdsActive ? 1 : 0,
		           m_adsLeanSide, pWpn ? pWpn->cNameSect().c_str() : "none");
		pInput->WriteGamepadTelemetryLine(line);
	}
}

int CGamepadContextDispatcher::ResolveNextWeaponSlotAction()
{
	// Циклическое переключение оружейных слотов 1..4 (kWPN_1..kWPN_4: нож/топор, пистолет/ПП, основное 1, основное 2)
	// эквивалентно нажатию клавиш 1-4 на клавиатуре по реально занятым слотам актора (вместо kWPN_NEXT, меняющего патроны).
	static const u16 kWeaponSlots[4] = { KNIFE_SLOT, INV_SLOT_2, INV_SLOT_3, GRENADE_SLOT };
	static const int kSlotActions[4] = { kWPN_1, kWPN_2, kWPN_3, kWPN_4 };
	if (CActor* pActor = g_actor)
	{
		CInventory& inv = pActor->inventory();
		u16 activeSlot = inv.GetActiveSlot();
		if (activeSlot == NO_ACTIVE_SLOT)
			activeSlot = inv.GetPrevActiveSlot();

		int curIdx = -1;
		for (int i = 0; i < 4; ++i)
		{
			if (activeSlot == kWeaponSlots[i])
			{
				curIdx = i;
				break;
			}
		}
		if (curIdx < 0)
			curIdx = static_cast<int>(m_nextWeaponCycleIdx % 4);

		for (int step = 1; step <= 4; ++step)
		{
			const int idx = (curIdx + step) % 4;
			// GAYS v0.2: Y cycles firearms only (knife, axes and grenades are skipped)
			if (smart_cast<CWeaponMagazined*>(inv.ItemFromSlot(kWeaponSlots[idx])) != nullptr)
			{
				m_nextWeaponCycleIdx = static_cast<u32>(idx);
				return kSlotActions[idx];
			}
		}
	}

	const int fallbackAct = kSlotActions[m_nextWeaponCycleIdx % 4];
	m_nextWeaponCycleIdx = (m_nextWeaponCycleIdx + 1) % 4;
	return fallbackAct;
}

// E2.2: вызов прямого адаптера gp_weapon.script (без эмуляции клавиш). Адаптер возвращает "<выбор>:<почему>",
// результат пишется в телеметрию и в gp_weapon_state.
void CGamepadContextDispatcher::RunXAdapter(LPCSTR luaFn, LPCSTR phase, LPCSTR why)
{
	string256 result = "lua_unavailable";
	if (g_ai_space)
	{
		::luabind::functor<LPCSTR> fn;
		if (ai().script_engine().functor(luaFn, fn))
		{
			try
			{
				LPCSTR r = fn();
				strncpy_s(result, sizeof(result), (r && r[0]) ? r : "nil", _TRUNCATE); // длинная строка не роняет strcpy_s
			}
			catch (...)
			{
				xr_strcpy(result, "lua_exception");
				Msg("! [Gamepad] Exception in %s", luaFn);
			}
		}
		else
		{
			xr_strcpy(result, "lua_fn_missing");
		}
	}

	string512 choice;
	_snprintf_s(choice, sizeof(choice), _TRUNCATE, "%s/%s", phase, result);
	m_xLastChoice = choice;
	m_xLastChoiceTime = Device.dwTimeContinual;
	++m_xActionCount;
	m_lastWeaponAction = "x_adapter";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;

	if (ps_gp_telemetry && pInput)
	{
		const CWeapon* pWpn = GetGamepadActorWeapon();
		string512 line;
		_snprintf_s(line, sizeof(line), _TRUNCATE, "gp_weapon_x,%u,phase=%s,why=%s,fn=%s,result=%s,state=%u,lt=%d,wpn=%s",
		           Device.dwTimeContinual, phase, why, luaFn, result, pWpn ? pWpn->GetState() : 0u,
		           m_bLtAdsActive ? 1 : 0, pWpn ? pWpn->cNameSect().c_str() : "none");
		pInput->WriteGamepadTelemetryLine(line);
	}
}

// E2.2: короткое X при цели HUD — штатное действие kUSE (press + release в одном кадре). Если нажатие открыло
// окно (разговор, обыск, ящик), release в новое окно не посылаем — как в общем цикле D3.2.
// E3.1: если цель — предмет в мире (m_pObjectWeLookingAt->cast_inventory_item() или COD m_pPickUpItem),
// синхронно вызываем PickupModeUpdate/PickupModeUpdate_COD (при AF_MULTI_ITEM_PICKUP=1 ActorUse не взводит m_bPickupMode).
// INV-REWORK-21: удержание X при цели HUD (Dot Marks: «(Удерживайте) Взять тайник», «Разрядить» и т.п.).
// Вместо проверки магазина посылаем kUSE press и держим его до отпускания X — Dot Marks сам считает время
// удержания, рисует кольцо и выбирает действие, как при удержании F на клавиатуре.
bool CGamepadContextDispatcher::BeginUseHoldFromX(u32 btn)
{
	if (m_xUseHoldDik > 0 || !HasInteractiveTargetForUse())
		return false;
	const int dik = ResolveActionDik(kUSE);
	if (dik <= 0)
		return false;
	if (!InjectKeyboardEvent(dik, kUSE, "press", m_currentContext, false))
		return false;
	m_xUseHoldDik = dik;
	m_xUseHoldBtn = btn;
	m_xLastChoice = "hold/use:hud_target";
	m_xLastChoiceTime = Device.dwTimeContinual;
	++m_xActionCount;
	return true;
}

void CGamepadContextDispatcher::EndUseHoldFromX(LPCSTR why)
{
	if (m_xUseHoldDik <= 0)
		return;
	const int dik = m_xUseHoldDik;
	m_xUseHoldDik = 0;
	m_xUseHoldBtn = VBTN_TOTAL_COUNT;
	// CR1-05: if holding X opened a modal dialog (stash/corpse/talk), clear injected key and release kUSE on
	// the world entity only, never routing IR_OnKeyboardRelease(dik) into the newly opened UI window.
	const bool bModalOpenedNow = (m_currentContext != EGamepadContext::World_Gameplay) ||
	                             (CurrentGameUI() != nullptr && CurrentGameUI()->TopInputReceiver() != nullptr) ||
	                             (MainMenu() != nullptr && MainMenu()->IsActive()) ||
	                             (Console != nullptr && Console->bVisible);
	if (bModalOpenedNow)
	{
		if (pInput)
			pInput->SetGamepadInjectedKeyState(dik, false);
		if (g_pGameLevel != nullptr && Level().CurrentEntity() != nullptr)
		{
			if (IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(Level().CurrentEntity())))
			{
				IR->IR_OnKeyboardRelease(kUSE);
				++m_injectionStats.totalReleaseInjected;
			}
		}
	}
	else
	{
		InjectKeyboardEvent(dik, kUSE, "release", EGamepadContext::World_Gameplay, false);
	}
	if (ps_gp_telemetry && pInput)
		Msg("gp_weapon_x,%u,phase=hold_release,why=%s,act=kUSE,dik=%d", Device.dwTimeContinual, why ? why : "", dik);
}

void CGamepadContextDispatcher::TapUseFromX(LPCSTR targetWhy)
{
	if (!targetWhy || !targetWhy[0])
		targetWhy = "hud_target";
	const int dik = ResolveActionDik(kUSE);
	bool ok = false;
	bool releaseSent = false;
	bool itemPickupTriggered = false;
	LPCSTR pickedSect = "none";
	// E2.2c: при цели-туториале (костёр) маршрутизация нажатия -- в InjectKeyboardEvent
	if (dik > 0)
	{
		CUIDialogWnd* const tirBefore = (CurrentGameUI() != nullptr) ? CurrentGameUI()->TopInputReceiver() : nullptr;
		ok = InjectKeyboardEvent(dik, kUSE, "press", m_currentContext, false);
		CUIDialogWnd* const tirAfter = (CurrentGameUI() != nullptr) ? CurrentGameUI()->TopInputReceiver() : nullptr;
		if (ok && tirAfter == tirBefore && !IsModalOverWorldNow())
		{
			if (CActor* pActor = g_actor)
			{
				itemPickupTriggered = CActorPickupProbe::TriggerGamepadWorldItemPickup(pActor, &pickedSect);
			}
			InjectKeyboardEvent(dik, kUSE, "release", m_currentContext, false);
			releaseSent = true;
		}
		if (ok)
			PollLuaBridgeHints();
	}

	string128 choice;
	if (itemPickupTriggered)
	{
		xr_sprintf(choice, sizeof(choice), "tap/use:world_item_pickup:%s", pickedSect ? pickedSect : "item");
		m_fieldLastChoice = choice;
		m_fieldLastChoiceTime = Device.dwTimeContinual;
		++m_fieldActionCount;
	}
	else
	{
		xr_sprintf(choice, sizeof(choice), "tap/use:%s%s", targetWhy, ok ? "" : "_inject_failed");
	}
	m_xLastChoice = choice;
	m_xLastChoiceTime = Device.dwTimeContinual;
	++m_xActionCount;
	m_lastWeaponAction = itemPickupTriggered ? "x_pickup_item" : "x_use";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;

	if (ps_gp_telemetry && pInput)
	{
		const CWeapon* pWpn = GetGamepadActorWeapon();
		string256 line;
		_snprintf_s(line, sizeof(line), _TRUNCATE,
		            "gp_weapon_x,%u,phase=tap,why=%s,act=kUSE,dik=%d,ok=%d,release=%d,pickup=%d,item=%s,state=%u,wpn=%s,tut_routed=%u",
		            Device.dwTimeContinual,
		            itemPickupTriggered ? "world_item_pickup" : targetWhy,
		            dik, ok ? 1 : 0, releaseSent ? 1 : 0, itemPickupTriggered ? 1 : 0,
		            pickedSect ? pickedSect : "none", pWpn ? pWpn->GetState() : 0u,
		            pWpn ? pWpn->cNameSect().c_str() : "none", m_tutorialRoutedPressCount);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

// E2.2d: короткое X рядом с подсказкой (gp_x_target_angle), но не точно на ней: ни kUSE, ни перезарядки
void CGamepadContextDispatcher::NoteXNearTargetNoReload(LPCSTR detail)
{
	string256 choice;
	xr_sprintf(choice, sizeof(choice), "tap/none:%s", (detail && detail[0]) ? detail : "near_target");
	m_xLastChoice = choice;
	m_xLastChoiceTime = Device.dwTimeContinual;
	++m_xActionCount;
	m_lastWeaponAction = "x_near_target_no_reload";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;
	if (ps_gp_telemetry && pInput)
	{
		const CWeapon* pWpn = GetGamepadActorWeapon();
		string256 line;
		_snprintf_s(line, sizeof(line), _TRUNCATE, "gp_weapon_x,%u,phase=tap,why=%s,act=none,angle=%.1f,state=%u,wpn=%s",
		            Device.dwTimeContinual, (detail && detail[0]) ? detail : "near_target", ps_gp_x_target_angle,
		            pWpn ? pWpn->GetState() : 0u, pWpn ? pWpn->cNameSect().c_str() : "none");
		pInput->WriteGamepadTelemetryLine(line);
	}
}

// E3.1: вызов полевого адаптера gp_field.script (ПНВ без импульса kNIGHT_VISION, протирка маски, КПК, болт/граната).
void CGamepadContextDispatcher::RunFieldAdapter(LPCSTR luaFn, LPCSTR phase, LPCSTR why, string256* outResult)
{
	string256 result = "lua_unavailable";
	if (g_ai_space)
	{
		::luabind::functor<LPCSTR> fn;
		if (ai().script_engine().functor(luaFn, fn))
		{
			try
			{
				LPCSTR r = fn();
				strncpy_s(result, sizeof(result), (r && r[0]) ? r : "nil", _TRUNCATE);
			}
			catch (...)
			{
				xr_strcpy(result, "lua_exception");
				Msg("! [Gamepad] Exception in %s", luaFn);
			}
		}
		else
		{
			xr_strcpy(result, "lua_fn_missing");
		}
	}

	if (outResult)
		xr_strcpy(*outResult, result);

	string512 choice;
	_snprintf_s(choice, sizeof(choice), _TRUNCATE, "%s/%s", phase, result);
	m_fieldLastChoice = choice;
	m_fieldLastChoiceTime = Device.dwTimeContinual;
	++m_fieldActionCount;
	m_lastWeaponAction = "field_adapter";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;

	if (ps_gp_telemetry && pInput)
	{
		const CWeapon* pWpn = GetGamepadActorWeapon();
		string512 line;
		_snprintf_s(line, sizeof(line), _TRUNCATE, "gp_field,%u,phase=%s,why=%s,fn=%s,result=%s,ads=%d,wpn=%s",
		            Device.dwTimeContinual, phase, why, luaFn, result, m_bLtAdsActive ? 1 : 0,
		            pWpn ? pWpn->cNameSect().c_str() : "none");
		pInput->WriteGamepadTelemetryLine(line);
	}
}

void CGamepadContextDispatcher::SetAdsLean(int side, LPCSTR tag)
{
	const int prev = m_adsLeanSide;
	if (prev == side)
		return;
	m_adsLeanSide = side;

	// В режиме AF_LOOKOUT_TOGGLE наклон переключается нажатием (ActorInput.cpp IR_OnKeyboardPress kL/R_LOOKOUT);
	// без него наклон держится инъекцией hold каждый кадр (ActorInput.cpp IR_OnKeyboardHold), а снимается
	// сам — Actor.cpp сбрасывает mcLookout в wishful каждый кадр, если hold не пришёл.
	CActor* pActor = g_actor;
	if (psActorFlags.test(AF_LOOKOUT_TOGGLE) && pActor)
	{
		const u32 wf = pActor->get_state_wishful();
		const bool bWorld = (m_currentContext == EGamepadContext::World_Gameplay) && !IsModalOverWorldNow();
		auto tapLean = [&](int act) {
			if (bWorld)
			{
				InjectWeaponTap(act, tag);
			}
			else if (IInputReceiver* IR = static_cast<IInputReceiver*>(pActor))
			{
				IR->IR_OnKeyboardPress(static_cast<EGameActions>(act));
				IR->IR_OnKeyboardRelease(static_cast<EGameActions>(act));
			}
		};
		if (side != 0)
		{
			const u32 bit = (side < 0) ? mcLLookout : mcRLookout;
			if (!(wf & bit))
				tapLean((side < 0) ? kL_LOOKOUT : kR_LOOKOUT); // противоположный бит снимает сам обработчик
		}
		else if (prev != 0)
		{
			const u32 bit = (prev < 0) ? mcLLookout : mcRLookout;
			if (wf & bit)
				tapLean((prev < 0) ? kL_LOOKOUT : kR_LOOKOUT);
		}
	}
	m_lastWeaponAction = tag ? tag : "lean";
	m_lastWeaponActionTime = Device.dwTimeContinual;
	++m_weaponActionCount;
}

void CGamepadContextDispatcher::ExecuteWorldGameplayAction(int gpbindActionId,
                                                           u32 btnBit,
                                                           int triggerType,
                                                           u32 nowContinual)
{
	const gpbind::ActionId act = static_cast<gpbind::ActionId>(gpbindActionId);
	const bool isHold = (triggerType == static_cast<int>(gpbind::TriggerType::Hold));

	switch (act)
	{
	case gpbind::ActionId::Jump:
		InjectWeaponTap(kJUMP, "world_jump");
		break;
	case gpbind::ActionId::Crouch:
	{
		const int crouchDik = ResolveActionDik(kCROUCH);
		if (crouchDik > 0)
		{
			InjectKeyboardEvent(crouchDik, kCROUCH, "press", m_currentContext, false);
			InjectKeyboardEvent(crouchDik, kCROUCH, "release", m_currentContext, false);
		}
		if (CActor* pActor = g_actor)
			pActor->SetGamepadCrouchToggle();
		break;
	}
	case gpbind::ActionId::LowCrouch:
	{
		const int crouchDik = ResolveActionDik(kCROUCH);
		if (crouchDik > 0)
		{
			InjectKeyboardEvent(crouchDik, kCROUCH, "press", m_currentContext, false);
			InjectKeyboardEvent(crouchDik, kCROUCH, "release", m_currentContext, false);
		}
		if (CActor* pActor = g_actor)
			pActor->SetGamepadLowCrouchHold();
		break;
	}
	case gpbind::ActionId::Sprint:
	{
		const int sprintDik = ResolveActionDik(kSPRINT_TOGGLE);
		if (sprintDik > 0)
		{
			InjectKeyboardEvent(sprintDik, kSPRINT_TOGGLE, "press", m_currentContext, false);
			InjectKeyboardEvent(sprintDik, kSPRINT_TOGGLE, "release", m_currentContext, false);
		}
		if (CActor* pActor = g_actor)
			pActor->SetGamepadSprintToggle();
		break;
	}
	case gpbind::ActionId::MeleeQuick:
		InjectWeaponTap(kCUSTOM24, "field_quick_melee_kcustom24");
		RunFieldAdapter("gp_field.on_quick_melee_tap", "tap", "r3_short_outside_ads");
		break;
	case gpbind::ActionId::NextWeapon:
	{
		string256 result;
		RunFieldAdapter("gp_weapon_select.draw_pinned", "tap", "weapon_select_draw_pinned", &result);
		if (0 == xr_strcmp(result, "queued"))
			break;
		const int slotAct = ResolveNextWeaponSlotAction();
		InjectWeaponTap(slotAct, "world_next_weapon");
		break;
	}
	case gpbind::ActionId::QuickBoltOrSlot:
		RunFieldAdapter("gp_wheel.on_quick_consumable", "tap", "wheel_quick_consumable");
		break;
	case gpbind::ActionId::WeaponWheel:
		m_bWheelLbWasPhysicallyDown = true;
		m_wheelHoldBtnBit = btnBit;
		RunFieldAdapter("gp_field.lb_wheel_hold", isHold ? "hold" : "tap", "lb_held_ge_250ms");
		PollLuaBridgeHints();
		break;
	case gpbind::ActionId::GrenadeQuick:
	{
		if (triggerType == static_cast<int>(gpbind::TriggerType::Button))
			ConsumeButtonUntilRelease(btnBit);
		InjectWeaponTap(kCUSTOM19, "field_quick_grenade_kcustom19");
		string256 rbRes = "";
		RunFieldAdapter("gp_field.on_quick_grenade", "tap", "rb_outside_ads", &rbRes);
		if (strstr(rbRes, "fallback_slot4:") == rbRes)
			InjectWeaponTap(kWPN_4, "field_grenade_slot4_fallback");
		break;
	}
	case gpbind::ActionId::LeanLeft:
		ConsumeButtonUntilRelease(btnBit);
		SetAdsLean(m_adsLeanSide == -1 ? 0 : -1, "lean_left_toggle");
		break;
	case gpbind::ActionId::LeanRight:
		ConsumeButtonUntilRelease(btnBit);
		SetAdsLean(m_adsLeanSide == 1 ? 0 : 1, "lean_right_toggle");
		break;
	case gpbind::ActionId::TorchToggle:
		InjectWeaponTap(kTORCH, "field_torch_toggle");
		RunFieldAdapter("gp_field.on_torch_tap", "tap", "dpad_up_short_ktorch");
		break;
	case gpbind::ActionId::NightVision:
		RunFieldAdapter("gp_field.toggle_nvg", "hold", "dpad_up_held_ge_300ms");
		break;
	case gpbind::ActionId::Pda3DToggle:
		InjectWeaponTap(kCUSTOM20, "field_3dpda_kcustom20");
		RunFieldAdapter("gp_field.on_3d_pda_tap", "tap", "dpad_down_short_kcustom20");
		break;
	case gpbind::ActionId::MaskClean:
	{
		string256 maskRes = "";
		RunFieldAdapter("gp_field.clean_mask", "hold", "dpad_down_held_ge_300ms", &maskRes);
		if (strstr(maskRes, "fallback_kcam_zoom_in:") == maskRes)
			InjectWeaponTap(kCAM_ZOOM_IN, "field_mask_clean_fallback");
		break;
	}
	case gpbind::ActionId::DetectorToggle:
		InjectWeaponTap(kDETECTOR, "detector");
		RunFieldAdapter("gp_field.on_detector_tap", "tap", "dpad_left_short_kdetector");
		break;
	case gpbind::ActionId::UnderbarrelToggle:
	{
		const int ubglAct = useSeparateUBGLKeybind ? kCUSTOM21 : kWPN_FUNC;
		InjectWeaponTap(ubglAct, useSeparateUBGLKeybind ? "ubgl_kcustom21" : "ubgl_via_wpn_func");
		break;
	}
	case gpbind::ActionId::FireModeNext:
		InjectWeaponTap(kWPN_FIREMODE_NEXT, "firemode_next");
		break;
	case gpbind::ActionId::AmmoTypeNext:
		if (isHold)
		{
			// старый профиль: AmmoTypeNext на удержании = колесо патронов
			m_bWheelLbWasPhysicallyDown = true;
			m_wheelHoldBtnBit = btnBit;
			RunFieldAdapter("gp_wheel.open_ammo_wheel", "hold", "ammo_type_hold_legacy");
			m_lastWeaponAction = "ammo_type_wheel";
			PollLuaBridgeHints();
			break;
		}
		// GAYS v0.2: tap = next ammo type of the active barrel (UBGL too). Not kWPN_NEXT: in G.A.M.M.A. its release
		// opens the ammo wheel (item_weapon.on_key_release), so the switch goes straight through gp_compat.
		RunFieldAdapter("gp_field.on_ammo_next", "tap", "ammo_type_next");
		m_lastWeaponAction = "ammo_type_next";
		break;
	case gpbind::ActionId::ContextUseReload:
	{
		string128 xTargetDetail;
		const EXTapTarget xTarget = ResolveXTapTarget(xTargetDetail);
		if (xTarget != EXTapTarget::None)
		{
			m_xLastReloadStartTime = 0;
			TapUseFromX(xTargetDetail);
		}
		else
		{
			RunXAdapter("gp_weapon.x_reload", "tap", "no_hud_target");
			m_xLastReloadStartTime =
			    (m_xLastChoice.size() && strstr(m_xLastChoice.c_str(), "/reload_started:")) ? nowContinual : 0;
		}
		break;
	}
	case gpbind::ActionId::AmmoCheck:
		m_xLastReloadStartTime = 0;
		if (BeginUseHoldFromX(btnBit))
			break;
		RunXAdapter("gp_weapon.x_ammo_check", "hold", "held_ge_350ms");
		break;
	case gpbind::ActionId::FastReload:
		m_xLastReloadStartTime = 0;
		RunXAdapter("gp_weapon.x_fast_reload", "chord_lt_x", "profile_fast_reload");
		break;
	case gpbind::ActionId::PauseMenu:
		InjectWeaponTap(kQUIT, "world_pause_menu");
		break;
	case gpbind::ActionId::PdaFullToggle:
		InjectWeaponTap(kINVENTORY, "world_inventory_toggle");
		break;
	case gpbind::ActionId::Fire:
		InjectWeaponTap(kWPN_FIRE, "world_fire_tap");
		break;
	// --- GAYS v0.2 (2026-11-13) ---
	case gpbind::ActionId::KnifeDraw:
	{
		// INV-REWORK-28b (Миша 2026-11-13): нож может лежать и в MELEE (слот 1, туда влезает и пистолет —
		// ui_inventory slot_cell[2] = {2,1,3}), и в VISION (слот 5, st_equ_binocular_cell = "VISION").
		// gp_field.vision_draw_slot: нож где бы он ни был -> его слот; ножа нет -> что лежит в VISION (бинокль).
		string256 rbRes = "";
		RunFieldAdapter("gp_field.vision_draw_slot", "hold", "rb_hold_vision", &rbRes);
		if (strstr(rbRes, "slot:5") == rbRes)
			InjectWeaponTap(kWPN_5, "vision_draw_wpn5");
		else if (strstr(rbRes, "slot:1") == rbRes)
			InjectWeaponTap(kWPN_1, "knife_draw_wpn1");
		else
			m_lastWeaponAction = "rb_hold_nothing";
		break;
	}
	case gpbind::ActionId::WeaponLower:
		InjectWeaponTap(kSAFEMODE, "weapon_lower_safemode");
		break;
	case gpbind::ActionId::PdaFlat:
		InjectWeaponTap(kACTIVE_JOBS, "pda_flat_active_jobs");
		break;
	case gpbind::ActionId::AmmoWheel:
		m_bWheelLbWasPhysicallyDown = true;
		m_wheelHoldBtnBit = btnBit;
		RunFieldAdapter("gp_wheel.open_ammo_wheel", "hold", "ammo_wheel_hold");
		m_lastWeaponAction = "ammo_type_wheel";
		PollLuaBridgeHints();
		break;
	case gpbind::ActionId::BoltThrow:
		InjectWeaponTap(kWPN_6, "bolt_wpn6");
		break;
	case gpbind::ActionId::WeaponSelect:
	{
		string256 result;
		RunFieldAdapter("gp_weapon_select.open", "hold", "weapon_select_open", &result);
		if (0 == xr_strcmp(result, "opened"))
		{
			m_weaponSelectorOpen = true;
			m_wheelHoldBtnBit = btnBit;
			m_bWheelLbWasPhysicallyDown = true;
			ResetWheelSectorState();
			PollLuaBridgeHints();
		}
		break;
	}
	case gpbind::ActionId::Freelook:
	{
		// GAYS v0.2: chord LB + B toggles freelook (no stick hold needed)
		CActor* pActor = g_actor;
		if (!m_bChordFreelookOn)
		{
			m_bChordFreelookOn = true;
			m_chordFreelookDik = ResolveActionDik(kFREELOOK);
			if (m_chordFreelookDik > 0)
				InjectKeyboardEvent(m_chordFreelookDik, kFREELOOK, "press", m_currentContext, false);
			if (pActor && pActor->CanUseFreelook())
				pActor->cam_SetFreelook();
		}
		else
		{
			m_bChordFreelookOn = false;
			if (m_chordFreelookDik > 0)
				InjectKeyboardEvent(m_chordFreelookDik, kFREELOOK, "release", m_currentContext, false);
			m_chordFreelookDik = 0;
			if (pActor)
				pActor->cam_UnsetFreelook();
		}
		m_lastWeaponAction = m_bChordFreelookOn ? "freelook_toggle_on" : "freelook_toggle_off";
		break;
	}
	case gpbind::ActionId::WheelMaskClean:
		// FIX-LIVE2 п.15: LB + R3 до открытия колеса — сразу протереть маску (gp_qaw_patch.shortcut без колеса)
		RunFieldAdapter("gp_wheel.on_shortcut_mask", "chord", "world_lb_r3_mask");
		break;
	case gpbind::ActionId::WheelLaser:
		RunFieldAdapter("gp_wheel.on_shortcut_laser", "chord", "world_lb_dpr_laser");
		break;
	case gpbind::ActionId::TutorialPrompt:
		// GAYS v0.2: LB + View; tap/hold is resolved on View release (StepWorldWeaponButtons)
		m_tutorialChordBtn = btnBit;
		m_tutorialChordStart = nowContinual;
		m_tutorialChordHoldSent = false;
		break;
	default:
		break;
	}
}

u32 CGamepadContextDispatcher::StepWorldWeaponButtons(u32 nowContinual, bool handledR3)
{
	u32 handled = 0;
	const bool bWorld = (m_currentContext == EGamepadContext::World_Gameplay) && AllowsGamepadButtons();
	CActor* pActor = g_actor;
	CWeapon* pWpn = GetGamepadActorWeapon();
	const u32 aimBtnBit = GetWorldSingleButtonBit(gpbind::ActionId::Aim, VBTN_LT);
	const u32 ltBit = (aimBtnBit < VBTN_TOTAL_COUNT) ? (1u << aimBtnBit) : 0u;

	// --- (3) Aim (по умолчанию LT) — ADS по удержанию (D16), независимо от AF_AIM_TOGGLE ---
	auto exitLtAds = [&](LPCSTR tagOverride) {
		m_bLtAdsActive = false;
		const int zoomDik = ResolveActionDik(kWPN_ZOOM);
		const bool bToggle = psActorFlags.test(AF_AIM_TOGGLE);
		const bool bZoomedNow = (pWpn != nullptr && pWpn->IsZoomed());
		const bool bNeedToggleOut = bToggle && bZoomedNow && !m_bLtAdsAdopted;
		const bool bAdopted = m_bLtAdsAdopted;
		m_bLtAdsAdopted = false;
		if (bAdopted)
		{
			// kWPN_ZOOM press не посылался — release тоже не шлём
		}
		else if (!bWorld && IsModalOverWorldNow())
		{
			if (IInputReceiver* IR = pActor ? static_cast<IInputReceiver*>(pActor) : nullptr)
			{
				if (bNeedToggleOut)
					IR->IR_OnKeyboardPress(kWPN_ZOOM);
				IR->IR_OnKeyboardRelease(kWPN_ZOOM);
			}
		}
		else if (zoomDik > 0)
		{
			if (bNeedToggleOut)
				InjectKeyboardEvent(zoomDik, kWPN_ZOOM, "press", EGamepadContext::World_Gameplay, false);
			InjectKeyboardEvent(zoomDik, kWPN_ZOOM, "release", EGamepadContext::World_Gameplay, false);
		}
		m_lastWeaponAction = tagOverride ? tagOverride : (bAdopted ? "ads_exit_kept_external_zoom" : (bNeedToggleOut ? "ads_exit_toggle" : "ads_exit"));
		m_lastWeaponActionTime = nowContinual;
		++m_weaponActionCount;
		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_weapon,%u,action=%s,act=%d,dik=%d,ok=%d,ads=%d,lean=%d,wpn=%s",
			           nowContinual, m_lastWeaponAction, kWPN_ZOOM, zoomDik, (zoomDik > 0) ? 1 : 0, 0,
			           m_adsLeanSide, pWpn ? pWpn->cNameSect().c_str() : "none");
			pInput->WriteGamepadTelemetryLine(line);
		}
	};

	const bool bLtReleased = (aimBtnBit >= VBTN_TOTAL_COUNT) ||
	                         IsButtonReleased(aimBtnBit) ||
	                         WasButtonReleasedOnContextExit(aimBtnBit) ||
	                         !bWorld;
	if (m_bLtAdsActive && bLtReleased)
	{
		exitLtAds(nullptr);
	}

	// FIX-LIVE2 п.18.1 (Миша 2026-11-16, 18а): во время анимации предмета (FDDA: аптечка, еда — частичная блокировка,
	// только движение) LT + LB/RB наклоняют героя без подъёма прицела; после анимации оружие достаётся в том же
	// наклоне, пока LT удержан (дальше обычная ветка ADS ниже). Прочие кнопки в этом контексте не трогаем.
	const bool bFddaLean = !bWorld && m_currentContext == EGamepadContext::Cutscene_InputBlock &&
	                       m_subFlags.bPartialMoveOnlyBlock && !m_subFlags.bCutsceneFullBlock &&
	                       pActor != nullptr && pActor->g_Alive() && AllowsGamepadButtons();
	if (bFddaLean)
	{
		const gpbind::ActionId leanActs[2] = { gpbind::ActionId::LeanLeft, gpbind::ActionId::LeanRight };
		bool modHeld = false;
		for (u32 i = 0; i < 2; ++i)
		{
			const gpbind::BindingTrigger* t = FindWorldBinding(leanActs[i]);
			if (!t || t->type != gpbind::TriggerType::Chord || t->button == gpbind::ButtonId::None ||
			    t->modifier == gpbind::ButtonId::None)
				continue;
			const u32 mod = static_cast<u32>(t->modifier);
			const u32 btn = static_cast<u32>(t->button);
			if (mod >= VBTN_TOTAL_COUNT || btn >= VBTN_TOTAL_COUNT || !IsButtonHeld(mod))
				continue;
			modHeld = true;
			if (IsButtonPressed(btn))
			{
				ConsumeButtonUntilRelease(btn);
				const int side = (i == 0) ? -1 : 1;
				SetAdsLean(m_adsLeanSide == side ? 0 : side, (i == 0) ? "lean_left_fdda" : "lean_right_fdda");
				handled |= (1u << btn);
			}
		}
		if (!modHeld)
			SetAdsLean(0, "lean_reset_fdda_lt_up");
		else if (m_adsLeanSide != 0 && !psActorFlags.test(AF_LOOKOUT_TOGGLE))
		{
			if (IInputReceiver* IR = static_cast<IInputReceiver*>(pActor))
				IR->IR_OnKeyboardHold(static_cast<EGameActions>((m_adsLeanSide < 0) ? kL_LOOKOUT : kR_LOOKOUT));
		}
	}

	if (!bWorld)
	{
		if (m_bChordFreelookOn)
		{
			m_bChordFreelookOn = false;
			const int flDik = m_chordFreelookDik;
			m_chordFreelookDik = 0;
			if (flDik > 0)
			{
				if (IsModalOverWorldNow())
				{
					if (pInput)
						pInput->SetGamepadInjectedKeyState(flDik, false);
					if (IInputReceiver* IR = pActor ? static_cast<IInputReceiver*>(pActor) : nullptr)
						IR->IR_OnKeyboardRelease(kFREELOOK);
				}
				else
				{
					InjectKeyboardEvent(flDik, kFREELOOK, "release", EGamepadContext::World_Gameplay, false);
				}
			}
			if (pActor)
				pActor->cam_UnsetFreelook();
		}
		if (!bFddaLean)
			SetAdsLean(0, "lean_reset_ctx");
		for (u32 b = 0; b < VBTN_TOTAL_COUNT; ++b)
		{
			m_worldDualPending[b] = false;
			m_worldDualHoldCommitted[b] = false;
		}
		m_bDpadLeftPending = m_bDpadLeftHoldCommitted = false;
		m_bDpadRightPending = m_bDpadRightHoldCommitted = false;
		m_bDpadUpPending = m_bDpadUpHoldCommitted = false;
		m_bDpadDownPending = m_bDpadDownHoldCommitted = false;
		m_bLbPending = m_bLbHoldCommitted = false;
		m_bXPending = m_bXHoldCommitted = false;
		EndUseHoldFromX("ctx_left_world");
		return handled;
	}

	if (aimBtnBit < VBTN_TOTAL_COUNT)
	{
		if (IsButtonPressed(aimBtnBit))
		{
			handled |= ltBit;
			const bool bToggle = psActorFlags.test(AF_AIM_TOGGLE);
			const int zoomDik = ResolveActionDik(kWPN_ZOOM);
			if (!(bToggle && pWpn != nullptr && pWpn->IsZoomed()))
			{
				if (zoomDik > 0 && InjectKeyboardEvent(zoomDik, kWPN_ZOOM, "press", m_currentContext, false))
					m_bLtAdsActive = true;
			}
			else
			{
				m_bLtAdsActive = true;
				m_bLtAdsAdopted = true;
			}
			m_lastWeaponAction = "ads_enter";
			m_lastWeaponActionTime = nowContinual;
			++m_weaponActionCount;
			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_weapon,%u,action=%s,act=%d,dik=%d,ok=%d,ads=%d,lean=%d,wpn=%s",
				           nowContinual, m_lastWeaponAction, kWPN_ZOOM, zoomDik, m_bLtAdsActive ? 1 : 0, m_bLtAdsActive ? 1 : 0,
				           m_adsLeanSide, pWpn ? pWpn->cNameSect().c_str() : "none");
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
		else if (IsButtonHeld(aimBtnBit) || IsButtonReleased(aimBtnBit))
		{
			handled |= ltBit;
		}
	}

	// FIX-LIVE3 п.10/11 (Миша 2026-11-16): LT + крестовина вверх -> Free Zoom (gp_compat.on_free_zoom_chord).
	// Срабатывает по фронту нажатия крестовины, а не по навигационному триггеру: при ходьбе LS вверх уже
	// держит nav Up (lstick_up), и нажатие dpad_up давало только «повтор» — связка не срабатывала (п.10).
	// Работает и без оружия в руках (п.11), поэтому стоит до проверки ADS. Отпускание LT снимает свой
	// Free Zoom (без оружия события actor_on_weapon_zoom_out нет).
	static bool s_freeZoomUpOwned = false;
	{
		const u32 upBtn = CInput::GP_BTN_DPAD_UP;
		const bool ltHeld = aimBtnBit < VBTN_TOTAL_COUNT && IsButtonHeld(aimBtnBit);
		if (!IsButtonHeld(upBtn) && !IsButtonPressed(upBtn))
			s_freeZoomUpOwned = false;
		if (ltHeld && IsButtonPressed(upBtn) && m_activeInjectedDik[upBtn] <= 0 && g_ai_space)
		{
			::luabind::functor<bool> fzFn;
			if (ai().script_engine().functor("gp_compat.on_free_zoom_chord", fzFn))
			{
				try
				{
					s_freeZoomUpOwned = fzFn();
				}
				catch (...)
				{
					s_freeZoomUpOwned = false;
				}
			}
		}
		if (s_freeZoomUpOwned)
		{
			handled |= 1u << upBtn;
			if (m_worldDualPending[upBtn])
			{
				m_worldDualPending[upBtn] = m_worldDualHoldCommitted[upBtn] = false;
				ConsumeButtonUntilRelease(upBtn);
			}
		}
		static bool s_ltWasHeld = false;
		if (s_ltWasHeld && !ltHeld && g_ai_space)
		{
			::luabind::functor<bool> fzOut;
			if (ai().script_engine().functor("gp_compat.on_free_zoom_ads_out", fzOut))
			{
				try
				{
					fzOut();
				}
				catch (...)
				{
				}
			}
		}
		s_ltWasHeld = ltHeld;
	}

	const bool bInAds = (aimBtnBit < VBTN_TOTAL_COUNT && IsButtonHeld(aimBtnBit)) ||
	                    (pActor != nullptr && pActor->IsZoomAimingMode());

	if (!bInAds)
	{
		SetAdsLean(0, "lean_reset_ads_exit");
	}
	else
	{
		if (m_adsLeanSide != 0 && !psActorFlags.test(AF_LOOKOUT_TOGGLE))
		{
			const int act = (m_adsLeanSide < 0) ? kL_LOOKOUT : kR_LOOKOUT;
			const int dik = ResolveActionDik(act);
			if (dik > 0)
				InjectKeyboardEvent(dik, act, "hold", m_currentContext, false);
		}

		// В ADS крестовина Вверх/Вниз — кратность 3DSS (kWPN_ZOOM_INC/DEC), R3 — смена прицела (kWPN_FUNC)
		ENavDir navDir = ENavDir::None;
		bool navRepeat = false;
		LPCSTR navSrc = "none";
		const bool navNow = IsNavDirTriggeredThisFrame(&navDir, &navRepeat, &navSrc);
		const u32 dpadVert[2] = { CInput::GP_BTN_DPAD_UP, CInput::GP_BTN_DPAD_DOWN };
		for (u32 i = 0; i < 2; ++i)
		{
			const u32 btn = dpadVert[i];
			if (m_worldDualPending[btn])
			{
				m_worldDualPending[btn] = m_worldDualHoldCommitted[btn] = false;
				ConsumeButtonUntilRelease(btn);
			}
			if (m_activeInjectedDik[btn] > 0)
				continue;
			if (IsButtonPressed(btn) || IsButtonHeld(btn) || IsButtonReleased(btn))
				handled |= 1u << btn;
			const bool bThisDir = navNow && ((i == 0 && navDir == ENavDir::Up) || (i == 1 && navDir == ENavDir::Down)) &&
			                      navSrc && (0 == xr_strcmp(navSrc, (i == 0) ? "dpad_up" : "dpad_down"));
			// FIX-LIVE2 п.21 / FIX-LIVE3 п.10: связку LT + вверх (Free Zoom) разобрали выше, до проверки ADS;
			// у оптики (Free Zoom отказался) та же связка — кратность 3DSS, как раньше.
			if (i == 0 && s_freeZoomUpOwned)
				continue;
			if (bThisDir && IsButtonHeld(btn))
				InjectWeaponTap((i == 0) ? kWPN_ZOOM_INC : kWPN_ZOOM_DEC, (i == 0) ? "zoom_inc_3dss" : "zoom_dec_3dss");
		}

		if (!handledR3 && IsButtonPressed(CInput::GP_BTN_RIGHT_STICK))
		{
			handled |= 1u << CInput::GP_BTN_RIGHT_STICK;
			InjectWeaponTap(kWPN_FUNC, "switch_zoom_type");
		}
	}

	// --- K2 (D61): 1) Разрешение всех сочетаний (Chord) из активного профиля World_Gameplay ---
	const gpbind::BindingProfileModel& bindProf = gpbind::GetActiveBindingProfile();
	for (const auto& entry : bindProf.GetAllBindings())
	{
		if (entry.context != gpbind::ContextId::World_Gameplay || entry.trigger.type != gpbind::TriggerType::Chord)
			continue;
		const u32 modBtn = static_cast<u32>(entry.trigger.modifier);
		const u32 primBtn = static_cast<u32>(entry.trigger.button);
		if (modBtn >= VBTN_TOTAL_COUNT || primBtn >= VBTN_TOTAL_COUNT)
			continue;

		const bool modActive = (modBtn == aimBtnBit) ? bInAds : IsButtonHeld(modBtn);
		if (!modActive)
			continue;

		// Если модификатор активен (например, вход в ADS), а кнопка аккорда была нажата раньше — гасим отложенный tap/hold
		if (m_worldDualPending[primBtn])
		{
			m_worldDualPending[primBtn] = m_worldDualHoldCommitted[primBtn] = false;
			ConsumeButtonUntilRelease(primBtn);
		}

		if ((entry.action == gpbind::ActionId::FastReload || entry.action == gpbind::ActionId::ForceReload) &&
		    m_luaHints.bFddaInputDisabled)
			continue;

		if (IsButtonPressed(primBtn) && m_activeInjectedDik[primBtn] <= 0)
		{
			handled |= (1u << primBtn) | (1u << modBtn);
			m_worldDualPending[primBtn] = m_worldDualHoldCommitted[primBtn] = false;
			if (modBtn != aimBtnBit)
			{
				m_worldDualPending[modBtn] = false;
				m_worldDualHoldCommitted[modBtn] = true;
			}
			ConsumeButtonUntilRelease(primBtn);

			if (entry.action == gpbind::ActionId::FastReload || entry.action == gpbind::ActionId::ForceReload)
			{
				const bool bForce = (entry.action == gpbind::ActionId::ForceReload);
				const bool bLtOwned = m_bLtAdsActive;
				m_xLastReloadStartTime = 0;
				if (bLtOwned)
				{
					exitLtAds("ads_exit_for_fast_reload");
					if (aimBtnBit < VBTN_TOTAL_COUNT)
						ConsumeButtonUntilRelease(aimBtnBit);
				}
				RunXAdapter(bForce ? "gp_weapon.x_force_reload" : "gp_weapon.x_fast_reload", "chord_lt_x",
				            bLtOwned ? "lt_ads_exited" : "ads_not_owned_by_lt");
			}
			else
			{
				ExecuteWorldGameplayAction(static_cast<int>(entry.action),
				                           primBtn,
				                           static_cast<int>(gpbind::TriggerType::Chord),
				                           nowContinual);
			}
		}
	}

	// FIX-LIVE3 п.18 (Миша 2026-11-16): LB (кнопка колеса) + D-pad до открытия колеса = закреплённый слот крестовины
	// (gp_qaw_patch.use_pin). Сочетания профиля с тем же модификатором и кнопкой (цикл выше) имеют приоритет.
	{
		const gpbind::BindingTrigger* wwt = FindWorldBinding(gpbind::ActionId::WeaponWheel);
		const u32 pinMod = (wwt && wwt->button != gpbind::ButtonId::None) ? static_cast<u32>(wwt->button)
		                                                                   : static_cast<u32>(CInput::GP_BTN_LEFT_SHOULDER);
		if (pinMod < VBTN_TOTAL_COUNT && IsButtonHeld(pinMod))
		{
			static const struct { u32 btn; LPCSTR fn; LPCSTR why; } kWorldPins[] = {
				{ CInput::GP_BTN_DPAD_UP, "gp_wheel.on_pin_use_up", "world_lb_pin_up" },
				{ CInput::GP_BTN_DPAD_DOWN, "gp_wheel.on_pin_use_down", "world_lb_pin_down" },
				{ CInput::GP_BTN_DPAD_LEFT, "gp_wheel.on_pin_use_left", "world_lb_pin_left" },
				{ CInput::GP_BTN_DPAD_RIGHT, "gp_wheel.on_pin_use_right", "world_lb_pin_right" },
			};
			for (const auto& pn : kWorldPins)
			{
				if (pn.btn == pinMod)
					continue;
				if (m_worldDualPending[pn.btn])
				{
					m_worldDualPending[pn.btn] = m_worldDualHoldCommitted[pn.btn] = false;
					ConsumeButtonUntilRelease(pn.btn);
				}
				if ((handled & (1u << pn.btn)) || !IsButtonPressed(pn.btn) || m_activeInjectedDik[pn.btn] > 0)
					continue;
				handled |= (1u << pn.btn) | (1u << pinMod);
				m_worldDualPending[pn.btn] = m_worldDualHoldCommitted[pn.btn] = false;
				m_worldDualPending[pinMod] = false;
				m_worldDualHoldCommitted[pinMod] = true;
				ConsumeButtonUntilRelease(pn.btn);
				RunFieldAdapter(pn.fn, "chord", pn.why);
			}
		}
	}

	// GAYS v0.2: LB + View for timed tutorial prompts -- tap opens, hold >= 500 ms hides (gp_tutorial_prompt.script)
	if (m_tutorialChordBtn < VBTN_TOTAL_COUNT)
	{
		const u32 tb = m_tutorialChordBtn;
		handled |= (1u << tb);
		const bool down = (m_rawDownMask & (1u << tb)) != 0;
		if (down && !m_tutorialChordHoldSent && (nowContinual - m_tutorialChordStart) >= GAMEPAD_TUTORIAL_HOLD_MS)
		{
			m_tutorialChordHoldSent = true;
			RunFieldAdapter("gp_tutorial_prompt.on_gamepad_hold", "hold", "lb_view_hold");
		}
		if (!down)
		{
			if (!m_tutorialChordHoldSent)
				RunFieldAdapter("gp_tutorial_prompt.on_gamepad_tap", "tap", "lb_view_tap");
			m_tutorialChordBtn = VBTN_TOTAL_COUNT;
			m_tutorialChordHoldSent = false;
		}
	}

	// INV-REWORK-21: удержание X на цели Dot Marks держит kUSE до отпускания X (родное «(Удерживайте) ...» как с F)
	if (m_xUseHoldDik > 0 && m_xUseHoldBtn < VBTN_TOTAL_COUNT)
	{
		handled |= (1u << m_xUseHoldBtn);
		if (!IsButtonHeld(m_xUseHoldBtn) && !IsButtonPressed(m_xUseHoldBtn))
			EndUseHoldFromX("x_released");
	}

	// --- K2 (D61): 2) Единое разрешение нажатий (Button) и удержаний (Hold) по профилю World_Gameplay ---
	// Если на одной кнопке назначены и нажатие (Button), и удержание (Hold) — например, Y = NextWeapon, удерж. Y = WeaponWheel,
	// то нажатие срабатывает по отпусканию до порога hold_ms, а удержание — по достижению порога hold_ms (п. 3 K2).
	for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
	{
		if (btn == aimBtnBit || (handled & (1u << btn)) != 0)
			continue;
		if (btn == m_freelookBtnBit && (m_bR3DualModePending || m_bR3FreelookActive || handledR3))
			continue;

		gpbind::ActionId buttonAct = gpbind::ActionId::Invalid;
		gpbind::ActionId holdAct = gpbind::ActionId::Invalid;
		u32 holdMs = gpbind::limits::kDefaultHoldMs;
		QueryWorldButtonAndHoldActions(btn, buttonAct, holdAct, holdMs);

		if (buttonAct == gpbind::ActionId::ContextUseReload && m_luaHints.bFddaInputDisabled)
		{
			m_worldDualPending[btn] = m_worldDualHoldCommitted[btn] = false;
			continue; // оставляем общему циклу для отмены анимации FDDA через kWPN_RELOAD
		}

		const u32 bit = (1u << btn);

		if (holdAct != gpbind::ActionId::Invalid)
		{
			// На кнопке есть Hold (и, возможно, Button): двухрежимная обработка (Tap по отпусканию < holdMs, Hold >= holdMs)
			if (IsButtonPressed(btn) && m_activeInjectedDik[btn] <= 0)
			{
				handled |= bit;
				if (buttonAct == gpbind::ActionId::ContextUseReload &&
				    m_xLastReloadStartTime != 0 &&
				    (nowContinual - m_xLastReloadStartTime) < GAMEPAD_X_REPEAT_MS)
				{
					m_worldDualPending[btn] = m_worldDualHoldCommitted[btn] = false;
					ConsumeButtonUntilRelease(btn);
					m_xLastReloadStartTime = 0;
					RunXAdapter("gp_weapon.x_fast_upgrade", "repeat_x", "second_press_lt_250ms_after_reload_start");
				}
				else
				{
					m_worldDualPending[btn] = true;
					m_worldDualHoldCommitted[btn] = false;
					m_worldDualPressTime[btn] = nowContinual;
				}
			}
			else if (m_worldDualPending[btn] && IsButtonHeld(btn))
			{
				handled |= bit;
				if (!m_worldDualHoldCommitted[btn] && (nowContinual - m_worldDualPressTime[btn]) >= holdMs)
				{
					m_worldDualHoldCommitted[btn] = true;
					ExecuteWorldGameplayAction(static_cast<int>(holdAct),
					                           btn,
					                           static_cast<int>(gpbind::TriggerType::Hold),
					                           nowContinual);
				}
			}

			if (m_worldDualPending[btn] && IsButtonReleased(btn))
			{
				handled |= bit;
				if (!m_worldDualHoldCommitted[btn] && (nowContinual - m_worldDualPressTime[btn]) >= holdMs)
				{
					m_worldDualHoldCommitted[btn] = true;
					ExecuteWorldGameplayAction(static_cast<int>(holdAct),
					                           btn,
					                           static_cast<int>(gpbind::TriggerType::Hold),
					                           nowContinual);
				}
				else if (!m_worldDualHoldCommitted[btn] && buttonAct != gpbind::ActionId::Invalid)
				{
					ExecuteWorldGameplayAction(static_cast<int>(buttonAct),
					                           btn,
					                           static_cast<int>(gpbind::TriggerType::Button),
					                           nowContinual);
				}
				m_worldDualPending[btn] = false;
				m_worldDualHoldCommitted[btn] = false;
			}
			else if (m_worldDualPending[btn] && !IsButtonHeld(btn) && !IsButtonPressed(btn))
			{
				// Если кнопка была поглощена барьером до физического отпускания, сбрасываем pending
				m_worldDualPending[btn] = false;
				m_worldDualHoldCommitted[btn] = false;
			}
		}
		else if (buttonAct != gpbind::ActionId::Invalid)
		{
			// На кнопке только одиночное нажатие (Button) без Hold
			m_worldDualPending[btn] = false;
			m_worldDualHoldCommitted[btn] = false;

			// FIX-LIVE2 п.33 (Миша 2026-11-16, 33б): пока идёт анимация закрытия рюкзака (FDDA backpack_close),
			// мировой View не открывает рюкзак сразу на нажатии. gp_inv_rework.view_world_press() == true ->
			// жест View: отпускание до 700 мс (как InvAHoldMs) = обычный kINVENTORY (рюкзак открывается сразу,
			// анимация догоняет), удержание 700 мс = gp_inv_rework.view_world_hold() (КПК, когда руки освободятся).
			if (buttonAct == gpbind::ActionId::PdaFullToggle)
			{
				static int s_viewDeferBtn = -1;
				static u32 s_viewDeferStart = 0;
				static bool s_viewDeferFired = false;
				if (IsButtonPressed(btn) && m_activeInjectedDik[btn] <= 0)
				{
					bool defer = false;
					::luabind::functor<bool> fnDefer;
					if (ai().script_engine().functor("gp_inv_rework.view_world_press", fnDefer))
					{
						try { defer = fnDefer(); } catch (...) { defer = false; }
					}
					if (defer)
					{
						s_viewDeferBtn = static_cast<int>(btn);
						s_viewDeferStart = nowContinual;
						s_viewDeferFired = false;
					}
					else
					{
						s_gpViewLateArmed = true; // FIX-LIVE3 п.19: рюкзак откроется сразу, удержание -> КПК
						s_gpViewLateStart = nowContinual;
					}
				}
				if (s_viewDeferBtn == static_cast<int>(btn))
				{
					handled |= bit;
					if (IsButtonHeld(btn) || IsButtonPressed(btn))
					{
						if (!s_viewDeferFired && nowContinual - s_viewDeferStart >= GP_VIEW_PDA_HOLD_MS)
						{
							s_viewDeferFired = true;
							::luabind::functor<bool> fnHold;
							if (ai().script_engine().functor("gp_inv_rework.view_world_hold", fnHold))
							{
								try { fnHold(); } catch (...) {}
							}
						}
					}
					else
					{
						if (!s_viewDeferFired)
						{
							ExecuteWorldGameplayAction(static_cast<int>(buttonAct),
							                           btn,
							                           static_cast<int>(gpbind::TriggerType::Button),
							                           nowContinual);
						}
						s_viewDeferBtn = -1;
						s_viewDeferFired = false;
					}
					continue;
				}
			}

			// Чистые движковые зажатия/нажатия (Jump, PauseMenu, PdaFullToggle, Fire) без Hold на этой кнопке
			// обрабатываются общим циклом DispatchDiscreteActions -> ResolveContextualButtonInjection (с press/hold/release).
			// NextWeapon обрабатывается через ExecuteWorldGameplayAction (цикл по слотам оружия 1..4).
			const bool bLeaveToGenericInjection =
			    (buttonAct == gpbind::ActionId::Jump ||
			     buttonAct == gpbind::ActionId::PauseMenu ||
			     buttonAct == gpbind::ActionId::PdaFullToggle ||
			     buttonAct == gpbind::ActionId::Fire);

			if (!bLeaveToGenericInjection && IsButtonPressed(btn) && m_activeInjectedDik[btn] <= 0)
			{
				handled |= bit;
				if (buttonAct == gpbind::ActionId::ContextUseReload &&
				    m_xLastReloadStartTime != 0 &&
				    (nowContinual - m_xLastReloadStartTime) < GAMEPAD_X_REPEAT_MS)
				{
					ConsumeButtonUntilRelease(btn);
					m_xLastReloadStartTime = 0;
					RunXAdapter("gp_weapon.x_fast_upgrade", "repeat_x", "second_press_lt_250ms_after_reload_start");
				}
				else
				{
					ExecuteWorldGameplayAction(static_cast<int>(buttonAct),
					                           btn,
					                           static_cast<int>(gpbind::TriggerType::Button),
					                           nowContinual);
				}
			}
		}
	}

	if (m_xLastReloadStartTime != 0 && (nowContinual - m_xLastReloadStartTime) >= GAMEPAD_X_REPEAT_MS)
		m_xLastReloadStartTime = 0;

	return handled;
}

void CGamepadContextDispatcher::LogWeaponState() const
{
	const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
	float ltVal = 0.f, rtVal = 0.f;
	if (pInput)
	{
		const auto& gp = pInput->GetGamepadState();
		ltVal = gpmath::NormalizeTriggerAxis(gp.axes[4]);
		rtVal = gpmath::NormalizeTriggerAxis(gp.axes[5]);
	}
	Msg("* [Gamepad] gp_weapon_state ctx=%s triggers: LT=%.3f down=%d (press=%.3f release=%.3f) RT=%.3f down=%d (press=%.3f release=%.3f)",
	    GetContextName(m_currentContext), ltVal, m_bLtDigitalDown ? 1 : 0, prof.left_trigger.press, prof.left_trigger.release,
	    rtVal, m_bRtDigitalDown ? 1 : 0, prof.right_trigger.press, prof.right_trigger.release);

	CActor* pActor = g_actor;
	Msg("* [Gamepad] gp_weapon_state pad: ads_held=%d actor_zoom_mode=%d lean_side=%d dpadL(pending=%d hold=%d) dpadR(pending=%d hold=%d) last=%s@%u count=%u",
	    m_bLtAdsActive ? 1 : 0, (pActor && pActor->IsZoomAimingMode()) ? 1 : 0, m_adsLeanSide,
	    m_bDpadLeftPending ? 1 : 0, m_bDpadLeftHoldCommitted ? 1 : 0, m_bDpadRightPending ? 1 : 0, m_bDpadRightHoldCommitted ? 1 : 0,
	    m_lastWeaponAction, m_lastWeaponActionTime, m_weaponActionCount);

	Msg("* [Gamepad] gp_weapon_state x (E2.2): pending=%d hold_committed=%d press_t=%u repeat_window_from=%u hold_ms=%u repeat_ms=%u count=%u last=%s@%u",
	    m_bXPending ? 1 : 0, m_bXHoldCommitted ? 1 : 0, m_xPressTime, m_xLastReloadStartTime, GAMEPAD_X_HOLD_MS,
	    GAMEPAD_X_REPEAT_MS, m_xActionCount, m_xLastChoice.size() ? m_xLastChoice.c_str() : "none", m_xLastChoiceTime);

	Msg("* [Gamepad] gp_weapon_state field (E3.1): dpadU(pending=%d hold=%d) dpadD(pending=%d hold=%d) lb(pending=%d hold=%d) dpad_hold_ms=%u lb_hold_ms=%u count=%u last=%s@%u",
	    m_bDpadUpPending ? 1 : 0, m_bDpadUpHoldCommitted ? 1 : 0,
	    m_bDpadDownPending ? 1 : 0, m_bDpadDownHoldCommitted ? 1 : 0,
	    m_bLbPending ? 1 : 0, m_bLbHoldCommitted ? 1 : 0,
	    GAMEPAD_DPAD_HOLD_MS, GAMEPAD_LB_HOLD_MS, m_fieldActionCount,
	    m_fieldLastChoice.size() ? m_fieldLastChoice.c_str() : "none", m_fieldLastChoiceTime);

	Msg("* [Gamepad] gp_weapon_state flags: AF_AIM_TOGGLE=%d AF_LOOKOUT_TOGGLE=%d AF_MULTI_ITEM_PICKUP=%d useSeparateUBGLKeybind=%d sens_multiple=%.3f wishful_lookout=%s",
	    psActorFlags.test(AF_AIM_TOGGLE) ? 1 : 0, psActorFlags.test(AF_LOOKOUT_TOGGLE) ? 1 : 0,
	    psActorFlags.test(AF_MULTI_ITEM_PICKUP) ? 1 : 0, useSeparateUBGLKeybind ? 1 : 0,
	    sens_multiple,
	    !pActor ? "n/a" : ((pActor->get_state_wishful() & mcLLookout) ? "left" : ((pActor->get_state_wishful() & mcRLookout) ? "right" : "none")));

	const int acts[] = { kWPN_FIRE, kWPN_ZOOM, kWPN_ZOOM_INC, kWPN_ZOOM_DEC, kWPN_FUNC, kCUSTOM21, kWPN_FIREMODE_NEXT, kWPN_NEXT, kDETECTOR, kL_LOOKOUT, kR_LOOKOUT, kTORCH, kCAM_ZOOM_IN, kCUSTOM19, kCUSTOM20, kCUSTOM24, kWPN_4, kWPN_6 };
	string512 binds = "";
	for (int act : acts)
	{
		bool synth = false;
		const int dik = ResolveActionDik(act, &synth);
		string64 one;
		LPCSTR actName = id_to_action_name(static_cast<EGameActions>(act));
		xr_sprintf(one, sizeof(one), "%s=%d%s ", actName ? actName : "?", dik, synth ? "(synth)" : "");
		xr_strcat(binds, one);
	}
	Msg("* [Gamepad] gp_weapon_state binds: %s", binds);

	CWeapon* pWpn = GetGamepadActorWeapon();
	if (!pWpn)
	{
		Msg("* [Gamepad] gp_weapon_state weapon: none (active slot has no CWeapon)");
		return;
	}
	CWeaponMagazined* pMag = smart_cast<CWeaponMagazined*>(pWpn);
	CWeaponMagazinedWGrenade* pGl = smart_cast<CWeaponMagazinedWGrenade*>(pWpn);
	Msg("* [Gamepad] gp_weapon_state weapon: sect=%s state=%u zoom_enabled=%d zoomed=%d zoom_type=%u zoom_factor=%.3f ammo_type=%u ammo=%d misfire=%d fire_mode=%d gl_attached=%d gl_mode=%d scope=%d",
	    pWpn->cNameSect().c_str(), pWpn->GetState(), pWpn->IsZoomEnabled() ? 1 : 0, pWpn->IsZoomed() ? 1 : 0,
	    (u32)pWpn->GetZoomType(), pWpn->GetZoomFactor(), (u32)pWpn->GetAmmoType(), pWpn->GetAmmoElapsed(),
	    pWpn->IsMisfire() ? 1 : 0, pMag ? pMag->GetCurrentFireMode() : 0,
	    pWpn->IsGrenadeLauncherAttached() ? 1 : 0, (pGl && pGl->GetGrenadeLauncherMode()) ? 1 : 0, pWpn->IsScopeAttached() ? 1 : 0);
}

void CGamepadContextDispatcher::DispatchDiscreteActions()
{
	OnFrame();

	if (Device.dwFrame == m_lastDispatchFrame)
		return;
	m_lastDispatchFrame = Device.dwFrame;
	TickMenuLayoutScan();

	// 1. Обрабатываем кнопки, отпущенные при смене контекста (WasButtonReleasedOnContextExit) или физически (IsButtonReleased)
	// ДО вызова StepWorldWeaponButtons, чтобы m_activeInjectedDik[btn] никогда не блокировал новые нажатия
	for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
	{
		const int activeDik = m_activeInjectedDik[btn];
		const int activeAct = m_activeInjectedAction[btn];
		const EGamepadContext activeCtx = m_activeInjectedCtx[btn];
		if (activeDik <= 0)
			continue;

		if (WasButtonReleasedOnContextExit(btn))
		{
			// Если кнопка удерживалась в мировом контексте (например, стрельба RT, прицел LT, наклон LB/RB),
			// а поверх мира открылось модальное окно — снимаем удержание у сущности мира без посылки Release в новое окно.
			if (IsWorldGameplayLikeContext(activeCtx) && activeAct >= 0 && activeAct < kLASTACTION)
			{
				const bool bModalOpenedNow = (CurrentGameUI() != nullptr && CurrentGameUI()->TopInputReceiver() != nullptr) ||
				                             (MainMenu() != nullptr && MainMenu()->IsActive()) ||
				                             (Console != nullptr && Console->bVisible);
				if (bModalOpenedNow)
				{
					if (pInput)
					{
						pInput->SetGamepadInjectedKeyState(activeDik, false);
						if (activeAct == kJUMP && activeDik != DIK_SPACE)
							pInput->SetGamepadInjectedKeyState(DIK_SPACE, false);
					}
					if (g_pGameLevel != nullptr && Level().CurrentEntity() != nullptr)
					{
						if (IInputReceiver* IR = smart_cast<IInputReceiver*>(smart_cast<CGameObject*>(Level().CurrentEntity())))
						{
							IR->IR_OnKeyboardRelease(static_cast<EGameActions>(activeAct));
							++m_injectionStats.totalReleaseInjected;
						}
					}
				}
				else
				{
					InjectKeyboardEvent(activeDik, activeAct, "release", activeCtx, false);
				}
			}
			else if (pInput)
			{
				pInput->SetGamepadInjectedKeyState(activeDik, false);
			}
			m_activeInjectedDik[btn] = 0;
			m_activeInjectedAction[btn] = kNOTBINDED;
			continue;
		}

		if (IsButtonReleased(btn) || (!IsButtonHeld(btn) && !IsButtonPressed(btn)) || !AllowsGamepadButtons())
		{
			InjectKeyboardEvent(activeDik, activeAct, "release", activeCtx, false);
			if (btn == VBTN_RT && activeAct == kWPN_FIRE)
			{
				m_lastWeaponAction = "rt_fire_release";
				m_lastWeaponActionTime = Device.dwTimeContinual;
				++m_weaponActionCount;
				if (ps_gp_telemetry && pInput)
				{
					const CWeapon* pWpn = GetGamepadActorWeapon();
					string256 line;
					xr_sprintf(line, sizeof(line), "gp_weapon,%u,action=rt_fire_release,act=%d,dik=%d,ok=1,ads=%d,lean=%d,wpn=%s",
					           Device.dwTimeContinual, activeAct, activeDik, m_bLtAdsActive ? 1 : 0,
					           m_adsLeanSide, pWpn ? pWpn->cNameSect().c_str() : "none");
					pInput->WriteGamepadTelemetryLine(line);
				}
			}
			m_activeInjectedDik[btn] = 0;
			m_activeInjectedAction[btn] = kNOTBINDED;
		}
	}

	ValidateGamepadMouseModeAutoReset();

	bool handledL3 = false;
	bool handledB = false;
	bool handledR3 = false;
	const u32 menuNow = Device.dwTimeContinual;
	StepWorldMovementButtons(menuNow, handledL3, handledB, handledR3);
	if (HandleMenuCaptureInput(menuNow))
		return;
	if (StepGamepadMouseModeToggle(menuNow))
		return;
	StepVirtualCursor(menuNow);

	u32 handledWeaponMask = 0;
	if (m_bGamepadMouseMode)
	{
		// CURSOR1: в режиме «мышь» B выходит из режима «мышь» без закрытия самого окна (не шлёт Escape),
		// а Y открывает экранную клавиатуру, если в чужом окне есть поле ввода CUIEditBox.
		handledWeaponMask = StepWorldWeaponButtons(Device.dwTimeContinual, true);
		if (!AllowsGamepadButtons())
			return;
		if (IsButtonPressed(CInput::GP_BTN_EAST))
		{
			SetGamepadMouseMode(false, "button_b");
			ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
			return;
		}
		if (!CGamepadOnScreenKeyboardUI::Instance().IsOpen() && IsButtonPressed(CInput::GP_BTN_NORTH))
		{
			if (CGamepadOnScreenKeyboardUI::Instance().TryOpenForActiveDialog("gamepad_btn_y"))
			{
				SetGamepadMouseMode(false, "osk_opened");
				ConsumeButtonUntilRelease(CInput::GP_BTN_NORTH);
				return;
			}
		}
	}
	else
	{
		if (NewGameDialog() && AllowsGamepadButtons() && pInput && pInput->IsGamepadActiveSource() &&
		    !CGamepadOnScreenKeyboardUI::Instance().IsOpen())
		{
			StepWorkshopButtons(menuNow);
			return;
		}
		if (AllowsGamepadButtons() && pInput && pInput->IsGamepadActiveSource() && CurrentGameUI() &&
		    IsLightingDialog(CurrentGameUI()->TopInputReceiver()))
		{
			if (IsButtonPressed(CInput::GP_BTN_EAST) && g_ai_space)
			{
				::luabind::functor<void> close;
				if (ai().script_engine().functor("ui_ctrl_lighting.gamepad_close", close))
				{
					try { close(); } catch (...) {}
				}
			}
			else if (IsButtonPressed(CInput::GP_BTN_WEST) && g_ai_space)
			{
				::luabind::functor<void> reset;
				if (ai().script_engine().functor("ui_ctrl_lighting.gamepad_reset_current", reset))
				{
					try { reset(); } catch (...) {}
				}
			}
			for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
				if (IsButtonPressed(btn) && btn != CInput::GP_BTN_DPAD_UP && btn != CInput::GP_BTN_DPAD_DOWN &&
				    btn != CInput::GP_BTN_DPAD_LEFT && btn != CInput::GP_BTN_DPAD_RIGHT)
					ConsumeButtonUntilRelease(btn);
			return;
		}
		if (AllowsGamepadButtons() && CurrentGameUI() && pInput && pInput->IsGamepadActiveSource())
		{
			auto* root = CurrentGameUI()->TopInputReceiver();
			if (root && root->WindowName() == "GABlackjackMessage")
			{
				const bool a = IsButtonPressed(CInput::GP_BTN_SOUTH);
				const bool b = IsButtonPressed(CInput::GP_BTN_EAST);
				ConsumeAllCurrentlyHeldButtons();
				if (auto* message = smart_cast<CUIMessageBoxEx*>(root))
				{
					auto* box = message->m_pMessageBox;
					if ((a || b) && box && box->HasGamepadOk()) box->OnYesOk();
					else if ((a || b) && box) box->ActivateGamepadConfirmation(a && !b);
				}
				return;
			}
		}
		if (HandleConfirmationShortcuts() || HandleMenuPageInput(menuNow) || HandleMenuShortcuts(menuNow))
			return;
		handledWeaponMask = StepWorldWeaponButtons(Device.dwTimeContinual, handledR3) |
		                    StepRadialWheelButtons(Device.dwTimeContinual) |
		                    StepInventoryButtons(Device.dwTimeContinual) |
		                    StepPdaButtons(Device.dwTimeContinual) |
		                    StepItemSheetButtons(Device.dwTimeContinual) |
		                    StepWorkshopButtons(Device.dwTimeContinual);
	}

	if (!AllowsGamepadButtons())
		return;

	// F5.1 (D51) + DEF-P1-03 (G5.1 PAW метки поверх КПК): в диалогах с полем ввода (UISaveDialog, UICreateStash,
	// UIWPSettings/UIPinSettingsWnd поверх CUIPdaWnd, UINewGame и др.) нажатие Y (GP_BTN_NORTH) открывает
	// экранную клавиатуру в 1 клик даже без наведения курсора на поле.
	const bool bModalDialogOverPda = CurrentGameUI() != nullptr &&
	                                 CurrentGameUI()->GetPdaMenu().IsShown() &&
	                                 CurrentGameUI()->TopInputReceiver() != nullptr &&
	                                 CurrentGameUI()->TopInputReceiver() != &CurrentGameUI()->GetPdaMenu();
	if (!m_bGamepadMouseMode &&
	    !CGamepadOnScreenKeyboardUI::Instance().IsOpen() &&
	    (m_currentContext == EGamepadContext::MainMenu ||
	     m_currentContext == EGamepadContext::MenuUI_Modal ||
	     m_currentContext == EGamepadContext::TextEntry) &&
	    !IsInventoryModalActive() &&
	    (!IsPdaInteractiveActive() || bModalDialogOverPda) &&
	    IsButtonPressed(CInput::GP_BTN_NORTH))
	{
		if (CGamepadOnScreenKeyboardUI::Instance().TryOpenForActiveDialog("gamepad_btn_y"))
		{
			ConsumeButtonUntilRelease(CInput::GP_BTN_NORTH);
			return;
		}
	}

	// 2. Обрабатываем новые нажатия (IsButtonPressed) и удержания (IsButtonHeld) в текущем контексте
	for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
	{
		if ((btn == CInput::GP_BTN_LEFT_STICK && handledL3) ||
		    (btn == CInput::GP_BTN_EAST && handledB) ||
		    (btn == CInput::GP_BTN_RIGHT_STICK && handledR3) ||
		    (handledWeaponMask & (1u << btn)))
		{
			continue;
		}

		if (IsButtonPressed(btn))
		{
			int actionId = kNOTBINDED;
			int dik = 0;
			LPCSTR reason = "none";
			if (!ResolveContextualButtonInjection(btn, m_currentContext, m_subFlags, actionId, dik, reason))
			{
				++m_injectionStats.blockedByContextCount;
				if (reason && 0 == xr_strcmp(reason, "blocked_placement_guard"))
				{
					++m_injectionStats.blockedByPlacementGuardCount;
				}
				continue;
			}

			CUIDialogWnd* const tirBefore = (CurrentGameUI() != nullptr) ? CurrentGameUI()->TopInputReceiver() : nullptr;
			const bool mmBefore = (MainMenu() != nullptr && MainMenu()->IsActive());
			const bool consoleBefore = (Console != nullptr && Console->bVisible);

			CUIDialogWnd* const menuBefore = btn == CInput::GP_BTN_SOUTH && IsMenuFocusMode() &&
				mmBefore && m_bVcTargetHit ? MainMenu()->TopInputReceiver() : nullptr;
			if (InjectKeyboardEvent(dik, actionId, "press", m_currentContext, false))
			{
				if (menuBefore && menuBefore == m_menuFocusRoot && MainMenu() && MainMenu()->IsActive() &&
				    MainMenu()->TopInputReceiver() == menuBefore && menuBefore->IR_process())
					StepMenuFocus(menuBefore, ENavDir::None, 0, 0, Device.dwTimeContinual, EMenuFocusAction::Enter);

				if (btn == VBTN_RT && actionId == kWPN_FIRE)
				{
					m_lastWeaponAction = "rt_fire_press";
					m_lastWeaponActionTime = Device.dwTimeContinual;
					++m_weaponActionCount;
					if (ps_gp_telemetry && pInput)
					{
						const CWeapon* pWpn = GetGamepadActorWeapon();
						string256 line;
						xr_sprintf(line, sizeof(line), "gp_weapon,%u,action=rt_fire_press,act=%d,dik=%d,ok=1,ads=%d,lean=%d,wpn=%s",
						           Device.dwTimeContinual, actionId, dik, m_bLtAdsActive ? 1 : 0,
						           m_adsLeanSide, pWpn ? pWpn->cNameSect().c_str() : "none");
						pInput->WriteGamepadTelemetryLine(line);
					}
				}

				// Синхронизируем Lua-подсказки сразу после Press (на случай если Press открыл/закрыл окно или сменил стейт мебели)
				PollLuaBridgeHints();

				CUIDialogWnd* const tirAfter = (CurrentGameUI() != nullptr) ? CurrentGameUI()->TopInputReceiver() : nullptr;
				const bool mmAfter = (MainMenu() != nullptr && MainMenu()->IsActive());
				const bool consoleAfter = (Console != nullptr && Console->bVisible);
				SContextSubFlags afterSub = {};
				LPCSTR afterReason = "";
				const EGamepadContext ctxAfter = EvaluateContextWaterfall(afterSub, afterReason);

				// Если само нажатие закрыло или открыло окно / сменило контекст внутри этого же кадра (например, B/A закрыл инвентарь или сон),
				// немедленно взводим барьер consumed_until_release для этой кнопки и не оставляем активный DIK для Release в новом контексте!
				if (tirAfter != tirBefore || mmAfter != mmBefore || consoleAfter != consoleBefore || ctxAfter != m_currentContext)
				{
					ConsumeButtonUntilRelease(btn);
					m_activeInjectedDik[btn] = 0;
					m_activeInjectedAction[btn] = kNOTBINDED;
					if (m_bGamepadMouseMode)
						ValidateGamepadMouseModeAutoReset();
				}
				else
				{
					m_activeInjectedDik[btn] = dik;
					m_activeInjectedAction[btn] = actionId;
					m_activeInjectedCtx[btn] = m_currentContext;
				}
			}
		}
		else if (IsButtonHeld(btn))
		{
			const int activeDik = m_activeInjectedDik[btn];
			const int activeAct = m_activeInjectedAction[btn];
			if (activeDik > 0)
			{
				InjectKeyboardEvent(activeDik, activeAct, "hold", m_currentContext, false);
			}
		}
	}
}

void CGamepadContextDispatcher::LogState() const
{
	Msg("* [Gamepad] gp_ctx: ctx=%s (prio=%u) prev=%s reason=\"%s\" transitions=%u allow_move=%d allow_look=%d allow_btns=%d",
	    GetContextName(m_currentContext),
	    GetContextPriorityNumber(m_currentContext),
	    GetContextName(m_prevContext),
	    m_lastTransitionReason ? m_lastTransitionReason : "",
	    m_transitionCount,
	    AllowsWorldMove() ? 1 : 0,
	    AllowsWorldLook() ? 1 : 0,
	    AllowsGamepadButtons() ? 1 : 0);

	Msg("* [Gamepad] gp_ctx buttons: raw=0x%05X barrier_consumed=0x%05X effective=0x%05X pressed=0x%05X held=0x%05X released=0x%05X exit_released=0x%05X",
	    m_rawDownMask,
	    m_consumedUntilReleaseMask,
	    m_effectiveDownMask,
	    m_pressedMask,
	    m_heldMask,
	    m_releasedMask,
	    m_releasedOnContextExitMask);

	Msg("* [Gamepad] gp_ctx nav: dir=%s src=%s trig=%d repeat=%d stick_barrier=%d | subflags: mm_sub=%d partial_move=%d full_cutscene=%d pda_ui=%d modal_move=%d lua_hint=%d",
	    GetNavDirName(m_activeNavDir),
	    m_activeNavSource ? m_activeNavSource : "none",
	    m_bNavTriggeredThisFrame ? 1 : 0,
	    m_bNavTriggeredIsRepeat ? 1 : 0,
	    m_bStickNavConsumedUntilNeutral ? 1 : 0,
	    m_subFlags.bMainMenuHasModalSubDialog ? 1 : 0,
	    m_subFlags.bPartialMoveOnlyBlock ? 1 : 0,
	    m_subFlags.bCutsceneFullBlock ? 1 : 0,
	    m_subFlags.b3dPdaInteractive ? 1 : 0,
	    m_subFlags.bModalAllowsMovement ? 1 : 0,
	    m_subFlags.bDetectedViaLuaHint ? 1 : 0);

	Msg("* [Gamepad] gp_ctx d3_2_lua_hints: syncs=%u mcm_bind=%d fdda=%d sleep_info=%d sleep_dlg=%d wheel=%s placement=%s (diks=%d/%d/%d/%d) modal_tag=\"%s\"",
	    m_luaHints.luaHintSyncCount,
	    m_luaHints.bMcmKeybindCapture ? 1 : 0,
	    m_luaHints.bFddaInputDisabled ? 1 : 0,
	    m_luaHints.bActorSleepingInfo ? 1 : 0,
	    m_luaHints.bSleepDialogShown ? 1 : 0,
	    GetRadialWheelTypeName(m_luaHints.radialWheelType),
	    GetPlacementStateName(m_luaHints.placementState),
	    m_luaHints.placementDikPlace,
	    m_luaHints.placementDikCollision,
	    m_luaHints.placementDikAlign,
	    m_luaHints.placementDikControls,
	    m_luaHints.modalMenuTag.size() ? m_luaHints.modalMenuTag.c_str() : "");

	Msg("* [Gamepad] gp_ctx d3_2_injection: press=%u hold=%u release=%u blocked_ctx=%u blocked_placement=%u last_phase=%s last_ctx=%s last_dik=%d last_action=%d",
	    m_injectionStats.totalPressInjected,
	    m_injectionStats.totalHoldInjected,
	    m_injectionStats.totalReleaseInjected,
	    m_injectionStats.blockedByContextCount,
	    m_injectionStats.blockedByPlacementGuardCount,
	    m_injectionStats.lastInjectedPhase ? m_injectionStats.lastInjectedPhase : "none",
	    GetContextName(m_injectionStats.lastInjectedContext),
	    m_injectionStats.lastInjectedDik,
	    m_injectionStats.lastInjectedAction);

	// K2 (D61): вывод активных привязок World_Gameplay из профиля (перечитываем файл, если он был изменён на диске)
	gpbind::ReloadActiveBindingProfileIfModifiedOnDisk();
	const gpbind::BindingProfileModel& bindProf = gpbind::GetActiveBindingProfile();
	std::string worldBindsSummary;
	for (const auto& entry : bindProf.GetBindingsForContext(gpbind::ContextId::World_Gameplay))
	{
		if (!worldBindsSummary.empty())
			worldBindsSummary += " ";
		worldBindsSummary += gpbind::BindingProfileModel::ActionToString(entry.action);
		worldBindsSummary += "=";
		worldBindsSummary += gpbind::BindingProfileModel::FormatTriggerSpec(entry.trigger);
	}
	Msg("* [Gamepad] gp_ctx world_bindings (D4.1/K2): %s", worldBindsSummary.c_str());
}

bool CGamepadContextDispatcher::RunSelfTest()
{
	// Сохраняем текущее рабочее состояние диспетчера, чтобы после теста восстановить его без побочных эффектов
	const auto savedState = *this;

	u32 passed = 0;
	u32 failed = 0;
	auto check = [&](bool cond, LPCSTR name) {
		if (cond)
		{
			++passed;
		}
		else
		{
			++failed;
			Msg("! [Gamepad] gp_ctx selftest FAIL: %s", name);
		}
	};

	// 1. Проверка порядка приоритетов всех 12 контекстов (R3 REC-1)
	check(GetContextPriorityNumber(EGamepadContext::Console) == 1, "prio_1_Console");
	check(GetContextPriorityNumber(EGamepadContext::TextEntry) == 2, "prio_2_TextEntry");
	check(GetContextPriorityNumber(EGamepadContext::MainMenu) == 3, "prio_3_MainMenu");
	check(GetContextPriorityNumber(EGamepadContext::Cutscene_InputBlock) == 4, "prio_4_Cutscene_InputBlock");
	check(GetContextPriorityNumber(EGamepadContext::Sleep) == 5, "prio_5_Sleep");
	check(GetContextPriorityNumber(EGamepadContext::Dialog_NPC) == 6, "prio_6_Dialog_NPC");
	check(GetContextPriorityNumber(EGamepadContext::RadialWheel) == 7, "prio_7_RadialWheel");
	check(GetContextPriorityNumber(EGamepadContext::Placement_HideoutFurniture) == 8, "prio_8_Placement_HideoutFurniture");
	check(GetContextPriorityNumber(EGamepadContext::World_3DPDA) == 9, "prio_9_World_3DPDA");
	check(GetContextPriorityNumber(EGamepadContext::MenuUI_Modal) == 10, "prio_10_MenuUI_Modal");
	check(GetContextPriorityNumber(EGamepadContext::World_VehicleOrHolder) == 11, "prio_11_World_VehicleOrHolder");
	check(GetContextPriorityNumber(EGamepadContext::World_Gameplay) == 12, "prio_12_World_Gameplay");

	// 2. Сброс в чистый тестовый автомат в контексте MenuUI_Modal
	*this = CGamepadContextDispatcher();
	m_bInSelfTest = true;
	SContextSubFlags sub = {};
	const u32 bitA = (1u << CInput::GP_BTN_SOUTH);
	const u32 bitB = (1u << CInput::GP_BTN_EAST);
	const u32 bitDpadDown = (1u << CInput::GP_BTN_DPAD_DOWN);

	// Кадр 1: инициализация в MenuUI_Modal без нажатых кнопок
	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_init_modal", 0, 0, 0, 1000, false);
	check(m_currentContext == EGamepadContext::MenuUI_Modal, "init_ctx_modal");
	check(m_consumedUntilReleaseMask == 0, "init_barrier_zero");

	// Кадр 2: зажимаем кнопку A (GP_BTN_SOUTH) в MenuUI_Modal -> Pressed=1, Held=1, Released=0
	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_press_A_in_modal", bitA, 0, 0, 1016, false);
	check(IsButtonPressed(CInput::GP_BTN_SOUTH), "btnA_pressed_frame1");
	check(IsButtonHeld(CInput::GP_BTN_SOUTH), "btnA_held_frame1");
	check(!IsButtonReleased(CInput::GP_BTN_SOUTH), "btnA_not_released_frame1");

	// Кадр 3: удерживаем кнопку A во 2-м кадре -> Pressed=0, Held=1, Released=0
	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_hold_A_in_modal", bitA, 0, 0, 1032, false);
	check(!IsButtonPressed(CInput::GP_BTN_SOUTH), "btnA_not_pressed_frame2");
	check(IsButtonHeld(CInput::GP_BTN_SOUTH), "btnA_held_frame2");

	// Кадр 4 (REC-2 Барьер отпускания): при зажатой кнопке A и отклонённом вниз левом стике закрывается меню -> переход в World_Gameplay!
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "test_close_modal_with_A_held", bitA, 0, 28000, 1048, false);
	check(DidContextChangeThisFrame(), "ctx_changed_modal_to_world");
	check(WasButtonReleasedOnContextExit(CInput::GP_BTN_SOUTH), "btnA_synthetic_release_on_exit");
	check(IsButtonConsumedUntilRelease(CInput::GP_BTN_SOUTH), "btnA_consumed_until_release_armed");
	check(!IsButtonPressed(CInput::GP_BTN_SOUTH), "btnA_suppressed_pressed_in_new_ctx");
	check(!IsButtonHeld(CInput::GP_BTN_SOUTH), "btnA_suppressed_held_in_new_ctx");
	check(!IsButtonReleased(CInput::GP_BTN_SOUTH), "btnA_suppressed_released_in_new_ctx");
	check(IsStickNavConsumedUntilNeutral(), "stick_nav_consumed_until_neutral_armed");

	// Кадр 5: продолжаем физически удерживать A в World_Gameplay, одновременно нажимаем B (которая НЕ была зажата при переходе)
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "test_hold_A_press_B", bitA | bitB, 0, 28000, 1064, false);
	check(IsButtonConsumedUntilRelease(CInput::GP_BTN_SOUTH), "btnA_still_in_barrier");
	check(!IsButtonHeld(CInput::GP_BTN_SOUTH), "btnA_still_not_held");
	check(IsButtonPressed(CInput::GP_BTN_EAST), "btnB_unmasked_pressed_ok");
	check(IsButtonHeld(CInput::GP_BTN_EAST), "btnB_unmasked_held_ok");

	// Кадр 6: физически отпускаем A и B, возвращаем стик в нейтраль -> A выходит из барьера БЕЗ ложного Released в новом контексте, B получает штатный Released
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "test_release_A_and_B", 0, 0, 0, 1080, false);
	check(!IsButtonConsumedUntilRelease(CInput::GP_BTN_SOUTH), "btnA_barrier_cleared_on_physical_release");
	check(!IsButtonPressed(CInput::GP_BTN_SOUTH), "btnA_no_press_on_barrier_clear");
	check(!IsButtonReleased(CInput::GP_BTN_SOUTH), "btnA_no_duplicate_release_in_new_ctx");
	check(IsButtonReleased(CInput::GP_BTN_EAST), "btnB_normal_release_ok");
	check(!IsStickNavConsumedUntilNeutral(), "stick_nav_barrier_cleared_on_neutral");

	// Кадр 7: повторное физическое нажатие A в World_Gameplay после отпускания -> теперь штатно срабатывает Pressed!
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "test_repress_A_after_release", bitA, 0, 0, 1096, false);
	check(IsButtonPressed(CInput::GP_BTN_SOUTH), "btnA_pressed_after_barrier_cleared");
	check(IsButtonHeld(CInput::GP_BTN_SOUTH), "btnA_held_after_barrier_cleared");

	// 3. Проверка автоповтора навигации на Device.dwTimeContinual (320 мс задержка, 90 мс шаг)
	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_repeat_enter_modal", 0, 0, 0, 2000, false);
	ENavDir navDir = ENavDir::None;
	bool isRepeat = false;
	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_press", bitDpadDown, 0, 0, 2010, false);
	check(IsNavDirTriggeredThisFrame(&navDir, &isRepeat) && navDir == ENavDir::Down && !isRepeat, "nav_initial_step_at_2010");

	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_before_320ms", bitDpadDown, 0, 0, 2329, false);
	check(!IsNavDirTriggeredThisFrame(), "nav_no_repeat_before_320ms");

	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_at_320ms", bitDpadDown, 0, 0, 2330, false);
	check(IsNavDirTriggeredThisFrame(&navDir, &isRepeat) && navDir == ENavDir::Down && isRepeat, "nav_first_repeat_at_320ms");

	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_before_90ms", bitDpadDown, 0, 0, 2419, false);
	check(!IsNavDirTriggeredThisFrame(), "nav_no_repeat_before_90ms");

	StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "test_nav_at_90ms", bitDpadDown, 0, 0, 2420, false);
	check(IsNavDirTriggeredThisFrame(&navDir, &isRepeat) && navDir == ENavDir::Down && isRepeat, "nav_second_repeat_at_90ms");

	// 4. Проверка C4.3-D6: контекст Console подавляет движение, камеру и кнопки
	StepStateMachine(EGamepadContext::Console, sub, "test_console_c4_3_d6", bitA | bitDpadDown, 20000, 20000, 2500, false);
	check(!AllowsWorldMove(), "c4_3_d6_console_blocks_world_move");
	check(!AllowsWorldLook(), "c4_3_d6_console_blocks_world_look");
	check(!AllowsGamepadButtons(), "c4_3_d6_console_blocks_buttons");
	check(!IsButtonPressed(CInput::GP_BTN_SOUTH), "c4_3_d6_console_blocks_btnA");
	check(!IsNavDirTriggeredThisFrame(), "c4_3_d6_console_blocks_nav");

	// 5. Проверки D3.2 (REC-3 / REC-4):
	// 5a. Пакетная установка Lua-подсказок SetLuaBridgeHintsBatch
	SetLuaBridgeHintsBatch(false, true, false, false, 5, 2, "UIInventory:inventory");
	check(m_luaHints.bFddaInputDisabled, "d3_2_batch_fdda_true");
	check(m_luaHints.radialWheelType == ERadialWheelType::Companion_Flat, "d3_2_batch_wheel_companion");
	check(m_luaHints.placementState == EPlacementState::Holding, "d3_2_batch_placement_holding");
	check(0 == xr_strcmp(m_luaHints.modalMenuTag.c_str(), "UIInventory:inventory"), "d3_2_batch_modal_tag");

	// 5b. В контексте Placement_HideoutFurniture (HOLDING) блокируются посторонние геймплейные бинды (защита от сброса placeable_furniture:398),
	// но разрешены клавиши установки (A -> kUSE, B -> kQUIT, X -> DIK_TAB, Y -> DIK_CAPITAL, View -> DIK_HOME)
	int actId = kNOTBINDED;
	int dik = 0;
	LPCSTR injReason = "";
	check(ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::Placement_HideoutFurniture, sub, actId, dik, injReason) && actId == kUSE && dik > 0,
	      "d3_2_placement_allows_A_place");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_EAST, EGamepadContext::Placement_HideoutFurniture, sub, actId, dik, injReason) && actId == kQUIT && dik > 0,
	      "d3_2_placement_allows_B_cancel");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_WEST, EGamepadContext::Placement_HideoutFurniture, sub, actId, dik, injReason) && dik == 15,
	      "d3_2_placement_allows_X_collision_tab");
	check(!ResolveContextualButtonInjection(CInput::GP_BTN_DPAD_UP, EGamepadContext::Placement_HideoutFurniture, sub, actId, dik, injReason) &&
	      0 == xr_strcmp(injReason, "blocked_placement_guard"),
	      "d3_2_placement_blocks_dpad_torch");
	check(!ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::Placement_HideoutFurniture, sub, actId, dik, injReason) &&
	      0 == xr_strcmp(injReason, "blocked_placement_guard"),
	      "d3_2_placement_blocks_rt_fire");

	// 5c. В контекстах MenuUI_Modal, RadialWheel, Dialog_NPC, Sleep кнопки A / B / RT НЕ маппятся в kJUMP / kCROUCH / kWPN_FIRE
	check(ResolveContextualButtonInjection(CInput::GP_BTN_EAST, EGamepadContext::RadialWheel, sub, actId, dik, injReason) && actId == kQUIT,
	      "d3_2_radial_B_is_kQUIT_not_crouch");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::RadialWheel, sub, actId, dik, injReason) && actId == kNOTBINDED && dik == MOUSE_1,
	      "d3_2_radial_A_is_MOUSE1_not_jump");
	check(ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::RadialWheel, sub, actId, dik, injReason) && actId == kNOTBINDED && dik == MOUSE_1,
	      "d3_2_radial_RT_is_MOUSE1_not_fire");

	// 5d. При активном FDDA (bFddaInputDisabled = true) кнопка X в частичном блоке маппится в kWPN_RELOAD (для отмены анимации через skip_bidings[34])
	SContextSubFlags fddaSub = {};
	fddaSub.bPartialMoveOnlyBlock = true;
	m_luaHints.bFddaInputDisabled = true;
	check(ResolveContextualButtonInjection(CInput::GP_BTN_WEST, EGamepadContext::Cutscene_InputBlock, fddaSub, actId, dik, injReason) && actId == kWPN_RELOAD && dik > 0,
	      "d3_2_fdda_X_maps_to_kWPN_RELOAD");

	// 5e. E3.1: вне ADS в World_Gameplay LB/RB не маппятся в наклоны kL_LOOKOUT/kR_LOOKOUT (LB -> kWPN_6, RB -> kCUSTOM19, D-Pad Down -> kCUSTOM20, R3 -> kCUSTOM24)
	const gpbind::BindingProfileModel savedBindingProfile = gpbind::GetActiveBindingProfile();
	gpbind::GetMutableActiveBindingProfile().ResetToDefaults();
	check(ResolveContextualButtonInjection(CInput::GP_BTN_LEFT_SHOULDER, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kWPN_6 && dik > 0,
	      "e3_1_world_LB_is_kWPN_6_not_lookout");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_RIGHT_SHOULDER, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kCUSTOM19 && dik > 0,
	      "e3_1_world_RB_is_kCUSTOM19_not_lookout");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_DPAD_DOWN, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kCUSTOM20 && dik > 0,
	      "e3_1_world_dpad_down_is_kCUSTOM20");
	check(ResolveContextualButtonInjection(CInput::GP_BTN_RIGHT_STICK, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kCUSTOM24 && dik > 0,
	      "e3_1_world_R3_is_kCUSTOM24");

	// 5f. E2.1b: в World_Gameplay курки RT/LT разрешаются в kWPN_FIRE / kWPN_ZOOM с валидным DIK > 0
	check(ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kWPN_FIRE && dik > 0,
	      "e2_1b_world_RT_maps_to_kWPN_FIRE");
	check(ResolveContextualButtonInjection(VBTN_LT, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) && actId == kWPN_ZOOM && dik > 0,
	      "e2_1b_world_LT_maps_to_kWPN_ZOOM");

	// E2.2c: X у костра -- цель-туториал (bind_campfire -> game.start_tutorial, пункт по kUSE)
	{
		struct SFakeTutorialAction
		{
			EGameActions m_action;
		};
		const SFakeTutorialAction campfireItem[] = {{kUSE}};
		const SFakeTutorialAction otherItem[] = {{kQUIT}, {kWPN_RELOAD}};
		check(TutorialActionItemsContain(campfireItem, campfireItem + 1, kUSE), "e2_2c_tutorial_item_with_kUSE_listens");
		check(!TutorialActionItemsContain(otherItem, otherItem + 2, kUSE), "e2_2c_tutorial_item_without_kUSE_ignored");
		check(!TutorialActionItemsContain(campfireItem, campfireItem, kUSE), "e2_2c_empty_tutorial_item_ignored");
		check(!TutorialActionItemsContain(campfireItem, campfireItem + 1, -1), "e2_2c_negative_action_ignored");
		check(XTapChoosesUse(false, true), "e2_2c_x_tap_campfire_tutorial_is_use_not_reload");
		check(XTapChoosesUse(true, false), "e2_2c_x_tap_hud_target_is_use");
		check(!XTapChoosesUse(false, false), "e2_2c_x_tap_no_target_is_reload");
	}

	// E2.2d: приоритет целей X и конус "радиуса подсказок"
	{
		check(XTapTargetFromFlags(true, true, true, true) == EXTapTarget::Hud, "e2_2d_hud_target_first");
		check(XTapTargetFromFlags(false, true, true, true) == EXTapTarget::Tutorial, "e2_2d_tutorial_before_script");
		check(XTapTargetFromFlags(false, false, true, true) == EXTapTarget::Script, "e2_2d_script_before_near");
		check(XTapTargetFromFlags(false, false, false, true) == EXTapTarget::Near, "e2_2d_near_target_no_reload");
		check(XTapTargetFromFlags(false, false, false, false) == EXTapTarget::None, "e2_2d_nothing_is_reload");
		const Fvector cam = {0.f, 0.f, 0.f};
		const Fvector dir = {0.f, 0.f, 1.f};
		const Fvector ahead = {0.f, 0.f, 1.5f};
		const Fvector off5 = {1.5f * _sin(deg2rad(5.f)), 0.f, 1.5f * _cos(deg2rad(5.f))};
		const Fvector off20 = {1.5f * _sin(deg2rad(20.f)), 0.f, 1.5f * _cos(deg2rad(20.f))};
		const Fvector far3 = {0.f, 0.f, 3.f};
		const Fvector behind = {0.f, 0.f, -1.f};
		check(XPointInTargetCone(cam, dir, ahead, 2.f, 10.f), "e2_2d_cone_straight_ahead");
		check(XPointInTargetCone(cam, dir, off5, 2.f, 10.f), "e2_2d_cone_5deg_inside_10");
		check(!XPointInTargetCone(cam, dir, off20, 2.f, 10.f), "e2_2d_cone_20deg_outside_10");
		check(XPointInTargetCone(cam, dir, off20, 2.f, 25.f), "e2_2d_cone_20deg_inside_25");
		check(!XPointInTargetCone(cam, dir, far3, 2.f, 10.f), "e2_2d_cone_too_far");
		check(!XPointInTargetCone(cam, dir, behind, 2.f, 89.f), "e2_2d_cone_behind");
		check(!XPointInTargetCone(cam, dir, ahead, 2.f, 0.f), "e2_2d_cone_angle_0_off");
		// E2.2e: липкая цель взгляда -- только геймпад
		check(StickyLookTargetAllowed(true, 10.f, false), "e2_2e_sticky_gamepad_on");
		check(!StickyLookTargetAllowed(false, 10.f, false), "e2_2e_sticky_mouse_off");
		check(!StickyLookTargetAllowed(true, 0.f, false), "e2_2e_sticky_angle_0_off");
		check(!StickyLookTargetAllowed(true, 10.f, true), "e2_2e_sticky_external_handler_off");
	}

	// 6. F1.2: переход между вложенными модальными окнами внутри MenuUI_Modal (L1 -> L2 -> L3 -> L2)
	// обязан срабатывать как смена контекста с барьером consumed_until_release на зажатых A и B
	{
		SetLuaModalMenuTag("gp_focus_modal_L1");
		StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "f1_2_modal_L1", 0, 0, 0, 3000, false);
		StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "f1_2_press_A_in_L1", bitA, 0, 0, 3016, false);
		check(IsButtonPressed(CInput::GP_BTN_SOUTH), "f1_2_btnA_pressed_in_L1");

		// Нажатие A открывает L2: тег меняется на gp_focus_modal_L2 при всё ещё зажатой A
		SetLuaModalMenuTag("gp_focus_modal_L2");
		StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "f1_2_push_L2_with_A_held", bitA, 0, 0, 3032, false);
		check(DidContextChangeThisFrame(), "f1_2_modal_layer_L1_to_L2_triggers_ctx_change");
		check(IsButtonConsumedUntilRelease(CInput::GP_BTN_SOUTH), "f1_2_btnA_consumed_in_L2_until_release");
		check(!IsButtonPressed(CInput::GP_BTN_SOUTH) && !IsButtonHeld(CInput::GP_BTN_SOUTH), "f1_2_btnA_does_not_punch_through_to_L2");

		// В L2 отпускаем A и нажимаем B для закрытия L2 -> возврат в L1 с зажатой B
		StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "f1_2_release_A_press_B_in_L2", bitB, 0, 0, 3048, false);
		check(IsButtonPressed(CInput::GP_BTN_EAST), "f1_2_btnB_pressed_in_L2");
		SetLuaModalMenuTag("gp_focus_modal_L1");
		StepStateMachine(EGamepadContext::MenuUI_Modal, sub, "f1_2_pop_L2_to_L1_with_B_held", bitB, 0, 0, 3064, false);
		check(DidContextChangeThisFrame(), "f1_2_modal_layer_L2_to_L1_triggers_ctx_change");
		check(IsButtonConsumedUntilRelease(CInput::GP_BTN_EAST), "f1_2_btnB_consumed_in_L1_until_release");
		check(!IsButtonPressed(CInput::GP_BTN_EAST) && !IsButtonHeld(CInput::GP_BTN_EAST), "f1_2_btnB_does_not_punch_through_to_L1");
	}

	// 7. FG.6h: проверка блокировки аналогового движения актора при открытом UI (MenuUI_Modal с AllowMovement(true),
	//    интерактивный 3D-КПК World_3DPDA) и барьера отпускания стика m_bStickMoveConsumedUntilNeutral при закрытии UI
	{
		SContextSubFlags uiSub = {};
		uiSub.bModalAllowsMovement = true;
		StepStateMachine(EGamepadContext::MenuUI_Modal, uiSub, "fg6h_inv_no_pause_open", 0, 28000, 0, 4000, false);
		check(!AllowsWorldMove(), "fg6h_modal_ui_blocks_world_move_even_with_allow_movement");

		uiSub.b3dPdaInteractive = true;
		StepStateMachine(EGamepadContext::World_3DPDA, uiSub, "fg6h_3dpda_interactive", 0, 28000, 0, 4016, false);
		check(!AllowsWorldMove(), "fg6h_3dpda_interactive_blocks_world_move");

		// При переходе из интерактивного 3D-КПК в freelook с отклонённым левым стиком взводится барьер до нейтрали
		uiSub.b3dPdaInteractive = false;
		StepStateMachine(EGamepadContext::World_3DPDA, uiSub, "fg6h_3dpda_to_freelook_stick_held", 0, 28000, 0, 4032, false);
		check(IsStickMoveConsumedUntilNeutral() && !AllowsWorldMove(), "fg6h_3dpda_freelook_stick_barrier_until_neutral");

		// Возврат стика в нейтраль снимает барьер и разрешает движение в freelook
		StepStateMachine(EGamepadContext::World_3DPDA, uiSub, "fg6h_3dpda_freelook_stick_neutral", 0, 0, 0, 4048, false);
		check(!IsStickMoveConsumedUntilNeutral() && AllowsWorldMove(), "fg6h_3dpda_freelook_allows_move_after_neutral");

		// При закрытии MenuUI_Modal в World_Gameplay с отклонённым стиком движение заблокировано до нейтрали
		StepStateMachine(EGamepadContext::MenuUI_Modal, uiSub, "fg6h_reopen_modal", 0, 28000, 0, 4064, false);
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "fg6h_close_modal_stick_held", 0, 28000, 0, 4080, false);
		check(IsStickMoveConsumedUntilNeutral() && !AllowsWorldMove(), "fg6h_close_modal_blocks_move_until_neutral");

		StepStateMachine(EGamepadContext::World_Gameplay, sub, "fg6h_world_stick_neutral", 0, 0, 0, 4096, false);
		check(!IsStickMoveConsumedUntilNeutral() && AllowsWorldMove(), "fg6h_world_allows_move_after_neutral");
	}

	// 8. K2 (D61): проверка разрешения World_Gameplay через BindingProfileModel,
	//    разделения Tap/Hold при переназначении колеса на Y (NextWeapon = button:Y, WeaponWheel = hold:Y:220)
	//    и отпускания колеса по назначенной кнопке Y.
	{
		gpbind::BindingProfileModel& model = gpbind::GetMutableActiveBindingProfile();
		model.ResetToDefaults();
		m_luaHints.bFddaInputDisabled = false;

		// 8a. При перестановке Jump <-> Crouch в профиле ResolveContextualButtonInjection возвращает новые действия
		model.SetBinding(gpbind::ContextId::World_Gameplay,
		                 gpbind::ActionId::Jump,
		                 gpbind::BindingTrigger::MakeButton(gpbind::ButtonId::East),
		                 true);
		check(ResolveContextualButtonInjection(CInput::GP_BTN_EAST, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) &&
		          actId == kJUMP,
		      "k2_profile_swap_B_resolves_to_kJUMP");
		check(ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::World_Gameplay, sub, actId, dik, injReason) &&
		          actId == kCROUCH,
		      "k2_profile_swap_A_resolves_to_kCROUCH");
		model.ResetToDefaults();

		// 8b. Переназначение WeaponWheel на hold:Y:220 при NextWeapon = button:Y (п. 3 K2):
		//     - на нажатии Y переключение оружия НЕ срабатывает (кнопка ждёт порога или отпускания);
		//     - при коротком отпускании (< 220 мс) срабатывает NextWeapon;
		//     - при удержании (>= 220 мс) срабатывает WeaponWheel и фиксируется m_wheelHoldBtnBit = GP_BTN_NORTH.
		const gpbind::EditResult wheelOnY = model.SetBinding(
		    gpbind::ContextId::World_Gameplay,
		    gpbind::ActionId::WeaponWheel,
		    gpbind::BindingTrigger::MakeHold(gpbind::ButtonId::North, 220),
		    true);
		check(wheelOnY.Succeeded(), "k2_set_weapon_wheel_hold_Y_220_succeeded");

		const u32 bitY = (1u << CInput::GP_BTN_NORTH);
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_init_world", 0, 0, 0, 5000, false);

		// Нажимаем Y в момент 5010: кнопка перехвачена в handledMask (не уходит в мгновенный NextWeapon на нажатии)
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_press_Y", bitY, 0, 0, 5010, false);
		const u32 handledPress = StepWorldWeaponButtons(5010, false);
		check((handledPress & bitY) != 0 && m_worldDualPending[CInput::GP_BTN_NORTH] && !m_worldDualHoldCommitted[CInput::GP_BTN_NORTH],
		      "k2_press_Y_enters_dual_pending_without_firing_tap");

		// Короткое отпускание Y через 100 мс (< 220 мс) -> срабатывает NextWeapon (world_next_weapon) по отпусканию!
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_release_Y_short", 0, 0, 0, 5110, false);
		m_lastWeaponAction = "none";
		StepWorldWeaponButtons(5110, false);
		check(!m_worldDualPending[CInput::GP_BTN_NORTH] && 0 == xr_strcmp(m_lastWeaponAction, "world_next_weapon"),
		      "k2_short_release_Y_fires_next_weapon_on_release");

		// Снова нажимаем Y и держим >= 220 мс (5200 -> 5430 = 230 мс) -> срабатывает WeaponWheel на удержании Y!
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_press_Y_for_hold", bitY, 0, 0, 5200, false);
		StepWorldWeaponButtons(5200, false);
		m_bWheelLbWasPhysicallyDown = false;
		m_wheelHoldBtnBit = CInput::GP_BTN_LEFT_SHOULDER;
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_hold_Y_230ms", bitY, 0, 0, 5430, false);
		StepWorldWeaponButtons(5430, false);
		check(m_worldDualHoldCommitted[CInput::GP_BTN_NORTH] &&
		          m_bWheelLbWasPhysicallyDown &&
		          m_wheelHoldBtnBit == CInput::GP_BTN_NORTH,
		      "k2_hold_Y_220ms_commits_weapon_wheel_on_Y");

		// Отпускание Y после срабатывания Hold НЕ вызывает NextWeapon
		m_lastWeaponAction = "none";
		StepStateMachine(EGamepadContext::World_Gameplay, sub, "k2_release_Y_after_hold", 0, 0, 0, 5500, false);
		StepWorldWeaponButtons(5500, false);
		check(0 == xr_strcmp(m_lastWeaponAction, "none"), "k2_release_Y_after_hold_does_not_fire_next_weapon");

		// 8c. FIX-4 / FIX-5: NextWeapon циклически переключает слоты 1..4 (kWPN_1..kWPN_4, а не kWPN_NEXT),
		//     колесо патронов (ERadialWheelType::Ammo) использует нативный выбор сектора без виртуального курсора,
		//     а удержание kJUMP выставляет DIK_SPACE в pInput->iGetAsyncKeyState(DIK_SPACE) для demonized_ledge_grabbing.
		model.ResetToDefaults();
		m_nextWeaponCycleIdx = 0;
		const int w1 = ResolveNextWeaponSlotAction();
		const int w2 = ResolveNextWeaponSlotAction();
		const int w3 = ResolveNextWeaponSlotAction();
		const int w4 = ResolveNextWeaponSlotAction();
		check(w1 >= kWPN_1 && w1 <= kWPN_4 && w2 != w1 && w3 != w2 && w4 != w3,
		      "fix4_next_weapon_cycles_slots_1_to_4_not_kwpn_next");
		check(!IsVirtualCursorWheelType(ERadialWheelType::Ammo) &&
		          !IsVirtualCursorWheelType(ERadialWheelType::QuickAction_Haru) &&
		          IsVirtualCursorWheelType(ERadialWheelType::Interactive_Pie),
		      "fix4_ammo_wheel_uses_native_sector_without_virtual_cursor");
		if (pInput)
		{
			pInput->SetGamepadInjectedKeyState(DIK_SPACE, true);
			check(!!pInput->iGetAsyncKeyState(DIK_SPACE), "fix5_jump_holds_dik_space_in_async_key_state");
			pInput->SetGamepadInjectedKeyState(DIK_SPACE, false);
			check(!pInput->iGetAsyncKeyState(DIK_SPACE), "fix5_jump_releases_dik_space_in_async_key_state");
		}
	}

	gpbind::GetMutableActiveBindingProfile() = savedBindingProfile;

	// Восстанавливаем исходное состояние диспетчера
	*this = savedState;

	Msg("* [Gamepad] gp_ctx selftest: %s (passed=%u failed=%u)",
	    (failed == 0) ? "PASS" : "FAIL", passed, failed);
	return failed == 0;
}

// ---------------------------------------------------------------------------------------------
// E5.1 (D13, D14, D43, D45): быстрое колесо (gp_wheel.script), выбор сектора с гистерезисом и барьер закрытия
// ---------------------------------------------------------------------------------------------

void CGamepadContextDispatcher::ResetWheelSectorState()
{
	m_bWheelStickActive = false;
	m_wheelSelectedSector = -1;
	m_wheelLastAngleRad = 0.0f;
	m_wheelLastMagnitude = 0.0f;
}

gpmath::RadialSectorResult CGamepadContextDispatcher::UpdateWheelSectorFromRightStick(int sectorCount, int currentSector)
{
	float rx = 0.0f;
	float ry = 0.0f;
	if (pInput)
	{
		const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		if (gp.connected && gp.has_focus)
		{
			rx = gpmath::NormalizeStickAxis(gp.axes[2]);
			ry = -gpmath::NormalizeStickAxis(gp.axes[3]);
		}
	}

	gpmath::RadialSectorConfig cfg{};
	cfg.enter_threshold = 0.35f;
	cfg.exit_threshold = 0.25f;
	cfg.angular_hysteresis_rad = 0.12f;

	const int prevSec = (currentSector >= 0) ? currentSector : m_wheelSelectedSector;
	const gpmath::RadialSectorResult res =
	    gpmath::SelectRadialSector(cfg, rx, ry, sectorCount, prevSec, m_bWheelStickActive);

	m_bWheelStickActive = res.stick_active;
	m_wheelSelectedSector = res.selected_sector;
	m_wheelLastAngleRad = res.angle_rad;
	m_wheelLastMagnitude = res.magnitude;
	return res;
}

void CGamepadContextDispatcher::SetWheelTimeMode(LPCSTR mode)
{
	if (!mode || !mode[0])
	{
		m_wheelTimeMode = "slowmo";
		return;
	}
	if (0 == _stricmp(mode, "pause") || 0 == _stricmp(mode, "realtime") || 0 == _stricmp(mode, "slowmo"))
	{
		m_wheelTimeMode = mode;
	}
	else
	{
		m_wheelTimeMode = "slowmo";
	}
}

bool CGamepadContextDispatcher::IsVirtualCursorWheelType(ERadialWheelType t)
{
	// Колесо патронов (ERadialWheelType::Ammo) и основное колесо (QuickAction_Haru) используют нативный выбор сектора
	// правым стиком БЕЗ виртуального курсора (проблема 4).
	return t == ERadialWheelType::Interactive_Pie || t == ERadialWheelType::Camo_3DSS ||
	       t == ERadialWheelType::Companion_Flat;
}

Fvector2 CGamepadContextDispatcher::StepCursorKinematics(Fvector2 curPos,
                                                         gpmath::Vec2 moveStick,
                                                         u32 dtMs,
                                                         bool targetHit,
                                                         Fvector2 targetCenter,
                                                         u32 timeSinceStickActiveMs,
                                                         float& ioRemX,
                                                         float& ioRemY,
                                                         bool* outMoved,
                                                         bool* outSoftSnapped,
                                                         bool smoothAccelCurve)
{
	if (outMoved)
		*outMoved = false;
	if (outSoftSnapped)
		*outSoftSnapped = false;

	if (dtMs > 50)
		dtMs = 50;
	const float dtSec = static_cast<float>(dtMs) * 0.001f;
	const float mag = _sqrt(moveStick.x * moveStick.x + moveStick.y * moveStick.y);
	const float clampedMag = clampr(mag, 0.0f, 1.0f);

	// CURSOR1: множитель ползунка скорости курсора (0.4..2.5) во всех курсорных окнах + плавная кривая ускорения в режиме «мышь»
	const float speedMult = clampr(ps_gp_cursor_speed, 0.4f, 2.5f) *
	                        (smoothAccelCurve ? (0.55f + 0.85f * clampedMag * clampedMag) : 1.0f);

	// FG.1 (4): когда курсор проходит над видимой ячейкой/слотом/кнопкой, скорость снижается на ~55% (множитель 0.45)
	const float frictionMult = targetHit ? VIRTUAL_CURSOR_FRICTION_MULT : 1.0f;
	const float speed = VIRTUAL_CURSOR_SPEED_UI_PX_PER_S * speedMult * frictionMult;

	float stepX = moveStick.x * speed * dtSec;
	float stepY = moveStick.y * speed * dtSec;

	// FG.1 (4): мягкое притяжение (Soft Snap) к центру ячейки при замедлении или отпускании стика внутри цели
	bool softSnapApplied = false;
	if (targetHit && mag < 0.35f && timeSinceStickActiveMs <= VIRTUAL_CURSOR_SOFT_SNAP_WINDOW_MS)
	{
		const float toCx = targetCenter.x - curPos.x;
		const float toCy = targetCenter.y - curPos.y;
		const float dist = _sqrt(toCx * toCx + toCy * toCy);
		if (dist > VIRTUAL_CURSOR_SOFT_SNAP_STOP_RADIUS_PX)
		{
			const float pullAlpha = clampr(VIRTUAL_CURSOR_SOFT_SNAP_PULL_PER_S * dtSec * (1.0f - (mag / 0.35f)), 0.0f, 0.45f);
			stepX += toCx * pullAlpha;
			stepY += toCy * pullAlpha;
			softSnapApplied = true;
		}
	}

	ioRemX += stepX;
	ioRemY += stepY;
	const int dx = static_cast<int>(ioRemX);
	const int dy = static_cast<int>(ioRemY);
	if (dx == 0 && dy == 0)
		return curPos;

	ioRemX -= static_cast<float>(dx);
	ioRemY -= static_cast<float>(dy);

	Fvector2 newPos;
	newPos.set(curPos.x + static_cast<float>(dx), curPos.y + static_cast<float>(dy));
	clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
	clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);

	if (outMoved)
		*outMoved = (_abs(newPos.x - curPos.x) > 1e-4f) || (_abs(newPos.y - curPos.y) > 1e-4f);
	if (outSoftSnapped)
		*outSoftSnapped = softSnapApplied && (outMoved ? *outMoved : true);
	return newPos;
}

int CGamepadContextDispatcher::StepRightStickScrollAccumulator(float stickY,
                                                               u32 dtMs,
                                                               u32 nowContinual,
                                                               float& ioScrollAccum,
                                                               u32& ioLastScrollStepMs)
{
	if (dtMs > 50)
		dtMs = 50;

	if (_abs(stickY) < 0.08f)
	{
		ioScrollAccum = 0.0f;
		return 0;
	}

	const float dtSec = static_cast<float>(dtMs) * 0.001f;
	// SDL/ui_stick: положительный stickY (стик вниз) -> прокрутка списка вниз (отрицательное направление колеса мыши -1)
	ioScrollAccum += (-stickY) * VIRTUAL_CURSOR_SCROLL_STEPS_PER_S * dtSec;

	if (_abs(ioScrollAccum) < 1.0f)
		return 0;

	if (ioLastScrollStepMs != 0 && nowContinual >= ioLastScrollStepMs &&
	    (nowContinual - ioLastScrollStepMs) < VIRTUAL_CURSOR_SCROLL_MIN_INTERVAL_MS)
	{
		return 0;
	}

	const int stepDir = (ioScrollAccum > 0.0f) ? 1 : -1;
	ioScrollAccum -= static_cast<float>(stepDir);
	ioLastScrollStepMs = (nowContinual != 0) ? nowContinual : 1;
	return stepDir;
}

int CGamepadContextDispatcher::SelectBestDpadSnapTarget(Fvector2 curPos,
                                                        int curContainerId,
                                                        ENavDir dir,
                                                        const SCursorSnapTarget* targets,
                                                        int targetCount)
{
	if (!targets || targetCount <= 0 || dir == ENavDir::None)
		return -1;

	auto findBest = [&](bool strictCone, bool requireSameContainer) -> int
	{
		int bestIdx = -1;
		float bestScore = 1e9f;

		for (int i = 0; i < targetCount; ++i)
		{
			const SCursorSnapTarget& t = targets[i];
			if (requireSameContainer && curContainerId != 0 && t.containerId != curContainerId)
				continue;

			const float dx = t.cx - curPos.x;
			const float dy = t.cy - curPos.y;
			if (_abs(dx) < 2.0f && _abs(dy) < 2.0f)
				continue;

			float para = 0.0f;
			float perp = 0.0f;
			switch (dir)
			{
			case ENavDir::Up:
				if (dy >= -3.0f) continue;
				para = -dy;
				perp = _abs(dx);
				break;
			case ENavDir::Down:
				if (dy <= 3.0f) continue;
				para = dy;
				perp = _abs(dx);
				break;
			case ENavDir::Left:
				if (dx >= -3.0f) continue;
				para = -dx;
				perp = _abs(dy);
				break;
			case ENavDir::Right:
				if (dx <= 3.0f) continue;
				para = dx;
				perp = _abs(dy);
				break;
			default:
				continue;
			}

			if (strictCone && perp > (para * 1.85f + 16.0f))
				continue;

			float score = para + perp * 2.25f;
			if (curContainerId != 0 && t.containerId == curContainerId)
				score *= 0.85f;

			if (score < bestScore)
			{
				bestScore = score;
				bestIdx = i;
			}
		}
		return bestIdx;
	};

	// Сначала ищем соседа в том же контейнере в узком конусе, затем в любом контейнере в узком конусе, затем в широком полупространстве
	if (curContainerId != 0)
	{
		const int sameStrict = findBest(true, true);
		if (sameStrict >= 0)
			return sameStrict;
	}
	const int anyStrict = findBest(true, false);
	if (anyStrict >= 0)
		return anyStrict;
	return findBest(false, false);
}

struct SMenuFocusTarget
{
	CUIWindow* widget = nullptr;
	CUIWindow* panel = nullptr;
	CUIScrollView* scroll = nullptr;
	Frect bounds;
	Frect clip;
	float widgetTop = 0.0f;
	LPCSTR kind = "";
	bool activeTab = false;
	bool footer = false;
};

namespace
{
constexpr size_t MenuShortcutCount = 4;
constexpr u32 MenuDefaultHoldMs = 800;
constexpr u32 MenuRadioCaptionMs = 150 + 220; // Travel to the caption, then pause before moving to the ring.
constexpr int MenuDefaultFrames = 31;
using MenuFooter = std::array<CUIButton*, MenuShortcutCount>;

float CenterX(const Frect& rect) { return (rect.x1 + rect.x2) * 0.5f; }
float CenterY(const Frect& rect) { return (rect.y1 + rect.y2) * 0.5f; }

Frect IntersectMenuRects(const Frect& a, const Frect& b)
{
	return Frect().set(_max(a.x1, b.x1), _max(a.y1, b.y1), _min(a.x2, b.x2), _min(a.y2, b.y2));
}

bool IsMenuRectVisible(const Frect& rect, const Frect& visible)
{
	return visible.width() >= _min(8.0f, rect.width() * 0.5f) && visible.height() >= _min(8.0f, rect.height() * 0.5f);
}

bool IsSameMenuRow(const Frect& a, const Frect& b)
{
	return _abs(CenterY(a) - CenterY(b)) <= 0.4f * _min(a.height(), b.height());
}

// MT-MAY: в майском движке нет CUIWindow::MouseCapturer — ищем раскрытый список обходом видимого дерева root.
CUIComboBox* GetExpandedMenuCombo(CUIWindow* root, int depth = 0)
{
	if (!root || !root->IsShown() || depth > 24)
		return nullptr;
	if (auto* combo = smart_cast<CUIComboBox*>(root))
		return CUIComboBoxProbe::IsExpanded(combo) ? combo : nullptr;
	for (CUIWindow* child : root->GetChildWndList())
	{
		if (CUIComboBox* found = GetExpandedMenuCombo(child, depth + 1))
			return found;
	}
	return nullptr;
}

CUIMessageBox* FindActiveMenuMessageBox(CUIWindow* window, int depth = 0)
{
	if (!window || !window->IsShown() || !window->IsEnabled() || depth > 24)
		return nullptr;
	if (auto* box = smart_cast<CUIMessageBox*>(window))
		return box;
	const auto& children = window->GetChildWndList();
	for (auto child = children.rbegin(); child != children.rend(); ++child)
		if (auto* box = FindActiveMenuMessageBox(*child, depth + 1))
			return box;
	return nullptr;
}

bool IsNativeMenuPage(const CUIWindow* window)
{
	const auto name = window->WindowName();
	return name.size() && xr_strcmp(name.c_str(), "gp_native_page") == 0;
}

constexpr LPCSTR NativeMenuButtons[] = {
	"A", "B", "X", "Y", "View", "Guide", "Menu", "L3", "R3", "LB", "RB", "DU", "DD", "DL", "DR", "LT", "RT"
};
static_assert(std::size(NativeMenuButtons) == CGamepadContextDispatcher::VBTN_TOTAL_COUNT);

bool NativeMenuButton(LPCSTR button, LPCSTR phase, u32 now)
{
	try
	{
		luabind::functor<bool> action;
		return ai().script_engine().functor("ui_gp_native.menu_row_button", action) && action(button, phase, now);
	}
	catch (...)
	{
		Msg("! [Gamepad] Native menu row action failed");
		return false;
	}
}

bool NativeMenuCaptureActive()
{
	try
	{
		luabind::functor<bool> active;
		return ai().script_engine().functor("ui_gp_native.menu_capture_active", active) && active();
	}
	catch (...)
	{
		return false;
	}
}

void SelectNativeMenuRow(LPCSTR name)
{
	try
	{
		luabind::functor<bool> select;
		if (ai().script_engine().functor("ui_gp_native.select_menu_row", select))
			select(name);
	}
	catch (...)
	{
		Msg("! [Gamepad] Native menu row selection failed");
	}
}

shared_str SelectedNativeMenuRow()
{
	try
	{
		luabind::functor<LPCSTR> selected;
		if (ai().script_engine().functor("ui_gp_native.menu_selected_row", selected))
			return shared_str(static_cast<LPCSTR>(selected()));
	}
	catch (...)
	{
		Msg("! [Gamepad] Native menu row lookup failed");
	}
	return {};
}

// MENU-FOCUS P16: Y in the main/pause menu (quick save, LB+Y = hidden emergency save). ui_gp_pause.script decides.
bool PauseMenuQuickSave(bool withLeftShoulder)
{
	try
	{
		luabind::functor<bool> action;
		return ai().script_engine().functor("ui_gp_pause.menu_quick_save", action) && action(withLeftShoulder);
	}
	catch (...)
	{
		Msg("! [Gamepad] Pause menu quick save failed");
		return false;
	}
}

// MENU-FOCUS P16: one short weak pulse (end of a Y/B hold).
void MenuPulse()
{
	if (pInput && pInput->GetGamepadHaptics())
		pInput->GetGamepadHaptics()->PulseMenu("menu_hold");
}

CUICustomEdit* FindMenuEdit(CUIWindow* window, int depth = 0)
{
	if (!window || !window->IsShown() || depth > 24)
		return nullptr;
	if (auto* edit = smart_cast<CUICustomEdit*>(window))
		return edit;
	for (CUIWindow* child : window->GetChildWndList())
		if (auto* edit = FindMenuEdit(child, depth + 1))
			return edit;
	return nullptr;
}

bool IsMenuSaveFooter(const MenuFooter& footer)
{
	return footer[0] && xr_strcmp(footer[0]->TextItemControl()->GetText(),
		CStringTable().translate("ui_mm_save").c_str()) == 0;
}

// MENU-FOCUS P16: the save window with the gamepad names a new save by itself: "<level> - dd.mm.yyyy hh-mm".
void FillAutoSaveName(CUIWindow* root)
{
	auto* edit = FindMenuEdit(root);
	if (!edit || (edit->GetText() && *edit->GetText()))
		return;
	string256 level = "save";
	if (g_pGameLevel)
		xr_strcpy(level, CStringTable().translate(Level().name()).c_str());
	for (char* c = level; *c; ++c)
		if (strchr("/\\:*?\"<>|^()[]%", *c))
			*c = ' ';
	const __time64_t now = _time64(nullptr);
	tm local{};
	_localtime64_s(&local, &now);
	string512 name;
	xr_sprintf(name, "%s - %02d.%02d.%04d %02d-%02d", level, local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
		local.tm_hour, local.tm_min);
	edit->SetText(name);
}

MenuFooter FindMenuFooter(CUIWindow* root, bool* isLoad = nullptr)
{
	const CStringTable strings;
	const shared_str labels[] = {
		strings.translate("ui_mm_apply"), strings.translate("ui_mm_reset"),
		strings.translate("ui_mm_cancel"), strings.translate("ui_mm_default")
	};
	const shared_str load = strings.translate("ui_mm_load");
	const shared_str save = strings.translate("ui_mm_save"); // MENU-FOCUS P16: the save window works like loading
	const shared_str erase = strings.translate("ui_mm_delete");
	auto find = [&](auto& self, CUIWindow* parent, int depth) -> MenuFooter
	{
		MenuFooter row{};
		CUIButton* loadButton = nullptr;
		CUIButton* deleteButton = nullptr;
		if (!parent->IsShown() || depth > 24)
			return row;
		for (CUIWindow* child : parent->GetChildWndList())
		{
			auto* button = smart_cast<CUI3tButton*>(child);
			if (!button || !button->IsShown())
				continue;
			const LPCSTR label = button->TextItemControl()->GetText();
			if (label && (xr_strcmp(label, load.c_str()) == 0 || xr_strcmp(label, save.c_str()) == 0))
				loadButton = button;
			if (label && xr_strcmp(label, erase.c_str()) == 0)
				deleteButton = button;
			for (size_t i = 0; i < row.size(); ++i)
			{
				if (label && xr_strcmp(label, labels[i].c_str()) == 0)
					row[i] = button;
			}
		}
		const bool loading = !row[0] && !row[1] && loadButton && deleteButton;
		if (loading)
		{
			row[0] = loadButton;
			row[1] = deleteButton;
		}
		if (row[0] && row[1] && row[2])
		{
			Frect a, b, c;
			row[0]->GetAbsoluteRect(a);
			row[1]->GetAbsoluteRect(b);
			row[2]->GetAbsoluteRect(c);
			if (IsSameMenuRow(a, b) && IsSameMenuRow(a, c))
			{
				if (isLoad)
					*isLoad = loading;
				return row;
			}
		}
		for (CUIWindow* child : parent->GetChildWndList())
		{
			if (smart_cast<CUIScrollView*>(child) || smart_cast<CUIButton*>(child) ||
				smart_cast<CUIComboBox*>(child) || smart_cast<CUIMMShniaga*>(child))
				continue;
			const MenuFooter found = self(self, child, depth + 1);
			if (found[0])
				return found;
		}
		return {};
	};
	return find(find, root, 0);
}

Frect GetMenuTargetClip(const SMenuFocusTarget& target, const CUIComboBox* combo)
{
	Frect clip = Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT);
	for (CUIWindow* parent = target.widget->GetParent(); parent && parent != combo; parent = parent->GetParent())
	{
		if (auto* scroll = smart_cast<CUIScrollView*>(parent))
		{
			Frect rect;
			CUIScrollViewProbe::GetVisibleRect(scroll, rect);
			clip = IntersectMenuRects(clip, rect);
		}
	}
	return clip;
}

void TrimMenuRadioBounds(Frect& rect)
{
	const float width = rect.width();
	const float height = rect.height();
	rect.x1 += width * (28.0f / 94.0f);
	rect.x2 -= width * (29.0f / 94.0f);
	rect.y1 += height * (10.0f / 56.0f);
	rect.y2 -= height * (11.0f / 56.0f);
}

// MENU-FOCUS-5 P14: Settings/MCM rows (ui_options.script / ui_mcm.script Reset_opt): every row is a
// "main:st" static added to scroll_opt; its caption is "elements:cap" (CUIStatic, x=10, vert_align c).
CUIStatic* FindMenuRowCaption(CUIWindow* row)
{
	if (!row)
		return nullptr;
	for (CUIWindow* child : row->GetChildWndList())
	{
		auto* st = smart_cast<CUIStatic*>(child);
		if (!st || smart_cast<CUIButton*>(child) || !child->IsShown())
			continue;
		const float x = child->GetWndPos().x;
		if ((x < 4.0f || x > 16.0f) && child->WindowName() != "gp_lighting_caption")
			continue;
		const LPCSTR text = st->TextItemControl()->GetText();
		if (text && text[0])
			return st;
	}
	return nullptr;
}

bool IsMenuSettingsScroll(CUIScrollView* scroll)
{
	if (!scroll || smart_cast<CUIListBox*>(scroll))
		return false;
	for (CUIWindow* row : scroll->Items())
		if (FindMenuRowCaption(row))
			return true;
	return false;
}

bool IsMenuTab(CUIWindow* widget)
{
	auto* check = smart_cast<CUICheckButton*>(widget);
	const CUIStatic* face = check && check->m_background ? check->m_background->Get(S_Enabled) : nullptr;
	return face && face->m_TextureName.size() && strstr(face->m_TextureName.c_str(), "TabLED");
}

bool IsMenuTabColumn(CUIScrollView* view)
{
	for (CUIWindow* row : view->Items())
		for (CUIWindow* child : row->GetChildWndList())
			if (IsMenuTab(child))
				return true;
	return false;
}

CUIWindow* FindMenuSettingsRow(CUIWindow* widget, CUIScrollView* scroll)
{
	for (CUIWindow* w = widget; w; w = w->GetParent())
	{
		CUIWindow* parent = w->GetParent();
		if (!parent || parent == scroll)
			return nullptr;
		if (parent->GetParent() == scroll)
			return w;
	}
	return nullptr;
}

bool IsMenuRadioTexture(CUIWindow* widget)
{
	auto* check = smart_cast<CUICheckButton*>(widget);
	const CUIStatic* face = check && check->m_background ? check->m_background->Get(S_Enabled) : nullptr;
	return face && face->m_TextureName.size() && strstr(face->m_TextureName.c_str(), "ui_radio");
}

struct SHorizontalMenuRadio
{
	CUIWindow* group = nullptr;
	CUIWindow* first = nullptr;
	bool hasSelection = false;
};

SHorizontalMenuRadio GetHorizontalMenuRadio(const SMenuFocusTarget& target)
{
	if (!target.scroll || !IsMenuRadioTexture(target.widget))
		return {};
	auto* parent = target.widget->GetParent();
	if (!parent)
		return {};
	Frect focus;
	target.widget->GetAbsoluteRect(focus);
	SHorizontalMenuRadio radio{parent, target.widget, false};
	float left = focus.x1;
	int count = 0;
	for (CUIWindow* child : parent->GetChildWndList())
	{
		if (!child->IsShown() || !IsMenuRadioTexture(child))
			continue;
		Frect bounds;
		child->GetAbsoluteRect(bounds);
		if (!IsSameMenuRow(bounds, focus))
			return {};
		radio.hasSelection |= static_cast<CUICheckButton*>(child)->GetCheck();
		if (!child->IsEnabled())
			continue;
		++count;
		if (bounds.x1 < left)
		{
			radio.first = child;
			left = bounds.x1;
		}
	}
	return count > 1 ? radio : SHorizontalMenuRadio{};
}

// Caption text block (not the 30-unit box): left edge of the text and the vertical center of all lines.
void GetMenuCaptionText(CUIStatic* cap, float& left, float& centerY, int& lines)
{
	Frect r;
	cap->GetAbsoluteRect(r);
	CUILines* text = cap->TextItemControl();
	const float h = text->GetVisibleHeight();
	float top = r.y1 + text->m_TextOffset.y;
	switch (text->GetVTextAlignment())
	{
	case valCenter: top += (r.height() - h) * 0.5f; break;
	case valBotton: top += r.height() - h; break;
	default: break;
	}
	left = r.x1 + text->m_TextOffset.x;
	centerY = top + h * 0.5f;
	lines = _max(1, int(text->m_lines.size()));
}

// Vertical radio groups keep P14 behavior. Horizontal groups first anchor at the caption.
bool ComputeMenuCaretAnchor(const SMenuFocusTarget& target, const Frect& visible, Fvector2& anchor, float& gapPx,
	const SHorizontalMenuRadio& horizontal, bool radioAtRing)
{
	if (xr_strcmp(target.kind, "gp_native_row") == 0)
	{
		Frect page;
		target.panel->GetAbsoluteRect(page);
		anchor.set(page.x1 + 6.0f, CenterY(visible));
		gapPx = 6.0f;
		return true;
	}
	if (xr_strcmp(target.kind, "keybind") == 0)
	{
		anchor.set(visible.x1, CenterY(visible));
		gapPx = 6.0f;
		return true;
	}
	if (!target.scroll || target.footer || !IsMenuSettingsScroll(target.scroll))
		return false;
	CUIWindow* row = FindMenuSettingsRow(target.widget, target.scroll);
	if (!row)
		return false;
	gapPx = 16.0f;
	if (IsMenuRadioTexture(target.widget) && target.widget->GetParent())
	{
		CUIWindow* first = nullptr;
		for (CUIWindow* child : target.widget->GetParent()->GetChildWndList())
			if (IsMenuRadioTexture(child))
			{
				first = child;
				break;
			}
		if (horizontal.group ? radioAtRing : first && first != target.widget)
		{
			anchor.set(visible.x1, (visible.y1 + visible.y2) * 0.5f);
			gapPx = 18.0f;
			return true;
		}
	}
	if (CUIStatic* cap = FindMenuRowCaption(row))
	{
		int lines = 1;
		GetMenuCaptionText(cap, anchor.x, anchor.y, lines);
		return true;
	}
	anchor.set(visible.x1, (visible.y1 + visible.y2) * 0.5f);
	return true;
}

float MenuEdgePadding()
{
	const float scaleY = UI().ClientToScreenScaledY(1.0f);
	return (scaleY > 1e-4f) ? (20.0f / scaleY) : 20.0f;
}

void GatherMenuFocusTargets(CUIWindow* root, CUIComboBox* combo, const MenuFooter& footer,
	xr_vector<SMenuFocusTarget>& targets, bool padScroll = false)
{
	const Frect screen = Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT);
	auto add = [&](CUIWindow* widget, CUIWindow* panel, CUIScrollView* scroll, const Frect& rect,
				   const Frect& clip, LPCSTR kind, bool activeTab, bool isFooter)
	{
		const Frect visible = IntersectMenuRects(rect, clip);
		if (visible.width() < _min(8.0f, rect.width() * 0.5f) || rect.height() < 6.0f || (!scroll && !IsMenuRectVisible(rect, visible)))
			return;
		Frect absolute;
		widget->GetAbsoluteRect(absolute);
		targets.push_back({widget, panel, scroll, rect, clip, absolute.y1, kind, activeTab, isFooter});
	};

	if (combo)
	{
		auto& list = combo->m_list_box;
		if (padScroll)
			list.SetGamepadEdgePadding(MenuEdgePadding());
		list.GetPadSize();
		Frect clip;
		CUIScrollViewProbe::GetVisibleRect(&list, clip);
		clip = IntersectMenuRects(clip, screen);
		for (u32 i = 0; i < list.GetSize(); ++i)
		{
			auto* item = list.GetItemByIDX(i);
			if (item && item->IsShown() && item->IsEnabled())
			{
				Frect rect;
				item->GetAbsoluteRect(rect);
				add(item, &list, &list, rect, clip, "listbox_item", false, false);
			}
		}
		return;
	}

	Frect footerRect{};
	if (footer[0])
		footer[0]->GetAbsoluteRect(footerRect);

	auto walk = [&](auto& self, CUIWindow* widget, CUIWindow* panel, CUIScrollView* scroll,
					const Frect& clip, int depth) -> void
	{
		if (!widget->IsShown() || !widget->IsEnabled() || depth > 24 || smart_cast<CUIScrollBar*>(widget))
			return;

		if (IsNativeMenuPage(widget))
			panel = widget;
		if (IsNativeMenuPage(panel))
		{
			const auto name = widget->WindowName();
			if (name.size() && strncmp(name.c_str(), "gp_native_row_", 14) == 0)
			{
				Frect bounds;
				widget->GetAbsoluteRect(bounds);
				add(widget, panel, nullptr, bounds, clip, "gp_native_row", false, false);
				return;
			}
			for (CUIWindow* child : widget->GetChildWndList())
				self(self, child, panel, nullptr, clip, depth + 1);
			return;
		}

		if (auto* view = smart_cast<CUIScrollView*>(widget))
		{
			if (padScroll && footer[0])
				view->SetGamepadEdgePadding(IsMenuTabColumn(view) ? 0.0f : MenuEdgePadding());
			view->GetPadSize();
			Frect rect;
			CUIScrollViewProbe::GetVisibleRect(view, rect);
			// Nested lists scroll independently but belong to their enclosing column.
			const Frect nextClip = IntersectMenuRects(clip, rect);
			for (CUIWindow* child : view->GetChildWndList())
				self(self, child, scroll ? panel : view, view, nextClip, depth + 1);
			return;
		}

		LPCSTR kind = nullptr;
		Frect rect;
		widget->GetAbsoluteRect(rect);
		bool activeTab = false;
		bool isFooter = false;
		if (smart_cast<CUIEditKeyBind*>(widget))
			kind = "keybind";
		else if (smart_cast<CUICustomEdit*>(widget))
			kind = "editbox";
		else if (auto* check = smart_cast<CUICheckButton*>(widget))
		{
			kind = "check_btn";
			const CUIStatic* face = check->m_background ? check->m_background->Get(S_Enabled) : nullptr;
			const LPCSTR texture = face ? face->m_TextureName.c_str() : nullptr;
			if (texture && strstr(texture, "TabLED"))
				activeTab = check->GetCheck();
			else if (texture && strstr(texture, "ui_radio"))
			{
				// P13: the focused state is drawn by its own texture (one even ring), no overlay.
				kind = "radio_tex";
				TrimMenuRadioBounds(rect);
			}
			else if (texture && strstr(texture, "ui_inGame2_checkbox"))
			{
				kind = "check_square";
				const float width = rect.width();
				const float height = rect.height();
				rect.x1 += width * 0.307f;
				rect.x2 -= width * 0.307f;
				rect.y1 += height * 0.241f;
				rect.y2 -= height * 0.224f;
			}
			else if (face && check->TextItemControl()->GetText() && check->TextItemControl()->GetText()[0])
			{
				const float width = face->GetTextureRect().width();
				if (width > 4.0f && rect.width() > width * 1.25f)
					rect.x2 = rect.x1 + width;
			}
		}
		else if (smart_cast<CUIRadioButton*>(widget))
		{
			kind = "radio_btn";
			TrimMenuRadioBounds(rect);
		}
		else if (smart_cast<CUITrackBar*>(widget))
			kind = "trackbar";
		else if (auto* box = smart_cast<CUIComboBox*>(widget))
		{
			kind = "combobox";
			CUIComboBoxProbe::GetHeaderRect(box, rect);
		}
		else if (smart_cast<CUIListBoxItem*>(widget))
			kind = "listbox_item";
		else if (auto* tab = smart_cast<CUITabButton*>(widget))
		{
			kind = "tab_btn";
			activeTab = !scroll && tab->GetButtonState() == CUIButton::BUTTON_PUSHED;
		}
		else if (auto* button3t = smart_cast<CUI3tButton*>(widget))
		{
			kind = "dialog_btn";
			isFooter = footer[0] && widget->GetParent() == footer[0]->GetParent() && IsSameMenuRow(rect, footerRect);
			const CUIStatic* face = button3t->m_background ? button3t->m_background->Get(S_Enabled) : nullptr;
			if (!isFooter && face && strstr(face->m_TextureName.c_str(), "ui_button_ordinary"))
			{
				// P15c: ui_button_ordinary_* (117x29 in ui_common.dds) is opaque only at x 3..114, y 1..26;
				// the frame follows the visible plate, not the transparent margin of the window.
				const float width = rect.width();
				const float height = rect.height();
				rect.x1 += width * (3.0f / 117.0f);
				rect.x2 -= width * (2.0f / 117.0f);
				rect.y1 += height * (1.0f / 29.0f);
				rect.y2 -= height * (2.0f / 29.0f);
			}
		}
		else if (smart_cast<CUIButton*>(widget))
			kind = "button";

		if (kind)
		{
			add(widget, panel, scroll, rect, clip, kind, activeTab, isFooter);
			return;
		}
		if (!scroll && (smart_cast<CUITabControl*>(widget) || smart_cast<CUIFrameWindow*>(widget) ||
						smart_cast<CUIFrameLineWnd*>(widget)))
			panel = widget;
		for (CUIWindow* child : widget->GetChildWndList())
			self(self, child, panel, scroll, clip, depth + 1);
	};
	walk(walk, root, root, nullptr, screen, 0);

	// Preset and search controls are siblings of their column's scroll view.
	for (auto& target : targets)
	{
		if (target.panel != root || target.footer)
			continue;
		CUIWindow* column = nullptr;
		float bestWidth = UI_BASE_WIDTH + 1.0f;
		for (const auto& candidate : targets)
		{
			if (!candidate.scroll || candidate.panel->GetParent() != target.widget->GetParent())
				continue;
			Frect bounds;
			candidate.panel->GetAbsoluteRect(bounds);
			const float x = CenterX(target.bounds);
			if (x >= bounds.x1 && x <= bounds.x2 && bounds.width() < bestWidth)
			{
				bestWidth = bounds.width();
				column = candidate.panel;
			}
		}
		if (!column && target.widget->GetParent())
		{
			for (CUIWindow* sibling : target.widget->GetParent()->GetChildWndList())
			{
				auto* view = smart_cast<CUIScrollView*>(sibling);
				if (!view || !view->IsShown() || !view->IsEnabled())
					continue;
				Frect bounds;
				view->GetAbsoluteRect(bounds);
				const float x = CenterX(target.bounds);
				if (x >= bounds.x1 && x <= bounds.x2 && bounds.width() < bestWidth)
				{
					bestWidth = bounds.width();
					column = view;
				}
			}
		}
		if (column)
			target.panel = column;
	}
}

using MenuTargets = xr_vector<SMenuFocusTarget>;
using MenuChoices = xr_vector<const SMenuFocusTarget*>;

const SMenuFocusTarget* FindMenuTarget(const MenuTargets& targets, const CUIWindow* widget)
{
	for (const auto& target : targets)
	{
		if (target.widget == widget)
			return &target;
	}
	return nullptr;
}

MenuChoices GetMenuChoices(const MenuTargets& targets, CUIWindow* panel, bool footer)
{
	MenuChoices choices;
	for (const auto& target : targets)
	{
		if (footer ? target.footer : (!target.footer && target.panel == panel))
			choices.push_back(&target);
	}
	return choices;
}

xr_vector<CUIWindow*> GetMenuPanels(const MenuTargets& targets)
{
	xr_vector<CUIWindow*> panels;
	for (const auto& target : targets)
	{
		if (!target.footer && std::find(panels.begin(), panels.end(), target.panel) == panels.end())
			panels.push_back(target.panel);
	}
	std::stable_sort(panels.begin(), panels.end(), [&](CUIWindow* a, CUIWindow* b)
	{
		Frect ar, br;
		a->GetAbsoluteRect(ar);
		b->GetAbsoluteRect(br);
		return ar.x1 < br.x1;
	});
	return panels;
}

CUIWindow* NextMenuPanel(const xr_vector<CUIWindow*>& panels, CUIWindow* current, int direction)
{
	if (panels.empty())
		return nullptr;
	const auto it = std::find(panels.begin(), panels.end(), current);
	if (it == panels.end())
		return panels.front();
	const int count = static_cast<int>(panels.size());
	return panels[(static_cast<int>(it - panels.begin()) + direction + count) % count];
}

const SMenuFocusTarget* PickMenuEdge(const MenuChoices& choices, float x, bool bottom)
{
	const SMenuFocusTarget* edge = nullptr;
	for (const auto* target : choices)
	{
		if (!edge || (bottom ? CenterY(target->bounds) > CenterY(edge->bounds)
							 : CenterY(target->bounds) < CenterY(edge->bounds)))
			edge = target;
	}
	if (!edge)
		return nullptr;
	const SMenuFocusTarget* best = edge;
	for (const auto* target : choices)
	{
		if (IsSameMenuRow(target->bounds, edge->bounds) &&
			_abs(CenterX(target->bounds) - x) < _abs(CenterX(best->bounds) - x))
			best = target;
	}
	return best;
}

const SMenuFocusTarget* PickMenuEntry(const MenuChoices& choices, const CUIWindow* remembered, int index, bool skipSearch)
{
	for (const auto* target : choices)
		if (target->widget == remembered)
			return target;
	if (index >= 0 && !choices.empty())
		return choices[_min(index, static_cast<int>(choices.size()) - 1)];

	if (!skipSearch)
		return PickMenuEdge(choices, 0.0f, false);
	MenuChoices withoutSearch;
	for (const auto* target : choices)
		if (xr_strcmp(target->kind, "editbox") != 0)
			withoutSearch.push_back(target);
	return PickMenuEdge(withoutSearch.empty() ? choices : withoutSearch, 0.0f, false);
}

LPCSTR MenuTargetLabel(CUIWindow* widget)
{
	if (auto* item = smart_cast<CUIListBoxItem*>(widget))
		return item->GetText();
	if (auto* button = smart_cast<CUIButton*>(widget))
		return button->TextItemControl()->GetText();
	return widget->WindowName().c_str();
}

xr_string MenuPanelKey(const MenuTargets& targets, const xr_vector<CUIWindow*>& panels, CUIWindow* panel)
{
	xr_string key;
	for (CUIWindow* column : panels)
	{
		key += '/';
		if (column == panel)
			break;
		for (const auto& target : targets)
		{
			if (target.panel == column && target.activeTab)
			{
				const LPCSTR label = MenuTargetLabel(target.widget);
				if (label)
					key += label;
				break;
			}
		}
	}
	return key;
}

CUIListBox* MenuSaveList(const MenuFooter& footer)
{
	for (auto* child : footer[0]->GetParent()->GetChildWndList())
		if (auto* list = smart_cast<CUIListBox*>(child))
			return list;
	return nullptr;
}

// MENU-FOCUS P16: LB/RB jump to the previous/next group. A group starts after a list row without any focus
// target (a section title or a divider in Settings and MCM). A list without such rows moves by one screen.
const SMenuFocusTarget* JumpMenuGroup(const MenuChoices& column, const SMenuFocusTarget& current, int direction)
{
	if (CUIScrollView* scroll = current.scroll)
	{
		auto& items = scroll->Items();
		const CUIWindow* pad = items.empty() ? nullptr : items.front()->GetParent();
		auto itemOf = [&](const SMenuFocusTarget* target) -> int
		{
			const CUIWindow* w = target->widget;
			while (w && w->GetParent() != pad)
				w = w->GetParent();
			const auto it = w ? std::find(items.begin(), items.end(), w) : items.end();
			return it == items.end() ? -1 : static_cast<int>(it - items.begin());
		};
		xr_vector<const SMenuFocusTarget*> firstIn(items.size(), nullptr);
		for (const auto* choice : column)
		{
			const int index = choice->scroll == scroll ? itemOf(choice) : -1;
			if (index < 0)
				continue;
			const auto*& first = firstIn[index];
			if (!first || choice->bounds.y1 < first->bounds.y1 - 1.0f ||
				(std::abs(choice->bounds.y1 - first->bounds.y1) <= 1.0f && choice->bounds.x1 < first->bounds.x1))
				first = choice;
		}
		xr_vector<int> starts;
		bool afterTitle = true;
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
		{
			if (!items[i]->IsShown())
				continue;
			if (!firstIn[i])
				afterTitle = true;
			else if (afterTitle)
			{
				starts.push_back(i);
				afterTitle = false;
			}
		}
		const int here = itemOf(&current);
		if (starts.size() > 1 && here >= 0)
		{
			int destination = -1;
			if (direction > 0)
			{
				for (const int start : starts)
					if (start > here)
					{
						destination = start;
						break;
					}
			}
			else
			{
				for (const int start : starts)
					if (start < here)
						destination = start;
			}
			return destination >= 0 ? firstIn[destination] : nullptr;
		}
	}

	// No titles: one visible screen up or down.
	const float step = std::max(current.clip.height(), current.bounds.height());
	const float goal = CenterY(current.bounds) + direction * step;
	const SMenuFocusTarget* best = nullptr;
	float bestDistance = 1e9f;
	for (const auto* choice : column)
	{
		const float dy = CenterY(choice->bounds) - CenterY(current.bounds);
		if (dy * direction <= 1.0f)
			continue;
		const float distance = std::abs(CenterY(choice->bounds) - goal) + std::abs(choice->bounds.x1 - current.bounds.x1) * 0.01f;
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = choice;
		}
	}
	return best;
}

const SMenuFocusTarget* PickMenuStep(const SMenuFocusTarget& current, const MenuChoices& column,
	const MenuChoices& footer, bool horizontal, int direction)
{
	const float x = CenterX(current.bounds);
	const float y = CenterY(current.bounds);
	const auto& choices = current.footer ? footer : column;
	const SMenuFocusTarget* next = nullptr;
	float bestDistance = 1e9f;
	if (horizontal)
	{
		for (const auto* target : choices)
		{
			const float dx = (CenterX(target->bounds) - x) * direction;
			if (dx > 0.5f && dx < bestDistance && IsSameMenuRow(target->bounds, current.bounds))
			{
				bestDistance = dx;
				next = target;
			}
		}
		return next;
	}

	if (current.footer)
		return PickMenuEdge(column, x, direction < 0);

	for (const auto* target : column)
	{
		const float dy = (CenterY(target->bounds) - y) * direction;
		if (dy > 0.0f && dy < bestDistance && !IsSameMenuRow(target->bounds, current.bounds))
		{
			bestDistance = dy;
			next = target;
		}
	}
	if (next)
	{
		const auto* row = next;
		for (const auto* target : column)
		{
			if ((CenterY(target->bounds) - y) * direction > 0.0f &&
			    !IsSameMenuRow(target->bounds, current.bounds) && IsSameMenuRow(target->bounds, row->bounds) &&
			    _abs(CenterX(target->bounds) - x) < _abs(CenterX(next->bounds) - x))
				next = target;
		}
		return next;
	}
	return PickMenuEdge(footer.empty() ? column : footer, x, direction < 0 && footer.empty());
}

CUIWindow* AdjacentMenuPanel(const xr_vector<CUIWindow*>& panels, CUIWindow* current, int direction)
{
	const auto it = std::find(panels.begin(), panels.end(), current);
	if (it == panels.end())
		return nullptr;
	const auto index = (it - panels.begin()) + direction;
	return index >= 0 && index < static_cast<std::ptrdiff_t>(panels.size()) ? panels[index] : nullptr;
}

int MenuRevealDelta(float topGap, float bottomGap, float padding, int direction)
{
	if (topGap < 0.0f || (direction < 0 && topGap < padding))
		return iFloor(topGap - padding);
	if (bottomGap < 0.0f || (direction > 0 && bottomGap < padding))
		return iCeil(padding - bottomGap);
	return 0;
}

void RevealMenuTarget(const SMenuFocusTarget& target, const CUIComboBox* combo, int direction)
{
	for (CUIWindow* parent = target.widget->GetParent(); parent && parent != combo; parent = parent->GetParent())
	{
		auto* view = smart_cast<CUIScrollView*>(parent);
		if (!view)
			continue;
		Frect visible, widget;
		CUIScrollViewProbe::GetVisibleRect(view, visible);
		target.widget->GetAbsoluteRect(widget);
		// Use the focus rectangle (e.g. a checkbox's face), translated by earlier scrolls.
		const float offset = widget.y1 - target.widgetTop;
		const float topGap = target.bounds.y1 + offset - visible.y1;
		const float bottomGap = visible.y2 - target.bounds.y2 - offset;
		float padding = direction && !IsMenuTabColumn(view) ? MenuEdgePadding() : 0.0f;
		padding = std::clamp(padding, 0.0f, std::max(0.0f, visible.height() - target.bounds.height()));
		const int delta = MenuRevealDelta(topGap, bottomGap, padding, direction);
		if (delta)
			view->SetScrollPos(view->GetCurrentScrollPos() + delta);
	}
}
}

void CGamepadContextDispatcher::ApplyMenuFocus(CUIDialogWnd* root, const SMenuFocusTarget& target,
	CUIComboBox* combo, bool reveal, u32 now, int scrollDirection)
{
	if (reveal)
		RevealMenuTarget(target, combo, scrollDirection);

	Frect widget;
	target.widget->GetAbsoluteRect(widget);
	Frect bounds = target.bounds;
	const float offset = widget.y1 - target.widgetTop;
	bounds.y1 += offset;
	bounds.y2 += offset;
	const Frect visible = IntersectMenuRects(bounds, GetMenuTargetClip(target, combo));
	m_bVcTargetHit = IsMenuRectVisible(bounds, visible);
	if (!m_bVcTargetHit)
		return;

	const bool targetChanged = m_menuTarget != target.widget;
	m_menuTarget = target.widget;
	// INV-REWORK-4 (load window): the highlighted save row follows the frame, so one A loads it.
	// Only the load window: in the save window choosing a row means overwriting it.
	if (targetChanged && !combo)
	{
		if (auto* row = smart_cast<CUIListBoxItem*>(target.widget))
		{
			bool loading = false;
			const MenuFooter footer = FindMenuFooter(root, &loading);
			auto* list = loading && !IsMenuSaveFooter(footer) ? MenuSaveList(footer) : nullptr;
			if (list && row->GetParent() && row->GetParent()->GetParent() == list && list->GetSelectedItem() != row)
			{
				row->OnMouseDown(MOUSE_1);
				m_menuLastSaveRow[0] = row->GetText();
			}
		}
	}
	m_menuFocusOnTrackbar = !combo && smart_cast<CUITrackBar*>(target.widget) != nullptr;
	if (!target.footer && !combo)
		m_menuPanel = target.panel;

	m_vcTargetCx = CenterX(visible);
	m_vcTargetCy = CenterY(visible);
	m_vcTargetW = visible.width();
	m_vcTargetH = visible.height();
	m_vcTargetKind = target.kind;
	const auto radio = !combo ? GetHorizontalMenuRadio(target) : SHorizontalMenuRadio{};
	if (radio.group != m_menuRadioGroup)
	{
		m_menuRadioGroup = radio.group;
		m_menuRadioEntered = now;
		m_menuRadioAtRing = false;
	}
	if (radio.group && (target.widget != radio.first ||
		(radio.hasSelection && now - m_menuRadioEntered >= MenuRadioCaptionMs)))
		m_menuRadioAtRing = true;
	m_menuCaretValid = !combo && ComputeMenuCaretAnchor(target, visible, m_menuCaretAnchor, m_menuCaretGapPx,
		radio, m_menuRadioAtRing);
	m_menuTransitionPanel = !combo && !target.footer &&
		(m_menuCaretValid || IsMenuTab(target.widget)) ? target.panel : nullptr;
	const Fvector2 previous = GetUICursor().GetCursorPosition();
	if (!fsimilar(previous.x, m_vcTargetCx) || !fsimilar(previous.y, m_vcTargetCy))
	{
		if (pInput)
			pInput->NotifyGamepadCursorMoved(now);
		GetUICursor().SetUICursorPosition(Fvector2().set(m_vcTargetCx, m_vcTargetCy));
		root->OnMouseAction(m_vcTargetCx, m_vcTargetCy, WINDOW_MOUSE_MOVE);
		++m_vcMoveCount;
	}
	if (targetChanged && xr_strcmp(target.kind, "gp_native_row") == 0)
		SelectNativeMenuRow(target.widget->WindowName().c_str());
}

bool CGamepadContextDispatcher::HasMenuFocus(const CUIWindow* widget) const
{
	return widget && widget == m_menuTarget && m_menuFocusRoot && IsCursorOverInteractiveTarget() &&
		(!m_menuRadioGroup || m_menuRadioAtRing) &&
		IsMenuFocusMode() &&
		((MainMenu() && MainMenu()->IsActive() && MainMenu()->TopInputReceiver() == m_menuFocusRoot) ||
		 (CurrentGameUI() && IsLightingDialog(CurrentGameUI()->TopInputReceiver()) &&
		  CurrentGameUI()->TopInputReceiver() == m_menuFocusRoot));
}

bool CGamepadContextDispatcher::GetMenuFocusDraw(SMenuFocusDraw& out) const
{
	const LPCSTR kind = GetCursorTargetKind();
	out.transitionRoot = m_menuTransitionPanel ? m_menuFocusRoot : nullptr;
	out.transitionPanel = m_menuTransitionPanel;
	out.caretKey = m_menuTarget;
	if (m_bVcTargetHit && m_menuCaretValid)
	{
		// P14: settings rows get the breathing caret instead of the frame; checkbox/slider/radio keep their
		// own lit textures (CUI3tButton::DrawTexture).
		out.shape = SMenuFocusDraw::Shape::Caret;
		out.frame.set(m_menuCaretAnchor.x, m_menuCaretAnchor.y, m_menuCaretAnchor.x, m_menuCaretAnchor.y);
		out.caretGapPx = m_menuCaretGapPx;
		return true;
	}
	if (!m_bVcTargetHit || xr_strcmp(kind, "trackbar") == 0 || xr_strcmp(kind, "check_square") == 0 ||
		xr_strcmp(kind, "radio_tex") == 0)
		return false;
	out.frame.set(m_vcTargetCx - m_vcTargetW * 0.5f, m_vcTargetCy - m_vcTargetH * 0.5f,
		m_vcTargetCx + m_vcTargetW * 0.5f, m_vcTargetCy + m_vcTargetH * 0.5f);
	if (xr_strcmp(kind, "radio_btn") == 0)
	{
		out.shape = SMenuFocusDraw::Shape::Ellipse;
		out.frame.set(out.frame.x1 - 1, out.frame.y1 - 1, out.frame.x2 + 1, out.frame.y2 + 1);
	}
	return m_vcTargetW > 4.0f && m_vcTargetH > 4.0f;
}

// MENU-FOCUS-5 P14: layout scan (-dbg, console "gp_menu_layout_scan" with Settings or MCM open).
// Walks the option tree depth-first by pressing the tree check buttons (same path as a mouse click:
// CUICheckButton::OnMouseDown -> BUTTON_CLICKED -> UIOptions/UI_MCM:Callback_Tree -> Reset_opt), then measures
// every row of the shown page in absolute UI coordinates (offscreen rows included, no scrolling needed).
namespace
{
bool IsMenuWindowVisible(CUIWindow* w)
{
	for (; w; w = w->GetParent())
		if (!w->IsShown())
			return false;
	return true;
}

void CollectMenuScrolls(CUIWindow* w, xr_vector<CUIScrollView*>& out, int depth = 0)
{
	if (!w || !w->IsShown() || depth > 24)
		return;
	if (auto* view = smart_cast<CUIScrollView*>(w))
	{
		out.push_back(view);
		return;
	}
	for (CUIWindow* child : w->GetChildWndList())
		CollectMenuScrolls(child, out, depth + 1);
}

CUICheckButton* GetMenuTreeRowButton(CUIWindow* row)
{
	for (CUIWindow* child : row->GetChildWndList())
		if (auto* check = smart_cast<CUICheckButton*>(child))
			return check;
	return nullptr;
}

LPCSTR GetMenuTreeRowText(CUIWindow* row)
{
	for (CUIWindow* child : row->GetChildWndList())
		if (auto* text = smart_cast<CUITextWnd*>(child))
			return text->GetText();
	return "?";
}

void FindMenuScanColumns(CUIWindow* root, xr_vector<CUIScrollView*>& columns, CUIScrollView*& settings)
{
	xr_vector<CUIScrollView*> all;
	CollectMenuScrolls(root, all);
	settings = nullptr;
	columns.clear();
	for (CUIScrollView* view : all)
	{
		if (!IsMenuWindowVisible(view) || smart_cast<CUIListBox*>(view))
			continue;
		if (IsMenuSettingsScroll(view))
		{
			if (!settings)
				settings = view;
			continue;
		}
		bool tree = !view->Items().empty();
		for (CUIWindow* row : view->Items())
			tree = tree && GetMenuTreeRowButton(row);
		if (tree)
			columns.push_back(view);
	}
	std::sort(columns.begin(), columns.end(), [](CUIScrollView* a, CUIScrollView* b) {
		Frect ra, rb;
		a->GetAbsoluteRect(ra);
		b->GetAbsoluteRect(rb);
		return ra.x1 < rb.x1;
	});
}

CUIWindow* GetMenuColumnRow(CUIScrollView* column, int index)
{
	int i = 0;
	for (CUIWindow* row : column->Items())
		if (i++ == index)
			return row;
	return nullptr;
}

int GetMenuColumnChecked(CUIScrollView* column)
{
	int i = 0;
	for (CUIWindow* row : column->Items())
	{
		CUICheckButton* check = GetMenuTreeRowButton(row);
		if (check && check->GetCheck())
			return i;
		++i;
	}
	return 0;
}

void PressMenuTreeRow(CUIScrollView* column, int index)
{
	CUIWindow* row = GetMenuColumnRow(column, index);
	CUICheckButton* check = row ? GetMenuTreeRowButton(row) : nullptr;
	if (check && !check->GetCheck())
		check->OnMouseDown(MOUSE_1);
}

// Visible center of a settings control, same trims as GatherMenuFocusTargets (P12/P13).
bool GetMenuControlCenter(CUIWindow* w, LPCSTR& kind, float& cy)
{
	Frect r;
	w->GetAbsoluteRect(r);
	kind = nullptr;
	if (auto* check = smart_cast<CUICheckButton*>(w))
	{
		const CUIStatic* face = check->m_background ? check->m_background->Get(S_Enabled) : nullptr;
		const LPCSTR tex = face ? face->m_TextureName.c_str() : "";
		if (strstr(tex, "ui_radio"))
		{
			kind = "radio";
			TrimMenuRadioBounds(r);
		}
		else if (strstr(tex, "ui_inGame2_checkbox"))
		{
			kind = "check";
			const float h = r.height();
			r.y1 += h * 0.241f;
			r.y2 -= h * 0.224f;
		}
		else
			kind = "check_other";
	}
	else if (smart_cast<CUITrackBar*>(w))
		kind = "trackbar";
	else if (auto* box = smart_cast<CUIComboBox*>(w))
	{
		kind = "list";
		CUIComboBoxProbe::GetHeaderRect(box, r);
	}
	else if (smart_cast<CUICustomEdit*>(w))
		kind = "input";
	else if (smart_cast<CUIButton*>(w))
		kind = "button";
	if (!kind)
		return false;
	cy = (r.y1 + r.y2) * 0.5f;
	return true;
}

void CollectMenuRowControls(CUIWindow* w, xr_vector<CUIWindow*>& out, int depth = 0)
{
	for (CUIWindow* child : w->GetChildWndList())
	{
		if (!child->IsShown() || depth > 8)
			continue;
		LPCSTR kind;
		float cy;
		if (GetMenuControlCenter(child, kind, cy))
			out.push_back(child);
		else
			CollectMenuRowControls(child, out, depth + 1);
	}
}
}

void CGamepadContextDispatcher::StartMenuLayoutScan()
{
	CUIDialogWnd* root = MainMenu() && MainMenu()->IsActive() ? MainMenu()->TopInputReceiver() : nullptr;
	xr_vector<CUIScrollView*> columns;
	CUIScrollView* settings = nullptr;
	if (root)
		FindMenuScanColumns(root, columns, settings);
	if (!root || columns.empty())
	{
		Msg("! gp_menu_layout_scan: open Settings or MCM first (tree columns=%u)", u32(columns.size()));
		return;
	}
	m_menuScan = {};
	m_menuScan.active = true;
	m_menuScan.root = root;
	m_menuScan.waitUntil = Device.dwTimeContinual;
	string512 line;
	xr_sprintf(line, sizeof(line),
		"; gp_menu_layout_scan (MENU-FOCUS-5 P14), screen %ux%u, threshold 1.5 px; text cp1251\r\n"
		"; page | caption | control | lines | caption_cy control_cy (UI) | dy UI | dy screen px | dy px@1079\r\n",
		Device.dwWidth, Device.dwHeight);
	m_menuScan.log = line;
	Msg("- gp_menu_layout_scan: started, %u tree columns", u32(columns.size()));
}

void CGamepadContextDispatcher::TickMenuLayoutScan()
{
	auto& scan = m_menuScan;
	if (!scan.active)
		return;
	const u32 now = Device.dwTimeContinual;
	auto finish = [&](LPCSTR why) {
		string512 tail;
		xr_sprintf(tail, sizeof(tail), "; %s: pages=%d rows=%d issues=%d\r\n", why, scan.pages, scan.rows, scan.issues);
		scan.log += tail;
		string_path fn;
		FS.update_path(fn, "$app_data_root$", "gamepad\\menu_layout_scan.log");
		if (IWriter* w = FS.w_open(fn))
		{
			w->w(scan.log.c_str(), u32(scan.log.size()));
			FS.w_close(w);
		}
		Msg("- gp_menu_layout_scan: %s, pages=%d rows=%d issues=%d -> %s", why, scan.pages, scan.rows, scan.issues, fn);
		scan.active = false;
	};
	if (!MainMenu() || !MainMenu()->IsActive() || MainMenu()->TopInputReceiver() != scan.root)
	{
		finish("aborted (window closed)");
		return;
	}
	if (now < scan.waitUntil)
		return;
	xr_vector<CUIScrollView*> columns;
	CUIScrollView* settings = nullptr;
	FindMenuScanColumns(scan.root, columns, settings);
	if (columns.empty())
	{
		finish("aborted (tree lost)");
		return;
	}
	constexpr u32 settle = 90;
	if (scan.path.empty())
	{
		// Force a fresh Callback_Tree for the first page even if it is already checked.
		const int count = int(columns[0]->Items().size());
		PressMenuTreeRow(columns[0], count > 1 ? 1 : 0);
		scan.path.push_back(-1);
		scan.waitUntil = now + settle;
		return;
	}
	if (scan.path[0] < 0)
	{
		PressMenuTreeRow(columns[0], 0);
		scan.path[0] = 0;
		scan.waitUntil = now + settle;
		return;
	}
	if (scan.path.size() > columns.size())
		scan.path.resize(columns.size());
	while (scan.path.size() < columns.size())
		scan.path.push_back(GetMenuColumnChecked(columns[scan.path.size()]));

	// Measure the shown page.
	xr_string page;
	for (size_t level = 0; level < scan.path.size(); ++level)
	{
		if (CUIWindow* row = GetMenuColumnRow(columns[level], scan.path[level]))
		{
			if (level)
				page += " / ";
			page += GetMenuTreeRowText(row);
		}
	}
	++scan.pages;
	const float toScreen = float(Device.dwHeight) / UI_BASE_HEIGHT;
	const float toRef = 1079.0f / UI_BASE_HEIGHT;
	if (settings)
	{
		for (CUIWindow* row : settings->Items())
		{
			CUIStatic* cap = FindMenuRowCaption(row);
			if (!cap)
				continue;
			++scan.rows;
			float left, capCy;
			int lines;
			GetMenuCaptionText(cap, left, capCy, lines);
			xr_vector<CUIWindow*> controls;
			CollectMenuRowControls(row, controls);
			bool radioSeen = false;
			for (CUIWindow* control : controls)
			{
				LPCSTR kind;
				float cy;
				GetMenuControlCenter(control, kind, cy);
				if (xr_strcmp(kind, "radio") == 0)
				{
					// Only the first ring sits on the caption line (vertical groups stack downwards).
					if (radioSeen)
						continue;
					radioSeen = true;
				}
				const float dy = cy - capCy;
				if (_abs(dy * toScreen) <= 1.5f)
					continue;
				++scan.issues;
				string1024 line;
				xr_sprintf(line, sizeof(line), "%s | %s | %s | %d | %.2f %.2f | %+.2f | %+.1f | %+.1f\r\n", page.c_str(),
					cap->TextItemControl()->GetText(), kind, lines, capCy, cy, dy, dy * toScreen, dy * toRef);
				scan.log += line;
			}
		}
	}

	// Advance depth-first.
	for (int level = int(scan.path.size()) - 1; level >= 0; --level)
	{
		const int count = int(columns[level]->Items().size());
		if (scan.path[level] + 1 < count)
		{
			++scan.path[level];
			scan.path.resize(level + 1);
			PressMenuTreeRow(columns[level], scan.path[level]);
			scan.waitUntil = now + settle;
			return;
		}
	}
	finish("done");
}

bool CGamepadContextDispatcher::StepMenuFocus(CUIDialogWnd* root, ENavDir dir, int panelStep, int wheel, u32 now,
                                              EMenuFocusAction action)
{
	const bool changedWindow = m_menuFocusRoot != root;
	if (changedWindow)
	{
		m_menuFocusRoot = root;
		m_menuTarget = m_menuPanel = nullptr;
		m_menuOpenCombo = nullptr;
		m_menuRadioGroup = nullptr;
		m_menuRadioAtRing = false;
	}
	m_vcLastTargetQueryTime = now;
	m_bVcTargetHit = false;
	m_menuCaretValid = false;
	m_menuFocusOnTrackbar = false;

	bool loading = false;
	const MenuFooter footer = FindMenuFooter(root, &loading);
	if (!loading && m_menuDeleteIndex >= 0 && root != m_menuLoadRoot)
		m_menuDeleteModalSeen = true;
	if (footer[0])
	{
		if (m_menuMemoryRoot != root)
			m_menuPanelMemory.clear();
		std::copy(footer.begin(), footer.end(), m_menuShortcutButtons);
		m_menuMemoryRoot = root;
	}
	static CUIDialogWnd* s_nativeWestRoot = nullptr;
	static bool s_nativeWestHeld = false;
	static CUIDialogWnd* s_rsScrollRoot = nullptr;
	static u32 s_rsLastMs = 0;
	static float s_rsPixelAccum = 0.0f;
	if (changedWindow || (s_nativeWestRoot && s_nativeWestRoot != root))
	{
		if (s_nativeWestHeld)
		{
			s_nativeWestHeld = false;
			NativeMenuButton("X", "up", now);
		}
		s_nativeWestRoot = root;
	}
	if (changedWindow || s_rsScrollRoot != root)
	{
		s_rsScrollRoot = root;
		s_rsLastMs = now;
		s_rsPixelAccum = 0.0f;
	}
	const float rsDt = (now > s_rsLastMs) ? _min(0.06f, static_cast<float>(now - s_rsLastMs) * 0.001f) : 0.0f;
	s_rsLastMs = now;

	auto* combo = GetExpandedMenuCombo(root);
	// W5-OPTIONS (B16): header Preset combobox in UIOptions steps Left/Right without expanding dropdown.
	if (combo && footer[0] && !loading && combo->GetParent() == footer[0]->GetParent())
	{
		CUIComboBoxProbe::Close(combo);
		combo = nullptr;
	}
	auto* previousCombo = m_menuOpenCombo;
	m_menuOpenCombo = combo;

	MenuTargets targets;
	GatherMenuFocusTargets(root, combo, footer, targets, true);
	if (IsLightingDialog(root))
		targets.erase(std::remove_if(targets.begin(), targets.end(), [](const SMenuFocusTarget& target) {
			return smart_cast<CUITrackBar*>(target.widget) == nullptr;
		}), targets.end());
	const auto panels = GetMenuPanels(targets);
	const auto footerChoices = GetMenuChoices(targets, nullptr, true);
	const auto* current = FindMenuTarget(targets, m_menuTarget);
	bool reveal = false;

	if (combo && combo != previousCombo)
	{
		current = FindMenuTarget(targets, combo->m_list_box.GetSelectedItem());
		reveal = true;
	}
	else if (!combo && previousCombo)
	{
		current = FindMenuTarget(targets, previousCombo);
		reveal = true;
	}

	if (!current && !combo && !changedWindow && !footer[0])
	{
		const Fvector2 pos = GetUICursor().GetCursorPosition();
		for (const auto& target : targets)
		{
			const Frect visible = IntersectMenuRects(target.bounds, target.clip);
			if (IsMenuRectVisible(target.bounds, visible) && visible.in(pos.x, pos.y) &&
				(!current || target.bounds.width() * target.bounds.height() <
							 current->bounds.width() * current->bounds.height()))
				current = &target;
		}
	}
	if (loading)
	{
		if (m_menuLoadRoot != root)
		{
			m_menuLoadRoot = root;
			m_menuLoadFooterIndex = 0;
			m_menuLoadArmed = nullptr;
			m_menuDeleteIndex = -1;
			// MENU-FOCUS P16: open on the row chosen last time in this window kind.
			const shared_str& last = m_menuLastSaveRow[IsMenuSaveFooter(footer) ? 1 : 0];
			auto* list = last.size() ? MenuSaveList(footer) : nullptr;
			for (u32 i = 0; list && i < list->GetSize(); ++i)
			{
				auto* row = list->GetItemByIDX(static_cast<int>(i));
				if (row && xr_strcmp(row->GetText(), last.c_str()) == 0)
				{
					if (const auto* remembered = FindMenuTarget(targets, row))
					{
						current = remembered;
						reveal = true;
					}
					break;
				}
			}
		}
		if (m_menuDeleteIndex >= 0)
		{
			if (auto* list = MenuSaveList(footer))
			{
				const auto size = list->GetSize();
				if (size != m_menuDeleteCount || m_menuDeleteModalSeen)
				{
					auto* row = size ? list->GetItemByIDX(_min(m_menuDeleteIndex, static_cast<int>(size) - 1)) : nullptr;
					current = FindMenuTarget(targets, row ? static_cast<CUIWindow*>(row) : footer[2]);
					m_menuLoadArmed = nullptr;
					m_menuDeleteIndex = -1;
					reveal = true;
				}
			}
		}
	}
	CUIWindow* panel = current && !current->footer ? current->panel : NextMenuPanel(panels, m_menuPanel, 0);
	auto column = GetMenuChoices(targets, panel, false);
	auto splitScrollHeader = [](CUIWindow* destination, const MenuChoices& choices,
	                            MenuChoices& headerChoices, MenuChoices& bodyChoices)
	{
		if (!smart_cast<CUIScrollView*>(destination))
			return false;
		for (const auto* choice : choices)
			(choice->scroll ? bodyChoices : headerChoices).push_back(choice);
		return !headerChoices.empty() && !bodyChoices.empty();
	};
	auto entry = [&](CUIWindow* destination, const MenuChoices& choices)
	{
		const auto it = m_menuPanelMemory.find(MenuPanelKey(targets, panels, destination));
		const auto* remembered = it != m_menuPanelMemory.end() ? it->second.widget : nullptr;
		const int index = it != m_menuPanelMemory.end() ? it->second.index : -1;
		MenuChoices headerChoices, bodyChoices;
		if (!remembered && splitScrollHeader(destination, choices, headerChoices, bodyChoices))
			return PickMenuEntry(bodyChoices, nullptr, -1, footer[0] && !loading);
		return PickMenuEntry(choices, remembered, index, footer[0] && !loading);
	};
	if (!current)
	{
		current = entry(panel, column);
		if (!current)
			current = loading ? FindMenuTarget(targets, footer[2]) : PickMenuEdge(footerChoices, 0.0f, false);
		reveal = true;
	}
	if (!current)
		return false;

	auto apply = [&](const SMenuFocusTarget& candidate, bool show, int scrollDirection = 0)
	{
		const auto radio = !combo ? GetHorizontalMenuRadio(candidate) : SHorizontalMenuRadio{};
		const auto* first = radio.group && radio.group != m_menuRadioGroup ?
			FindMenuTarget(targets, radio.first) : nullptr;
		const auto& target = first ? *first : candidate;
		if (!target.footer && !combo && footer[0])
		{
			const auto choices = GetMenuChoices(targets, target.panel, false);
			const auto idx = std::find(choices.begin(), choices.end(), &target) - choices.begin();
			m_menuPanelMemory[MenuPanelKey(targets, panels, target.panel)] = {target.widget, static_cast<int>(idx)};
		}
		if (loading)
		{
			if (target.widget != m_menuTarget)
				m_menuLoadArmed = nullptr;
			if (target.footer)
				for (int i = 0; i < 3; ++i)
					if (target.widget == footer[i])
						m_menuLoadFooterIndex = i;
		}
		ApplyMenuFocus(root, target, combo, show, now, scrollDirection);
	};

	if (action != EMenuFocusAction::Navigate)
	{
		if (!footer[0] || combo)
			return false;

		const SMenuFocusTarget* next = nullptr;
		if (action == EMenuFocusAction::View)
			next = current->footer ? entry(panel, column) :
				FindMenuTarget(targets, footer[loading ? m_menuLoadFooterIndex : 0]);
		else if (action == EMenuFocusAction::Back && !loading)
		{
			if (current->footer)
				next = entry(panel, column);
			else if (auto* previous = AdjacentMenuPanel(panels, panel, -1))
				next = entry(previous, GetMenuChoices(targets, previous, false));
		}
		else if (action == EMenuFocusAction::Enter && !previousCombo && current->widget == m_menuTarget &&
			!current->footer && current->activeTab)
		{
			if (auto* following = AdjacentMenuPanel(panels, panel, 1))
			{
				// The Gamepad entry already opens its selected native page.
				auto* page = AdjacentMenuPanel(panels, following, 1);
				if (page && IsNativeMenuPage(page) && !IsNativeMenuPage(following))
					following = page;
				next = entry(following, GetMenuChoices(targets, following, false));
			}
		}
		else if (action == EMenuFocusAction::Enter && xr_strcmp(current->kind, "editbox") == 0)
		{
			// W5-OPTIONS (B16): pressing A on numeric/text input opens OSK immediately.
			if (auto* edit = smart_cast<CUICustomEdit*>(current->widget))
			{
				if (!CGamepadOnScreenKeyboardUI::Instance().IsOpen())
					CGamepadOnScreenKeyboardUI::Instance().OpenForEditBox(edit, "options_enter");
			}
		}
		apply(next ? *next : *current, next || reveal);
		return next != nullptr;
	}

	// W5-OPTIONS (B4): track X (GP_BTN_WEST) hold/release on native gamepad tabs for 0.3 + 0.7 s default reset.
	const bool onNativeRow = !combo && xr_strcmp(current->kind, "gp_native_row") == 0;
	const bool westDown = onNativeRow && pInput && pInput->IsGamepadActiveSource() &&
	                      (m_rawDownMask & (1u << CInput::GP_BTN_WEST)) != 0;
	if (westDown)
	{
		s_nativeWestHeld = true;
		s_nativeWestRoot = root;
		NativeMenuButton("X", "hold", now);
	}
	else if (s_nativeWestHeld)
	{
		s_nativeWestHeld = false;
		NativeMenuButton("X", "up", now);
	}

	float rsStickY = 0.0f;
	if (!combo && pInput && pInput->IsGamepadActiveSource())
	{
		const auto& gp = pInput->GetGamepadState();
		if (gp.connected && gp.has_focus && !gp.need_release_all)
		{
			const auto stick = gpmath::ProcessStick(gpmath::GetCalibrationProfile().ui_stick,
				gpmath::NormalizeStickAxis(gp.axes[2]), gpmath::NormalizeStickAxis(gp.axes[3]));
			if (_abs(stick.y) >= 0.08f)
				rsStickY = stick.y;
		}
	}
	if (rsStickY == 0.0f)
		s_rsPixelAccum = 0.0f;

	int scrollDirection = 0;
	auto* track = combo ? nullptr : smart_cast<CUITrackBar*>(current->widget);
	if (panelStep && !combo && panels.size() > 1)
	{
		panel = NextMenuPanel(panels, panel, panelStep);
		current = entry(panel, GetMenuChoices(targets, panel, false));
		reveal = true;
	}
	else if (track &&
	         (IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER) || IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER)))
	{
		const bool lower = IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER);
		const bool higher = IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER);
		if (lower)
			ConsumeButtonUntilRelease(CInput::GP_BTN_LEFT_SHOULDER);
		if (higher)
			ConsumeButtonUntilRelease(CInput::GP_BTN_RIGHT_SHOULDER);
		apply(*current, reveal);
		if (lower != higher)
			track->StepValue(higher ? 1 : -1, true);
		return false;
	}
	else if (!combo && !current->footer && xr_strcmp(current->kind, "gp_native_row") != 0 &&
	         (IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER) || IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER)))
	{
		const bool lower = IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER);
		const bool higher = IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER);
		if (lower)
			ConsumeButtonUntilRelease(CInput::GP_BTN_LEFT_SHOULDER);
		if (higher)
			ConsumeButtonUntilRelease(CInput::GP_BTN_RIGHT_SHOULDER);
		const auto* next = lower != higher ? JumpMenuGroup(column, *current, higher ? 1 : -1) : nullptr;
		apply(next ? *next : *current, true);
		return next != nullptr;
	}
	else if (dir != ENavDir::None)
	{
		const bool horizontal = dir == ENavDir::Left || dir == ENavDir::Right;
		const int direction = (dir == ENavDir::Right || dir == ENavDir::Down) ? 1 : -1;
		if (!combo && xr_strcmp(current->kind, "gp_native_row") == 0)
		{
			const auto button = horizontal ? (direction > 0 ? "DR" : "DL") : (direction > 0 ? "DD" : "DU");
			if (NativeMenuButton(button, "tap", now) && !horizontal)
			{
				const auto selected = SelectedNativeMenuRow();
				targets.clear();
				GatherMenuFocusTargets(root, nullptr, footer, targets);
				for (const auto& target : targets)
				{
					if (xr_strcmp(target.kind, "gp_native_row") == 0 &&
						target.widget->WindowName() == selected)
					{
						ApplyMenuFocus(root, target, nullptr, false, now, direction);
						return false;
					}
				}
			}
			else
			{
				apply(*current, false);
			}
			return false;
		}
		if (horizontal && !combo)
		{
			const auto radio = GetHorizontalMenuRadio(*current);
			if (radio.group)
			{
				if (!m_menuRadioAtRing)
				{
					if (direction > 0)
						m_menuRadioAtRing = true;
					apply(*current, false);
					return false;
				}
				MenuChoices siblings;
				for (const auto* target : column)
					if (target->widget->GetParent() == radio.group && IsMenuRadioTexture(target->widget))
						siblings.push_back(target);
				const auto* next = PickMenuStep(*current, siblings, {}, true, direction);
				apply(next ? *next : *current, next != nullptr);
				return false;
			}
			if (track)
			{
				apply(*current, reveal);
				track->StepValue(direction);
				return false;
			}
			if (auto* box = smart_cast<CUIComboBox*>(current->widget))
			{
				// W5-OPTIONS (B16): UIOptions:Register_Preset sets text without enable_id; sync selected item by text.
				auto& list = box->m_list_box;
				if (list.GetSelectedIDX() >= list.GetSize() && box->GetText() && box->GetText()[0])
				{
					for (u32 i = 0; i < list.GetSize(); ++i)
					{
						if (auto* item = list.GetItemByIDX(static_cast<int>(i)))
						{
							if (item->GetText() && xr_strcmp(item->GetText(), box->GetText()) == 0)
							{
								list.SetSelected(item);
								break;
							}
						}
					}
				}
				apply(*current, reveal);
				CUIComboBoxProbe::StepClosed(box, direction);
				return false;
			}
		}
		const bool firstPanel = !panels.empty() && panels.front() == panel;
		const MenuChoices noFooter;
		const auto& navigationFooter = firstPanel || current->footer ? footerChoices : noFooter;
		const SMenuFocusTarget* next = nullptr;
		MenuChoices headerChoices, bodyChoices;
		if (!horizontal && !combo && !current->footer &&
			splitScrollHeader(panel, column, headerChoices, bodyChoices))
		{
			auto hasVerticalStep = [](const SMenuFocusTarget& from, const MenuChoices& pool, int stepDir)
			{
				const float fy = CenterY(from.bounds);
				for (const auto* item : pool)
					if ((CenterY(item->bounds) - fy) * stepDir > 0.0f && !IsSameMenuRow(item->bounds, from.bounds))
						return true;
				return false;
			};
			const float x = CenterX(current->bounds);
			if (current->scroll)
			{
				if (hasVerticalStep(*current, bodyChoices, direction))
					next = PickMenuStep(*current, bodyChoices, noFooter, false, direction);
				else if (direction < 0)
				{
					next = PickMenuEdge(headerChoices, x, true);
					current->scroll->SetScrollPos(current->scroll->GetMinScrollPos());
				}
				else
				{
					next = !navigationFooter.empty()
						? PickMenuEdge(navigationFooter, x, false)
						: PickMenuEdge(headerChoices, x, false);
				}
			}
			else
			{
				if (hasVerticalStep(*current, headerChoices, direction))
					next = PickMenuStep(*current, headerChoices, noFooter, false, direction);
				else if (direction > 0)
					next = PickMenuEdge(bodyChoices, x, false);
				else
					next = PickMenuEdge(bodyChoices, x, true);
			}
		}
		else
		{
			next = PickMenuStep(*current, column, navigationFooter, horizontal, direction);
		}
		if (!next && horizontal && direction > 0 && !combo && !current->footer &&
			footer[0] && panels.size() > 2 && panel == panels[panels.size() - 2])
		{
			auto* following = AdjacentMenuPanel(panels, panel, 1);
			next = entry(following, GetMenuChoices(targets, following, false));
		}
		if (next)
		{
			current = next;
			reveal = true;
			m_vcLastDpadSnapDir = dir;
			++m_vcDpadSnapCount;
			scrollDirection = horizontal ? 0 : direction;
		}
	}
	else if (onNativeRow && rsStickY != 0.0f && rsDt > 0.0f)
	{
		s_rsPixelAccum += rsStickY * 520.0f * rsDt;
		const int pixelDelta = static_cast<int>(s_rsPixelAccum);
		if (pixelDelta != 0)
		{
			s_rsPixelAccum -= static_cast<float>(pixelDelta);
			try
			{
				luabind::functor<bool> scrollFn;
				if (ai().script_engine().functor("ui_gp_native.menu_scroll_by", scrollFn) && scrollFn(pixelDelta))
				{
					const auto selected = SelectedNativeMenuRow();
					targets.clear();
					GatherMenuFocusTargets(root, nullptr, footer, targets);
					current = nullptr;
					for (const auto& target : targets)
					{
						if (xr_strcmp(target.kind, "gp_native_row") == 0 &&
							target.widget->WindowName() == selected)
						{
							current = &target;
							break;
						}
					}
					reveal = false;
				}
			}
			catch (...)
			{
			}
		}
	}
	else if ((wheel || (rsStickY != 0.0f && rsDt > 0.0f)) && !current->footer)
	{
		CUIScrollView* view = current->scroll;
		if (!view || view->GetPadSize().y <= view->GetHeight())
			view = smart_cast<CUIScrollView*>(panel);
		if (view && view->GetPadSize().y > view->GetHeight())
		{
			const float x = CenterX(current->bounds);
			const float y = CenterY(current->bounds);
			CUIWindow* focus = current->widget;
			const int previousScroll = view->GetCurrentScrollPos();
			if (rsStickY != 0.0f && rsDt > 0.0f)
			{
				s_rsPixelAccum += rsStickY * 520.0f * rsDt;
				const int pixelDelta = static_cast<int>(s_rsPixelAccum);
				if (pixelDelta != 0)
				{
					s_rsPixelAccum -= static_cast<float>(pixelDelta);
					view->SetScrollPos(previousScroll + pixelDelta);
				}
			}
			else if (wheel > 0)
				view->ScrollBar()->TryScrollDec(true);
			else
				view->ScrollBar()->TryScrollInc(true);

			if (view->GetCurrentScrollPos() != previousScroll)
			{
				++m_vcScrollCount;
				targets.clear();
				GatherMenuFocusTargets(root, combo, footer, targets);
				current = FindMenuTarget(targets, focus);
				if (!current || !IsMenuRectVisible(current->bounds, IntersectMenuRects(current->bounds, current->clip)))
				{
					current = nullptr;
					float bestDistance = 1e9f;
					for (const auto* candidate : GetMenuChoices(targets, panel, false))
					{
						if (!IsMenuRectVisible(candidate->bounds, IntersectMenuRects(candidate->bounds, candidate->clip)))
							continue;
						const float dx = CenterX(candidate->bounds) - x;
						const float dy = CenterY(candidate->bounds) - y;
						const float distance = dx * dx + dy * dy;
						if (distance < bestDistance)
						{
							bestDistance = distance;
							current = candidate;
						}
					}
				}
				reveal = false;
			}
		}
	}

	if (current)
		apply(*current, reveal, scrollDirection);
	return false;
}

bool CGamepadContextDispatcher::HandleMenuCaptureInput(u32 now)
{
	auto* root = MainMenu() && MainMenu()->IsActive() ? MainMenu()->TopInputReceiver() : nullptr;
	if (auto* key = CUIEditKeyBind::ActiveGamepadEdit(root))
	{
		if (IsButtonPressed(CInput::GP_BTN_EAST))
			key->CancelGamepadEdit();
		ConsumeAllCurrentlyHeldButtons();
		return true;
	}
	if (!root || !IsMenuFocusMode() || root != m_menuFocusRoot ||
		xr_strcmp(GetCursorTargetKind(), "gp_native_row") != 0 || !NativeMenuCaptureActive())
	{
		m_menuNativeCaptureButtons = 0;
		return false;
	}

	for (u32 bit = 0; bit < VBTN_TOTAL_COUNT; ++bit)
	{
		const u32 mask = 1u << bit;
		if (IsButtonPressed(bit))
		{
			m_menuNativeCaptureButtons |= mask;
			NativeMenuButton(NativeMenuButtons[bit], "down", now);
		}
		else if ((m_menuNativeCaptureButtons & mask) && !(m_rawDownMask & mask))
		{
			m_menuNativeCaptureButtons &= ~mask;
			NativeMenuButton(NativeMenuButtons[bit], "up", now);
		}
	}
	ConsumeAllCurrentlyHeldButtons();
	return true;
}

static bool IsLiveMenuWidgetDescendant(const CUIWindow* root, const CUIWindow* target)
{
	if (!root || !target)
		return false;
	if (root == target)
		return true;
	for (CUIWindow* child : const_cast<CUIWindow*>(root)->GetChildWndList())
	{
		if (IsLiveMenuWidgetDescendant(child, target))
			return true;
	}
	return false;
}

bool CGamepadContextDispatcher::HandleMenuPageInput(u32 now)
{
	if (!IsMenuFocusMode() || !MainMenu() || !MainMenu()->IsActive() ||
		MainMenu()->TopInputReceiver() != m_menuFocusRoot || !m_bVcTargetHit)
		return false;

	if (xr_strcmp(GetCursorTargetKind(), "keybind") == 0 && IsButtonPressed(CInput::GP_BTN_SOUTH))
	{
		if (IsLiveMenuWidgetDescendant(m_menuFocusRoot, m_menuTarget))
		{
			if (auto* key = smart_cast<CUIEditKeyBind*>(m_menuTarget))
				key->BeginGamepadEdit();
		}
		ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
		return true;
	}
	// INV-REWORK-15 (п.6, Misha 2026-11-12): A on a slider did a mouse click at the cursor (the slider
	// centre) and threw the value to the middle. A does nothing on sliders; D-pad/LB/RB move them.
	if (xr_strcmp(GetCursorTargetKind(), "trackbar") == 0 && IsButtonPressed(CInput::GP_BTN_SOUTH))
	{
		ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
		return true;
	}
	if (xr_strcmp(GetCursorTargetKind(), "gp_native_row") != 0)
		return false;

	for (const u32 button : {CInput::GP_BTN_SOUTH, CInput::GP_BTN_WEST, CInput::GP_BTN_NORTH,
		CInput::GP_BTN_LEFT_SHOULDER, CInput::GP_BTN_RIGHT_SHOULDER, CInput::GP_BTN_BACK, CInput::GP_BTN_START})
	{
		if (IsButtonPressed(button) && NativeMenuButton(NativeMenuButtons[button], "tap", now))
		{
			ConsumeButtonUntilRelease(button);
			return true;
		}
	}
	return false;
}

bool CGamepadContextDispatcher::HandleConfirmationShortcuts()
{
	constexpr u32 aButton = CInput::GP_BTN_SOUTH;
	constexpr u32 xButton = CInput::GP_BTN_WEST;
	constexpr u32 bButton = CInput::GP_BTN_EAST;
	constexpr u32 yButton = CInput::GP_BTN_NORTH;
	constexpr u32 ConfirmHoldPauseMs = 300;
	constexpr u32 ConfirmHoldFillMs = 700;

	static CUIMessageBox* s_holdBox = nullptr;
	static u32 s_holdButton = VBTN_TOTAL_COUNT;
	static int s_holdChoice = -1;
	static u32 s_holdStartedMs = 0;
	static bool s_holdFired = false;
	static CUIMessageBox* s_fatalBox = nullptr;
	static int s_fatalFocusOrder = 1; // 0 = Copy (left), 1 = Yes/Reload (center), 2 = No/Continue (right)

	auto resetHold = [&](CUIMessageBox* liveBox)
	{
		if (liveBox && s_holdBox == liveBox && !s_holdFired)
			liveBox->SetGamepadHoldState(-1, 0.0f, false);
		s_holdBox = nullptr;
		s_holdButton = VBTN_TOTAL_COUNT;
		s_holdChoice = -1;
		s_holdStartedMs = 0;
		s_holdFired = false;
	};

	if (!pInput || !pInput->IsGamepadActiveSource() ||
		CGamepadOnScreenKeyboardUI::Instance().IsOpen() || (Console && Console->bVisible))
	{
		resetHold(nullptr);
		s_fatalBox = nullptr;
		return false;
	}

	CDialogHolder* holder = MainMenu() && MainMenu()->IsActive() ?
		static_cast<CDialogHolder*>(MainMenu()) : static_cast<CDialogHolder*>(CurrentGameUI());
	auto* root = holder ? holder->TopInputReceiver() : nullptr;
	auto* box = root && root->IR_process() ? FindActiveMenuMessageBox(root) : nullptr;
	if (!box)
	{
		resetHold(nullptr);
		s_fatalBox = nullptr;
		return false;
	}
	if (s_holdBox && s_holdBox != box)
		resetHold(nullptr);

	const bool a = IsButtonPressed(aButton);
	const bool x = IsButtonPressed(xButton);
	const bool b = IsButtonPressed(bButton);
	const bool y = IsButtonPressed(yButton);
	const u32 now = Device.dwTimeContinual;

	// DEF-P1-05 pt. 1 + W5-MENU В3: одиночные окна MESSAGEBOX_OK закрываются и кнопкой A, и кнопкой B.
	if (box->HasGamepadOk())
	{
		resetHold(box);
		if (!a && !b)
			return false;
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		m_menuHoldRoot = nullptr;
		if (a)
			ConsumeButtonUntilRelease(aButton);
		if (b)
			ConsumeButtonUntilRelease(bButton);
		return box->ActivateGamepadOk();
	}

	// W5-MENU В7: аварийное окно LuaFatalErrorUI (MESSAGEBOX_YES_NO_COPY) — начальная рамка на безопасном
	// «Перезагрузить сохранение» (button_yes, центр), «Продолжить» (button_no) защищено удержанием 0,3 + 0,7 с.
	if (box->HasGamepadFatalErrorChoice())
	{
		constexpr int orderToChoice[3] = { 1, 0, 2 }; // left=Copy(1), center=Yes(0), right=No(2)
		auto snapToOrder = [&](int ord)
		{
			s_fatalFocusOrder = ord;
			if (auto* targetBtn = box->GetGamepadChoiceButton(orderToChoice[ord]))
			{
				Frect br;
				targetBtn->GetAbsoluteRect(br);
				Fvector2 pos;
				pos.set((br.x1 + br.x2) * 0.5f, (br.y1 + br.y2) * 0.5f);
				GetUICursor().SetUICursorPosition(pos);
				m_bVcTargetHit = true;
				m_vcTargetCx = pos.x;
				m_vcTargetCy = pos.y;
				m_vcTargetW = br.width();
				m_vcTargetH = br.height();
				m_vcTargetKind = "button";
				m_menuTarget = targetBtn;
			}
		};

		if (s_fatalBox != box)
		{
			s_fatalBox = box;
			ConsumeAllCurrentlyHeldButtons();
			snapToOrder(1);
		}

		ENavDir navDir = ENavDir::None;
		if (IsNavDirTriggeredThisFrame(&navDir, nullptr, nullptr) &&
		    (navDir == ENavDir::Left || navDir == ENavDir::Right))
		{
			resetHold(box);
			const int nextOrd = _max(0, _min(2, s_fatalFocusOrder + (navDir == ENavDir::Right ? 1 : -1)));
			if (nextOrd != s_fatalFocusOrder)
				snapToOrder(nextOrd);
			MenuPulse();
			return true;
		}

		const int focusedChoice = orderToChoice[s_fatalFocusOrder];
		if (s_holdBox == box && s_holdChoice == 2)
		{
			if (x || y)
			{
				resetHold(box);
			}
			else if ((m_rawDownMask & (1u << s_holdButton)) != 0)
			{
				const u32 elapsed = (now >= s_holdStartedMs) ? (now - s_holdStartedMs) : 0;
				if (elapsed >= ConfirmHoldPauseMs + ConfirmHoldFillMs)
				{
					s_holdFired = true;
					box->SetGamepadHoldState(2, 1.0f, true);
					ConsumeButtonUntilRelease(s_holdButton);
					resetHold(box);
					MenuPulse();
					return box->ActivateGamepadFatalChoice(2);
				}
				const float prog = elapsed > ConfirmHoldPauseMs
					? float(elapsed - ConfirmHoldPauseMs) / float(ConfirmHoldFillMs)
					: 0.0f;
				box->SetGamepadHoldState(2, prog, false);
				return true;
			}
			else
			{
				const bool fired = s_holdFired;
				resetHold(box);
				if (!fired)
				{
					box->TriggerGamepadDenyShake(2);
					MenuPulse();
				}
				return true;
			}
		}

		if (x || (a && focusedChoice == 0))
		{
			ConsumeButtonUntilRelease(x ? xButton : aButton);
			MenuPulse();
			return box->ActivateGamepadFatalChoice(0);
		}
		if (y || (a && focusedChoice == 1))
		{
			ConsumeButtonUntilRelease(y ? yButton : aButton);
			MenuPulse();
			return box->ActivateGamepadFatalChoice(1);
		}
		if (b || (a && focusedChoice == 2))
		{
			s_holdBox = box;
			s_holdButton = b ? bButton : aButton;
			s_holdChoice = 2;
			s_holdStartedMs = now;
			s_holdFired = false;
			box->SetGamepadHoldState(2, 0.0f, false);
			return true;
		}
		return false;
	}
	s_fatalBox = nullptr;

	// FIX-LIVE2 п.34 -> FIX-LIVE3 п.24 (Миша 2026-11-16, 34.1б): «Выход» — диалог закрывает игру одним нажатием
	// «ОК» (A или X), B или A на «Отмена» — отмена. FIX-LIVE3: блок стоит ДО HasGamepadConfirmation (у шаблонов
	// GAMMA тексты кнопок могут быть пустыми — тогда прежний блок не достигался и A уходил в общий клик меню),
	// подтверждение идёт напрямую через OnYesOk (без проверок текста/IsEnabled кнопки), первый кадр каждого
	// нового показа окна поглощает нажатие, которым его открыли; в лог пишется итог для разбора.
	{
		static CUIMessageBox* s_quitBox = nullptr;
		if (!box->IsGamepadQuitConfirmation())
			s_quitBox = nullptr;
		else
		{
			resetHold(box);
			if (s_quitBox != box)
			{
				s_quitBox = box;
				ConsumeAllCurrentlyHeldButtons();
				return true;
			}
			if (!a && !x && !b)
				return false;
			m_menuBackRoot = nullptr;
			m_menuBackButton = nullptr;
			m_menuHoldRoot = nullptr;
			const u32 btnUsed = b ? bButton : (a ? aButton : xButton);
			ConsumeButtonUntilRelease(btnUsed);
			const bool cancelFocused = !b && !x && m_menuTarget && m_menuTarget == box->GetGamepadChoiceButton(2);
			const bool yes = !b && !cancelFocused;
			MenuPulse();
			const bool done = box->ActivateGamepadQuit(yes);
			Msg("* [Gamepad] quit_confirm: btn=%s yes=%d cancel_focused=%d done=%d",
			    b ? "B" : (a ? "A" : "X"), yes ? 1 : 0, cancelFocused ? 1 : 0, done ? 1 : 0);
			s_quitBox = nullptr;
			return true;
		}
	}

	if (!box->HasGamepadConfirmation())
	{
		resetHold(box);
		return false;
	}

	// В1 / В2: мгновенная отмена по B всегда имеет приоритет (в том числе при одновременном нажатии X+B).
	if (b)
	{
		resetHold(box);
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		m_menuHoldRoot = nullptr;
		if (x)
			ConsumeButtonUntilRelease(xButton);
		ConsumeButtonUntilRelease(bButton);
		box->ActivateGamepadConfirmation(false);
		return true;
	}

	// Обычные подтверждения (например В12 «Открыть сайт?») — короткое нажатие X = Да.
	if (!box->IsGamepadDangerousConfirmation())
	{
		resetHold(box);
		if (!x)
			return false;
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		m_menuHoldRoot = nullptr;
		ConsumeButtonUntilRelease(xButton);
		box->ActivateGamepadConfirmation(true);
		return true;
	}

	// В1 / В2: разрушительные «Да» (выход в меню/Windows, удаление/перезапись/загрузка поверх прогресса, смена локации)
	// требуют удержания X (0,3 с пауза + 0,7 с белое кольцо вокруг значка X).
	if (s_holdBox == box && s_holdButton == xButton)
	{
		if ((m_rawDownMask & (1u << xButton)) != 0)
		{
			const u32 elapsed = (now >= s_holdStartedMs) ? (now - s_holdStartedMs) : 0;
			if (elapsed >= ConfirmHoldPauseMs + ConfirmHoldFillMs)
			{
				s_holdFired = true;
				box->SetGamepadHoldState(0, 1.0f, true);
				m_menuBackRoot = nullptr;
				m_menuBackButton = nullptr;
				m_menuHoldRoot = nullptr;
				ConsumeButtonUntilRelease(xButton);
				resetHold(box);
				MenuPulse();
				box->ActivateGamepadConfirmation(true);
				return true;
			}
			const float prog = elapsed > ConfirmHoldPauseMs
				? float(elapsed - ConfirmHoldPauseMs) / float(ConfirmHoldFillMs)
				: 0.0f;
			box->SetGamepadHoldState(0, prog, false);
			return true;
		}
		const bool fired = s_holdFired;
		resetHold(box);
		if (!fired)
		{
			box->TriggerGamepadDenyShake(0);
			MenuPulse();
		}
		return true;
	}

	if (x)
	{
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		m_menuHoldRoot = nullptr;
		s_holdBox = box;
		s_holdButton = xButton;
		s_holdChoice = 0;
		s_holdStartedMs = now;
		s_holdFired = false;
		box->SetGamepadHoldState(0, 0.0f, false);
		return true;
	}
	return false;
}

bool CGamepadContextDispatcher::HandleMenuShortcuts(u32 now)
{
	const auto* input = pInput ? &pInput->GetGamepadState() : nullptr;
	const bool active = input && input->connected && input->has_focus && !input->need_release_all &&
		pInput->IsGamepadActiveSource() && IsMenuFocusMode() && MainMenu() && MainMenu()->IsActive() &&
		!m_luaHints.bMcmKeybindCapture && !HasActiveCustomEditInHolder(MainMenu());
	auto* root = active ? MainMenu()->TopInputReceiver() : nullptr;
	if (!root || !root->IsShown() || !root->IR_process())
	{
		m_menuHoldRoot = nullptr;
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		return false;
	}

	// W5-MENU В11 [решено без Миши]: упор на верхнем и нижнем краях вертикального меню CUIMMShniaga
	// без кольцевого перескока, с короткой тактильной отдачей MenuPulse().
	{
		struct ShniagaEdgeClampProbe : public CUIMMShniaga
		{
			static enum_page_id Page(const CUIMMShniaga* s)
			{
				return static_cast<const ShniagaEdgeClampProbe*>(s)->m_page;
			}
			static void SnapStop(CUIMMShniaga* s, CUIWindow* btn)
			{
				auto* p = static_cast<ShniagaEdgeClampProbe*>(s);
				p->SelectBtn(btn);
				p->ProcessEvent(E_Stop);
			}
		};
		static CUIMMShniaga* s_shn = nullptr;
		static int s_shnPage = -1;
		static int s_shnPrevIdx = -1;
		static u32 s_shnPrevSnapCount = 0;

		CUIMMShniaga* shn = !FindActiveMenuMessageBox(root) ? CUIMMShniagaButtonsProbe::FindShownIn(root) : nullptr;
		if (shn)
		{
			xr_vector<CUIWindow*> btns;
			CUIMMShniagaButtonsProbe::GatherButtons(shn, btns);
			const int n = static_cast<int>(btns.size());
			const int page = static_cast<int>(ShniagaEdgeClampProbe::Page(shn));
			CUIWindow* sel = CUIMMShniagaButtonsProbe::GetSelected(shn);
			int curIdx = 0;
			for (int i = 0; i < n; ++i)
			{
				if (btns[i] == sel)
				{
					curIdx = i;
					break;
				}
			}
			if (shn == s_shn && page == s_shnPage && n >= 2 && m_vcDpadSnapCount != s_shnPrevSnapCount)
			{
				const bool wrappedTop = (s_shnPrevIdx == 0 && curIdx == n - 1 && m_vcLastDpadSnapDir == ENavDir::Up);
				const bool wrappedBot = (s_shnPrevIdx == n - 1 && curIdx == 0 && m_vcLastDpadSnapDir == ENavDir::Down);
				if (wrappedTop || wrappedBot)
				{
					curIdx = s_shnPrevIdx;
					CUIWindow* edgeBtn = btns[curIdx];
					Frect er;
					edgeBtn->GetAbsoluteRect(er);
					Fvector2 edgePos;
					edgePos.set((er.x1 + er.x2) * 0.5f, (er.y1 + er.y2) * 0.5f);
					clamp(edgePos.x, 0.0f, UI_BASE_WIDTH);
					clamp(edgePos.y, 0.0f, UI_BASE_HEIGHT);
					GetUICursor().SetUICursorPosition(edgePos);
					m_vcTargetCx = edgePos.x;
					m_vcTargetCy = edgePos.y;
					m_vcTargetW = er.width();
					m_vcTargetH = er.height();
					if (root->IR_process() && (GetUICursor().IsVisible() || !root->NeedCursor()))
						root->OnMouseAction(edgePos.x, edgePos.y, WINDOW_MOUSE_MOVE);
					ShniagaEdgeClampProbe::SnapStop(shn, edgeBtn);
					MenuPulse();
				}
			}
			s_shn = shn;
			s_shnPage = page;
			s_shnPrevIdx = curIdx;
			s_shnPrevSnapCount = m_vcDpadSnapCount;
		}
		else
		{
			s_shn = nullptr;
			s_shnPage = -1;
			s_shnPrevIdx = -1;
		}
	}

	auto ensureDotFreeAutoSaveName = [&](CUIWindow* dlgRoot)
	{
		FillAutoSaveName(dlgRoot);
		if (auto* ed = FindMenuEdit(dlgRoot))
		{
			if (ed->GetText() && strchr(ed->GetText(), '.'))
			{
				string512 fixed;
				xr_strcpy(fixed, ed->GetText());
				for (char* p = fixed; *p; ++p)
					if (*p == '.')
						*p = '-';
				ed->SetText(fixed);
			}
		}
	};

	// W5-MENU В13: при открытии UISaveDialog с геймпадом сразу подставляем имя сохранения, если поле пустое.
	{
		static CUIWindow* s_autoNamedSaveRoot = nullptr;
		bool initLoading = false;
		const MenuFooter initFooter = FindMenuFooter(root, &initLoading);
		if (initLoading && IsMenuSaveFooter(initFooter))
		{
			if (s_autoNamedSaveRoot != root)
			{
				s_autoNamedSaveRoot = root;
				ensureDotFreeAutoSaveName(root);
			}
		}
		else
		{
			s_autoNamedSaveRoot = nullptr;
		}
	}

	constexpr u32 xButton = CInput::GP_BTN_WEST;
	constexpr u32 yButton = CInput::GP_BTN_NORTH;
	constexpr u32 bButton = CInput::GP_BTN_EAST;
	constexpr u32 aButton = CInput::GP_BTN_SOUTH;
	constexpr u32 viewButton = CInput::GP_BTN_BACK;
	const bool x = IsButtonPressed(xButton), y = IsButtonPressed(yButton), b = IsButtonPressed(bButton);
	const bool a = IsButtonPressed(aButton), view = IsButtonPressed(viewButton);
	if (!x && !y && !b && !a && !view && !m_menuHoldRoot && !m_menuBackRoot)
		return false;
	if (y && !x && !b && CUIMMShniagaButtonsProbe::FindShownIn(root) && !FindActiveMenuMessageBox(root))
	{
		ConsumeButtonUntilRelease(yButton);
		PauseMenuQuickSave((m_rawDownMask & (1u << CInput::GP_BTN_LEFT_SHOULDER)) != 0);
		return true;
	}

	bool loading = false;
	const MenuFooter footer = FindMenuFooter(root, &loading);
	auto* combo = GetExpandedMenuCombo(root);
	const bool hasMessageBox = (b || m_menuBackRoot) && FindActiveMenuMessageBox(root);
	const bool canHoldBack = footer[0] && !loading && !hasMessageBox;
	if (!canHoldBack || m_menuBackRoot != root || x || y || view)
	{
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
	}
	if (canHoldBack && b && !x && !y && !view)
	{
		ConsumeButtonUntilRelease(bButton);
		if (a)
			ConsumeButtonUntilRelease(aButton);
		m_menuHoldRoot = nullptr;
		m_menuBackRoot = root;
		m_menuBackButton = footer[2];
		m_menuBackStarted = now;
		m_menuBackClosedCombo = combo != nullptr;
		if (combo)
		{
			CUIComboBoxProbe::Close(combo);
			StepMenuFocus(root, ENavDir::None, 0, 0, now);
		}
		return true;
	}
	if (m_menuBackRoot)
	{
		const bool held = (m_rawDownMask & (1u << bButton)) != 0;
		const bool expired = now - m_menuBackStarted >= MenuDefaultHoldMs;
		if (held && !expired)
			return false;
		const bool closedCombo = m_menuBackClosedCombo;
		m_menuBackRoot = nullptr;
		m_menuBackButton = nullptr;
		if (expired)
		{
			MenuPulse();
			if (MainMenu()->TopInputReceiver() == root && footer[2]->IsEnabled())
				footer[2]->OnClick();
		}
		else if (!closedCombo && !StepMenuFocus(root, ENavDir::None, 0, 0, now, EMenuFocusAction::Back) &&
			MainMenu()->TopInputReceiver() == root && footer[2]->IsEnabled())
			footer[2]->OnClick();
		return true;
	}

	if (combo)
	{
		m_menuHoldRoot = nullptr;
		for (const auto button : {xButton, yButton, bButton, viewButton})
			if (IsButtonPressed(button))
				ConsumeButtonUntilRelease(button);
		if (b)
		{
			if (a)
				ConsumeButtonUntilRelease(aButton);
			CUIComboBoxProbe::Close(combo);
			StepMenuFocus(root, ENavDir::None, 0, 0, now);
			return true;
		}
		return false;
	}

	if (!footer[0] || hasMessageBox)
	{
		m_menuHoldRoot = nullptr;
		return false;
	}
	if (view)
	{
		ConsumeButtonUntilRelease(viewButton);
		m_menuHoldRoot = nullptr;
		StepMenuFocus(root, ENavDir::None, 0, 0, now, EMenuFocusAction::View);
		return true;
	}

	if (loading)
	{
		const bool saving = IsMenuSaveFooter(footer);
		auto* list = MenuSaveList(footer);
		// SA1: a stale m_menuTarget (row freed by list rebuild) must not be dereferenced.
		const bool targetAlive = (m_menuFocusRoot == root && m_bVcTargetHit &&
		                          IsLiveMenuWidgetDescendant(root, m_menuTarget));
		auto* row = (list && targetAlive)
			? smart_cast<CUIListBoxItem*>(m_menuTarget) : nullptr;
		auto* edit = saving ? FindMenuEdit(root) : nullptr;
		const bool onEdit = edit && targetAlive && m_menuTarget == edit;
		int footerIndex = -1;
		if (targetAlive)
			for (int i = 0; i < 3; ++i)
				if (m_menuTarget == footer[i])
					footerIndex = i;

		// W5-MENU В13: нажатие A на поле имени сохранения (или Y вне строк списка) открывает экранную клавиатуру с 6 шаблонами.
		if (saving && edit && ((a && onEdit) || (y && !b && !x && (!row || footerIndex >= 0) && onEdit)))
		{
			if (a)
				ConsumeButtonUntilRelease(aButton);
			if (y)
				ConsumeButtonUntilRelease(yButton);
			if (!CGamepadOnScreenKeyboardUI::Instance().IsOpen())
				CGamepadOnScreenKeyboardUI::Instance().OpenForEditBox(edit, a ? "save_edit_a" : "save_edit_y");
			return true;
		}

		auto action = [&](int index)
		{
			if (index == 1 && list && list->GetSelectedItem() && footer[index]->IsEnabled())
			{
				m_menuDeleteIndex = static_cast<int>(list->GetSelectedIDX());
				m_menuDeleteCount = list->GetSize();
				m_menuDeleteModalSeen = false;
				m_menuTarget = nullptr;
				m_bVcTargetHit = false;
			}
			if (footer[index]->IsEnabled())
				footer[index]->OnClick();
		};
		if (b || x || y)
		{
			for (const auto button : {aButton, xButton, yButton, bButton})
				if (IsButtonPressed(button))
					ConsumeButtonUntilRelease(button);
			if (b)
				action(2);
			else if (row && footerIndex < 0 && row->GetParent() && row->GetParent()->GetParent() == list)
			{
				row->OnMouseDown(MOUSE_1);
				m_menuLastSaveRow[saving ? 1 : 0] = row->GetText();
				if (list->GetSelectedItem() == row)
					action(x ? 0 : 1);
			}
			else if (saving && x)
			{
				ensureDotFreeAutoSaveName(root);
				action(0);
			}
			else if (saving && y && edit && !CGamepadOnScreenKeyboardUI::Instance().IsOpen())
			{
				CGamepadOnScreenKeyboardUI::Instance().OpenForEditBox(edit, "save_edit_y");
			}
			return true;
		}
		if (a && ((row && row->GetParent() && row->GetParent()->GetParent() == list) || footerIndex >= 0))
		{
			ConsumeButtonUntilRelease(aButton);
			if (footerIndex >= 0)
			{
				if (footerIndex == 2 || (list && list->GetSelectedItem()))
					action(footerIndex);
			}
			else if (!IsMenuSaveFooter(footer))
			{
				// INV-REWORK-4: one A on a save row loads it at once (no second press)
				m_menuLastSaveRow[0] = row->GetText();
				if (list->GetSelectedItem() != row)
					row->OnMouseDown(MOUSE_1);
				if (list->GetSelectedItem() == row)
					action(0);
			}
			else if (m_menuLoadArmed.size() && list->GetSelectedItem() == row && m_menuLoadArmed == row->GetText())
				action(0);
			else
			{
				m_menuLoadArmed = row->GetText();
				m_menuLastSaveRow[IsMenuSaveFooter(footer) ? 1 : 0] = row->GetText();
				row->OnMouseDown(MOUSE_1);
			}
			return true;
		}
		return false;
	}

	auto page = [&]()
	{
		MenuTargets targets;
		GatherMenuFocusTargets(root, nullptr, footer, targets);
		const auto panels = GetMenuPanels(targets);
		return MenuPanelKey(targets, panels, nullptr);
	};
	if (m_menuHoldRoot)
	{
		if (root != m_menuHoldRoot || footer[3] != m_menuHoldDefault || page() != m_menuHoldPage || b || x)
			m_menuHoldRoot = nullptr;
		else if (!(m_rawDownMask & (1u << yButton)))
		{
			m_menuHoldRoot = nullptr;
			if (!m_menuHoldFired && footer[1]->IsEnabled())
			{
				m_menuFocusOnTrackbar = false;
				m_bVcTargetHit = false;
				footer[1]->OnClick();
				if (MainMenu()->TopInputReceiver() == root)
					StepMenuFocus(root, ENavDir::None, 0, 0, now);
			}
			return true;
		}
		else if (!m_menuHoldFired && now - m_menuHoldStart >= MenuDefaultHoldMs)
		{
			m_menuHoldFired = true;
			MenuPulse();
			if (footer[3]->IsEnabled())
			{
				m_menuFocusOnTrackbar = false;
				m_bVcTargetHit = false;
				footer[3]->OnClick();
				if (MainMenu()->TopInputReceiver() == root)
					StepMenuFocus(root, ENavDir::None, 0, 0, now);
			}
			return true;
		}
	}
	if (!x && !y && !b)
		return false;
	for (const auto button : {aButton, xButton, yButton, bButton})
		if (IsButtonPressed(button))
			ConsumeButtonUntilRelease(button);
	if (b && StepMenuFocus(root, ENavDir::None, 0, 0, now, EMenuFocusAction::Back))
		return true;
	if (y && !x && !b && footer[3] && footer[3]->IsShown())
	{
		m_menuHoldRoot = root;
		m_menuHoldDefault = footer[3];
		m_menuHoldPage = page();
		m_menuHoldStart = now;
		m_menuHoldFired = false;
		return true;
	}
	CUIButton* button = footer[b ? 2 : x ? 0 : 1];
	if (button->IsEnabled())
		button->OnClick();
	return true;
}

int CGamepadContextDispatcher::GetMenuDefaultHoldFrame() const
{
	if (!m_menuHoldRoot)
		return 0;
	return static_cast<int>(std::min(MenuDefaultHoldMs, Device.dwTimeContinual - m_menuHoldStart) *
		MenuDefaultFrames / MenuDefaultHoldMs);
}

int CGamepadContextDispatcher::GetMenuBackHoldFrame(const CUIButton* button) const
{
	if (!m_menuBackRoot || m_menuBackButton != button || !pInput || !pInput->IsGamepadActiveSource() ||
		!MainMenu() || MainMenu()->TopInputReceiver() != m_menuBackRoot)
		return 0;
	return static_cast<int>(std::min(MenuDefaultHoldMs, Device.dwTimeContinual - m_menuBackStarted) *
		MenuDefaultFrames / MenuDefaultHoldMs);
}

void CGamepadContextDispatcher::ArrangeMenuShortcutRow(CUIButton* button)
{
	if (!button || !MainMenu() || !MainMenu()->IsActive() || m_menuFooterLayoutFrame == Device.dwFrame)
		return;
	auto* root = MainMenu()->TopInputReceiver();
	if (!root)
		return;
	const MenuFooter footer = FindMenuFooter(root);
	if (std::find(footer.begin(), footer.end(), button) == footer.end())
		return;
	m_menuFooterLayoutFrame = Device.dwFrame;

	xr_vector<CUIButton*> row;
	for (CUIButton* item : footer)
		if (item && item->IsShown())
			row.push_back(item);
	CUIWindow* parent = row.empty() ? nullptr : row[0]->GetParent();
	if (!parent)
		return;
	for (CUIButton* item : row)
		if (item->GetParent() != parent)
			return;
	auto rectOf = [](CUIWindow* w) { Frect r; w->GetAbsoluteRect(r); return r; };
	std::sort(row.begin(), row.end(), [&](CUIButton* a, CUIButton* b) { return rectOf(a).x1 < rectOf(b).x1; });

	Frect span = rectOf(row[0]);
	for (CUIButton* item : row)
	{
		const Frect r = rectOf(item);
		span.set(_min(span.x1, r.x1), _min(span.y1, r.y1), _max(span.x2, r.x2), _max(span.y2, r.y2));
	}
	// The panel directly above: notify strip in Settings/MCM, save list frame in Load.
	CUIWindow* above = nullptr;
	Frect aboveRect{};
	for (CUIWindow* child : parent->GetChildWndList())
	{
		if (!child->IsShown() || smart_cast<CUIButton*>(child))
			continue;
		const Frect r = rectOf(child);
		if (r.y2 > span.y1 + 2.0f || r.y2 < span.y1 - 3.0f * span.height() || r.width() < 2.0f * span.height() ||
			CenterX(span) < r.x1 || CenterX(span) > r.x2)
			continue;
		if (!above || r.y2 > aboveRect.y2 + 0.5f || (_abs(r.y2 - aboveRect.y2) <= 0.5f && r.width() > aboveRect.width()))
		{
			above = child;
			aboveRect = r;
		}
	}
	if (!above)
		return;
	if (smart_cast<CUITextWnd*>(above))
	{
		// Settings/MCM: the visible strip is baked into the dialog art inside the notify text window
		// (measured 166..949 px on the 2559x1079 reference frame).
		aboveRect.x1 += 3.4f;
		aboveRect.x2 -= 6.3f;
	}
	// Reference frame 2559x1079: button 43 px high, 8 px between buttons.
	const float scaleX = UI().ClientToScreenScaledX(1.0f);
	const float scaleY = UI().ClientToScreenScaledY(1.0f);
	if (scaleX <= 1e-4f || scaleY <= 1e-4f)
		return;
	const float unitY = 768.0f / 1079.0f;
	const float unitX = unitY * scaleY / scaleX;
	const float height = 43.0f * unitY;
	const float gap = 8.0f * unitX;
	const float count = static_cast<float>(row.size());
	const float width = (aboveRect.width() - gap * (count - 1.0f)) / count;
	if (!(width >= 8.0f))
		return;
	const Frect origin = rectOf(parent);
	const float top = CenterY(span) - height * 0.5f;
	for (size_t i = 0; i < row.size(); ++i)
	{
		const Fvector2 pos = Fvector2().set(aboveRect.x1 + i * (width + gap) - origin.x1, top - origin.y1);
		const Fvector2& oldPos = row[i]->GetWndPos();
		const Fvector2& oldSize = row[i]->GetWndSize();
		if (_abs(oldPos.x - pos.x) > 0.01f || _abs(oldPos.y - pos.y) > 0.01f)
			row[i]->SetWndPos(pos);
		// SetWidth/SetHeight also resize the CUI3tButton background child.
		if (_abs(oldSize.x - width) > 0.01f)
			row[i]->SetWidth(width);
		if (_abs(oldSize.y - height) > 0.01f)
			row[i]->SetHeight(height);
	}
}

int CGamepadContextDispatcher::GetMenuShortcutIndex(CUIButton* button) const
{
	if (!MainMenu() || !MainMenu()->IsActive())
		return -1;
	for (size_t i = 0; i < MenuShortcutCount; ++i)
	{
		if (m_menuShortcutButtons[i] == button)
			for (const CUIWindow* parent = button->GetParent(); parent; parent = parent->GetParent())
				if (parent == m_menuMemoryRoot)
					return static_cast<int>(i);
	}
	const LPCSTR label = button->TextItemControl()->GetText();
	auto* root = MainMenu()->TopInputReceiver();
	if (!root || !label || !*label || !smart_cast<CUI3tButton*>(button))
		return -1;
	const CStringTable strings;
	constexpr LPCSTR ids[] = {"ui_mm_apply", "ui_mm_reset", "ui_mm_cancel", "ui_mm_default", "ui_mm_load", "ui_mm_delete",
		"ui_mm_save"};
	for (auto id : ids)
	{
		if (xr_strcmp(label, strings.translate(id).c_str()) == 0)
		{
			const auto footer = FindMenuFooter(root);
			for (size_t i = 0; i < footer.size(); ++i)
				if (footer[i] == button)
					return static_cast<int>(i);
			break;
		}
	}
	return -1;
}

void CGamepadContextDispatcher::RefreshCursorTargetFromLua(Fvector2 pos, u32 nowContinual)
{
	m_vcLastTargetQueryTime = nowContinual;
	m_bVcTargetHit = false;
	m_vcTargetW = 0.0f;
	m_vcTargetH = 0.0f;
	m_vcTargetKind = "none";

	if (IsPdaInteractiveActive())
	{
		m_bPdaEyeSpotHit = false;
		if (IsPdaMapEyeActive())
		{
			CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
			if (CUITaskWnd* taskWnd = pda.GetTaskWnd())
			{
				if (CUIMapWnd* mapWnd = taskWnd->GetMapWnd())
				{
					if (CMapSpot* sp = mapWnd->FindGamepadSpotUnderCursor())
					{
						Frect r;
						sp->GetAbsoluteRect(r);
						m_bVcTargetHit = true;
						m_vcTargetCx = (r.x1 + r.x2) * 0.5f;
						m_vcTargetCy = (r.y1 + r.y2) * 0.5f;
						m_vcTargetW = r.width();
						m_vcTargetH = r.height();
						m_vcTargetKind = "pda_spot";
						m_bPdaEyeSpotHit = true;
						m_pdaEyeSpotCx = m_vcTargetCx;
						m_pdaEyeSpotCy = m_vcTargetCy;
						m_pdaEyeSpotW = m_vcTargetW;
						m_pdaEyeSpotH = m_vcTargetH;
					}
				}
			}
			return;
		}

		if (m_bInvFocusFrame && g_ai_space)
		{
			::luabind::functor<LPCSTR> fn;
			if (ai().script_engine().functor("gp_bridge.cursor_query_target", fn))
			{
				try
				{
					LPCSTR res = fn(pos.x, pos.y);
					if (res && strstr(res, "hit:1;") == res)
					{
						float cx = pos.x, cy = pos.y, w = 0.0f, h = 0.0f;
						char kindBuf[64] = "cell";
						if (sscanf(res, "hit:1;cx=%f;cy=%f;w=%f;h=%f;kind=%63s", &cx, &cy, &w, &h, kindBuf) >= 4)
						{
							m_bVcTargetHit = true;
							m_vcTargetCx = cx;
							m_vcTargetCy = cy;
							m_vcTargetW = w;
							m_vcTargetH = h;
							m_vcTargetKind = kindBuf;
							return;
						}
					}
				}
				catch (...)
				{
					Msg("! [Gamepad] Exception in gp_bridge.cursor_query_target (PDA)");
				}
			}
		}

		if (CurrentGameUI() && CurrentGameUI()->GetPdaMenu().IsShown())
		{
			CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
			if (CUITaskWnd* taskWnd = pda.GetTaskWnd())
			{
				LPCSTR ov = taskWnd->GetGamepadOverlayName();
				if (taskWnd->IsShown() && ov[0] != '\0')
				{
					xr_vector<Frect> ovRects;
					taskWnd->GatherGamepadOverlayTargets(ovRects);
					float bestDistSq = 48.0f * 48.0f;
					const bool isMenu = (0 == xr_strcmp(ov, "menu"));
					for (const Frect& r : ovRects)
					{
						const float cx = (r.x1 + r.x2) * 0.5f;
						const float cy = (r.y1 + r.y2) * 0.5f;
						const float dx = pos.x - cx;
						const float dy = pos.y - cy;
						float d2 = dx * dx + dy * dy;
						if (r.in(pos.x, pos.y))
							d2 *= 0.25f;
						if (d2 <= bestDistSq)
						{
							bestDistSq = d2;
							m_bVcTargetHit = true;
							m_vcTargetCx = cx;
							m_vcTargetCy = cy;
							m_vcTargetW = r.width();
							m_vcTargetH = r.height();
							m_vcTargetKind = isMenu ? "prop" : "pda_overlay";
						}
					}
				}
			}
		}
		return;
	}

	// FIX-LIVE1 (Misha 2026-11-15): the new character window lives in the MainMenu context, but its focus
	// target comes from Lua (gp_workshop -> gp_new_game.query); returning here left the white frame without a target.
	if (m_currentContext == EGamepadContext::MainMenu && !CGamepadOnScreenKeyboardUI::Instance().IsOpen() &&
	    NewGameDialog() == nullptr)
	{
		if (MainMenu() != nullptr && MainMenu()->IsActive())
		{
			if (CUIDialogWnd* mmTir = MainMenu()->TopInputReceiver())
			{
				if (CUIMMShniaga* shn = CUIMMShniagaButtonsProbe::FindShownIn(mmTir))
				{
					xr_vector<CUIWindow*> shnBtns;
					CUIMMShniagaButtonsProbe::GatherButtons(shn, shnBtns);
					for (CUIWindow* b : shnBtns)
					{
						Frect r;
						b->GetAbsoluteRect(r);
						if (r.in(pos.x, pos.y))
						{
							m_bVcTargetHit = true;
							m_vcTargetCx = (r.x1 + r.x2) * 0.5f;
							m_vcTargetCy = (r.y1 + r.y2) * 0.5f;
							m_vcTargetW = r.width();
							m_vcTargetH = r.height();
							m_vcTargetKind = "shniaga_btn";
							break;
						}
					}
					return;
				}

			}
		}
		return;
	}

	if (g_ai_space)
	{
		::luabind::functor<LPCSTR> fn;
		if (ai().script_engine().functor("gp_bridge.cursor_query_target", fn))
		{
			try
			{
				LPCSTR res = fn(pos.x, pos.y);
				if (res && strstr(res, "hit:1;") == res)
				{
					float cx = pos.x, cy = pos.y, w = 41.0f, h = 41.0f;
					char kindBuf[64] = "cell";
					if (sscanf(res, "hit:1;cx=%f;cy=%f;w=%f;h=%f;kind=%63s", &cx, &cy, &w, &h, kindBuf) >= 4)
					{
						m_bVcTargetHit = w > 1.0f && h > 1.0f;
						m_vcTargetCx = cx;
						m_vcTargetCy = cy;
						m_vcTargetW = w;
						m_vcTargetH = h;
						m_vcTargetKind = kindBuf;
						return;
					}
				}
			}
			catch (...)
			{
				Msg("! [Gamepad] Exception in gp_bridge.cursor_query_target");
			}
		}

		::luabind::functor<LPCSTR> fnScreens;
		if (ai().script_engine().functor("gp_screens.cursor_query_target", fnScreens))
		{
			try
			{
				LPCSTR res = fnScreens(pos.x, pos.y);
				float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
				char kindBuf[32] = { 0 };
				// MENU-FOCUS-1: необязательное 6-е поле — тип цели (например "menu_item" у пунктов главного меню)
				const int nScan = res ? sscanf(res, "1;%f;%f;%f;%f;%31[^;]", &x1, &y1, &x2, &y2, kindBuf) : 0;
				if (nScan >= 4)
				{
					m_bVcTargetHit = true;
					m_vcTargetCx = (x1 + x2) * 0.5f;
					m_vcTargetCy = (y1 + y2) * 0.5f;
					m_vcTargetW = _abs(x2 - x1);
					m_vcTargetH = _abs(y2 - y1);
					m_vcTargetKind = (nScan == 5 && 0 == xr_strcmp(kindBuf, "menu_item")) ? "screen_lua_menu_item" : "screen_lua";
					return;
				}
			}
			catch (...)
			{
				Msg("! [Gamepad] Exception in gp_screens.cursor_query_target");
			}
		}
	}

	// F5.1 + FIX-9: C++ обход всех интерактивных контролов в активном диалоге / главном меню / MCM (глубина до 20)
	if (!CGamepadOnScreenKeyboardUI::Instance().IsOpen())
	{
		CDialogHolder* holder = (MainMenu() != nullptr && MainMenu()->IsActive())
		                            ? static_cast<CDialogHolder*>(MainMenu())
		                            : static_cast<CDialogHolder*>(CurrentGameUI());
		CUIDialogWnd* tir = holder ? holder->TopInputReceiver() : nullptr;
		if (tir && tir->IsShown())
		{
			float bestDistSq = 36.0f * 36.0f;
			auto checkControlRect = [&](CUIWindow* ctrl, LPCSTR kindTag, float weight)
			{
				if (!ctrl || !ctrl->IsShown() || !ctrl->IsEnabled())
					return;
				Frect r;
				ctrl->GetAbsoluteRect(r);
				if (r.width() >= 10.0f && r.height() >= 8.0f && r.in(pos.x, pos.y))
				{
					const float cx = (r.x1 + r.x2) * 0.5f;
					const float cy = (r.y1 + r.y2) * 0.5f;
					const float dx = pos.x - cx;
					const float dy = pos.y - cy;
					const float d2 = (dx * dx + dy * dy) * weight;
					if (d2 <= bestDistSq)
					{
						bestDistSq = d2;
						m_bVcTargetHit = true;
						m_vcTargetCx = cx;
						m_vcTargetCy = cy;
						m_vcTargetW = r.width();
						m_vcTargetH = r.height();
						m_vcTargetKind = kindTag;
					}
				}
			};
			auto scanWnd = [&](auto& self, CUIWindow* w, int depth) -> void
			{
				if (!w || !w->IsShown() || depth > 20)
					return;
				if (CUIMMShniaga* shn = smart_cast<CUIMMShniaga*>(w))
				{
					xr_vector<CUIWindow*> shnBtns;
					CUIMMShniagaButtonsProbe::GatherButtons(shn, shnBtns);
					for (CUIWindow* b : shnBtns)
						checkControlRect(b, "shniaga_btn", 0.20f);
				}
				else if (smart_cast<CUICustomEdit*>(w) != nullptr)
					checkControlRect(w, "editbox", 0.15f);
				else if (smart_cast<CUICheckButton*>(w) != nullptr)
					checkControlRect(w, "check_btn", 0.20f);
				else if (smart_cast<CUIRadioButton*>(w) != nullptr)
					checkControlRect(w, "radio_btn", 0.20f);
				else if (smart_cast<CUITabButton*>(w) != nullptr)
					checkControlRect(w, "tab_btn", 0.22f);
				else if (smart_cast<CUI3tButton*>(w) != nullptr)
					checkControlRect(w, "dialog_btn", 0.25f);
				else if (smart_cast<CUIButton*>(w) != nullptr)
					checkControlRect(w, "button", 0.25f);
				else if (smart_cast<CUIComboBox*>(w) != nullptr)
					checkControlRect(w, "combobox", 0.22f);
				else if (smart_cast<CUITrackBar*>(w) != nullptr)
					checkControlRect(w, "trackbar", 0.22f);
				else if (smart_cast<CUIListBoxItem*>(w) != nullptr)
					checkControlRect(w, "listbox_item", 0.22f);
				else if (smart_cast<CUIListBox*>(w) != nullptr)
					checkControlRect(w, "listbox", 0.35f);
				else if (smart_cast<CUITabControl*>(w) != nullptr)
					checkControlRect(w, "tab_ctrl", 0.35f);

				for (auto it = w->GetChildWndList().begin(); it != w->GetChildWndList().end(); ++it)
					self(self, *it, depth + 1);
			};
			scanWnd(scanWnd, tir, 0);
		}
	}
}

bool CGamepadContextDispatcher::IsMenuFocusMode() const
{
	if (m_bGamepadMouseMode)
		return false;
	// MENU-FOCUS-1: главное меню и всё, что открыто поверх него (Настройки, MCM, загрузка/сохранение, окна подтверждения).
	// Экранная клавиатура — отдельный режим со своим курсором.
	return pInput && pInput->IsGamepadActiveSource() && !CGamepadOnScreenKeyboardUI::Instance().IsOpen() &&
	       ((m_currentContext == EGamepadContext::MainMenu && !NewGameDialog()) ||
	        (m_currentContext == EGamepadContext::MenuUI_Modal && CurrentGameUI() &&
	         IsLightingDialog(CurrentGameUI()->TopInputReceiver())));
}

bool CGamepadContextDispatcher::IsInventoryFocusFrameMode() const
{
	if (m_bGamepadMouseMode)
		return false;
	return m_bInvFocusFrame && m_bVcActive && m_bVcInModalMenu &&
	       (m_currentContext == EGamepadContext::MenuUI_Modal || NewGameDialog() || IsWorkshopModalActive() || IsPdaInteractiveActive()) &&
	       pInput != nullptr && pInput->IsGamepadActiveSource();
}

// INV-REWORK-2 (фикс вылета 2026-11-11, lj_gc.c gc_traverse_trace из CLevel::LuaGC): UICursor::OnRender
// идёт в потоке рендера параллельно с Lua GC (EngineThreading.cpp seqLuaGC) — из рендера Lua звать нельзя.
// Lua опрашивается только здесь, в игровом потоке (начало StepVirtualCursor), рендер читает готовый флаг.
void CGamepadContextDispatcher::UpdateInventoryFocusFramePending()
{
	m_invPendingFrame = Device.dwFrame;
	m_bInvPending = false;
	if (m_bGamepadMouseMode || ps_gp_inv_focus_frame == 0 || !g_ai_space || !pInput || !pInput->IsGamepadActiveSource())
		return;
	if (MainMenu() != nullptr && MainMenu()->IsActive())
		return;
	::luabind::functor<bool> fnInv;
	if (ai().script_engine().functor("gp_bridge.inv_focus_frame_active", fnInv))
	{
		try
		{
			m_bInvPending = fnInv();
		}
		catch (...)
		{
			m_bInvPending = false;
		}
	}
}

bool CGamepadContextDispatcher::IsInventoryFocusFramePending() const
{
	if (m_bGamepadMouseMode)
		return false;
	// флаг свежий (этот или прошлый кадр игрового потока)
	return m_bInvPending && Device.dwFrame - m_invPendingFrame <= 2;
}

bool CGamepadContextDispatcher::IsMenuFocusNativeHighlightTarget() const
{
	const LPCSTR kind = GetCursorTargetKind();
	return 0 == xr_strcmp(kind, "shniaga_btn") || 0 == xr_strcmp(kind, "screen_lua_menu_item");
}

bool CGamepadContextDispatcher::TryDpadSnapInModalUI(ENavDir dir, u32 nowContinual)
{
	if (dir == ENavDir::None)
		return false;

	const Fvector2 curPos = GetUICursor().GetCursorPosition();

	// MENU-FOCUS-1: в главном меню и его подокнах (Настройки, MCM, Загрузка/Сохранение, модальные окна) обходим только живое C++ дерево без Lua-догадок
	if (m_currentContext == EGamepadContext::MainMenu && !CGamepadOnScreenKeyboardUI::Instance().IsOpen())
	{
		if (MainMenu() != nullptr && MainMenu()->IsActive())
		{
			if (CUIDialogWnd* mmTir = MainMenu()->TopInputReceiver())
			{
				if (CUIMMShniaga* shn = CUIMMShniagaButtonsProbe::FindShownIn(mmTir))
				{
					if (dir == ENavDir::Left || dir == ENavDir::Right)
						return true;

					xr_vector<CUIWindow*> shnBtns;
					CUIMMShniagaButtonsProbe::GatherButtons(shn, shnBtns);
					if (!shnBtns.empty())
					{
						const int n = static_cast<int>(shnBtns.size());
						int curIdx = -1;
						for (int i = 0; i < n; ++i)
						{
							Frect r;
							shnBtns[i]->GetAbsoluteRect(r);
							if (r.in(curPos.x, curPos.y))
							{
								curIdx = i;
								break;
							}
						}
						if (curIdx < 0)
						{
							if (CUIWindow* sel = CUIMMShniagaButtonsProbe::GetSelected(shn))
							{
								for (int i = 0; i < n; ++i)
								{
									if (shnBtns[i] == sel)
									{
										curIdx = i;
										break;
									}
								}
							}
						}
						if (curIdx < 0)
							curIdx = 0;

						const int step = (dir == ENavDir::Down) ? 1 : -1;
						const int nextIdx = (curIdx + step + n) % n;

						Frect nr;
						shnBtns[nextIdx]->GetAbsoluteRect(nr);
						Fvector2 newPos;
						newPos.set((nr.x1 + nr.x2) * 0.5f, (nr.y1 + nr.y2) * 0.5f);
						clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
						clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);

						if (pInput)
							pInput->NotifyGamepadCursorMoved(nowContinual);

						GetUICursor().SetUICursorPosition(newPos);
						m_vcRemX = m_vcRemY = 0.0f;
						m_bVcTargetHit = true;
						m_vcTargetCx = newPos.x;
						m_vcTargetCy = newPos.y;
						m_vcTargetW = nr.width();
						m_vcTargetH = nr.height();
						m_vcTargetKind = "shniaga_btn";
						m_vcLastDpadSnapDir = dir;
						++m_vcDpadSnapCount;
						++m_vcMoveCount;

						if (mmTir->IR_process() && (GetUICursor().IsVisible() || !mmTir->NeedCursor()))
							mmTir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
						return true;
					}
					return false;
				}

			}
		}
		return false;
	}

	const LPCSTR pdaMtag = m_luaHints.modalMenuTag.size() ? m_luaHints.modalMenuTag.c_str() : "";
	const bool bPawModalTag = (0 == xr_strcmp(pdaMtag, "UIWPSettings") ||
	                           0 == xr_strcmp(pdaMtag, "UIPinSettingsWnd") ||
	                           0 == xr_strcmp(pdaMtag, "UIPAWIndicator"));
	if (IsPdaInteractiveActive() && CurrentGameUI() &&
	    (CurrentGameUI()->GetPdaMenu().IsShown() || bPawModalTag))
	{
		if (IsPdaMapEyeActive())
			return false;

		CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
		const bool bPdaTop = (CurrentGameUI()->TopInputReceiver() == &pda);
		if (g_ai_space)
		{
			::luabind::functor<LPCSTR> fn;
			if (ai().script_engine().functor("gp_bridge.cursor_dpad_snap", fn))
			{
				try
				{
					LPCSTR res = fn(curPos.x, curPos.y, static_cast<int>(dir));
					if (res && strstr(res, "snap:1;") == res)
					{
						float nx = curPos.x, ny = curPos.y;
						char kindBuf[64] = "cell";
						if (sscanf(res, "snap:1;cx=%f;cy=%f;kind=%63s", &nx, &ny, kindBuf) >= 2)
						{
							Fvector2 newPos;
							newPos.set(nx, ny);
							if (psActorFlags.test(AF_3D_PDA) && bPdaTop)
							{
								clamp(newPos.x, pda.m_cursor_box.x1, pda.m_cursor_box.x2);
								clamp(newPos.y, pda.m_cursor_box.y1, pda.m_cursor_box.y2);
							}
							else
							{
								clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
								clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);
							}
							SnapPdaCursor(newPos);
							m_vcRemX = m_vcRemY = 0.0f;
							m_bVcTargetHit = true;
							m_vcTargetCx = newPos.x;
							m_vcTargetCy = newPos.y;
							m_vcTargetKind = kindBuf;
							m_vcLastDpadSnapDir = dir;
							++m_vcDpadSnapCount;
							++m_vcMoveCount;
							if (m_bInvFocusFrame)
								RefreshCursorTargetFromLua(newPos, nowContinual);
							return true;
						}
					}
					if (res && 0 == xr_strcmp(res, "stay"))
						return true;
				}
				catch (...)
				{
					Msg("! [Gamepad] Exception in gp_bridge.cursor_dpad_snap (PDA)");
				}
			}
		}

		if (bPdaTop)
		{
			if (CUITaskWnd* taskWnd = pda.GetTaskWnd())
			{
				LPCSTR ov = taskWnd->GetGamepadOverlayName();
				if (taskWnd->IsShown() && ov[0] != '\0')
				{
					xr_vector<SCursorSnapTarget> snapTargets;
					xr_vector<Frect> ovRects;
					taskWnd->GatherGamepadOverlayTargets(ovRects);
					const bool isMenu = (0 == xr_strcmp(ov, "menu"));
					for (const Frect& r : ovRects)
					{
						SCursorSnapTarget st;
						st.cx = (r.x1 + r.x2) * 0.5f;
						st.cy = (r.y1 + r.y2) * 0.5f;
						st.w = r.width();
						st.h = r.height();
						st.containerId = 1;
						st.kind = isMenu ? "prop" : "pda_overlay";
						snapTargets.push_back(st);
					}
					if (!snapTargets.empty())
					{
						const int bestIdx = SelectBestDpadSnapTarget(curPos, 1, dir, snapTargets.data(), static_cast<int>(snapTargets.size()));
						if (bestIdx >= 0 && bestIdx < static_cast<int>(snapTargets.size()))
						{
							Fvector2 newPos;
							newPos.set(snapTargets[bestIdx].cx, snapTargets[bestIdx].cy);
							if (psActorFlags.test(AF_3D_PDA))
							{
								clamp(newPos.x, pda.m_cursor_box.x1, pda.m_cursor_box.x2);
								clamp(newPos.y, pda.m_cursor_box.y1, pda.m_cursor_box.y2);
							}
							else
							{
								clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
								clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);
							}
							SnapPdaCursor(newPos);
							m_vcRemX = m_vcRemY = 0.0f;
							m_bVcTargetHit = true;
							m_vcTargetCx = newPos.x;
							m_vcTargetCy = newPos.y;
							m_vcTargetW = snapTargets[bestIdx].w;
							m_vcTargetH = snapTargets[bestIdx].h;
							m_vcTargetKind = snapTargets[bestIdx].kind;
							m_vcLastDpadSnapDir = dir;
							++m_vcDpadSnapCount;
							++m_vcMoveCount;
							return true;
						}
					}
				}
			}
			return false;
		}
	}

	if (g_ai_space)
	{
		::luabind::functor<LPCSTR> fn;
		if (ai().script_engine().functor("gp_bridge.cursor_dpad_snap", fn))
		{
			try
			{
				LPCSTR res = fn(curPos.x, curPos.y, static_cast<int>(dir));
				if (res && strstr(res, "snap:1;") == res)
				{
					float nx = curPos.x, ny = curPos.y;
					char kindBuf[64] = "cell";
					if (sscanf(res, "snap:1;cx=%f;cy=%f;kind=%63s", &nx, &ny, kindBuf) >= 2)
					{
						Fvector2 newPos;
						newPos.set(nx, ny);
						clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
						clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);

						if (pInput)
							pInput->NotifyGamepadCursorMoved(nowContinual);

						GetUICursor().SetUICursorPosition(newPos);
						m_vcRemX = m_vcRemY = 0.0f;
						m_bVcTargetHit = true;
						m_vcTargetCx = newPos.x;
						m_vcTargetCy = newPos.y;
						m_vcTargetKind = kindBuf;
						m_vcLastDpadSnapDir = dir;
						++m_vcDpadSnapCount;
						++m_vcMoveCount;

						if (CUIGameCustom* gui = CurrentGameUI())
						{
							if (CUIDialogWnd* tir = gui->TopInputReceiver())
							{
								if (tir->IR_process() && (GetUICursor().IsVisible() || !tir->NeedCursor()))
								{
									tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
								}
							}
						}
						// INV-REWORK-2: сразу берём размер новой цели — рамка едет и меняет размер одним движением
						if (m_bInvFocusFrame)
							RefreshCursorTargetFromLua(newPos, nowContinual);
						return true;
					}
				}
			}
			catch (...)
			{
				Msg("! [Gamepad] Exception in gp_bridge.cursor_dpad_snap");
			}
		}

		// FIX-9: вызов gp_screens.cursor_dpad_snap(dir, curPos.x, curPos.y) -> "1;%f;%f" для всех скриптовых экранов и диалогов
		::luabind::functor<LPCSTR> fnScreensSnap;
		if (ai().script_engine().functor("gp_screens.cursor_dpad_snap", fnScreensSnap))
		{
			try
			{
				LPCSTR res = fnScreensSnap(static_cast<int>(dir), curPos.x, curPos.y);
				float nx = curPos.x, ny = curPos.y;
				if (res && sscanf(res, "1;%f;%f", &nx, &ny) == 2)
				{
					Fvector2 newPos;
					newPos.set(nx, ny);
					clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
					clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);

					if (pInput)
						pInput->NotifyGamepadCursorMoved(nowContinual);

					GetUICursor().SetUICursorPosition(newPos);
					m_vcRemX = m_vcRemY = 0.0f;
					m_bVcTargetHit = true;
					m_vcTargetCx = newPos.x;
					m_vcTargetCy = newPos.y;
					m_vcTargetKind = "screen_lua";
					m_vcLastDpadSnapDir = dir;
					++m_vcDpadSnapCount;
					++m_vcMoveCount;

					CDialogHolder* holder = (MainMenu() != nullptr && MainMenu()->IsActive())
					                            ? static_cast<CDialogHolder*>(MainMenu())
					                            : static_cast<CDialogHolder*>(CurrentGameUI());
					if (CUIDialogWnd* tir = holder ? holder->TopInputReceiver() : nullptr)
					{
						if (tir->IR_process() && (GetUICursor().IsVisible() || !tir->NeedCursor()))
							tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
					}
					// MENU-FOCUS-1: сразу берём точный прямоугольник новой цели, чтобы белая рамка не мелькала старым размером
					if (m_currentContext == EGamepadContext::MainMenu)
						RefreshCursorTargetFromLua(newPos, nowContinual);
					return true;
				}
			}
			catch (...)
			{
				Msg("! [Gamepad] Exception in gp_screens.cursor_dpad_snap");
			}
		}
	}

	// F5.1 + FIX-9: C++ обход всех интерактивных контролов в диалогах/меню/MCM (глубина до 20)
	if (!CGamepadOnScreenKeyboardUI::Instance().IsOpen())
	{
		CDialogHolder* holder = (MainMenu() != nullptr && MainMenu()->IsActive())
		                            ? static_cast<CDialogHolder*>(MainMenu())
		                            : static_cast<CDialogHolder*>(CurrentGameUI());
		CUIDialogWnd* tir = holder ? holder->TopInputReceiver() : nullptr;
		if (tir && tir->IsShown())
		{
			xr_vector<SCursorSnapTarget> snapTargets;
			auto addTargetWnd = [&](CUIWindow* ctrl, LPCSTR kindTag, int containerId = 1)
			{
				if (!ctrl || !ctrl->IsShown() || !ctrl->IsEnabled())
					return;
				Frect r;
				ctrl->GetAbsoluteRect(r);
				if (r.width() >= 10.0f && r.height() >= 8.0f &&
				    r.x2 > 0.0f && r.y2 > 0.0f && r.x1 < UI_BASE_WIDTH && r.y1 < UI_BASE_HEIGHT)
				{
					SCursorSnapTarget st;
					st.cx = (r.x1 + r.x2) * 0.5f;
					st.cy = (r.y1 + r.y2) * 0.5f;
					st.w = r.width();
					st.h = r.height();
					st.containerId = containerId;
					st.kind = kindTag;
					snapTargets.push_back(st);
				}
			};
			auto collectTargets = [&](auto& self, CUIWindow* w, int depth) -> void
			{
				if (!w || !w->IsShown() || depth > 20)
					return;
				if (CUIMMShniaga* shn = smart_cast<CUIMMShniaga*>(w))
				{
					xr_vector<CUIWindow*> shnBtns;
					CUIMMShniagaButtonsProbe::GatherButtons(shn, shnBtns);
					for (CUIWindow* b : shnBtns)
						addTargetWnd(b, "shniaga_btn", 1);
				}
				else if (smart_cast<CUICustomEdit*>(w) != nullptr)
					addTargetWnd(w, "editbox", 1);
				else if (smart_cast<CUICheckButton*>(w) != nullptr)
					addTargetWnd(w, "check_btn", 1);
				else if (smart_cast<CUIRadioButton*>(w) != nullptr)
					addTargetWnd(w, "radio_btn", 1);
				else if (smart_cast<CUITabButton*>(w) != nullptr)
					addTargetWnd(w, "tab_btn", 2);
				else if (smart_cast<CUI3tButton*>(w) != nullptr)
					addTargetWnd(w, "dialog_btn", 1);
				else if (smart_cast<CUIButton*>(w) != nullptr)
					addTargetWnd(w, "button", 1);
				else if (smart_cast<CUIComboBox*>(w) != nullptr)
					addTargetWnd(w, "combobox", 1);
				else if (smart_cast<CUITrackBar*>(w) != nullptr)
					addTargetWnd(w, "trackbar", 1);
				else if (smart_cast<CUIListBoxItem*>(w) != nullptr)
					addTargetWnd(w, "listbox_item", 3);
				else if (CUIListBox* lb = smart_cast<CUIListBox*>(w))
				{
					if (lb->GetSize() == 0)
						addTargetWnd(w, "listbox", 3);
				}

				for (auto it = w->GetChildWndList().begin(); it != w->GetChildWndList().end(); ++it)
					self(self, *it, depth + 1);
			};
			collectTargets(collectTargets, tir, 0);

			if (!snapTargets.empty())
			{
				const int bestIdx = SelectBestDpadSnapTarget(curPos, 1, dir, snapTargets.data(), static_cast<int>(snapTargets.size()));
				if (bestIdx >= 0 && bestIdx < static_cast<int>(snapTargets.size()))
				{
					Fvector2 newPos;
					newPos.set(snapTargets[bestIdx].cx, snapTargets[bestIdx].cy);
					clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
					clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);

					if (pInput)
						pInput->NotifyGamepadCursorMoved(nowContinual);

					GetUICursor().SetUICursorPosition(newPos);
					m_vcRemX = m_vcRemY = 0.0f;
					m_bVcTargetHit = true;
					m_vcTargetCx = newPos.x;
					m_vcTargetCy = newPos.y;
					m_vcTargetW = snapTargets[bestIdx].w;
					m_vcTargetH = snapTargets[bestIdx].h;
					m_vcTargetKind = snapTargets[bestIdx].kind;
					m_vcLastDpadSnapDir = dir;
					++m_vcDpadSnapCount;
					++m_vcMoveCount;

					if (tir->IR_process() && (GetUICursor().IsVisible() || !tir->NeedCursor()))
						tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
					return true;
				}
			}
		}
	}

	return false;
}

bool CGamepadContextDispatcher::IsDialogSelectMode() const
{
	return m_currentContext == EGamepadContext::Dialog_NPC && m_bVcActive && pInput != nullptr && pInput->IsGamepadActiveSource();
}

// INV-REWORK-21 (диалог Д1, Миша 2026-11-12): выбор реплик по строкам; A/RT кликают через MOUSE_1 по курсору,
// который здесь каждый кадр ставится в центр выбранной реплики (белый hover даёт сама кнопка реплики).
void CGamepadContextDispatcher::StepDialogSelect(CUIDialogWnd* tir, u32 dtMs, u32 nowContinual)
{
	CUIGameSP* gsp = smart_cast<CUIGameSP*>(CurrentGameUI());
	if (!gsp || !gsp->TalkMenu || !gsp->TalkMenu->IsShown() || !pInput)
		return;
	CUITalkDialogWnd* dlg = gsp->TalkMenu->GetDialogWnd();
	if (!dlg)
		return;
	ENavDir navDir = ENavDir::None;
	bool navRepeat = false;
	LPCSTR navSrc = "none";
	if (IsNavDirTriggeredThisFrame(&navDir, &navRepeat, &navSrc) && navSrc &&
	    (strstr(navSrc, "dpad_") == navSrc || strstr(navSrc, "lstick_") == navSrc))
	{
		if (navDir == ENavDir::Up)
			dlg->GpStep(-1);
		else if (navDir == ENavDir::Down)
			dlg->GpStep(+1);
	}
	const CInput::sxr_gamepad_state& gpd = pInput->GetGamepadState();
	const gpmath::Vec2 rs = gpmath::ProcessStick(gpmath::GetCalibrationProfile().ui_stick,
	                                             gpmath::NormalizeStickAxis(gpd.axes[2]),
	                                             gpmath::NormalizeStickAxis(gpd.axes[3]));
	dlg->GpUpdate(static_cast<float>(dtMs), rs.y);
	Fvector2 pos;
	if (dlg->GpSelectedCenter(pos))
	{
		const Fvector2 cur = GetUICursor().GetCursorPosition();
		if (_abs(cur.x - pos.x) > 0.5f || _abs(cur.y - pos.y) > 0.5f)
		{
			pInput->NotifyGamepadCursorMoved(nowContinual);
			GetUICursor().SetUICursorPosition(pos);
		}
		tir->OnMouseAction(pos.x, pos.y, WINDOW_MOUSE_MOVE);
	}
	m_bVcTargetHit = false;
}

// E5.2b + FG.1: виртуальный курсор геймпада:
// - в RadialWheel (Interactive_Pie, Camo_3DSS, Ammo, Companion_Flat): правый стик двигает курсор;
// - в MenuUI_Modal (UIInventory и другие курсорные модальные окна, кроме тестового окна gp_focus_test):
//   левый стик двигает курсор с автозадержкой (55% торможение) и мягким притяжением к центру ячейки/кнопки,
//   правый стик (вертикаль) плавно крутит списки CUIScrollView под курсором по Device.dwTimeContinual,
//   а крестовина (D-Pad Up/Down/Left/Right) прыгает по ячейкам и строкам контекстного меню с автопрокруткой.
CUIDialogWnd* CGamepadContextDispatcher::GetActiveModalDialog(CDialogHolder** outHolder) const
{
	if (outHolder)
		*outHolder = nullptr;
	if (!m_bInSelfTest && Console != nullptr && Console->bVisible)
		return nullptr;

	CDialogHolder* holder = nullptr;
	if (MainMenu() != nullptr && MainMenu()->IsActive())
		holder = MainMenu();
	else if (CurrentGameUI() != nullptr)
		holder = CurrentGameUI();

	if (!holder)
		return nullptr;

	CUIDialogWnd* tir = holder->TopInputReceiver();
	if (!tir || !tir->IsShown() || !tir->IR_process())
		return nullptr;

	if (outHolder)
		*outHolder = holder;
	return tir;
}

void CGamepadContextDispatcher::ReleaseActiveMouseModeInjections()
{
	CUIDialogWnd* curTir = GetActiveModalDialog();
	const bool sameOwnerStillActive = (curTir != nullptr && (m_pMouseModeOwnerTir == nullptr || curTir == m_pMouseModeOwnerTir));

	for (u32 btn = 0; btn < VBTN_TOTAL_COUNT; ++btn)
	{
		const int activeDik = m_activeInjectedDik[btn];
		const int activeAct = m_activeInjectedAction[btn];
		if (activeDik <= 0)
			continue;
		if (activeDik != MOUSE_1 && activeDik != MOUSE_2)
			continue;

		if (sameOwnerStillActive && !m_bInSelfTest)
		{
			InjectKeyboardEvent(activeDik, activeAct, "release", m_activeInjectedCtx[btn], false);
		}
		else if (pInput)
		{
			pInput->SetGamepadInjectedKeyState(activeDik, false);
		}
		m_activeInjectedDik[btn] = 0;
		m_activeInjectedAction[btn] = kNOTBINDED;
	}
}

void CGamepadContextDispatcher::UpdateMouseModeHintBarCache(u32 nowContinual, bool force)
{
	if (!force && m_mouseModeLastHintPollMs != 0 && nowContinual >= m_mouseModeLastHintPollMs &&
	    (nowContinual - m_mouseModeLastHintPollMs) < 200)
	{
		return;
	}
	m_mouseModeLastHintPollMs = (nowContinual != 0) ? nowContinual : 1;

	char formatted[512] = {};
	if (g_ai_space && !m_bInSelfTest)
	{
		::luabind::functor<LPCSTR> fnHint;
		if (ai().script_engine().functor("gp_cursor.get_hint_bar_text", fnHint))
		{
			try
			{
				LPCSTR luaStr = fnHint();
				if (luaStr && luaStr[0])
					xr_strcpy(formatted, sizeof(formatted), luaStr);
			}
			catch (...)
			{
				formatted[0] = '\0';
			}
		}
	}

	if (!formatted[0])
	{
		// Локализованный fallback (Windows-1251 для pFontLetterica16Russian)
		const gpbind::BindingTrigger* trig = gpbind::GetActiveBindingProfile().FindBinding(
		    gpbind::ContextId::MenuUI, gpbind::ActionId::MouseCursor);
		const char* toggleTag = "R3";
		char customTag[48] = {};
		if (trig && trig->button != gpbind::ButtonId::None)
		{
			if (trig->type == gpbind::TriggerType::Button)
			{
				toggleTag = gpbind::BindingProfileModel::ButtonToString(trig->button);
			}
			else if (trig->type == gpbind::TriggerType::Hold)
			{
				xr_sprintf(customTag, sizeof(customTag), "Hold %s",
				           gpbind::BindingProfileModel::ButtonToString(trig->button));
				toggleTag = customTag;
			}
			else if (trig->type == gpbind::TriggerType::Chord && trig->modifier != gpbind::ButtonId::None)
			{
				xr_sprintf(customTag, sizeof(customTag), "%s+%s",
				           gpbind::BindingProfileModel::ButtonToString(trig->modifier),
				           gpbind::BindingProfileModel::ButtonToString(trig->button));
				toggleTag = customTag;
			}
		}

		const shared_str tr = CStringTable().translate("ui_gp_cursor_hint_bar");
		if (tr.size() > 0 && 0 != xr_strcmp(tr.c_str(), "ui_gp_cursor_hint_bar"))
		{
			xr_strcpy(formatted, sizeof(formatted), tr.c_str());
		}
		else if (CStringTable::getPData() != nullptr && CStringTable::getPData()->m_sLanguage.size() > 0 &&
		         0 == _stricmp(CStringTable::getPData()->m_sLanguage.c_str(), "rus"))
		{
			// "[LS] Курсор   [A] ЛКМ   [X] ПКМ   [RS] Прокрутка   [B / <R3>] Выйти" в CP1251
			xr_sprintf(formatted, sizeof(formatted),
			           "[LS] \xCA\xF3\xF0\xF1\xEE\xF0   [A] \xCB\xCA\xCC   [X] \xCF\xCA\xCC   [RS] \xCF\xF0\xEE\xEA\xF0\xF3\xF2\xEA\xE0   [B / %s] \xC2\xFB\xE9\xF2\xE8",
			           toggleTag);
		}
		else
		{
			xr_sprintf(formatted, sizeof(formatted),
			           "[LS] Cursor   [A] LMB   [X] RMB   [RS] Scroll   [B / %s] Exit",
			           toggleTag);
		}
	}

	const LONG curIdx = m_mouseModeHintActiveIdx & 1;
	const LONG nextIdx = curIdx ^ 1;
	xr_strcpy(m_mouseModeHintBuf[nextIdx], sizeof(m_mouseModeHintBuf[nextIdx]), formatted);
	InterlockedExchange(&m_mouseModeHintActiveIdx, nextIdx);
}

LPCSTR CGamepadContextDispatcher::GetMouseModeHintBarText() const
{
	const LONG idx = m_mouseModeHintActiveIdx & 1;
	return m_mouseModeHintBuf[idx];
}

bool CGamepadContextDispatcher::CanEnableGamepadMouseMode(CUIDialogWnd* tir) const
{
	if (!tir)
		return false;

	if (!m_bInSelfTest && Console != nullptr && Console->bVisible)
		return false;

	if (IsInventoryModalActive() ||
	    IsWorkshopModalActive() ||
	    NewGameDialog() != nullptr ||
	    IsLightingDialog(tir) ||
	    CGamepadOnScreenKeyboardUI::Instance().IsOpen() ||
	    CGamepadCalibrationUI::Instance().IsOpen() ||
	    CGamepadRemapUI::Instance().IsOpen() ||
	    CPauseTokenManager::Instance().IsFocusTestWindowOpen() ||
	    IsPdaInteractiveActive() ||
	    m_currentContext == EGamepadContext::Console ||
	    m_currentContext == EGamepadContext::Dialog_NPC ||
	    m_currentContext == EGamepadContext::Sleep ||
	    m_currentContext == EGamepadContext::World_3DPDA ||
	    (m_currentContext == EGamepadContext::RadialWheel && m_luaHints.radialWheelType != ERadialWheelType::None))
	{
		return false;
	}

	if (g_ai_space && !m_bInSelfTest)
	{
		::luabind::functor<bool> fnBtnTest;
		if (ai().script_engine().functor("ui_gp_button_test.is_open", fnBtnTest))
		{
			try
			{
				if (fnBtnTest())
					return false;
			}
			catch (...)
			{
			}
		}

		::luabind::functor<bool> fnCan;
		if (ai().script_engine().functor("gp_cursor.can_toggle_mouse_mode", fnCan))
		{
			try
			{
				return fnCan();
			}
			catch (...)
			{
				return false;
			}
		}
	}

	return !IsMenuFocusMode();
}

void CGamepadContextDispatcher::ValidateGamepadMouseModeAutoReset()
{
	if (!m_bGamepadMouseMode || m_bInSelfTest)
		return;

	CUIDialogWnd* tir = GetActiveModalDialog();
	const bool badContext =
	    (m_currentContext == EGamepadContext::Console) ||
	    (m_currentContext == EGamepadContext::World_Gameplay) ||
	    (m_currentContext == EGamepadContext::World_VehicleOrHolder) ||
	    (m_currentContext == EGamepadContext::Dialog_NPC) ||
	    (m_currentContext == EGamepadContext::Sleep) ||
	    (m_currentContext == EGamepadContext::RadialWheel && m_luaHints.radialWheelType != ERadialWheelType::None);

	if (tir == nullptr ||
	    (m_pMouseModeOwnerTir != nullptr && tir != m_pMouseModeOwnerTir) ||
	    (Console != nullptr && Console->bVisible) ||
	    CGamepadOnScreenKeyboardUI::Instance().IsOpen() ||
	    CGamepadCalibrationUI::Instance().IsOpen() ||
	    CGamepadRemapUI::Instance().IsOpen() ||
	    badContext)
	{
		SetGamepadMouseMode(false, "window_closed_or_changed");
	}
}

bool CGamepadContextDispatcher::SetGamepadMouseMode(bool enable, LPCSTR reason, CUIDialogWnd* explicitOwnerTir)
{
	if (!enable)
	{
		const bool wasEnabled = m_bGamepadMouseMode;
		if (wasEnabled)
			ReleaseActiveMouseModeInjections();

		m_bGamepadMouseMode = false;
		m_pMouseModeOwnerTir = nullptr;
		m_mouseModeHoldStartMs = 0;
		m_bMouseModeHoldFired = false;

		if (wasEnabled && !m_bInSelfTest)
		{
			if (pInput)
			{
				const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
				if (gp.connected)
				{
					const gpmath::Vec2 ls = gpmath::ProcessStick(
					    gpmath::GetCalibrationProfile().left_stick,
					    gpmath::NormalizeStickAxis(gp.axes[0]),
					    gpmath::NormalizeStickAxis(gp.axes[1]));
					if ((ls.x * ls.x + ls.y * ls.y) > 0.04f)
					{
						m_bStickMoveConsumedUntilNeutral = true;
						m_bStickNavConsumedUntilNeutral = true;
					}
				}
			}

			if (g_ai_space)
			{
				::luabind::functor<void> fnNotify;
				if (ai().script_engine().functor("gp_cursor.on_mouse_mode_changed", fnNotify))
				{
					try { fnNotify(false, reason ? reason : "off"); } catch (...) {}
				}
			}
		}
		return true;
	}

	CUIDialogWnd* tir = explicitOwnerTir ? explicitOwnerTir : GetActiveModalDialog();
	if (!tir || !CanEnableGamepadMouseMode(tir))
		return false;

	if (!m_bInSelfTest && g_actor)
	{
		g_actor->m_vGamepadMove.set(0.0f, 0.0f);
		g_actor->m_bGamepadMoveActive = false;
		g_actor->StopAnyMove();
	}

	m_bGamepadMouseMode = true;
	m_pMouseModeOwnerTir = tir;
	m_mouseModeHoldStartMs = 0;
	m_bMouseModeHoldFired = false;
	m_bVcLocked = m_bVcHidden = false;
	m_bInvFocusFrame = false;
	m_bInvPending = false;
	m_vcRemX = m_vcRemY = 0.0f;
	m_vcScrollAccum = 0.0f;

	if (!m_bInSelfTest)
	{
		const u32 now = Device.dwTimeContinual;
		if (pInput)
		{
			if (!pInput->IsGamepadActiveSource())
				pInput->ForceActiveInputSource(CInput::EActiveInputSource::Gamepad, "mouse_mode_on");
			pInput->NotifyGamepadCursorMoved(now);
		}

		GetUICursor().Show();
		Fvector2 cp = GetUICursor().GetCursorPosition();
		if (cp.x <= 1.0f || cp.x >= UI_BASE_WIDTH - 1.0f || cp.y <= 1.0f || cp.y >= UI_BASE_HEIGHT - 1.0f)
		{
			cp.set(UI_BASE_WIDTH * 0.5f, UI_BASE_HEIGHT * 0.5f);
			GetUICursor().SetUICursorPosition(cp);
		}
		if (tir->IR_process())
			tir->OnMouseAction(cp.x, cp.y, WINDOW_MOUSE_MOVE);

		UpdateMouseModeHintBarCache(now, true);

		if (g_ai_space)
		{
			::luabind::functor<void> fnNotify;
			if (ai().script_engine().functor("gp_cursor.on_mouse_mode_changed", fnNotify))
			{
				try { fnNotify(true, reason ? reason : "on"); } catch (...) {}
			}
		}
	}
	else
	{
		UpdateMouseModeHintBarCache(1, true);
	}

	return true;
}

bool CGamepadContextDispatcher::StepGamepadMouseModeToggle(u32 nowContinual)
{
	if (!AllowsGamepadButtons())
	{
		m_mouseModeHoldStartMs = 0;
		m_bMouseModeHoldFired = false;
		return false;
	}

	const gpbind::BindingTrigger fallbackTrig = gpbind::BindingTrigger::MakeButton(gpbind::ButtonId::RightStick);
	const gpbind::BindingTrigger* trig = gpbind::GetActiveBindingProfile().FindBinding(
	    gpbind::ContextId::MenuUI, gpbind::ActionId::MouseCursor);
	if (!trig)
		trig = &fallbackTrig;
	if (trig->button == gpbind::ButtonId::None)
		return false;

	const u32 actBtn = static_cast<u32>(trig->button);
	if (actBtn >= VBTN_TOTAL_COUNT)
		return false;

	bool fired = false;
	if (trig->type == gpbind::TriggerType::Button)
	{
		m_mouseModeHoldStartMs = 0;
		m_bMouseModeHoldFired = false;
		if (IsButtonPressed(actBtn))
		{
			bool shadowedByMenuChord = false;
			for (const auto& entry : gpbind::GetActiveBindingProfile().GetAllBindings())
			{
				if (entry.context == gpbind::ContextId::MenuUI &&
				    entry.action != gpbind::ActionId::MouseCursor &&
				    entry.trigger.type == gpbind::TriggerType::Chord &&
				    entry.trigger.button == trig->button &&
				    entry.trigger.modifier != gpbind::ButtonId::None)
				{
					const u32 modBit = static_cast<u32>(entry.trigger.modifier);
					if (modBit < VBTN_TOTAL_COUNT && (m_effectiveDownMask & (1u << modBit)) != 0)
					{
						shadowedByMenuChord = true;
						break;
					}
				}
			}
			fired = !shadowedByMenuChord;
		}
	}
	else if (trig->type == gpbind::TriggerType::Chord)
	{
		m_mouseModeHoldStartMs = 0;
		m_bMouseModeHoldFired = false;
		const u32 modBtn = static_cast<u32>(trig->modifier);
		if (modBtn < VBTN_TOTAL_COUNT && (m_effectiveDownMask & (1u << modBtn)) != 0 && IsButtonPressed(actBtn))
			fired = true;
	}
	else if (trig->type == gpbind::TriggerType::Hold)
	{
		const bool downNow = (m_effectiveDownMask & (1u << actBtn)) != 0;
		if (!downNow)
		{
			m_mouseModeHoldStartMs = 0;
			m_bMouseModeHoldFired = false;
		}
		else if (m_bGamepadMouseMode || GetActiveModalDialog() != nullptr)
		{
			if (m_mouseModeHoldStartMs == 0)
			{
				m_mouseModeHoldStartMs = (nowContinual != 0) ? nowContinual : 1;
				m_bMouseModeHoldFired = false;
			}
			else if (!m_bMouseModeHoldFired)
			{
				const u32 reqMs = (trig->hold_ms > 0) ? trig->hold_ms : gpbind::limits::kDefaultHoldMs;
				if (nowContinual >= m_mouseModeHoldStartMs && (nowContinual - m_mouseModeHoldStartMs) >= reqMs)
				{
					m_bMouseModeHoldFired = true;
					fired = true;
				}
			}
		}
	}

	if (!fired)
		return false;

	if (m_bGamepadMouseMode)
	{
		SetGamepadMouseMode(false, "toggle_button");
		ConsumeButtonUntilRelease(actBtn);
		return true;
	}

	CUIDialogWnd* tir = GetActiveModalDialog();
	if (!tir)
	{
		// В мире без открытого окна НЕ потребляем кнопку (R3 опускает оружие / переключает прицел штатно)
		return false;
	}

	if (CanEnableGamepadMouseMode(tir))
	{
		if (SetGamepadMouseMode(true, "toggle_button", tir))
		{
			ConsumeButtonUntilRelease(actBtn);
			return true;
		}
		return false;
	}

	// Окно открыто, но оно поддерживается нативно (режим «мышь» запрещён):
	// кроме UIInventory (где R3 переключает статы), 3D-КПК и экранов калибровки/ремапа/теста кнопок — мягкая вибрация отказа.
	if (!IsInventoryModalActive() &&
	    m_currentContext != EGamepadContext::World_3DPDA &&
	    !CGamepadCalibrationUI::Instance().IsOpen() &&
	    !CGamepadRemapUI::Instance().IsOpen() &&
	    !CGamepadOnScreenKeyboardUI::Instance().IsOpen())
	{
		bool inBtnTest = false;
		bool luaHandledDeny = false;
		if (g_ai_space && !m_bInSelfTest)
		{
			::luabind::functor<bool> fnBtnTest;
			if (ai().script_engine().functor("ui_gp_button_test.is_open", fnBtnTest))
			{
				try { inBtnTest = fnBtnTest(); } catch (...) { inBtnTest = false; }
			}
			if (!inBtnTest)
			{
				::luabind::functor<void> fnDeny;
				if (ai().script_engine().functor("gp_cursor.on_denied_toggle", fnDeny))
				{
					try
					{
						fnDeny();
						luaHandledDeny = true;
					}
					catch (...)
					{
					}
				}
			}
		}
		if (!inBtnTest && !luaHandledDeny && !m_bInSelfTest && pInput && pInput->GetGamepadHaptics())
		{
			pInput->GetGamepadHaptics()->PulseMenu("menu_deny");
		}
	}
	return false;
}

void CGamepadContextDispatcher::StepVirtualCursor(u32 nowContinual)
{
	ValidateGamepadMouseModeAutoReset();

	// CURSOR1: выделенный контур режима виртуальной мыши поверх любого неподдерживаемого модового окна
	if (m_bGamepadMouseMode)
	{
		CDialogHolder* holder = nullptr;
		CUIDialogWnd* mouseTir = GetActiveModalDialog(&holder);
		if (!mouseTir || !holder)
		{
			SetGamepadMouseMode(false, "window_closed_or_changed");
			return;
		}

		UpdateMouseModeHintBarCache(nowContinual, false);
		m_bInvFocusFrame = false;
		m_bInvPending = false;
		m_bVcLocked = m_bVcHidden = false;
		m_bPdaEyeSpotHit = false;
		m_bVcInModalMenu = true;
		if (!GetUICursor().IsVisible())
			GetUICursor().Show();

		const u32 prevTime = m_vcLastTime;
		m_vcLastTime = nowContinual;
		if (!m_bVcActive)
		{
			m_bVcActive = true;
			m_vcRemX = m_vcRemY = 0.0f;
			m_vcScrollAccum = 0.0f;
			RefreshCursorTargetFromLua(GetUICursor().GetCursorPosition(), nowContinual);
			return;
		}

		u32 dtMs = (nowContinual >= prevTime) ? (nowContinual - prevTime) : 0;
		if (dtMs > 50)
			dtMs = 50;

		if (!pInput)
			return;
		const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
		if (!gp.connected || !gp.has_focus)
			return;

		const gpmath::StickConfig& uiCfg = gpmath::GetCalibrationProfile().ui_stick;
		const gpmath::Vec2 moveStick = gpmath::ProcessStick(
		    uiCfg,
		    gpmath::NormalizeStickAxis(gp.axes[0]),
		    gpmath::NormalizeStickAxis(gp.axes[1]));
		const float moveMag = _sqrt(moveStick.x * moveStick.x + moveStick.y * moveStick.y);
		if (moveMag >= 0.05f)
			m_vcLastStickMoveTime = nowContinual;

		const Fvector2 curPos = GetUICursor().GetCursorPosition();
		if (moveMag >= 0.02f || (nowContinual - m_vcLastTargetQueryTime) >= 30)
			RefreshCursorTargetFromLua(curPos, nowContinual);

		const u32 sinceStickMs = (m_vcLastStickMoveTime != 0 && nowContinual >= m_vcLastStickMoveTime)
		                             ? (nowContinual - m_vcLastStickMoveTime)
		                             : 999999u;
		bool bMoved = false;
		bool bSoftSnapped = false;
		Fvector2 newPos = StepCursorKinematics(
		    curPos,
		    moveStick,
		    dtMs,
		    m_bVcTargetHit,
		    Fvector2().set(m_vcTargetCx, m_vcTargetCy),
		    sinceStickMs,
		    m_vcRemX,
		    m_vcRemY,
		    &bMoved,
		    &bSoftSnapped,
		    true);

		// Попиксельная подводка крестовиной (D-Pad ~140 px/s * ps_gp_cursor_speed) без прыжков TryDpadSnapInModalUI
		const float dxDpad = ((m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_RIGHT)) ? 1.0f : 0.0f) -
		                     ((m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_LEFT)) ? 1.0f : 0.0f);
		const float dyDpad = ((m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_DOWN)) ? 1.0f : 0.0f) -
		                     ((m_effectiveDownMask & (1u << CInput::GP_BTN_DPAD_UP)) ? 1.0f : 0.0f);
		if (_abs(dxDpad) > 0.0f || _abs(dyDpad) > 0.0f)
		{
			const float diagNorm = (_abs(dxDpad) > 0.0f && _abs(dyDpad) > 0.0f) ? 0.70710678f : 1.0f;
			const float nudgeSpeed = 140.0f * clampr(ps_gp_cursor_speed, 0.4f, 2.5f) * (static_cast<float>(dtMs) * 0.001f);
			m_vcRemX += dxDpad * diagNorm * nudgeSpeed;
			m_vcRemY += dyDpad * diagNorm * nudgeSpeed;
			const int ndx = static_cast<int>(m_vcRemX);
			const int ndy = static_cast<int>(m_vcRemY);
			if (ndx != 0 || ndy != 0)
			{
				m_vcRemX -= static_cast<float>(ndx);
				m_vcRemY -= static_cast<float>(ndy);
				newPos.x = clampr(newPos.x + static_cast<float>(ndx), 0.0f, UI_BASE_WIDTH);
				newPos.y = clampr(newPos.y + static_cast<float>(ndy), 0.0f, UI_BASE_HEIGHT);
				bMoved = (_abs(newPos.x - curPos.x) > 1e-4f) || (_abs(newPos.y - curPos.y) > 1e-4f);
			}
		}

		if (bMoved)
		{
			pInput->NotifyGamepadCursorMoved(nowContinual);
			GetUICursor().SetUICursorPosition(newPos);
			if (mouseTir->IR_process())
				mouseTir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
			++m_vcMoveCount;
			if (bSoftSnapped)
				++m_vcSoftSnapCount;
			RefreshCursorTargetFromLua(newPos, nowContinual);
		}

		// Правый стик (вертикаль) -> колесо мыши в активный holder (MainMenu() или CurrentGameUI())
		const gpmath::Vec2 scrollStick = gpmath::ProcessStick(
		    uiCfg,
		    gpmath::NormalizeStickAxis(gp.axes[2]),
		    gpmath::NormalizeStickAxis(gp.axes[3]));
		const int wheelDir = StepRightStickScrollAccumulator(
		    scrollStick.y, dtMs, nowContinual, m_vcScrollAccum, m_vcLastScrollStepTime);
		if (wheelDir != 0 && holder)
		{
			holder->IR_UIOnMouseWheel(wheelDir);
			++m_vcScrollCount;
		}
		return;
	}

	// INV-REWORK-21: диалог с НПС — при вводе с мыши/клавиатуры реплики возвращаются в исходный вид
	if (m_currentContext == EGamepadContext::Dialog_NPC && pInput && !pInput->IsGamepadActiveSource())
	{
		CUIGameSP* gspR = smart_cast<CUIGameSP*>(CurrentGameUI());
		if (gspR && gspR->TalkMenu && gspR->TalkMenu->GetDialogWnd())
			gspR->TalkMenu->GetDialogWnd()->GpReset();
	}
	const bool inWheelVc = (m_currentContext == EGamepadContext::RadialWheel) &&
	                       IsVirtualCursorWheelType(m_luaHints.radialWheelType);
	const bool inPdaVc = IsPdaInteractiveActive();
	const bool inMainMenuVc = (m_currentContext == EGamepadContext::MainMenu &&
	                           !CGamepadOnScreenKeyboardUI::Instance().IsOpen());
	const bool inModalVc = ((((m_currentContext == EGamepadContext::MenuUI_Modal) ||
                              IsWorkshopModalActive() ||
	                          (m_currentContext == EGamepadContext::Dialog_NPC)) &&
	                         !CPauseTokenManager::Instance().IsFocusTestWindowOpen() &&
	                         !CGamepadOnScreenKeyboardUI::Instance().IsOpen()) ||
	                        inPdaVc ||
	                        inMainMenuVc);

	LPCSTR pdaActiveSec = (inPdaVc && CurrentGameUI() && CurrentGameUI()->GetPdaMenu().IsShown())
	                          ? CurrentGameUI()->GetPdaMenu().GetActiveSection()
	                          : nullptr;
	if (!pdaActiveSec || 0 != xr_strcmp(pdaActiveSec, "eptTasks"))
	{
		m_bPdaMapFiltersMode = false;
		m_bPdaEyeSpotHit = false;
	}
	else if (CUITaskWnd* tw = CurrentGameUI()->GetPdaMenu().GetTaskWnd())
	{
		if (tw->GetGamepadOverlayName()[0] != '\0')
			m_bPdaMapFiltersMode = false;
	}

	UpdateInventoryFocusFramePending();
	// INV-REWORK-1 + PDA1-E: Lua говорит, активна ли белая рамка фокуса (инвентарь, верстак, вкладки/оверлеи КПК)
	m_bInvFocusFrame = false;
	if (ps_gp_inv_focus_frame != 0 && inModalVc && !inMainMenuVc &&
	    (m_currentContext == EGamepadContext::MenuUI_Modal || IsWorkshopModalActive() || inPdaVc) &&
	    pInput && pInput->IsGamepadActiveSource() && g_ai_space)
	{
		::luabind::functor<bool> fnInv;
		if (ai().script_engine().functor("gp_bridge.inv_focus_frame_active", fnInv))
		{
			try
			{
				m_bInvFocusFrame = fnInv();
			}
			catch (...)
			{
				m_bInvFocusFrame = false;
			}
		}
	}
	if (!IsPdaMapEyeActive())
		m_bPdaEyeSpotHit = false;
	const bool lstickJumps = inMainMenuVc || m_bInvFocusFrame;
	// INV-REWORK-16 (п.6, Миша 2026-11-12): окно «Подробно» (ui_itm_details) поверх инвентаря — LS и
	// крестовина курсор не двигают, A не кликает, курсор не рисуется; RS листает (cursor_rs_analog), B закрывает.
	m_bVcLocked = false;
	if (inModalVc && !inPdaVc && !inMainMenuVc && m_currentContext == EGamepadContext::MenuUI_Modal &&
	    pInput && pInput->IsGamepadActiveSource() && g_ai_space)
	{
		::luabind::functor<bool> fnLock;
		if (ai().script_engine().functor("gp_bridge.cursor_locked", fnLock))
		{
			try
			{
				m_bVcLocked = fnLock();
			}
			catch (...)
			{
				m_bVcLocked = false;
			}
		}
	}
	// FIX-LIVE2 п.20 (Миша 2026-11-16, 20а): карточки учебника (npe_dialog_tutorial/codec) — под геймпадом курсор
	// не рисуется (кнопки ведёт gp_sys_dialogs); движение мыши делает её активным источником и курсор возвращается.
	m_bVcHidden = false;
	if (inModalVc && !inMainMenuVc && pInput && pInput->IsGamepadActiveSource() && g_ai_space)
	{
		::luabind::functor<bool> fnHide;
		if (ai().script_engine().functor("gp_bridge.cursor_hidden", fnHide))
		{
			try { m_bVcHidden = fnHide(); }
			catch (...) { m_bVcHidden = false; }
		}
	}

	// Для нативных колёс выбора оружия (QuickAction_Haru) и патронов (Ammo) скрываем виртуальный курсор геймпада
	if (m_currentContext == EGamepadContext::RadialWheel && !inWheelVc && pInput && pInput->IsGamepadActiveSource())
	{
		if (GetUICursor().IsVisible())
			GetUICursor().Hide();
	}

	bool allowed = (inWheelVc || inModalVc) && !(Console && Console->bVisible);
	CUIGameCustom* gui = nullptr;
	CUIDialogWnd* tir = nullptr;
	if (allowed)
	{
		gui = CurrentGameUI();
		CDialogHolder* holder = (MainMenu() != nullptr && MainMenu()->IsActive())
		                            ? static_cast<CDialogHolder*>(MainMenu())
		                            : static_cast<CDialogHolder*>(gui);
		tir = holder ? holder->TopInputReceiver() : nullptr;
		if ((inPdaVc || inMainMenuVc) && tir != nullptr && !GetUICursor().IsVisible())
			GetUICursor().Show();
		allowed = (tir != nullptr) && tir->IR_process() && (inWheelVc || GetUICursor().IsVisible() || !tir->NeedCursor());
	}
	const bool inLighting = allowed && IsLightingDialog(tir) && !inMainMenuVc;
	if ((!MainMenu() || !MainMenu()->IsActive()) && !inLighting)
	{
		m_menuFocusRoot = m_menuMemoryRoot = nullptr;
		m_menuLoadRoot = m_menuHoldRoot = nullptr;
		m_menuDeleteIndex = -1;
		m_menuLoadArmed = nullptr;
		std::fill(std::begin(m_menuShortcutButtons), std::end(m_menuShortcutButtons), nullptr);
		m_menuFocusOnTrackbar = false;
		m_menuPanelMemory.clear();
	}

	// FIX-LIVE1 п.2: одна строка в лог при каждой смене состояния окна «Создание персонажа» — по логу видно,
	// почему рамка не включилась (контекст, источник ввода, режим мыши, окно сверху).
	{
		static int s_ngLogged = -1;
		CUIDialogWnd* ng = NewGameDialog();
		const int ngState = ng ? (1 | (int(m_currentContext) << 1) | ((pInput && pInput->IsGamepadActiveSource()) ? 256 : 0) |
		                           (m_bGamepadMouseMode ? 512 : 0) | (allowed ? 1024 : 0)) : 0;
		if (ngState != s_ngLogged)
		{
			s_ngLogged = ngState;
			const bool mmActive = MainMenu() && MainMenu()->IsActive();
			CUIDialogWnd* mmTop = mmActive ? MainMenu()->TopInputReceiver() : nullptr;
			Msg("~ [GP-NEWGAME] open=%d ctx=%d gamepad=%d mouse_mode=%d allowed=%d top=%s", ng ? 1 : 0, int(m_currentContext),
			    (pInput && pInput->IsGamepadActiveSource()) ? 1 : 0, m_bGamepadMouseMode ? 1 : 0, allowed ? 1 : 0,
			    mmTop ? mmTop->WindowName().c_str() : "-");
		}
	}

	if (!allowed)
	{
		m_bVcActive = false;
		m_bVcInModalMenu = false;
		m_menuFocusOnTrackbar = false;
		m_pVcLastMainMenuTir = nullptr;
		m_bVcTargetHit = false;
		m_vcRemX = m_vcRemY = 0.0f;
		m_vcScrollAccum = 0.0f;
		return;
	}

	if (inLighting && AllowsGamepadButtons())
	{
		if (!pInput || !pInput->IsGamepadActiveSource())
		{
			m_menuFocusRoot = nullptr;
			m_menuTarget = nullptr;
			m_bVcTargetHit = false;
			return;
		}
		m_bVcActive = m_bVcInModalMenu = true;
		ENavDir dir = ENavDir::None;
		IsNavDirTriggeredThisFrame(&dir, nullptr, nullptr);
		StepMenuFocus(tir, dir, 0, 0, nowContinual);
		return;
	}

	if (inMainMenuVc && NewGameDialog() && pInput && pInput->IsGamepadActiveSource() && g_ai_space)
	{
		m_bVcActive = m_bVcInModalMenu = m_bInvFocusFrame = true;
		const u32 dt = m_vcLastTime ? _min(nowContinual - m_vcLastTime, 50u) : 0;
		m_vcLastTime = nowContinual;
		const auto& pad = pInput->GetGamepadState();
		const auto right = gpmath::ProcessStick(gpmath::GetCalibrationProfile().ui_stick,
			gpmath::NormalizeStickAxis(pad.axes[2]), gpmath::NormalizeStickAxis(pad.axes[3]));
		if (_abs(right.y) >= 0.08f)
		{
			::luabind::functor<bool> scroll;
			if (ai().script_engine().functor("gp_bridge.cursor_rs_analog", scroll))
				try { scroll(right.y, static_cast<float>(dt)); } catch (...) {}
		}
		ENavDir dir = ENavDir::None;
		if (IsNavDirTriggeredThisFrame(&dir, nullptr, nullptr))
		{
			::luabind::functor<LPCSTR> snap;
			if (ai().script_engine().functor("gp_bridge.cursor_dpad_snap", snap))
			{
				const Fvector2 pos = GetUICursor().GetCursorPosition();
				try { snap(pos.x, pos.y, static_cast<int>(dir)); } catch (...) {}
			}
		}
		RefreshCursorTargetFromLua(GetUICursor().GetCursorPosition(), nowContinual);
		return;
	}

	if (inMainMenuVc)
	{
		const bool left = IsButtonPressed(VBTN_LT);
		const bool right = IsButtonPressed(VBTN_RT);
		if (left)
			ConsumeButtonUntilRelease(VBTN_LT);
		if (right)
			ConsumeButtonUntilRelease(VBTN_RT);

		if (!pInput || !pInput->IsGamepadActiveSource())
		{
			m_menuTarget = nullptr;
			m_bVcTargetHit = false;
			m_menuFocusOnTrackbar = false;
			m_vcLastTime = nowContinual;
			m_vcScrollAccum = 0.0f;
			return;
		}
		if (!CUIMMShniagaButtonsProbe::FindShownIn(tir))
		{
			const auto& gp = pInput->GetGamepadState();
			const u32 dt = (m_menuFocusRoot == tir && nowContinual >= m_vcLastTime)
				? nowContinual - m_vcLastTime : 0;
			m_vcLastTime = nowContinual;
			m_bVcActive = m_bVcInModalMenu = true;
			m_pVcLastMainMenuTir = tir;
			ENavDir dir = ENavDir::None;
			int wheel = 0;
			if (gp.connected && gp.has_focus && !gp.need_release_all)
			{
				IsNavDirTriggeredThisFrame(&dir, nullptr, nullptr);
				const auto stick = gpmath::ProcessStick(gpmath::GetCalibrationProfile().ui_stick,
					gpmath::NormalizeStickAxis(gp.axes[2]), gpmath::NormalizeStickAxis(gp.axes[3]));
				wheel = StepRightStickScrollAccumulator(stick.y, dt, nowContinual, m_vcScrollAccum, m_vcLastScrollStepTime);
			}
			const int panelStep = static_cast<int>(right) - static_cast<int>(left);
			if (m_menuFocusRoot != tir || m_menuOpenCombo != GetExpandedMenuCombo(tir) || !m_menuTarget ||
			    dir != ENavDir::None || panelStep || wheel || IsButtonPressed(CInput::GP_BTN_SOUTH) ||
			    IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER) || IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER) ||
			    nowContinual - m_vcLastTargetQueryTime >= 30)
				StepMenuFocus(tir, dir, panelStep, wheel, nowContinual);
			return;
		}

		m_menuFocusRoot = nullptr;
		m_menuMemoryRoot = nullptr;
		m_menuLoadRoot = m_menuHoldRoot = nullptr;
		m_menuDeleteIndex = -1;
		m_menuLoadArmed = nullptr;
		std::fill(std::begin(m_menuShortcutButtons), std::end(m_menuShortcutButtons), nullptr);
		m_menuFocusOnTrackbar = false;
		m_menuPanelMemory.clear();
	}

	if (!inMainMenuVc)
	{
		m_pVcLastMainMenuTir = nullptr;
	}
	else if (tir != m_pVcLastMainMenuTir)
	{
		// В главном меню сохраняем штатную лупу на выбранном пункте.
		if (CUIMMShniaga* shn = CUIMMShniagaButtonsProbe::FindShownIn(tir))
		{
			m_pVcLastMainMenuTir = tir;
			CUIWindow* sel = CUIMMShniagaButtonsProbe::GetSelected(shn);
			if (!sel)
			{
				xr_vector<CUIWindow*> btns;
				CUIMMShniagaButtonsProbe::GatherButtons(shn, btns);
				if (!btns.empty())
					sel = btns[0];
			}
			if (sel)
			{
				Frect sr;
				sel->GetAbsoluteRect(sr);
				Fvector2 selPos;
				selPos.set((sr.x1 + sr.x2) * 0.5f, (sr.y1 + sr.y2) * 0.5f);
				clamp(selPos.x, 0.0f, UI_BASE_WIDTH);
				clamp(selPos.y, 0.0f, UI_BASE_HEIGHT);
				if (pInput)
					pInput->NotifyGamepadCursorMoved(nowContinual);
				GetUICursor().SetUICursorPosition(selPos);
				if (tir->IR_process() && (GetUICursor().IsVisible() || !tir->NeedCursor()))
					tir->OnMouseAction(selPos.x, selPos.y, WINDOW_MOUSE_MOVE);
				RefreshCursorTargetFromLua(selPos, nowContinual);
			}
		}
	}

	// INV-REWORK-14 (п.4): мышь -> геймпад в инвентаре. Пока активна мышь, левый стик НЕ водит игровой
	// курсор (раньше 0,5-1 с кулдауна + 100 мс удержания арбитража Xr_input.cpp курсор ездил стиком).
	// Первое заметное отклонение левого стика сразу переключает источник на геймпад; Lua-слушатель
	// gp_inv_rework.on_source_changed ставит рамку на ближайший к курсору предмет (любой раздел).
	// INV-REWORK-22: то же в диалоге с НПС (раньше стандартный курсор Гаммы ещё рисовался и ездил от стика).
	if (inModalVc && !inMainMenuVc &&
	    ((m_currentContext == EGamepadContext::MenuUI_Modal && IsInventoryModalActive()) ||
         IsWorkshopModalActive() ||
	     inPdaVc ||
	     m_currentContext == EGamepadContext::Dialog_NPC) && // WS1 + PDA1-E: верстак и КПК
	    pInput && !pInput->IsGamepadActiveSource() && !m_bInSelfTest)
	{
		const CInput::sxr_gamepad_state& gpk = pInput->GetGamepadState();
		if (gpk.connected && gpk.has_focus && !gpk.need_release_all)
		{
			const gpmath::Vec2 ls = gpmath::ProcessStick(gpmath::GetCalibrationProfile().ui_stick,
				gpmath::NormalizeStickAxis(gpk.axes[0]), gpmath::NormalizeStickAxis(gpk.axes[1]));
			if (ls.x * ls.x + ls.y * ls.y >= 0.30f * 0.30f)
				pInput->ForceActiveInputSource(CInput::EActiveInputSource::Gamepad, "inv_stick_instant");
		}
		m_bVcActive = false; // следующий кадр геймпада начнётся с dt = 0 и RefreshCursorTargetFromLua
		m_vcRemX = m_vcRemY = 0.0f;
		m_vcScrollAccum = 0.0f;
		m_vcLastTime = nowContinual;
		return;
	}

	const u32 prevTime = m_vcLastTime;
	m_vcLastTime = nowContinual;
	m_bVcInModalMenu = inModalVc;
	if (!m_bVcActive)
	{
		m_bVcActive = true; // первый кадр: dt = 0, защита от скачка
		m_vcRemX = m_vcRemY = 0.0f;
		m_vcScrollAccum = 0.0f;
		if (inModalVc)
			RefreshCursorTargetFromLua(GetUICursor().GetCursorPosition(), nowContinual);
		return;
	}

	u32 dtMs = (nowContinual >= prevTime) ? (nowContinual - prevTime) : 0;
	if (dtMs > 50)
		dtMs = 50;

	if (!pInput)
		return;
	const CInput::sxr_gamepad_state& gp = pInput->GetGamepadState();
	if (!gp.connected || !gp.has_focus)
		return;

	// INV-REWORK-21: диалог с НПС — свой выбор реплик (LS/крестовина шагают, RS листает историю, курсор не рисуется)
	if (m_currentContext == EGamepadContext::Dialog_NPC && tir != nullptr && pInput->IsGamepadActiveSource())
	{
		StepDialogSelect(tir, dtMs, nowContinual);
		return;
	}

	// FIX-LIVE3 п.8/14/16/17/21/22 (Миша 2026-11-16): на карте метка неподвижна в центре видимой области,
	// LS двигает карту инвертированно (стик вправо — взгляд вправо, карта влево), RS вверх/вниз — плавный зум,
	// в покое метка прилипает к ближайшему маркеру (карта подтягивается), крестовина вверх/вниз — список
	// заданий (открывает его), влево/вправо — ничего (по меткам больше не прыгает).
	m_bPdaEyeOnButton = false;
	const bool pdaMapEye = inPdaVc && IsPdaMapEyeActive();
	CUITaskWnd* pdaTaskWnd = (inPdaVc && gui && 0 == xr_strcmp(pdaActiveSec ? pdaActiveSec : "", "eptTasks"))
	                             ? gui->GetPdaMenu().GetTaskWnd()
	                             : nullptr;
	const bool pdaTasksOverlay = pdaTaskWnd && 0 == xr_strcmp(pdaTaskWnd->GetGamepadOverlayName(), "tasks") &&
	                             !m_bPdaMapFiltersMode;
	if ((pdaMapEye || pdaTasksOverlay) && pdaTaskWnd)
	{
		ENavDir navDir = ENavDir::None;
		bool navRepeat = false;
		LPCSTR navSrc = "none";
		if (IsNavDirTriggeredThisFrame(&navDir, &navRepeat, &navSrc) && navSrc && strstr(navSrc, "dpad_") == navSrc)
		{
			if (navDir == ENavDir::Up || navDir == ENavDir::Down)
			{
				Fvector2 snap = GetUICursor().GetCursorPosition();
				const bool snapOk = pdaTaskWnd->GamepadTaskStep(navDir == ENavDir::Up ? -1 : 1, &snap);
				if (snapOk && pdaTasksOverlay)
					SnapPdaCursor(snap);
				else if (snapOk)
					m_pdaTaskSnapPending = true; // список только что открылся: строки ещё не разложены
			}
			return;
		}
		if (pdaTasksOverlay && m_pdaTaskSnapPending)
		{
			m_pdaTaskSnapPending = false;
			Fvector2 snap;
			if (pdaTaskWnd->GamepadTaskStep(0, &snap))
				SnapPdaCursor(snap);
			return;
		}
	}
	if (!pdaTasksOverlay)
		m_pdaTaskSnapPending = false;
	if (!pdaMapEye)
		m_pdaReticleOff.set(0.0f, 0.0f);
	if (pdaMapEye && pdaTaskWnd)
	{
		CUIMapWnd* mapWnd = pdaTaskWnd->GetMapWnd();
		if (mapWnd)
		{
			const Frect mr = mapWnd->ActiveMapRect();
			const Fvector2 center = Fvector2().set((mr.x1 + mr.x2) * 0.5f, (mr.y1 + mr.y2) * 0.5f);
			const gpmath::StickConfig& panCfg = gpmath::GetCalibrationProfile().ui_stick;
			const gpmath::Vec2 ls = gpmath::ProcessStick(panCfg, gpmath::NormalizeStickAxis(gp.axes[0]),
			                                             gpmath::NormalizeStickAxis(gp.axes[1]));
			const gpmath::Vec2 rs = gpmath::ProcessStick(panCfg, gpmath::NormalizeStickAxis(gp.axes[2]),
			                                             gpmath::NormalizeStickAxis(gp.axes[3]));
			const float dtSec = clampr(float(dtMs) * 0.001f, 0.001f, 0.1f);
			const bool lsMove = _abs(ls.x) > 0.02f || _abs(ls.y) > 0.02f;
			bool moved = false;
			// FIX-LIVE4 (Миша 2026-11-16): у края карты метка сходит с центра. MoveMap клипует карту
			// (CUIGlobalMap::ClipByVisRect), поэтому меряем фактический сдвиг; недоставшуюся часть
			// забирает метка (m_pdaReticleOff), ограниченная видимой областью карты минус 8 px.
			const float limX = _max(0.0f, (mr.x2 - mr.x1) * 0.5f - 8.0f);
			const float limY = _max(0.0f, (mr.y2 - mr.y1) * 0.5f - 8.0f);
			auto moveMapOrReticle = [&](const Fvector2& req)
			{
				Fvector2 before, after;
				mapWnd->GlobalMap()->GetAbsolutePos(before);
				mapWnd->MoveMap(req, true);
				mapWnd->GlobalMap()->GetAbsolutePos(after);
				const float leftX = req.x - (after.x - before.x);
				const float leftY = req.y - (after.y - before.y);
				if (_abs(leftX) > 0.01f)
					m_pdaReticleOff.x = clampr(m_pdaReticleOff.x - leftX, -limX, limX);
				if (_abs(leftY) > 0.01f)
					m_pdaReticleOff.y = clampr(m_pdaReticleOff.y - leftY, -limY, limY);
			};
			m_pdaReticleOff.x = clampr(m_pdaReticleOff.x, -limX, limX);
			m_pdaReticleOff.y = clampr(m_pdaReticleOff.y, -limY, limY);
			if (lsMove)
			{
				// FIX-LIVE4: медленнее (680 -> 420 px/с при полном отклонении)
				float want[2] = { ls.x * 420.0f * dtSec, ls.y * 420.0f * dtSec }; // движение метки по карте
				float* off[2] = { &m_pdaReticleOff.x, &m_pdaReticleOff.y };
				for (int i = 0; i < 2; ++i)
				{
					// метка смещена, а стик тянет обратно — сначала возвращаем метку к центру
					if (*off[i] * want[i] < 0.0f)
					{
						const float n = *off[i] + want[i];
						if (n * *off[i] <= 0.0f)
						{
							*off[i] = 0.0f;
							want[i] = n;
						}
						else
						{
							*off[i] = n;
							want[i] = 0.0f;
						}
					}
				}
				if (want[0] != 0.0f || want[1] != 0.0f)
					moveMapOrReticle(Fvector2().set(-want[0], -want[1])); // п.8: инверсия
				++m_vcScrollCount;
				moved = true;
			}
			if (_abs(rs.y) > 0.08f && _abs(rs.y) >= _abs(rs.x))
			{
				// п.17: стик вверх (y < 0) — приблизить; ~x5 за секунду при полном отклонении
				mapWnd->GamepadZoomSmooth(expf(-rs.y * 1.6f * dtSec));
				moved = true;
			}
			if (!lsMove)
			{
				// п.14: метка прилипает к ближайшему маркеру в радиусе 0.75 размера метки
				const float rad = float(clampr(ps_gp_pda_reticle_size, 20, 64)) * 0.75f;
				Fvector2 sc;
				const Fvector2 ret = Fvector2().set(center.x + m_pdaReticleOff.x, center.y + m_pdaReticleOff.y);
				if (mapWnd->FindGamepadSpotNear(ret, rad, &sc))
				{
					Fvector2 d;
					d.set(ret.x - sc.x, ret.y - sc.y);
					if (d.x * d.x + d.y * d.y > 0.25f)
					{
						const float k = _min(1.0f, dtSec * 12.0f);
						moveMapOrReticle(Fvector2().set(d.x * k, d.y * k)); // у края метка сама доходит до маркера
						moved = true;
					}
				}
			}
			const Fvector2 reticle = Fvector2().set(center.x + m_pdaReticleOff.x, center.y + m_pdaReticleOff.y);
			const Fvector2 cur = GetUICursor().GetCursorPosition();
			if (_abs(cur.x - reticle.x) > 0.5f || _abs(cur.y - reticle.y) > 0.5f)
			{
				GetUICursor().SetUICursorPosition(reticle);
				tir->OnMouseAction(reticle.x, reticle.y, WINDOW_MOUSE_MOVE);
			}
			if ((nowContinual - m_vcLastTargetQueryTime) >= 30 || moved)
				RefreshCursorTargetFromLua(reticle, nowContinual);
			return;
		}
	}

	// 1. В MenuUI_Modal и интерактивном КПК крестовина (D-Pad Up/Down/Left/Right) прыгает по ячейкам/меткам с автоповтором 320/90 мс
	if (inModalVc && !m_bVcLocked && !pdaMapEye)
	{
		ENavDir navDir = ENavDir::None;
		bool navRepeat = false;
		LPCSTR navSrc = "none";
		// MENU-FOCUS-1: в меню (MainMenu) и в рамках фокуса левый стик прыгает так же, как крестовина
		if (IsNavDirTriggeredThisFrame(&navDir, &navRepeat, &navSrc) && navSrc &&
		    (strstr(navSrc, "dpad_") == navSrc || (lstickJumps && strstr(navSrc, "lstick_") == navSrc)))
		{
			if (TryDpadSnapInModalUI(navDir, nowContinual))
				return;
		}
	}

	const gpmath::StickConfig& uiCfg = gpmath::GetCalibrationProfile().ui_stick;

	gpmath::Vec2 moveStick = inModalVc
	                             ? gpmath::ProcessStick(uiCfg,
	                                                    gpmath::NormalizeStickAxis(gp.axes[0]),
	                                                    gpmath::NormalizeStickAxis(gp.axes[1]))
	                             : gpmath::ProcessStick(uiCfg,
	                                                    gpmath::NormalizeStickAxis(gp.axes[2]),
	                                                    gpmath::NormalizeStickAxis(gp.axes[3]));

	// PDA1-E (G6): на карте КПК крестовина плавно ведёт «око» так же, как левый стик (без прыжков по меткам)
	if (pdaMapEye)
	{
		const float dxDpad = ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_RIGHT)) ? 1.0f : 0.0f) -
		                     ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_LEFT)) ? 1.0f : 0.0f);
		const float dyDpad = ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_DOWN)) ? 1.0f : 0.0f) -
		                     ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_UP)) ? 1.0f : 0.0f);
		if (_abs(dxDpad) > 0.0f || _abs(dyDpad) > 0.0f)
		{
			moveStick.x += dxDpad;
			moveStick.y += dyDpad;
			const float m2 = moveStick.x * moveStick.x + moveStick.y * moveStick.y;
			if (m2 > 1.0f)
			{
				const float inv = 1.0f / _sqrt(m2);
				moveStick.x *= inv;
				moveStick.y *= inv;
			}
		}
	}

	const float moveMag = _sqrt(moveStick.x * moveStick.x + moveStick.y * moveStick.y);
	if (moveMag >= 0.05f)
	{
		m_vcLastStickMoveTime = nowContinual;
	}

	const Fvector2 curPos = GetUICursor().GetCursorPosition();
	{
		if (inModalVc)
		{
			if (moveMag >= 0.02f || (nowContinual - m_vcLastTargetQueryTime) >= 30)
			{
				RefreshCursorTargetFromLua(curPos, nowContinual);
			}
		}
		else
		{
			m_bVcTargetHit = false;
		}

		// MENU-FOCUS-1: в меню и режиме рамки свободного движения курсора стиком нет — только прыжки по целям
		if (!lstickJumps && !m_bVcLocked)
		{
			const u32 sinceStickMs = (m_vcLastStickMoveTime != 0 && nowContinual >= m_vcLastStickMoveTime)
			                             ? (nowContinual - m_vcLastStickMoveTime)
			                             : 999999u;
			bool bMoved = false;
			bool bSoftSnapped = false;
			Fvector2 newPos = StepCursorKinematics(curPos,
			                                       moveStick,
			                                       dtMs,
			                                       inModalVc && m_bVcTargetHit,
			                                       Fvector2().set(m_vcTargetCx, m_vcTargetCy),
			                                       sinceStickMs,
			                                       m_vcRemX,
			                                       m_vcRemY,
			                                       &bMoved,
			                                       &bSoftSnapped);

			if (inPdaVc && gui && gui->GetPdaMenu().IsShown())
			{
				if (psActorFlags.test(AF_3D_PDA))
				{
					const Frect& cb = gui->GetPdaMenu().m_cursor_box;
					clamp(newPos.x, cb.x1, cb.x2);
					clamp(newPos.y, cb.y1, cb.y2);
				}
				if (pdaMapEye)
					gui->GetPdaMenu().StepGamepadMapPan(moveStick.x, moveStick.y, false, newPos.x, newPos.y, dtMs);
			}

			if (bMoved && (GetUICursor().IsVisible() || !tir->NeedCursor()))
			{
				pInput->NotifyGamepadCursorMoved(nowContinual);
				GetUICursor().SetUICursorPosition(newPos);
				tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
				++m_vcMoveCount;
				if (bSoftSnapped)
					++m_vcSoftSnapCount;
				if (inModalVc)
					RefreshCursorTargetFromLua(newPos, nowContinual);
			}
		}
	}

	// 3. В MenuUI_Modal и интерактивном КПК правый стик (вертикаль) плавно крутит списки
	if (inModalVc)
	{
		const gpmath::Vec2 scrollStick = gpmath::ProcessStick(uiCfg,
		                                                      gpmath::NormalizeStickAxis(gp.axes[2]),
		                                                      gpmath::NormalizeStickAxis(gp.axes[3]));
		bool analogConsumed = false;
		if (g_ai_space && _abs(scrollStick.y) >= 0.08f)
		{
			::luabind::functor<bool> analogFn;
			if (ai().script_engine().functor("gp_bridge.cursor_rs_analog", analogFn))
			{
				try
				{
					analogConsumed = analogFn(scrollStick.y, static_cast<float>(_min<u32>(dtMs, 50u)));
				}
				catch (...)
				{
					analogConsumed = false;
				}
			}
		}
		if (analogConsumed)
		{
			m_vcScrollAccum = 0.0f;
			if (inPdaVc)
				++m_vcScrollCount;
		}
		else if (inPdaVc)
		{
			m_vcScrollAccum = 0.0f;
			if (_abs(scrollStick.y) >= 0.08f && gui && gui->GetPdaMenu().IsShown())
			{
				const float dyUi = scrollStick.y * 520.0f * (static_cast<float>(_min<u32>(dtMs, 50u)) * 0.001f);
				if (gui->GetPdaMenu().GpScrollAnalog(dyUi))
					++m_vcScrollCount;
			}
		}
		const int wheelDir = (analogConsumed || inPdaVc) ? 0
			: StepRightStickScrollAccumulator(scrollStick.y, dtMs, nowContinual, m_vcScrollAccum, m_vcLastScrollStepTime);
		if (wheelDir != 0 && gui)
		{
			const Fvector2 posNow = GetUICursor().GetCursorPosition();
			if (false)
			{
			}
			else
			{
				if (g_ai_space)
				{
					::luabind::functor<void> prepFn;
					if (ai().script_engine().functor("gp_bridge.cursor_prepare_scroll", prepFn))
					{
						try
						{
							prepFn(posNow.x, posNow.y);
						}
						catch (...)
						{
						}
					}
				}
				// INV-REWORK-1: правый стик прокручивает описание предмета, если оно показано и не влезает
				bool consumedByLua = false;
				if (g_ai_space)
				{
					::luabind::functor<bool> rsFn;
					if (ai().script_engine().functor("gp_bridge.cursor_rs_override", rsFn))
					{
						try
						{
							consumedByLua = rsFn(wheelDir);
						}
						catch (...)
						{
							consumedByLua = false;
						}
					}
				}
				if (!consumedByLua)
					gui->IR_UIOnMouseWheel(wheelDir);
				++m_vcScrollCount;
			}
		}
	}
}

u32 CGamepadContextDispatcher::StepRadialWheelButtons(u32 nowContinual)
{
	static const struct { u32 bit; LPCSTR name; } kCompFurnButtons[] = {
		{ CInput::GP_BTN_SOUTH, "A" }, { CInput::GP_BTN_EAST, "B" }, { CInput::GP_BTN_WEST, "X" },
		{ CInput::GP_BTN_NORTH, "Y" }, { CInput::GP_BTN_LEFT_SHOULDER, "LB" }, { CInput::GP_BTN_RIGHT_SHOULDER, "RB" },
		{ CInput::GP_BTN_BACK, "View" }, { VBTN_LT, "LT" }, { VBTN_RT, "RT" },
	};
	static u32 s_compWheelDpadBit = VBTN_TOTAL_COUNT;
	static u32 s_compWheelDpadNextMs = 0;

	CUIDialogWnd* const topModal = CurrentGameUI() ? CurrentGameUI()->TopInputReceiver() : nullptr;
	const bool topShown = topModal && topModal->IsShown();
	LPCSTR const topName = (topShown && topModal->WindowName().size() > 0) ? topModal->WindowName().c_str() : "";
	const bool isCompanionWheel = (m_currentContext == EGamepadContext::RadialWheel &&
	                               m_luaHints.radialWheelType == ERadialWheelType::Companion_Flat);
	const bool isCompFurnModal = (m_currentContext == EGamepadContext::MenuUI_Modal &&
	                              !CPauseTokenManager::Instance().IsFocusTestWindowOpen() &&
	                              !m_bVcLocked &&
	                              topName[0] != '\0' &&
	                              (0 == xr_strcmp(topName, "UICompanionInv") ||
	                               0 == xr_strcmp(topName, "addonCompanionsContextMenu") ||
	                               0 == xr_strcmp(topName, "UIFurnitureLight") ||
	                               0 == xr_strcmp(topName, "UIFurnitureRadio")));
	const bool compFurnActive = g_ai_space && (isCompanionWheel || isCompFurnModal) &&
	                            AllowsGamepadButtons() && pInput && pInput->IsGamepadActiveSource() &&
	                            !CGamepadOnScreenKeyboardUI::Instance().IsOpen() && !m_weaponSelectorOpen;
	if (!compFurnActive)
	{
		StepHoldButtonProtocol(nowContinual, 3, false, "gp_companions_furn.on_gamepad_button",
		                       kCompFurnButtons, sizeof(kCompFurnButtons) / sizeof(kCompFurnButtons[0]),
		                       false, &m_invFastButtonCount, &m_invLastFastButton, "CF");
		s_compWheelDpadBit = VBTN_TOTAL_COUNT;
	}

	if (m_weaponSelectorLookGate)
	{
		const auto* gp = pInput ? &pInput->GetGamepadState() : nullptr;
		const float x = gp ? gpmath::NormalizeStickAxis(gp->axes[2]) : 0.f;
		const float y = gp ? gpmath::NormalizeStickAxis(gp->axes[3]) : 0.f;
		if (x * x + y * y <= 0.15f * 0.15f)
			m_weaponSelectorLookGate = false;
	}
	if (m_weaponSelectorOpen)
	{
		LPCSTR action = nullptr;
		if (m_luaHints.radialWheelType != ERadialWheelType::QuickAction_Haru ||
		    (m_currentContext != EGamepadContext::RadialWheel && m_currentContext != EGamepadContext::World_Gameplay) ||
		    IsButtonPressed(CInput::GP_BTN_EAST))
			action = "gp_weapon_select.cancel";
		else if (IsButtonPressed(CInput::GP_BTN_SOUTH) ||
		         (m_rawDownMask & (1u << m_wheelHoldBtnBit)) == 0)
			action = "gp_weapon_select.confirm";
		if (action)
		{
			RunFieldAdapter(action, "tap", "weapon_select_close");
			m_weaponSelectorOpen = false;
			m_weaponSelectorLookGate = true;
			m_bWheelLbWasPhysicallyDown = false;
			ConsumeAllCurrentlyHeldButtons();
			PollLuaBridgeHints();
		}
		return (1u << VBTN_TOTAL_COUNT) - 1u;
	}
	if (compFurnActive)
	{
		m_bWheelLbWasPhysicallyDown = false;
		u32 cfHandled = StepHoldButtonProtocol(nowContinual,
		                                       3,
		                                       true,
		                                       "gp_companions_furn.on_gamepad_button",
		                                       kCompFurnButtons,
		                                       sizeof(kCompFurnButtons) / sizeof(kCompFurnButtons[0]),
		                                       false,
		                                       &m_invFastButtonCount,
		                                       &m_invLastFastButton,
		                                       "CF");
		if (isCompanionWheel)
		{
			static const struct { u32 bit; int dir; } kWheelDpad[] = {
				{ CInput::GP_BTN_DPAD_UP, 1 },
				{ CInput::GP_BTN_DPAD_DOWN, 2 },
				{ CInput::GP_BTN_DPAD_LEFT, 3 },
				{ CInput::GP_BTN_DPAD_RIGHT, 4 },
			};
			cfHandled |= (1u << CInput::GP_BTN_DPAD_UP) |
			             (1u << CInput::GP_BTN_DPAD_DOWN) |
			             (1u << CInput::GP_BTN_DPAD_LEFT) |
			             (1u << CInput::GP_BTN_DPAD_RIGHT);
			int fireDir = 0;
			u32 activeDpadBit = VBTN_TOTAL_COUNT;
			for (const auto& d : kWheelDpad)
			{
				if (IsButtonPressed(d.bit))
				{
					activeDpadBit = d.bit;
					s_compWheelDpadBit = d.bit;
					s_compWheelDpadNextMs = nowContinual + 320u;
					fireDir = d.dir;
					break;
				}
				if (IsButtonHeld(d.bit))
				{
					activeDpadBit = d.bit;
					if (s_compWheelDpadBit != d.bit)
					{
						s_compWheelDpadBit = d.bit;
						s_compWheelDpadNextMs = nowContinual + 320u;
						fireDir = d.dir;
					}
					else if (static_cast<s32>(nowContinual - s_compWheelDpadNextMs) >= 0)
					{
						s_compWheelDpadNextMs = nowContinual + 90u;
						fireDir = d.dir;
					}
					break;
				}
			}
			if (activeDpadBit == VBTN_TOTAL_COUNT)
				s_compWheelDpadBit = VBTN_TOTAL_COUNT;
			if (fireDir > 0)
			{
				::luabind::functor<LPCSTR> fnDpad;
				if (ai().script_engine().functor("gp_companions_furn.on_wheel_dpad", fnDpad))
				{
					try
					{
						LPCSTR snapRes = fnDpad(fireDir);
						float cx = 0.f, cy = 0.f;
						if (snapRes && 2 == sscanf(snapRes, "snap:1;cx=%f;cy=%f", &cx, &cy))
						{
							Fvector2 newPos;
							newPos.set(cx, cy);
							clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
							clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);
							if (pInput)
								pInput->NotifyGamepadCursorMoved(nowContinual);
							GetUICursor().SetUICursorPosition(newPos);
							if (topModal && topModal->IR_process())
								topModal->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
						}
					}
					catch (...)
					{
					}
				}
			}
			if (cfHandled & (1u << CInput::GP_BTN_EAST))
			{
				ConsumeAllCurrentlyHeldButtons();
				PollLuaBridgeHints();
			}
		}
		return cfHandled;
	}

	const bool isNativeWheel = (m_luaHints.radialWheelType == ERadialWheelType::QuickAction_Haru ||
	                            m_luaHints.radialWheelType == ERadialWheelType::Ammo);
	if (m_currentContext != EGamepadContext::RadialWheel || !isNativeWheel)
	{
		m_bWheelLbWasPhysicallyDown = false;
		return 0;
	}

	u32 wheelHoldBtn = m_wheelHoldBtnBit;
	if (wheelHoldBtn >= VBTN_TOTAL_COUNT)
	{
		if (m_luaHints.radialWheelType == ERadialWheelType::Ammo)
		{
			const gpbind::BindingTrigger* at = FindWorldBinding(gpbind::ActionId::AmmoWheel);
			wheelHoldBtn = (at && at->button != gpbind::ButtonId::None)
			                   ? static_cast<u32>(at->button)
			                   : static_cast<u32>(CInput::GP_BTN_DPAD_DOWN);
		}
		else
		{
			const gpbind::BindingTrigger* wt = FindWorldBinding(gpbind::ActionId::WeaponWheel);
			wheelHoldBtn = (wt && wt->button != gpbind::ButtonId::None)
			                   ? static_cast<u32>(wt->button)
			                   : static_cast<u32>(CInput::GP_BTN_LEFT_SHOULDER);
		}
	}

	u32 handled = (1u << VBTN_LT) |
	              (1u << VBTN_RT) |
	              (1u << CInput::GP_BTN_SOUTH) |
	              (1u << CInput::GP_BTN_EAST) |
	              (1u << CInput::GP_BTN_WEST) |
	              (1u << CInput::GP_BTN_NORTH) |
	              (1u << CInput::GP_BTN_RIGHT_SHOULDER) |
	              (1u << CInput::GP_BTN_LEFT_SHOULDER) |
	              (1u << wheelHoldBtn);

	const bool wheelBtnPhysDown = (m_rawDownMask & (1u << wheelHoldBtn)) != 0;
	if (wheelBtnPhysDown)
	{
		m_bWheelLbWasPhysicallyDown = true;
	}
	else if (m_bWheelLbWasPhysicallyDown)
	{
		m_bWheelLbWasPhysicallyDown = false;
		RunFieldAdapter("gp_wheel.on_gamepad_lb_released", "release", "wheel_lb_released");
		ConsumeAllCurrentlyHeldButtons();
		PollLuaBridgeHints();
		return handled;
	}

	// FIX-LIVE2 п.15 (Миша 2026-11-16, 15а/15.1): сочетания мира WheelMaskClean / WheelLaser (по умолчанию LB + R3 /
	// LB + D-pad вправо) работают на любом круге открытого колеса, если их модификатор — кнопка, держащая колесо.
	// После шортката отпускание LB закрывает колесо без сектора (gp_qaw_patch.release, shortcut_used).
	if (m_luaHints.radialWheelType == ERadialWheelType::QuickAction_Haru)
	{
		static const struct { gpbind::ActionId act; LPCSTR fn; LPCSTR why; } kWheelShortcuts[] = {
			{ gpbind::ActionId::WheelMaskClean, "gp_wheel.on_shortcut_mask", "wheel_shortcut_mask" },
			{ gpbind::ActionId::WheelLaser, "gp_wheel.on_shortcut_laser", "wheel_shortcut_laser" },
		};
		for (const auto& sc : kWheelShortcuts)
		{
			const gpbind::BindingTrigger* t = FindWorldBinding(sc.act);
			if (!t || t->type != gpbind::TriggerType::Chord || t->button == gpbind::ButtonId::None)
				continue;
			if (static_cast<u32>(t->modifier) != wheelHoldBtn)
				continue;
			const u32 b = static_cast<u32>(t->button);
			if (b >= VBTN_TOTAL_COUNT || b == wheelHoldBtn)
				continue;
			handled |= (1u << b);
			if (IsButtonPressed(b))
			{
				RunFieldAdapter(sc.fn, "chord", sc.why);
				ConsumeButtonUntilRelease(b);
			}
		}

		// FIX-LIVE3 п.18 (Миша 2026-11-16): D-pad в открытом колесе = закрепить наведённый сектор (предмет или действие)
		// за этим направлением крестовины (gp_qaw_patch.pin_from_wheel). Занятое направление -> «Заменить?» (A / B).
		static const struct { u32 btn; LPCSTR fn; LPCSTR why; } kWheelPins[] = {
			{ CInput::GP_BTN_DPAD_UP, "gp_wheel.on_pin_wheel_up", "wheel_pin_up" },
			{ CInput::GP_BTN_DPAD_DOWN, "gp_wheel.on_pin_wheel_down", "wheel_pin_down" },
			{ CInput::GP_BTN_DPAD_LEFT, "gp_wheel.on_pin_wheel_left", "wheel_pin_left" },
			{ CInput::GP_BTN_DPAD_RIGHT, "gp_wheel.on_pin_wheel_right", "wheel_pin_right" },
		};
		for (const auto& pn : kWheelPins)
		{
			if (pn.btn == wheelHoldBtn || (handled & (1u << pn.btn)))
				continue;
			handled |= (1u << pn.btn);
			if (IsButtonPressed(pn.btn))
			{
				RunFieldAdapter(pn.fn, "tap", pn.why);
				ConsumeButtonUntilRelease(pn.btn);
				if (pInput && pInput->GetGamepadHaptics())
					pInput->GetGamepadHaptics()->PulseSoftClick("wheel_pin");
			}
		}
	}

	if (IsButtonPressed(VBTN_LT))
	{
		RunFieldAdapter("gp_wheel.on_gamepad_tab_prev", "tap", "wheel_lt_tab_prev");
	}
	if (IsButtonPressed(VBTN_RT))
	{
		RunFieldAdapter("gp_wheel.on_gamepad_tab_next", "tap", "wheel_rt_tab_next");
	}
	if (IsButtonPressed(CInput::GP_BTN_WEST))
		RunFieldAdapter("gp_wheel.on_edit_remove_press", "press", "wheel_x");
	if (IsButtonReleased(CInput::GP_BTN_WEST))
		RunFieldAdapter("gp_wheel.on_edit_remove_release", "release", "wheel_x");
	if (wheelHoldBtn != CInput::GP_BTN_NORTH)
	{
		if (IsButtonPressed(CInput::GP_BTN_NORTH))
			RunFieldAdapter("gp_wheel.on_edit_move_press", "press", "wheel_y");
		if (IsButtonReleased(CInput::GP_BTN_NORTH))
			RunFieldAdapter("gp_wheel.on_edit_move_release", "release", "wheel_y");
	}
	if (IsButtonPressed(CInput::GP_BTN_SOUTH))
	{
		RunFieldAdapter("gp_wheel.on_gamepad_confirm", "tap", "wheel_a_confirm");
		PollLuaBridgeHints();
		return handled;
	}
	if (IsButtonPressed(CInput::GP_BTN_EAST))
	{
		RunFieldAdapter("gp_wheel.on_gamepad_cancel", "tap", "wheel_b_cancel");
		ConsumeAllCurrentlyHeldButtons();
		PollLuaBridgeHints();
		return handled;
	}

	return handled;
}

void CGamepadContextDispatcher::LogWheelState() const
{
	auto& ptm = CPauseTokenManager::Instance();
	string256 luaSummary = "unavailable";
	if (g_ai_space)
	{
		::luabind::functor<LPCSTR> fn;
		if (ai().script_engine().functor("gp_wheel.get_state_summary", fn))
		{
			try
			{
				LPCSTR s = fn();
				if (s && s[0])
					strncpy_s(luaSummary, sizeof(luaSummary), s, _TRUNCATE);
			}
			catch (...)
			{
				xr_strcpy(luaSummary, "lua_exception");
			}
		}
	}

	Msg("* [Gamepad] gp_wheel cursor: vc_active=%d vc_moves=%u", m_bVcActive ? 1 : 0, m_vcMoveCount);
	Msg("* [Gamepad] gp_wheel state: ctx=%s wheel_type=%s time_mode=%s stick_active=%d sector=%d mag=%.3f angle_deg=%.1f lb_phys=%d eff_tf=%.3f ext_tf=%.3f tf_reqs=%u pause_tokens=%u consumed_mask=0x%05X lua=[%s]",
	    GetContextName(m_currentContext),
	    GetRadialWheelTypeName(m_luaHints.radialWheelType),
	    GetWheelTimeMode(),
	    m_bWheelStickActive ? 1 : 0,
	    m_wheelSelectedSector,
	    m_wheelLastMagnitude,
	    rad2deg(m_wheelLastAngleRad),
	    m_bWheelLbWasPhysicallyDown ? 1 : 0,
	    ptm.GetEffectiveTimeFactor(),
	    ptm.GetExternalTimeFactor(),
	    ptm.TimeFactorRequestCount(),
	    ptm.Count(),
	    m_consumedUntilReleaseMask,
	    luaSummary);
}

bool CGamepadContextDispatcher::RunWheelSelfTest()
{
	const CGamepadContextDispatcher savedDisp = *this;
	auto& ptm = CPauseTokenManager::Instance();
	const float savedExtTf = ptm.GetExternalTimeFactor();
	ptm.ResetAllTimeFactors("wheel_selftest_begin");

	u32 passed = 0;
	u32 failed = 0;
	auto check = [&](bool cond, LPCSTR testName) {
		if (cond)
		{
			++passed;
		}
		else
		{
			++failed;
			Msg("! [Gamepad] gp_wheel selftest FAIL: %s", testName);
		}
	};

	// 1. D43 TimeFactor Dispatcher: slowest wins, releasing wheel restores Bullet Time, reset restores 1.0
	check(_abs(ptm.GetEffectiveTimeFactor() - 1.0f) < 1e-4f, "d43_initial_tf_is_1_0");
	ptm.OnExternalTimeFactorCommand(0.4f); // Bullet Time kCUSTOM25 active at 0.4x
	check(_abs(ptm.GetEffectiveTimeFactor() - 0.4f) < 1e-4f && _abs(Device.time_factor() - 0.4f) < 1e-4f,
	      "d43_external_bullet_time_0_4");

	ptm.RequestTimeFactor("gp_wheel", 0.2f); // Wheel opens in Slow-Mo 0.2x
	check(_abs(ptm.GetEffectiveTimeFactor() - 0.2f) < 1e-4f && _abs(Device.time_factor() - 0.2f) < 1e-4f,
	      "d43_wheel_slowmo_0_2_overrides_0_4");

	// Bullet Time updates while wheel is open (e.g. 0.35x) -> effective remains 0.2x
	ptm.OnExternalTimeFactorCommand(0.35f);
	check(_abs(ptm.GetEffectiveTimeFactor() - 0.2f) < 1e-4f, "d43_wheel_keeps_0_2_while_bt_ticks");

	// Wheel closes -> returns to Bullet Time 0.35x (NOT hardcoded 1.0x!)
	ptm.ReleaseTimeFactor("gp_wheel");
	check(_abs(ptm.GetEffectiveTimeFactor() - 0.35f) < 1e-4f && _abs(Device.time_factor() - 0.35f) < 1e-4f,
	      "d43_wheel_release_restores_bullet_time_0_35");

	// Load / Death / NetDestroy reset -> clears all requests and forces 1.0x
	ptm.RequestTimeFactor("gp_wheel", 0.2f);
	ptm.ResetAllTimeFactors("selftest_death_or_load");
	check(_abs(ptm.GetEffectiveTimeFactor() - 1.0f) < 1e-4f && _abs(Device.time_factor() - 1.0f) < 1e-4f && ptm.TimeFactorRequestCount() == 0,
	      "d43_reset_all_restores_1_0");

	// 2. GamepadMath::SelectRadialSector: central deadzone hysteresis (enter 0.35, exit 0.25) & angular hysteresis
	gpmath::RadialSectorConfig cfg{};
	cfg.enter_threshold = 0.35f;
	cfg.exit_threshold = 0.25f;
	cfg.angular_hysteresis_rad = 0.12f; // ~6.9 deg

	// 2a. Below enter threshold (r = 0.30 < 0.35) from neutral -> inactive, sector = -1
	auto r0 = gpmath::SelectRadialSector(cfg, 0.0f, 0.30f, 8, -1, false);
	check(!r0.stick_active && r0.selected_sector == -1, "sector_dz_below_enter_inactive");

	// 2b. Above enter threshold straight UP (rx=0, ry=0.80) -> active, sector 0 (12 o'clock)
	auto r1 = gpmath::SelectRadialSector(cfg, 0.0f, 0.80f, 8, r0.selected_sector, r0.stick_active);
	check(r1.stick_active && r1.selected_sector == 0, "sector_up_selects_0");

	// 2c. Between exit (0.25) and enter (0.35) when already active (r = 0.28) -> stays active on sector 0!
	auto r2 = gpmath::SelectRadialSector(cfg, 0.0f, 0.28f, 8, r1.selected_sector, r1.stick_active);
	check(r2.stick_active && r2.selected_sector == 0, "sector_dz_hysteresis_band_stays_active");

	// 2d. Return to center (r = 0.10 < 0.25) -> stick_active = false, but selected_sector 0 is preserved!
	auto r3 = gpmath::SelectRadialSector(cfg, 0.0f, 0.10f, 8, r2.selected_sector, r2.stick_active);
	check(!r3.stick_active && r3.selected_sector == 0, "sector_return_center_preserves_selection");

	// 2e. Angular hysteresis on boundary between sector 0 (0 deg) and sector 1 (45 deg): boundary is 22.5 deg.
	// With hysteresis ~6.87 deg, moving to 25 deg (inside hysteresis band 22.5 + 6.87 = 29.37 deg) must stay on sector 0!
	const float deg25 = deg2rad(25.0f);
	auto r4 = gpmath::SelectRadialSector(cfg, _sin(deg25) * 0.8f, _cos(deg25) * 0.8f, 8, 0, true);
	check(r4.stick_active && r4.selected_sector == 0, "sector_angular_hysteresis_holds_at_25deg");

	// Moving past hysteresis band to 32 deg (> 29.37 deg) switches to sector 1!
	const float deg32 = deg2rad(32.0f);
	auto r5 = gpmath::SelectRadialSector(cfg, _sin(deg32) * 0.8f, _cos(deg32) * 0.8f, 8, r4.selected_sector, true);
	check(r5.stick_active && r5.selected_sector == 1, "sector_angular_hysteresis_switches_at_32deg");

	// Moving back counter-clockwise to 20 deg (inside sector 1's hysteresis band: 45 - 29.37 = 15.63 deg) stays on sector 1!
	const float deg20 = deg2rad(20.0f);
	auto r6 = gpmath::SelectRadialSector(cfg, _sin(deg20) * 0.8f, _cos(deg20) * 0.8f, 8, r5.selected_sector, true);
	check(r6.stick_active && r6.selected_sector == 1, "sector_angular_hysteresis_holds_reverse_at_20deg");

	// 3. Anti-shot barrier on wheel close: held RT (VBTN_RT), A (GP_BTN_SOUTH), B (GP_BTN_EAST), LB (GP_BTN_LEFT_SHOULDER)
	// are put into consumed_until_release and do NOT fire in World_Gameplay until physically released
	const u32 heldMask = (1u << VBTN_RT) | (1u << CInput::GP_BTN_SOUTH) | (1u << CInput::GP_BTN_LEFT_SHOULDER);
	m_rawDownMask = heldMask;
	ConsumeAllCurrentlyHeldButtons();
	SContextSubFlags sub = {};
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "wheel_close_to_world", heldMask, 0, 0, 5000, false);
	check(IsButtonConsumedUntilRelease(VBTN_RT) && !IsButtonPressed(VBTN_RT) && !IsButtonHeld(VBTN_RT),
	      "barrier_rt_consumed_no_shot_on_close");
	check(IsButtonConsumedUntilRelease(CInput::GP_BTN_SOUTH) && !IsButtonPressed(CInput::GP_BTN_SOUTH),
	      "barrier_btnA_consumed_no_jump_on_close");
	check(IsButtonConsumedUntilRelease(CInput::GP_BTN_LEFT_SHOULDER) && !IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER),
	      "barrier_lb_consumed_on_close");

	// Physical release of RT clears the barrier and allows subsequent press in World_Gameplay
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "rt_released", 0, 0, 0, 5016, false);
	StepStateMachine(EGamepadContext::World_Gameplay, sub, "rt_pressed_again", (1u << VBTN_RT), 0, 0, 5032, false);
	check(!IsButtonConsumedUntilRelease(VBTN_RT) && IsButtonPressed(VBTN_RT),
	      "barrier_rt_fires_after_physical_release_and_repress");

	*this = savedDisp;
	ptm.OnExternalTimeFactorCommand(savedExtTf);

	Msg("* [Gamepad] gp_wheel selftest: %s (passed=%u failed=%u)",
	    (failed == 0) ? "PASS" : "FAIL", passed, failed);
	return failed == 0;
}

void CGamepadContextDispatcher::LogCursorState() const
{
	const Fvector2 pos = GetUICursor().GetCursorPosition();
	const auto visMode = GetUICursor().GetCurrentVisualMode();
	LPCSTR visModeStr = "standard_mouse";
	switch (visMode)
	{
	case CUICursor::ECursorVisualMode::GamepadIdle:    visModeStr = "cyberpunk_idle"; break;
	case CUICursor::ECursorVisualMode::GamepadHover:   visModeStr = "cyberpunk_hover_lock"; break;
	case CUICursor::ECursorVisualMode::GamepadPressed: visModeStr = "cyberpunk_pressed"; break;
	default: break;
	}

	Msg("* [Gamepad] gp_cursor: vc_active=%d modal_vc=%d mouse_mode=%d vis=%s pos=(%.1f, %.1f) size=%.2f speed=%.2f use_win_cursor=%d gp_source=%d recent_move=%d",
	    m_bVcActive ? 1 : 0,
	    m_bVcInModalMenu ? 1 : 0,
	    m_bGamepadMouseMode ? 1 : 0,
	    visModeStr,
	    pos.x,
	    pos.y,
	    ps_gp_cursor_size,
	    ps_gp_cursor_speed,
	    GetUICursor().IsUsingWinCursor() ? 1 : 0,
	    (pInput && pInput->IsGamepadActiveSource()) ? 1 : 0,
	    (pInput && pInput->IsGamepadCursorRecentlyMoved()) ? 1 : 0);

	Msg("* [Gamepad] gp_cursor target: hit=%d kind=%s center=(%.1f, %.1f) size=(%.1f x %.1f) friction_mult=%.2f",
	    m_bVcTargetHit ? 1 : 0,
	    GetCursorTargetKind(),
	    m_vcTargetCx,
	    m_vcTargetCy,
	    m_vcTargetW,
	    m_vcTargetH,
	    m_bVcTargetHit ? VIRTUAL_CURSOR_FRICTION_MULT : 1.0f);

	Msg("* [Gamepad] gp_cursor stats: moves=%u soft_snaps=%u scrolls=%u dpad_snaps=%u last_dpad_dir=%s ctx=%s modal_tag=\"%s\" inv_active=%d inv_lb=%d inv_btns=%u last_inv_btn=%s pda_active=%d pda_btns=%u last_pda_btn=%s",
	    m_vcMoveCount,
	    m_vcSoftSnapCount,
	    m_vcScrollCount,
	    m_vcDpadSnapCount,
	    GetNavDirName(m_vcLastDpadSnapDir),
	    GetContextName(m_currentContext),
	    m_luaHints.modalMenuTag.size() ? m_luaHints.modalMenuTag.c_str() : "",
	    IsInventoryModalActive() ? 1 : 0,
	    m_bInvLbModifierActive ? 1 : 0,
	    m_invFastButtonCount,
	    GetLastInventoryFastButton(),
	    IsPdaInteractiveActive() ? 1 : 0,
	    m_pdaFastButtonCount,
	    GetLastPdaFastButton());
}

bool CGamepadContextDispatcher::IsPdaInteractiveActive() const
{
	if (CPauseTokenManager::Instance().IsFocusTestWindowOpen())
		return false;
	if (m_currentContext == EGamepadContext::World_3DPDA && m_subFlags.b3dPdaInteractive)
		return true;
	const LPCSTR mtag = m_luaHints.modalMenuTag.size() ? m_luaHints.modalMenuTag.c_str() : "";
	const bool bPawModalTag = (0 == xr_strcmp(mtag, "UIWPSettings") ||
	                           0 == xr_strcmp(mtag, "UIPinSettingsWnd") ||
	                           0 == xr_strcmp(mtag, "UIPAWIndicator"));
	if (m_currentContext == EGamepadContext::MenuUI_Modal && CurrentGameUI() &&
	    (CurrentGameUI()->GetPdaMenu().IsShown() || bPawModalTag))
	{
		if (!IsInventoryModalActive())
			return true;
	}
	return false;
}

bool CGamepadContextDispatcher::IsPdaMapEyeActive() const
{
	if (!IsPdaInteractiveActive() || !pInput || !pInput->IsGamepadActiveSource())
		return false;
	if (m_bPdaMapFiltersMode || m_bInvFocusFrame || m_bPdaEyeOnButton)
		return false;
	if (!CurrentGameUI() || !CurrentGameUI()->GetPdaMenu().IsShown())
		return false;
	CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
	if (CurrentGameUI()->TopInputReceiver() != &pda)
		return false;
	LPCSTR actSec = pda.GetActiveSection();
	if (!actSec || 0 != xr_strcmp(actSec, "eptTasks"))
		return false;
	CUITaskWnd* taskWnd = pda.GetTaskWnd();
	if (!taskWnd || !taskWnd->IsShown() || !taskWnd->GetMapWnd() || !taskWnd->GetMapWnd()->IsShown())
		return false;
	return taskWnd->GetGamepadOverlayName()[0] == '\0';
}

void CGamepadContextDispatcher::SnapPdaCursor(const Fvector2& targetPos)
{
	if (!CurrentGameUI())
		return;
	CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
	Fvector2 p = targetPos;
	if (psActorFlags.test(AF_3D_PDA) && pda.IsShown() && CurrentGameUI()->TopInputReceiver() == &pda)
	{
		clamp(p.x, pda.m_cursor_box.x1, pda.m_cursor_box.x2);
		clamp(p.y, pda.m_cursor_box.y1, pda.m_cursor_box.y2);
	}
	else
	{
		clamp(p.x, 0.0f, UI_BASE_WIDTH);
		clamp(p.y, 0.0f, UI_BASE_HEIGHT);
	}
	if (pInput)
		pInput->NotifyGamepadCursorMoved(Device.dwTimeContinual);
	GetUICursor().SetUICursorPosition(p);
	if (CUIDialogWnd* tir = CurrentGameUI()->TopInputReceiver())
	{
		if (tir->IR_process() && (GetUICursor().IsVisible() || !tir->NeedCursor()))
			tir->OnMouseAction(p.x, p.y, WINDOW_MOUSE_MOVE);
	}
}

extern u32 g_gpInvFocusHoldStart;
extern u32 g_gpInvFocusHoldFillMs;
extern u32 g_gpInvHoldIconStart;
extern u32 g_gpInvHoldIconFillMs;
extern Frect g_gpInvHoldIconRect;

namespace
{
CUIListBoxItem* FindHoveredPropertiesBoxItem(CUIPropertiesBox* box, const Fvector2& curPos, Frect& outRect)
{
	if (!box || !box->IsShown())
		return nullptr;

	// MT-MAY: в майском движке нет подменю у пунктов CUIPropertiesBox — проверяем только сам список.
	const u32 count = box->GetItemsCount();
	for (u32 i = 0; i < count; ++i)
	{
		CUIListBoxItem* itm = box->GetItemByIDX(static_cast<int>(i));
		if (!itm || !itm->IsShown())
			continue;
		Frect r;
		itm->GetAbsoluteRect(r);
		if (r.in(curPos.x, curPos.y))
		{
			outRect = r;
			return itm;
		}
	}
	return nullptr;
}
} // namespace

u32 CGamepadContextDispatcher::StepPdaButtons(u32 nowContinual)
{
	static const struct { u32 bit; LPCSTR name; } kPdaButtons[] = {
		{ CInput::GP_BTN_SOUTH, "A" }, { CInput::GP_BTN_EAST, "B" }, { CInput::GP_BTN_WEST, "X" },
		{ CInput::GP_BTN_NORTH, "Y" }, { CInput::GP_BTN_LEFT_SHOULDER, "LB" }, { CInput::GP_BTN_RIGHT_SHOULDER, "RB" },
		{ CInput::GP_BTN_BACK, "View" }, { VBTN_LT, "LT" }, { VBTN_RT, "RT" },
		{ CInput::GP_BTN_LEFT_STICK, "LS" }, { CInput::GP_BTN_RIGHT_STICK, "RS" },
	};
	const LPCSTR mtag = m_luaHints.modalMenuTag.size() ? m_luaHints.modalMenuTag.c_str() : "";
	const bool bPawModalTag = (0 == xr_strcmp(mtag, "UIWPSettings") ||
	                           0 == xr_strcmp(mtag, "UIPinSettingsWnd") ||
	                           0 == xr_strcmp(mtag, "UIPAWIndicator"));
	const bool active = IsPdaInteractiveActive() && AllowsGamepadButtons() &&
	                    !CGamepadOnScreenKeyboardUI::Instance().IsOpen() &&
	                    CurrentGameUI() != nullptr &&
	                    (CurrentGameUI()->GetPdaMenu().IsShown() || bPawModalTag);

	static xr_string s_lastMenuHoverText;
	static Frect s_lastMenuHoverRect = { 0.f, 0.f, 0.f, 0.f };
	static bool s_hadMenuHover = false;
	if (active && g_ai_space)
	{
		xr_string hoverTxt;
		Frect hoverRect = { 0.f, 0.f, 0.f, 0.f };
		bool hasHoverNow = false;
		if (CUITaskWnd* taskWnd = CurrentGameUI()->GetPdaMenu().GetTaskWnd())
		{
			LPCSTR ov = taskWnd->GetGamepadOverlayName();
			CUIMapWnd* mapWnd = taskWnd->GetMapWnd();
			CUIPropertiesBox* propBox = mapWnd ? mapWnd->GetPropertiesBox() : nullptr;
			if (taskWnd->IsShown() && 0 == xr_strcmp(ov, "menu") && propBox && propBox->IsShown())
			{
				const Fvector2 cp = GetUICursor().GetCursorPosition();
				Frect itemRect;
				CUIListBoxItem* hovered = FindHoveredPropertiesBoxItem(propBox, cp, itemRect);
				if (hovered && hovered->GetText())
				{
					hoverTxt = hovered->GetText();
					hoverRect = itemRect;
					hasHoverNow = true;
				}
			}
		}
		if (hasHoverNow || s_hadMenuHover)
		{
			const bool changed = !hasHoverNow ||
			                     hoverTxt != s_lastMenuHoverText ||
			                     !fsimilar(hoverRect.x1, s_lastMenuHoverRect.x1, 0.5f) ||
			                     !fsimilar(hoverRect.y1, s_lastMenuHoverRect.y1, 0.5f) ||
			                     !fsimilar(hoverRect.x2, s_lastMenuHoverRect.x2, 0.5f) ||
			                     !fsimilar(hoverRect.y2, s_lastMenuHoverRect.y2, 0.5f);
			if (changed)
			{
				::luabind::functor<void> hoverFn;
				if (ai().script_engine().functor("gp_pda.on_map_menu_hover", hoverFn))
				{
					try
					{
						if (hasHoverNow)
							hoverFn(hoverTxt.c_str(), hoverRect.x1, hoverRect.y1, hoverRect.x2, hoverRect.y2);
						else
							hoverFn("", 0.f, 0.f, 0.f, 0.f);
					}
					catch (...)
					{
					}
				}
				s_lastMenuHoverText = hasHoverNow ? hoverTxt : "";
				s_lastMenuHoverRect = hoverRect;
				s_hadMenuHover = hasHoverNow;
			}
		}
	}
	else if (s_hadMenuHover && g_ai_space)
	{
		::luabind::functor<void> hoverFn;
		if (ai().script_engine().functor("gp_pda.on_map_menu_hover", hoverFn))
		{
			try
			{
				hoverFn("", 0.f, 0.f, 0.f, 0.f);
			}
			catch (...)
			{
			}
		}
		s_lastMenuHoverText.clear();
		s_lastMenuHoverRect.set(0.f, 0.f, 0.f, 0.f);
		s_hadMenuHover = false;
	}

	u32 handled = StepHoldButtonProtocol(nowContinual,
	                                     2,
	                                     active && g_ai_space,
	                                     "gp_bridge.pda_button",
	                                     kPdaButtons,
	                                     sizeof(kPdaButtons) / sizeof(kPdaButtons[0]),
	                                     false,
	                                     &m_pdaFastButtonCount,
	                                     &m_pdaLastFastButton,
	                                     "PDA");
	if (!active)
		return 0;

	// Белая заливка рамки фокуса при удержании X на кнопке модалки PAW или в меню карты (не View на нижней плашке y=728..756).
	if (m_bInvFocusFrame && g_gpInvHoldIconStart != 0 && g_gpInvHoldIconRect.y2 < 700.0f)
	{
		g_gpInvFocusHoldStart = g_gpInvHoldIconStart;
		g_gpInvFocusHoldFillMs = g_gpInvHoldIconFillMs;
	}

	CUIPdaWnd& pda = CurrentGameUI()->GetPdaMenu();
	const bool bPdaIsTopReceiver = (CurrentGameUI()->TopInputReceiver() == &pda);

	// Съедаем D-Pad из кнопочной инъекции, когда КПК наверху, событие обработано Lua или активна рамка фокуса.
	if (bPdaIsTopReceiver || (handled != 0) || m_bInvFocusFrame)
	{
		handled |= (1u << CInput::GP_BTN_DPAD_UP) |
		           (1u << CInput::GP_BTN_DPAD_DOWN) |
		           (1u << CInput::GP_BTN_DPAD_LEFT) |
		           (1u << CInput::GP_BTN_DPAD_RIGHT);
	}

	// Запасной путь для A (если Lua вернул ""): на карте над меткой сдвигаем курсор в точный центр метки перед MOUSE_1.
	if (IsButtonPressed(CInput::GP_BTN_SOUTH) && !(handled & (1u << CInput::GP_BTN_SOUTH)))
	{
		if (IsPdaMapEyeActive() && m_bPdaEyeSpotHit)
			SnapPdaCursor(Fvector2().set(m_pdaEyeSpotCx, m_pdaEyeSpotCy));
	}

	// Запасной путь для B (если Lua вернул ""): закрыть верхнее наложение КПК, иначе закрыть КПК с барьером.
	// DEF-P1-03: если поверх КПК открыт отдельный модальный диалог (UIWPSettings / UIPinSettingsWnd и др.),
	// не закрываем сам КПК под ним — отдаём B штатному обработчику MenuUI_Modal (DIK_ESCAPE в верхнее окно).
	if (bPdaIsTopReceiver && IsButtonPressed(CInput::GP_BTN_EAST) && !(handled & (1u << CInput::GP_BTN_EAST)))
	{
		handled |= (1u << CInput::GP_BTN_EAST);
		ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
		if (pda.CloseTopOverlay())
		{
			++m_pdaFastButtonCount;
			m_pdaLastFastButton = "B:close_pda_overlay";
		}
		else
		{
			pda.ClosePdaFromGamepad();
			ConsumeAllCurrentlyHeldButtons();
			++m_pdaFastButtonCount;
			m_pdaLastFastButton = "B:close_pda_barrier";
		}
	}

	return handled;
}

bool CGamepadContextDispatcher::IsInventoryModalActive() const
{
	if (m_currentContext != EGamepadContext::MenuUI_Modal)
		return false;
	if (CPauseTokenManager::Instance().IsFocusTestWindowOpen())
		return false;
	if (!m_luaHints.modalMenuTag.size())
		return false;
	return 0 == strncmp(m_luaHints.modalMenuTag.c_str(), "UIInventory", 11);
}

extern u32 g_gpInvFocusHoldStart;
extern u32 g_gpInvFocusHoldFillMs;
// INV-REWORK-16 (Misha 2026-11-12): A hold animation starts after a 300 ms pause, fires at 1000 ms
constexpr u32 InvAHoldPauseMs = 300;
constexpr u32 InvAHoldMs = 700; // INV-REWORK-20 (Миша 2026-11-12): все удержания короче, 0.3 с паузы без анимации остаются
extern u32 g_gpInvViewHoldStart; // INV-REWORK-15: corner brackets around stats/stack panel (UICursor.cpp)
extern u32 g_gpInvViewHoldDoneAt;
extern Frect g_gpInvViewHoldRect;
extern u32 g_gpInvViewHoldFillMs; // INV-REWORK-17: side bars fill after the A-like 300 ms pause
extern u32 g_gpInvHoldIconStart;  // INV-REWORK-17: ring around the footer badge (UICursor.cpp)
extern u32 g_gpInvHoldIconFillMs;
extern u32 g_gpInvHoldIconDoneAt;
extern int g_gpInvHoldIconKind;
extern int g_gpInvHoldIconRound; // INV-REWORK-27: ring on the round icon's outline (LB/RB pill: outside)
extern Frect g_gpInvHoldIconRect;
// INV-REWORK-17: ring around the footer badge; rect from gp_inv_rework.hold_icon_rect("a"|"view").
static void StartHoldIconRing(LPCSTR btn, int kind, u32 start, u32 fillMs)
{
	g_gpInvHoldIconStart = 0;
	g_gpInvHoldIconDoneAt = 0;
	::luabind::functor<LPCSTR> fnIcon;
	if (!ai().script_engine().functor("gp_inv_rework.hold_icon_rect", fnIcon))
		return;
	try
	{
		LPCSTR rs = fnIcon(btn);
		float x1, y1, x2, y2;
		if (rs && 4 == sscanf(rs, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) && x2 - x1 > 1.0f && y2 - y1 > 1.0f)
		{
			g_gpInvHoldIconRect.set(x1, y1, x2, y2);
			g_gpInvHoldIconKind = kind;
			g_gpInvHoldIconRound = (btn && (0 == xr_strcmp(btn, "lb") || 0 == xr_strcmp(btn, "rb"))) ? 0 : 1;
			g_gpInvHoldIconFillMs = fillMs;
			g_gpInvHoldIconStart = start ? start : 1;
		}
	}
	catch (...)
	{
	}
}

static void FinishHoldIconRing(u32 now, bool fired)
{
	const bool had = g_gpInvHoldIconStart != 0;
	g_gpInvHoldIconStart = 0;
	g_gpInvHoldIconDoneAt = (had && fired) ? (now ? now : 1) : 0;
}

// INV-REWORK-17 (п.4, Миша 2026-11-12): окно «Подробно» (ui_itm_details, курсор заперт m_bVcLocked):
// LB/RB — выделение по значкам предметов окна, Y — спрятать/показать описание, A — «Подробно» выделенного
// предмета, B — назад к прошлому предмету (true) или закрыть окно (false -> обычный kQUIT в
// ResolveContextualButtonInjection). Решает Lua gp_bridge.sheet_button("LB"|"RB"|"Y"|"A"|"B").
u32 CGamepadContextDispatcher::StepItemSheetButtons(u32 nowContinual)
{
	(void)nowContinual;
	if (!m_bVcLocked || !g_ai_space)
		return 0;
	static const struct { u32 bit; LPCSTR name; } kButtons[] = {
		{ CInput::GP_BTN_LEFT_SHOULDER, "LB" }, { CInput::GP_BTN_RIGHT_SHOULDER, "RB" },
		{ CInput::GP_BTN_NORTH, "Y" }, { CInput::GP_BTN_SOUTH, "A" }, { CInput::GP_BTN_EAST, "B" },
	};
	u32 handled = 0;
	for (const auto& b : kButtons)
	{
		if (!IsButtonPressed(b.bit))
			continue;
		bool used = false;
		::luabind::functor<bool> fnSheet;
		if (ai().script_engine().functor("gp_bridge.sheet_button", fnSheet))
		{
			try { used = fnSheet(b.name); } catch (...) { used = false; }
		}
		if (used)
		{
			handled |= (1u << b.bit);
			ConsumeButtonUntilRelease(b.bit);
			++m_invFastButtonCount;
			m_invLastFastButton = "ItemSheet";
		}
	}
	return handled;
}

// WS1 (Миша 2026-11-13, docs/ui/live-layout/workshop-gamepad-final.md §10 этап 1): верстак UIWorkshop и окошко
// переносного верстака UIFurnitureWorkshop. Раньше в MenuUI_Modal работали только A (клик) и B (Escape = закрыть
// всё), остальные кнопки — blocked_modal_ui. Теперь A, B, X, Y, LB, RB, View, LT, RT решает Lua
// gp_bridge.ws_button(name, phase):
//   phase "press"   -> ""   — кнопка не наша: обычный путь (A — клик в точке рамки, B — Escape);
//                      "used" — съедена до отпускания;
//                      "hold:<pauseMs>:<fillMs>[:x1,y1,x2,y2]" — ждать удержания; кольцо в значке кнопки по
//                      прямоугольнику (ед. UI), заполняется после паузы за fillMs;
//   phase "release" — кнопка отпущена до срабатывания удержания (короткое нажатие: «Закрыть» на B в окошке);
//   phase "hold"    — удержание дошло (кольцо полное): «Отремонтировать»/«Модифицировать»/«Создать», «Взять».
// Lua вызывается в игровом потоке; сами действия Lua откладывает до Update окна (CRASH-REPAIR).
bool CGamepadContextDispatcher::IsWorkshopModalActive() const
{
	if (m_currentContext != EGamepadContext::MenuUI_Modal &&
	    m_currentContext != EGamepadContext::Sleep &&
	    m_currentContext != EGamepadContext::TextEntry)
		return false;
	if (CPauseTokenManager::Instance().IsFocusTestWindowOpen() ||
	    CGamepadOnScreenKeyboardUI::Instance().IsOpen())
		return false;
	auto* ui = CurrentGameUI();
	auto* top = ui ? ui->TopInputReceiver() : nullptr;
	if (!top || !top->IsShown())
		return false;
	const auto& name = top->WindowName();
	return name.size() && (0 == xr_strcmp(name.c_str(), "UIWorkshop") ||
	                       0 == xr_strcmp(name.c_str(), "UIFurnitureWorkshop") ||
                           0 == xr_strcmp(name.c_str(), "UIMutantLoot") ||
                           0 == xr_strcmp(name.c_str(), "UICook") ||
                           0 == xr_strcmp(name.c_str(), "UIRepair") ||
                           0 == xr_strcmp(name.c_str(), "UICookStove") ||
                           0 == xr_strcmp(name.c_str(), "UISleep") ||
                           0 == xr_strcmp(name.c_str(), "GABlackjack") ||
                           0 == xr_strcmp(name.c_str(), "GABlackjackMessage") ||
                           0 == xr_strcmp(name.c_str(), "UINumpad") ||
                           0 == xr_strcmp(name.c_str(), "UICreateStash") ||
                           0 == xr_strcmp(name.c_str(), "UIRecipe") ||
                           0 == xr_strcmp(name.c_str(), "UINewPlayerExperience") ||
	                       0 == xr_strcmp(name.c_str(), "UITradingComputer") ||
	                       0 == xr_strcmp(name.c_str(), "UIFurniturePiano") ||
	                       0 == xr_strcmp(name.c_str(), "UIMapKit") ||
	                       0 == xr_strcmp(name.c_str(), "UIHarukaSkills") ||
	                       0 == xr_strcmp(name.c_str(), "UIExperienceMenu") ||
	                       0 == xr_strcmp(name.c_str(), "LaserSettingsDialog"));
}

static xr_string CallBridgeHoldButton(LPCSTR functorName, LPCSTR name, LPCSTR phase)
{
	::luabind::functor<LPCSTR> fn;
	if (!ai().script_engine().functor(functorName, fn))
		return "";
	try
	{
		LPCSTR res = fn(name, phase);
		return res ? res : "";
	}
	catch (...)
	{
	}
	return "";
}

u32 CGamepadContextDispatcher::StepHoldButtonProtocol(u32 nowContinual,
                                                      int ownerId,
                                                      bool active,
                                                      LPCSTR functorName,
                                                      const void* buttonsPtr,
                                                      size_t buttonCount,
                                                      bool allowMutantLootCancel,
                                                      u32* ioFastCount,
                                                      LPCSTR* ioLastFast,
                                                      LPCSTR logPrefix)
{
	struct SHoldBtnEntry { u32 bit; LPCSTR name; };
	const auto* buttons = static_cast<const SHoldBtnEntry*>(buttonsPtr);

	static int s_owner = 0;
	static u32 holdBit = VBTN_TOTAL_COUNT;
	static LPCSTR holdName = "";
	static CUIDialogWnd* holdDialog = nullptr;
	static shared_str holdSection;
	static u32 holdStart = 0;
	static u32 holdTotal = 0;
	static u32 holdPause = 0;
	static bool holdFired = false;

	if (!active)
	{
		if (s_owner == ownerId && holdBit != VBTN_TOTAL_COUNT)
		{
			if (!holdFired && holdName)
				CallBridgeHoldButton(functorName, holdName, "cancel");
			ConsumeButtonUntilRelease(holdBit);
			holdBit = VBTN_TOTAL_COUNT;
			s_owner = 0;
			g_gpInvHoldIconStart = 0;
			g_gpInvHoldIconDoneAt = 0;
		}
		return 0;
	}

	auto* top = NewGameDialog();
	if (!top && CurrentGameUI()) top = CurrentGameUI()->TopInputReceiver();
	LPCSTR rawSection = (ownerId == 2 && CurrentGameUI() && CurrentGameUI()->GetPdaMenu().IsShown())
	                        ? CurrentGameUI()->GetPdaMenu().GetActiveSection()
	                        : nullptr;
	const shared_str curSection(rawSection ? rawSection : "");

	if (holdBit != VBTN_TOTAL_COUNT && (s_owner != ownerId || holdDialog != top || holdSection != curSection))
	{
		if (s_owner == ownerId && !holdFired && holdName)
			CallBridgeHoldButton(functorName, holdName, "cancel");
		ConsumeButtonUntilRelease(holdBit);
		FinishHoldIconRing(nowContinual, false);
		holdBit = VBTN_TOTAL_COUNT;
		s_owner = 0;
	}

	u32 handled = 0;
	if (s_owner == ownerId && holdBit != VBTN_TOTAL_COUNT)
	{
		handled |= (1u << holdBit);
		if (IsButtonHeld(holdBit) || IsButtonPressed(holdBit))
		{
			if (!holdFired && nowContinual - holdStart >= holdTotal)
			{
				holdFired = true;
				FinishHoldIconRing(nowContinual, true);
				CallBridgeHoldButton(functorName, holdName, "hold");
				if (ioFastCount) ++(*ioFastCount);
				if (ioLastFast) *ioLastFast = (logPrefix && logPrefix[0] == 'P') ? "PDA:hold" : "WS:hold";
			}
		}
		else
		{
			if (!holdFired)
			{
				FinishHoldIconRing(nowContinual, false);
				const bool cancelLoot = allowMutantLootCancel && top != nullptr &&
				                        0 == xr_strcmp(top->WindowName().c_str(), "UIMutantLoot") &&
				                        nowContinual - holdStart >= holdPause;
				CallBridgeHoldButton(functorName, holdName, cancelLoot ? "cancel" : "release");
				if (ioFastCount) ++(*ioFastCount);
				if (ioLastFast) *ioLastFast = (logPrefix && logPrefix[0] == 'P') ? "PDA:release" : "WS:release";
			}
			holdBit = VBTN_TOTAL_COUNT;
			s_owner = 0;
		}
	}

	for (size_t i = 0; i < buttonCount; ++i)
	{
		const auto& b = buttons[i];
		if ((s_owner == ownerId && b.bit == holdBit) || !IsButtonPressed(b.bit))
			continue;
		const xr_string res = CallBridgeHoldButton(functorName, b.name, "press");
		if (res.empty())
			continue;
		handled |= (1u << b.bit);
		if (ioFastCount) ++(*ioFastCount);
		if (ioLastFast) *ioLastFast = (logPrefix && logPrefix[0] == 'P') ? "PDA:press" : "WS:press";
		u32 pauseMs = 0, fillMs = 0;
		if (0 == strncmp(res.c_str(), "hold:", 5) && 2 == sscanf(res.c_str() + 5, "%u:%u", &pauseMs, &fillMs))
		{
			if (holdBit != VBTN_TOTAL_COUNT)
			{
				if (!holdFired)
				{
					if (s_owner == ownerId && holdName)
						CallBridgeHoldButton(functorName, holdName, "cancel");
					FinishHoldIconRing(nowContinual, false);
				}
				if (holdBit != b.bit)
					ConsumeButtonUntilRelease(holdBit);
			}
			s_owner = ownerId;
			holdBit = b.bit;
			holdName = b.name;
			holdDialog = top;
			holdSection = curSection;
			holdStart = nowContinual;
			holdTotal = pauseMs + fillMs;
			holdPause = pauseMs;
			holdFired = false;
			g_gpInvHoldIconStart = 0;
			g_gpInvHoldIconDoneAt = 0;
			const char* p = strchr(res.c_str() + 5, ':');
			p = p ? strchr(p + 1, ':') : nullptr;
			float x1, y1, x2, y2;
			if (p && 4 == sscanf(p + 1, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) && x2 - x1 > 1.0f && y2 - y1 > 1.0f)
			{
				g_gpInvHoldIconRect.set(x1, y1, x2, y2);
				g_gpInvHoldIconKind = 0;
				g_gpInvHoldIconRound = 1;
				g_gpInvHoldIconFillMs = _max(1u, fillMs);
				g_gpInvHoldIconStart = nowContinual + pauseMs;
			}
		}
		else
		{
			ConsumeButtonUntilRelease(b.bit);
		}
	}
	return handled;
}

u32 CGamepadContextDispatcher::StepWorkshopButtons(u32 nowContinual)
{
	static const struct { u32 bit; LPCSTR name; } kWsButtons[] = {
		{ CInput::GP_BTN_SOUTH, "A" }, { CInput::GP_BTN_EAST, "B" }, { CInput::GP_BTN_WEST, "X" },
		{ CInput::GP_BTN_NORTH, "Y" }, { CInput::GP_BTN_LEFT_SHOULDER, "LB" }, { CInput::GP_BTN_RIGHT_SHOULDER, "RB" },
		{ CInput::GP_BTN_BACK, "View" }, { VBTN_LT, "LT" }, { VBTN_RT, "RT" },
	};
	const bool active = g_ai_space && (IsWorkshopModalActive() || NewGameDialog()) && AllowsGamepadButtons() &&
        !m_bVcLocked && pInput && pInput->IsGamepadActiveSource() &&
        !CGamepadOnScreenKeyboardUI::Instance().IsOpen();
	return StepHoldButtonProtocol(nowContinual,
	                              1,
	                              active,
	                              "gp_bridge.ws_button",
	                              kWsButtons,
	                              sizeof(kWsButtons) / sizeof(kWsButtons[0]),
	                              true,
	                              &m_invFastButtonCount,
	                              &m_invLastFastButton,
	                              "WS");
}

// INV-REWORK-20: флаг «характеристики при максимальном состоянии» в Lua (gp_bridge.inv_set_full_stats).
static void SetInvFullStats(bool on)
{
	if (!g_ai_space)
		return;
	::luabind::functor<LPCSTR> fn;
	if (ai().script_engine().functor("gp_bridge.inv_set_full_stats", fn))
	{
		try
		{
			fn(on);
		}
		catch (...)
		{
		}
	}
}

u32 CGamepadContextDispatcher::StepInventoryButtons(u32 nowContinual)
{
	static bool aHoldCaptured = false;
	static bool aHoldFired = false;
	static u32 aHoldStarted = 0;
	static bool viewHoldCaptured = false; // INV-REWORK-14 (п.5)
	static bool viewHoldFired = false;
	static u32 viewHoldStarted = 0;
	static bool xHoldCaptured = false; // INV-REWORK-19: удержание X = доп. действие предмета
	static bool xHoldFired = false;
	static u32 xHoldStarted = 0;
	// INV-REWORK-21 (Миша 2026-11-12): обыск/торговля — RB/LB забирает gp_inv_rework (rb_press / lb_press)
	static bool lbHoldCaptured = false; // INV-REWORK-22: удержание LB = автолутер (кольцо вокруг значка LB на кнопке)
	static bool lbHoldFired = false;
	static u32 lbHoldStarted = 0;
	static bool lbSideTaken = false; // LB = «Взять всё»/«Купить»: модификатор LB до отпускания не включается
	const bool bInInventory = IsInventoryModalActive() && AllowsGamepadButtons() && !m_bVcLocked; // INV-REWORK-16: not under the item sheet
	if (!bInInventory)
	{
		if (aHoldCaptured)
			ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
		if (viewHoldCaptured)
			ConsumeButtonUntilRelease(CInput::GP_BTN_BACK);
		if (xHoldCaptured)
			ConsumeButtonUntilRelease(CInput::GP_BTN_WEST);
		if (lbHoldCaptured || lbSideTaken)
			ConsumeButtonUntilRelease(CInput::GP_BTN_LEFT_SHOULDER);
		aHoldCaptured = aHoldFired = false;
		viewHoldCaptured = viewHoldFired = false;
		xHoldCaptured = xHoldFired = false;
		lbHoldCaptured = lbHoldFired = lbSideTaken = false;
		g_gpInvFocusHoldStart = 0;
		g_gpInvViewHoldStart = g_gpInvViewHoldDoneAt = 0;
		// The workshop owns the shared icon ring while its dialog is on top.
		if (!IsWorkshopModalActive() && !IsPdaInteractiveActive())
			g_gpInvHoldIconStart = g_gpInvHoldIconDoneAt = 0;
		if (m_bInvR3FullStats)
		{
			m_bInvR3FullStats = false;
			SetInvFullStats(false);
		}
		if (m_bInvLbModifierActive)
		{
			m_bInvLbModifierActive = false;
			if (g_ai_space)
			{
				::luabind::functor<LPCSTR> fnLb;
				if (ai().script_engine().functor("gp_bridge.inv_set_lb_modifier", fnLb))
				{
					try
					{
						fnLb(false);
					}
					catch (...)
					{
					}
				}
			}
		}
		return 0;
	}

	// В UIInventory перехватываем LT, RT, LB, Y, X, View напрямую через gp_bridge.script (FG.2, D59),
	// а A и B перехватываем только когда открыто контекстное меню item_props или пачка picker.
	u32 handled = (1u << VBTN_LT) |
	              (1u << VBTN_RT) |
	              (1u << CInput::GP_BTN_LEFT_SHOULDER) |
	              (1u << CInput::GP_BTN_RIGHT_SHOULDER) |
	              (1u << CInput::GP_BTN_NORTH) |
	              (1u << CInput::GP_BTN_WEST) |
	              (1u << CInput::GP_BTN_BACK) |
	              (1u << CInput::GP_BTN_RIGHT_STICK);

	if (!g_ai_space)
		return handled;

	// INV-REWORK-20 (Миша 2026-11-12): нажатие RS переключает «характеристики как у предмета в максимальном
	// состоянии» (родной show_full_stats, у мыши — зажатая клавиша). Сброс при смене предмета под рамкой —
	// в Lua (gp_inv_rework.full_stats_sync), при выходе из инвентаря — здесь (SetInvFullStats(false)).
	if (IsButtonPressed(CInput::GP_BTN_RIGHT_STICK))
	{
		m_bInvR3FullStats = true;
		::luabind::functor<LPCSTR> fnFs;
		if (ai().script_engine().functor("gp_inv_rework.full_stats_toggle", fnFs))
		{
			try
			{
				fnFs();
			}
			catch (...)
			{
			}
		}
		ConsumeButtonUntilRelease(CInput::GP_BTN_RIGHT_STICK);
	}

	// INV-REWORK-22: LB = панель NPC/ящика слева («Взять всё»/«Купить»); "hold" — ждём удержания (автолутер, кольцо
	// вокруг значка LB), "done" — уже выполнено, "" — обычная обработка LB (модификатор пачки).
	if (IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER))
	{
		::luabind::functor<LPCSTR> fnLbSide;
		if (ai().script_engine().functor("gp_inv_rework.lb_press", fnLbSide))
		{
			try
			{
				LPCSTR r = fnLbSide();
				if (r && 0 == xr_strcmp(r, "hold"))
				{
					lbSideTaken = true;
					lbHoldCaptured = true;
					lbHoldFired = false;
					lbHoldStarted = nowContinual;
					StartHoldIconRing("lb", 0, nowContinual + InvAHoldPauseMs, InvAHoldMs - InvAHoldPauseMs);
				}
				else if (r && *r)
					lbSideTaken = true;
				else
					lbSideTaken = false;
			}
			catch (...) { lbSideTaken = false; }
			if (lbSideTaken)
			{
				++m_invFastButtonCount;
				m_invLastFastButton = "LB:side";
			}
		}
	}
	if (lbHoldCaptured)
	{
		if (IsButtonHeld(CInput::GP_BTN_LEFT_SHOULDER))
		{
			if (!lbHoldFired && nowContinual - lbHoldStarted >= InvAHoldMs)
			{
				lbHoldFired = true;
				FinishHoldIconRing(nowContinual, true);
				::luabind::functor<bool> fnLbHold;
				if (ai().script_engine().functor("gp_inv_rework.lb_hold_fire", fnLbHold))
				{ try { fnLbHold(); } catch (...) {} }
			}
		}
		else
		{
			if (!lbHoldFired)
			{
				::luabind::functor<bool> fnLbTap;
				if (ai().script_engine().functor("gp_inv_rework.lb_hold_tap", fnLbTap))
				{ try { fnLbTap(); } catch (...) {} }
				FinishHoldIconRing(nowContinual, false);
			}
			lbHoldCaptured = lbHoldFired = false;
		}
	}
	if (lbSideTaken && !IsButtonHeld(CInput::GP_BTN_LEFT_SHOULDER) && !IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER))
		lbSideTaken = false;

	// 1. LB (GP_BTN_LEFT_SHOULDER): удержание включает модификатор пачки (holding_ctrl) и 100%-статов (show_full_stats)
	const bool lbHeld = IsButtonHeld(CInput::GP_BTN_LEFT_SHOULDER) && !lbSideTaken;
	if (lbHeld != m_bInvLbModifierActive)
	{
		m_bInvLbModifierActive = lbHeld;
		::luabind::functor<LPCSTR> fnLb;
		if (ai().script_engine().functor("gp_bridge.inv_set_lb_modifier", fnLb))
		{
			try
			{
				fnLb(lbHeld);
			}
			catch (...)
			{
			}
		}
		if (lbHeld)
		{
			++m_invFastButtonCount;
			m_invLastFastButton = "LB:modifier_on";
		}
	}

	// RB follows the same inventory action routing as other fast buttons.
	// INV-REWORK-22: в обыске/торговле сначала gp_inv_rework.rb_press: "done" — уже выполнено
	// (сложить всё / продать / закрыть окно), "" — обычная обработка RB.
	if (IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER))
	{
		bool rbSide = false;
		::luabind::functor<LPCSTR> fnRbSide;
		if (ai().script_engine().functor("gp_inv_rework.rb_press", fnRbSide))
		{
			try
			{
				LPCSTR r = fnRbSide();
				if (r && *r)
					rbSide = true;
			}
			catch (...) {}
		}
		if (rbSide)
		{
			++m_invFastButtonCount;
			m_invLastFastButton = "RB:side";
		}
		else
		{
			::luabind::functor<LPCSTR> fnRb;
			if (ai().script_engine().functor("gp_bridge.inv_on_button_rb", fnRb))
			{
				try { fnRb(); ++m_invFastButtonCount; m_invLastFastButton = "RB:inventory"; }
				catch (...) {}
			}
		}
	}

	// 2. LT / RT (VBTN_LT / VBTN_RT): 2-шаговое переключение вкладок сортировки sort_btn[i]
	if (IsButtonPressed(VBTN_LT))
	{
		::luabind::functor<LPCSTR> fnSort;
		if (ai().script_engine().functor("gp_bridge.inv_cycle_sort_tab", fnSort))
		{
			try
			{
				fnSort(-1);
				++m_invFastButtonCount;
				m_invLastFastButton = "LT:sort_prev";
			}
			catch (...)
			{
			}
		}
	}
	if (IsButtonPressed(VBTN_RT))
	{
		::luabind::functor<LPCSTR> fnSort;
		if (ai().script_engine().functor("gp_bridge.inv_cycle_sort_tab", fnSort))
		{
			try
			{
				fnSort(1);
				++m_invFastButtonCount;
				m_invLastFastButton = "RT:sort_next";
			}
			catch (...)
			{
			}
		}
	}

	// 3. Y (GP_BTN_NORTH): прямой вызов On_CC_Mouse1_DB(bag, idx) без двойного клика
	if (IsButtonPressed(CInput::GP_BTN_NORTH))
	{
		::luabind::functor<LPCSTR> fnY;
		if (ai().script_engine().functor("gp_bridge.inv_on_button_y", fnY))
		{
			try
			{
				fnY();
				++m_invFastButtonCount;
				m_invLastFastButton = "Y:mouse1_db";
			}
			catch (...)
			{
			}
		}
	}

	// 4. X (GP_BTN_WEST): открытие item_props (On_CC_Mouse2 -> InitProperties) либо LB+X (LMode_TakeAll / TMode_Sell+TMode_Buy).
	//    INV-REWORK-19 (Миша 2026-11-12): если gp_inv_rework.x_hold_press() == true, короткое X (меню) срабатывает на
	//    ОТПУСКАНИИ, а удержание 1,0 с (пауза 300 мс, заполнение рамки ячейки и кольцо у значка X, как у A)
	//    вызывает gp_inv_rework.x_hold_fire() — дополнительное действие предмета (или родное меню из доп. строк).
	auto applyXRes = [&](LPCSTR res)
	{
		if (!res)
			return;
		float cx = 0.0f, cy = 0.0f;
		const char* cxPtr = strstr(res, ";cx=");
		if (cxPtr && sscanf(cxPtr, ";cx=%f;cy=%f", &cx, &cy) == 2)
		{
			Fvector2 newPos;
			newPos.set(cx, cy);
			clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
			clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);
			if (pInput)
				pInput->NotifyGamepadCursorMoved(nowContinual);
			GetUICursor().SetUICursorPosition(newPos);
			if (CUIGameCustom* gui = CurrentGameUI())
			{
				if (CUIDialogWnd* tir = gui->TopInputReceiver())
				{
					if (tir->IR_process())
						tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
				}
			}
		}
	};
	auto runX = [&]()
	{
		::luabind::functor<LPCSTR> fnX;
		if (ai().script_engine().functor("gp_bridge.inv_on_button_x", fnX))
		{
			try
			{
				LPCSTR res = fnX();
				++m_invFastButtonCount;
				m_invLastFastButton = "X:item_props";
				if (res && (0 == strncmp(res, "handled_item_props", 18) || 0 == strncmp(res, "handled_modal", 13)))
				{
					handled |= (1u << CInput::GP_BTN_WEST);
					ConsumeButtonUntilRelease(CInput::GP_BTN_WEST);
				}
				applyXRes(res);
			}
			catch (...)
			{
			}
		}
	};
	if (IsButtonPressed(CInput::GP_BTN_WEST))
	{
		xHoldCaptured = false;
		::luabind::functor<bool> fnXPress;
		if (ai().script_engine().functor("gp_inv_rework.x_hold_press", fnXPress))
		{
			try { xHoldCaptured = fnXPress(); } catch (...) { xHoldCaptured = false; }
		}
		if (xHoldCaptured)
		{
			xHoldFired = false;
			xHoldStarted = nowContinual;
			g_gpInvFocusHoldFillMs = InvAHoldMs - InvAHoldPauseMs;
			g_gpInvFocusHoldStart = nowContinual + InvAHoldPauseMs;
			StartHoldIconRing("x", 0, nowContinual + InvAHoldPauseMs, InvAHoldMs - InvAHoldPauseMs);
		}
		else
			runX();
	}
	if (xHoldCaptured)
	{
		handled |= (1u << CInput::GP_BTN_WEST);
		if (IsButtonHeld(CInput::GP_BTN_WEST))
		{
			if (!xHoldFired && nowContinual - xHoldStarted >= InvAHoldMs)
			{
				xHoldFired = true;
				g_gpInvFocusHoldStart = 0;
				FinishHoldIconRing(nowContinual, true);
				::luabind::functor<LPCSTR> fnXHold;
				if (ai().script_engine().functor("gp_inv_rework.x_hold_fire", fnXHold))
				{
					try
					{
						LPCSTR res = fnXHold();
						++m_invFastButtonCount;
						m_invLastFastButton = "X:hold_extra_action";
						applyXRes(res);
					}
					catch (...)
					{
					}
				}
			}
		}
		else
		{
			if (!xHoldFired)
			{
				::luabind::functor<bool> fnXTap;
				if (ai().script_engine().functor("gp_inv_rework.x_hold_tap", fnXTap))
				{ try { fnXTap(); } catch (...) {} }
				FinishHoldIconRing(nowContinual, false);
				runX();
			}
			xHoldCaptured = xHoldFired = false;
			g_gpInvFocusHoldStart = 0;
		}
	}

	// 5. View (GP_BTN_BACK, D24): смена основной панели текущего режима.
	//    INV-REWORK-14 (п.5): если gp_inv_rework.view_hold_press() == true — короткое нажатие (смена
	//    раздела снаряжение <-> рюкзак) срабатывает на ОТПУСКАНИИ, удержание 800 мс (то же зелёное
	//    заполнение рамки 31 кадр, что у A, g_gpInvFocusHoldStart) = gp_inv_rework.view_hold_fire()
	//    («вход/выход»: меню стопки или характеристики и обратно).
	auto applyPanelRes = [&](LPCSTR res)
	{
		if (!res || strstr(res, "panel:1;") != res)
			return;
		float cx = 0.0f, cy = 0.0f;
		const char* cxPtr = strstr(res, ";cx=");
		if (!cxPtr || sscanf(cxPtr, ";cx=%f;cy=%f", &cx, &cy) != 2)
			return;
		Fvector2 newPos;
		newPos.set(cx, cy);
		clamp(newPos.x, 0.0f, UI_BASE_WIDTH);
		clamp(newPos.y, 0.0f, UI_BASE_HEIGHT);
		if (pInput)
			pInput->NotifyGamepadCursorMoved(nowContinual);
		GetUICursor().SetUICursorPosition(newPos);
		m_vcRemX = m_vcRemY = 0.0f;
		if (CUIGameCustom* gui = CurrentGameUI())
		{
			if (CUIDialogWnd* tir = gui->TopInputReceiver())
			{
				if (tir->IR_process())
					tir->OnMouseAction(newPos.x, newPos.y, WINDOW_MOUSE_MOVE);
			}
		}
		RefreshCursorTargetFromLua(newPos, nowContinual);
	};
	auto runView = [&]()
	{
		::luabind::functor<LPCSTR> fnView;
		if (ai().script_engine().functor("gp_bridge.inv_switch_panel_view", fnView))
		{
			try
			{
				LPCSTR res = fnView();
				++m_invFastButtonCount;
				m_invLastFastButton = "View:switch_panel";
				applyPanelRes(res);
			}
			catch (...)
			{
			}
		}
	};
	// FIX-LIVE2 п.32 (Миша 2026-11-16): LB + View = прежнее удержание View (характеристики / меню стопки
	// и обратно, gp_inv_rework.view_lb_chord); само удержание View в своём рюкзаке теперь открывает КПК.
	if (IsButtonPressed(CInput::GP_BTN_BACK) && lbHeld)
	{
		viewHoldCaptured = viewHoldFired = false;
		handled |= (1u << CInput::GP_BTN_BACK);
		ConsumeButtonUntilRelease(CInput::GP_BTN_BACK);
		::luabind::functor<LPCSTR> fnChord;
		if (ai().script_engine().functor("gp_inv_rework.view_lb_chord", fnChord))
		{
			try
			{
				LPCSTR res = fnChord();
				++m_invFastButtonCount;
				m_invLastFastButton = "LB+View:stats";
				applyPanelRes(res);
			}
			catch (...)
			{
			}
		}
	}
	else if (IsButtonPressed(CInput::GP_BTN_BACK))
	{
		viewHoldCaptured = false;
		::luabind::functor<bool> fnPress;
		if (ai().script_engine().functor("gp_inv_rework.view_hold_press", fnPress))
		{
			try { viewHoldCaptured = fnPress(); } catch (...) { viewHoldCaptured = false; }
		}
		if (viewHoldCaptured)
		{
			viewHoldFired = false;
			viewHoldStarted = nowContinual;
			// INV-REWORK-15: brackets around the panel (gp_inv_rework.view_hold_rect); no panel -> cell fill
			bool haveRect = false;
			::luabind::functor<LPCSTR> fnRect;
			if (ai().script_engine().functor("gp_inv_rework.view_hold_rect", fnRect))
			{
				try
				{
					LPCSTR rs = fnRect();
					float x1, y1, x2, y2;
					if (rs && 4 == sscanf(rs, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) && x2 - x1 > 4.0f && y2 - y1 > 4.0f)
					{
						g_gpInvViewHoldRect.set(x1, y1, x2, y2);
						haveRect = true;
					}
				}
				catch (...)
				{
				}
			}
			g_gpInvViewHoldDoneAt = 0;
			// INV-REWORK-17 / FIX-LIVE3 p.20: View hold for the PDA is 0.3 s shorter than A: 150 ms pause, fires at 400 ms
			g_gpInvViewHoldFillMs = GP_VIEW_PDA_HOLD_MS - GP_VIEW_PDA_HOLD_PAUSE_MS;
			if (haveRect)
				g_gpInvViewHoldStart = nowContinual + GP_VIEW_PDA_HOLD_PAUSE_MS;
			else
			{
				g_gpInvFocusHoldFillMs = GP_VIEW_PDA_HOLD_MS - GP_VIEW_PDA_HOLD_PAUSE_MS;
				g_gpInvFocusHoldStart = nowContinual + GP_VIEW_PDA_HOLD_PAUSE_MS;
			}
			StartHoldIconRing("view", 1, nowContinual + GP_VIEW_PDA_HOLD_PAUSE_MS, GP_VIEW_PDA_HOLD_MS - GP_VIEW_PDA_HOLD_PAUSE_MS);
		}
		else
			runView();
	}
	if (viewHoldCaptured)
	{
		handled |= (1u << CInput::GP_BTN_BACK);
		if (IsButtonHeld(CInput::GP_BTN_BACK))
		{
			if (!viewHoldFired && nowContinual - viewHoldStarted >= GP_VIEW_PDA_HOLD_MS)
			{
				viewHoldFired = true;
				g_gpInvFocusHoldStart = 0;
				FinishHoldIconRing(nowContinual, true);
				if (g_gpInvViewHoldStart)
				{
					g_gpInvViewHoldStart = 0;
					g_gpInvViewHoldDoneAt = nowContinual ? nowContinual : 1;
				}
				// INV-REWORK-15 (Misha 2026-11-12): soft click when the hold fires, not the deny pulse
				if (pInput && pInput->GetGamepadHaptics())
					pInput->GetGamepadHaptics()->PulseSoftClick("inv_view_hold");
				::luabind::functor<LPCSTR> fnHold;
				if (ai().script_engine().functor("gp_inv_rework.view_hold_fire", fnHold))
				{
					try
					{
						LPCSTR res = fnHold();
						++m_invFastButtonCount;
						m_invLastFastButton = "View:hold_enter_exit";
						applyPanelRes(res);
					}
					catch (...)
					{
					}
				}
			}
		}
		else
		{
			if (!viewHoldFired)
				runView();
			if (!viewHoldFired)
				FinishHoldIconRing(nowContinual, false);
			viewHoldCaptured = viewHoldFired = false;
			g_gpInvFocusHoldStart = 0;
			g_gpInvViewHoldStart = 0;
		}
	}

	// 6. A (GP_BTN_SOUTH): если открыто item_props — активирует выбранную строку и поглощает A до отпускания;
	//    иначе пропускает A дальше в MOUSE_1 (выбор ячейки / перетаскивание > 150 мс)
	if (IsButtonPressed(CInput::GP_BTN_SOUTH))
	{
		::luabind::functor<LPCSTR> fnA;
		if (ai().script_engine().functor("gp_bridge.inv_on_button_a_in_props", fnA))
		{
			try
			{
				LPCSTR res = fnA();
				if (res && strstr(res, "handled_item_props") == res)
				{
					handled |= (1u << CInput::GP_BTN_SOUTH);
					ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
					++m_invFastButtonCount;
					m_invLastFastButton = "A:item_props_exec";
				}
			}
			catch (...)
			{
			}
		}
	}

	// Same 800 ms / 31-frame hold used by the settings and MCM buttons.
	if (IsButtonPressed(CInput::GP_BTN_SOUTH) && !(handled & (1u << CInput::GP_BTN_SOUTH)))
	{
		::luabind::functor<bool> fnPress;
		if (ai().script_engine().functor("gp_inv_rework.a_hold_press", fnPress))
		{
			try { aHoldCaptured = fnPress(); } catch (...) { aHoldCaptured = false; }
			if (aHoldCaptured)
			{
				aHoldFired = false;
				aHoldStarted = nowContinual;
				g_gpInvFocusHoldFillMs = InvAHoldMs - InvAHoldPauseMs;
				g_gpInvFocusHoldStart = nowContinual + InvAHoldPauseMs;
				StartHoldIconRing("a", 0, nowContinual + InvAHoldPauseMs, InvAHoldMs - InvAHoldPauseMs);
			}
		}
	}
	if (aHoldCaptured)
	{
		handled |= (1u << CInput::GP_BTN_SOUTH);
		if (IsButtonHeld(CInput::GP_BTN_SOUTH))
		{
			if (!aHoldFired && nowContinual - aHoldStarted >= InvAHoldMs)
			{
				aHoldFired = true;
				g_gpInvFocusHoldStart = 0;
				FinishHoldIconRing(nowContinual, true);
				::luabind::functor<bool> fnHold;
				if (ai().script_engine().functor("gp_inv_rework.a_hold_fire", fnHold))
				{ try { fnHold(); } catch (...) {} }
			}
		}
		else
		{
			if (!aHoldFired)
			{
				::luabind::functor<bool> fnTap;
				if (ai().script_engine().functor("gp_inv_rework.a_hold_tap", fnTap))
				{ try { fnTap(); } catch (...) {} }
			}
			if (!aHoldFired)
				FinishHoldIconRing(nowContinual, false);
			aHoldCaptured = aHoldFired = false;
			g_gpInvFocusHoldStart = 0;
		}
	}

	// 7. B (GP_BTN_EAST): если открыто item_props или picker — закрывает только его (без закрытия UIInventory)
	//    и взводит барьер consumed_until_release на B
	if (IsButtonPressed(CInput::GP_BTN_EAST))
	{
		::luabind::functor<LPCSTR> fnB;
		if (ai().script_engine().functor("gp_bridge.inv_on_button_b", fnB))
		{
			try
			{
				LPCSTR res = fnB();
				if (res && (0 == xr_strcmp(res, "closed_item_props") || 0 == xr_strcmp(res, "closed_picker") || 0 == xr_strcmp(res, "pick:cancel") || 0 == xr_strcmp(res, "panel:return")))
				{
					handled |= (1u << CInput::GP_BTN_EAST);
					ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
					++m_invFastButtonCount;
					m_invLastFastButton = "B:closed_overlay";
				}
			}
			catch (...)
			{
			}
		}
	}

	// 8. L3 (GP_BTN_LEFT_STICK, FG.6d/FG.6h): отмена последнего действия очереди на паузе в инвентаре
	if (IsButtonPressed(CInput::GP_BTN_LEFT_STICK))
	{
		::luabind::functor<bool> fnL3;
		if (ai().script_engine().functor("gp_lifecycle.on_l3_cancel_queue", fnL3))
		{
			try
			{
				if (fnL3())
				{
					handled |= (1u << CInput::GP_BTN_LEFT_STICK);
					ConsumeButtonUntilRelease(CInput::GP_BTN_LEFT_STICK);
					++m_invFastButtonCount;
					m_invLastFastButton = "L3:cancel_queue";
				}
			}
			catch (...)
			{
			}
		}
		if (!(handled & (1u << CInput::GP_BTN_LEFT_STICK)))
		{
			::luabind::functor<bool> fnL3Rework;
			if (ai().script_engine().functor("gp_inv_rework.l3_press", fnL3Rework))
			{
				try
				{
					if (fnL3Rework())
					{
						handled |= (1u << CInput::GP_BTN_LEFT_STICK);
						ConsumeButtonUntilRelease(CInput::GP_BTN_LEFT_STICK);
						++m_invFastButtonCount;
						m_invLastFastButton = "L3:inv_rework";
					}
				}
				catch (...)
				{
				}
			}
		}
	}

	return handled;
}

bool CGamepadContextDispatcher::RunCursorSelfTest()
{
	const CGamepadContextDispatcher savedDisp = *this;
	const Fvector2 savedCursorPos = GetUICursor().GetCursorPosition();
	const float savedCursorSize = ps_gp_cursor_size;
	const float savedCursorSpeed = ps_gp_cursor_speed;
	ps_gp_cursor_size = 1.5f;
	ps_gp_cursor_speed = 1.0f;
	m_bInSelfTest = true;

	u32 passed = 0;
	u32 failed = 0;
	auto check = [&](bool cond, LPCSTR testName)
	{
		if (cond)
		{
			++passed;
		}
		else
		{
			++failed;
			Msg("! [Gamepad] gp_cursor selftest FAIL: %s", testName);
		}
	};

	// 1. FG.1 (1): Fullscreen & windowed cursor position via SetUICursorPosition + anti-chatter mask for D1.2
	GetUICursor().SetUICursorPosition(Fvector2().set(200.0f, 300.0f));
	const Fvector2 p0 = GetUICursor().GetCursorPosition();
	check(_abs(p0.x - 200.0f) < 0.5f && _abs(p0.y - 300.0f) < 0.5f, "fg1_set_ui_cursor_pos_exact");

	GetUICursor().SetUICursorPosition(Fvector2().set(1500.0f, -50.0f));
	const Fvector2 pClamped = GetUICursor().GetCursorPosition();
	check(_abs(pClamped.x - UI_BASE_WIDTH) < 0.5f && _abs(pClamped.y - 0.0f) < 0.5f, "fg1_set_ui_cursor_pos_clamped_1024x768");

	if (pInput)
	{
		pInput->NotifyGamepadCursorMoved(10000);
		check(pInput->IsGamepadCursorRecentlyMoved(10025, 60), "fg1_d1_2_mask_active_within_60ms");
		check(!pInput->IsGamepadCursorRecentlyMoved(10085, 60), "fg1_d1_2_mask_expires_after_60ms_allowing_mouse");
	}
	else
	{
		check(true, "fg1_d1_2_mask_active_within_60ms");
		check(true, "fg1_d1_2_mask_expires_after_60ms_allowing_mouse");
	}

	// 2. FG.1 (2): Cyberpunk 2077 stylized cursor visual modes & instant KBM fallback
	check(CUICursor::EvaluateVisualMode(false, false, false) == CUICursor::ECursorVisualMode::StandardMouse &&
	      CUICursor::EvaluateVisualMode(false, true, true) == CUICursor::ECursorVisualMode::StandardMouse,
	      "fg1_visual_kbm_instant_standard_cursor");
	check(CUICursor::EvaluateVisualMode(true, false, false) == CUICursor::ECursorVisualMode::GamepadIdle,
	      "fg1_visual_gamepad_idle_reticle");
	check(CUICursor::EvaluateVisualMode(true, true, false) == CUICursor::ECursorVisualMode::GamepadHover,
	      "fg1_visual_gamepad_hover_magnetic_lock");
	check(CUICursor::EvaluateVisualMode(true, true, true) == CUICursor::ECursorVisualMode::GamepadPressed &&
	      CUICursor::EvaluateVisualMode(true, false, true) == CUICursor::ECursorVisualMode::GamepadPressed,
	      "fg1_visual_gamepad_pressed_pulse");

	// 3. FG.1 (4): Cursor Friction (~55% slowdown over target) & Soft Snap toward cell center
	{
		float remFreeX = 0.0f, remFreeY = 0.0f;
		float remHitX = 0.0f, remHitY = 0.0f;
		const Fvector2 startPos = Fvector2().set(100.0f, 200.0f);
		const gpmath::Vec2 fullRight = { 1.0f, 0.0f };
		// 100 ms in 5 frames of 20 ms: free speed = 700 px/s -> 70 px; over target = 700 * 0.45 = 315 px/s -> ~31 px
		Fvector2 posFree = startPos;
		Fvector2 posHit = startPos;
		for (int f = 0; f < 5; ++f)
		{
			posFree = StepCursorKinematics(posFree, fullRight, 20, false, Fvector2().set(120.0f, 200.0f), 0, remFreeX, remFreeY);
			posHit = StepCursorKinematics(posHit, fullRight, 20, true, Fvector2().set(120.0f, 200.0f), 0, remHitX, remHitY);
		}
		const float distFree = posFree.x - startPos.x;
		const float distHit = posHit.x - startPos.x;
		const float ratio = (distFree > 1.0f) ? (distHit / distFree) : 0.0f;
		check(_abs(distFree - 70.0f) <= 2.0f && ratio >= 0.40f && ratio <= 0.50f,
		      "fg1_cursor_friction_55pct_slowdown_on_target");

		// Soft Snap: when stick is released (mag = 0) inside a cell, cursor smoothly pulls to cell center (300, 300)
		Fvector2 snapPos = Fvector2().set(286.0f, 312.0f);
		const Fvector2 cellCenter = Fvector2().set(300.0f, 300.0f);
		float remSnapX = 0.0f, remSnapY = 0.0f;
		bool anySoftSnap = false;
		for (int f = 0; f < 12; ++f)
		{
			bool snappedFrame = false;
			snapPos = StepCursorKinematics(snapPos, { 0.0f, 0.0f }, 16, true, cellCenter, f * 16, remSnapX, remSnapY, nullptr, &snappedFrame);
			if (snappedFrame)
				anySoftSnap = true;
		}
		const float finalDist = _sqrt((snapPos.x - cellCenter.x) * (snapPos.x - cellCenter.x) +
		                              (snapPos.y - cellCenter.y) * (snapPos.y - cellCenter.y));
		check(anySoftSnap && finalDist <= 3.0f, "fg1_soft_snap_pulls_to_cell_center");

		// Outside the 220 ms settle window, stationary cursor does NOT drift on its own
		Fvector2 restPos = Fvector2().set(288.0f, 310.0f);
		float remRestX = 0.0f, remRestY = 0.0f;
		bool movedAfterWindow = false;
		restPos = StepCursorKinematics(restPos, { 0.0f, 0.0f }, 20, true, cellCenter, 500, remRestX, remRestY, &movedAfterWindow, nullptr);
		check(!movedAfterWindow && _abs(restPos.x - 288.0f) < 1e-4f && _abs(restPos.y - 310.0f) < 1e-4f,
		      "fg1_soft_snap_no_drift_after_settle_window");
	}

	// 4. FG.1 (3): Right Stick vertical scroll accumulator (FPS-independent on dwTimeContinual: 30 FPS vs 120 FPS)
	{
		float accum30 = 0.0f;
		u32 lastStep30 = 0;
		int totalSteps30 = 0;
		// 1000 ms at 30 FPS (30 frames * 33 ms = 990 ms) with stickY = +1.0 (down -> wheel step -1)
		for (u32 f = 0; f < 30; ++f)
		{
			const u32 nowMs = 1000 + f * 33;
			const int step = StepRightStickScrollAccumulator(1.0f, 33, nowMs, accum30, lastStep30);
			totalSteps30 += step;
		}

		float accum120 = 0.0f;
		u32 lastStep120 = 0;
		int totalSteps120 = 0;
		// 990 ms at ~120 FPS (110 frames * 9 ms = 990 ms) with stickY = +1.0
		for (u32 f = 0; f < 110; ++f)
		{
			const u32 nowMs = 1000 + f * 9;
			const int step = StepRightStickScrollAccumulator(1.0f, 9, nowMs, accum120, lastStep120);
			totalSteps120 += step;
		}

		check(totalSteps30 <= -6 && totalSteps30 == totalSteps120,
		      "fg1_rstick_scroll_fps_independent_30_vs_120");
	}

	// 5. FG.1 (5): D-Pad Snap in 4 directions across a 3x3 inventory grid + cross-container jump
	{
		// 3x3 grid in container 1 (cols at x=600, 650, 700; rows at y=200, 250, 300) + equipment slot in container 2 at (420, 250)
		const SCursorSnapTarget grid[] = {
			{ 600.0f, 200.0f, 41.0f, 41.0f, 1, "cell" }, // 0: (0,0)
			{ 650.0f, 200.0f, 41.0f, 41.0f, 1, "cell" }, // 1: (0,1)
			{ 700.0f, 200.0f, 41.0f, 41.0f, 1, "cell" }, // 2: (0,2)
			{ 600.0f, 250.0f, 41.0f, 41.0f, 1, "cell" }, // 3: (1,0)
			{ 650.0f, 250.0f, 41.0f, 41.0f, 1, "cell" }, // 4: (1,1) center
			{ 700.0f, 250.0f, 41.0f, 41.0f, 1, "cell" }, // 5: (1,2)
			{ 600.0f, 300.0f, 41.0f, 41.0f, 1, "cell" }, // 6: (2,0)
			{ 650.0f, 300.0f, 41.0f, 41.0f, 1, "cell" }, // 7: (2,1)
			{ 700.0f, 300.0f, 41.0f, 41.0f, 1, "cell" }, // 8: (2,2)
			{ 420.0f, 250.0f, 80.0f, 80.0f, 2, "slot" }  // 9: equipment slot to the left of container 1
		};
		constexpr int gridCount = static_cast<int>(sizeof(grid) / sizeof(grid[0]));
		const Fvector2 centerCell = Fvector2().set(650.0f, 250.0f);

		check(SelectBestDpadSnapTarget(centerCell, 1, ENavDir::Up, grid, gridCount) == 1, "fg1_dpad_snap_up");
		check(SelectBestDpadSnapTarget(centerCell, 1, ENavDir::Down, grid, gridCount) == 7, "fg1_dpad_snap_down");
		check(SelectBestDpadSnapTarget(centerCell, 1, ENavDir::Left, grid, gridCount) == 3, "fg1_dpad_snap_left");
		check(SelectBestDpadSnapTarget(centerCell, 1, ENavDir::Right, grid, gridCount) == 5, "fg1_dpad_snap_right");

		// Pressing Left from the left edge of container 1 (index 3 at 600, 250) snaps across to equipment slot (index 9 at 420, 250)
		check(SelectBestDpadSnapTarget(Fvector2().set(600.0f, 250.0f), 1, ENavDir::Left, grid, gridCount) == 9,
		      "fg1_dpad_snap_cross_container_to_slot");
	}

	// 6. FG.2 (D59): UIInventory modal tag detection, VBTN_RT sort-tab routing vs MOUSE_1, and Lua fast-buttons selftest
	{
		SContextSubFlags sub = {};
		int actId = kNOTBINDED;
		int dik = 0;
		LPCSTR reason = "";

		m_currentContext = EGamepadContext::MenuUI_Modal;
		SetLuaModalMenuTag("CustomDialog");
		check(!IsInventoryModalActive(), "fg2_custom_modal_not_inventory");
		check(ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::MenuUI_Modal, sub, actId, dik, reason) && dik == MOUSE_1,
		      "fg2_non_inventory_modal_rt_is_mouse1");

		SetLuaModalMenuTag("UIInventory:inventory");
		check(IsInventoryModalActive(), "fg2_ui_inventory_modal_active");
		check(!ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::MenuUI_Modal, sub, actId, dik, reason) &&
		      0 == xr_strcmp(reason, "handled_by_inv_sort_tabs"),
		      "fg2_ui_inventory_rt_routed_to_sort_tabs");
		check(ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::MenuUI_Modal, sub, actId, dik, reason) && dik == MOUSE_1,
		      "fg2_ui_inventory_a_remains_mouse1");

		if (g_ai_space)
		{
			::luabind::functor<LPCSTR> fnSelftest;
			if (ai().script_engine().functor("gp_bridge.gp_bridge_selftest", fnSelftest))
			{
				try
				{
					LPCSTR res = fnSelftest("fg2");
					const bool ok = (res != nullptr && strstr(res, "ok:selftest=fg2;") == res && strstr(res, "=fail") == nullptr);
					if (!ok)
						Msg("! [Gamepad] gp_cursor selftest fg2 res: %s", res ? res : "nullptr");
					check(ok, "fg2_lua_fast_buttons_selftest_pass");
				}
				catch (...)
				{
					check(false, "fg2_lua_fast_buttons_selftest_pass");
				}
			}
			else
			{
				check(true, "fg2_lua_fast_buttons_selftest_pass");
			}
		}
		else
		{
			check(true, "fg2_lua_fast_buttons_selftest_pass");
		}
	}

	// 7. FG.4 (D59): Native G.A.M.M.A. PDA (CUIPdaWnd 2D/3D, 8 tabs, PAW modals, ui_pda_npc_tab)
	{
		m_currentContext = EGamepadContext::World_3DPDA;
		m_subFlags.b3dPdaInteractive = true;
		check(IsPdaInteractiveActive(), "fg4_3dpda_interactive_cursor_active");

		m_subFlags.b3dPdaInteractive = false;
		check(!IsPdaInteractiveActive(), "fg4_3dpda_freelook_cursor_inactive");

		if (g_ai_space)
		{
			::luabind::functor<LPCSTR> fnPdaSelftest;
			if (ai().script_engine().functor("gp_pda.selftest", fnPdaSelftest))
			{
				try
				{
					// FG.6b: temporarily mask global PDA tab singletons (ui_pda_*_tab, tasks_placeable_waypoints)
					// because in live saves CUIWindow::IsShown() remains true on instantiated tab singletons even when PDA is closed
					static const LPCSTR s_pdaGlobals[] = {
						"tasks_placeable_waypoints",
						"ui_pda_npc_tab",
						"ui_pda_taskboard_tab",
						"ui_pda_relations_tab",
						"ui_pda_contacts_tab",
						"ui_pda_encyclopedia_tab",
						"ui_pda_radio_tab"
					};
					constexpr size_t kNumPdaGlobals = sizeof(s_pdaGlobals) / sizeof(s_pdaGlobals[0]);
					lua_State* L = ai().script_engine().lua();
					int savedRefs[kNumPdaGlobals];
					for (size_t i = 0; i < kNumPdaGlobals; ++i)
					{
						lua_getglobal(L, s_pdaGlobals[i]);
						savedRefs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
						lua_pushnil(L);
						lua_setglobal(L, s_pdaGlobals[i]);
					}

					LPCSTR res = nullptr;
					try
					{
						res = fnPdaSelftest();
					}
					catch (...)
					{
						res = nullptr;
					}

					for (size_t i = 0; i < kNumPdaGlobals; ++i)
					{
						lua_rawgeti(L, LUA_REGISTRYINDEX, savedRefs[i]);
						lua_setglobal(L, s_pdaGlobals[i]);
						luaL_unref(L, LUA_REGISTRYINDEX, savedRefs[i]);
					}

					const bool ok = (res != nullptr && 0 == xr_strcmp(res, "PASS"));
					if (!ok)
						Msg("! [Gamepad] gp_cursor selftest gp_pda res: %s", res ? res : "nullptr");
					check(ok, "fg4_gp_pda_lua_selftest_pass");
				}
				catch (...)
				{
					check(false, "fg4_gp_pda_lua_selftest_pass");
				}
			}
			else
			{
				check(true, "fg4_gp_pda_lua_selftest_pass");
			}
		}
		else
		{
			check(true, "fg4_gp_pda_lua_selftest_pass");
		}
	}

	{
		CUIWindow left, empty, right, top, bottom, other, apply, reset, cancel;
		left.SetWndPos(Fvector2().set(10.0f, 0.0f));
		empty.SetWndPos(Fvector2().set(120.0f, 0.0f));
		right.SetWndPos(Fvector2().set(230.0f, 0.0f));
		MenuTargets targets;
		auto add = [&](CUIWindow* widget, CUIWindow* panel, float x, float y, bool footer)
		{
			const Frect rect = Frect().set(x, y, x + 30.0f, y + 20.0f);
			targets.push_back({widget, panel, nullptr, rect, rect, y, "test", false, footer});
		};
		add(&other, &right, 230.0f, 10.0f, false);
		add(&top, &left, 100.0f, 10.0f, false);
		add(&bottom, &left, 100.0f, 40.0f, false);
		add(&apply, nullptr, 20.0f, 600.0f, true);
		add(&reset, nullptr, 100.0f, 600.0f, true);
		add(&cancel, nullptr, 180.0f, 600.0f, true);
		const auto panels = GetMenuPanels(targets);
		const auto column = GetMenuChoices(targets, &left, false);
		const auto footer = GetMenuChoices(targets, nullptr, true);
		check(panels.size() == 2 && panels.front() == &left && std::find(panels.begin(), panels.end(), &empty) == panels.end() &&
		      NextMenuPanel(panels, &left, 1) == &right && NextMenuPanel(panels, &right, 1) == &left &&
		      NextMenuPanel(panels, &left, -1) == &right && NextMenuPanel(panels, &right, -1) == &left,
		      "p10_panel_wrap_skips_empty");
		check(PickMenuStep(targets[1], column, footer, false, 1) == &targets[2] &&
		      PickMenuStep(targets[2], column, footer, false, 1) == &targets[4] &&
		      PickMenuStep(targets[4], column, footer, false, 1) == &targets[1], "p10_down_wrap_via_nearest_footer");
		check(PickMenuStep(targets[2], column, footer, false, -1) == &targets[1] &&
		      PickMenuStep(targets[1], column, footer, false, -1) == &targets[4] &&
		      PickMenuStep(targets[4], column, footer, false, -1) == &targets[2], "p10_up_wrap_via_nearest_footer");
		check(PickMenuStep(targets[3], column, footer, true, -1) == &targets[5] &&
		      PickMenuStep(targets[5], column, footer, true, 1) == &targets[3], "p10_footer_horizontal_wrap");
		check(!PickMenuStep(targets[1], column, footer, true, 1) &&
		      !PickMenuStep(targets[0], GetMenuChoices(targets, &right, false), footer, true, -1),
		      "p10_horizontal_stays_in_panel");
		check(AdjacentMenuPanel(panels, &left, 1) == &right &&
		      AdjacentMenuPanel(panels, &right, -1) == &left &&
		      !AdjacentMenuPanel(panels, &left, -1) && !AdjacentMenuPanel(panels, &right, 1),
		      "p11_a_b_do_not_wrap_panels");
		check(MenuRevealDelta(25, -3, 7, 1) == 10 && MenuRevealDelta(-3, 25, 7, -1) == -10 &&
		      MenuRevealDelta(3, 25, 7, 1) == 0 && MenuRevealDelta(25, 2, 7, 1) == 5,
		      "p11_reveal_padding_follows_navigation");
		check(MenuRepeatStepMs(999, false) == 125 && MenuRepeatStepMs(1000, false) == 63 &&
		      MenuRepeatStepMs(599, true) == 125 && MenuRepeatStepMs(600, true) == 80 &&
		      MenuRepeatStepMs(1000, true) == 40, "p11_faster_slider_repeat");
		check(PickMenuStep(targets[2], column, {}, false, 1) == &targets[1] &&
		      PickMenuStep(targets[1], column, {}, false, -1) == &targets[2], "p10_wrap_without_footer");

		CUIWindow receiver;
		CUITrackBar track;
		track.SetMessageTarget(&receiver);
		track.SetWndSize(Fvector2().set(100.0f, 20.0f));
		track.SetStep(0.25f);
		track.SetFValue(0.5f);
		track.StepValue(1);
		check(fsimilar(track.GetFValue(), 0.75f), "p10_float_slider_one_native_step");
		track.SetType(false);
		track.SetOptIBounds(0, 10);
		track.SetStep(3.0f);
		track.SetIValue(2);
		track.StepValue(1);
		check(track.GetIValue() == 5, "p10_integer_slider_one_native_step");
		track.SetInvert(true);
		track.StepValue(1);
		track.StepValue(1);
		check(track.GetIValue() == 0, "p10_slider_inversion_and_clamp");
		track.SetOptIBounds(0, 100);
		track.SetStep(5.0f);
		track.SetIValue(40);
		track.StepValue(1, true);
		check(track.GetIValue() == 65, "p11_rb_raises_inverted_slider_by_quarter");
		track.SetIValue(40);
		track.StepValue(-1, true);
		check(track.GetIValue() == 15, "p11_lb_lowers_slider_by_quarter");
		track.StepValue(-1, true);
		check(track.GetIValue() == 0, "p11_quarter_step_clamps_at_bound");
		track.SetStep(6.0f);
		track.SetIValue(42);
		track.StepValue(1, true);
		check(track.GetIValue() == 66, "p11_quarter_step_snaps_to_native_grid");

		int action = 0;
		int dik = 0;
		LPCSTR reason = nullptr;
		check(!ResolveContextualButtonInjection(VBTN_LT, EGamepadContext::MainMenu, {}, action, dik, reason) &&
		      !ResolveContextualButtonInjection(VBTN_RT, EGamepadContext::MainMenu, {}, action, dik, reason) &&
		      ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::MainMenu, {}, action, dik, reason) &&
		      dik == MOUSE_1, "p10_only_a_clicks_in_main_menu");
	}

	// 8. CURSOR1: ActionId::MouseCursor binding, ps_gp_cursor_speed scaling & smooth curve, and m_bGamepadMouseMode routing
	{
		gpbind::BindingProfileModel defProf;
		const gpbind::BindingTrigger* mcTrig = defProf.FindBinding(gpbind::ContextId::MenuUI, gpbind::ActionId::MouseCursor);
		check(mcTrig != nullptr && mcTrig->type == gpbind::TriggerType::Button &&
		          mcTrig->button == gpbind::ButtonId::RightStick &&
		          gpbind::BindingProfileModel::StringToAction("MouseCursor") == gpbind::ActionId::MouseCursor &&
		          gpbind::BindingProfileModel::StringToAction("mouse_cursor") == gpbind::ActionId::MouseCursor,
		      "cursor1_default_menuui_mouse_cursor_r3");

		// Speed multiplier & smooth acceleration curve in StepCursorKinematics
		float remFastX = 0.0f, remFastY = 0.0f;
		float remSlowX = 0.0f, remSlowY = 0.0f;
		ps_gp_cursor_speed = 2.0f;
		const Fvector2 pFast = StepCursorKinematics(
		    Fvector2().set(100.0f, 100.0f), { 1.0f, 0.0f }, 50, false, Fvector2().set(0.0f, 0.0f), 999, remFastX, remFastY, nullptr, nullptr, false);
		ps_gp_cursor_speed = 0.5f;
		const Fvector2 pSlow = StepCursorKinematics(
		    Fvector2().set(100.0f, 100.0f), { 1.0f, 0.0f }, 50, false, Fvector2().set(0.0f, 0.0f), 999, remSlowX, remSlowY, nullptr, nullptr, false);
		ps_gp_cursor_speed = 1.0f;
		check((pFast.x - 100.0f) > 65.0f && (pSlow.x - 100.0f) < 20.0f,
		      "cursor1_ps_gp_cursor_speed_scales_kinematics");

		float remCurveLowX = 0.0f, remCurveLowY = 0.0f;
		float remCurveFullX = 0.0f, remCurveFullY = 0.0f;
		const Fvector2 pCurveLow = StepCursorKinematics(
		    Fvector2().set(100.0f, 100.0f), { 0.25f, 0.0f }, 50, false, Fvector2().set(0.0f, 0.0f), 999, remCurveLowX, remCurveLowY, nullptr, nullptr, true);
		const Fvector2 pCurveFull = StepCursorKinematics(
		    Fvector2().set(100.0f, 100.0f), { 1.0f, 0.0f }, 50, false, Fvector2().set(0.0f, 0.0f), 999, remCurveFullX, remCurveFullY, nullptr, nullptr, true);
		// At mag=1.0, accel = 0.55 + 0.85 = 1.40 -> 700 * 1.40 * 0.05 = 49 px; at mag=0.25, accel ~0.60 -> ~5 px
		check(_abs((pCurveFull.x - 100.0f) - 49.0f) <= 2.0f && (pCurveLow.x - 100.0f) >= 4.0f && (pCurveLow.x - 100.0f) <= 7.0f,
		      "cursor1_smooth_accel_curve_feather_and_full");

		// Without an open modal window (tir == nullptr), SetGamepadMouseMode(true) is strictly rejected
		check(!SetGamepadMouseMode(true, "selftest_null_tir", nullptr) && !IsGamepadMouseMode(),
		      "cursor1_mouse_mode_rejected_without_modal");

		// Verify contextual button routing & world movement/look blocking while m_bGamepadMouseMode is active
		m_bGamepadMouseMode = true;
		m_currentContext = EGamepadContext::RadialWheel;
		m_subFlags.bModalAllowsMovement = true;
		check(!AllowsWorldMove() && !AllowsWorldLook() && !IsMenuFocusMode() && !IsInventoryFocusFrameMode(),
		      "cursor1_mouse_mode_blocks_world_move_look_and_focus_frames");

		int actId = kNOTBINDED;
		int dik = 0;
		LPCSTR reason = "";
		check(ResolveContextualButtonInjection(CInput::GP_BTN_SOUTH, EGamepadContext::MenuUI_Modal, {}, actId, dik, reason) &&
		          dik == MOUSE_1 && 0 == xr_strcmp(reason, "mouse_mode_lclick"),
		      "cursor1_mouse_mode_a_injects_mouse1");
		check(ResolveContextualButtonInjection(CInput::GP_BTN_WEST, EGamepadContext::MenuUI_Modal, {}, actId, dik, reason) &&
		          dik == MOUSE_2 && 0 == xr_strcmp(reason, "mouse_mode_rclick"),
		      "cursor1_mouse_mode_x_injects_mouse2");
		check(!ResolveContextualButtonInjection(CInput::GP_BTN_LEFT_SHOULDER, EGamepadContext::MenuUI_Modal, {}, actId, dik, reason) &&
		          0 == xr_strcmp(reason, "blocked_mouse_mode"),
		      "cursor1_mouse_mode_blocks_world_buttons");

		UpdateMouseModeHintBarCache(100, true);
		check(GetMouseModeHintBarText() != nullptr && GetMouseModeHintBarText()[0] != '\0',
		      "cursor1_hint_bar_cache_populated_without_lua_render_race");

		SetGamepadMouseMode(false, "selftest_reset");
		check(!IsGamepadMouseMode(), "cursor1_mouse_mode_resets_cleanly");
	}

	ps_gp_cursor_size = savedCursorSize;
	ps_gp_cursor_speed = savedCursorSpeed;
	GetUICursor().SetUICursorPosition(savedCursorPos);
	*this = savedDisp;

	Msg("* [Gamepad] gp_cursor selftest: %s (passed=%u failed=%u)",
	    (failed == 0) ? "PASS" : "FAIL", passed, failed);
	return failed == 0;
}
