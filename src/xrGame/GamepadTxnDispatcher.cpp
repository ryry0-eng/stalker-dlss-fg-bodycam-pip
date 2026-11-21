#include "GamepadTxnDispatcher.h"

#include <sstream>

namespace gptxn
{
    namespace
    {
        // Точные значения enum GE_* из src/xrServerEntities/xrMessages.h:98-170
        constexpr uint16_t kGE_OWNERSHIP_TAKE           = 1u;
        constexpr uint16_t kGE_OWNERSHIP_TAKE_MP_FORCED = 2u;
        constexpr uint16_t kGE_OWNERSHIP_REJECT         = 3u;
        constexpr uint16_t kGE_DESTROY_REJECT           = 9u;
        constexpr uint16_t kGE_TRADE_SELL               = 16u;
        constexpr uint16_t kGE_TRADE_BUY                = 17u;
        constexpr uint16_t kGE_ADDON_ATTACH             = 20u;
        constexpr uint16_t kGE_ADDON_DETACH             = 21u;
        constexpr uint16_t kGE_ADDON_CHANGE             = 22u;
        constexpr uint16_t kGEG_PLAYER_ITEM2SLOT        = 38u;
        constexpr uint16_t kGEG_PLAYER_ITEM2BELT        = 39u;
        constexpr uint16_t kGEG_PLAYER_ITEM2RUCK        = 40u;
        constexpr uint16_t kGEG_PLAYER_ITEM_EAT         = 41u;
        constexpr uint16_t kGEG_PLAYER_ITEM_SELL        = 42u;
    } // namespace

    const char* TxnCategoryToString(TxnCategory cat) noexcept
    {
        switch (cat)
        {
        case TxnCategory::A_SyncUI:            return "A_SyncUI";
        case TxnCategory::B_ServerClientItem:  return "B_ServerClientItem";
        case TxnCategory::C_DeferredAnimQueue: return "C_DeferredAnimQueue";
        case TxnCategory::D_ContextChange:     return "D_ContextChange";
        default:                               return "Unknown";
        }
    }

    const char* TxnStateToString(TxnState st) noexcept
    {
        switch (st)
        {
        case TxnState::Idle:      return "Idle";
        case TxnState::Pending:   return "Pending";
        case TxnState::Confirmed: return "Confirmed";
        case TxnState::Failed:    return "Failed";
        case TxnState::Cancelled: return "Cancelled";
        case TxnState::TimedOut:  return "TimedOut";
        default:                  return "Unknown";
        }
    }

    ExpectedEventMask ClassifyEngineEventType(uint16_t geType) noexcept
    {
        switch (geType)
        {
        case kGE_OWNERSHIP_TAKE:
        case kGE_OWNERSHIP_TAKE_MP_FORCED:
            return ExpectedEventMask::OwnershipTake;
        case kGE_OWNERSHIP_REJECT:
        case kGE_DESTROY_REJECT:
            return ExpectedEventMask::OwnershipReject;
        case kGE_TRADE_BUY:
            return ExpectedEventMask::TradeBuy;
        case kGE_TRADE_SELL:
        case kGEG_PLAYER_ITEM_SELL:
            return ExpectedEventMask::TradeSell;
        case kGEG_PLAYER_ITEM2SLOT:
            return ExpectedEventMask::ItemToSlot;
        case kGEG_PLAYER_ITEM2BELT:
            return ExpectedEventMask::ItemToBelt;
        case kGEG_PLAYER_ITEM2RUCK:
            return ExpectedEventMask::ItemToRuck;
        case kGEG_PLAYER_ITEM_EAT:
            return ExpectedEventMask::ItemEat;
        case kGE_ADDON_ATTACH:
        case kGE_ADDON_DETACH:
        case kGE_ADDON_CHANGE:
            return ExpectedEventMask::AddonChange;
        default:
            return ExpectedEventMask::None;
        }
    }

