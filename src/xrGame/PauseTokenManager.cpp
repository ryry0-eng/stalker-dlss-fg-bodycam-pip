#include "stdafx.h"
#include "PauseTokenManager.h"
#include "GamepadContextDispatcher.h"
#include "../xrEngine/device.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/GamepadDeviceManager.h"
#include "../xrEngine/GamepadHaptics.h"
#include "../xrEngine/GamepadGlyphs.h"
#include "../xrEngine/GamepadErrorGuard.h"
#include "../xrEngine/GamepadBindings.h"
#include "GamepadOnScreenKeyboard.h"
#include "GamepadWidgets.h"
#include "../xrSound/Sound.h"
#include "string_table.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIActorMenu.h"
#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "InventoryBox.h"
#include "eatable_item.h"
#include "WeaponAmmo.h"
#include "Weapon.h"
#include "HudItem.h"
#include "player_hud.h"
#include "CustomDevice.h"
#include "UIGameCustom.h"
#include "ui/UIDialogWnd.h"
#include "ui/UIStatic.h"
#include "ui_base.h"
#include "MainMenu.h"
#include <dinput.h>
#include <sstream>
#include "ai_space.h"
#include "../xrServerEntities/script_engine.h"
#include "script_game_object.h"
#include <luabind/luabind.hpp>
extern "C" {
#include <lua.h>
}

extern ENGINE_API BOOL bShowPauseString;
extern ENGINE_API int ps_gp_telemetry;
extern ENGINE_API void (*g_pPauseExternalUnpauseCallback)(BOOL bTimer, BOOL bSound, LPCSTR reason);

// C4.2c (D33): режим анимаций расходников в очереди после паузы (0 = per_category, 1 = per_item)
u32 g_gp_queue_consumable_anim = 0;

static constexpr u32 GP_TX_MIN_STEP_FRAMES = 2;
static constexpr u32 GP_TX_HANDS_FREE_STABLE_FRAMES = 3;
static constexpr u32 GP_TX_STEP_TIMEOUT_MS = 15000;

static void PauseTokenExternalUnpauseBridge(BOOL bTimer, BOOL bSound, LPCSTR reason)
{
	CPauseTokenManager::Instance().OnExternalUnpause(bTimer, bSound, reason);
}

static void ApplyLiveGamepadPromptsRefresh()
{
	CStringTable().ReparseKeyBindings();
	if (CurrentGameUI() && CurrentGameUI()->UIMainIngameWnd)
	{
		CurrentGameUI()->UIMainIngameWnd->UpdateQuickSlots();
	}
}

// D1.2 + D5.1: проброс события смены активного источника ввода из CInput (xrEngine) в CStringTable/HUD и Lua-мост gp_bridge
static void InputSourceChangedLuaBridge(CInput::EActiveInputSource newSource,
                                        CInput::EActiveInputSource prevSource,
                                        LPCSTR reason,
                                        u32 dwCurTime)
{
	auto& glyphs = CGamepadGlyphManager::Instance();
	glyphs.EnsureInitialized();
	glyphs.RefreshFromActiveDevice("input_source_switch");
	glyphs.NotifyPromptsDirty((newSource == CInput::EActiveInputSource::Gamepad) ? "source_gamepad" : "source_kbm");
	ApplyLiveGamepadPromptsRefresh();

	if (!g_ai_space)
		return;

	::luabind::functor<LPCSTR> fn;
	if (!ai().script_engine().functor("gp_bridge.on_input_source_changed", fn))
		return;

	try
	{
		(void)fn(CInput::GetInputSourceName(newSource),
		         CInput::GetInputSourceName(prevSource),
		         (reason && reason[0]) ? reason : "unknown",
		         static_cast<int>(dwCurTime));
	}
	catch (...)
	{
		Msg("! [Gamepad] Exception in gp_bridge.on_input_source_changed");
	}
}

// C4.1: прототип, настройки — Этап D
CPauseTokenManager& CPauseTokenManager::Instance()
{
	static CPauseTokenManager instance;
	return instance;
}

u32 CPauseTokenManager::Acquire(LPCSTR tag)
{
	LPCSTR safeTag = (tag && tag[0]) ? tag : "default";
	const bool isFirst = m_tokens.empty();

	g_pPauseExternalUnpauseCallback = &PauseTokenExternalUnpauseBridge;

	if (isFirst)
	{
		m_bExtTimer = Device.Paused();
		m_iBaseSoundCounter = ::Sound ? ::Sound->pause_emitters_count() : 1;
		if (m_iBaseSoundCounter < 1)
			m_iBaseSoundCounter = 1;
		Device.Pause(TRUE, !m_bExtTimer, TRUE, "gp_token");
		m_bOwnTimer = !m_bExtTimer && Device.Paused();
		m_bOwnSound = (::Sound != nullptr) && (::Sound->pause_emitters_count() > m_iBaseSoundCounter);
		m_bSavedShowPauseString = bShowPauseString;
		bShowPauseString = FALSE;
		m_lastCheckTimeContinual = Device.dwTimeContinual;
	}

	const u32 id = m_nextId++;
	if (m_nextId == 0)
		m_nextId = 1;

	TokenEntry entry;
	entry.id = id;
	entry.tag = safeTag;
	m_tokens.push_back(entry);

	// D5.2: мгновенная остановка вибрации при выдаче токена паузы (открытие модального меню/радиалов)
	if (pInput && pInput->GetGamepadHaptics())
	{
		pInput->GetGamepadHaptics()->SetPauseTokenGate(true);
	}

	if (ps_gp_telemetry && pInput)
	{
		const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : -1;
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_pause_acquire,%u,%u,%s,%u,%d,%d,%d,%d,%d",
		           Device.dwTimeContinual,
		           id,
		           safeTag,
		           Count(),
		           m_bOwnTimer ? 1 : 0,
		           m_bOwnSound ? 1 : 0,
		           Device.Paused() ? 1 : 0,
		           Device.GetLastSndEmitters(),
		           sndCounter);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return id;
}

void CPauseTokenManager::RestoreEngineStateOnEmpty(LPCSTR reason)
{
	const int sndBefore = ::Sound ? ::Sound->pause_emitters_count() : 0;
	const BOOL releaseTimer = (m_bOwnTimer && Device.Paused()) ? TRUE : FALSE;
	const BOOL releaseSound = (m_bOwnSound && ::Sound && sndBefore > m_iBaseSoundCounter) ? TRUE : FALSE;

	Device.Pause(FALSE, releaseTimer, releaseSound, "gp_token");
	bShowPauseString = m_bSavedShowPauseString;

	// D5.2: снятие барьера токена паузы для микшера вибрации, когда стек токенов опустел
	if (pInput && pInput->GetGamepadHaptics())
	{
		pInput->GetGamepadHaptics()->SetPauseTokenGate(false);
	}

	m_bExtTimer = false;
	m_bOwnTimer = false;
	m_bOwnSound = false;
	m_iBaseSoundCounter = 1;
	m_bSavedShowPauseString = TRUE;

	if (ps_gp_telemetry && pInput)
	{
		LPCSTR safeReason = (reason && reason[0]) ? reason : "gp_token";
		const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : -1;
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_pause_empty,%u,%s,%d,%d,%d,%d",
		           Device.dwTimeContinual,
		           safeReason,
		           releaseTimer ? 1 : 0,
		           releaseSound ? 1 : 0,
		           Device.Paused() ? 1 : 0,
		           sndCounter);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

bool CPauseTokenManager::Release(u32 id)
{
	if (m_tokens.empty())
		return false;

	auto it = m_tokens.end();
	for (auto cur = m_tokens.begin(); cur != m_tokens.end(); ++cur)
	{
		if (cur->id == id)
		{
			it = cur;
			break;
		}
	}

	if (it == m_tokens.end())
		return false;

	shared_str releasedTag = it->tag;
	m_tokens.erase(it);

	if (m_tokens.empty())
	{
		RestoreEngineStateOnEmpty(releasedTag.c_str());
		FlushTxQueue(releasedTag.c_str());
	}
	else if (ps_gp_telemetry && pInput)
	{
		const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : -1;
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_pause_release,%u,%u,%s,%u,%d,%d,%d,%d,%d",
		           Device.dwTimeContinual,
		           id,
		           releasedTag.c_str(),
		           Count(),
		           m_bOwnTimer ? 1 : 0,
		           m_bOwnSound ? 1 : 0,
		           Device.Paused() ? 1 : 0,
		           Device.GetLastSndEmitters(),
		           sndCounter);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return true;
}

bool CPauseTokenManager::ReleaseTop(LPCSTR reason)
{
	if (m_tokens.empty())
		return false;

	const u32 topId = m_tokens.back().id;
	(void)reason;
	return Release(topId);
}

void CPauseTokenManager::ReleaseAll(LPCSTR reason)
{
	LPCSTR safeReason = (reason && reason[0]) ? reason : "release_all";
	if (!m_modalTestCtrl.GetModalStack().IsEmpty())
	{
		m_modalTestCtrl.EmergencyCloseAll(safeReason, 0, Device.dwTimeContinual, nullptr);
	}
	CGamepadContextDispatcher::Instance().ResetLuaHints(safeReason);
	CloseFocusTestWindow();
	ResetAllTimeFactors(safeReason);
	if (!m_txQueue.empty() || !m_collapsedEatBatch.empty() || m_runnerState != ETxRunnerState::Idle)
	{
		ClearTxQueue(safeReason);
	}

	if (m_tokens.empty())
		return;

	const u32 droppedCount = Count();
	m_tokens.clear();

	Msg("* [Gamepad] CPauseTokenManager::ReleaseAll: dropped=%u reason=\"%s\" dwTimeContinual=%u",
	    droppedCount, safeReason, Device.dwTimeContinual);

	RestoreEngineStateOnEmpty(safeReason);
}

// E5.1 (D43): единый диспетчер заявок масштаба времени Device.time_factor
float CPauseTokenManager::GetEffectiveTimeFactor() const
{
	float eff = m_fExternalTimeFactor;
	clamp(eff, 0.001f, 10.0f);
	for (const auto& entry : m_timeFactorRequests)
	{
		if (entry.factor < eff)
			eff = entry.factor;
	}
	clamp(eff, 0.001f, 10.0f);
	return eff;
}

void CPauseTokenManager::ApplyEffectiveTimeFactor(LPCSTR reason)
{
	const float eff = GetEffectiveTimeFactor();
	if (!strstr(Core.Params, "-sound_constant_speed"))
		psSpeedOfSound = eff;
	Device.time_factor(eff);

	if (ps_gp_telemetry && pInput)
	{
		LPCSTR safeReason = (reason && reason[0]) ? reason : "time_factor";
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_time_factor,%u,eff=%.3f,ext=%.3f,reqs=%u,reason=%s",
		           Device.dwTimeContinual, eff, m_fExternalTimeFactor,
		           static_cast<u32>(m_timeFactorRequests.size()), safeReason);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

float CPauseTokenManager::RequestTimeFactor(LPCSTR tag, float factor)
{
	LPCSTR safeTag = (tag && tag[0]) ? tag : "gp_wheel";
	float clamped = factor;
	clamp(clamped, 0.001f, 10.0f);

	bool found = false;
	for (auto& entry : m_timeFactorRequests)
	{
		if (0 == xr_strcmp(entry.tag.c_str(), safeTag))
		{
			entry.factor = clamped;
			found = true;
			break;
		}
	}
	if (!found)
	{
		TimeFactorEntry entry;
		entry.tag = safeTag;
		entry.factor = clamped;
		m_timeFactorRequests.push_back(entry);
	}

	ApplyEffectiveTimeFactor(safeTag);
	return GetEffectiveTimeFactor();
}

float CPauseTokenManager::ReleaseTimeFactor(LPCSTR tag)
{
	LPCSTR safeTag = (tag && tag[0]) ? tag : "gp_wheel";
	for (auto it = m_timeFactorRequests.begin(); it != m_timeFactorRequests.end(); ++it)
	{
		if (0 == xr_strcmp(it->tag.c_str(), safeTag))
		{
			m_timeFactorRequests.erase(it);
			ApplyEffectiveTimeFactor(safeTag);
			break;
		}
	}
	return GetEffectiveTimeFactor();
}

void CPauseTokenManager::OnExternalTimeFactorCommand(float factor)
{
	float clamped = factor;
	clamp(clamped, 0.001f, 10.0f);
	m_fExternalTimeFactor = clamped;
	ApplyEffectiveTimeFactor("external_console");
}

void CPauseTokenManager::ResetAllTimeFactors(LPCSTR reason)
{
	const bool hadRequests = !m_timeFactorRequests.empty() || (_abs(m_fExternalTimeFactor - 1.0f) > 1e-4f) || (_abs(Device.time_factor() - 1.0f) > 1e-4f);
	m_timeFactorRequests.clear();
	m_fExternalTimeFactor = 1.0f;
	if (hadRequests)
	{
		ApplyEffectiveTimeFactor((reason && reason[0]) ? reason : "reset_all");
	}
}

// C5.1: жизненный цикл instance_gen в таблице менеджера без изменения раскладки CObject (#pragma pack(4))
void CPauseTokenManager::OnObjectNetSpawn(u16 id)
{
	u64 gen = m_nextInstanceGen++;
	if (m_nextInstanceGen == 0)
		m_nextInstanceGen = 1;
	m_objectInstanceGen[id] = gen;
}

void CPauseTokenManager::OnObjectNetDestroy(u16 id)
{
	m_txnDispatcher.OnObjectNetDestroy(id, Device.dwFrame, Device.dwTimeContinual);
	if (m_pendingBridgeOp.active && m_pendingBridgeOp.stableRef.object_id == id)
	{
		m_pendingBridgeOp = {};
	}
	m_objectInstanceGen[id] = 0;
}

void CPauseTokenManager::ClearObjectInstanceGens()
{
	// CR1-06: always run ReleaseAll so active slowmo time_factor requests (e.g. RadialWheel 0.2x without a pause
	// token), Lua modal hints, and pending item transactions are cleared on level transitions and eQuickLoad.
	ReleaseAll("net_Stop_level_change");
	++m_sessionGen;
	if (m_sessionGen == 0)
		m_sessionGen = 1;
	m_txnDispatcher.OnSessionChanged(m_sessionGen, Device.dwFrame, Device.dwTimeContinual);
	m_pendingBridgeOp = {};
	m_objectInstanceGen.fill(0);
}

u64 CPauseTokenManager::GetObjectInstanceGen(u16 id) const
{
	return m_objectInstanceGen[id];
}

CPauseTokenManager::StableItemID CPauseTokenManager::MakeStableItemID(u16 id) const
{
	StableItemID res;
	res.session_gen = m_sessionGen;
	res.object_id = id;
	res.instance_gen = m_objectInstanceGen[id];
	return res;
}

CGameObject* CPauseTokenManager::ResolveStableItem(const StableItemID& ref) const
{
	if (!ref.IsValid())
		return nullptr;

	if (ref.session_gen != m_sessionGen)
		return nullptr;

	if (m_objectInstanceGen[ref.object_id] != ref.instance_gen)
		return nullptr;

	if (!g_pGameLevel)
		return nullptr;

	CObject* rawObj = Level().Objects.net_Find(ref.object_id);
	if (!rawObj || rawObj->getDestroy())
		return nullptr;

	return smart_cast<CGameObject*>(rawObj);
}

void CPauseTokenManager::RebuildFocusGraphFromActorInventory(u8 tabId)
{
	if (m_focusGraph.GetGroups().empty())
	{
		m_focusGraph.AddGroup("ActorRuck",         gpfocus::FocusLayoutMode::Grid2D, 1, false);
		m_focusGraph.AddGroup("ActorSlotsAndBelt", gpfocus::FocusLayoutMode::List1D, 2, false);
	}

	std::vector<gpfocus::FocusNode> ruckNodes;
	std::vector<gpfocus::FocusNode> equipNodes;

	if (g_pGameLevel && g_actor && g_actor->g_Alive())
	{
		CInventory& inv = g_actor->inventory();
		constexpr int16_t kRuckGridCols = 10;
		int16_t curX = 0;
		int16_t curY = 0;
		int16_t rowMaxH = 1;

		u32 nextNodeId = 1;
		int32_t cellIdx = 0;
		for (CInventoryItem* itm : inv.m_ruck)
		{
			if (!itm || itm->GetDropManual())
				continue;

			const u16 objId = itm->object().ID();
			if (GetObjectInstanceGen(objId) == 0)
				OnObjectNetSpawn(objId);

			const Irect gridRect = itm->GetInvGridRect();
			const int16_t gw = static_cast<int16_t>(_max(1, gridRect.width()));
			const int16_t gh = static_cast<int16_t>(_max(1, gridRect.height()));

			if (curX + gw > kRuckGridCols && curX > 0)
			{
				curX = 0;
				curY = static_cast<int16_t>(curY + rowMaxH);
				rowMaxH = 1;
			}

			u16 usesOrAmmo = 1;
			if (CEatableItem* eat = smart_cast<CEatableItem*>(itm))
			{
				const u8 rem = eat->GetRemainingUses();
				if (rem > 0)
					usesOrAmmo = rem;
			}
			else if (CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(itm))
			{
				if (ammo->m_boxCurr > 0)
					usesOrAmmo = ammo->m_boxCurr;
			}
			else if (CWeapon* wpn = smart_cast<CWeapon*>(itm))
			{
				usesOrAmmo = static_cast<u16>(_max(0, wpn->GetAmmoElapsed()));
			}

			gpfocus::FocusNode n;
			n.node_id                = nextNodeId++;
			n.kind                   = gpfocus::FocusNodeKind::Item;
			n.stable_id.session_gen  = m_sessionGen;
			n.stable_id.object_id    = objId;
			n.stable_id.instance_gen = GetObjectInstanceGen(objId);
			n.key.section            = itm->object().cNameSect().c_str();
			n.key.cond_permille      = gpfocus::FocusItemKey::QuantizeCondition(itm->GetCondition());
			n.key.uses_or_ammo       = usesOrAmmo;
			n.key.grid_x             = curX;
			n.key.grid_y             = curY;
			n.key.grid_w             = gw;
			n.key.grid_h             = gh;
			n.key.list_index         = cellIdx;
			n.key.cell_indx          = cellIdx;
			n.key.bag_id             = 1;
			n.key.tab_id             = tabId;
			n.label                  = std::string(itm->object().cNameSect().c_str()) + "#" + std::to_string(objId);

			ruckNodes.push_back(std::move(n));

			curX = static_cast<int16_t>(curX + gw);
			if (gh > rowMaxH)
				rowMaxH = gh;
			++cellIdx;
		}

		u32 equipNodeId = 1;
		int32_t equipIdx = 0;
		for (u16 s = 1; s <= inv.LastSlot(); ++s)
		{
			CInventoryItem* itm = inv.ItemFromSlot(s);
			if (!itm || itm->GetDropManual())
				continue;

			const u16 objId = itm->object().ID();
			if (GetObjectInstanceGen(objId) == 0)
				OnObjectNetSpawn(objId);

			gpfocus::FocusNode n;
			n.node_id                = equipNodeId++;
			n.kind                   = gpfocus::FocusNodeKind::Item;
			n.stable_id.session_gen  = m_sessionGen;
			n.stable_id.object_id    = objId;
			n.stable_id.instance_gen = GetObjectInstanceGen(objId);
			n.key.section            = itm->object().cNameSect().c_str();
			n.key.cond_permille      = gpfocus::FocusItemKey::QuantizeCondition(itm->GetCondition());
			n.key.uses_or_ammo       = 1;
			n.key.list_index         = equipIdx;
			n.key.cell_indx          = static_cast<int32_t>(s);
			n.key.bag_id             = 2;
			n.key.tab_id             = tabId;
			n.label                  = std::string("slot") + std::to_string(s) + ":" + itm->object().cNameSect().c_str() + "#" + std::to_string(objId);

			equipNodes.push_back(std::move(n));
			++equipIdx;
		}

		for (CInventoryItem* itm : inv.m_belt)
		{
			if (!itm || itm->GetDropManual())
				continue;

			const u16 objId = itm->object().ID();
			if (GetObjectInstanceGen(objId) == 0)
				OnObjectNetSpawn(objId);

			gpfocus::FocusNode n;
			n.node_id                = equipNodeId++;
			n.kind                   = gpfocus::FocusNodeKind::Item;
			n.stable_id.session_gen  = m_sessionGen;
			n.stable_id.object_id    = objId;
			n.stable_id.instance_gen = GetObjectInstanceGen(objId);
			n.key.section            = itm->object().cNameSect().c_str();
			n.key.cond_permille      = gpfocus::FocusItemKey::QuantizeCondition(itm->GetCondition());
			n.key.uses_or_ammo       = 1;
			n.key.list_index         = equipIdx;
			n.key.cell_indx          = 100 + equipIdx;
			n.key.bag_id             = 2;
			n.key.tab_id             = tabId;
			n.label                  = std::string("belt:") + itm->object().cNameSect().c_str() + "#" + std::to_string(objId);

			equipNodes.push_back(std::move(n));
			++equipIdx;
		}
	}

	const u32 nowMs = Device.dwTimeContinual;
	m_focusGraph.RebuildGroupItems(0, ruckNodes,  tabId, nowMs);
	m_focusGraph.RebuildGroupItems(1, equipNodes, tabId, nowMs);
}

void CPauseTokenManager::DumpFocusGraph(LPCSTR args)
{
	string64 subcmd = "";
	if (args && args[0])
	{
		sscanf(args, "%63s", subcmd);
	}

	if (0 == _stricmp(subcmd, "selftest") || 0 == _stricmp(subcmd, "test"))
	{
		const gpfocus::SelfTestSummary summary = gpfocus::RunFocusGraphSelfTest();
		Msg("* [Gamepad] %s", summary.report.c_str());
		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line),
			           "gp_focus_dump_selftest,%u,total=%u,passed=%u,failed=%u,old_regressions=%u",
			           Device.dwTimeContinual,
			           summary.total_cases,
			           summary.passed_cases,
			           summary.failed_cases,
			           summary.old_behavior_regressions);
			pInput->WriteGamepadTelemetryLine(line);
		}
		return;
	}

	if (0 == _stricmp(subcmd, "rebuild"))
	{
		RebuildFocusGraphFromActorInventory(0);
	}
	else if (0 == _stricmp(subcmd, "switch") || 0 == _stricmp(subcmd, "view"))
	{
		if (m_focusGraph.GetGroups().empty())
			RebuildFocusGraphFromActorInventory(0);
		m_focusGraph.SwitchGroupNext(Device.dwTimeContinual);
	}
	else if (m_focusGraph.GetGroups().empty() && g_pGameLevel)
	{
		RebuildFocusGraphFromActorInventory(0);
	}

	const std::string dump = m_focusGraph.FormatDump();
	std::istringstream iss(dump);
	std::string line;
	while (std::getline(iss, line))
	{
		if (!line.empty())
			Msg("* %s", line.c_str());
	}
}

// C5.1 + F1.1 + F1.2: нативное фокусное и модальное окно мода (CUIGamepadFocusTest) на Device.dwTimeContinual
// Поддерживает:
//   - список из N предметов с плавной прокруткой ContinualSmoothScroller и автоповтором на полной паузе;
//   - стек вложенных модальных окон до 3 уровней глубины (L1: список предметов, L2: меню действий предмета, L3: диалог подтверждения);
//   - закрытие строго одного верхнего уровня по кнопке B (gp_ui_cancel) с возвратом фокуса на вызвавший узел и барьером consumed_until_release;
//   - кнопки и хоткеи удаления первого / среднего / последнего / единственного / текущего элемента с проверкой F1.1 восстановления фокуса.
class CUIGamepadFocusTest : public CUIDialogWnd
{
public:
	static constexpr int VISIBLE_ROWS = gpmodal::FocusModalTestController::kDefaultVisibleRows; // 6 видимых строк списка
	static constexpr int MODAL_MAX_ROWS = 10; // до 10 строк в правой панели (ComparisonCard с Accurate Defense Values / QuantityModal / ConfirmDialog / стек L2-L3)
	static constexpr u32 REPEAT_INITIAL_DELAY_MS = 320;
	static constexpr u32 REPEAT_STEP_MS = 90;

	CUIGamepadFocusTest()
	{
		// C5.1 п.1: штатный способ получения ввода диалогом на паузе (UIDialogWnd.cpp:45-54)
		AllowWorkInPause(true);
		AllowMovement(false);
		AllowCursor(false);
		AllowCenterCursor(false);

		SetWndRect(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT));

		m_pHeaderText = xr_new<CUITextWnd>();
		m_pHeaderText->SetAutoDelete(true);
		m_pHeaderText->SetWndPos(Fvector2().set(80.0f, 68.0f));
		m_pHeaderText->SetWndSize(Fvector2().set(864.0f, 32.0f));
		m_pHeaderText->SetFont(UI().Font().pFontLetterica18Russian);
		m_pHeaderText->SetTextColor(color_argb(255, 240, 215, 120));
		m_pHeaderText->SetTextAlignment(CGameFont::alCenter);
		m_pHeaderText->SetText("F2.1 Base Widgets Showcase & F1.2 Modal Stack Test");
		AttachChild(m_pHeaderText);

