#pragma once

#include <array>
#include "GamepadFocusGraph.h"
#include "GamepadTxnDispatcher.h"
#include "GamepadModalStack.h"
#include "GamepadWidgets.h"

class CGameObject;
class CUIGamepadFocusTest;

// C4.1: прототип, настройки — Этап D
class CPauseTokenManager
{
public:
	struct TokenEntry
	{
		u32 id;
		shared_str tag;
	};

	// C5.1 + F1.1: идентификация предмета без изменения раскладки CObject (#pragma pack(4))
	using StableItemID = gpfocus::StableItemID;

	// C4.2: прототип очереди отложенных операций D11 до последнего Release
	enum class EQueuedTxType : u8
	{
		ActivateSlot = 0,
		EatItem = 1
	};

	// C4.2c: состояния пошагового автомата исполнения очереди в OnFrame
	enum class ETxRunnerState : u8
	{
		Idle = 0,
		WaitingHandsFree = 1
	};

	struct QueuedTxEntry
	{
		EQueuedTxType type;
		u16 itemId;
		u16 slotId;
		u16 ownerId;
		u16 prevPlaceType;
		u16 prevSlotId;
		StableItemID stableId;
		u32 txnId = 0;
		shared_str section;
		shared_str category;
		u32 enqueuedTimeContinual;
	};

	// F4.1 (D49, C5.1-N2): ожидающая подтверждения GEG_PLAYER_ITEM2* операция моста без синхронного дубля CInventory::Slot/Ruck
	struct PendingBridgeOp
	{
		bool active = false;
		u32 txnId = 0;
		u32 pressFrame = 0;
		u32 pressTimeContinual = 0;
		StableItemID stableRef = {};
		bool moveSlotToRuck = false;
		u16 targetSlot = 0xFFFF;
		shared_str sourceTag;
		shared_str itemSection;
		shared_str actionDir;
		shared_str bridgeResultStr;
	};

	static CPauseTokenManager& Instance();

	u32 Acquire(LPCSTR tag);
	bool Release(u32 id);
	bool ReleaseTop(LPCSTR reason = nullptr);
	void ReleaseAll(LPCSTR reason);

	u32 Count() const { return static_cast<u32>(m_tokens.size()); }
	bool OwnsTimer() const { return m_bOwnTimer; }
	bool OwnsSound() const { return m_bOwnSound; }
	int BaseSoundCounter() const { return m_iBaseSoundCounter; }
	u32 TopTokenId() const { return m_tokens.empty() ? 0 : m_tokens.back().id; }
	LPCSTR TopTokenTag() const { return m_tokens.empty() ? "" : m_tokens.back().tag.c_str(); }

	// C4.2b: синхронный учёт внешнего снятия паузы таймера и/или звука
	void OnExternalUnpause(BOOL bTimer, BOOL bSound, LPCSTR reason);

	// C4.2: операции A/B/C на паузе и очередь D11
	bool ExecuteOrQueueTransfer(u16 itemId, u16 toId);
	bool ExecuteOrQueueSlot(u16 itemId, u16 slotId);
	bool ExecuteOrQueueActivate(u16 slotId);
	bool ExecuteOrQueueEat(u16 itemId);
	u32 QueuedTxCount() const { return static_cast<u32>(m_txQueue.size()); }
	bool IsTxRunnerBusy() const { return m_runnerState != ETxRunnerState::Idle; }
	void ClearTxQueue(LPCSTR reason);

	// C5.1 + F1.1: жизненный цикл session_gen / instance_gen и разрешение StableItemID
	void OnObjectNetSpawn(u16 id);
	void OnObjectNetDestroy(u16 id);
	void ClearObjectInstanceGens();
	u32 GetSessionGen() const { return m_sessionGen; }
	u64 GetObjectInstanceGen(u16 id) const;
	StableItemID MakeStableItemID(u16 id) const;
	CGameObject* ResolveStableItem(const StableItemID& ref) const;