    void TransactionDispatcher::Clear() noexcept
    {
        m_next_txn_id = 1;
        m_records.clear();
        m_stats = {};
    }

    void TransactionDispatcher::AppendRecord(const TxnRecord& rec)
    {
        constexpr size_t kHardMaxPendingRecords = kMaxHistoryRecords * 4;
        if (m_records.size() >= kMaxHistoryRecords)
        {
            // Удаляем самую старую завершённую запись; активные Pending-блокировки сохраняем вплоть до жёсткого лимита
            bool removed = false;
            for (auto it = m_records.begin(); it != m_records.end(); ++it)
            {
                if (it->state != TxnState::Pending)
                {
                    m_records.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed && m_records.size() >= kHardMaxPendingRecords && !m_records.empty())
            {
                m_records.erase(m_records.begin());
            }
        }
        m_records.push_back(rec);
    }

    TxnRecord* TransactionDispatcher::FindRecordMutable(uint32_t txnId) noexcept
    {
        if (txnId == 0)
            return nullptr;
        for (TxnRecord& r : m_records)
        {
            if (r.txn_id == txnId)
                return &r;
        }
        return nullptr;
    }

    const TxnRecord* TransactionDispatcher::FindRecordById(uint32_t txnId) const noexcept
    {
        if (txnId == 0)
            return nullptr;
        for (const TxnRecord& r : m_records)
        {
            if (r.txn_id == txnId)
                return &r;
        }
        return nullptr;
    }

    const TxnRecord* TransactionDispatcher::FindPendingByItem(const gpfocus::StableItemID& stableId) const noexcept
    {
        if (!stableId.IsValid())
            return nullptr;
        for (const TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending
                && r.category == TxnCategory::B_ServerClientItem
                && r.stable_id == stableId)
            {
                return &r;
            }
        }
        return nullptr;
    }

    bool TransactionDispatcher::IsItemLocked(const gpfocus::StableItemID& stableId) const noexcept
    {
        return FindPendingByItem(stableId) != nullptr;
    }