		m_pStatusText = xr_new<CUITextWnd>();
		m_pStatusText->SetAutoDelete(true);
		m_pStatusText->SetWndPos(Fvector2().set(80.0f, 102.0f));
		m_pStatusText->SetWndSize(Fvector2().set(864.0f, 26.0f));
		m_pStatusText->SetFont(UI().Font().pFontLetterica16Russian);
		m_pStatusText->SetTextColor(color_argb(255, 200, 225, 255));
		m_pStatusText->SetTextAlignment(CGameFont::alCenter);
		m_pStatusText->SetText("D-Pad/L-Stick: nav | LB/RB (Q/E): tabs | LT/RT (Z/C): categories | Y: List/Grid 4xN | R3 (R): slot | L3 (T): 100% ADV");
		AttachChild(m_pStatusText);

		m_pControlsHintText = xr_new<CUITextWnd>();
		m_pControlsHintText->SetAutoDelete(true);
		m_pControlsHintText->SetWndPos(Fvector2().set(80.0f, 130.0f));
		m_pControlsHintText->SetWndSize(Fvector2().set(864.0f, 26.0f));
		m_pControlsHintText->SetFont(UI().Font().pFontLetterica16Russian);
		m_pControlsHintText->SetTextColor(color_argb(255, 175, 210, 190));
		m_pControlsHintText->SetTextAlignment(CGameFont::alCenter);
		m_pControlsHintText->SetText("A/Enter: Quantity/Confirm modal | X: Destructive confirm | G/View: Glyph style | [F2]: Toggle F2.1 Widgets <-> F1.2 Modal");
		AttachChild(m_pControlsHintText);

		// Левая колонка: список или сетка 4xN предметов с прокруткой (VISIBLE_ROWS = 6 строк)
		const float listX = 80.0f;
		const float listY = 168.0f;
		const float listW = 450.0f;
		const float rowH = 44.0f;
		const float rowGap = 6.0f;

		m_pListHeaderText = xr_new<CUITextWnd>();
		m_pListHeaderText->SetAutoDelete(true);
		m_pListHeaderText->SetWndPos(Fvector2().set(listX, listY));
		m_pListHeaderText->SetWndSize(Fvector2().set(listW, 28.0f));
		m_pListHeaderText->SetFont(UI().Font().pFontLetterica18Russian);
		m_pListHeaderText->SetTextColor(color_argb(255, 220, 230, 255));
		m_pListHeaderText->SetTextAlignment(CGameFont::alLeft);
		AttachChild(m_pListHeaderText);

		for (int i = 0; i < VISIBLE_ROWS; ++i)
		{
			CUITextWnd* row = xr_new<CUITextWnd>();
			row->SetAutoDelete(true);
			row->SetWndPos(Fvector2().set(listX, listY + 34.0f + i * (rowH + rowGap)));
			row->SetWndSize(Fvector2().set(listW, rowH));
			row->SetFont(UI().Font().pFontLetterica16Russian);
			row->SetTextAlignment(CGameFont::alLeft);
			row->SetVTextAlignment(valCenter);
			AttachChild(row);
			m_listRows[i] = row;
		}

		// Правая колонка: карточка сравнения Accurate Defense Values (F2.1) или модальное окно (QuantityModal / ConfirmDialog / L2-L3)
		const float modalX = 545.0f;
		const float modalY = 168.0f;
		const float modalW = 400.0f;
		const float modalRowH = 26.0f;
		const float modalRowGap = 3.0f;

		m_pModalTitleText = xr_new<CUITextWnd>();
		m_pModalTitleText->SetAutoDelete(true);
		m_pModalTitleText->SetWndPos(Fvector2().set(modalX, modalY));
		m_pModalTitleText->SetWndSize(Fvector2().set(modalW, 28.0f));
		m_pModalTitleText->SetFont(UI().Font().pFontLetterica18Russian);
		m_pModalTitleText->SetTextColor(color_argb(255, 255, 215, 110));
		m_pModalTitleText->SetTextAlignment(CGameFont::alLeft);
		AttachChild(m_pModalTitleText);

		m_pModalSubText = xr_new<CUITextWnd>();
		m_pModalSubText->SetAutoDelete(true);
		m_pModalSubText->SetWndPos(Fvector2().set(modalX, modalY + 30.0f));
		m_pModalSubText->SetWndSize(Fvector2().set(modalW, 24.0f));
		m_pModalSubText->SetFont(UI().Font().pFontLetterica16Russian);
		m_pModalSubText->SetTextColor(color_argb(255, 190, 205, 230));
		m_pModalSubText->SetTextAlignment(CGameFont::alLeft);
		AttachChild(m_pModalSubText);

		for (int i = 0; i < MODAL_MAX_ROWS; ++i)
		{
			CUITextWnd* mrow = xr_new<CUITextWnd>();
			mrow->SetAutoDelete(true);
			mrow->SetWndPos(Fvector2().set(modalX, modalY + 58.0f + i * (modalRowH + modalRowGap)));
			mrow->SetWndSize(Fvector2().set(modalW, modalRowH));
			mrow->SetFont(UI().Font().pFontLetterica16Russian);
			mrow->SetTextAlignment(CGameFont::alLeft);
			mrow->SetVTextAlignment(valCenter);
			AttachChild(mrow);
			m_modalRows[i] = mrow;
		}

		m_pBridgeResultText = xr_new<CUITextWnd>();
		m_pBridgeResultText->SetAutoDelete(true);
		m_pBridgeResultText->SetWndPos(Fvector2().set(80.0f, 520.0f));
		m_pBridgeResultText->SetWndSize(Fvector2().set(864.0f, 32.0f));
		m_pBridgeResultText->SetFont(UI().Font().pFontLetterica16Russian);
		m_pBridgeResultText->SetTextColor(color_argb(255, 180, 240, 180));
		m_pBridgeResultText->SetTextAlignment(CGameFont::alCenter);
		m_pBridgeResultText->SetText("F2.1 Base Widgets Showcase ready (strictly read-only, inventory mutations = 0)");
		AttachChild(m_pBridgeResultText);

		m_pFooterGlyphText = xr_new<CUITextWnd>();
		m_pFooterGlyphText->SetAutoDelete(true);
		m_pFooterGlyphText->SetWndPos(Fvector2().set(80.0f, 558.0f));
		m_pFooterGlyphText->SetWndSize(Fvector2().set(864.0f, 34.0f));
		m_pFooterGlyphText->SetFont(UI().Font().pFontLetterica18Russian);
		m_pFooterGlyphText->SetTextColor(color_argb(255, 255, 230, 140));
		m_pFooterGlyphText->SetTextAlignment(CGameFont::alCenter);
		AttachChild(m_pFooterGlyphText);

