#include "GamepadModalStack.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace gpmodal
{
    // =========================================================================
    // 1. CallerFocusSnapshot — сохранение и возврат фокуса родительского окна
    // =========================================================================
    void CallerFocusSnapshot::CaptureFromGraph(
        const gpfocus::FocusGraph& graph,
        int32_t                    cur_scroll_top,
        float                      cur_scroll_px)
    {
        scroll_top = cur_scroll_top;
        scroll_px  = cur_scroll_px;

        const gpfocus::FocusGroup* active_group = graph.GetActiveGroup();
        const gpfocus::FocusNode*  focused_node = graph.GetFocusedNode();

        if (!active_group)
        {
            valid    = false;
            group_id = gpfocus::kInvalidGroupId;
            node_id  = gpfocus::kInvalidNodeId;
            stable_id = gpfocus::StableItemID{};
            key       = gpfocus::FocusItemKey{};
            return;
        }

        valid    = true;
        group_id = active_group->group_id;
        if (focused_node)
        {
            node_id   = focused_node->node_id;
            stable_id = focused_node->stable_id;
            key       = focused_node->key;
        }
        else
        {
            node_id   = active_group->current_node_id;
            stable_id = active_group->anchor_stable_id;
            key       = active_group->anchor_key;
        }
    }

    bool CallerFocusSnapshot::RestoreToGraph(gpfocus::FocusGraph& graph, uint32_t now_ms) const
    {
        if (!valid)
            return false;

        gpfocus::FocusGroup* g = graph.GetGroupMutable(group_id);
        if (!g)
        {
            // Если группа по какой-то причине исчезла, переключаемся на текущую активную
            return graph.GetFocusedNode() != nullptr;
        }

        graph.SwitchToGroup(group_id, now_ms);

        // 1. Если сохранённый узел всё ещё существует и (для предмета) имеет тот же StableItemID — ставим прямо на него
        if (const gpfocus::FocusNode* direct = g->FindNode(node_id))
        {
            if (!stable_id.IsValid() || direct->stable_id == stable_id)
            {
                return graph.SetFocusedNode(group_id, direct->node_id, true);
            }
        }

        // 2. Ищем по точному совпадению StableItemID среди всех узлов группы
        if (stable_id.IsValid())
        {
            for (const gpfocus::FocusNode& n : g->nodes)
            {
                if (n.stable_id == stable_id)
                {
                    return graph.SetFocusedNode(group_id, n.node_id, true);
                }
            }
        }

        // 3. Если вызывавший предмет был удалён/изменён внутри модального окна (например, подтвердили удаление на L3),
        //    восстанавливаем фокус по детерминированному 5-ступенчатому правилу F1.1 через якорь!
        //    Сбрасываем current_node_id перед RebuildGroupItems, чтобы старый фокус группы не перезаписал якорь снимка.
        g->current_node_id  = gpfocus::kInvalidNodeId;
        g->anchor_stable_id = stable_id;
        g->anchor_key       = key;
        g->has_anchor       = key.HasItemIdentity() || stable_id.IsValid();

        std::vector<gpfocus::FocusNode> current_items;
        current_items.reserve(g->nodes.size());
        for (const gpfocus::FocusNode& n : g->nodes)
        {
            if (n.kind == gpfocus::FocusNodeKind::Item)
                current_items.push_back(n);
        }

        graph.RebuildGroupItems(group_id, current_items, g->active_tab_id, now_ms);
        return graph.GetFocusedNode() != nullptr;
    }

    // =========================================================================
    // 2. ContinualRepeatTimer — автоповтор строго по Device.dwTimeContinual
    // =========================================================================
    void ContinualRepeatTimer::Reset() noexcept
    {
        m_active              = false;
        m_active_dir          = gpfocus::NavDir::Up;
        m_press_time_ms       = 0;
        m_next_repeat_time_ms = 0;
    }

    uint32_t ContinualRepeatTimer::Update(
        bool            has_dir,
        gpfocus::NavDir dir,
        uint32_t        now_continual_ms,
        bool*           out_is_repeat) noexcept
    {
        if (out_is_repeat)
            *out_is_repeat = false;

        if (!has_dir)
        {
            Reset();
            return 0;
        }

        if (!m_active || dir != m_active_dir)
        {
            m_active              = true;
            m_active_dir          = dir;
            m_press_time_ms       = now_continual_ms;
            m_next_repeat_time_ms = now_continual_ms + m_initial_delay_ms;
            ++m_total_triggers;
            if (out_is_repeat)
                *out_is_repeat = false;
            return 1;
        }

        // Знаковая разность для безопасной обработки переполнения uint32_t
        if (static_cast<int32_t>(now_continual_ms - m_next_repeat_time_ms) >= 0)
        {
            uint32_t steps = 0;
            while (static_cast<int32_t>(now_continual_ms - m_next_repeat_time_ms) >= 0)
            {
                m_next_repeat_time_ms += m_step_ms;
                ++steps;
                // Страховка от гигантского скачка времени (например, подвисание отладчика на несколько секунд)
                if (steps >= 8)
                {
                    m_next_repeat_time_ms = now_continual_ms + m_step_ms;
                    break;
                }
            }

            m_total_triggers  += steps;
            m_repeat_triggers += steps;
            if (out_is_repeat)
                *out_is_repeat = true;
            return steps;
        }

        return 0;
    }

    // =========================================================================
    // 3. ContinualSmoothScroller — плавная прокрутка строго по dwTimeContinual
    // =========================================================================
    void ContinualSmoothScroller::Configure(
        int32_t total_items,
        int32_t visible_rows,
        float   row_height_px) noexcept
    {
        m_total_items   = std::max<int32_t>(0, total_items);
        m_visible_rows  = std::max<int32_t>(1, visible_rows);
        m_row_height_px = (row_height_px > 1.0f) ? row_height_px : kDefaultRowHeightPx;
        ClampTarget();
    }

    void ContinualSmoothScroller::Reset(int32_t top_index) noexcept
    {
        m_target_top_index = top_index;
        ClampTarget();
        m_target_offset_px = static_cast<float>(m_target_top_index) * m_row_height_px;
        m_visual_offset_px = m_target_offset_px;
        m_has_last_step    = false;
    }

    void ContinualSmoothScroller::ClampTarget() noexcept
    {
        const int32_t max_top = std::max<int32_t>(0, m_total_items - m_visible_rows);
        if (m_target_top_index < 0)
            m_target_top_index = 0;
        if (m_target_top_index > max_top)
            m_target_top_index = max_top;

        const float max_offset_px = static_cast<float>(max_top) * m_row_height_px;
        if (m_target_offset_px < 0.0f)
            m_target_offset_px = 0.0f;
        if (m_target_offset_px > max_offset_px)
            m_target_offset_px = max_offset_px;

        if (m_visual_offset_px < 0.0f)
            m_visual_offset_px = 0.0f;
        if (m_visual_offset_px > max_offset_px)
            m_visual_offset_px = max_offset_px;
    }

    void ContinualSmoothScroller::EnsureFocusedVisible(int32_t focused_index) noexcept
    {
        if (m_total_items <= 0 || focused_index < 0)
        {
            m_target_top_index = 0;
            m_target_offset_px = 0.0f;
            ClampTarget();
            return;
        }

        const int32_t clamped_focus = std::min<int32_t>(focused_index, m_total_items - 1);
        if (clamped_focus < m_target_top_index)
        {
            m_target_top_index = clamped_focus;
        }
        else if (clamped_focus >= m_target_top_index + m_visible_rows)
        {
            m_target_top_index = clamped_focus - m_visible_rows + 1;
        }

        const int32_t max_top = std::max<int32_t>(0, m_total_items - m_visible_rows);
        m_target_top_index = std::clamp<int32_t>(m_target_top_index, 0, max_top);
        m_target_offset_px = static_cast<float>(m_target_top_index) * m_row_height_px;
    }

    void ContinualSmoothScroller::AddAnalogScrollVelocity(
        float    normalized_axis,
        float    rows_per_sec,
        uint32_t dt_continual_ms) noexcept
    {
        if (std::fabs(normalized_axis) <= 1e-4f || rows_per_sec <= 0.0f || dt_continual_ms == 0)
            return;

        const float dt_sec   = static_cast<float>(dt_continual_ms) * 0.001f;
        const float delta_px = normalized_axis * rows_per_sec * m_row_height_px * dt_sec;
        m_target_offset_px  += delta_px;

        const int32_t max_top       = std::max<int32_t>(0, m_total_items - m_visible_rows);
        const float   max_offset_px = static_cast<float>(max_top) * m_row_height_px;
        m_target_offset_px = std::clamp(m_target_offset_px, 0.0f, max_offset_px);
        m_target_top_index = std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(m_target_offset_px / m_row_height_px)),
            0,
            max_top);
    }

    void ContinualSmoothScroller::Step(uint32_t now_continual_ms) noexcept
    {
        if (!m_has_last_step)
        {
            m_has_last_step = true;
            m_last_step_ms  = now_continual_ms;
            return;
        }

        const int32_t signed_dt = static_cast<int32_t>(now_continual_ms - m_last_step_ms);
        m_last_step_ms = now_continual_ms;
        if (signed_dt <= 0)
            return;

        const float dt_sec = std::min<float>(static_cast<float>(signed_dt) * 0.001f, 0.25f);
        const float diff   = m_target_offset_px - m_visual_offset_px;
        if (std::fabs(diff) <= kDefaultSnapEpsPx)
        {
            m_visual_offset_px = m_target_offset_px;
            return;
        }

        // Независимое от FPS экспоненциальное приближение: alpha = 1 - exp(-k * dt_continual)
        const float alpha = 1.0f - std::exp(-kDefaultSmoothSpeed * dt_sec);
        m_visual_offset_px += diff * alpha;
        if (std::fabs(m_target_offset_px - m_visual_offset_px) <= kDefaultSnapEpsPx)
        {
            m_visual_offset_px = m_target_offset_px;
        }
    }

    int32_t ContinualSmoothScroller::GetCurrentTopIndex() const noexcept
    {
        if (m_row_height_px <= 0.0f)
            return 0;
        const int32_t max_top = std::max<int32_t>(0, m_total_items - m_visible_rows);
        const int32_t idx     = static_cast<int32_t>(std::floor((m_visual_offset_px + 0.5f) / m_row_height_px));
        return std::clamp<int32_t>(idx, 0, max_top);
    }

    float ContinualSmoothScroller::GetSubRowOffsetPx() const noexcept
    {
        const int32_t top_idx = GetCurrentTopIndex();
        return m_visual_offset_px - static_cast<float>(top_idx) * m_row_height_px;
    }

    bool ContinualSmoothScroller::IsAnimating() const noexcept
    {
        return std::fabs(m_target_offset_px - m_visual_offset_px) > kDefaultSnapEpsPx;
    }

    // =========================================================================
    // 4. ModalStack — стек модальных окон с барьером отпускания и возвратом фокуса
    // =========================================================================
    uint32_t ModalStack::PushModal(
        const std::string&   tag,
        const std::string&   title,
        gpfocus::FocusGraph& root_graph,
        uint32_t             pause_token_id,
        uint32_t             currently_held_buttons_mask,
        uint32_t             now_continual_ms,
        int32_t              parent_scroll_top,
        float                parent_scroll_px)
    {
        if (m_layers.size() >= kMaxModalDepth)
            return kInvalidModalId;

        ModalLayer layer;
        layer.modal_id       = m_next_modal_id++;
        if (m_next_modal_id == 0)
            m_next_modal_id = 1;
        layer.depth          = static_cast<uint32_t>(m_layers.size() + 1);
        layer.tag            = tag.empty() ? ("gp_modal_L" + std::to_string(layer.depth)) : tag;
        layer.title          = title;
        layer.pause_token_id = pause_token_id;
        layer.opened_at_ms   = now_continual_ms;

        // Сохраняем вызывающий узел из текущего активного графа (для depth=1 — из root_graph, для depth>=2 — из предыдущего слоя)
        const gpfocus::FocusGraph& parent_graph = GetActiveFocusGraph(root_graph);
        layer.caller_focus.CaptureFromGraph(parent_graph, parent_scroll_top, parent_scroll_px);

        m_layers.push_back(std::move(layer));

        // Взводим барьер consumed_until_release на все зажатые в момент открытия кнопки (A, B и др.),
        // чтобы кнопка A, открывшая модальное окно, не сработала повторно внутри нового окна до физического отпускания!
        m_raw_down_mask = currently_held_buttons_mask;
        ConsumeAllHeldButtons(currently_held_buttons_mask);

        ++m_push_count;
        m_last_event_time_ms = now_continual_ms;
        m_last_reason        = "push:" + m_layers.back().tag;

        return m_layers.back().modal_id;
    }

    uint32_t ModalStack::BeginFrameButtons(
        uint32_t raw_buttons_mask,
        uint32_t frame_id,
        uint32_t /*now_continual_ms*/) noexcept
    {
        m_raw_down_mask = raw_buttons_mask;

        // Снимаем барьер consumed_until_release только с тех кнопок, которые физически отпущены в этом кадре
        m_consumed_until_release_mask &= raw_buttons_mask;

        if (m_layers.empty())
        {
            m_effective_down_mask      = 0;
            m_prev_effective_down_mask = 0;
            m_pressed_mask             = 0;
            return 0;
        }

        // Если в этом же кадре уже произошёл переход стека (Push или Pop), новые нажатия в этом кадре блокируются
        if (m_transition_locked_frame == frame_id)
        {
            m_consumed_until_release_mask |= raw_buttons_mask;
            m_effective_down_mask      = 0;
            m_prev_effective_down_mask = 0;
            m_pressed_mask             = 0;
            return 0;
        }

        m_effective_down_mask      = raw_buttons_mask & ~m_consumed_until_release_mask;
        m_pressed_mask             = m_effective_down_mask & ~m_prev_effective_down_mask;
        m_prev_effective_down_mask = m_effective_down_mask;
        return m_pressed_mask;
    }

    bool ModalStack::PopTopModal(
        gpfocus::FocusGraph& root_graph,
        uint32_t             raw_buttons_mask,
        uint32_t             frame_id,
        uint32_t             now_continual_ms,
        const char*          reason,
        uint32_t*            out_released_pause_token,
        int32_t*             out_restored_scroll_top,
        float*               out_restored_scroll_px)
    {
        if (out_released_pause_token)
            *out_released_pause_token = 0;

        if (m_layers.empty())
            return false;

        // Защита от двойного закрытия в один и тот же кадр (стоп-условие F1.2: B никогда не закрывает > 1 уровня!)
        if (frame_id != 0xFFFFFFFFu && m_transition_locked_frame == frame_id)
            return false;

        ModalLayer popped = std::move(m_layers.back());
        m_layers.pop_back();

        if (out_released_pause_token)
            *out_released_pause_token = popped.pause_token_id;
        if (out_restored_scroll_top)
            *out_restored_scroll_top = popped.caller_focus.scroll_top;
        if (out_restored_scroll_px)
            *out_restored_scroll_px = popped.caller_focus.scroll_px;

        // Возвращаем фокус на вызвавший элемент в родительском окне (для закрытия L3 -> в L2, для закрытия L2 -> в L1 root_graph)
        gpfocus::FocusGraph& parent_graph = GetActiveFocusGraph(root_graph);
        popped.caller_focus.RestoreToGraph(parent_graph, now_continual_ms);

        // Взводим барьер consumed_until_release для кнопки B и всех физически удерживаемых кнопок,
        // а также блокируем повторные переходы стека до конца текущего frame_id!
        const uint32_t barrier_mask = raw_buttons_mask | ModalButtonMask(ModalButton::B);
        m_raw_down_mask = raw_buttons_mask;
        ConsumeAllHeldButtons(barrier_mask);
        m_transition_locked_frame = frame_id;

        ++m_pop_count;
        m_last_event_time_ms = now_continual_ms;
        m_last_reason        = std::string("pop:") + popped.tag + (reason && reason[0] ? (":" + std::string(reason)) : "");
        return true;
    }

    bool ModalStack::PopToDepth(
        size_t                 target_depth,
        gpfocus::FocusGraph&   root_graph,
        uint32_t               raw_buttons_mask,
        uint32_t               frame_id,
        uint32_t               now_continual_ms,
        const char*            reason,
        std::vector<uint32_t>* out_released_pause_tokens,
        int32_t*               out_restored_scroll_top,
        float*                 out_restored_scroll_px)
    {
        if (out_released_pause_tokens)
            out_released_pause_tokens->clear();

        if (m_layers.size() <= target_depth)
            return false;

        if (frame_id != 0xFFFFFFFFu && m_transition_locked_frame == frame_id)
            return false;

        std::string last_popped_tag;
        while (m_layers.size() > target_depth)
        {
            ModalLayer popped = std::move(m_layers.back());
            m_layers.pop_back();
            last_popped_tag = popped.tag;

            if (out_released_pause_tokens && popped.pause_token_id != 0)
                out_released_pause_tokens->push_back(popped.pause_token_id);
            if (out_restored_scroll_top)
                *out_restored_scroll_top = popped.caller_focus.scroll_top;
            if (out_restored_scroll_px)
                *out_restored_scroll_px = popped.caller_focus.scroll_px;

            gpfocus::FocusGraph& parent_graph = GetActiveFocusGraph(root_graph);
            popped.caller_focus.RestoreToGraph(parent_graph, now_continual_ms);
            ++m_pop_count;
        }

        const uint32_t barrier_mask = raw_buttons_mask | ModalButtonMask(ModalButton::A) | ModalButtonMask(ModalButton::B);
        m_raw_down_mask = raw_buttons_mask;
        ConsumeAllHeldButtons(barrier_mask);
        m_transition_locked_frame = frame_id;

        m_last_event_time_ms = now_continual_ms;
        m_last_reason        = std::string("pop_to_depth:") + last_popped_tag + (reason && reason[0] ? (":" + std::string(reason)) : "");
        return true;
    }

    size_t ModalStack::EmergencyClearAll(
        const char*            reason,
        uint32_t               raw_buttons_mask,
        uint32_t               now_continual_ms,
        std::vector<uint32_t>* out_released_pause_tokens)
    {
        if (out_released_pause_tokens)
            out_released_pause_tokens->clear();

        const size_t closed_count = m_layers.size();
        // Собираем токены паузы сверху вниз (LIFO) для освобождения
        for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it)
        {
            if (out_released_pause_tokens && it->pause_token_id != 0)
            {
                out_released_pause_tokens->push_back(it->pause_token_id);
            }
        }

        m_layers.clear();
        m_raw_down_mask = raw_buttons_mask;
        ConsumeAllHeldButtons(raw_buttons_mask | ModalButtonMask(ModalButton::A) | ModalButtonMask(ModalButton::B));
        m_transition_locked_frame = 0xFFFFFFFFu;

        if (closed_count > 0)
            ++m_emergency_clear_count;
        m_last_event_time_ms = now_continual_ms;
        m_last_reason        = std::string("emergency_clear:") + (reason && reason[0] ? reason : "unknown");

        return closed_count;
    }

    const ModalLayer* ModalStack::GetLayerAtDepth(size_t depth_1_based) const noexcept
    {
        if (depth_1_based == 0 || depth_1_based > m_layers.size())
            return nullptr;
        return &m_layers[depth_1_based - 1];
    }

    ModalLayer* ModalStack::GetLayerAtDepthMutable(size_t depth_1_based) noexcept
    {
        if (depth_1_based == 0 || depth_1_based > m_layers.size())
            return nullptr;
        return &m_layers[depth_1_based - 1];
    }

    gpfocus::FocusGraph& ModalStack::GetActiveFocusGraph(gpfocus::FocusGraph& root_graph) noexcept
    {
        if (m_layers.size() >= 2)
            return m_layers.back().local_graph;
        return root_graph;
    }

    const gpfocus::FocusGraph& ModalStack::GetActiveFocusGraph(const gpfocus::FocusGraph& root_graph) const noexcept
    {
        if (m_layers.size() >= 2)
            return m_layers.back().local_graph;
        return root_graph;
    }

    std::string ModalStack::GetTopDispatcherContextTag() const
    {
        if (m_layers.empty())
            return "";
        const ModalLayer& top = m_layers.back();
        return "GPModalStack:L" + std::to_string(top.depth) + ":" + top.tag + "#" + std::to_string(top.modal_id);
    }

    std::string ModalStack::FormatDump(const gpfocus::FocusGraph& root_graph) const
    {
        std::ostringstream oss;
        oss << "[gp_modal_stack] depth=" << m_layers.size()
            << " pushes=" << m_push_count
            << " pops=" << m_pop_count
            << " emergency_clears=" << m_emergency_clear_count
            << " raw_btns=0x" << std::hex << m_raw_down_mask
            << " barrier=0x" << m_consumed_until_release_mask
            << " eff=0x" << m_effective_down_mask << std::dec
            << " ctx_tag='" << GetTopDispatcherContextTag() << "'"
            << " last_reason='" << m_last_reason << "'\n";

        for (size_t i = 0; i < m_layers.size(); ++i)
        {
            const ModalLayer& l = m_layers[i];
            const bool is_top = (i + 1 == m_layers.size());
            const gpfocus::FocusGraph& g = (l.depth >= 2) ? l.local_graph : root_graph;
            const gpfocus::FocusNode* fn = g.GetFocusedNode();

            oss << "  " << (is_top ? "-> " : "   ")
                << "Layer[L" << l.depth << "] id=" << l.modal_id
                << " tag='" << l.tag << "'"
                << " title='" << l.title << "'"
                << " token=" << l.pause_token_id
                << " opened_ms=" << l.opened_at_ms
                << " caller_node=" << (l.caller_focus.node_id == gpfocus::kInvalidNodeId ? -1 : static_cast<int>(l.caller_focus.node_id))
                << " caller_sec='" << l.caller_focus.key.section << "'"
                << " focused_node=" << (fn ? static_cast<int>(fn->node_id) : -1)
                << " focused_label='" << (fn ? fn->label : "none") << "'\n";
        }
        return oss.str();
    }

    // =========================================================================
    // 5. FocusModalTestController — контроллер тестового экрана C5.1 / F1.1 / F1.2
    // =========================================================================
    FocusModalTestController::FocusModalTestController()
    {
        m_root_list_group_id  = m_root_graph.AddGroup("ActorRuckList", gpfocus::FocusLayoutMode::List1D, 1, false);
        m_root_equip_group_id = m_root_graph.AddGroup("ActorSlotsAndBelt", gpfocus::FocusLayoutMode::List1D, 2, false);
        m_scroller.Configure(0, kDefaultVisibleRows, ContinualSmoothScroller::kDefaultRowHeightPx);
    }

    void FocusModalTestController::PopulateSyntheticItems(int32_t count, uint32_t session_gen, uint32_t now_ms)
    {
        static const char* kSampleSections[] = {
            "bandage",
            "medkit",
            "medkit_army",
            "medkit_scientic",
            "antirad",
            "stimpack",
            "conserva",
            "kolbasa",
            "bread",
            "water_bottle",
            "energy_drink",
            "wpn_pm",
            "wpn_ak74",
            "ammo_5.45x39_fmj",
            "ammo_9x18_fmj",
            "detector_advanced",
            "af_medusa",
            "af_cristall",
            "swiss_knife",
            "toolkit_p"
        };
        constexpr size_t kNumSampleSections = sizeof(kSampleSections) / sizeof(kSampleSections[0]);

        const int32_t safe_count = std::max<int32_t>(0, count);
        m_items.clear();
        m_items.reserve(static_cast<size_t>(safe_count));

        for (int32_t i = 0; i < safe_count; ++i)
        {
            const char* sec = kSampleSections[static_cast<size_t>(i) % kNumSampleSections];
            const uint16_t obj_id = static_cast<uint16_t>(500 + i);

            gpfocus::FocusNode n;
            n.node_id                = static_cast<uint32_t>(i + 1);
            n.kind                   = gpfocus::FocusNodeKind::Item;
            n.stable_id.session_gen  = (session_gen != 0) ? session_gen : 1;
            n.stable_id.object_id    = obj_id;
            n.stable_id.instance_gen = static_cast<uint64_t>(i + 1);
            n.key.section            = sec;
            n.key.cond_permille      = static_cast<uint16_t>(1000 - (i % 5) * 40);
            n.key.uses_or_ammo       = static_cast<uint16_t>(1 + (i % 4));
            n.key.list_index         = i;
            n.key.cell_indx          = i;
            n.key.grid_x             = 0;
            n.key.grid_y             = static_cast<int16_t>(i);
            n.key.grid_w             = 1;
            n.key.grid_h             = 1;
            n.key.bag_id             = 1;
            n.key.tab_id             = 0;
            n.label                  = std::string(sec) + " #" + std::to_string(obj_id);
            m_items.push_back(std::move(n));
        }

        ReindexAndRebuildRoot(now_ms);
        m_scroller.Reset(0);
        SyncScrollerToRootFocus(now_ms);

        std::ostringstream oss;
        oss << "populated " << m_items.size() << " items";
        m_last_op_summary = oss.str();
    }

    void FocusModalTestController::SetItemsFromNodes(const std::vector<gpfocus::FocusNode>& nodes, uint32_t now_ms)
    {
        m_items = nodes;
        ReindexAndRebuildRoot(now_ms);
        SyncScrollerToRootFocus(now_ms);
    }

    void FocusModalTestController::ReindexAndRebuildRoot(uint32_t now_ms)
    {
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            m_items[i].node_id        = static_cast<uint32_t>(i + 1);
            m_items[i].key.list_index = static_cast<int32_t>(i);
            m_items[i].key.grid_x     = 0;
            m_items[i].key.grid_y     = static_cast<int16_t>(i);
            m_items[i].key.grid_w     = 1;
            m_items[i].key.grid_h     = 1;
        }

        m_last_delete_restore = m_root_graph.RebuildGroupItems(m_root_list_group_id, m_items, 0, now_ms);
        m_scroller.Configure(static_cast<int32_t>(m_items.size()), kDefaultVisibleRows, ContinualSmoothScroller::kDefaultRowHeightPx);
        SyncScrollerToRootFocus(now_ms);
    }

    void FocusModalTestController::SyncScrollerToRootFocus(uint32_t now_ms)
    {
        const gpfocus::FocusNode* fn = m_root_graph.GetGroupFocusedNode(m_root_list_group_id);
        if (fn && fn->kind == gpfocus::FocusNodeKind::Item && fn->key.list_index >= 0)
        {
            m_scroller.EnsureFocusedVisible(fn->key.list_index);
        }
        else
        {
            m_scroller.EnsureFocusedVisible(0);
        }
        m_scroller.Step(now_ms);
    }

    uint32_t FocusModalTestController::OpenRootModal(
        uint32_t pause_token_id,
        uint32_t held_buttons_mask,
        uint32_t now_ms)
    {
        if (!m_modal_stack.IsEmpty())
            return m_modal_stack.GetLayerAtDepth(1)->modal_id;

        if (m_items.empty())
            PopulateSyntheticItems(kDefaultListCount, 1, now_ms);

        const uint32_t id = m_modal_stack.PushModal(
            "gp_modal_L1_list",
            "Уровень 1: Список предметов (прокрутка и фокус F1.1)",
            m_root_graph,
            pause_token_id,
            held_buttons_mask,
            now_ms,
            m_scroller.GetTargetTopIndex(),
            m_scroller.GetVisualOffsetPx());

        m_last_op_summary = "opened L1 root list modal";
        return id;
    }

    uint32_t FocusModalTestController::OpenLevel2ActionModal(
        uint32_t               pause_token_id,
        uint32_t               held_buttons_mask,
        uint32_t               now_ms,
        std::vector<uint32_t>* out_released_tokens)
    {
        if (out_released_tokens)
            out_released_tokens->clear();

        if (m_modal_stack.IsEmpty())
        {
            OpenRootModal(0, held_buttons_mask, now_ms);
        }
        while (m_modal_stack.GetDepth() > 1)
        {
            uint32_t released_tok = 0;
            m_modal_stack.PopTopModal(m_root_graph, held_buttons_mask, 0xFFFFFFFFu, now_ms, "reset_to_L1", &released_tok);
            if (out_released_tokens && released_tok != 0)
                out_released_tokens->push_back(released_tok);
        }

        const gpfocus::FocusNode* caller_item = m_root_graph.GetFocusedNode();
        const std::string item_name = caller_item ? caller_item->label : "нет предмета";

        const uint32_t id = m_modal_stack.PushModal(
            "gp_modal_L2_actions",
            "Уровень 2: Действия [" + item_name + "]",
            m_root_graph,
            pause_token_id,
            held_buttons_mask,
            now_ms,
            m_scroller.GetTargetTopIndex(),
            m_scroller.GetVisualOffsetPx());

        ModalLayer* l2 = m_modal_stack.GetTopLayerMutable();
        if (l2)
        {
            const uint32_t g_act = l2->local_graph.AddGroup("ActionMenuL2", gpfocus::FocusLayoutMode::List1D, 10, false);
            std::vector<gpfocus::FocusNode> action_nodes;
            const char* kActions[4] = {
                "act_open_confirm_L3",
                "act_use_bridge",
                "act_delete_focused",
                "act_cancel_L2"
            };
            const char* kActionLabels[4] = {
                "1. Открыть подтверждение (Уровень 3)...",
                "2. Выполнить операцию моста (Slot <-> Ruck)",
                "3. Удалить выбранный предмет сразу",
                "4. Отмена (или кнопка B)"
            };
            for (int i = 0; i < 4; ++i)
            {
                gpfocus::FocusNode n;
                n.node_id                = static_cast<uint32_t>(i + 1);
                n.kind                   = gpfocus::FocusNodeKind::Item;
                n.stable_id.session_gen  = 1;
                n.stable_id.object_id    = static_cast<uint16_t>(2000 + i);
                n.stable_id.instance_gen = 1;
                n.key.section            = kActions[i];
                n.key.list_index         = i;
                n.key.cell_indx          = i;
                n.label                  = kActionLabels[i];
                action_nodes.push_back(std::move(n));
            }
            l2->local_graph.RebuildGroupItems(g_act, action_nodes, 0, now_ms);
            l2->local_graph.SetFocusedNode(g_act, 1, true);
        }

        m_last_op_summary = "opened L2 action menu over [" + item_name + "]";
        return id;
    }

    uint32_t FocusModalTestController::OpenLevel3ConfirmModal(
        uint32_t               pause_token_id,
        uint32_t               held_buttons_mask,
        uint32_t               now_ms,
        std::vector<uint32_t>* out_released_tokens)
    {
        if (out_released_tokens)
            out_released_tokens->clear();

        if (m_modal_stack.GetDepth() < 2)
        {
            OpenLevel2ActionModal(0, held_buttons_mask, now_ms, out_released_tokens);
        }
        while (m_modal_stack.GetDepth() > 2)
        {
            uint32_t released_tok = 0;
            m_modal_stack.PopTopModal(m_root_graph, held_buttons_mask, 0xFFFFFFFFu, now_ms, "reset_to_L2", &released_tok);
            if (out_released_tokens && released_tok != 0)
                out_released_tokens->push_back(released_tok);
        }

        const gpfocus::FocusNode* l2_caller = GetActiveGraph().GetFocusedNode();
        const std::string l2_label = l2_caller ? l2_caller->label : "действие";

        const uint32_t id = m_modal_stack.PushModal(
            "gp_modal_L3_confirm",
            "Уровень 3: Подтверждение [" + l2_label + "]",
            m_root_graph,
            pause_token_id,
            held_buttons_mask,
            now_ms,
            m_scroller.GetTargetTopIndex(),
            m_scroller.GetVisualOffsetPx());

        ModalLayer* l3 = m_modal_stack.GetTopLayerMutable();
        if (l3)
        {
            const uint32_t g_conf = l3->local_graph.AddGroup("ConfirmDialogL3", gpfocus::FocusLayoutMode::List1D, 20, false);
            std::vector<gpfocus::FocusNode> confirm_nodes;
            const char* kConfSecs[2] = { "confirm_yes_delete", "confirm_no_cancel" };
            const char* kConfLabels[2] = {
                "Да, подтвердить удаление предмета",
                "Нет, вернуться назад (или кнопка B)"
            };
            for (int i = 0; i < 2; ++i)
            {
                gpfocus::FocusNode n;
                n.node_id                = static_cast<uint32_t>(i + 1);
                n.kind                   = gpfocus::FocusNodeKind::Item;
                n.stable_id.session_gen  = 1;
                n.stable_id.object_id    = static_cast<uint16_t>(3000 + i);
                n.stable_id.instance_gen = 1;
                n.key.section            = kConfSecs[i];
                n.key.list_index         = i;
                n.key.cell_indx          = i;
                n.label                  = kConfLabels[i];
                confirm_nodes.push_back(std::move(n));
            }
            l3->local_graph.RebuildGroupItems(g_conf, confirm_nodes, 0, now_ms);
            l3->local_graph.SetFocusedNode(g_conf, 1, true);
        }

        m_last_op_summary = "opened L3 confirmation dialog";
        return id;
    }

    void FocusModalTestController::OpenToDepth(
        size_t                       target_depth,
        const std::vector<uint32_t>& token_ids,
        uint32_t                     held_buttons_mask,
        uint32_t                     now_ms)
    {
        const size_t clamped = std::min<size_t>(target_depth, 3);
        if (clamped >= 1 && m_modal_stack.GetDepth() < 1)
        {
            const uint32_t t1 = (token_ids.size() >= 1) ? token_ids[0] : 0;
            OpenRootModal(t1, held_buttons_mask, now_ms);
        }
        if (clamped >= 2 && m_modal_stack.GetDepth() < 2)
        {
            const uint32_t t2 = (token_ids.size() >= 2) ? token_ids[1] : 0;
            OpenLevel2ActionModal(t2, held_buttons_mask, now_ms);
        }
        if (clamped >= 3 && m_modal_stack.GetDepth() < 3)
        {
            const uint32_t t3 = (token_ids.size() >= 3) ? token_ids[2] : 0;
            OpenLevel3ConfirmModal(t3, held_buttons_mask, now_ms);
        }
    }

    bool FocusModalTestController::CancelTopModal(
        uint32_t  raw_buttons_mask,
        uint32_t  frame_id,
        uint32_t  now_ms,
        uint32_t* out_released_token)
    {
        const size_t depth_before = m_modal_stack.GetDepth();
        int32_t restored_top = m_scroller.GetTargetTopIndex();
        float   restored_px  = m_scroller.GetVisualOffsetPx();

        if (!m_modal_stack.PopTopModal(
                m_root_graph,
                raw_buttons_mask,
                frame_id,
                now_ms,
                "gp_ui_cancel_B",
                out_released_token,
                &restored_top,
                &restored_px))
        {
            return false;
        }

        if (m_modal_stack.GetDepth() == 1)
        {
            SyncScrollerToRootFocus(now_ms);
        }

        const gpfocus::FocusNode* restored_node = GetActiveGraph().GetFocusedNode();
        std::ostringstream oss;
        oss << "B closed L" << depth_before << " -> depth=" << m_modal_stack.GetDepth()
            << ", restored focus='" << (restored_node ? restored_node->label : "none") << "'";
        m_last_op_summary = oss.str();
        return true;
    }

    bool FocusModalTestController::ConfirmAndCloseToRoot(
        uint32_t               raw_buttons_mask,
        uint32_t               frame_id,
        uint32_t               now_ms,
        std::vector<uint32_t>* out_released_tokens)
    {
        const size_t depth_before = m_modal_stack.GetDepth();
        int32_t restored_top = m_scroller.GetTargetTopIndex();
        float   restored_px  = m_scroller.GetVisualOffsetPx();

        if (!m_modal_stack.PopToDepth(
                1,
                m_root_graph,
                raw_buttons_mask,
                frame_id,
                now_ms,
                "confirm_to_L1",
                out_released_tokens,
                &restored_top,
                &restored_px))
        {
            return false;
        }

        SyncScrollerToRootFocus(now_ms);
        const gpfocus::FocusNode* restored_node = m_root_graph.GetFocusedNode();
        std::ostringstream oss;
        oss << "confirmed L" << depth_before << " -> L1, restored focus='"
            << (restored_node ? restored_node->label : "none") << "'";
        m_last_op_summary = oss.str();
        return true;
    }

    size_t FocusModalTestController::EmergencyCloseAll(
        const char*            reason,
        uint32_t               raw_buttons_mask,
        uint32_t               now_ms,
        std::vector<uint32_t>* out_released_tokens)
    {
        const size_t n = m_modal_stack.EmergencyClearAll(reason, raw_buttons_mask, now_ms, out_released_tokens);
        m_repeat_timer.Reset();
        m_last_op_summary = std::string("emergency close all (") + (reason ? reason : "reset") + "): closed=" + std::to_string(n);
        return n;
    }

    bool FocusModalTestController::StepNavigation(gpfocus::NavDir dir, uint32_t now_ms)
    {
        gpfocus::FocusGraph& active_graph = GetActiveGraphMutable();
        const bool moved = active_graph.MoveFocus(dir, now_ms);
        if (moved && m_modal_stack.GetDepth() <= 1)
        {
            SyncScrollerToRootFocus(now_ms);
        }
        return moved;
    }

    uint32_t FocusModalTestController::StepNavigationWithRepeat(
        bool            has_dir,
        gpfocus::NavDir dir,
        uint32_t        now_ms,
        bool*           out_is_repeat)
    {
        const uint32_t steps = m_repeat_timer.Update(has_dir, dir, now_ms, out_is_repeat);
        uint32_t moved_steps = 0;
        for (uint32_t i = 0; i < steps; ++i)
        {
            if (StepNavigation(dir, now_ms))
                ++moved_steps;
        }
        return moved_steps;
    }

    void FocusModalTestController::StepContinualScroll(
        uint32_t now_ms,
        float    right_stick_y,
        uint32_t dt_continual_ms)
    {
        if (m_modal_stack.GetDepth() <= 1 && std::fabs(right_stick_y) > 0.15f)
        {
            m_scroller.AddAnalogScrollVelocity(right_stick_y, 8.0f, dt_continual_ms);
        }
        m_scroller.Step(now_ms);
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteItemAtListIndex(
        size_t   idx,
        uint32_t now_ms,
        bool     focus_target_before_delete)
    {
        if (m_items.empty() || idx >= m_items.size())
            return gpfocus::FocusRestoreReason::None;

        m_root_graph.SwitchToGroup(m_root_list_group_id, now_ms);
        if (focus_target_before_delete)
        {
            // Фокусируем удаляемый предмет перед его удалением, чтобы проверить правило восстановления F1.1 именно с него
            const uint32_t target_node_id = m_items[idx].node_id;
            m_root_graph.SetFocusedNode(m_root_list_group_id, target_node_id, true);
        }

        const std::string deleted_label = m_items[idx].label;
        m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(idx));

        // Пересобираем узлы (с новыми уникальными секциями при проверке соседства или сохранением текущих)
        ReindexAndRebuildRoot(now_ms);

        const gpfocus::FocusNode* after_node = m_root_graph.GetFocusedNode();
        std::ostringstream oss;
        oss << "deleted['" << deleted_label << "' at idx=" << idx << "] -> reason="
            << gpfocus::FocusRestoreReasonToString(m_last_delete_restore)
            << ", now='" << (after_node ? after_node->label : "none")
            << "' (remaining=" << m_items.size() << ")";
        m_last_op_summary = oss.str();
        return m_last_delete_restore;
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteFirstItem(uint32_t now_ms)
    {
        if (m_items.empty())
            PopulateSyntheticItems(kDefaultListCount, 1, now_ms);
        return DeleteItemAtListIndex(0, now_ms);
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteMiddleItem(uint32_t now_ms)
    {
        if (m_items.empty())
            PopulateSyntheticItems(kDefaultListCount, 1, now_ms);
        return DeleteItemAtListIndex(m_items.size() / 2, now_ms);
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteLastItem(uint32_t now_ms)
    {
        if (m_items.empty())
            PopulateSyntheticItems(kDefaultListCount, 1, now_ms);
        return DeleteItemAtListIndex(m_items.size() - 1, now_ms);
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteSingleItem(uint32_t now_ms)
    {
        if (m_items.empty())
        {
            PopulateSyntheticItems(1, 1, now_ms);
        }
        else if (m_items.size() > 1)
        {
            // Оставляем ровно 1 предмет, чтобы гарантированно протестировать переход 1 -> 0 (EmptyStatePlaceholder)
            m_items.resize(1);
            ReindexAndRebuildRoot(now_ms);
        }
        return DeleteItemAtListIndex(0, now_ms);
    }

    gpfocus::FocusRestoreReason FocusModalTestController::DeleteFocusedItem(uint32_t now_ms)
    {
        const gpfocus::FocusNode* fn = m_root_graph.GetGroupFocusedNode(m_root_list_group_id);
        if (!fn || fn->kind != gpfocus::FocusNodeKind::Item || fn->key.list_index < 0)
            return gpfocus::FocusRestoreReason::None;

        return DeleteItemAtListIndex(static_cast<size_t>(fn->key.list_index), now_ms);
    }

    std::string FocusModalTestController::FormatStatusDump() const
    {
        std::ostringstream oss;
        const gpfocus::FocusNode* root_fn   = m_root_graph.GetFocusedNode();
        const gpfocus::FocusNode* active_fn = GetActiveGraph().GetFocusedNode();

        oss << "[gp_focus_test modal_dump] depth=" << m_modal_stack.GetDepth()
            << " items=" << m_items.size()
            << " scroll_top=" << m_scroller.GetCurrentTopIndex() << "/" << m_scroller.GetTargetTopIndex()
            << " scroll_px=" << m_scroller.GetVisualOffsetPx() << "/" << m_scroller.GetTargetOffsetPx()
            << " last_restore=" << gpfocus::FocusRestoreReasonToString(m_last_delete_restore)
            << " root_focus='" << (root_fn ? root_fn->label : "none") << "'"
            << " active_focus='" << (active_fn ? active_fn->label : "none") << "'"
            << " last_op=\"" << m_last_op_summary << "\"\n";
        oss << m_modal_stack.FormatDump(m_root_graph);
        return oss.str();
    }

    // =========================================================================
    // 6. Встроенный самотест F1.2 (RunModalStackSelfTest)
    // =========================================================================
    gpfocus::SelfTestSummary RunModalStackSelfTest()
    {
        gpfocus::SelfTestSummary summary;
        std::ostringstream log;

        auto check = [&](const char* name, bool cond)
        {
            ++summary.total_cases;
            if (cond)
            {
                ++summary.passed_cases;
            }
            else
            {
                ++summary.failed_cases;
                log << "  [FAIL] " << name << "\n";
            }
        };

        // 1. Вложенность модальных окон 1 -> 2 -> 3, закрытие по одному кнопкой B и возврат фокуса на вызвавший узел
        {
            FocusModalTestController ctrl;
            ctrl.PopulateSyntheticItems(12, 1, 1000);
            ctrl.OpenRootModal(101, 0, 1010);
            check("L1_open_depth_1", ctrl.GetModalStack().GetDepth() == 1);

            // Выбираем 4-й предмет (индекс 3, node_id 4) в корневом списке
            ctrl.GetRootGraphMutable().SetFocusedNode(0, 4, true);
            const uint16_t caller_L1_obj = ctrl.GetRootGraph().GetFocusedNode()->stable_id.object_id;

            // Нажимаем A для открытия модалки L2: кнопка A зажата (ModalButtonMask(A))
            ctrl.OpenLevel2ActionModal(102, ModalButtonMask(ModalButton::A), 1020);
            check("L2_open_depth_2", ctrl.GetModalStack().GetDepth() == 2);
            check("L2_A_barrier_armed", ctrl.GetModalStack().IsButtonConsumedByBarrier(ModalButton::A));

            // Переводим фокус внутри L2 на 3-й пункт меню (node_id 3)
            ctrl.GetActiveGraphMutable().SetFocusedNode(0, 3, true);
            const uint32_t caller_L2_node = ctrl.GetActiveGraph().GetFocusedNode()->node_id;

            // Открываем модалку L3 поверх L2
            ctrl.OpenLevel3ConfirmModal(103, ModalButtonMask(ModalButton::A), 1030);
            check("L3_open_depth_3", ctrl.GetModalStack().GetDepth() == 3);

            // Нажимаем B на кадре 50: закрывается СТРОГО ТОЛЬКО L3 (глубина становится 2), фокус в L2 вернулся на node_id 3!
            uint32_t rel_tok = 0;
            bool popped_L3 = ctrl.CancelTopModal(ModalButtonMask(ModalButton::B), 50, 1040, &rel_tok);
            check("B_closes_only_L3_to_L2",
                popped_L3 && ctrl.GetModalStack().GetDepth() == 2 && rel_tok == 103);
            check("B_restores_L2_caller_focus",
                ctrl.GetActiveGraph().GetFocusedNode() != nullptr &&
                ctrl.GetActiveGraph().GetFocusedNode()->node_id == caller_L2_node);

            // Попытка закрыть ещё одно окно в ТОТ ЖЕ кадр 50 или при удерживаемой кнопке B блокируется барьером!
            bool same_frame_pop = ctrl.CancelTopModal(ModalButtonMask(ModalButton::B), 50, 1040, &rel_tok);
            check("B_same_frame_double_pop_blocked", !same_frame_pop && ctrl.GetModalStack().GetDepth() == 2);

            uint32_t pressed_while_B_held = ctrl.GetModalStackMutable().BeginFrameButtons(
                ModalButtonMask(ModalButton::B), 51, 1056);
            check("B_held_next_frame_suppressed_by_barrier",
                (pressed_while_B_held & ModalButtonMask(ModalButton::B)) == 0 &&
                !ctrl.GetModalStack().IsButtonPressedInTopModal(ModalButton::B));

            // Физически отпускаем B на кадре 52, затем снова нажимаем B на кадре 53 -> закрывается L2 (глубина 1), фокус в L1 на предмете caller_L1_obj!
            ctrl.GetModalStackMutable().BeginFrameButtons(0, 52, 1072);
            ctrl.GetModalStackMutable().BeginFrameButtons(ModalButtonMask(ModalButton::B), 53, 1088);
            bool popped_L2 = ctrl.CancelTopModal(ModalButtonMask(ModalButton::B), 53, 1088, &rel_tok);
            check("B_closes_L2_to_L1",
                popped_L2 && ctrl.GetModalStack().GetDepth() == 1 && rel_tok == 102);
            check("B_restores_L1_caller_focus",
                ctrl.GetActiveGraph().GetFocusedNode() != nullptr &&
                ctrl.GetActiveGraph().GetFocusedNode()->stable_id.object_id == caller_L1_obj);

            // Отпускаем B на кадре 54 и нажимаем B на кадре 55 -> закрывается L1 (стек пуст, токен 101 освобождён)
            ctrl.GetModalStackMutable().BeginFrameButtons(0, 54, 1104);
            bool popped_L1 = ctrl.CancelTopModal(ModalButtonMask(ModalButton::B), 55, 1120, &rel_tok);
            check("B_closes_L1_to_empty",
                popped_L1 && ctrl.GetModalStack().IsEmpty() && rel_tok == 101);
        }

        // 2. Аварийный сброс всего стека при смерти/загрузке/смене уровня
        {
            FocusModalTestController ctrl;
            ctrl.OpenToDepth(3, { 201, 202, 203 }, 0, 2000);
            std::vector<uint32_t> released;
            size_t closed = ctrl.EmergencyCloseAll("actor_dead", ModalButtonMask(ModalButton::A), 2050, &released);
            check("emergency_clear_closes_all_3",
                closed == 3 && ctrl.GetModalStack().IsEmpty() &&
                released.size() == 3 && released[0] == 203 && released[1] == 202 && released[2] == 201);
        }

        // 3. Автоповтор и плавная прокрутка на полной паузе (dwTimeContinual, независимость от 30 vs 75 FPS)
        {
            ContinualRepeatTimer timer30(320, 90);
            ContinualRepeatTimer timer75(320, 90);

            uint32_t steps30 = 0;
            uint32_t steps75 = 0;
            for (uint32_t t = 0; t <= 1200; t += 33) // ~30 FPS
                steps30 += timer30.Update(true, gpfocus::NavDir::Down, 5000 + t);
            for (uint32_t t = 0; t <= 1200; t += 13) // ~75 FPS
                steps75 += timer75.Update(true, gpfocus::NavDir::Down, 5000 + t);

            check("continual_repeat_30_vs_75_fps_match", steps30 == steps75 && steps30 == 11);

            ContinualSmoothScroller sc30;
            ContinualSmoothScroller sc75;
            sc30.Configure(20, 6, 32.0f);
            sc75.Configure(20, 6, 32.0f);
            sc30.Reset(0);
            sc75.Reset(0);
            sc30.EnsureFocusedVisible(15);
            sc75.EnsureFocusedVisible(15);

            for (uint32_t t = 0; t <= 500; t += 33)
                sc30.Step(10000 + t);
            for (uint32_t t = 0; t <= 500; t += 13)
                sc75.Step(10000 + t);

            check("continual_scroll_converges_on_pause",
                std::fabs(sc30.GetVisualOffsetPx() - sc75.GetVisualOffsetPx()) < 1.0f &&
                sc30.GetCurrentTopIndex() == 10 && sc75.GetCurrentTopIndex() == 10);
        }

        // 4. Удаление первого / среднего / последнего / единственного элемента на тестовом экране
        {
            FocusModalTestController ctrl;
            ctrl.PopulateSyntheticItems(8, 1, 3000);
            auto r_first  = ctrl.DeleteFirstItem(3010);
            auto r_mid    = ctrl.DeleteMiddleItem(3020);
            auto r_last   = ctrl.DeleteLastItem(3030);
            auto r_single = ctrl.DeleteSingleItem(3040);

            check("ctrl_delete_first_middle_last_single",
                r_first == gpfocus::FocusRestoreReason::NeighborBelow &&
                r_mid   == gpfocus::FocusRestoreReason::NeighborBelow &&
                r_last  == gpfocus::FocusRestoreReason::NeighborAbove &&
                r_single == gpfocus::FocusRestoreReason::EmptyGroupHeader &&
                ctrl.GetRootGraph().GetFocusedNode() != nullptr &&
                ctrl.GetRootGraph().GetFocusedNode()->kind == gpfocus::FocusNodeKind::EmptyStatePlaceholder);
        }

        log << "[gp_modal_selftest] total=" << summary.total_cases
            << " passed=" << summary.passed_cases
            << " failed=" << summary.failed_cases;
        summary.report = log.str();
        return summary;
    }

} // namespace gpmodal