    bool TransactionDispatcher::IsObjectIdLockedInCategoryB(uint16_t objectId) const noexcept
    {
        if (objectId == gpfocus::kInvalidObjId)
            return false;
        for (const TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending
                && r.category == TxnCategory::B_ServerClientItem
                && r.stable_id.object_id == objectId)
            {
                return true;
            }
        }
        return false;
    }

    TxnBeginResult TransactionDispatcher::BeginCategoryA(
        const char* opName,
        const gpfocus::StableItemID& stableId,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        TxnRecord rec;
        rec.txn_id          = m_next_txn_id++;
        rec.category        = TxnCategory::A_SyncUI;
        rec.state           = TxnState::Confirmed;
        rec.stable_id       = stableId;
        rec.op_name         = opName ? opName : "cat_a";
        rec.expected_events = ExpectedEventMask::None;
        rec.start_frame     = currentFrame;
        rec.done_frame      = currentFrame;
        rec.delta_frames    = 0;
        rec.start_time_ms   = nowContinualMs;
        rec.done_time_ms    = nowContinualMs;
        rec.timeout_ms      = 0;
        rec.reason          = "ok:sync_ui";

        AppendRecord(rec);
        ++m_stats.total_started_a;
        ++m_stats.total_confirmed;
        m_stats.last_confirmed_txn_id = rec.txn_id;

        TxnBeginResult res;
        res.accepted          = true;
        res.duplicate_blocked = false;
        res.txn_id            = rec.txn_id;
        res.state             = TxnState::Confirmed;
        res.reason            = rec.reason;
        return res;
    }

    TxnBeginResult TransactionDispatcher::BeginCategoryB(
        const char* opName,
        const gpfocus::StableItemID& stableId,
        ExpectedEventMask expectedEvents,
        uint32_t currentFrame,
        uint32_t nowContinualMs,
        uint32_t timeoutMs,
        bool slotWithoutInvUpdate)
    {
        TxnBeginResult res;
        if (!stableId.IsValid())
        {
            res.accepted          = false;
            res.duplicate_blocked = false;
            res.state             = TxnState::Failed;
            res.reason            = "err:invalid_stable_id";
            ++m_stats.total_failed;
            return res;
        }

        // Защита от двойного A до GE_OWNERSHIP_* / GEG_PLAYER_ITEM2*:
        // пока предмет заблокирован в Pending (или имеет тот же object_id в Pending B),
        // повторное нажатие A отклоняется без повторной отправки пакета.
        if (IsItemLocked(stableId) || IsObjectIdLockedInCategoryB(stableId.object_id))
        {
            ++m_stats.duplicate_a_blocked;
            res.accepted          = false;
            res.duplicate_blocked = true;
            res.state             = TxnState::Pending;
            res.reason            = "err:duplicate_a_blocked_while_pending";
            return res;
        }

        TxnRecord rec;
        rec.txn_id             = m_next_txn_id++;
        rec.category           = TxnCategory::B_ServerClientItem;
        rec.state              = TxnState::Pending;
        rec.stable_id          = stableId;
        rec.op_name            = opName ? opName : "cat_b";
        rec.expected_events    = (expectedEvents != ExpectedEventMask::None)
                                     ? expectedEvents
                                     : ExpectedEventMask::AnyItemContainer;
        rec.start_frame        = currentFrame;
        rec.done_frame         = 0;
        rec.delta_frames       = 0;
        rec.start_time_ms      = nowContinualMs;
        rec.done_time_ms       = 0;
        rec.timeout_ms         = (timeoutMs > 0) ? timeoutMs : kDefaultCategoryBTimeoutMs;
        rec.no_inv_update_slot = slotWithoutInvUpdate;
        rec.reason             = "pending:awaiting_ge";

        AppendRecord(rec);
        ++m_stats.total_started_b;

        res.accepted          = true;
        res.duplicate_blocked = false;
        res.txn_id            = rec.txn_id;
        res.state             = TxnState::Pending;
        res.reason            = rec.reason;
        return res;
    }

    TxnBeginResult TransactionDispatcher::BeginCategoryC(
        const char* opName,
        const gpfocus::StableItemID& stableId,
        ExpectedEventMask expectedEvents,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        TxnBeginResult res;
        if (!stableId.IsValid())
        {
            res.accepted          = false;
            res.duplicate_blocked = false;
            res.state             = TxnState::Failed;
            res.reason            = "err:invalid_stable_id";
            ++m_stats.total_failed;
            return res;
        }

        // Если предмет сейчас в Pending-транзакции B (например, выбрасывается или продаётся),
        // блокируем постановку в очередь C для того же предмета
        if (IsItemLocked(stableId) || IsObjectIdLockedInCategoryB(stableId.object_id))
        {
            ++m_stats.duplicate_a_blocked;
            res.accepted          = false;
            res.duplicate_blocked = true;
            res.state             = TxnState::Pending;
            res.reason            = "err:item_locked_in_category_b";
            return res;
        }

        TxnRecord rec;
        rec.txn_id          = m_next_txn_id++;
        rec.category        = TxnCategory::C_DeferredAnimQueue;
        rec.state           = TxnState::Pending;
        rec.stable_id       = stableId;
        rec.op_name         = opName ? opName : "cat_c";
        rec.expected_events = expectedEvents;
        rec.start_frame     = currentFrame;
        rec.done_frame      = 0;
        rec.delta_frames    = 0;
        rec.start_time_ms   = nowContinualMs;
        rec.done_time_ms    = 0;
        rec.timeout_ms      = 0; // очередь C ждёт снятия паузы, а не фиксированного таймаута B
        rec.reason          = "pending:deferred_until_unpause";

        AppendRecord(rec);
        ++m_stats.total_started_c;

        res.accepted          = true;
        res.duplicate_blocked = false;
        res.txn_id            = rec.txn_id;
        res.state             = TxnState::Pending;
        res.reason            = rec.reason;
        return res;
    }

    bool TransactionDispatcher::VerifyAndCommitCategoryC(
        uint32_t txnId,
        const gpfocus::StableItemID& actualCurrentStableId,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        TxnRecord* rec = FindRecordMutable(txnId);
        if (!rec || rec->state != TxnState::Pending || rec->category != TxnCategory::C_DeferredAnimQueue)
            return false;

        if (!actualCurrentStableId.IsValid() || rec->stable_id != actualCurrentStableId)
        {
            rec->state        = TxnState::Cancelled;
            rec->done_frame   = currentFrame;
            rec->delta_frames = (currentFrame >= rec->start_frame) ? (currentFrame - rec->start_frame) : 0;
            rec->done_time_ms = nowContinualMs;
            rec->reason       = "err:stale_stable_id";
            ++m_stats.total_cancelled;
            ++m_stats.stale_c_rejected;
            return false;
        }

        rec->state        = TxnState::Confirmed;
        rec->done_frame   = currentFrame;
        rec->delta_frames = (currentFrame >= rec->start_frame) ? (currentFrame - rec->start_frame) : 0;
        rec->done_time_ms = nowContinualMs;
        rec->reason       = "ok:deferred_executed";
        ++m_stats.total_confirmed;
        m_stats.last_confirmed_txn_id = rec->txn_id;
        return true;
    }

    TxnBeginResult TransactionDispatcher::BeginCategoryD(
        const char* opName,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        // Смена контекста (D) немедленно отменяет все незавершённые транзакции B и C
        for (TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending)
            {
                r.state        = TxnState::Cancelled;
                r.done_frame   = currentFrame;
                r.delta_frames = (currentFrame >= r.start_frame) ? (currentFrame - r.start_frame) : 0;
                r.done_time_ms = nowContinualMs;
                r.reason       = "cancelled:context_change_d";
                ++m_stats.total_cancelled;
            }
        }

        TxnRecord rec;
        rec.txn_id        = m_next_txn_id++;
        rec.category      = TxnCategory::D_ContextChange;
        rec.state         = TxnState::Confirmed;
        rec.op_name       = opName ? opName : "cat_d_context_change";
        rec.start_frame   = currentFrame;
        rec.done_frame    = currentFrame;
        rec.delta_frames  = 0;
        rec.start_time_ms = nowContinualMs;
        rec.done_time_ms  = nowContinualMs;
        rec.reason        = "ok:context_changed_barrier_engaged";

        AppendRecord(rec);
        ++m_stats.total_started_d;
        ++m_stats.total_confirmed;
        m_stats.last_confirmed_txn_id = rec.txn_id;

        TxnBeginResult res;
        res.accepted          = true;
        res.duplicate_blocked = false;
        res.txn_id            = rec.txn_id;
        res.state             = TxnState::Confirmed;
        res.reason            = rec.reason;
        return res;
    }

    uint32_t TransactionDispatcher::OnInventoryEvent(
        uint16_t geType,
        uint16_t itemId,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        const ExpectedEventMask eventFlag = ClassifyEngineEventType(geType);
        if (eventFlag == ExpectedEventMask::None)
            return 0;

        for (TxnRecord& r : m_records)
        {
            if (r.state != TxnState::Pending)
                continue;
            if (r.category != TxnCategory::B_ServerClientItem)
                continue;
            if (r.stable_id.object_id != itemId)
                continue;
            if (!HasEventFlag(r.expected_events, eventFlag))
                continue;

            r.state           = TxnState::Confirmed;
            r.done_frame      = currentFrame;
            r.delta_frames    = (currentFrame >= r.start_frame) ? (currentFrame - r.start_frame) : 0;
            r.done_time_ms    = nowContinualMs;
            r.matched_ge_type = geType;
            r.reason          = "ok:ge_confirmed";

            ++m_stats.total_confirmed;
            m_stats.last_b_delta_frames   = r.delta_frames;
            m_stats.last_confirmed_txn_id = r.txn_id;
            if (r.no_inv_update_slot && eventFlag == ExpectedEventMask::ItemToSlot)
            {
                ++m_stats.slot_without_inv_update;
            }
            return r.txn_id;
        }
        return 0;
    }

    bool TransactionDispatcher::ConfirmTransaction(
        uint32_t txnId,
        uint16_t geType,
        uint32_t currentFrame,
        uint32_t nowContinualMs,
        const char* reason)
    {
        TxnRecord* r = FindRecordMutable(txnId);
        if (!r || r->state != TxnState::Pending)
            return false;

        r->state           = TxnState::Confirmed;
        r->done_frame      = currentFrame;
        r->delta_frames    = (currentFrame >= r->start_frame) ? (currentFrame - r->start_frame) : 0;
        r->done_time_ms    = nowContinualMs;
        r->matched_ge_type = geType;
        r->reason          = reason ? reason : "ok:confirmed";

        ++m_stats.total_confirmed;
        if (r->category == TxnCategory::B_ServerClientItem)
        {
            m_stats.last_b_delta_frames = r->delta_frames;
            if (r->no_inv_update_slot)
                ++m_stats.slot_without_inv_update;
        }
        m_stats.last_confirmed_txn_id = r->txn_id;
        return true;
    }

    bool TransactionDispatcher::FailOrCancelTransaction(
        uint32_t txnId,
        TxnState terminalState,
        uint32_t currentFrame,
        uint32_t nowContinualMs,
        const char* reason)
    {
        TxnRecord* r = FindRecordMutable(txnId);
        if (!r || r->state != TxnState::Pending)
            return false;

        if (terminalState != TxnState::Failed
            && terminalState != TxnState::Cancelled
            && terminalState != TxnState::TimedOut)
        {
            terminalState = TxnState::Cancelled;
        }

        r->state        = terminalState;
        r->done_frame   = currentFrame;
        r->delta_frames = (currentFrame >= r->start_frame) ? (currentFrame - r->start_frame) : 0;
        r->done_time_ms = nowContinualMs;
        r->reason       = reason ? reason : "cancelled";

        if (terminalState == TxnState::Failed)
            ++m_stats.total_failed;
        else if (terminalState == TxnState::TimedOut)
            ++m_stats.total_timed_out;
        else
            ++m_stats.total_cancelled;

        return true;
    }

    uint32_t TransactionDispatcher::OnObjectNetDestroy(
        uint16_t objectId,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        uint32_t cancelledCount = 0;
        for (TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending && r.stable_id.object_id == objectId)
            {
                r.state        = TxnState::Cancelled;
                r.done_frame   = currentFrame;
                r.delta_frames = (currentFrame >= r.start_frame) ? (currentFrame - r.start_frame) : 0;
                r.done_time_ms = nowContinualMs;
                r.reason       = "cancelled:object_net_destroy";
                ++m_stats.total_cancelled;
                ++cancelledCount;
            }
        }
        return cancelledCount;
    }

    uint32_t TransactionDispatcher::OnSessionChanged(
        uint32_t newSessionGen,
        uint32_t currentFrame,
        uint32_t nowContinualMs)
    {
        uint32_t cancelledCount = 0;
        for (TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending && r.stable_id.session_gen != newSessionGen)
            {
                r.state        = TxnState::Cancelled;
                r.done_frame   = currentFrame;
                r.delta_frames = (currentFrame >= r.start_frame) ? (currentFrame - r.start_frame) : 0;
                r.done_time_ms = nowContinualMs;
                r.reason       = "cancelled:session_gen_changed";
                ++m_stats.total_cancelled;
                ++cancelledCount;
            }
        }
        return cancelledCount;
    }

    uint32_t TransactionDispatcher::UpdateTimeouts(uint32_t currentFrame, uint32_t nowContinualMs)
    {
        uint32_t timedOutCount = 0;
        for (TxnRecord& r : m_records)
        {
            if (r.state != TxnState::Pending)
                continue;
            if (r.category != TxnCategory::B_ServerClientItem)
                continue;
            if (r.timeout_ms == 0)
                continue;

            const uint32_t elapsed = (nowContinualMs >= r.start_time_ms)
                                         ? (nowContinualMs - r.start_time_ms)
                                         : 0u;
            if (elapsed >= r.timeout_ms)
            {
                // Таймаут снимает блокировку без автоповтора (docs/10-pause-and-transactions.md §4)
                r.state        = TxnState::TimedOut;
                r.done_frame   = currentFrame;
                r.delta_frames = (currentFrame >= r.start_frame) ? (currentFrame - r.start_frame) : 0;
                r.done_time_ms = nowContinualMs;
                r.reason       = "err:timeout_no_autoretry";
                ++m_stats.total_timed_out;
                ++timedOutCount;
            }
        }
        return timedOutCount;
    }

    size_t TransactionDispatcher::CountPending() const noexcept
    {
        size_t count = 0;
        for (const TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending)
                ++count;
        }
        return count;
    }

    size_t TransactionDispatcher::CountPendingByCategory(TxnCategory cat) const noexcept
    {
        size_t count = 0;
        for (const TxnRecord& r : m_records)
        {
            if (r.state == TxnState::Pending && r.category == cat)
                ++count;
        }
        return count;
    }

    std::string TransactionDispatcher::FormatDump() const
    {
        std::ostringstream oss;
        oss << "pending=" << CountPending()
            << " pending_b=" << CountPendingByCategory(TxnCategory::B_ServerClientItem)
            << " pending_c=" << CountPendingByCategory(TxnCategory::C_DeferredAnimQueue)
            << " started(A=" << m_stats.total_started_a
            << ",B=" << m_stats.total_started_b
            << ",C=" << m_stats.total_started_c
            << ",D=" << m_stats.total_started_d << ")"
            << " confirmed=" << m_stats.total_confirmed
            << " dup_a_blocked=" << m_stats.duplicate_a_blocked
            << " timed_out=" << m_stats.total_timed_out
            << " stale_c_rejected=" << m_stats.stale_c_rejected
            << " slot_no_inv_update=" << m_stats.slot_without_inv_update
            << " last_b_delta_frames=" << m_stats.last_b_delta_frames;
        return oss.str();
    }

    bool RunTxnDispatcherSelfTest(std::string* outReport)
    {
        TransactionDispatcher disp;
        std::ostringstream rep;
        bool ok = true;

        auto check = [&](bool cond, const char* label)
        {
            if (!cond)
            {
                ok = false;
                rep << "[FAIL:" << label << "] ";
            }
            else
            {
                rep << "[OK:" << label << "] ";
            }
        };

        // 1. Category A: мгновенный переход в Confirmed в том же кадре (delta_frames = 0)
        const gpfocus::StableItemID itemA{ 1u, 101u, 1001ull };
        TxnBeginResult resA = disp.BeginCategoryA("SP_fav", itemA, 500u, 10000u);
        check(resA.accepted && resA.state == TxnState::Confirmed, "CatA_SyncConfirm");

        // 2. Category B: нажатие A ставит предмет в Pending; повторное нажатие A до GE_* блокируется;
        //    на следующем кадре (+1) приходит GEG_PLAYER_ITEM2SLOT (38) без CInventory::Update()
        const gpfocus::StableItemID itemB{ 1u, 202u, 2002ull };
        TxnBeginResult resB1 = disp.BeginCategoryB(
            "Action_Equip:slot_no_inv_update",
            itemB,
            ExpectedEventMask::ItemToSlot | ExpectedEventMask::ItemToRuck,
            600u,
            12000u,
            500u,
            true);
        check(resB1.accepted && resB1.state == TxnState::Pending && disp.IsItemLocked(itemB), "CatB_PendingLock");

        // Повторное нажатие A в том же или следующем кадре до прихода GEG_PLAYER_ITEM2SLOT должно быть заблокировано
        TxnBeginResult resB2_dup = disp.BeginCategoryB(
            "Action_Equip:duplicate_A",
            itemB,
            ExpectedEventMask::ItemToSlot,
            600u,
            12005u,
            500u,
            true);
        check(!resB2_dup.accepted && resB2_dup.duplicate_blocked && disp.GetStats().duplicate_a_blocked == 1u, "CatB_DupABlocked");

        // Приход GEG_PLAYER_ITEM2SLOT (38) на следующем кадре (601 = press_frame + 1)
        uint32_t confirmedId = disp.OnInventoryEvent(38u, 202u, 601u, 12016u);
        const TxnRecord* recB = disp.FindRecordById(resB1.txn_id);
        check(confirmedId == resB1.txn_id
              && recB != nullptr
              && recB->state == TxnState::Confirmed
              && recB->delta_frames == 1u
              && !disp.IsItemLocked(itemB)
              && disp.GetStats().slot_without_inv_update == 1u,
              "CatB_ConfirmNextFrameDelta1");

        // 3. Category B Timeout: отсутствие события за 500 мс снимает блокировку без автоповтора
        const gpfocus::StableItemID itemTimeout{ 1u, 303u, 3003ull };
        TxnBeginResult resB_to = disp.BeginCategoryB(
            "Action_Drop",
            itemTimeout,
            ExpectedEventMask::OwnershipReject,
            700u,
            15000u,
            500u,
            false);
        check(resB_to.accepted && disp.IsItemLocked(itemTimeout), "CatB_TimeoutStart");
        uint32_t toCountEarly = disp.UpdateTimeouts(710u, 15400u);
        check(toCountEarly == 0u && disp.IsItemLocked(itemTimeout), "CatB_BeforeTimeoutStillLocked");
        uint32_t toCountFired = disp.UpdateTimeouts(730u, 15500u);
        const TxnRecord* recTo = disp.FindRecordById(resB_to.txn_id);
        check(toCountFired == 1u
              && recTo != nullptr
              && recTo->state == TxnState::TimedOut
              && !disp.IsItemLocked(itemTimeout),
              "CatB_TimeoutUnlockedNoRetry");

        // 4. Category C: отложенное действие + отклонение при подмене instance_gen (LIFO CID_Generator)
        const gpfocus::StableItemID itemC_orig{ 1u, 404u, 4004ull };
        const gpfocus::StableItemID itemC_reused{ 1u, 404u, 4005ull }; // тот же object_id=404, но другой instance_gen!
        TxnBeginResult resC = disp.BeginCategoryC("EatItem", itemC_orig, ExpectedEventMask::ItemEat, 800u, 16000u);
        check(resC.accepted && resC.state == TxnState::Pending, "CatC_Queued");
        bool cCommittedStale = disp.VerifyAndCommitCategoryC(resC.txn_id, itemC_reused, 850u, 16800u);
        check(!cCommittedStale && disp.GetStats().stale_c_rejected == 1u, "CatC_StaleInstanceGenRejected");

        // 5. Category D: смена контекста отменяет все активные Pending B/C
        const gpfocus::StableItemID itemPendingBeforeD{ 1u, 505u, 5005ull };
        TxnBeginResult resBeforeD = disp.BeginCategoryB(
            "TransferItem",
            itemPendingBeforeD,
            ExpectedEventMask::OwnershipTake,
            900u,
            18000u);
        check(resBeforeD.accepted && disp.CountPending() == 1u, "CatD_PrePending");
        TxnBeginResult resD = disp.BeginCategoryD("UISleep", 901u, 18016u);
        check(resD.accepted && disp.CountPending() == 0u && !disp.IsItemLocked(itemPendingBeforeD), "CatD_ClearsPending");

        rep << disp.FormatDump();
        if (outReport)
            *outReport = rep.str();
        return ok;
    }

} // namespace gptxn