		RefreshVisual(0);
	}

	virtual ~CUIGamepadFocusTest()
	{
		CPauseTokenManager::Instance().EmergencyCloseFocusTestModalStack("wnd_dtor");
	}

	virtual bool WorkInPause() const override
	{
		return true;
	}

	virtual bool NeedCursor() const override
	{
		return false;
	}

	virtual void Show(bool status) override
	{
		const bool wasShown = IsShown();
		inherited::Show(status);

		auto& ptm = CPauseTokenManager::Instance();
		if (status && !wasShown)
		{
			ptm.RebuildFocusGraphFromActorInventory(0);
			if (ptm.GetModalTestController().GetItems().empty())
			{
				ptm.PopulateFocusTestItems(gpmodal::FocusModalTestController::kDefaultListCount);
			}
			if (ptm.GetWidgetShowcaseController().GetItemCatalog().GetAllItems().empty())
			{
				ptm.PopulateWidgetsShowcaseItems();
			}
			if (ptm.GetModalTestController().GetModalStack().IsEmpty())
			{
				ptm.OpenFocusTestModalToDepth(1);
			}
			m_openTimeContinual = Device.dwTimeContinual;
			m_lastUpdateTimeContinual = Device.dwTimeContinual;
			m_bPrevLtPressed = false;
			m_bPrevRtPressed = false;
			RefreshVisual(0);

			const u32 topTok = ptm.TopTokenId();
			Msg("* [Gamepad] CUIGamepadFocusTest::Show(true): mode=%s depth=%zu top_token=%u items=%zu widget_items=%zu dwTimeContinual=%u fTimeDelta=%.4f Paused=%d",
			    ptm.IsFocusTestWidgetsMode() ? "F2.1_Widgets" : "F1.2_Modal",
			    ptm.GetModalTestController().GetModalStack().GetDepth(),
			    topTok,
			    ptm.GetModalTestController().GetItems().size(),
			    ptm.GetWidgetShowcaseController().GetItemCatalog().GetAllItems().size(),
			    Device.dwTimeContinual,
			    Device.fTimeDelta,
			    Device.Paused() ? 1 : 0);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_focus_window,%u,open,mode=%s,token=%u,depth=%zu,items=%zu,paused=%d",
				           Device.dwTimeContinual,
				           ptm.IsFocusTestWidgetsMode() ? "widgets" : "modal",
				           topTok,
				           ptm.GetModalTestController().GetModalStack().GetDepth(),
				           ptm.GetModalTestController().GetItems().size(),
				           Device.Paused() ? 1 : 0);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
		else if (!status && wasShown)
		{
			const u32 releasedCount = ptm.EmergencyCloseFocusTestModalStack("wnd_hide");

			Msg("* [Gamepad] CUIGamepadFocusTest::Show(false): released_layers=%u dwTimeContinual=%u Paused=%d",
			    releasedCount, Device.dwTimeContinual, Device.Paused() ? 1 : 0);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_focus_window,%u,close,released_layers=%u,paused=%d",
				           Device.dwTimeContinual,
				           releasedCount,
				           Device.Paused() ? 1 : 0);
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
		const u32 dtContinual = (now >= m_lastUpdateTimeContinual) ? (now - m_lastUpdateTimeContinual) : 0;
		m_lastUpdateTimeContinual = now;

		PollGamepadNavigationAndActions(now, dtContinual);
		RefreshVisual(dtContinual);
	}

	virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override
	{
		if (!IR_process())
			return false;

		if (keyboard_action == WINDOW_KEY_PRESSED)
		{
			const u32 now = Device.dwTimeContinual;
			const u32 dtContinual = (now >= m_lastUpdateTimeContinual) ? (now - m_lastUpdateTimeContinual) : 0;
			auto& ptm = CPauseTokenManager::Instance();

			if (dik == DIK_F2)
			{
				ptm.SetFocusTestWidgetsMode(!ptm.IsFocusTestWidgetsMode());
				RefreshVisual(dtContinual);
				return true;
			}

			if (dik == DIK_ESCAPE)
			{
				ptm.CancelFocusTestTopModal("key_escape");
				RefreshVisual(dtContinual);
				return true;
			}
			if (dik == DIK_UP || dik == DIK_W)
			{
				StepNav(gpfocus::NavDir::Up, "key_up", false, dtContinual);
				return true;
			}
			if (dik == DIK_DOWN || dik == DIK_S)
			{
				StepNav(gpfocus::NavDir::Down, "key_down", false, dtContinual);
				return true;
			}
			if (dik == DIK_LEFT || dik == DIK_A)
			{
				StepNav(gpfocus::NavDir::Left, "key_left", false, dtContinual);
				return true;
			}
			if (dik == DIK_RIGHT || dik == DIK_D)
			{
				StepNav(gpfocus::NavDir::Right, "key_right", false, dtContinual);
				return true;
			}
			if (dik == DIK_RETURN || dik == DIK_NUMPADENTER || dik == DIK_SPACE)
			{
				ActivateFocusedElement("key_confirm");
				RefreshVisual(dtContinual);
				return true;
			}

			if (ptm.IsFocusTestWidgetsMode())
			{
				auto& wctrl = ptm.GetWidgetShowcaseController();
				if (dik == DIK_Q || dik == DIK_LBRACKET)
				{
					wctrl.StepTopTab(-1, now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_E || dik == DIK_RBRACKET)
				{
					wctrl.StepTopTab(+1, now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_Z)
				{
					wctrl.StepCategoryFilter(-1, now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_C)
				{
					wctrl.StepCategoryFilter(+1, now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_Y || dik == DIK_V)
				{
					wctrl.ToggleListGridView(now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_R)
				{
					wctrl.ToggleCompareSlot(now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_T)
				{
					wctrl.ToggleFullStats100(now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_G || dik == DIK_TAB)
				{
					wctrl.CycleGlyphStyle(now);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_DELETE || dik == DIK_X)
				{
					if (wctrl.GetModalStack().GetDepth() <= 1)
					{
						const u32 tok = ptm.Acquire("gp_widget_confirm");
						if (!wctrl.OpenConfirmModalForFocused(tok, gpmodal::ModalButtonMask(gpmodal::ModalButton::X), now))
							ptm.Release(tok);
						SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
						RefreshVisual(dtContinual);
					}
					return true;
				}
				if (dik == DIK_PRIOR)
				{
					wctrl.GetComparisonCardMutable().ScrollByStick(-1.0f, 120u, now);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_NEXT)
				{
					wctrl.GetComparisonCardMutable().ScrollByStick(+1.0f, 120u, now);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_5)
				{
					ptm.PopulateWidgetsShowcaseItems();
					RefreshVisual(dtContinual);
					return true;
				}
			}
			else
			{
				if (dik == DIK_DELETE || dik == DIK_X)
				{
					ptm.DeleteFocusTestItem("focused");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_1)
				{
					ptm.DeleteFocusTestItem("first");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_2)
				{
					ptm.DeleteFocusTestItem("middle");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_3)
				{
					ptm.DeleteFocusTestItem("last");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_4)
				{
					ptm.DeleteFocusTestItem("single");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_5)
				{
					ptm.PopulateFocusTestItems(gpmodal::FocusModalTestController::kDefaultListCount);
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_Y)
				{
					ptm.ExecuteFocusTestBridgeOp("key_Y_bridge");
					RefreshVisual(dtContinual);
					return true;
				}
				if (dik == DIK_TAB)
				{
					ptm.GetFocusGraph().SwitchGroupNext(now);
					ptm.GetModalTestController().GetActiveGraphMutable().SwitchGroupNext(now);
					RefreshVisual(dtContinual);
					return true;
				}
			}
		}

		return inherited::OnKeyboardAction(dik, keyboard_action);
	}

	void SetBridgeStatusMessage(LPCSTR msg, bool ok)
	{
		if (!m_pBridgeResultText)
			return;
		m_pBridgeResultText->SetTextColor(ok ? color_argb(255, 140, 255, 150) : color_argb(255, 255, 130, 130));
		m_pBridgeResultText->SetText(msg ? msg : "");
	}

	void RefreshVisual(u32 dtContinual)
	{
		const u32 now = Device.dwTimeContinual;
		auto& ptm = CPauseTokenManager::Instance();
		if (ptm.IsFocusTestWidgetsMode())
		{
			RefreshWidgetsShowcaseVisual(now, dtContinual);
			return;
		}

		auto& ctrl = ptm.GetModalTestController();
		const auto& stack = ctrl.GetModalStack();
		const auto& scroller = ctrl.GetScroller();
		const auto& items = ctrl.GetItems();
		const size_t depth = stack.GetDepth();

		const float phase = static_cast<float>(now % 1000u) / 1000.0f;
		const float pulse = 0.5f + 0.5f * _sin(phase * PI_MUL_2);
		const u32 activeGreen = static_cast<u32>(180.0f + 75.0f * pulse);
		const u32 activeColor = color_argb(255, 255, activeGreen, 80);
		const u32 callerColor = color_argb(255, 120, 210, 255);
		const u32 inactiveColor = color_argb(200, 145, 160, 180);
		const u32 dimmedColor = color_argb(140, 100, 110, 125);

		if (m_pHeaderText)
		{
			m_pHeaderText->SetText("F1.2 Focus & Modal Stack Test [Depth 1..3 | Continual Scroll | F1.1 Restore]  ([F2]: F2.1 Widgets)");
		}
		if (m_pStatusText)
		{
			string512 statusBuf;
			xr_sprintf(statusBuf, sizeof(statusBuf),
			           "dwTimeContinual=%u ms (dt=%u ms) | fTimeDelta=%.4f | Paused=%d | ModalDepth=%zu | PauseTokens=%u | TopScroll=%d (%.1f px)",
			           now, dtContinual, Device.fTimeDelta, Device.Paused() ? 1 : 0, depth, ptm.Count(),
			           scroller.GetTargetTopIndex(), scroller.GetVisualOffsetPx());
			m_pStatusText->SetText(statusBuf);
		}
		if (m_pControlsHintText)
		{
			m_pControlsHintText->SetText("X/Del: del focused | [1]: del first | [2]: del middle | [3]: del last | [4]: del single (1->0) | [5]: repopulate 18 | Y: bridge op");
		}
		if (m_pFooterGlyphText)
		{
			m_pFooterGlyphText->SetText("[A] Open L2/L3 Modal   [X] Delete Item   [Y] Bridge Op   [B] Close Top Modal   [F2] Switch to F2.1 Widgets Showcase");
		}

		const gpfocus::FocusNode* rootFocused = ctrl.GetRootGraph().GetFocusedNode();
		const int32_t rootFocusedIdx = rootFocused ? rootFocused->key.list_index : -1;
		const int32_t topIdx = scroller.GetCurrentTopIndex();
		const float subRowPx = scroller.GetSubRowOffsetPx();

		if (m_pListHeaderText)
		{
			string256 listHdr;
			xr_sprintf(listHdr, sizeof(listHdr),
			           "L1 Item List: %zu item(s) | viewport [%d..%d] (target=%d, sub=%.1fpx) | restore=%s",
			           items.size(),
			           items.empty() ? 0 : (topIdx + 1),
			           items.empty() ? 0 : _min<int>(static_cast<int>(items.size()), topIdx + VISIBLE_ROWS),
			           scroller.GetTargetTopIndex() + 1,
			           subRowPx,
			           gpfocus::FocusRestoreReasonToString(ctrl.GetLastDeleteRestoreReason()));
			m_pListHeaderText->SetText(listHdr);
		}

		for (int row = 0; row < VISIBLE_ROWS; ++row)
		{
			CUITextWnd* wnd = m_listRows[row];
			if (!wnd)
				continue;

			wnd->SetWndPos(Fvector2().set(80.0f, 168.0f + 34.0f + row * 50.0f - subRowPx));

			if (items.empty())
			{
				if (row == 0)
				{
					wnd->SetTextColor(activeColor);
					wnd->SetText("[ > <Список пуст — EmptyStatePlaceholder (нажмите [5] для 18 предметов)> < ]");
				}
				else
				{
					wnd->SetTextColor(dimmedColor);
					wnd->SetText("");
				}
				continue;
			}

			const int itemIdx = topIdx + row;
			if (itemIdx < 0 || itemIdx >= static_cast<int>(items.size()))
			{
				wnd->SetTextColor(dimmedColor);
				wnd->SetText("   ---");
				continue;
			}

			const gpfocus::FocusNode& item = items[static_cast<size_t>(itemIdx)];
			const bool isSelectedRoot = (itemIdx == rootFocusedIdx);
			if (depth <= 1)
				wnd->SetTextColor(isSelectedRoot ? activeColor : inactiveColor);
			else
				wnd->SetTextColor(isSelectedRoot ? callerColor : dimmedColor);

			string256 rowText;
			xr_sprintf(rowText, sizeof(rowText),
			           "%s [%02d] %s (id=%u, cond=%u%%, x%u)%s",
			           isSelectedRoot ? (depth <= 1 ? "[ > ]" : "[CAL]") : "     ",
			           itemIdx + 1,
			           item.label.c_str(),
			           item.stable_id.object_id,
			           static_cast<u32>(item.key.cond_permille / 10),
			           static_cast<u32>(item.key.uses_or_ammo),
			           (isSelectedRoot && depth > 1) ? " <-- caller" : "");
			wnd->SetText(rowText);
		}

		// Правая колонка: стек модальных окон (L2 / L3)
		if (depth <= 1)
		{
			if (m_pModalTitleText)
			{
				m_pModalTitleText->SetTextColor(color_argb(255, 170, 200, 230));
				m_pModalTitleText->SetText("Modal Stack: Depth 1 (Root List Active)");
			}
			if (m_pModalSubText)
			{
				m_pModalSubText->SetText("Press A / Enter on an item to push L2 Action Modal");
			}
			for (int i = 0; i < MODAL_MAX_ROWS; ++i)
			{
				if (!m_modalRows[i])
					continue;
				m_modalRows[i]->SetTextColor(dimmedColor);
				if (i == 0)
					m_modalRows[i]->SetText("  [A / Enter] -> Open L2 Item Actions");
				else if (i == 1)
					m_modalRows[i]->SetText("  [X / Del]   -> Delete focused item");
				else if (i == 2)
					m_modalRows[i]->SetText("  [1/2/3/4]   -> Delete first/mid/last/single");
				else if (i == 3)
					m_modalRows[i]->SetText("  [B / Esc]   -> Close Focus Test Window");
				else
					m_modalRows[i]->SetText("");
			}
		}
		else
		{
			const gpmodal::ModalLayer* topLayer = stack.GetTopLayer();
			const gpfocus::FocusGraph& activeGraph = ctrl.GetActiveGraph();
			const gpfocus::FocusGroup* activeGrp = activeGraph.GetActiveGroup();
			const gpfocus::FocusNode* activeNode = activeGraph.GetFocusedNode();

			if (m_pModalTitleText && topLayer)
			{
				string256 titleBuf;
				xr_sprintf(titleBuf, sizeof(titleBuf),
				           "Modal L%zu [%s]: %s",
				           depth, topLayer->tag.c_str(), topLayer->title.c_str());
				m_pModalTitleText->SetTextColor(color_argb(255, 255, 220, 110));
				m_pModalTitleText->SetText(titleBuf);
			}
			if (m_pModalSubText && topLayer)
			{
				string256 subBuf;
				xr_sprintf(subBuf, sizeof(subBuf),
				           "Token=%u | CallerNode=%u (%s) | B closes ONLY L%zu -> L%zu",
				           topLayer->pause_token_id,
				           topLayer->caller_focus.node_id,
				           topLayer->caller_focus.key.section.c_str(),
				           depth,
				           depth - 1);
				m_pModalSubText->SetText(subBuf);
			}

			for (int i = 0; i < MODAL_MAX_ROWS; ++i)
			{
				CUITextWnd* mrow = m_modalRows[i];
				if (!mrow)
					continue;
				if (!activeGrp || i >= static_cast<int>(activeGrp->nodes.size()))
				{
					mrow->SetText("");
					continue;
				}
				const gpfocus::FocusNode& opt = activeGrp->nodes[static_cast<size_t>(i)];
				const bool isOptFocused = (activeNode && activeNode->node_id == opt.node_id);
				mrow->SetTextColor(isOptFocused ? activeColor : inactiveColor);

				string256 optBuf;
				xr_sprintf(optBuf, sizeof(optBuf),
				           "%s %s",
				           isOptFocused ? "[ > ]" : "     ",
				           opt.label.c_str());
				mrow->SetText(optBuf);
			}
		}
	}

private:
	typedef CUIDialogWnd inherited;

	void RefreshWidgetsShowcaseVisual(u32 now, u32 dtContinual)
	{
		auto& ptm = CPauseTokenManager::Instance();
		auto& wctrl = ptm.GetWidgetShowcaseController();
		const auto& tabStrip = wctrl.GetTabStrip();
		const auto& catalog = wctrl.GetItemCatalog();
		const auto& card = wctrl.GetComparisonCard();
		const auto& wstack = wctrl.GetModalStack();
		const auto& footer = wctrl.GetFooterGlyphBar();
		const size_t wdepth = wstack.GetDepth();

		const float phase = static_cast<float>(now % 1000u) / 1000.0f;
		const float pulse = 0.5f + 0.5f * _sin(phase * PI_MUL_2);
		const u32 activeGreen = static_cast<u32>(185.0f + 70.0f * pulse);
		const u32 activeColor = color_argb(255, 255, activeGreen, 85);
		const u32 callerColor = color_argb(255, 120, 210, 255);
		const u32 inactiveColor = color_argb(215, 175, 190, 210);
		const u32 dimmedColor = color_argb(145, 110, 120, 135);
		const u32 statPosColor = color_argb(255, 130, 250, 150);
		const u32 statNegColor = color_argb(255, 255, 135, 135);

		if (m_pHeaderText)
		{
			m_pHeaderText->SetText(tabStrip.FormatTopBarLine().c_str());
		}
		if (m_pStatusText)
		{
			const std::string catLine = tabStrip.FormatCategoryFilterLine(
			    catalog.GetViewMode(), catalog.GetFilteredItems().size());
			m_pStatusText->SetText(catLine.c_str());
		}
		if (m_pControlsHintText)
		{
			string512 hintBuf;
			xr_sprintf(hintBuf, sizeof(hintBuf),
			           "F2.1 Витрина виджетов | dwTimeContinual=%u ms (dt=%u ms) | Стек=%zu | Мутаций=%u (Read-Only) | [F2]: режим F1.2",
			           now, dtContinual, wdepth, wctrl.GetTotalInventoryMutations());
			m_pControlsHintText->SetText(hintBuf);
		}
		if (m_pFooterGlyphText)
		{
			m_pFooterGlyphText->SetText(footer.FormatFooterLine().c_str());
		}

		const auto& filtered = catalog.GetFilteredItems();
		const float subRowPx = (catalog.GetViewMode() == gpwidgets::ItemViewMode::List1D)
		                     ? catalog.GetScroller().GetSubRowOffsetPx()
		                     : 0.0f;

		if (m_pListHeaderText)
		{
			string256 listHdr;
			xr_sprintf(listHdr, sizeof(listHdr),
			           "Каталог D10 [%s]: %zu предм. (категория: %s, top=%d)",
			           gpwidgets::ViewModeToLabelRU(catalog.GetViewMode()),
			           filtered.size(),
			           gpwidgets::CategoryFilterToLabelRU(tabStrip.GetActiveCategoryFilter()),
			           catalog.GetScroller().GetCurrentTopIndex());
			m_pListHeaderText->SetText(listHdr);
		}

		const int32_t focusedItemIdx = catalog.GetFocusedItemIndex(wctrl.GetRootGraph());
		const int32_t topIdx = catalog.GetScroller().GetCurrentTopIndex();
		for (int row = 0; row < VISIBLE_ROWS; ++row)
		{
			CUITextWnd* wnd = m_listRows[row];
			if (!wnd)
				continue;

			wnd->SetWndPos(Fvector2().set(80.0f, 168.0f + 34.0f + row * 50.0f - subRowPx));
			if (filtered.empty())
			{
				if (row == 0)
				{
					wnd->SetTextColor(activeColor);
					wnd->SetText("[ > <Категория пуста — выберите другую категорию LT/RT> < ]");
				}
				else
				{
					wnd->SetTextColor(dimmedColor);
					wnd->SetText("");
				}
				continue;
			}

			if (catalog.GetViewMode() == gpwidgets::ItemViewMode::List1D)
			{
				const size_t itemIdx = static_cast<size_t>(std::max<int32_t>(0, topIdx) + row);
				if (itemIdx < filtered.size())
				{
					const bool isFoc = (static_cast<int32_t>(itemIdx) == focusedItemIdx);
					const std::string rline = catalog.FormatListRow(itemIdx, isFoc);
					if (wdepth <= 1)
						wnd->SetTextColor(isFoc ? activeColor : inactiveColor);
					else
						wnd->SetTextColor(isFoc ? callerColor : dimmedColor);
					wnd->SetText(rline.c_str());
				}
				else
				{
					wnd->SetTextColor(dimmedColor);
					wnd->SetText("");
				}
			}
			else
			{
				// В режиме Сетки 4xN формируем строку из 4 карточек сетки
				const size_t baseIdx = static_cast<size_t>((std::max<int32_t>(0, topIdx) + row) * gpwidgets::ItemListGridWidget::kGridColumns);
				if (baseIdx < filtered.size())
				{
					std::string gridRowLine;
					bool rowHasFocus = false;
					for (int col = 0; col < gpwidgets::ItemListGridWidget::kGridColumns; ++col)
					{
						const size_t cellIdx = baseIdx + static_cast<size_t>(col);
						if (cellIdx >= filtered.size())
							break;
						const bool isFoc = (static_cast<int32_t>(cellIdx) == focusedItemIdx);
						if (isFoc)
							rowHasFocus = true;
						if (!gridRowLine.empty())
							gridRowLine += "  ";
						gridRowLine += catalog.FormatGridCard(cellIdx, isFoc);
					}
					if (wdepth <= 1)
						wnd->SetTextColor(rowHasFocus ? activeColor : inactiveColor);
					else
						wnd->SetTextColor(rowHasFocus ? callerColor : dimmedColor);
					wnd->SetText(gridRowLine.c_str());
				}
				else
				{
					wnd->SetTextColor(dimmedColor);
					wnd->SetText("");
				}
			}
		}

		// Правая колонка: модальное окно (QuantityModal / ConfirmDialog) ИЛИ карточка сравнения с Accurate Defense Values
		if (wdepth >= 2 && wctrl.GetQuantityModal().IsOpen(wstack))
		{
			const auto& qm = wctrl.GetQuantityModal();
			const gpfocus::FocusNode* activeNode = wctrl.GetActiveGraph().GetFocusedNode();

			if (m_pModalTitleText)
			{
				m_pModalTitleText->SetTextColor(color_argb(255, 255, 225, 110));
				m_pModalTitleText->SetText("Модальный выбор количества (QuantityModalWidget)");
			}
			if (m_pModalSubText)
			{
				m_pModalSubText->SetText(qm.FormatModalSummaryLine().c_str());
			}

			for (int i = 0; i < MODAL_MAX_ROWS; ++i)
			{
				if (!m_modalRows[i])
					continue;
				m_modalRows[i]->SetText("");
				m_modalRows[i]->SetTextColor(inactiveColor);
			}

			if (m_modalRows[0])
			{
				string256 buf;
				xr_sprintf(buf, sizeof(buf), "Предмет: %s (%s)", qm.GetItem().display_name.c_str(), qm.GetItem().section.c_str());
				m_modalRows[0]->SetText(buf);
			}
			if (m_modalRows[1])
			{
				string256 buf;
				xr_sprintf(buf, sizeof(buf), "Суммарный вес: %.2f кг | Сумма: %u RU", qm.GetTotalWeightKg(), qm.GetTotalCostRu());
				m_modalRows[1]->SetTextColor(statPosColor);
				m_modalRows[1]->SetText(buf);
			}
			const gpfocus::FocusGroup* qgrp = wctrl.GetActiveGraph().GetActiveGroup();
			if (qgrp)
			{
				for (size_t idx = 0; idx < qgrp->nodes.size() && (idx + 3) < static_cast<size_t>(MODAL_MAX_ROWS); ++idx)
				{
					const auto& n = qgrp->nodes[idx];
					const bool isFoc = (activeNode && activeNode->node_id == n.node_id);
					CUITextWnd* mrow = m_modalRows[idx + 3];
					if (mrow)
					{
						mrow->SetTextColor(isFoc ? activeColor : inactiveColor);
						string256 optBuf;
						xr_sprintf(optBuf, sizeof(optBuf), "%s %s", isFoc ? "[ > ]" : "     ", n.label.c_str());
						mrow->SetText(optBuf);
					}
				}
			}
		}
		else if (wdepth >= 2 && wctrl.GetConfirmDialog().IsOpen(wstack))
		{
			const auto& cd = wctrl.GetConfirmDialog();
			const gpfocus::FocusNode* activeNode = wctrl.GetActiveGraph().GetFocusedNode();

			if (m_pModalTitleText)
			{
				m_pModalTitleText->SetTextColor(statNegColor);
				m_pModalTitleText->SetText(cd.GetTitle().c_str());
			}
			if (m_pModalSubText)
			{
				m_pModalSubText->SetText(cd.FormatDialogLine(wstack).c_str());
			}

			for (int i = 0; i < MODAL_MAX_ROWS; ++i)
			{
				if (!m_modalRows[i])
					continue;
				m_modalRows[i]->SetText("");
				m_modalRows[i]->SetTextColor(inactiveColor);
			}

			if (m_modalRows[0])
			{
				m_modalRows[0]->SetTextColor(statNegColor);
				m_modalRows[0]->SetText(cd.GetWarningMessage().c_str());
			}
			const gpfocus::FocusGroup* cgrp = wctrl.GetActiveGraph().GetActiveGroup();
			if (cgrp)
			{
				for (size_t idx = 0; idx < cgrp->nodes.size() && (idx + 2) < static_cast<size_t>(MODAL_MAX_ROWS); ++idx)
				{
					const auto& n = cgrp->nodes[idx];
					const bool isFoc = (activeNode && activeNode->node_id == n.node_id);
					CUITextWnd* mrow = m_modalRows[idx + 2];
					if (mrow)
					{
						mrow->SetTextColor(isFoc ? activeColor : inactiveColor);
						string256 optBuf;
						xr_sprintf(optBuf, sizeof(optBuf), "%s %s", isFoc ? "[ > ]" : "     ", n.label.c_str());
						mrow->SetText(optBuf);
					}
				}
			}
		}
		else
		{
			// Карточка сравнения с характеристиками Accurate Defense Values (Read-Only)
			const std::vector<std::string>& cardLines = card.GetFormattedLines();
			if (m_pModalTitleText)
			{
				m_pModalTitleText->SetTextColor(color_argb(255, 255, 220, 120));
				m_pModalTitleText->SetText(cardLines.size() > 0 ? cardLines[0].c_str() : "Карточка сравнения (Accurate Defense Values)");
			}
			if (m_pModalSubText)
			{
				m_pModalSubText->SetTextColor(color_argb(255, 185, 215, 245));
				m_pModalSubText->SetText(cardLines.size() > 1 ? cardLines[1].c_str() : "");
			}

			const int scrollOff = std::max<int32_t>(0, card.GetScroller().GetCurrentTopIndex());
			for (int i = 0; i < MODAL_MAX_ROWS; ++i)
			{
				CUITextWnd* mrow = m_modalRows[i];
				if (!mrow)
					continue;
				const size_t lineIdx = static_cast<size_t>(2 + scrollOff + i);
				if (lineIdx < cardLines.size())
				{
					const std::string& sline = cardLines[lineIdx];
					if (sline.find("(+") != std::string::npos)
						mrow->SetTextColor(statPosColor);
					else if (sline.find("(-") != std::string::npos)
						mrow->SetTextColor(statNegColor);
					else if (sline.find("ADV") != std::string::npos || sline.find("BR ") != std::string::npos)
						mrow->SetTextColor(color_argb(255, 150, 225, 255));
					else
						mrow->SetTextColor(inactiveColor);
					mrow->SetText(sline.c_str());
				}
				else
				{
					mrow->SetText("");
				}
			}
		}
	}

	void PollGamepadNavigationAndActions(u32 nowContinual, u32 dtContinual)
	{
		auto& ptm = CPauseTokenManager::Instance();
		auto& ctrl = ptm.GetModalTestController();
		auto& wctrl = ptm.GetWidgetShowcaseController();
		auto& ctxDisp = CGamepadContextDispatcher::Instance();
		ctxDisp.OnFrame();

		if (!ctxDisp.AllowsGamepadButtons())
			return;

		if (ptm.IsFocusTestWidgetsMode())
		{
			bool holdLeft = false;
			bool holdRight = false;
			float rightStickY = 0.0f;
			bool ltPressedNow = false;
			bool rtPressedNow = false;

			if (pInput)
			{
				const auto& gp = pInput->GetGamepadState();
				if (gp.connected && gp.has_focus && !gp.need_release_all)
				{
					const float rawRy = static_cast<float>(gp.axes[3]) / 32767.0f;
					if (_abs(rawRy) > 0.20f)
						rightStickY = rawRy;

					const float rawLx = static_cast<float>(gp.axes[0]) / 32767.0f;
					holdLeft = ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_LEFT)) != 0) || (rawLx < -0.5f);
					holdRight = ((gp.buttons_mask & (1u << CInput::GP_BTN_DPAD_RIGHT)) != 0) || (rawLx > 0.5f);
					ltPressedNow = (gp.axes[4] > 16000);
					rtPressedNow = (gp.axes[5] > 16000);
				}
			}

			// Автоповтор ползунка QuantityModal по Device.dwTimeContinual при удержании Влево/Вправо
			if (wctrl.GetModalStack().GetDepth() >= 2 && wctrl.GetQuantityModal().IsOpen(wctrl.GetModalStack()))
			{
				if (holdRight)
					wctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Right, true, nowContinual);
				else if (holdLeft)
					wctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Left, true, nowContinual);
				else
					wctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Up, false, nowContinual);
			}
			else
			{
				wctrl.GetItemCatalogMutable().TickSmoothScroll(nowContinual);
				if (_abs(rightStickY) > 0.35f)
				{
					wctrl.GetComparisonCardMutable().ScrollByStick(rightStickY, dtContinual, nowContinual);
				}
			}

			CGamepadContextDispatcher::ENavDir dispDir = CGamepadContextDispatcher::ENavDir::None;
			bool isRepeat = false;
			LPCSTR dirSource = "none";
			if (ctxDisp.IsNavDirTriggeredThisFrame(&dispDir, &isRepeat, &dirSource))
			{
				switch (dispDir)
				{
				case CGamepadContextDispatcher::ENavDir::Up:    StepNav(gpfocus::NavDir::Up,    dirSource, isRepeat, dtContinual); break;
				case CGamepadContextDispatcher::ENavDir::Down:  StepNav(gpfocus::NavDir::Down,  dirSource, isRepeat, dtContinual); break;
				case CGamepadContextDispatcher::ENavDir::Left:  StepNav(gpfocus::NavDir::Left,  dirSource, isRepeat, dtContinual); break;
				case CGamepadContextDispatcher::ENavDir::Right: StepNav(gpfocus::NavDir::Right, dirSource, isRepeat, dtContinual); break;
				default: break;
				}
			}

			// LB / RB: верхние вкладки (или ±10 в QuantityModal)
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_LEFT_SHOULDER))
			{
				wctrl.StepTopTab(-1, nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_RIGHT_SHOULDER))
			{
				wctrl.StepTopTab(+1, nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}

			// LT / RT: фильтр категорий (или Мин/Макс в QuantityModal)
			if (ltPressedNow && !m_bPrevLtPressed)
			{
				wctrl.StepCategoryFilter(-1, nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			if (rtPressedNow && !m_bPrevRtPressed)
			{
				wctrl.StepCategoryFilter(+1, nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			m_bPrevLtPressed = ltPressedNow;
			m_bPrevRtPressed = rtPressedNow;

			// Кнопка A (GP_BTN_SOUTH): открыть QuantityModal / ConfirmDialog или подтвердить в модалке
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_SOUTH))
			{
				ActivateFocusedElement("gamepad_A");
			}
			// Кнопка X (GP_BTN_WEST): открыть диалог подтверждения опасного действия (по умолчанию фокус на Отмена D24)
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_WEST) && wctrl.GetModalStack().GetDepth() <= 1)
			{
				const u32 tok = ptm.Acquire("gp_widget_confirm");
				if (!wctrl.OpenConfirmModalForFocused(tok, gpmodal::ModalButtonMask(gpmodal::ModalButton::X), nowContinual))
					ptm.Release(tok);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			// Кнопка Y (GP_BTN_NORTH): переключить вид Список 1D <-> Сетка 4xN (D10)
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_NORTH))
			{
				wctrl.ToggleListGridView(nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			// Нажатие R3 (GP_BTN_RIGHT_STICK): переключить слот сравнения оружия (Слот 1 <-> Слот 2)
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_RIGHT_STICK))
			{
				wctrl.ToggleCompareSlot(nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			// Нажатие L3 (GP_BTN_LEFT_STICK): переключить предпросмотр Accurate Defense Values 100% состояния
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_LEFT_STICK))
			{
				wctrl.ToggleFullStats100(nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			// Кнопка View (GP_BTN_BACK): переключить стиль глифов D5.1 (Xbox / PlayStation / Nintendo / Generic)
			if (ctxDisp.IsButtonPressed(CInput::GP_BTN_BACK))
			{
				wctrl.CycleGlyphStyle(nowContinual);
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			return;
		}

		// Плавная прокрутка списка правым стиком по Device.dwTimeContinual (на уровне 1 в режиме F1.2)
		float rightStickY = 0.0f;
		if (pInput && ctrl.GetModalStack().GetDepth() == 1)
		{
			const auto& gp = pInput->GetGamepadState();
			if (gp.connected && gp.has_focus && !gp.need_release_all)
			{
				const float rawRy = static_cast<float>(gp.axes[3]) / 32767.0f;
				if (_abs(rawRy) > 0.20f)
					rightStickY = rawRy;
			}
		}
		ctrl.StepContinualScroll(nowContinual, rightStickY, dtContinual);

		CGamepadContextDispatcher::ENavDir dispDir = CGamepadContextDispatcher::ENavDir::None;
		bool isRepeat = false;
		LPCSTR dirSource = "none";
		if (ctxDisp.IsNavDirTriggeredThisFrame(&dispDir, &isRepeat, &dirSource))
		{
			switch (dispDir)
			{
			case CGamepadContextDispatcher::ENavDir::Up:    StepNav(gpfocus::NavDir::Up,    dirSource, isRepeat, dtContinual); break;
			case CGamepadContextDispatcher::ENavDir::Down:  StepNav(gpfocus::NavDir::Down,  dirSource, isRepeat, dtContinual); break;
			case CGamepadContextDispatcher::ENavDir::Left:  StepNav(gpfocus::NavDir::Left,  dirSource, isRepeat, dtContinual); break;
			case CGamepadContextDispatcher::ENavDir::Right: StepNav(gpfocus::NavDir::Right, dirSource, isRepeat, dtContinual); break;
			default: break;
			}
		}

		if (ctxDisp.IsButtonPressed(CInput::GP_BTN_SOUTH))
		{
			ActivateFocusedElement("gamepad_A");
		}
		if (ctxDisp.IsButtonPressed(CInput::GP_BTN_WEST))
		{
			ptm.DeleteFocusTestItem("focused");
		}
		if (ctxDisp.IsButtonPressed(CInput::GP_BTN_NORTH))
		{
			ptm.ExecuteFocusTestBridgeOp("gamepad_Y");
		}
		if (ctxDisp.IsButtonPressed(CInput::GP_BTN_BACK))
		{
			ptm.GetFocusGraph().SwitchGroupNext(nowContinual);
			ctrl.GetActiveGraphMutable().SwitchGroupNext(nowContinual);
		}
	}

	void StepNav(gpfocus::NavDir dir, LPCSTR source, bool isRepeat, u32 dtContinual)
	{
		const u32 now = Device.dwTimeContinual;
		auto& ptm = CPauseTokenManager::Instance();
		if (ptm.IsFocusTestWidgetsMode())
		{
			auto& wctrl = ptm.GetWidgetShowcaseController();
			wctrl.MoveFocus(dir, isRepeat, now);
			RefreshVisual(dtContinual);
			return;
		}

		auto& ctrl = ptm.GetModalTestController();
		ptm.GetFocusGraph().MoveFocus(dir, now);
		ctrl.StepNavigation(dir, now);

		const gpfocus::FocusNode* focused = ctrl.GetActiveGraph().GetFocusedNode();
		const int32_t focusedIdx = focused ? focused->key.list_index : -1;
		const size_t depth = ctrl.GetModalStack().GetDepth();

		Msg("* [Gamepad] gp_focus_move: depth=%zu idx=%d node=%u src=%s repeat=%d top_scroll=%d dwTimeContinual=%u dt_continual=%u fTimeDelta=%.4f",
		    depth,
		    focusedIdx,
		    focused ? focused->node_id : 0u,
		    source ? source : "nav",
		    isRepeat ? 1 : 0,
		    ctrl.GetScroller().GetTargetTopIndex(),
		    now,
		    dtContinual,
		    Device.fTimeDelta);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line),
			           "gp_focus_move,%u,depth=%zu,idx=%d,node=%u,src=%s,repeat=%d,top_scroll=%d,dt_continual_ms=%u,fTimeDelta=%.4f",
			           now,
			           depth,
			           focusedIdx,
			           focused ? focused->node_id : 0u,
			           source ? source : "nav",
			           isRepeat ? 1 : 0,
			           ctrl.GetScroller().GetTargetTopIndex(),
			           dtContinual,
			           Device.fTimeDelta);
			pInput->WriteGamepadTelemetryLine(line);
		}
	}

	void ActivateFocusedElement(LPCSTR source)
	{
		auto& ptm = CPauseTokenManager::Instance();
		if (ptm.IsFocusTestWidgetsMode())
		{
			const u32 now = Device.dwTimeContinual;
			auto& wctrl = ptm.GetWidgetShowcaseController();
			const size_t wdepth = wctrl.GetModalStack().GetDepth();
			if (wdepth <= 1)
			{
				const gpwidgets::WidgetItemSnapshot* focused = wctrl.GetItemCatalog().GetFocusedItem(wctrl.GetRootGraph());
				if (!focused)
					return;
				if (focused->stack_count > 1)
				{
					const u32 tok = ptm.Acquire("gp_widget_qty");
					if (!wctrl.OpenQuantityModalForFocused(tok, gpmodal::ModalButtonMask(gpmodal::ModalButton::A), now))
						ptm.Release(tok);
				}
				else
				{
					const u32 tok = ptm.Acquire("gp_widget_confirm");
					if (!wctrl.OpenConfirmModalForFocused(tok, gpmodal::ModalButtonMask(gpmodal::ModalButton::A), now))
						ptm.Release(tok);
				}
				SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
			}
			else
			{
				std::vector<uint32_t> releasedToks;
				if (wctrl.StepModalPressA(gpmodal::ModalButtonMask(gpmodal::ModalButton::A), Device.dwFrame, now, &releasedToks))
				{
					for (uint32_t rTok : releasedToks)
					{
						if (rTok != 0)
							ptm.Release(rTok);
					}
					CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
					SetBridgeStatusMessage(wctrl.GetLastStatusMessage().c_str(), true);
				}
			}
			return;
		}

		auto& ctrl = ptm.GetModalTestController();
		const size_t depth = ctrl.GetModalStack().GetDepth();

		if (depth == 1)
		{
			// L1 -> открываем L2 («Действия с предметом») поверх выбранного элемента списка
			ptm.OpenFocusTestModalToDepth(2);
			string256 msg;
			xr_sprintf(msg, sizeof(msg), "Opened Modal L2 via %s (press A to open L3 confirm, or B to close L2 -> L1)", source ? source : "A");
			SetBridgeStatusMessage(msg, true);
		}
		else if (depth == 2)
		{
			const gpfocus::FocusNode* opt = ctrl.GetActiveGraph().GetFocusedNode();
			const int32_t optIdx = opt ? opt->key.list_index : 0;
			if (optIdx == 1)
			{
				// Пункт 2 в L2: «Выполнить операцию моста (Slot <-> Ruck)»
				ptm.ExecuteFocusTestBridgeOp("l2_bridge_option");
			}
			else if (optIdx == 2)
			{
				// Пункт 3 в L2: «Удалить выбранный предмет сразу» -> закрываем L2 в корень L1 и удаляем предмет
				ptm.ConfirmFocusTestModalToRoot("l2_delete_option");
				ptm.DeleteFocusTestItem("focused");
			}
			else if (optIdx == 3)
			{
				// Пункт 4 в L2: «Отмена (или кнопка B)» -> закрывает строго только верхний уровень L2
				ptm.CancelFocusTestTopModal("l2_close_option");
				SetBridgeStatusMessage("Closed Modal L2 via menu option -> focus restored to L1 item", true);
			}
			else
			{
				// Пункт 1 в L2: открываем L3 («Диалог подтверждения») поверх выбранного пункта L2
				ptm.OpenFocusTestModalToDepth(3);
				string256 msg;
				xr_sprintf(msg, sizeof(msg), "Opened Modal L3 Confirmation via %s (B closes ONLY L3 -> L2)", source ? source : "A");
				SetBridgeStatusMessage(msg, true);
			}
		}
		else if (depth >= 3)
		{
			const gpfocus::FocusNode* opt = ctrl.GetActiveGraph().GetFocusedNode();
			const int32_t optIdx = opt ? opt->key.list_index : 0;
			if (optIdx == 0)
			{
				// «Да, подтвердить удаление предмета» -> контролируемо сворачиваем стек до L1 за один шаг и удаляем предмет на L1
				ptm.ConfirmFocusTestModalToRoot("l3_confirm_delete");
				ptm.DeleteFocusTestItem("focused");
			}
			else
			{
				// «Отмена» в L3 -> закрываем строго только L3 и возвращаемся в L2
				ptm.CancelFocusTestTopModal("l3_cancel_option");
				SetBridgeStatusMessage("Cancelled Modal L3 -> returned to L2 calling action node", true);
			}
		}
	}

	u32 m_openTimeContinual = 0;
	u32 m_lastUpdateTimeContinual = 0;
	u32 m_lastCardScrollMs = 0;
	bool m_bPrevLtPressed = false;
	bool m_bPrevRtPressed = false;
	CUITextWnd* m_pHeaderText = nullptr;
	CUITextWnd* m_pStatusText = nullptr;
	CUITextWnd* m_pControlsHintText = nullptr;
	CUITextWnd* m_pListHeaderText = nullptr;
	CUITextWnd* m_pModalTitleText = nullptr;
	CUITextWnd* m_pModalSubText = nullptr;
	CUITextWnd* m_pBridgeResultText = nullptr;
	CUITextWnd* m_pFooterGlyphText = nullptr;
	CUITextWnd* m_listRows[VISIBLE_ROWS] = {};
	CUITextWnd* m_modalRows[MODAL_MAX_ROWS] = {};
};

void CPauseTokenManager::OpenFocusTestWindow()
{
	if (!g_pGameLevel || !CurrentGameUI())
	{
		Msg("! [Gamepad] gp_focus_test: cannot open window (no active level or GameUI)");
		return;
	}

	if (MainMenu() && MainMenu()->IsActive())
	{
		Msg("! [Gamepad] gp_focus_test: cannot open window while MainMenu is active");
		return;
	}

	if (!m_pFocusTestWnd)
	{
		m_pFocusTestWnd = xr_new<CUIGamepadFocusTest>();
	}

	if (!m_pFocusTestWnd->IsShown())
	{
		m_pFocusTestWnd->ShowDialog(true);
	}
}

void CPauseTokenManager::CloseFocusTestWindow()
{
	if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
	{
		m_pFocusTestWnd->HideDialog();
	}
}

void CPauseTokenManager::ToggleFocusTestWindow()
{
	if (IsFocusTestWindowOpen())
		CloseFocusTestWindow();
	else
		OpenFocusTestWindow();
}

bool CPauseTokenManager::IsFocusTestWindowOpen() const
{
	return m_pFocusTestWnd && m_pFocusTestWnd->IsShown();
}

