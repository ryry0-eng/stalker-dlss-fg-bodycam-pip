#pragma once

#include "GamepadFocusGraph.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gptxn
{
    // -------------------------------------------------------------------------
    // F4.1 (docs/10-pause-and-transactions.md §3–§6):
    // Диспетчер транзакций геймпада для UI-действий и операций с предметами.
    //
    // Четыре архитектурные категории операций:
    //   - Category A (SyncUI)           : мгновенные UI-операции (SP_fav, SP_junk,
    //                                     сортировка, вкладки фильтров, сворачивание).
    //                                     Завершаются синхронно в том же кадре (delta_frames = 0).
    //   - Category B (ServerClientItem) : серверно-клиентские транзакции предметов на паузе
    //                                     (GE_OWNERSHIP_TAKE/REJECT, GE_TRADE_BUY/SELL,
    //                                     GEG_PLAYER_ITEM2SLOT/RUCK/BELT, разборка/комбинирование).
    //                                     Блокируют повторный A на том же StableItemID в состоянии
    //                                     Pending до прихода GE_*/GEG_* или таймаута по
    //                                     Device.dwTimeContinual без автоповтора.
    //   - Category C (DeferredAnimQueue): отложенные анимированные действия (EatItem,
    //                                     ActivateSlot, FDDA), накапливаемые на паузе (D11/D33)
    //                                     и исполняемые после снятия паузы с повторной проверкой
    //                                     полного StableItemID (session_gen, object_id, instance_gen).
    //   - Category D (ContextChange)    : смена глобального контекста (UISleep, переход уровня,
    //                                     загрузка, главное меню). Немедленно отменяет все Pending
    //                                     транзакции B/C и взводит барьер отпускания.
    // -------------------------------------------------------------------------

    enum class TxnCategory : uint8_t
    {
        A_SyncUI            = 0,
        B_ServerClientItem  = 1,
        C_DeferredAnimQueue = 2,
        D_ContextChange     = 3
    };

    enum class TxnState : uint8_t
    {
        Idle      = 0,
        Pending   = 1,
        Confirmed = 2,
        Failed    = 3,
        Cancelled = 4,
        TimedOut  = 5
    };

    // Подмножество ожидаемых движковых событий для транзакций категории B/C
    // (соответствует константам xrMessages.h, но не требует включения тяжёлых заголовков в standalone-тесте)
    enum class ExpectedEventMask : uint32_t
    {
        None             = 0u,
        OwnershipTake    = 1u << 0, // GE_OWNERSHIP_TAKE (1), GE_OWNERSHIP_TAKE_MP_FORCED (2)
        OwnershipReject  = 1u << 1, // GE_OWNERSHIP_REJECT (3), GE_DESTROY_REJECT (12)
        TradeBuy         = 1u << 2, // GE_TRADE_BUY (19)
        TradeSell        = 1u << 3, // GE_TRADE_SELL (18), GEG_PLAYER_ITEM_SELL (39)
        ItemToSlot       = 1u << 4, // GEG_PLAYER_ITEM2SLOT (35)
        ItemToBelt       = 1u << 5, // GEG_PLAYER_ITEM2BELT (36)
        ItemToRuck       = 1u << 6, // GEG_PLAYER_ITEM2RUCK (37)
        ItemEat          = 1u << 7, // GEG_PLAYER_ITEM_EAT (38)
        AddonChange      = 1u << 8, // GE_ADDON_ATTACH (22), GE_ADDON_DETACH (23), GE_ADDON_CHANGE (24)
        AnyItemContainer = OwnershipTake | OwnershipReject | TradeBuy | TradeSell | ItemToSlot | ItemToBelt | ItemToRuck,
        AnyInventoryOp   = AnyItemContainer | ItemEat | AddonChange
    };

    constexpr ExpectedEventMask operator|(ExpectedEventMask a, ExpectedEventMask b) noexcept
    {
        return static_cast<ExpectedEventMask>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr ExpectedEventMask operator&(ExpectedEventMask a, ExpectedEventMask b) noexcept
    {
        return static_cast<ExpectedEventMask>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    constexpr bool HasEventFlag(ExpectedEventMask mask, ExpectedEventMask flag) noexcept
    {
        return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(flag)) != 0u;
    }

    struct TxnRecord
    {
        uint32_t             txn_id           = 0;
        TxnCategory          category         = TxnCategory::A_SyncUI;
        TxnState             state            = TxnState::Idle;
        gpfocus::StableItemID stable_id       = {};
        std::string          op_name;
        ExpectedEventMask    expected_events  = ExpectedEventMask::None;
        uint32_t             start_frame      = 0;
        uint32_t             done_frame       = 0;
        uint32_t             delta_frames     = 0;
        uint32_t             start_time_ms    = 0; // Device.dwTimeContinual на момент старта
        uint32_t             done_time_ms     = 0; // Device.dwTimeContinual на момент завершения
        uint32_t             timeout_ms       = 500;
        uint16_t             matched_ge_type  = 0xFFFFu;
        bool                 no_inv_update_slot = false; // слот обновлён через CActor::OnEvent без CInventory::Update()
        std::string          reason;
    };

    struct TxnBeginResult
    {
        bool     accepted          = false;
        bool     duplicate_blocked = false;
        uint32_t txn_id            = 0;
        TxnState state             = TxnState::Idle;
        std::string reason;
    };

    struct TxnStats
    {
        uint32_t total_started_a          = 0;
        uint32_t total_started_b          = 0;
        uint32_t total_started_c          = 0;
        uint32_t total_started_d          = 0;
        uint32_t total_confirmed          = 0;
        uint32_t total_failed             = 0;
        uint32_t total_cancelled          = 0;
        uint32_t total_timed_out          = 0;
        uint32_t duplicate_a_blocked      = 0;
        uint32_t stale_c_rejected         = 0;
        uint32_t slot_without_inv_update  = 0;
        uint32_t last_b_delta_frames      = 0;
        uint32_t last_confirmed_txn_id    = 0;
    };

    const char* TxnCategoryToString(TxnCategory cat) noexcept;
    const char* TxnStateToString(TxnState st) noexcept;
    ExpectedEventMask ClassifyEngineEventType(uint16_t geType) noexcept;

    class TransactionDispatcher
    {
    public:
        static constexpr uint32_t kDefaultCategoryBTimeoutMs = 500u;
        static constexpr size_t   kMaxHistoryRecords         = 32u;

        TransactionDispatcher() = default;

        void Clear() noexcept;

        // Категория A: мгновенный UI-вызов (SP_fav, SP_junk, сортировка, фильтр).
        // Сразу переходит Idle -> Pending -> Confirmed в том же кадре (delta_frames = 0).
        TxnBeginResult BeginCategoryA(
            const char* opName,
            const gpfocus::StableItemID& stableId,
            uint32_t currentFrame,
            uint32_t nowContinualMs);

        // Категория B: серверно-клиентская операция с предметом на паузе.
        // Если для stableId уже есть активная Pending-транзакция категории B —
        // блокирует повторное нажатие A (duplicate_blocked = true, accepted = false).
        TxnBeginResult BeginCategoryB(
            const char* opName,
            const gpfocus::StableItemID& stableId,
            ExpectedEventMask expectedEvents,
            uint32_t currentFrame,
            uint32_t nowContinualMs,
            uint32_t timeoutMs = kDefaultCategoryBTimeoutMs,
            bool slotWithoutInvUpdate = false);

        // Категория C: постановка отложенного анимированного действия в очередь на паузе.
        // Повторная постановка того же StableItemID или при заблокированном в B предмете отклоняется.
        TxnBeginResult BeginCategoryC(
            const char* opName,
            const gpfocus::StableItemID& stableId,
            ExpectedEventMask expectedEvents,
            uint32_t currentFrame,
            uint32_t nowContinualMs);

        // Повторная проверка StableItemID перед исполнением шага категории C после снятия паузы.
        // Если актуальный StableItemID не совпадает с сохранённым при постановке в очередь,
        // транзакция переводится в Cancelled ("err:stale_stable_id") и возвращается false.
        bool VerifyAndCommitCategoryC(
            uint32_t txnId,
            const gpfocus::StableItemID& actualCurrentStableId,
            uint32_t currentFrame,
            uint32_t nowContinualMs);

        // Категория D: смена глобального контекста (сон, переход уровня, главное меню).
        // Отменяет все Pending-транзакции B и C и фиксирует переход.
        TxnBeginResult BeginCategoryD(
            const char* opName,
            uint32_t currentFrame,
            uint32_t nowContinualMs);

        // Проверка: заблокирован ли предмет (StableItemID или object_id) активной Pending-транзакцией B.
        bool IsItemLocked(const gpfocus::StableItemID& stableId) const noexcept;
        bool IsObjectIdLockedInCategoryB(uint16_t objectId) const noexcept;
        const TxnRecord* FindPendingByItem(const gpfocus::StableItemID& stableId) const noexcept;
        const TxnRecord* FindRecordById(uint32_t txnId) const noexcept;

        // Обработка входящего события CActor::OnEvent (GE_OWNERSHIP_*, GE_TRADE_*, GEG_PLAYER_ITEM*).
        // Возвращает ID подтверждённой транзакции (или 0, если подходящей Pending-транзакции не было).
        uint32_t OnInventoryEvent(
            uint16_t geType,
            uint16_t itemId,
            uint32_t currentFrame,
            uint32_t nowContinualMs);

        // Принудительное подтверждение или отказ конкретной транзакции по ID
        bool ConfirmTransaction(
            uint32_t txnId,
            uint16_t geType,
            uint32_t currentFrame,
            uint32_t nowContinualMs,
            const char* reason = "ok:confirmed");

        bool FailOrCancelTransaction(
            uint32_t txnId,
            TxnState terminalState,
            uint32_t currentFrame,
            uint32_t nowContinualMs,
            const char* reason);

        // Обработка уничтожения объекта (net_Destroy) и смены сессии (session_gen):
        // немедленно отменяет все Pending-транзакции B/C для этого предмета/сессии.
        uint32_t OnObjectNetDestroy(uint16_t objectId, uint32_t currentFrame, uint32_t nowContinualMs);
        uint32_t OnSessionChanged(uint32_t newSessionGen, uint32_t currentFrame, uint32_t nowContinualMs);

        // Проверка таймаутов по Device.dwTimeContinual (работает и на паузе!).
        // Переводит просроченные Pending-транзакции B в TimedOut без автоповтора.
        // Возвращает количество сработавших таймаутов за этот вызов.
        uint32_t UpdateTimeouts(uint32_t currentFrame, uint32_t nowContinualMs);

        size_t CountPending() const noexcept;
        size_t CountPendingByCategory(TxnCategory cat) const noexcept;
        const TxnStats& GetStats() const noexcept { return m_stats; }
        const std::vector<TxnRecord>& GetRecords() const noexcept { return m_records; }

        std::string FormatDump() const;

    private:
        TxnRecord* FindRecordMutable(uint32_t txnId) noexcept;
        void AppendRecord(const TxnRecord& rec);

        uint32_t               m_next_txn_id = 1;
        std::vector<TxnRecord> m_records;
        TxnStats               m_stats;
    };

    // Встроенный автономный самотест F4.1 (вызывается из gp_txn_dump selftest и standalone-теста)
    bool RunTxnDispatcherSelfTest(std::string* outReport = nullptr);

} // namespace gptxn