	// F1.1: граф фокуса, событийная пересборка из инвентаря актора и дамп gp_focus_dump
	gpfocus::FocusGraph& GetFocusGraph() { return m_focusGraph; }
	const gpfocus::FocusGraph& GetFocusGraph() const { return m_focusGraph; }
	void RebuildFocusGraphFromActorInventory(u8 tabId = 0);
	void DumpFocusGraph(LPCSTR args = nullptr);

	// C5.1 + F1.1 + F1.2: управление тестовым фокусным/модальным экраном CUIGamepadFocusTest и мостом в ui_inventory.GUI
	void OpenFocusTestWindow();
	void CloseFocusTestWindow();
	void ToggleFocusTestWindow();
	bool IsFocusTestWindowOpen() const;
	bool ExecuteFocusTestBridgeOp(LPCSTR sourceTag = nullptr);

	// F1.2: контроллер стека модальных окон (1..3 уровня), прокрутки по dwTimeContinual и удаления элементов списка
	gpmodal::FocusModalTestController& GetModalTestController() { return m_modalTestCtrl; }
	const gpmodal::FocusModalTestController& GetModalTestController() const { return m_modalTestCtrl; }
	void PopulateFocusTestItems(int count = gpmodal::FocusModalTestController::kDefaultListCount);
	bool OpenFocusTestModalToDepth(u32 targetDepth);
	bool CancelFocusTestTopModal(LPCSTR reason = "gp_ui_cancel");
	bool ConfirmFocusTestModalToRoot(LPCSTR reason = "l3_confirm");
	u32 EmergencyCloseFocusTestModalStack(LPCSTR reason = "emergency_reset");
	bool DeleteFocusTestItem(LPCSTR which);
	bool RunFocusTestModalSelfTest();
	void DumpFocusTestModalState();

	// F2.1: контроллер витрины базовых виджетов (TabStrip, ItemListGrid D10, ComparisonCard с Accurate Defense Values, QuantityModal, ConfirmDialog, FooterGlyphBar D5.1)
	gpwidgets::WidgetShowcaseController& GetWidgetShowcaseController() { return m_widgetShowcaseCtrl; }
	const gpwidgets::WidgetShowcaseController& GetWidgetShowcaseController() const { return m_widgetShowcaseCtrl; }
	bool IsFocusTestWidgetsMode() const { return m_bFocusTestWidgetsMode; }
	void SetFocusTestWidgetsMode(bool enabled);
	void PopulateWidgetsShowcaseItems();
	bool RunWidgetsShowcaseSelfTest();
	void DumpWidgetsShowcaseState();

	// F4.1: диспетчер транзакций A/B/C/D, защита от двойного A до GE_OWNERSHIP_*, слот без CInventory::Update() и фикс C5.1-N2
	gptxn::TransactionDispatcher& GetTxnDispatcher() { return m_txnDispatcher; }
	const gptxn::TransactionDispatcher& GetTxnDispatcher() const { return m_txnDispatcher; }
	void OnActorInventoryEvent(u16 eventType, u16 itemId);
	bool HasPendingBridgeOp() const { return m_pendingBridgeOp.active; }
	bool RunTxnSelfTest();
	void DumpTxnState(LPCSTR args = nullptr);

	// D1.2b-N6: явная регистрация C++->Lua колбэка смены источника ввода при старте приложения/скриптового движка
	void EnsureInputSourceCallbackRegistered();

	// E5.1 (D43): единый диспетчер заявок масштаба времени Device.time_factor
	struct TimeFactorEntry
	{
		shared_str tag;
		float factor = 1.0f;
	};

	float RequestTimeFactor(LPCSTR tag, float factor);
	float ReleaseTimeFactor(LPCSTR tag);
	void ResetAllTimeFactors(LPCSTR reason);
	void OnExternalTimeFactorCommand(float factor);
	float GetEffectiveTimeFactor() const;
	float GetExternalTimeFactor() const { return m_fExternalTimeFactor; }
	u32 TimeFactorRequestCount() const { return static_cast<u32>(m_timeFactorRequests.size()); }