void CPauseTokenManager::PopulateFocusTestItems(int count)
{
	const int clampedCount = clampr(count, 1, 100);
	const u32 now = Device.dwTimeContinual;

	// Если в инвентаре актора есть реальные предметы, собираем их в начало списка и дополняем синтетическими до clampedCount,
	// чтобы на любом сохранении в пакете F1.3 список гарантированно имел N элементов для проверки прокрутки и удаления.
	std::vector<gpfocus::FocusNode> items;
	if (!m_focusGraph.GetGroups().empty())
	{
		const auto& ruckNodes = m_focusGraph.GetGroups()[0].nodes;
		for (const auto& n : ruckNodes)
		{
			if (n.kind == gpfocus::FocusNodeKind::Item && static_cast<int>(items.size()) < clampedCount)
			{
				gpfocus::FocusNode copy = n;
				copy.node_id = static_cast<u32>(items.size() + 1);
				copy.key.list_index = static_cast<int32_t>(items.size());
				copy.key.cell_indx = static_cast<int32_t>(items.size());
				copy.key.grid_x = 0;
				copy.key.grid_y = static_cast<int16_t>(items.size());
				copy.key.grid_w = 1;
				copy.key.grid_h = 1;
				items.push_back(std::move(copy));
			}
		}
	}

	static const char* kSynthSections[] = {
		"medkit", "medkit_army", "medkit_scientic", "bandage", "antirad",
		"conserva", "kolbasa", "bread", "energy_drink", "water_flask",
		"ammo_5.45x39_fmj", "ammo_9x19_fmj", "grenade_rgd5", "detector_advanced", "guitar_a"
	};
	constexpr size_t kNumSynth = sizeof(kSynthSections) / sizeof(kSynthSections[0]);

	while (static_cast<int>(items.size()) < clampedCount)
	{
		const int idx = static_cast<int>(items.size());
		const char* sec = kSynthSections[static_cast<size_t>(idx) % kNumSynth];
		gpfocus::FocusNode n;
		n.node_id = static_cast<u32>(idx + 1);
		n.kind = gpfocus::FocusNodeKind::Item;
		n.stable_id.session_gen = m_sessionGen;
		n.stable_id.object_id = static_cast<u16>(5000 + idx);
		n.stable_id.instance_gen = static_cast<u64>(9000 + idx);
		n.key.section = sec;
		n.key.cond_permille = static_cast<u16>(1000 - (idx % 5) * 50);
		n.key.uses_or_ammo = static_cast<u16>(1 + (idx % 4));
		n.key.list_index = idx;
		n.key.cell_indx = idx;
		n.key.grid_x = 0;
		n.key.grid_y = static_cast<int16_t>(idx);
		n.key.grid_w = 1;
		n.key.grid_h = 1;
		n.key.bag_id = 1;
		n.key.tab_id = 0;
		n.label = std::string(sec) + " #" + std::to_string(idx + 1);
		items.push_back(std::move(n));
	}

	m_modalTestCtrl.SetItemsFromNodes(items, now);
	if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
	{
		string256 msg;
		xr_sprintf(msg, sizeof(msg), "Populated %d items in L1 scrollable list (visible_rows=%d)",
		           clampedCount, gpmodal::FocusModalTestController::kDefaultVisibleRows);
		m_pFocusTestWnd->SetBridgeStatusMessage(msg, true);
		m_pFocusTestWnd->RefreshVisual(0);
	}
	Msg("* [Gamepad] gp_focus_test populate: count=%d top_scroll=%d",
	    clampedCount, m_modalTestCtrl.GetScroller().GetTargetTopIndex());
}

bool CPauseTokenManager::OpenFocusTestModalToDepth(u32 targetDepth)
{
	const u32 clampedDepth = clampr<u32>(targetDepth, 1u, 3u);
	const u32 now = Device.dwTimeContinual;
	auto& disp = CGamepadContextDispatcher::Instance();
	const u32 heldMask = disp.GetRawDownMask();

	if (m_modalTestCtrl.GetItems().empty())
	{
		PopulateFocusTestItems(gpmodal::FocusModalTestController::kDefaultListCount);
	}

	if (m_modalTestCtrl.GetModalStack().GetDepth() > clampedDepth)
	{
		std::vector<uint32_t> poppedTokens;
		m_modalTestCtrl.GetModalStackMutable().PopToDepth(
		    clampedDepth,
		    m_modalTestCtrl.GetRootGraphMutable(),
		    heldMask,
		    0xFFFFFFFFu,
		    now,
		    "modal_set_depth",
		    &poppedTokens);
		for (uint32_t tok : poppedTokens)
		{
			if (tok != 0)
				Release(tok);
		}
	}

	while (m_modalTestCtrl.GetModalStack().GetDepth() < clampedDepth)
	{
		const size_t nextDepth = m_modalTestCtrl.GetModalStack().GetDepth() + 1;
		u32 tok = 0;
		std::vector<uint32_t> extraReleased;
		if (nextDepth == 1)
		{
			tok = Acquire("gp_focus_modal_L1");
			m_modalTestCtrl.OpenRootModal(tok, heldMask, now);
		}
		else if (nextDepth == 2)
		{
			tok = Acquire("gp_focus_modal_L2");
			m_modalTestCtrl.OpenLevel2ActionModal(tok, heldMask, now, &extraReleased);
		}
		else
		{
			tok = Acquire("gp_focus_modal_L3");
			m_modalTestCtrl.OpenLevel3ConfirmModal(tok, heldMask, now, &extraReleased);
		}
		for (uint32_t rTok : extraReleased)
		{
			if (rTok != 0)
				Release(rTok);
		}
	}

	const std::string ctxTag = m_modalTestCtrl.GetModalStack().GetTopDispatcherContextTag();
	disp.NotifyModalLayerTransition(ctxTag.c_str(), "modal_push");

	Msg("* [Gamepad] gp_focus_test modal push: depth=%zu ctx_tag=\"%s\" top_token=%u summary=\"%s\"",
	    m_modalTestCtrl.GetModalStack().GetDepth(),
	    ctxTag.c_str(),
	    TopTokenId(),
	    m_modalTestCtrl.GetLastOperationSummary().c_str());

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_modal_push,%u,depth=%zu,ctx_tag=%s,top_token=%u",
		           now,
		           m_modalTestCtrl.GetModalStack().GetDepth(),
		           ctxTag.c_str(),
		           TopTokenId());
		pInput->WriteGamepadTelemetryLine(line);
	}

	if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
	{
		m_pFocusTestWnd->RefreshVisual(0);
	}
	return true;
}

bool CPauseTokenManager::CancelFocusTestTopModal(LPCSTR reason)
{
	LPCSTR safeReason = (reason && reason[0]) ? reason : "gp_ui_cancel";
	const u32 now = Device.dwTimeContinual;
	auto& disp = CGamepadContextDispatcher::Instance();
	const u32 rawMask = disp.GetRawDownMask() | gpmodal::ModalButtonMask(gpmodal::ModalButton::B);

	if (m_bFocusTestWidgetsMode && m_widgetShowcaseCtrl.GetModalStack().GetDepth() >= 2)
	{
		const size_t wDepthBefore = m_widgetShowcaseCtrl.GetModalStack().GetDepth();
		uint32_t wReleasedTok = 0;
		if (m_widgetShowcaseCtrl.StepModalCancelB(rawMask, Device.dwFrame, now, &wReleasedTok))
		{
			if (wReleasedTok != 0)
				Release(wReleasedTok);
			disp.ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);

			const size_t wDepthAfter = m_widgetShowcaseCtrl.GetModalStack().GetDepth();
			Msg("* [Gamepad] gp_widgets modal cancel (%s): depth %zu -> %zu released_token=%u status=\"%s\"",
			    safeReason, wDepthBefore, wDepthAfter, wReleasedTok, m_widgetShowcaseCtrl.GetLastStatusMessage().c_str());

			if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
			{
				m_pFocusTestWnd->SetBridgeStatusMessage(m_widgetShowcaseCtrl.GetLastStatusMessage().c_str(), true);
				m_pFocusTestWnd->RefreshVisual(0);
			}
			return true;
		}
		return false;
	}

	auto& stack = m_modalTestCtrl.GetModalStackMutable();
	if (stack.IsEmpty())
		return false;

	const size_t depthBefore = stack.GetDepth();

	u32 releasedTok = 0;
	if (!m_modalTestCtrl.CancelTopModal(rawMask, Device.dwFrame, now, &releasedTok))
	{
		return false;
	}

	if (releasedTok != 0)
	{
		Release(releasedTok);
	}

	const size_t depthAfter = stack.GetDepth();
	const std::string newCtxTag = stack.GetTopDispatcherContextTag();
	disp.NotifyModalLayerTransition(newCtxTag.c_str(), safeReason);
	disp.ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);

	const gpfocus::FocusNode* restoredNode = (depthAfter > 0) ? m_modalTestCtrl.GetActiveGraph().GetFocusedNode() : nullptr;

	Msg("* [Gamepad] gp_focus_test modal cancel (%s): depth %zu -> %zu released_token=%u restored_node=%u (%s) ctx_tag=\"%s\"",
	    safeReason,
	    depthBefore,
	    depthAfter,
	    releasedTok,
	    restoredNode ? restoredNode->node_id : 0u,
	    restoredNode ? restoredNode->key.section.c_str() : "none",
	    newCtxTag.c_str());

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_modal_pop,%u,reason=%s,depth_before=%zu,depth_after=%zu,released_token=%u,restored_node=%u,ctx_tag=%s",
		           now,
		           safeReason,
		           depthBefore,
		           depthAfter,
		           releasedTok,
		           restoredNode ? restoredNode->node_id : 0u,
		           newCtxTag.empty() ? "none" : newCtxTag.c_str());
		pInput->WriteGamepadTelemetryLine(line);
	}

	if (depthAfter == 0)
	{
		if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
		{
			m_pFocusTestWnd->HideDialog();
		}
	}
	else if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
	{
		string256 msg;
		xr_sprintf(msg, sizeof(msg),
		           "Closed Modal L%zu -> returned to L%zu (restored caller node=%u [%s])",
		           depthBefore,
		           depthAfter,
		           restoredNode ? restoredNode->node_id : 0u,
		           restoredNode ? restoredNode->label.c_str() : "none");
		m_pFocusTestWnd->SetBridgeStatusMessage(msg, true);
		m_pFocusTestWnd->RefreshVisual(0);
	}

	return true;
}

bool CPauseTokenManager::ConfirmFocusTestModalToRoot(LPCSTR reason)
{
	LPCSTR safeReason = (reason && reason[0]) ? reason : "l3_confirm";
	auto& stack = m_modalTestCtrl.GetModalStackMutable();
	if (stack.GetDepth() <= 1)
		return false;

	const size_t depthBefore = stack.GetDepth();
	const u32 now = Device.dwTimeContinual;
	auto& disp = CGamepadContextDispatcher::Instance();
	const u32 rawMask = disp.GetRawDownMask() | gpmodal::ModalButtonMask(gpmodal::ModalButton::A);

	std::vector<uint32_t> releasedTokens;
	if (!m_modalTestCtrl.ConfirmAndCloseToRoot(rawMask, Device.dwFrame, now, &releasedTokens))
	{
		return false;
	}

	for (uint32_t tok : releasedTokens)
	{
		if (tok != 0)
			Release(tok);
	}

	const size_t depthAfter = stack.GetDepth();
	const std::string newCtxTag = stack.GetTopDispatcherContextTag();
	disp.NotifyModalLayerTransition(newCtxTag.c_str(), safeReason);
	disp.ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);

	const gpfocus::FocusNode* restoredNode = m_modalTestCtrl.GetRootGraph().GetFocusedNode();

	Msg("* [Gamepad] gp_focus_test modal confirm (%s): depth %zu -> %zu released_tokens=%zu restored_node=%u (%s) ctx_tag=\"%s\"",
	    safeReason,
	    depthBefore,
	    depthAfter,
	    releasedTokens.size(),
	    restoredNode ? restoredNode->node_id : 0u,
	    restoredNode ? restoredNode->key.section.c_str() : "none",
	    newCtxTag.c_str());

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_modal_confirm,%u,reason=%s,depth_before=%zu,depth_after=%zu,released_tokens=%zu,restored_node=%u,ctx_tag=%s",
		           now,
		           safeReason,
		           depthBefore,
		           depthAfter,
		           releasedTokens.size(),
		           restoredNode ? restoredNode->node_id : 0u,
		           newCtxTag.empty() ? "none" : newCtxTag.c_str());
		pInput->WriteGamepadTelemetryLine(line);
	}

	if (m_pFocusTestWnd && m_pFocusTestWnd->IsShown())
	{
		m_pFocusTestWnd->RefreshVisual(0);
	}
	return true;
}

u32 CPauseTokenManager::EmergencyCloseFocusTestModalStack(LPCSTR reason)
{
	LPCSTR safeReason = (reason && reason[0]) ? reason : "emergency_reset";
	auto& disp = CGamepadContextDispatcher::Instance();
	u32 totalClosed = 0;

	if (m_widgetShowcaseCtrl.GetModalStack().GetDepth() >= 2)
	{
		std::vector<uint32_t> wReleased;
		m_widgetShowcaseCtrl.GetModalStackMutable().PopToDepth(
		    1u,
		    m_widgetShowcaseCtrl.GetRootGraphMutable(),
		    disp.GetRawDownMask(),
		    0xFFFFFFFFu,
		    Device.dwTimeContinual,
		    safeReason,
		    &wReleased);
		for (uint32_t tok : wReleased)
		{
			if (tok != 0)
				Release(tok);
		}
		totalClosed += static_cast<u32>(wReleased.size());
	}

	if (m_modalTestCtrl.GetModalStack().IsEmpty())
		return totalClosed;

	std::vector<uint32_t> releasedTokens;
	const size_t closedCount = m_modalTestCtrl.EmergencyCloseAll(safeReason, disp.GetRawDownMask(), Device.dwTimeContinual, &releasedTokens);
	for (uint32_t tok : releasedTokens)
	{
		if (tok != 0)
			Release(tok);
	}
	totalClosed += static_cast<u32>(closedCount);

	disp.NotifyModalLayerTransition("", safeReason);

	Msg("* [Gamepad] gp_focus_test emergency modal clear (%s): closed_layers=%u released_tokens=%zu remaining_tokens=%u",
	    safeReason, totalClosed, releasedTokens.size(), Count());

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_modal_emergency_clear,%u,reason=%s,closed_layers=%u,released_tokens=%zu,remaining_tokens=%u",
		           Device.dwTimeContinual,
		           safeReason,
		           totalClosed,
		           releasedTokens.size(),
		           Count());
		pInput->WriteGamepadTelemetryLine(line);
	}

	return totalClosed;
}

bool CPauseTokenManager::DeleteFocusTestItem(LPCSTR which)
{
	LPCSTR safeWhich = (which && which[0]) ? which : "focused";
	const u32 now = Device.dwTimeContinual;

	if (m_modalTestCtrl.GetItems().empty() && 0 != _stricmp(safeWhich, "single"))
	{
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage("Delete skipped: item list is already empty (press [5] to repopulate)", false);
		return false;
	}

	gpfocus::FocusRestoreReason reason = gpfocus::FocusRestoreReason::None;
	if (0 == _stricmp(safeWhich, "first") || 0 == _stricmp(safeWhich, "del_first"))
		reason = m_modalTestCtrl.DeleteFirstItem(now);
	else if (0 == _stricmp(safeWhich, "middle") || 0 == _stricmp(safeWhich, "mid") || 0 == _stricmp(safeWhich, "del_middle"))
		reason = m_modalTestCtrl.DeleteMiddleItem(now);
	else if (0 == _stricmp(safeWhich, "last") || 0 == _stricmp(safeWhich, "del_last"))
		reason = m_modalTestCtrl.DeleteLastItem(now);
	else if (0 == _stricmp(safeWhich, "single") || 0 == _stricmp(safeWhich, "del_single"))
		reason = m_modalTestCtrl.DeleteSingleItem(now);
	else
		reason = m_modalTestCtrl.DeleteFocusedItem(now);

	const gpfocus::FocusNode* focusedAfter = m_modalTestCtrl.GetRootGraph().GetFocusedNode();
	Msg("* [Gamepad] gp_focus_test delete (%s): restore_reason=%s remaining_items=%zu focused_node=%u (%s) top_scroll=%d summary=\"%s\"",
	    safeWhich,
	    gpfocus::FocusRestoreReasonToString(reason),
	    m_modalTestCtrl.GetItems().size(),
	    focusedAfter ? focusedAfter->node_id : 0u,
	    focusedAfter ? focusedAfter->label.c_str() : "none",
	    m_modalTestCtrl.GetScroller().GetTargetTopIndex(),
	    m_modalTestCtrl.GetLastOperationSummary().c_str());

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_focus_delete,%u,which=%s,restore=%s,remaining=%zu,focused_node=%u,top_scroll=%d",
		           now,
		           safeWhich,
		           gpfocus::FocusRestoreReasonToString(reason),
		           m_modalTestCtrl.GetItems().size(),
		           focusedAfter ? focusedAfter->node_id : 0u,
		           m_modalTestCtrl.GetScroller().GetTargetTopIndex());
		pInput->WriteGamepadTelemetryLine(line);
	}

	if (m_pFocusTestWnd)
	{
		m_pFocusTestWnd->SetBridgeStatusMessage(m_modalTestCtrl.GetLastOperationSummary().c_str(), true);
		if (m_pFocusTestWnd->IsShown())
			m_pFocusTestWnd->RefreshVisual(0);
	}
	return true;
}

bool CPauseTokenManager::RunFocusTestModalSelfTest()
{
	const gpfocus::SelfTestSummary modalSum = gpmodal::RunModalStackSelfTest();
	const gpfocus::SelfTestSummary widSum = gpwidgets::RunWidgetsSelfTest();
	Msg("* [Gamepad] %s", modalSum.report.c_str());
	Msg("* [Gamepad] %s", widSum.report.c_str());
	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_modal_selftest,%u,total=%u,passed=%u,failed=%u,widgets_total=%u,widgets_passed=%u,widgets_failed=%u",
		           Device.dwTimeContinual,
		           modalSum.total_cases,
		           modalSum.passed_cases,
		           modalSum.failed_cases,
		           widSum.total_cases,
		           widSum.passed_cases,
		           widSum.failed_cases);
		pInput->WriteGamepadTelemetryLine(line);
	}
	if (m_pFocusTestWnd)
	{
		const std::string combined = modalSum.report + " | " + widSum.report;
		m_pFocusTestWnd->SetBridgeStatusMessage(combined.c_str(), modalSum.failed_cases == 0 && widSum.failed_cases == 0);
	}
	return modalSum.failed_cases == 0 && widSum.failed_cases == 0;
}

void CPauseTokenManager::DumpFocusTestModalState()
{
	const std::string dump = m_bFocusTestWidgetsMode
	                       ? m_widgetShowcaseCtrl.FormatShowcaseDump()
	                       : m_modalTestCtrl.FormatStatusDump();
	std::istringstream iss(dump);
	std::string line;
	while (std::getline(iss, line))
	{
		if (!line.empty())
			Msg("* %s", line.c_str());
	}
}

void CPauseTokenManager::SetFocusTestWidgetsMode(bool enabled)
{
	if (m_bFocusTestWidgetsMode == enabled)
		return;
	m_bFocusTestWidgetsMode = enabled;
	if (m_bFocusTestWidgetsMode && m_widgetShowcaseCtrl.GetItemCatalog().GetAllItems().empty())
	{
		PopulateWidgetsShowcaseItems();
	}
	Msg("* [Gamepad] gp_focus_test mode switched to: %s", m_bFocusTestWidgetsMode ? "F2.1 Base Widgets Showcase" : "F1.2 Modal Stack Test");
	if (m_pFocusTestWnd)
	{
		m_pFocusTestWnd->SetBridgeStatusMessage(
		    m_bFocusTestWidgetsMode
		        ? "Mode: F2.1 Base Widgets Showcase (LB/RB tabs, LT/RT filters, Y: List/Grid 4xN, R3: Slot, L3: 100% ADV, A/X: Modals)"
		        : "Mode: F1.2 Focus & Modal Stack Test (A: L2/L3 modal, B: pop top modal, X/1/2/3/4: delete item)",
		    true);
		if (m_pFocusTestWnd->IsShown())
			m_pFocusTestWnd->RefreshVisual(0);
	}
}

void CPauseTokenManager::PopulateWidgetsShowcaseItems()
{
	const u32 now = Device.dwTimeContinual;
	m_widgetShowcaseCtrl.InitializeDefaultShowcase(now);

	if (g_pGameLevel && g_actor && g_actor->g_Alive())
	{
		const CInventory& inv = g_actor->inventory();
		m_widgetShowcaseCtrl.GetTabStripMutable().SetActorTelemetry(
		    inv.TotalWeight(), inv.GetMaxWeight(), g_actor->get_money(), OwnsTimer());

		// DEF-P0-03: берём актуальный набор глифов и раскладку кнопок напрямую из CGamepadGlyphManager (D5.1),
		// с опциональной сверкой gp_icons.get_effective_set() при наличии скрипта (без несуществующего gp_glyphs.*).
		auto& glyphMgr = CGamepadGlyphManager::Instance();
		glyphMgr.EnsureInitialized();

		gpwidgets::WidgetGlyphStyle st = gpwidgets::WidgetGlyphStyle::Xbox;
		gpwidgets::WidgetFaceStyle fc = gpwidgets::WidgetFaceStyle::ABXY;
		switch (glyphMgr.GetEffectiveGlyphSet())
		{
		case EGamepadGlyphSet::PlayStation:
			st = gpwidgets::WidgetGlyphStyle::PlayStation;
			fc = gpwidgets::WidgetFaceStyle::Sony;
			break;
		case EGamepadGlyphSet::Nintendo:
			st = gpwidgets::WidgetGlyphStyle::Nintendo;
			fc = gpwidgets::WidgetFaceStyle::BAYX;
			break;
		case EGamepadGlyphSet::Generic:
			st = gpwidgets::WidgetGlyphStyle::Generic;
			fc = gpwidgets::WidgetFaceStyle::Numbered;
			break;
		default:
			st = gpwidgets::WidgetGlyphStyle::Xbox;
			fc = gpwidgets::WidgetFaceStyle::ABXY;
			break;
		}
		switch (glyphMgr.GetEffectiveFaceStyle())
		{
		case EGamepadFaceStyle::Sony:     fc = gpwidgets::WidgetFaceStyle::Sony; break;
		case EGamepadFaceStyle::BAYX:     fc = gpwidgets::WidgetFaceStyle::BAYX; break;
		case EGamepadFaceStyle::Numbered: fc = gpwidgets::WidgetFaceStyle::Numbered; break;
		default:                          fc = gpwidgets::WidgetFaceStyle::ABXY; break;
		}

		if (g_ai_space)
		{
			::luabind::functor<LPCSTR> iconSetFn;
			if (ai().script_engine().functor("gp_icons.get_effective_set", iconSetFn))
			{
				try
				{
					LPCSTR styleStr = iconSetFn();
					if (styleStr && styleStr[0])
					{
						if (0 == _stricmp(styleStr, "ps") || 0 == _stricmp(styleStr, "ps4") ||
						    0 == _stricmp(styleStr, "dualsense") || 0 == _stricmp(styleStr, "playstation"))
						{
							st = gpwidgets::WidgetGlyphStyle::PlayStation;
							fc = gpwidgets::WidgetFaceStyle::Sony;
						}
						else if (0 == _stricmp(styleStr, "nintendo") || 0 == _stricmp(styleStr, "switch"))
						{
							st = gpwidgets::WidgetGlyphStyle::Nintendo;
							fc = gpwidgets::WidgetFaceStyle::BAYX;
						}
						else if (0 == _stricmp(styleStr, "generic"))
						{
							st = gpwidgets::WidgetGlyphStyle::Generic;
							fc = gpwidgets::WidgetFaceStyle::Numbered;
						}
						else if (0 == _stricmp(styleStr, "xbox") || 0 == _stricmp(styleStr, "steamdeck"))
						{
							st = gpwidgets::WidgetGlyphStyle::Xbox;
							fc = gpwidgets::WidgetFaceStyle::ABXY;
						}
					}
				}
				catch (...)
				{
				}
			}
		}
		m_widgetShowcaseCtrl.GetFooterGlyphBarMutable().SetGlyphStyle(st, fc, true);
	}

	m_widgetShowcaseCtrl.RefreshAllWidgets(now);
	Msg("* [Gamepad] gp_widgets populate: items=%zu view=%s category=%s mutations=%u (read-only)",
	    m_widgetShowcaseCtrl.GetItemCatalog().GetAllItems().size(),
	    gpwidgets::ViewModeToLabelRU(m_widgetShowcaseCtrl.GetItemCatalog().GetViewMode()),
	    gpwidgets::CategoryFilterToLabelRU(m_widgetShowcaseCtrl.GetTabStrip().GetActiveCategoryFilter()),
	    m_widgetShowcaseCtrl.GetTotalInventoryMutations());
}

bool CPauseTokenManager::RunWidgetsShowcaseSelfTest()
{
	const gpfocus::SelfTestSummary summary = gpwidgets::RunWidgetsSelfTest();
	const u32 mutations = m_widgetShowcaseCtrl.GetTotalInventoryMutations();
	Msg("* [Gamepad] %s", summary.report.c_str());
	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line),
		           "gp_widgets_selftest,%u,total=%u,passed=%u,failed=%u,mutations=%u",
		           Device.dwTimeContinual,
		           summary.total_cases,
		           summary.passed_cases,
		           summary.failed_cases,
		           mutations);
		pInput->WriteGamepadTelemetryLine(line);
	}
	if (m_pFocusTestWnd)
	{
		m_pFocusTestWnd->SetBridgeStatusMessage(summary.report.c_str(), summary.failed_cases == 0 && mutations == 0);
		if (m_pFocusTestWnd->IsShown())
			m_pFocusTestWnd->RefreshVisual(0);
	}
	return summary.failed_cases == 0 && mutations == 0;
}

void CPauseTokenManager::DumpWidgetsShowcaseState()
{
	if (m_widgetShowcaseCtrl.GetItemCatalog().GetAllItems().empty())
	{
		PopulateWidgetsShowcaseItems();
	}
	const std::string dump = m_widgetShowcaseCtrl.FormatShowcaseDump();
	std::istringstream iss(dump);
	std::string line;
	while (std::getline(iss, line))
	{
		if (!line.empty())
			Msg("* %s", line.c_str());
	}
}

// C5.1 п.2–4: вызов операции через gp_bridge.script -> ui_inventory.GUI с проверкой по StableItemID
// и замером времени от нажатия до обновления модели в кадрах (delta_frames)
bool CPauseTokenManager::ExecuteFocusTestBridgeOp(LPCSTR sourceTag)
{
	LPCSTR safeSource = (sourceTag && sourceTag[0]) ? sourceTag : "focus_test";
	const u32 pressFrame = Device.dwFrame;
	const u32 pressTimeContinual = Device.dwTimeContinual;

	if (!g_pGameLevel)
	{
		Msg("! [Gamepad] gp_focus_test op (%s) cancelled: no active level", safeSource);
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
	{
		Msg("! [Gamepad] gp_focus_test op (%s) cancelled: actor not available or dead", safeSource);
		return false;
	}

	CInventory& inv = pActor->inventory();

	// Подбираем подходящий предмет в инвентаре актора для перемещения между слотом и рюкзаком:
	// сначала проверяем слоты (нож/пистолет/винтовка/бинокль/болт/детектор/прочие слоты, которые можно убрать в рюкзак),
	// а если все слоты пусты или заблокированы — ищем предмет в рюкзаке, который помещается в свободный слот.
	CInventoryItem* targetItem = nullptr;
	bool moveSlotToRuck = false;
	u16 targetSlot = NO_ACTIVE_SLOT;

	// F4.1: если уже ожидается подтверждение GEG_PLAYER_ITEM2* для предыдущей операции моста,
	// повторное нажатие A блокируется диспетчером транзакций категории B до прихода события
	if (m_pendingBridgeOp.active && m_txnDispatcher.IsItemLocked(m_pendingBridgeOp.stableRef))
	{
		const gptxn::TxnBeginResult dupRes = m_txnDispatcher.BeginCategoryB(
			"ExecuteFocusTestBridgeOp:duplicate_A",
			m_pendingBridgeOp.stableRef,
			gptxn::ExpectedEventMask::ItemToSlot | gptxn::ExpectedEventMask::ItemToRuck,
			pressFrame,
			pressTimeContinual);
		Msg("! [Gamepad] gp_focus_test op (%s) blocked: duplicate A while item id=%u gen=%llu is Pending GEG_PLAYER_ITEM2* (dup_blocked=%u reason=%s)",
		    safeSource,
		    m_pendingBridgeOp.stableRef.object_id,
		    m_pendingBridgeOp.stableRef.instance_gen,
		    m_txnDispatcher.GetStats().duplicate_a_blocked,
		    dupRes.reason.c_str());
		if (m_pFocusTestWnd)
		{
			string256 dupMsg;
			xr_sprintf(dupMsg, sizeof(dupMsg),
			           "Bridge BLOCKED duplicate A: item id=%u (gen=%llu) awaiting GEG_PLAYER_ITEM2*",
			           m_pendingBridgeOp.stableRef.object_id,
			           m_pendingBridgeOp.stableRef.instance_gen);
			m_pFocusTestWnd->SetBridgeStatusMessage(dupMsg, false);
		}
		return false;
	}

	const u16 preferredSlots[] = { 1, 2, 3, 5, 6, 9, 4, 10, 7, 12, 13 };
	for (u16 s : preferredSlots)
	{
		if (s > inv.LastSlot())
			continue;
		CInventoryItem* itm = inv.ItemFromSlot(s);
		if (itm && !itm->GetDropManual() && inv.CanPutInRuck(itm))
		{
			targetItem = itm;
			moveSlotToRuck = true;
			targetSlot = s;
			break;
		}
	}

	if (!targetItem)
	{
		for (CInventoryItem* itm : inv.m_ruck)
		{
			if (!itm || itm->GetDropManual())
				continue;
			const u16 baseSlot = itm->BaseSlot();
			if (baseSlot != NO_ACTIVE_SLOT && baseSlot <= inv.LastSlot())
			{
				if (inv.CanPutInSlot(itm, baseSlot))
				{
					targetItem = itm;
					moveSlotToRuck = false;
					targetSlot = baseSlot;
					break;
				}
			}
		}
	}

	if (!targetItem)
	{
		Msg("! [Gamepad] gp_focus_test op (%s) cancelled: no suitable movable item found in actor slots or ruck", safeSource);
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage("Bridge op cancelled: no suitable item in slots/ruck", false);
		return false;
	}

	const u16 itemId = targetItem->object().ID();
	if (GetObjectInstanceGen(itemId) == 0)
	{
		// На случай объектов, созданных до инициализации или нестандартным путём
		OnObjectNetSpawn(itemId);
	}

	const StableItemID stableRef = MakeStableItemID(itemId);
	CGameObject* resolvedBefore = ResolveStableItem(stableRef);
	if (!resolvedBefore || resolvedBefore != &targetItem->object())
	{
		Msg("! [Gamepad] gp_focus_test op (%s) cancelled: StableItemID pre-check failed for id=%u gen=%llu",
		    safeSource, stableRef.object_id, stableRef.instance_gen);
		return false;
	}

	const shared_str itemSection = targetItem->object().cNameSect();
	LPCSTR actionDir = moveSlotToRuck ? "to_ruck" : "to_slot";
	const gptxn::ExpectedEventMask expectedEv = moveSlotToRuck
		? gptxn::ExpectedEventMask::ItemToRuck
		: gptxn::ExpectedEventMask::ItemToSlot;

	// F4.1: регистрируем транзакцию категории B и блокируем предмет (StableItemID) до прихода GEG_PLAYER_ITEM2*
	const gptxn::TxnBeginResult txnBegin = m_txnDispatcher.BeginCategoryB(
		moveSlotToRuck ? "Action_UnEquip:to_ruck" : "Action_Equip:to_slot",
		stableRef,
		expectedEv,
		pressFrame,
		pressTimeContinual,
		gptxn::TransactionDispatcher::kDefaultCategoryBTimeoutMs,
		!moveSlotToRuck);
	if (!txnBegin.accepted)
	{
		Msg("! [Gamepad] gp_focus_test op (%s) blocked by TxnDispatcher: id=%u gen=%llu reason=%s dup_blocked=%u",
		    safeSource,
		    stableRef.object_id,
		    stableRef.instance_gen,
		    txnBegin.reason.c_str(),
		    m_txnDispatcher.GetStats().duplicate_a_blocked);
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage(txnBegin.reason.c_str(), false);
		return false;
	}

	// Вызов Lua-моста gp_bridge.execute_focus_test_op через ai().script_engine().functor
	// (src/xrServerEntities/script_engine.h:154-155, script_engine_inline.h:57-103)
	::luabind::functor<LPCSTR> bridgeFn;
	if (!ai().script_engine().functor("gp_bridge.execute_focus_test_op", bridgeFn))
	{
		m_txnDispatcher.FailOrCancelTransaction(
			txnBegin.txn_id, gptxn::TxnState::Failed, Device.dwFrame, Device.dwTimeContinual, "err:functor_not_found");
		Msg("! [Gamepad] gp_focus_test op (%s) failed: functor gp_bridge.execute_focus_test_op not found", safeSource);
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage("Bridge error: gp_bridge.execute_focus_test_op not found", false);
		return false;
	}

	xr_string bridgeResultStr;
	try
	{
		LPCSTR res = bridgeFn(static_cast<int>(stableRef.object_id),
		                      itemSection.c_str(),
		                      actionDir,
		                      static_cast<int>(targetSlot));
		if (res)
			bridgeResultStr = res;
	}
	catch (...)
	{
		m_txnDispatcher.FailOrCancelTransaction(
			txnBegin.txn_id, gptxn::TxnState::Failed, Device.dwFrame, Device.dwTimeContinual, "err:lua_exception");
		Msg("! [Gamepad] gp_focus_test op (%s) exception during Lua call gp_bridge.execute_focus_test_op", safeSource);
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage("Bridge exception in gp_bridge.execute_focus_test_op", false);
		return false;
	}

	if (bridgeResultStr.empty() || 0 != strncmp(bridgeResultStr.c_str(), "ok:", 3))
	{
		m_txnDispatcher.FailOrCancelTransaction(
			txnBegin.txn_id,
			gptxn::TxnState::Failed,
			Device.dwFrame,
			Device.dwTimeContinual,
			bridgeResultStr.empty() ? "err:empty_lua_res" : bridgeResultStr.c_str());
		Msg("! [Gamepad] gp_focus_test op (%s) Lua bridge returned error: %s",
		    safeSource, bridgeResultStr.empty() ? "<empty>" : bridgeResultStr.c_str());
		if (m_pFocusTestWnd)
			m_pFocusTestWnd->SetBridgeStatusMessage(bridgeResultStr.c_str(), false);
		return false;
	}

	// Повторная проверка StableItemID сразу после возврата из Lua
	CGameObject* resolvedAfterLua = ResolveStableItem(stableRef);
	CInventoryItem* verifiedItem = resolvedAfterLua ? smart_cast<CInventoryItem*>(resolvedAfterLua) : nullptr;
	if (!verifiedItem || verifiedItem->GetDropManual() ||
	    !verifiedItem->object().H_Parent() || verifiedItem->object().H_Parent()->ID() != pActor->ID())
	{
		m_txnDispatcher.FailOrCancelTransaction(
			txnBegin.txn_id, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, "err:post_lua_stable_id_check");
		Msg("! [Gamepad] gp_focus_test op (%s) cancelled: StableItemID post-Lua check failed for id=%u gen=%llu",
		    safeSource, stableRef.object_id, stableRef.instance_gen);
		return false;
	}

	// F4.1 (D49, C5.1-N2 исправлено):
	// UIInventory:Action_UnEquip / Action_Equip внутри вызывает db.actor:move_to_ruck / move_to_slot,
	// которые ставят GEG_PLAYER_ITEM2RUCK / GEG_PLAYER_ITEM2SLOT в Level().game_events.
	// Синхронный вызов inv.Ruck / inv.Slot в этом же кадре УДАЛЁН, чтобы CInventory::Ruck / Slot
	// не вызывался дважды (первый раз синхронно и второй раз из CActor::OnEvent на следующем кадре).
	// Вместо этого транзакция остаётся в состоянии Pending (блокируя повторный A на том же StableItemID)
	// до прихода GEG_PLAYER_ITEM2RUCK / GEG_PLAYER_ITEM2SLOT в CActor::OnEvent -> OnActorInventoryEvent
	// на следующем кадре (delta_frames = +1 кадр, без CInventory::Update()).
	m_pendingBridgeOp.active = true;
	m_pendingBridgeOp.txnId = txnBegin.txn_id;
	m_pendingBridgeOp.pressFrame = pressFrame;
	m_pendingBridgeOp.pressTimeContinual = pressTimeContinual;
	m_pendingBridgeOp.stableRef = stableRef;
	m_pendingBridgeOp.moveSlotToRuck = moveSlotToRuck;
	m_pendingBridgeOp.targetSlot = targetSlot;
	m_pendingBridgeOp.sourceTag = safeSource;
	m_pendingBridgeOp.itemSection = itemSection;
	m_pendingBridgeOp.actionDir = actionDir;
	m_pendingBridgeOp.bridgeResultStr = bridgeResultStr.c_str();

	Msg("* [Gamepad] gp_focus_test bridge_op_pending (%s): txn_id=%u dir=%s id=%u gen=%llu sec=%s slot=%u press_frame=%u paused=%d lua_res=\"%s\" (awaiting GEG_PLAYER_ITEM2* on next frame, C5.1-N2 fixed)",
	    safeSource,
	    txnBegin.txn_id,
	    actionDir,
	    stableRef.object_id,
	    stableRef.instance_gen,
	    itemSection.c_str(),
	    targetSlot,
	    pressFrame,
	    Device.Paused() ? 1 : 0,
	    bridgeResultStr.c_str());

	if (m_pFocusTestWnd)
	{
		string512 uiMsg;
		xr_sprintf(uiMsg, sizeof(uiMsg),
		           "Bridge PENDING (txn=%u): %s id=%u (gen=%llu, %s) -> %s | awaiting GEG_PLAYER_ITEM2* (+1 frame)",
		           txnBegin.txn_id,
		           safeSource,
		           stableRef.object_id,
		           stableRef.instance_gen,
		           itemSection.c_str(),
		           actionDir);
		m_pFocusTestWnd->SetBridgeStatusMessage(uiMsg, true);
	}

	return true;
}

void CPauseTokenManager::OnActorInventoryEvent(u16 eventType, u16 itemId)
{
	const u32 nowFrame = Device.dwFrame;
	const u32 nowContinual = Device.dwTimeContinual;

	const u32 confirmedTxnId = m_txnDispatcher.OnInventoryEvent(eventType, itemId, nowFrame, nowContinual);
	if (confirmedTxnId != 0)
	{
		const gptxn::TxnRecord* rec = m_txnDispatcher.FindRecordById(confirmedTxnId);
		const u32 deltaFrames = rec ? rec->delta_frames : 0u;
		Msg("* [Gamepad] gp_txn confirmed: txn_id=%u op=\"%s\" ge_type=%u item_id=%u start_frame=%u done_frame=%u delta_frames=%u slot_no_inv_update=%d",
		    confirmedTxnId,
		    rec ? rec->op_name.c_str() : "",
		    static_cast<u32>(eventType),
		    static_cast<u32>(itemId),
		    rec ? rec->start_frame : nowFrame,
		    nowFrame,
		    deltaFrames,
		    (rec && rec->no_inv_update_slot) ? 1 : 0);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line),
			           "gp_txn_confirm,%u,txn=%u,ge=%u,item=%u,delta_frames=%u,no_inv_update=%d",
			           nowContinual,
			           confirmedTxnId,
			           static_cast<u32>(eventType),
			           static_cast<u32>(itemId),
			           deltaFrames,
			           (rec && rec->no_inv_update_slot) ? 1 : 0);
			pInput->WriteGamepadTelemetryLine(line);
		}
	}

	if (m_pendingBridgeOp.active && m_pendingBridgeOp.stableRef.object_id == itemId)
	{
		const gptxn::ExpectedEventMask evFlag = gptxn::ClassifyEngineEventType(eventType);
		const bool matchesPendingDir = m_pendingBridgeOp.moveSlotToRuck
			? (evFlag == gptxn::ExpectedEventMask::ItemToRuck)
			: (evFlag == gptxn::ExpectedEventMask::ItemToSlot);
		if (matchesPendingDir)
		{
			const PendingBridgeOp op = m_pendingBridgeOp;
			m_pendingBridgeOp = {};

			CActor* pActor = g_actor;
			CGameObject* resolvedObj = ResolveStableItem(op.stableRef);
			CInventoryItem* verifiedItem = resolvedObj ? smart_cast<CInventoryItem*>(resolvedObj) : nullptr;
			bool inExpectedContainer = false;
			if (pActor && verifiedItem && !verifiedItem->GetDropManual())
			{
				CInventory& inv = pActor->inventory();
				inExpectedContainer = op.moveSlotToRuck
					? inv.InRuck(verifiedItem)
					: (inv.InSlot(verifiedItem) && verifiedItem->CurrSlot() == op.targetSlot);
			}

			const u32 doneFrame = nowFrame;
			const u32 deltaFrames = (doneFrame >= op.pressFrame) ? (doneFrame - op.pressFrame) : 0u;
			const u32 deltaContinualMs = (nowContinual >= op.pressTimeContinual) ? (nowContinual - op.pressTimeContinual) : 0u;
			const bool finalOk = inExpectedContainer && (resolvedObj != nullptr);

			RebuildFocusGraphFromActorInventory(0);

			Msg("* [Gamepad] gp_focus_test bridge_op (%s): ok=%d dir=%s id=%u gen=%llu sec=%s slot=%u delta_frames=%u delta_continual_ms=%u paused=%d lua_res=\"%s\"",
			    op.sourceTag.size() ? op.sourceTag.c_str() : "focus_test",
			    finalOk ? 1 : 0,
			    op.actionDir.size() ? op.actionDir.c_str() : "",
			    op.stableRef.object_id,
			    op.stableRef.instance_gen,
			    op.itemSection.size() ? op.itemSection.c_str() : "",
			    op.targetSlot,
			    deltaFrames,
			    deltaContinualMs,
			    Device.Paused() ? 1 : 0,
			    op.bridgeResultStr.size() ? op.bridgeResultStr.c_str() : "");

			if (ps_gp_telemetry && pInput)
			{
				string512 line;
				xr_sprintf(line, sizeof(line),
				           "gp_bridge_op,%u,src=%s,ok=%d,dir=%s,id=%u,gen=%llu,sec=%s,slot=%u,press_frame=%u,done_frame=%u,delta_frames=%u,delta_ms=%u,paused=%d,lua=%s",
				           nowContinual,
				           op.sourceTag.size() ? op.sourceTag.c_str() : "focus_test",
				           finalOk ? 1 : 0,
				           op.actionDir.size() ? op.actionDir.c_str() : "",
				           op.stableRef.object_id,
				           op.stableRef.instance_gen,
				           op.itemSection.size() ? op.itemSection.c_str() : "",
				           op.targetSlot,
				           op.pressFrame,
				           doneFrame,
				           deltaFrames,
				           deltaContinualMs,
				           Device.Paused() ? 1 : 0,
				           op.bridgeResultStr.size() ? op.bridgeResultStr.c_str() : "");
				pInput->WriteGamepadTelemetryLine(line);
			}

			if (m_pFocusTestWnd)
			{
				string512 uiMsg;
				xr_sprintf(uiMsg, sizeof(uiMsg),
				           "Bridge %s: %s id=%u (gen=%llu, %s) -> %s | delta_frames=%u (+1 frame GE) | %s",
				           finalOk ? "OK" : "FAIL",
				           op.sourceTag.size() ? op.sourceTag.c_str() : "focus_test",
				           op.stableRef.object_id,
				           op.stableRef.instance_gen,
				           op.itemSection.size() ? op.itemSection.c_str() : "",
				           op.actionDir.size() ? op.actionDir.c_str() : "",
				           deltaFrames,
				           op.bridgeResultStr.size() ? op.bridgeResultStr.c_str() : "");
				m_pFocusTestWnd->SetBridgeStatusMessage(uiMsg, finalOk);
			}
		}
	}
}

bool CPauseTokenManager::RunTxnSelfTest()
{
	std::string report;
	const bool ok = gptxn::RunTxnDispatcherSelfTest(&report);
	Msg("* [Gamepad] gp_txn_selftest: pass=%d %s", ok ? 1 : 0, report.c_str());
	if (ps_gp_telemetry && pInput)
	{
		string512 line;
		xr_sprintf(line, sizeof(line), "gp_txn_selftest,%u,pass=%d", Device.dwTimeContinual, ok ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}
	return ok;
}

void CPauseTokenManager::DumpTxnState(LPCSTR args)
{
	if (args && 0 == _stricmp(args, "selftest"))
	{
		RunTxnSelfTest();
		return;
	}
	if (args && 0 == _stricmp(args, "clear"))
	{
		m_txnDispatcher.Clear();
		m_pendingBridgeOp = {};
		Msg("* [Gamepad] gp_txn_dump: cleared transaction dispatcher state");
		return;
	}

	const std::string summary = m_txnDispatcher.FormatDump();
	Msg("* [Gamepad] gp_txn_dump: %s bridge_pending=%d",
	    summary.c_str(),
	    m_pendingBridgeOp.active ? 1 : 0);

	for (const gptxn::TxnRecord& r : m_txnDispatcher.GetRecords())
	{
		Msg("  [txn #%u] cat=%s state=%s op=\"%s\" item=(s=%u,id=%u,g=%llu) frames=(%u->%u, delta=%u) time_ms=(%u->%u, to=%u) ge=%u no_inv_upd=%d reason=\"%s\"",
		    r.txn_id,
		    gptxn::TxnCategoryToString(r.category),
		    gptxn::TxnStateToString(r.state),
		    r.op_name.c_str(),
		    r.stable_id.session_gen,
		    r.stable_id.object_id,
		    static_cast<unsigned long long>(r.stable_id.instance_gen),
		    r.start_frame,
		    r.done_frame,
		    r.delta_frames,
		    r.start_time_ms,
		    r.done_time_ms,
		    r.timeout_ms,
		    r.matched_ge_type,
		    r.no_inv_update_slot ? 1 : 0,
		    r.reason.c_str());
	}
}

void CPauseTokenManager::ResetIdleWaitTimer()
{
	m_idleWaitElapsedUnpausedMs = 0;
	m_idleLastUnpausedTick = 0;
	m_idleLastBusyLogElapsedMs = 0;
}

void CPauseTokenManager::ResetActiveTxStep()
{
	m_collapsedEatBatch.clear();
	m_activeStep = {};
	m_runnerState = ETxRunnerState::Idle;
	m_stepElapsedUnpausedMs = 0;
	m_stepLastUnpausedTick = 0;
	m_stepLastBusyLogElapsedMs = 0;
	m_stepFramesElapsed = 0;
	m_handsFreeStableFrames = 0;
	m_activateIdleStableFrames = 0;
	m_activeEatInitialRemainingUses = 0;
	m_bActiveEatUsesCondition = false;
	m_bCollapsedBatchApplied = false;
	ResetIdleWaitTimer();
}

void CPauseTokenManager::CollapseConsecutiveActivateStepsInQueue(LPCSTR context)
{
	if (m_txQueue.size() < 2)
		return;

	u32 collapsedCount = 0;
	for (size_t i = 0; i + 1 < m_txQueue.size();)
	{
		if (m_txQueue[i].type == EQueuedTxType::ActivateSlot &&
		    m_txQueue[i + 1].type == EQueuedTxType::ActivateSlot &&
		    m_txQueue[i].slotId == m_txQueue[i + 1].slotId)
		{
			const u16 slotId = m_txQueue[i].slotId;
			const u16 itemId = m_txQueue[i + 1].itemId;
			const shared_str sec = m_txQueue[i + 1].section;
			m_txQueue[i] = m_txQueue[i + 1];
			m_txQueue.erase(m_txQueue.begin() + static_cast<ptrdiff_t>(i + 1));
			++collapsedCount;

			Msg("* [Gamepad] gp_tx activate collapsed consecutive duplicate (%s): slot=%u item=%u (%s) remaining_queue=%u",
			    context ? context : "queue", slotId, itemId, sec.c_str(), QueuedTxCount());

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_tx_activate_collapsed,%u,%u,%u,%s,%s,%u",
				           Device.dwTimeContinual,
				           itemId,
				           slotId,
				           sec.c_str(),
				           context ? context : "queue",
				           QueuedTxCount());
				pInput->WriteGamepadTelemetryLine(line);
			}
			continue;
		}
		++i;
	}
}

void CPauseTokenManager::ClearTxQueue(LPCSTR reason)
{
	if (m_txQueue.empty() && m_collapsedEatBatch.empty() && m_runnerState == ETxRunnerState::Idle)
		return;

	LPCSTR safeReason = (reason && reason[0]) ? reason : "clear_tx_queue";
	const u32 droppedTx = QueuedTxCount() + static_cast<u32>(m_collapsedEatBatch.size());
	if (m_runnerState != ETxRunnerState::Idle && m_activeStep.txnId != 0)
		m_txnDispatcher.FailOrCancelTransaction(m_activeStep.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, safeReason);
	for (const QueuedTxEntry& e : m_collapsedEatBatch)
	{
		if (e.txnId != 0)
			m_txnDispatcher.FailOrCancelTransaction(e.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, safeReason);
	}
	for (const QueuedTxEntry& e : m_txQueue)
	{
		if (e.txnId != 0)
			m_txnDispatcher.FailOrCancelTransaction(e.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, safeReason);
	}
	m_txQueue.clear();
	ResetActiveTxStep();

	Msg("* [Gamepad] CPauseTokenManager::ClearTxQueue: dropped=%u reason=\"%s\" dwTimeContinual=%u",
	    droppedTx, safeReason, Device.dwTimeContinual);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_queue_clear,%u,%u,%s",
		           Device.dwTimeContinual, droppedTx, safeReason);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