	void OnFrame();
	void LogState() const;

private:
	CPauseTokenManager() = default;
	~CPauseTokenManager() = default;
	CPauseTokenManager(const CPauseTokenManager&) = delete;
	CPauseTokenManager& operator=(const CPauseTokenManager&) = delete;

	void ApplyEffectiveTimeFactor(LPCSTR reason);
	void RestoreEngineStateOnEmpty(LPCSTR reason);
	void FlushTxQueue(LPCSTR reason);
	void AdvanceTxQueueOnFrame();
	void ResetActiveTxStep();
	void ResetIdleWaitTimer();
	void CollapseConsecutiveActivateStepsInQueue(LPCSTR context);
	bool AreActorHandsFreeNow(xr_string* outBusyReason = nullptr) const;
	bool IsTargetSlotActiveAndIdle(u16 slotId) const;
	bool HasActiveEatItemBeenConsumed() const;
	void ApplyPendingCollapsedEatBatch();
	shared_str ResolveConsumableCategory(LPCSTR section) const;
	bool ExecuteActivateNow(const QueuedTxEntry& entry, LPCSTR source);
	bool ExecuteEatNow(const QueuedTxEntry& entry, LPCSTR source);
	bool ExecuteEatWithoutAnimNow(const QueuedTxEntry& entry, LPCSTR source);

	xr_vector<TokenEntry> m_tokens;
	xr_vector<QueuedTxEntry> m_txQueue;
	xr_vector<QueuedTxEntry> m_collapsedEatBatch;
	QueuedTxEntry m_activeStep = {};
	ETxRunnerState m_runnerState = ETxRunnerState::Idle;
	u32 m_stepElapsedUnpausedMs = 0;
	u32 m_stepLastUnpausedTick = 0;
	u32 m_stepLastBusyLogElapsedMs = 0;
	u32 m_stepFramesElapsed = 0;
	u32 m_handsFreeStableFrames = 0;
	u32 m_activateIdleStableFrames = 0;
	u32 m_idleWaitElapsedUnpausedMs = 0;
	u32 m_idleLastUnpausedTick = 0;
	u32 m_idleLastBusyLogElapsedMs = 0;
	u8 m_activeEatInitialRemainingUses = 0;
	bool m_bActiveEatUsesCondition = false;
	bool m_bCollapsedBatchApplied = false;
	u32 m_nextId = 1;
	bool m_bExtTimer = false;
	bool m_bOwnTimer = false;
	bool m_bOwnSound = false;
	int m_iBaseSoundCounter = 1;
	BOOL m_bSavedShowPauseString = TRUE;
	u32 m_lastCheckTimeContinual = u32(-1);

	// C5.1 + F1.1 + F1.2: таблица поколений объектов, поколение сессии, граф фокуса и контроллер стека модалок
	std::array<u64, 0x10000> m_objectInstanceGen{};
	u64 m_nextInstanceGen = 1;
	u32 m_sessionGen = 1;
	gpfocus::FocusGraph m_focusGraph;
	gptxn::TransactionDispatcher m_txnDispatcher;
	PendingBridgeOp m_pendingBridgeOp;
	gpmodal::FocusModalTestController m_modalTestCtrl;
	gpwidgets::WidgetShowcaseController m_widgetShowcaseCtrl;
	bool m_bFocusTestWidgetsMode = true;
	CUIGamepadFocusTest* m_pFocusTestWnd = nullptr;

	// E5.1 (D43): активные заявки масштаба времени и внешний масштаб (например, Bullet Time через time_factor)
	xr_vector<TimeFactorEntry> m_timeFactorRequests;
	float m_fExternalTimeFactor = 1.0f;
};