// C4.2: проверка объекта по ID (существует, не разрушается, владелец, контейнер) без вызова CInventory::Update()
bool CPauseTokenManager::ExecuteOrQueueTransfer(u16 itemId, u16 toId)
{
	if (!g_pGameLevel)
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: no active level");
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: item [%u] not found or destroying", itemId);
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	if (!pIItem || pIItem->GetDropManual())
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: object [%u] is not valid CInventoryItem or pending drop", itemId);
		return false;
	}

	CObject* rawParent = pIItem->object().H_Parent();
	if (!rawParent || rawParent->getDestroy())
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: item [%u] has no valid H_Parent", itemId);
		return false;
	}

	const u16 fromId = rawParent->ID();
	if (fromId == toId)
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: item [%u] already belongs to [%u]", itemId, toId);
		return false;
	}

	CInventoryOwner* fromOwner = smart_cast<CInventoryOwner*>(rawParent);
	CInventoryBox* fromBox = smart_cast<CInventoryBox*>(rawParent);
	bool inSourceContainer = false;
	if (fromOwner)
	{
		inSourceContainer = (pIItem->m_pInventory == &fromOwner->inventory()) &&
		                    fromOwner->inventory().bfCheckForObject(itemId);
	}
	else if (fromBox)
	{
		inSourceContainer = (std::find(fromBox->m_items.begin(), fromBox->m_items.end(), itemId) != fromBox->m_items.end());
	}

	if (!inSourceContainer)
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: item [%u] container mismatch in source [%u]", itemId, fromId);
		return false;
	}

	CObject* rawTarget = Level().Objects.net_Find(toId);
	if (!rawTarget || rawTarget->getDestroy())
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: target [%u] not found or destroying", toId);
		return false;
	}

	CInventoryOwner* toOwner = smart_cast<CInventoryOwner*>(rawTarget);
	CInventoryBox* toBox = smart_cast<CInventoryBox*>(rawTarget);
	if (!toOwner && !toBox)
	{
		Msg("! [Gamepad] gp_tx transfer cancelled: target [%u] is neither CInventoryOwner nor CInventoryBox", toId);
		return false;
	}

	if (GetObjectInstanceGen(itemId) == 0)
		OnObjectNetSpawn(itemId);
	const StableItemID stableRef = MakeStableItemID(itemId);
	const gptxn::TxnBeginResult txnBegin = m_txnDispatcher.BeginCategoryB(
		"TransferItem",
		stableRef,
		gptxn::ExpectedEventMask::TradeSell | gptxn::ExpectedEventMask::TradeBuy |
		gptxn::ExpectedEventMask::OwnershipTake | gptxn::ExpectedEventMask::OwnershipReject,
		Device.dwFrame,
		Device.dwTimeContinual);
	if (!txnBegin.accepted)
	{
		Msg("! [Gamepad] gp_tx transfer blocked by TxnDispatcher: item [%u] (gen=%llu) reason=%s dup_blocked=%u",
		    itemId, stableRef.instance_gen, txnBegin.reason.c_str(), m_txnDispatcher.GetStats().duplicate_a_blocked);
		return false;
	}

	// Путь CScriptGameObject::TransferItem (script_game_object_inventory_owner.cpp:607-633)
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_TRADE_SELL, fromId);
	P.w_u16(itemId);
	CGameObject::u_EventSend(P);

	CGameObject::u_EventGen(P, GE_TRADE_BUY, toId);
	P.w_u16(itemId);
	CGameObject::u_EventSend(P);

	Msg("* [Gamepad] gp_tx transfer committed: item=%u (%s) from=%u to=%u paused=%d",
	    itemId, pIItem->object().cNameSect().c_str(), fromId, toId, Device.Paused() ? 1 : 0);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,transfer,%u,%u,%u,%s,%d",
		           Device.dwTimeContinual,
		           itemId,
		           fromId,
		           toId,
		           pIItem->object().cNameSect().c_str(),
		           Device.Paused() ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return true;
}

bool CPauseTokenManager::ExecuteOrQueueSlot(u16 itemId, u16 slotId)
{
	if (!g_pGameLevel)
	{
		Msg("! [Gamepad] gp_tx slot cancelled: no active level");
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
	{
		Msg("! [Gamepad] gp_tx slot cancelled: actor not available or dead");
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		Msg("! [Gamepad] gp_tx slot cancelled: item [%u] not found or destroying", itemId);
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	if (!pIItem || pIItem->GetDropManual())
	{
		Msg("! [Gamepad] gp_tx slot cancelled: object [%u] is not valid CInventoryItem or pending drop", itemId);
		return false;
	}

	CObject* rawParent = pIItem->object().H_Parent();
	if (!rawParent || rawParent->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(itemId))
	{
		Msg("! [Gamepad] gp_tx slot cancelled: item [%u] is not owned by actor [%u] or not in inventory",
		    itemId, pActor->ID());
		return false;
	}

	if (slotId == NO_ACTIVE_SLOT || slotId > pActor->inventory().LastSlot())
	{
		Msg("! [Gamepad] gp_tx slot cancelled: invalid slot [%u]", slotId);
		return false;
	}

	if (pActor->inventory().ItemFromSlot(slotId) == pIItem)
	{
		Msg("! [Gamepad] gp_tx slot cancelled: item [%u] is already in slot [%u]", itemId, slotId);
		return false;
	}

	// Путь CScriptGameObject::MoveItemToSlot (script_game_object_inventory_owner.cpp:522-558)
	// Если в целевом слоте уже есть предмет, сначала вытесняем его в рюкзак, чтобы CanPutInSlot вернул true
	CInventoryItem* itemInSlot = pActor->inventory().ItemFromSlot(slotId);
	if (itemInSlot && !pActor->inventory().CanPutInRuck(itemInSlot))
	{
		Msg("! [Gamepad] gp_tx slot cancelled: existing item [%u] in slot [%u] cannot be moved to ruck",
		    itemInSlot->object().ID(), slotId);
		return false;
	}

	// Если слот пуст, проверим CanPutInSlot напрямую; если занят, проверим, подходит ли слот и не заблокирован ли
	if (!itemInSlot && !pActor->inventory().CanPutInSlot(pIItem, slotId))
	{
		Msg("! [Gamepad] gp_tx slot cancelled: CanPutInSlot failed for item [%u] (%s) -> slot [%u]",
		    itemId, pIItem->object().cNameSect().c_str(), slotId);
		return false;
	}

	if (GetObjectInstanceGen(itemId) == 0)
		OnObjectNetSpawn(itemId);
	const StableItemID stableRef = MakeStableItemID(itemId);
	const gptxn::TxnBeginResult txnBegin = m_txnDispatcher.BeginCategoryB(
		"SlotItem",
		stableRef,
		gptxn::ExpectedEventMask::ItemToSlot,
		Device.dwFrame,
		Device.dwTimeContinual,
		gptxn::TransactionDispatcher::kDefaultCategoryBTimeoutMs,
		true);
	if (!txnBegin.accepted)
	{
		Msg("! [Gamepad] gp_tx slot blocked by TxnDispatcher: item [%u] (gen=%llu) reason=%s dup_blocked=%u",
		    itemId, stableRef.instance_gen, txnBegin.reason.c_str(), m_txnDispatcher.GetStats().duplicate_a_blocked);
		return false;
	}

	NET_Packet P;
	if (itemInSlot)
	{
		CGameObject::u_EventGen(P, GEG_PLAYER_ITEM2RUCK, pActor->object_id());
		P.w_u16(itemInSlot->object().ID());
		CGameObject::u_EventSend(P);
	}

	const bool doNotActivate = (Device.Paused() || Count() > 0);
	CGameObject::u_EventGen(P, GEG_PLAYER_ITEM2SLOT, pActor->object_id());
	P.w_u16(itemId);
	P.w_u16(slotId);
	P.w_u8(doNotActivate ? 1 : 0);
	CGameObject::u_EventSend(P);

	Msg("* [Gamepad] gp_tx slot committed: item=%u (%s) slot=%u do_not_activate=%d paused=%d",
	    itemId, pIItem->object().cNameSect().c_str(), slotId, doNotActivate ? 1 : 0, Device.Paused() ? 1 : 0);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,slot,%u,%u,%d,%s,%d",
		           Device.dwTimeContinual,
		           itemId,
		           slotId,
		           doNotActivate ? 1 : 0,
		           pIItem->object().cNameSect().c_str(),
		           Device.Paused() ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return true;
}

bool CPauseTokenManager::ExecuteActivateNow(const QueuedTxEntry& entry, LPCSTR source)
{
	auto cancelPending = [&](LPCSTR cancelReason)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.FailOrCancelTransaction(entry.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, cancelReason);
	};

	if (!g_pGameLevel)
	{
		cancelPending("cancelled:no_active_level");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: no active level", source);
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive() || pActor->ID() != entry.ownerId)
	{
		cancelPending("cancelled:actor_unavailable");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: actor unavailable, dead, or changed", source);
		return false;
	}

	if (entry.slotId == NO_ACTIVE_SLOT || entry.slotId > pActor->inventory().LastSlot())
	{
		cancelPending("cancelled:invalid_slot");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: invalid slot [%u]", source, entry.slotId);
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(entry.itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, {}, Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: item [%u] no longer exists or destroying", source, entry.itemId);
		return false;
	}

	if (entry.stableId.IsValid() && ResolveStableItem(entry.stableId) != rawItem)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: StableItemID mismatch for item [%u] (expected gen=%llu, actual gen=%llu)",
		    source, entry.itemId, entry.stableId.instance_gen, GetObjectInstanceGen(entry.itemId));
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	if (!pIItem || pIItem->GetDropManual() ||
	    !pIItem->object().H_Parent() ||
	    pIItem->object().H_Parent()->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(entry.itemId))
	{
		cancelPending("cancelled:ownership_check_failed");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: item [%u] ownership/inventory check failed", source, entry.itemId);
		return false;
	}

	if (xr_strcmp(pIItem->object().cNameSect(), entry.section) != 0)
	{
		cancelPending("cancelled:section_changed");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: item [%u] section changed (%s != %s)",
		    source, entry.itemId, pIItem->object().cNameSect().c_str(), entry.section.c_str());
		return false;
	}

	if (pActor->inventory().ItemFromSlot(entry.slotId) != pIItem ||
	    pIItem->CurrPlace() != eItemPlaceSlot ||
	    pIItem->CurrSlot() != entry.slotId)
	{
		cancelPending("cancelled:slot_changed");
		Msg("! [Gamepad] gp_tx activate (%s) cancelled: item [%u] is no longer in slot [%u]",
		    source, entry.itemId, entry.slotId);
		return false;
	}

	if (entry.txnId != 0)
	{
		m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
	}

	if (IsTargetSlotActiveAndIdle(entry.slotId))
	{
		Msg("* [Gamepad] gp_tx activate (%s) committed (already active and eIdle): slot=%u item=%u (%s) paused=%d",
		    source, entry.slotId, entry.itemId, entry.section.c_str(), Device.Paused() ? 1 : 0);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,activate,%u,%u,%s,%s,%d",
			           Device.dwTimeContinual,
			           entry.itemId,
			           entry.slotId,
			           entry.section.c_str(),
			           source,
			           Device.Paused() ? 1 : 0);
			pInput->WriteGamepadTelemetryLine(line);
		}
		return true;
	}

	// Путь MakeItemActive (script_game_object_inventory_owner.cpp:497-500): отправка GEG_PLAYER_ACTIVATE_SLOT
	NET_Packet P;
	CGameObject::u_EventGen(P, GEG_PLAYER_ACTIVATE_SLOT, pActor->object_id());
	P.w_u16(entry.slotId);
	CGameObject::u_EventSend(P);

	Msg("* [Gamepad] gp_tx activate (%s) committed: slot=%u item=%u (%s) paused=%d",
	    source, entry.slotId, entry.itemId, entry.section.c_str(), Device.Paused() ? 1 : 0);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,activate,%u,%u,%s,%s,%d",
		           Device.dwTimeContinual,
		           entry.itemId,
		           entry.slotId,
		           entry.section.c_str(),
		           source,
		           Device.Paused() ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return true;
}

bool CPauseTokenManager::ExecuteOrQueueActivate(u16 slotId)
{
	if (!g_pGameLevel)
	{
		Msg("! [Gamepad] gp_tx activate cancelled: no active level");
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
	{
		Msg("! [Gamepad] gp_tx activate cancelled: actor not available or dead");
		return false;
	}

	if (slotId == NO_ACTIVE_SLOT || slotId > pActor->inventory().LastSlot())
	{
		Msg("! [Gamepad] gp_tx activate cancelled: invalid slot [%u]", slotId);
		return false;
	}

	CInventoryItem* pIItem = pActor->inventory().ItemFromSlot(slotId);
	if (!pIItem)
	{
		Msg("! [Gamepad] gp_tx activate cancelled: slot [%u] is empty", slotId);
		return false;
	}

	const u16 itemId = pIItem->object().ID();
	CObject* rawItem = Level().Objects.net_Find(itemId);
	if (!rawItem || rawItem != &pIItem->object() || rawItem->getDestroy() || pIItem->GetDropManual())
	{
		Msg("! [Gamepad] gp_tx activate cancelled: item [%u] in slot [%u] is invalid or destroying", itemId, slotId);
		return false;
	}

	if (!pIItem->object().H_Parent() ||
	    pIItem->object().H_Parent()->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(itemId))
	{
		Msg("! [Gamepad] gp_tx activate cancelled: item [%u] ownership mismatch", itemId);
		return false;
	}

	if (GetObjectInstanceGen(itemId) == 0)
		OnObjectNetSpawn(itemId);
	const StableItemID stableRef = MakeStableItemID(itemId);
	if (m_txnDispatcher.IsItemLocked(stableRef) || m_txnDispatcher.IsObjectIdLockedInCategoryB(itemId))
	{
		Msg("! [Gamepad] gp_tx activate cancelled: item [%u] (gen=%llu) is locked in Pending Category B transaction",
		    itemId, stableRef.instance_gen);
		return false;
	}

	QueuedTxEntry entry;
	entry.type = EQueuedTxType::ActivateSlot;
	entry.itemId = itemId;
	entry.slotId = slotId;
	entry.ownerId = pActor->ID();
	entry.prevPlaceType = pIItem->CurrPlace();
	entry.prevSlotId = pIItem->CurrSlot();
	entry.stableId = stableRef;
	entry.section = pIItem->object().cNameSect();
	entry.enqueuedTimeContinual = Device.dwTimeContinual;

	if (Device.Paused() || Count() > 0 || m_runnerState != ETxRunnerState::Idle || !m_txQueue.empty())
	{
		const gptxn::TxnBeginResult txnC = m_txnDispatcher.BeginCategoryC(
			"ActivateSlot", stableRef, gptxn::ExpectedEventMask::None, Device.dwFrame, Device.dwTimeContinual);
		entry.txnId = txnC.txn_id;
		if (!m_txQueue.empty() &&
		    m_txQueue.back().type == EQueuedTxType::ActivateSlot &&
		    m_txQueue.back().slotId == slotId)
		{
			m_txQueue.back() = entry;
			Msg("* [Gamepad] gp_tx activate collapsed consecutive duplicate (enqueue): slot=%u item=%u (%s) queue_size=%u tokens=%u paused=%d",
			    slotId, itemId, entry.section.c_str(), QueuedTxCount(), Count(), Device.Paused() ? 1 : 0);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_tx_activate_collapsed,%u,%u,%u,%s,enqueue,%u",
				           Device.dwTimeContinual,
				           itemId,
				           slotId,
				           entry.section.c_str(),
				           QueuedTxCount());
				pInput->WriteGamepadTelemetryLine(line);
			}
			return true;
		}

		m_txQueue.push_back(entry);
		Msg("* [Gamepad] gp_tx activate queued (D11): slot=%u item=%u (%s) queue_size=%u tokens=%u paused=%d",
		    slotId, itemId, entry.section.c_str(), QueuedTxCount(), Count(), Device.Paused() ? 1 : 0);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_tx_queued,%u,activate,%u,%u,%s,%u,%u,%d",
			           Device.dwTimeContinual,
			           itemId,
			           slotId,
			           entry.section.c_str(),
			           QueuedTxCount(),
			           Count(),
			           Device.Paused() ? 1 : 0);
			pInput->WriteGamepadTelemetryLine(line);
		}
		return true;
	}

	return ExecuteActivateNow(entry, "direct");
}

shared_str CPauseTokenManager::ResolveConsumableCategory(LPCSTR section) const
{
	if (!section || !section[0])
		return shared_str("other");

	if (pSettings)
	{
		if (pSettings->section_exist("gp_consumable_categories") &&
		    pSettings->line_exist("gp_consumable_categories", section))
		{
			LPCSTR explicitCat = pSettings->r_string("gp_consumable_categories", section);
			if (explicitCat && explicitCat[0])
				return shared_str(explicitCat);
		}

		if (pSettings->section_exist(section) && pSettings->line_exist(section, "kind"))
		{
			LPCSTR kind = pSettings->r_string(section, "kind");
			if (kind && kind[0])
			{
				if (xr_strcmp(kind, "i_food") == 0 ||
				    xr_strcmp(kind, "i_drink") == 0 ||
				    xr_strcmp(kind, "i_mutant_cooked") == 0 ||
				    xr_strcmp(kind, "i_mutant_raw") == 0)
				{
					return shared_str("food_drink");
				}
				if (xr_strcmp(kind, "i_medical") == 0)
				{
					return shared_str("medical");
				}
			}
		}
	}

	return shared_str(section);
}

static bool RunGammaEatPrecheckWithoutAnim(CScriptGameObject* luaObj)
{
	if (!luaObj)
		return false;

	lua_State* L = ai().script_engine().lua();
	if (!L)
		return true;

	const int top = lua_gettop(L);

	// C4.2c (вариант б): вызываем штатный _G.CInventory__eat(obj), который рассылает actor_on_item_before_use
	// всем подписчикам РОВНО ОДИН РАЗ (включая liz_fdda_redone_consumables.originalAOIBU, western_goods_on_item_before_use
	// и прочие перехватчики), но на время вызова временно подменяем liz_fdda_redone_consumables.modifiedAOIBU
	// (проверяет только mcClimb / mcm_strict_sprinting без вызова perform_item_use / perform_vanilla_item_use)
	// с гарантированным восстановлением после pcall.
	static const char* kDefinePrecheckFn =
		"if not _G.__gp_eat_precheck_no_anim then\n"
		"  _G.__gp_eat_precheck_no_anim = function(obj)\n"
		"    if not _G.CInventory__eat then return true end\n"
		"    local fdda = _G.liz_fdda_redone_consumables\n"
		"    local saved_mod = fdda and fdda.modifiedAOIBU or nil\n"
		"    if fdda and saved_mod then\n"
		"      fdda.modifiedAOIBU = function(o, flags)\n"
		"        if IsMoveState and IsMoveState('mcClimb') then\n"
		"          if actor_menu and game then actor_menu.set_msg(1, game.translate_string('st_EA_ladder_item_not_allowed'), 3) end\n"
		"          flags.ret_value = false\n"
		"          return\n"
		"        end\n"
		"        if fdda.mcm_strict_sprinting and IsMoveState and IsMoveState('mcSprint') then\n"
		"          flags.ret_value = false\n"
		"          return\n"
		"        end\n"
		"      end\n"
		"    end\n"
		"    local ok, res = pcall(_G.CInventory__eat, obj)\n"
		"    if fdda and saved_mod then\n"
		"      fdda.modifiedAOIBU = saved_mod\n"
		"    end\n"
		"    if not ok then return false end\n"
		"    return res == true\n"
		"  end\n"
		"end";

	luaL_dostring(L, kDefinePrecheckFn);
	lua_settop(L, top);

	::luabind::functor<bool> precheckFunct;
	if (ai().script_engine().functor("_G.__gp_eat_precheck_no_anim", precheckFunct))
	{
		try
		{
			return precheckFunct(luaObj);
		}
		catch (...)
		{
			return false;
		}
	}
	return false;
}

static void ClearLam2SavedWeaponSlotForPendingActivate()
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return;

	const int top = lua_gettop(L);
	lua_getglobal(L, "lam2");
	if (lua_istable(L, -1))
	{
		lua_pushnil(L);
		lua_setfield(L, -2, "cur_slot");
	}
	lua_settop(L, top);
}

bool CPauseTokenManager::ExecuteEatWithoutAnimNow(const QueuedTxEntry& entry, LPCSTR source)
{
	auto cancelPending = [&](LPCSTR cancelReason)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.FailOrCancelTransaction(entry.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, cancelReason);
	};

	if (!g_pGameLevel)
	{
		cancelPending("cancelled:no_active_level");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: no active level", source);
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive() || pActor->ID() != entry.ownerId)
	{
		cancelPending("cancelled:actor_unavailable");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: actor unavailable, dead, or changed", source);
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(entry.itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, {}, Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] no longer exists or destroying", source, entry.itemId);
		return false;
	}

	if (entry.stableId.IsValid() && ResolveStableItem(entry.stableId) != rawItem)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: StableItemID mismatch for item [%u] (expected gen=%llu, actual gen=%llu)",
		    source, entry.itemId, entry.stableId.instance_gen, GetObjectInstanceGen(entry.itemId));
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	CEatableItem* pEatable = smart_cast<CEatableItem*>(rawItem);
	if (!pIItem || !pEatable || pIItem->GetDropManual() || !pEatable->Useful() || pEatable->Empty())
	{
		cancelPending("cancelled:invalid_eatable");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] is not a valid/non-empty CEatableItem", source, entry.itemId);
		return false;
	}

	if (!pIItem->object().H_Parent() ||
	    pIItem->object().H_Parent()->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(entry.itemId))
	{
		cancelPending("cancelled:ownership_check_failed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] ownership/inventory check failed", source, entry.itemId);
		return false;
	}

	if (xr_strcmp(pIItem->object().cNameSect(), entry.section) != 0)
	{
		cancelPending("cancelled:section_changed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] section changed (%s != %s)",
		    source, entry.itemId, pIItem->object().cNameSect().c_str(), entry.section.c_str());
		return false;
	}

	const u16 curPlace = pIItem->CurrPlace();
	const bool inValidContainer = (curPlace == eItemPlaceRuck && pActor->inventory().InRuck(pIItem)) ||
	                              (curPlace == eItemPlaceBelt && pActor->inventory().InBelt(pIItem)) ||
	                              (curPlace == eItemPlaceSlot && pActor->inventory().InSlot(pIItem));
	if (!inValidContainer || curPlace != entry.prevPlaceType)
	{
		cancelPending("cancelled:container_changed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] container changed (%u != %u)",
		    source, entry.itemId, curPlace, entry.prevPlaceType);
		return false;
	}

	CGameObject* GO = pIItem->cast_game_object();
	if (GO && !RunGammaEatPrecheckWithoutAnim(GO->lua_game_object()))
	{
		cancelPending("cancelled:precheck_blocked");
		Msg("* [Gamepad] gp_tx eat (%s): pre-use check blocked collapsed item=%u (%s)",
		    source, entry.itemId, entry.section.c_str());
		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_tx_eat_script_intercept,%u,%u,%s,%s,%d",
			           Device.dwTimeContinual,
			           entry.itemId,
			           entry.section.c_str(),
			           source,
			           Device.Paused() ? 1 : 0);
			pInput->WriteGamepadTelemetryLine(line);
		}
		return false;
	}

	if (entry.txnId != 0)
	{
		m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
	}

	// Временно отключаем actor_effects.play_item_fx на время мгновенного CInventory::Eat (чтобы не запускать ванильный cam/snd эффект поверх завершившейся анимации категории)
	lua_State* L = ai().script_engine().lua();
	int savedTop = L ? lua_gettop(L) : 0;
	bool suppressedPif = false;
	if (L)
	{
		static const char* kSuppressPif =
			"if actor_effects and actor_effects.play_item_fx then\n"
			"  _G.__gp_saved_pif = actor_effects.play_item_fx\n"
			"  actor_effects.play_item_fx = function() end\n"
			"end";
		if (luaL_dostring(L, kSuppressPif) == 0)
			suppressedPif = true;
		lua_settop(L, savedTop);
	}

	const bool ate = pActor->inventory().Eat(pIItem);

	if (L && suppressedPif)
	{
		static const char* kRestorePif =
			"if actor_effects and _G.__gp_saved_pif then\n"
			"  actor_effects.play_item_fx = _G.__gp_saved_pif\n"
			"  _G.__gp_saved_pif = nil\n"
			"end";
		luaL_dostring(L, kRestorePif);
		lua_settop(L, savedTop);
	}

	Msg("* [Gamepad] gp_tx eat (%s) committed (collapsed no-anim, cat=%s): item=%u (%s) ate=%d paused=%d",
	    source,
	    entry.category.size() ? entry.category.c_str() : "other",
	    entry.itemId,
	    entry.section.c_str(),
	    ate ? 1 : 0,
	    Device.Paused() ? 1 : 0);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,eat,%u,%d,%s,%s,%d",
		           Device.dwTimeContinual,
		           entry.itemId,
		           ate ? 1 : 0,
		           entry.section.c_str(),
		           source,
		           Device.Paused() ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return ate;
}

bool CPauseTokenManager::ExecuteEatNow(const QueuedTxEntry& entry, LPCSTR source)
{
	auto cancelPending = [&](LPCSTR cancelReason)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.FailOrCancelTransaction(entry.txnId, gptxn::TxnState::Cancelled, Device.dwFrame, Device.dwTimeContinual, cancelReason);
	};

	if (!g_pGameLevel)
	{
		cancelPending("cancelled:no_active_level");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: no active level", source);
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive() || pActor->ID() != entry.ownerId)
	{
		cancelPending("cancelled:actor_unavailable");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: actor unavailable, dead, or changed", source);
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(entry.itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, {}, Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] no longer exists or destroying", source, entry.itemId);
		return false;
	}

	if (entry.stableId.IsValid() && ResolveStableItem(entry.stableId) != rawItem)
	{
		if (entry.txnId != 0)
			m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: StableItemID mismatch for item [%u] (expected gen=%llu, actual gen=%llu)",
		    source, entry.itemId, entry.stableId.instance_gen, GetObjectInstanceGen(entry.itemId));
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	CEatableItem* pEatable = smart_cast<CEatableItem*>(rawItem);
	if (!pIItem || !pEatable || pIItem->GetDropManual() || !pEatable->Useful() || pEatable->Empty())
	{
		cancelPending("cancelled:invalid_eatable");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] is not a valid/non-empty CEatableItem", source, entry.itemId);
		return false;
	}

	if (!pIItem->object().H_Parent() ||
	    pIItem->object().H_Parent()->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(entry.itemId))
	{
		cancelPending("cancelled:ownership_check_failed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] ownership/inventory check failed", source, entry.itemId);
		return false;
	}

	if (xr_strcmp(pIItem->object().cNameSect(), entry.section) != 0)
	{
		cancelPending("cancelled:section_changed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] section changed (%s != %s)",
		    source, entry.itemId, pIItem->object().cNameSect().c_str(), entry.section.c_str());
		return false;
	}

	const u16 curPlace = pIItem->CurrPlace();
	const bool inValidContainer = (curPlace == eItemPlaceRuck && pActor->inventory().InRuck(pIItem)) ||
	                              (curPlace == eItemPlaceBelt && pActor->inventory().InBelt(pIItem)) ||
	                              (curPlace == eItemPlaceSlot && pActor->inventory().InSlot(pIItem));
	if (!inValidContainer || curPlace != entry.prevPlaceType)
	{
		cancelPending("cancelled:container_changed");
		Msg("! [Gamepad] gp_tx eat (%s) cancelled: item [%u] container changed (%u != %u)",
		    source, entry.itemId, curPlace, entry.prevPlaceType);
		return false;
	}

	// Штатный путь Actor_Events.cpp:253-263 / ActorInput.cpp:240-248 (_G.CInventory__eat -> inventory().Eat)
	::luabind::functor<bool> funct;
	CGameObject* GO = pIItem->cast_game_object();
	CScriptGameObject* luaGO = GO ? GO->lua_game_object() : nullptr;
	if (luaGO && ai().script_engine().functor("_G.CInventory__eat", funct))
	{
		bool scriptContinue = true;
		try
		{
			scriptContinue = funct(luaGO);
		}
		catch (...)
		{
			cancelPending("cancelled:script_exception");
			return false;
		}
		if (!scriptContinue)
		{
			if (entry.txnId != 0)
				m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
			Msg("* [Gamepad] gp_tx eat (%s): _G.CInventory__eat returned false (handled/blocked by script) for item=%u (%s)",
			    source, entry.itemId, entry.section.c_str());
			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_tx_eat_script_intercept,%u,%u,%s,%s,%d",
				           Device.dwTimeContinual,
				           entry.itemId,
				           entry.section.c_str(),
				           source,
				           Device.Paused() ? 1 : 0);
				pInput->WriteGamepadTelemetryLine(line);
			}
			return true;
		}
	}

	if (entry.txnId != 0)
	{
		m_txnDispatcher.VerifyAndCommitCategoryC(entry.txnId, MakeStableItemID(entry.itemId), Device.dwFrame, Device.dwTimeContinual);
	}

	const bool ate = pActor->inventory().Eat(pIItem);
	Msg("* [Gamepad] gp_tx eat (%s) committed: item=%u (%s) ate=%d paused=%d",
	    source, entry.itemId, entry.section.c_str(), ate ? 1 : 0, Device.Paused() ? 1 : 0);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_commit,%u,eat,%u,%d,%s,%s,%d",
		           Device.dwTimeContinual,
		           entry.itemId,
		           ate ? 1 : 0,
		           entry.section.c_str(),
		           source,
		           Device.Paused() ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}

	return ate;
}

bool CPauseTokenManager::ExecuteOrQueueEat(u16 itemId)
{
	if (!g_pGameLevel)
	{
		Msg("! [Gamepad] gp_tx eat cancelled: no active level");
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
	{
		Msg("! [Gamepad] gp_tx eat cancelled: actor not available or dead");
		return false;
	}

	CObject* rawItem = Level().Objects.net_Find(itemId);
	if (!rawItem || rawItem->getDestroy())
	{
		Msg("! [Gamepad] gp_tx eat cancelled: item [%u] not found or destroying", itemId);
		return false;
	}

	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	CEatableItem* pEatable = smart_cast<CEatableItem*>(rawItem);
	if (!pIItem || !pEatable || pIItem->GetDropManual() || !pEatable->Useful() || pEatable->Empty())
	{
		Msg("! [Gamepad] gp_tx eat cancelled: object [%u] is not a valid/non-empty CEatableItem", itemId);
		return false;
	}

	if (!pIItem->object().H_Parent() ||
	    pIItem->object().H_Parent()->ID() != pActor->ID() ||
	    pIItem->m_pInventory != &pActor->inventory() ||
	    !pActor->inventory().bfCheckForObject(itemId))
	{
		Msg("! [Gamepad] gp_tx eat cancelled: item [%u] is not owned by actor [%u]", itemId, pActor->ID());
		return false;
	}

	const u16 curPlace = pIItem->CurrPlace();
	const bool inValidContainer = (curPlace == eItemPlaceRuck && pActor->inventory().InRuck(pIItem)) ||
	                              (curPlace == eItemPlaceBelt && pActor->inventory().InBelt(pIItem)) ||
	                              (curPlace == eItemPlaceSlot && pActor->inventory().InSlot(pIItem));
	if (!inValidContainer)
	{
		Msg("! [Gamepad] gp_tx eat cancelled: item [%u] is not in ruck/belt/slot", itemId);
		return false;
	}

	if (GetObjectInstanceGen(itemId) == 0)
		OnObjectNetSpawn(itemId);
	const StableItemID stableRef = MakeStableItemID(itemId);
	if (m_txnDispatcher.IsItemLocked(stableRef) || m_txnDispatcher.IsObjectIdLockedInCategoryB(itemId))
	{
		Msg("! [Gamepad] gp_tx eat cancelled: item [%u] (gen=%llu) is locked in Pending Category B transaction",
		    itemId, stableRef.instance_gen);
		return false;
	}

	QueuedTxEntry entry;
	entry.type = EQueuedTxType::EatItem;
	entry.itemId = itemId;
	entry.slotId = pIItem->CurrSlot();
	entry.ownerId = pActor->ID();
	entry.prevPlaceType = curPlace;
	entry.prevSlotId = pIItem->CurrSlot();
	entry.stableId = stableRef;
	entry.section = pIItem->object().cNameSect();
	entry.category = ResolveConsumableCategory(entry.section.c_str());
	entry.enqueuedTimeContinual = Device.dwTimeContinual;

	if (Device.Paused() || Count() > 0 || m_runnerState != ETxRunnerState::Idle || !m_txQueue.empty())
	{
		// Проверка от дублирования одного и того же одноразового предмета в очереди сверх доступных порций
		u32 alreadyQueuedForItem = 0;
		for (const auto& q : m_txQueue)
		{
			if (q.type == EQueuedTxType::EatItem && q.itemId == itemId)
				++alreadyQueuedForItem;
		}
		for (const auto& q : m_collapsedEatBatch)
		{
			if (q.type == EQueuedTxType::EatItem && q.itemId == itemId)
				++alreadyQueuedForItem;
		}
		if (m_runnerState != ETxRunnerState::Idle &&
		    m_activeStep.type == EQueuedTxType::EatItem &&
		    m_activeStep.itemId == itemId &&
		    !HasActiveEatItemBeenConsumed())
		{
			++alreadyQueuedForItem;
		}
		const u32 maxAllowed = pEatable->IsUsingCondition() ? _max<u32>(1u, pEatable->GetRemainingUses()) : 1u;
		if (alreadyQueuedForItem >= maxAllowed)
		{
			Msg("! [Gamepad] gp_tx eat cancelled: item [%u] (%s) already queued %u time(s) (max uses=%u)",
			    itemId, entry.section.c_str(), alreadyQueuedForItem, maxAllowed);
			return false;
		}

		const gptxn::TxnBeginResult txnC = m_txnDispatcher.BeginCategoryC(
			"EatItem", stableRef, gptxn::ExpectedEventMask::ItemEat, Device.dwFrame, Device.dwTimeContinual);
		entry.txnId = txnC.txn_id;
		m_txQueue.push_back(entry);
		Msg("* [Gamepad] gp_tx eat queued (D11): item=%u (%s, cat=%s) queue_size=%u tokens=%u paused=%d",
		    itemId, entry.section.c_str(), entry.category.c_str(), QueuedTxCount(), Count(), Device.Paused() ? 1 : 0);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_tx_queued,%u,eat,%u,%u,%s,%u,%u,%d",
			           Device.dwTimeContinual,
			           itemId,
			           curPlace,
			           entry.section.c_str(),
			           QueuedTxCount(),
			           Count(),
			           Device.Paused() ? 1 : 0);
			pInput->WriteGamepadTelemetryLine(line);
		}
		return true;
	}

	return ExecuteEatNow(entry, "direct");
}

bool CPauseTokenManager::IsTargetSlotActiveAndIdle(u16 slotId) const
{
	if (!g_pGameLevel || slotId == NO_ACTIVE_SLOT)
		return false;

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
		return false;

	CInventory& inv = pActor->inventory();
	if (inv.GetActiveSlot() != slotId || inv.GetNextActiveSlot() != slotId)
		return false;

	PIItem slotItem = inv.ItemFromSlot(slotId);
	PIItem activeItem = inv.ActiveItem();
	if (!slotItem || activeItem != slotItem)
		return false;

	if (CHudItem* hudItem = activeItem->cast_hud_item())
	{
		if (hudItem->IsPending() || hudItem->GetState() != CHUDState::eIdle)
			return false;
	}

	return true;
}

bool CPauseTokenManager::AreActorHandsFreeNow(xr_string* outBusyReason) const
{
	if (outBusyReason)
		outBusyReason->clear();

	auto appendReason = [outBusyReason](LPCSTR text)
	{
		if (!outBusyReason || !text || !text[0])
			return;
		if (!outBusyReason->empty())
			*outBusyReason += "|";
		*outBusyReason += text;
	};

	if (!g_pGameLevel || Device.Paused() || !m_tokens.empty())
	{
		appendReason("precheck_paused_or_tokens");
		return false;
	}

	CActor* pActor = g_actor;
	if (!pActor || !pActor->g_Alive())
	{
		appendReason("precheck_actor_dead_or_null");
		return false;
	}

	bool allFree = true;

	// 1. Скриптовые анимации HUD в C++ (player_hud::script_anim_part != u8(-1) или несовместимость attached_item(1))
	if (g_player_hud && !g_player_hud->allow_activation(nullptr))
	{
		if (!outBusyReason)
			return false;
		string128 buf;
		xr_sprintf(buf, sizeof(buf), "check1_player_hud(script_anim_part=%u;attached1=%d)",
		           static_cast<u32>(g_player_hud->script_anim_part),
		           g_player_hud->attached_item(1) ? 1 : 0);
		appendReason(buf);
		allFree = false;
	}

	// 2. Переход между слотами оружия в C++
	CInventory& inv = pActor->inventory();
	if (inv.GetActiveSlot() != inv.GetNextActiveSlot())
	{
		if (!outBusyReason)
			return false;
		string128 buf;
		xr_sprintf(buf, sizeof(buf), "check2_slot_transition(active=%u;next=%u)",
		           static_cast<u32>(inv.GetActiveSlot()),
		           static_cast<u32>(inv.GetNextActiveSlot()));
		appendReason(buf);
		allFree = false;
	}

	// 3. Состояние активного предмета в руках (не eShowing / eHiding / перезарядка / eBore / IsPending)
	if (PIItem activeItem = inv.ActiveItem())
	{
		if (CHudItem* hudItem = activeItem->cast_hud_item())
		{
			if (hudItem->IsPending() || hudItem->GetState() != CHUDState::eIdle)
			{
				if (!outBusyReason)
					return false;
				string128 buf;
				xr_sprintf(buf, sizeof(buf), "check3_active_item(id=%u;sec=%s;state=%u;pending=%d)",
				           activeItem->object().ID(),
				           activeItem->object().cNameSect().c_str(),
				           hudItem->GetState(),
				           hudItem->IsPending() ? 1 : 0);
				appendReason(buf);
				allFree = false;
			}
		}
	}

	// 4. Состояние детектора / устройства в левой руке (DETECTOR_SLOT)
	if (PIItem detItem = inv.ItemFromSlot(DETECTOR_SLOT))
	{
		if (CCustomDevice* dev = smart_cast<CCustomDevice*>(detItem))
		{
			if (dev->GetState() != CHUDState::eHidden &&
			    (dev->IsPending() || dev->GetState() != CHUDState::eIdle))
			{
				if (!outBusyReason)
					return false;
				string128 buf;
				xr_sprintf(buf, sizeof(buf), "check4_detector(id=%u;sec=%s;state=%u;pending=%d)",
				           detItem->object().ID(),
				           detItem->object().cNameSect().c_str(),
				           dev->GetState(),
				           dev->IsPending() ? 1 : 0);
				appendReason(buf);
				allFree = false;
			}
		}
	}

	// 5. Скриптовые автоматы анимаций GAMMA: lam2.is_running() и actor_effects.item_not_in_use
	lua_State* L = ai().script_engine().lua();
	if (L)
	{
		const int top = lua_gettop(L);
		bool lam2Running = false;
		string128 lam2Details = "";

		lua_getglobal(L, "lam2");
		if (lua_istable(L, -1))
		{
			// C4.2c (R2): в lam2.script:14 переменная b_is_running объявлена как local,
			// поэтому опрашиваем её через публичный геттер lam2.is_running() (lam2.script:191-193).
			lua_getfield(L, -1, "is_running");
			if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0)
			{
				if (lua_toboolean(L, -1))
					lam2Running = true;
				lua_pop(L, 1);
			}
			else
			{
				lua_pop(L, 1);
			}

			if (lam2Running && outBusyReason)
			{
				// C4.2d-N1: current_action в lam2.script — local; читаем через геттер lam2.get_current_action() (lam2.script:195-197).
				lua_getfield(L, -1, "get_current_action");
				if (!lua_isfunction(L, -1) || lua_pcall(L, 0, 1, 0) != 0)
				{
					lua_pop(L, 1);
					lua_pushnil(L);
				}
				if (lua_istable(L, -1))
				{
					lua_getfield(L, -1, "tag");
					LPCSTR tagStr = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
					lua_getfield(L, -2, "name");
					LPCSTR nameStr = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
					xr_sprintf(lam2Details, sizeof(lam2Details), "(tag=%s;name=%s)",
					           tagStr ? tagStr : "",
					           nameStr ? nameStr : "");
					lua_pop(L, 2);
				}
				lua_pop(L, 1);
			}
		}
		lua_settop(L, top);

		if (lam2Running)
		{
			if (!outBusyReason)
				return false;
			string256 buf;
			xr_sprintf(buf, sizeof(buf), "check5_lam2_is_running%s", lam2Details);
			appendReason(buf);
			allFree = false;
		}

		bool actorEffectsBusy = false;
		lua_getglobal(L, "actor_effects");
		if (lua_istable(L, -1))
		{
			lua_getfield(L, -1, "item_not_in_use");
			if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
				actorEffectsBusy = true;
			lua_pop(L, 1);
		}
		lua_settop(L, top);

		if (actorEffectsBusy)
		{
			if (!outBusyReason)
				return false;
			appendReason("check5_actor_effects_item_not_in_use_false");
			allFree = false;
		}
	}

	return allFree;
}

bool CPauseTokenManager::HasActiveEatItemBeenConsumed() const
{
	if (!g_pGameLevel)
		return false;

	CObject* rawItem = Level().Objects.net_Find(m_activeStep.itemId);
	if (!rawItem || rawItem->getDestroy())
		return true;

	CActor* pActor = g_actor;
	CInventoryItem* pIItem = smart_cast<CInventoryItem*>(rawItem);
	CEatableItem* pEatable = smart_cast<CEatableItem*>(rawItem);
	if (!pIItem || !pEatable || pEatable->Empty())
		return true;

	if (!pActor || !pIItem->object().H_Parent() || pIItem->object().H_Parent()->ID() != pActor->ID())
		return true;

	if (m_bActiveEatUsesCondition && pEatable->GetRemainingUses() < m_activeEatInitialRemainingUses)
		return true;

	return false;
}

void CPauseTokenManager::ApplyPendingCollapsedEatBatch()
{
	if (m_bCollapsedBatchApplied || m_collapsedEatBatch.empty())
		return;

	m_bCollapsedBatchApplied = true;
	xr_vector<QueuedTxEntry> batch;
	batch.swap(m_collapsedEatBatch);

	Msg("* [Gamepad] CPauseTokenManager: applying %u collapsed consumable(s) of category \"%s\" without duplicate animation (rep_item=%u)",
	    static_cast<u32>(batch.size()),
	    m_activeStep.category.size() ? m_activeStep.category.c_str() : "other",
	    m_activeStep.itemId);

	for (const auto& collapsedEntry : batch)
	{
		ExecuteEatWithoutAnimNow(collapsedEntry, "queue_collapsed");
	}
}

void CPauseTokenManager::FlushTxQueue(LPCSTR reason)
{
	if (m_txQueue.empty() && m_runnerState == ETxRunnerState::Idle)
		return;

	if (!m_tokens.empty())
		return;

	if (Device.Paused())
	{
		Msg("* [Gamepad] CPauseTokenManager::FlushTxQueue deferred: engine still paused (reason=\"%s\" queued=%u)",
		    reason ? reason : "", QueuedTxCount());
		return;
	}

	Msg("* [Gamepad] CPauseTokenManager::FlushTxQueue: armed step-by-step OnFrame runner for %u queued operation(s) after \"%s\" (mode=%s, dwTimeContinual=%u)",
	    QueuedTxCount(),
	    reason ? reason : "release",
	    (g_gp_queue_consumable_anim == 0) ? "per_category" : "per_item",
	    Device.dwTimeContinual);

	if (ps_gp_telemetry && pInput)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_tx_queue_armed,%u,%u,%s,%s",
		           Device.dwTimeContinual,
		           QueuedTxCount(),
		           (g_gp_queue_consumable_anim == 0) ? "per_category" : "per_item",
		           reason ? reason : "release");
		pInput->WriteGamepadTelemetryLine(line);
	}
}

void CPauseTokenManager::AdvanceTxQueueOnFrame()
{
	if (!m_tokens.empty() || Device.Paused())
		return;

	if (!g_pGameLevel || !g_actor || !g_actor->g_Alive())
	{
		if (!m_txQueue.empty() || m_runnerState != ETxRunnerState::Idle)
			ClearTxQueue("actor_or_level_unavailable");
		return;
	}

	if (m_runnerState == ETxRunnerState::WaitingHandsFree)
	{
		++m_stepFramesElapsed;

		// C4.2c (R1): накапливаем время шага только на кадрах без паузы (!Device.Paused() && m_tokens.empty()).
		// Если между кадрами была пауза, m_stepLastUnpausedTick сбрасывается в OnFrame, поэтому длительность паузы не входит в таймаут.
		if (m_stepLastUnpausedTick != 0 && Device.dwTimeContinual > m_stepLastUnpausedTick)
		{
			const u32 dtMs = Device.dwTimeContinual - m_stepLastUnpausedTick;
			m_stepElapsedUnpausedMs += _min<u32>(dtMs, 250u);
		}
		m_stepLastUnpausedTick = Device.dwTimeContinual;

		// Если активный шаг — EatItem со свёрнутой пачкой той же категории: как только первый предмет реально съеден
		// (или по завершении анимации), применяем остальные предметы этой категории без повторной анимации.
		if (m_activeStep.type == EQueuedTxType::EatItem &&
		    !m_bCollapsedBatchApplied &&
		    !m_collapsedEatBatch.empty() &&
		    HasActiveEatItemBeenConsumed())
		{
			ApplyPendingCollapsedEatBatch();
		}

		// Если вслед за текущим EatItem в очереди ожидает ActivateSlot, сбрасываем lam2.cur_slot до того,
		// как lam2 перейдёт к show_items_in_hands, чтобы он не доставал старое оружие перед нашим ActivateSlot.
		if (m_activeStep.type == EQueuedTxType::EatItem &&
		    !m_txQueue.empty() &&
		    m_txQueue.front().type == EQueuedTxType::ActivateSlot)
		{
			ClearLam2SavedWeaponSlotForPendingActivate();
		}

		const bool handsFree = AreActorHandsFreeNow();
		if (handsFree)
			++m_handsFreeStableFrames;
		else
			m_handsFreeStableFrames = 0;

		const bool activateTargetIdle = (m_activeStep.type == EQueuedTxType::ActivateSlot &&
		                                 IsTargetSlotActiveAndIdle(m_activeStep.slotId));
		if (activateTargetIdle)
			++m_activateIdleStableFrames;
		else
			m_activateIdleStableFrames = 0;

		const u32 elapsedMs = m_stepElapsedUnpausedMs;
		const bool timedOut = (elapsedMs >= GP_TX_STEP_TIMEOUT_MS);
		const bool stepDoneByHandsFree = (m_stepFramesElapsed >= GP_TX_MIN_STEP_FRAMES &&
		                                  m_handsFreeStableFrames >= GP_TX_HANDS_FREE_STABLE_FRAMES);
		const bool stepDoneByActivateIdle = (m_activeStep.type == EQueuedTxType::ActivateSlot &&
		                                     m_stepFramesElapsed >= GP_TX_MIN_STEP_FRAMES &&
		                                     m_activateIdleStableFrames >= GP_TX_HANDS_FREE_STABLE_FRAMES);
		const bool stepDone = (stepDoneByHandsFree || stepDoneByActivateIdle);

		if (!stepDone && !timedOut)
		{
			if (!handsFree && elapsedMs >= m_stepLastBusyLogElapsedMs + 1000u)
			{
				m_stepLastBusyLogElapsedMs = (elapsedMs / 1000u) * 1000u;
				xr_string busyReason;
				AreActorHandsFreeNow(&busyReason);
				LPCSTR reasonStr = busyReason.empty() ? "unknown" : busyReason.c_str();

				Msg("* [Gamepad] CPauseTokenManager: waiting hands free (%u ms) after %s (id=%u, sec=%s) reason=%s",
				    elapsedMs,
				    (m_activeStep.type == EQueuedTxType::EatItem) ? "eat" : "activate",
				    (m_activeStep.type == EQueuedTxType::EatItem) ? m_activeStep.itemId : m_activeStep.slotId,
				    m_activeStep.section.size() ? m_activeStep.section.c_str() : "",
				    reasonStr);

				if (ps_gp_telemetry && pInput)
				{
					string512 line;
					xr_sprintf(line, sizeof(line), "gp_tx_hands_busy,%u,step_wait,%s,%u,%u,%s",
					           Device.dwTimeContinual,
					           (m_activeStep.type == EQueuedTxType::EatItem) ? "eat" : "activate",
					           (m_activeStep.type == EQueuedTxType::EatItem) ? m_activeStep.itemId : m_activeStep.slotId,
					           elapsedMs,
					           reasonStr);
					pInput->WriteGamepadTelemetryLine(line);
				}
			}
			return;
		}

		if (stepDoneByActivateIdle && !stepDoneByHandsFree)
		{
			xr_string busyReason;
			AreActorHandsFreeNow(&busyReason);
			LPCSTR reasonStr = busyReason.empty() ? "none" : busyReason.c_str();

			Msg("* [Gamepad] CPauseTokenManager: activate slot=%u (%s) completed as weapon is active and eIdle (%u ms), while handsFree=0 (other_busy=%s)",
			    m_activeStep.slotId,
			    m_activeStep.section.size() ? m_activeStep.section.c_str() : "",
			    elapsedMs,
			    reasonStr);

			if (ps_gp_telemetry && pInput)
			{
				string512 line;
				xr_sprintf(line, sizeof(line), "gp_tx_activate_done_other_busy,%u,%u,%u,%s",
				           Device.dwTimeContinual,
				           m_activeStep.slotId,
				           elapsedMs,
				           reasonStr);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}

		if (timedOut)
		{
			xr_string busyReason;
			AreActorHandsFreeNow(&busyReason);
			LPCSTR reasonStr = busyReason.empty() ? "unknown" : busyReason.c_str();

			Msg("! [Gamepad] CPauseTokenManager: step timeout (%u ms) waiting for hands free after %s (id=%u, sec=%s, reason=%s); advancing queue",
			    elapsedMs,
			    (m_activeStep.type == EQueuedTxType::EatItem) ? "eat" : "activate",
			    (m_activeStep.type == EQueuedTxType::EatItem) ? m_activeStep.itemId : m_activeStep.slotId,
			    m_activeStep.section.size() ? m_activeStep.section.c_str() : "",
			    reasonStr);

			if (ps_gp_telemetry && pInput)
			{
				string512 line;
				xr_sprintf(line, sizeof(line), "gp_tx_step_timeout,%u,%s,%u,%u,%s",
				           Device.dwTimeContinual,
				           (m_activeStep.type == EQueuedTxType::EatItem) ? "eat" : "activate",
				           (m_activeStep.type == EQueuedTxType::EatItem) ? m_activeStep.itemId : m_activeStep.slotId,
				           elapsedMs,
				           reasonStr);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}

		if (m_activeStep.type == EQueuedTxType::EatItem && !m_bCollapsedBatchApplied && !m_collapsedEatBatch.empty())
		{
			if (HasActiveEatItemBeenConsumed())
			{
				ApplyPendingCollapsedEatBatch();
			}
			else
			{
				Msg("! [Gamepad] CPauseTokenManager: representative eat item=%u (%s) was not consumed (skipped/blocked); dropping %u collapsed item(s) of category \"%s\"",
				    m_activeStep.itemId,
				    m_activeStep.section.c_str(),
				    static_cast<u32>(m_collapsedEatBatch.size()),
				    m_activeStep.category.size() ? m_activeStep.category.c_str() : "other");
				m_collapsedEatBatch.clear();
			}
		}

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_tx_step_done,%u,%s,%u,%u,%u",
			           Device.dwTimeContinual,
			           (m_activeStep.type == EQueuedTxType::EatItem) ? "eat" : "activate",
			           (m_activeStep.type == EQueuedTxType::EatItem) ? m_activeStep.itemId : m_activeStep.slotId,
			           elapsedMs,
			           QueuedTxCount());
			pInput->WriteGamepadTelemetryLine(line);
		}

		ResetActiveTxStep();
	}

	// В состоянии Idle: запускаем следующий шаг из очереди только когда руки свободны
	// (либо когда первый шаг — ActivateSlot слота, который уже активен и eIdle, либо по таймауту ожидания в Idle).
	if (m_txQueue.empty())
	{
		ResetIdleWaitTimer();
		return;
	}

	CollapseConsecutiveActivateStepsInQueue("idle_front");

	const bool frontIsAlreadyActiveIdleSlot = (m_txQueue.front().type == EQueuedTxType::ActivateSlot &&
	                                           IsTargetSlotActiveAndIdle(m_txQueue.front().slotId));

	if (!frontIsAlreadyActiveIdleSlot && !AreActorHandsFreeNow())
	{
		if (m_idleLastUnpausedTick != 0 && Device.dwTimeContinual > m_idleLastUnpausedTick)
		{
			const u32 dtMs = Device.dwTimeContinual - m_idleLastUnpausedTick;
			m_idleWaitElapsedUnpausedMs += _min<u32>(dtMs, 250u);
		}
		m_idleLastUnpausedTick = Device.dwTimeContinual;

		const QueuedTxEntry& pendingFront = m_txQueue.front();
		const u32 idleElapsedMs = m_idleWaitElapsedUnpausedMs;

		if (idleElapsedMs >= m_idleLastBusyLogElapsedMs + 1000u && idleElapsedMs < GP_TX_STEP_TIMEOUT_MS)
		{
			m_idleLastBusyLogElapsedMs = (idleElapsedMs / 1000u) * 1000u;
			xr_string busyReason;
			AreActorHandsFreeNow(&busyReason);
			LPCSTR reasonStr = busyReason.empty() ? "unknown" : busyReason.c_str();

			Msg("* [Gamepad] CPauseTokenManager: waiting hands free in Idle (%u ms) before queued %s (id=%u, sec=%s) reason=%s",
			    idleElapsedMs,
			    (pendingFront.type == EQueuedTxType::EatItem) ? "eat" : "activate",
			    (pendingFront.type == EQueuedTxType::EatItem) ? pendingFront.itemId : pendingFront.slotId,
			    pendingFront.section.size() ? pendingFront.section.c_str() : "",
			    reasonStr);

			if (ps_gp_telemetry && pInput)
			{
				string512 line;
				xr_sprintf(line, sizeof(line), "gp_tx_hands_busy,%u,idle_wait,%s,%u,%u,%s",
				           Device.dwTimeContinual,
				           (pendingFront.type == EQueuedTxType::EatItem) ? "eat" : "activate",
				           (pendingFront.type == EQueuedTxType::EatItem) ? pendingFront.itemId : pendingFront.slotId,
				           idleElapsedMs,
				           reasonStr);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}

		if (idleElapsedMs < GP_TX_STEP_TIMEOUT_MS)
			return;

		xr_string busyReason;
		AreActorHandsFreeNow(&busyReason);
		LPCSTR reasonStr = busyReason.empty() ? "unknown" : busyReason.c_str();

		Msg("! [Gamepad] CPauseTokenManager: idle wait timeout (%u ms) before queued %s (id=%u, sec=%s, reason=%s); forcing step start",
		    idleElapsedMs,
		    (pendingFront.type == EQueuedTxType::EatItem) ? "eat" : "activate",
		    (pendingFront.type == EQueuedTxType::EatItem) ? pendingFront.itemId : pendingFront.slotId,
		    pendingFront.section.size() ? pendingFront.section.c_str() : "",
		    reasonStr);

		if (ps_gp_telemetry && pInput)
		{
			string512 line;
			xr_sprintf(line, sizeof(line), "gp_tx_idle_timeout,%u,%s,%u,%u,%s",
			           Device.dwTimeContinual,
			           (pendingFront.type == EQueuedTxType::EatItem) ? "eat" : "activate",
			           (pendingFront.type == EQueuedTxType::EatItem) ? pendingFront.itemId : pendingFront.slotId,
			           idleElapsedMs,
			           reasonStr);
			pInput->WriteGamepadTelemetryLine(line);
		}

		ResetIdleWaitTimer();
	}
	else
	{
		ResetIdleWaitTimer();
	}

	while (!m_txQueue.empty())
	{
		QueuedTxEntry next = m_txQueue.front();
		m_txQueue.erase(m_txQueue.begin());

		if (next.type == EQueuedTxType::EatItem)
		{
			if (!next.category.size())
				next.category = ResolveConsumableCategory(next.section.c_str());

			u8 initialUses = 1;
			bool usesCond = false;
			if (CObject* rawObj = Level().Objects.net_Find(next.itemId))
			{
				if (CEatableItem* pEat = smart_cast<CEatableItem*>(rawObj))
				{
					usesCond = pEat->IsUsingCondition();
					initialUses = pEat->GetRemainingUses();
				}
			}

			if (!ExecuteEatNow(next, "queue_step"))
				continue;

			m_activeStep = next;
			m_runnerState = ETxRunnerState::WaitingHandsFree;
			m_stepElapsedUnpausedMs = 0;
			m_stepLastUnpausedTick = Device.dwTimeContinual;
			m_stepLastBusyLogElapsedMs = 0;
			m_stepFramesElapsed = 0;
			m_handsFreeStableFrames = 0;
			m_activateIdleStableFrames = 0;
			m_activeEatInitialRemainingUses = initialUses;
			m_bActiveEatUsesCondition = usesCond;
			m_bCollapsedBatchApplied = false;
			m_collapsedEatBatch.clear();

			// C4.2c (D33): при gp_queue_consumable_anim == per_category (0) собираем остальные шаги eat той же категории
			if (g_gp_queue_consumable_anim == 0 && next.category.size())
			{
				for (auto it = m_txQueue.begin(); it != m_txQueue.end();)
				{
					if (it->type == EQueuedTxType::EatItem)
					{
						shared_str itCat = it->category.size() ? it->category : ResolveConsumableCategory(it->section.c_str());
						if (itCat == next.category)
						{
							QueuedTxEntry collapsed = *it;
							collapsed.category = itCat;
							m_collapsedEatBatch.push_back(collapsed);
							it = m_txQueue.erase(it);
							continue;
						}
					}
					++it;
				}

				if (!m_collapsedEatBatch.empty())
				{
					Msg("* [Gamepad] CPauseTokenManager: collapsed %u additional eat step(s) of category \"%s\" into representative item=%u (%s)",
					    static_cast<u32>(m_collapsedEatBatch.size()),
					    next.category.c_str(),
					    next.itemId,
					    next.section.c_str());
				}

				// C4.2d: после изъятия свёрнутых eat-шагов соседние шаги activate одного слота могли стать подряд идущими
				CollapseConsecutiveActivateStepsInQueue("after_eat_collapse");
			}

			if (!m_txQueue.empty() && m_txQueue.front().type == EQueuedTxType::ActivateSlot)
			{
				ClearLam2SavedWeaponSlotForPendingActivate();
			}
			break;
		}
		else if (next.type == EQueuedTxType::ActivateSlot)
		{
			while (!m_txQueue.empty() &&
			       m_txQueue.front().type == EQueuedTxType::ActivateSlot &&
			       m_txQueue.front().slotId == next.slotId)
			{
				next = m_txQueue.front();
				m_txQueue.erase(m_txQueue.begin());
				Msg("* [Gamepad] gp_tx activate collapsed consecutive duplicate (step_pop): slot=%u item=%u (%s) remaining_queue=%u",
				    next.slotId, next.itemId, next.section.c_str(), QueuedTxCount());

				if (ps_gp_telemetry && pInput)
				{
					string256 line;
					xr_sprintf(line, sizeof(line), "gp_tx_activate_collapsed,%u,%u,%u,%s,step_pop,%u",
					           Device.dwTimeContinual,
					           next.itemId,
					           next.slotId,
					           next.section.c_str(),
					           QueuedTxCount());
					pInput->WriteGamepadTelemetryLine(line);
				}
			}

			const bool alreadyActiveAndIdle = IsTargetSlotActiveAndIdle(next.slotId);
			if (!ExecuteActivateNow(next, "queue_step"))
				continue;

			if (alreadyActiveAndIdle)
			{
				Msg("* [Gamepad] CPauseTokenManager: activate slot=%u item=%u (%s) already active and eIdle; step completed immediately (0 ms)",
				    next.slotId, next.itemId, next.section.c_str());

				if (ps_gp_telemetry && pInput)
				{
					string256 line;
					xr_sprintf(line, sizeof(line), "gp_tx_step_done,%u,activate,%u,0,%u",
					           Device.dwTimeContinual,
					           next.slotId,
					           QueuedTxCount());
					pInput->WriteGamepadTelemetryLine(line);
				}
				// C4.2d-N4: не стартуем следующий шаг в том же кадре — его возьмёт Idle-ветка со своей проверкой свободных рук.
				break;
			}

			m_activeStep = next;
			m_runnerState = ETxRunnerState::WaitingHandsFree;
			m_stepElapsedUnpausedMs = 0;
			m_stepLastUnpausedTick = Device.dwTimeContinual;
			m_stepLastBusyLogElapsedMs = 0;
			m_stepFramesElapsed = 0;
			m_handsFreeStableFrames = 0;
			m_activateIdleStableFrames = 0;
			break;
		}
	}
}

void CPauseTokenManager::OnExternalUnpause(BOOL bTimer, BOOL bSound, LPCSTR reason)
{
	if (m_tokens.empty())
		return;

	LPCSTR safeReason = (reason && reason[0]) ? reason : "external";

	// C4.2b: если внешняя пауза (например, Pause/Break -> m_bExtTimer=true) была активна до первого Acquire
	// и была снята во время жизни токена, она освобождает свой внешний +1 звука, снижая базовый уровень на 1,
	// тогда как звуковая пауза нашего токена остаётся активной относительно нового m_iBaseSoundCounter.
	if (m_bExtTimer && bTimer && !Device.Paused())
	{
		m_bExtTimer = false;
		if (bSound && m_iBaseSoundCounter > 1)
		{
			--m_iBaseSoundCounter;
		}
	}

	if (m_bOwnTimer && bTimer && !Device.Paused())
	{
		m_bOwnTimer = false;
		Msg("! [Gamepad] CPauseTokenManager: external unpause detected while %u token(s) active (dwTimeContinual=%u, reason=\"%s\"); own_timer cleared without re-freezing",
		    Count(), Device.dwTimeContinual, safeReason);

		if (ps_gp_telemetry && pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_pause_external_unfreeze,%u,%u,%s",
			           Device.dwTimeContinual, Count(), safeReason);
			pInput->WriteGamepadTelemetryLine(line);
		}
	}

	if (m_bOwnSound && bSound)
	{
		const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : 0;
		if (sndCounter <= m_iBaseSoundCounter)
		{
			m_bOwnSound = false;
			Msg("! [Gamepad] CPauseTokenManager: external sound unpause detected while %u token(s) active (dwTimeContinual=%u, reason=\"%s\", snd_pause_counter=%d, base_snd=%d); own_sound cleared",
			    Count(), Device.dwTimeContinual, safeReason, sndCounter, m_iBaseSoundCounter);

			if (ps_gp_telemetry && pInput)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_pause_external_sound_unpause,%u,%u,%d,%d,%s",
				           Device.dwTimeContinual, Count(), sndCounter, m_iBaseSoundCounter, safeReason);
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
	}
}

namespace
{
const char* TranslateEngineStringCp1251(const char* stringId, const char* fallbackCp1251)
{
	if (!stringId || !stringId[0] || !CStringTable::getPData())
		return fallbackCp1251;

	const shared_str translated = CStringTable().translate(stringId);
	if (!translated.size() || 0 == xr_strcmp(translated.c_str(), stringId))
		return fallbackCp1251;

	static thread_local char s_ring[16][512];
	static thread_local u32 s_ringIdx = 0;
	char* slot = s_ring[s_ringIdx++ & 15u];
	xr_strcpy(slot, 512, translated.c_str());
	return slot;
}

std::string TranslateEngineErrorUtf8(const char* stringId, const char* fallbackUtf8)
{
	if (!stringId || !stringId[0] || !CStringTable::getPData())
		return fallbackUtf8 ? std::string(fallbackUtf8) : std::string();

	const shared_str translated = CStringTable().translate(stringId);
	if (!translated.size() || 0 == xr_strcmp(translated.c_str(), stringId))
		return fallbackUtf8 ? std::string(fallbackUtf8) : std::string();

	return gposk::Cp1251ToUtf8(translated.c_str());
}

void EnsureEngineTranslateCallbacksRegistered()
{
	static bool s_translateRegistered = false;
	if (!s_translateRegistered)
	{
		CGamepadGlyphManager::SetTranslateCallback(&TranslateEngineStringCp1251);
		gperr::SetErrorTranslateUtf8Callback(&TranslateEngineErrorUtf8);
		gpbind::SetBindingTranslateCallback(&TranslateEngineStringCp1251);
		gposk::SetOskTranslateCallback(&TranslateEngineStringCp1251);
		gpwidgets::SetWidgetTranslateCallback(&TranslateEngineStringCp1251);
		s_translateRegistered = true;
	}
}
} // namespace

void CPauseTokenManager::EnsureInputSourceCallbackRegistered()
{
	EnsureEngineTranslateCallbacksRegistered();

	static bool s_sourceCallbackRegistered = false;
	if (!s_sourceCallbackRegistered && pInput)
	{
		pInput->AddInputSourceChangedCallback(&InputSourceChangedLuaBridge);
		s_sourceCallbackRegistered = true;
	}
}

void CPauseTokenManager::OnFrame()
{
	u32 sehCode = 0;
	u64 sehAddr = 0;
	const bool stepOk = gperr::ExecuteProtectedGamepadStepSEH(
	    +[](void* rawSelf)
	    {
		    CPauseTokenManager* self = static_cast<CPauseTokenManager*>(rawSelf);
		    if (!self)
			    return;

		    if (gperr::ConsumeSimulatedFaultMode() == gperr::ESimulatedFaultMode::ThrowCppExceptionInOnFrame)
		    {
			    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		    }

		    // D1.2 / D1.2b-N6: идемпотентная регистрация C++ колбэка смены источника ввода для gp_bridge
		    self->EnsureInputSourceCallbackRegistered();

		    // D5.2: синхронизация барьера токенов паузы для микшера вибрации
		    if (pInput && pInput->GetGamepadHaptics())
		    {
			    pInput->GetGamepadHaptics()->SetPauseTokenGate(!self->m_tokens.empty());
		    }

		    // D5.1: проверка актуальности набора глифов, раскладки D4.1 и источника ввода без перезапуска
		    {
			    auto& glyphs = CGamepadGlyphManager::Instance();
			    glyphs.EnsureInitialized();
			    // FR.1 №3: обновление глифов мгновенно при смене активного устройства или источника ввода,
			    // а периодический страховочный опрос хеша привязок D4.1 — не чаще раза в 50 мс вместо каждого кадра.
			    {
				    static u32 s_lastRefreshInstanceId = 0xFFFFFFFFu;
				    static int s_lastRefreshSource = -1;
				    static u32 s_lastRefreshTime = 0;
				    static bool s_everRefreshed = false;
				    const u32 curInstanceId = (pInput && pInput->GetGamepadDeviceManager())
				                                  ? pInput->GetGamepadDeviceManager()->GetActiveInstanceId()
				                                  : 0;
				    const int curRefreshSrc = pInput ? static_cast<int>(pInput->GetActiveInputSource()) : 0;
				    const u32 nowMs = Device.dwTimeContinual;
				    if (!s_everRefreshed || s_lastRefreshInstanceId != curInstanceId ||
				        s_lastRefreshSource != curRefreshSrc || nowMs - s_lastRefreshTime >= 50u)
				    {
					    s_everRefreshed = true;
					    s_lastRefreshInstanceId = curInstanceId;
					    s_lastRefreshSource = curRefreshSrc;
					    s_lastRefreshTime = nowMs;
					    glyphs.RefreshFromActiveDevice("on_frame");
				    }
			    }

			    static u32 s_lastGlyphRev = 0;
			    static int s_lastActiveSource = -1;
			    const u32 curRev = glyphs.GetPromptRevision();
			    const int curSrc = pInput ? static_cast<int>(pInput->GetActiveInputSource()) : 0;
			    if (s_lastGlyphRev != curRev || s_lastActiveSource != curSrc)
			    {
				    s_lastGlyphRev = curRev;
				    s_lastActiveSource = curSrc;
				    ApplyLiveGamepadPromptsRefresh();
			    }
		    }

		    // D3.1: по-кадровое обновление диспетчера контекстов (оценивается строго 1 раз за Device.dwFrame)
		    CGamepadContextDispatcher::Instance().OnFrame();

		    // F4.1: проверка таймаутов транзакций категории B по Device.dwTimeContinual (работает и на паузе, без автоповтора)
		    const u32 timedOutTxns = self->m_txnDispatcher.UpdateTimeouts(Device.dwFrame, Device.dwTimeContinual);
		    if (timedOutTxns > 0 && self->m_pendingBridgeOp.active)
		    {
			    const gptxn::TxnRecord* rec = self->m_txnDispatcher.FindRecordById(self->m_pendingBridgeOp.txnId);
			    if (!rec || rec->state == gptxn::TxnState::TimedOut)
			    {
				    Msg("! [Gamepad] gp_focus_test bridge_op timeout: txn=%u id=%u gen=%llu (unlocked without auto-retry)",
				        self->m_pendingBridgeOp.txnId,
				        self->m_pendingBridgeOp.stableRef.object_id,
				        self->m_pendingBridgeOp.stableRef.instance_gen);
				    self->m_pendingBridgeOp = {};
			    }
		    }

		    // E5.1 (D43) + F1.2: сброс масштаба времени в 1.0 и полное закрытие стека модалок + освобождение токенов при смерти актора
		    if (g_actor && !g_actor->g_Alive())
		    {
			    if (!self->m_timeFactorRequests.empty() || _abs(self->m_fExternalTimeFactor - 1.0f) > 1e-4f)
			    {
				    self->ResetAllTimeFactors("actor_dead_on_frame");
			    }
			    if (!self->m_modalTestCtrl.GetModalStack().IsEmpty() || !self->m_tokens.empty())
			    {
				    self->ReleaseAll("actor_dead_on_frame");
			    }
		    }

		    const bool bContinualTickAdvanced = (Device.dwTimeContinual != self->m_lastCheckTimeContinual);
		    self->m_lastCheckTimeContinual = Device.dwTimeContinual;

		    const auto& ctxDisp = CGamepadContextDispatcher::Instance();
		    const bool isQuickActionWheel = (ctxDisp.GetCurrentContext() == EGamepadContext::RadialWheel &&
		                                     ctxDisp.GetLuaHints().radialWheelType == CGamepadContextDispatcher::ERadialWheelType::QuickAction_Haru);
		    const bool eastJustPressed = ctxDisp.IsButtonPressed(CInput::GP_BTN_EAST);

		    const bool mainMenuActive = MainMenu() && MainMenu()->IsActive();
		    if (eastJustPressed && !mainMenuActive && !self->m_tokens.empty() && !isQuickActionWheel)
		    {
			    if (!self->m_modalTestCtrl.GetModalStack().IsEmpty())
			    {
				    self->CancelFocusTestTopModal("gamepad_B");
			    }
			    else
			    {
				    const u32 topBefore = self->TopTokenId();
				    if (CurrentGameUI() && CurrentGameUI()->TopInputReceiver())
				    {
					    CurrentGameUI()->TopInputReceiver()->HideDialog();
				    }
				    if (self->TopTokenId() == topBefore)
				    {
					    self->ReleaseTop("gamepad_B");
				    }
				    CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
			    }
		    }

		    // D3.2 (REC-3): инъекция дискретных событий геймпада через CLevel::IR_OnKeyboardPress/Release/Hold
		    CGamepadContextDispatcher::Instance().DispatchDiscreteActions();

		    if (!self->m_tokens.empty())
		    {
			    self->m_stepLastUnpausedTick = 0;
			    self->m_idleLastUnpausedTick = 0;
			    if (!bContinualTickAdvanced)
				    return;
			    if (self->m_bExtTimer && !Device.Paused())
			    {
				    self->m_bExtTimer = false;
			    }

			    if (self->m_bOwnTimer && !Device.Paused())
			    {
				    self->m_bOwnTimer = false;
				    Msg("! [Gamepad] CPauseTokenManager: external unpause detected while %u token(s) active (dwTimeContinual=%u, reason=\"on_frame\"); own_timer cleared without re-freezing",
				        self->Count(), Device.dwTimeContinual);

				    if (ps_gp_telemetry && pInput)
				    {
					    string256 line;
					    xr_sprintf(line, sizeof(line), "gp_pause_external_unfreeze,%u,%u,on_frame",
					               Device.dwTimeContinual, self->Count());
					    pInput->WriteGamepadTelemetryLine(line);
				    }
			    }

			    if (self->m_bOwnSound)
			    {
				    const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : 0;
				    if (sndCounter <= self->m_iBaseSoundCounter)
				    {
					    self->m_bOwnSound = false;
					    Msg("! [Gamepad] CPauseTokenManager: external sound unpause detected while %u token(s) active (dwTimeContinual=%u, reason=\"on_frame\", snd_pause_counter=%d, base_snd=%d); own_sound cleared",
					        self->Count(), Device.dwTimeContinual, sndCounter, self->m_iBaseSoundCounter);

					    if (ps_gp_telemetry && pInput)
					    {
						    string256 line;
						    xr_sprintf(line, sizeof(line), "gp_pause_external_sound_unpause,%u,%u,%d,%d,on_frame",
						               Device.dwTimeContinual, self->Count(), sndCounter, self->m_iBaseSoundCounter);
						    pInput->WriteGamepadTelemetryLine(line);
					    }
				    }
			    }
		    }
		    else if ((!self->m_txQueue.empty() || self->m_runnerState != ETxRunnerState::Idle) && !Device.Paused())
		    {
			    self->AdvanceTxQueueOnFrame();
		    }
		    else if (Device.Paused())
		    {
			    self->m_stepLastUnpausedTick = 0;
			    self->m_idleLastUnpausedTick = 0;
		    }
	    },
	    this,
	    sehCode,
	    sehAddr);

	if (!stepOk)
	{
		CGamepadContextDispatcher::Instance().ConsumeAllCurrentlyHeldButtons();
		const LPCSTR ctxName = CGamepadContextDispatcher::GetContextName(CGamepadContextDispatcher::Instance().GetCurrentContext());
		if (pInput)
		{
			pInput->HandleGamepadFrameError("CPauseTokenManager::OnFrame", nullptr, sehCode, sehAddr, ctxName);
		}
		else
		{
			gperr::HandleProtectedFrameFailure("CPauseTokenManager::OnFrame", nullptr, sehCode, sehAddr);
		}
	}
}

void CPauseTokenManager::LogState() const
{
	const int sndCounter = ::Sound ? ::Sound->pause_emitters_count() : -1;
	Msg("* [Gamepad] gp_pause_state: tokens=%u queued_tx=%u collapsed_batch=%u runner=%u anim_mode=%s own_timer=%d own_sound=%d ext_timer=%d base_snd=%d Device.Paused=%d last_snd_emitters=%d snd_pause_counter=%d bShowPauseString=%d top_id=%u top_tag=\"%s\"",
	    Count(),
	    QueuedTxCount(),
	    static_cast<u32>(m_collapsedEatBatch.size()),
	    static_cast<u32>(m_runnerState),
	    (g_gp_queue_consumable_anim == 0) ? "per_category" : "per_item",
	    m_bOwnTimer ? 1 : 0,
	    m_bOwnSound ? 1 : 0,
	    m_bExtTimer ? 1 : 0,
	    m_iBaseSoundCounter,
	    Device.Paused() ? 1 : 0,
	    Device.GetLastSndEmitters(),
	    sndCounter,
	    bShowPauseString ? 1 : 0,
	    TopTokenId(),
	    TopTokenTag());
}
