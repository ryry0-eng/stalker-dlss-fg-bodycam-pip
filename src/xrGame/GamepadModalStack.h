#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "GamepadFocusGraph.h"

namespace gpmodal
{
    constexpr uint32_t kInvalidModalId   = 0;
    constexpr uint32_t kMaxModalDepth    = 8;

    // -------------------------------------------------------------------------
    // 1. Виртуальные кнопки модального окна (A = 0, B = 1, X = 2, Y = 3, View = 4,
    //    LB = 5, RB = 6, LT = 7, RT = 8, Up = 9, Down = 10, Left = 11, Right = 12)
    // -------------------------------------------------------------------------
    enum class ModalButton : uint8_t
    {
        A     = 0,  // GP_BTN_SOUTH (подтверждение / открытие вложенного окна)
        B     = 1,  // GP_BTN_EAST  (gp_ui_cancel — закрытие только верхнего окна)
        X     = 2,  // GP_BTN_WEST  (действие / удаление)
        Y     = 3,  // GP_BTN_NORTH (доп. действие)
        View  = 4,  // GP_BTN_BACK  (переключение колонки D24)
        LB    = 5,
        RB    = 6,
        LT    = 7,
        RT    = 8,
        Up    = 9,
        Down  = 10,
        Left  = 11,
        Right = 12,
        Count = 13
    };

    constexpr uint32_t ModalButtonMask(ModalButton btn) noexcept
    {
        return 1u << static_cast<uint32_t>(btn);
    }

    // -------------------------------------------------------------------------
    // 2. Снимок вызывающего узла FocusGraph для возврата фокуса при закрытии (B)
    // -------------------------------------------------------------------------
    struct CallerFocusSnapshot
    {
        bool                  valid       = false;
        uint32_t              group_id    = gpfocus::kInvalidGroupId;
        uint32_t              node_id     = gpfocus::kInvalidNodeId;
        gpfocus::StableItemID stable_id;
        gpfocus::FocusItemKey key;
        int32_t               scroll_top  = 0;
        float                 scroll_px   = 0.0f;

        void CaptureFromGraph(const gpfocus::FocusGraph& graph, int32_t cur_scroll_top = 0, float cur_scroll_px = 0.0f);
        bool RestoreToGraph(gpfocus::FocusGraph& graph, uint32_t now_ms = 0) const;
    };

    // -------------------------------------------------------------------------
    // 3. Плавная прокрутка и автоповтор удержания крестовины/стика строго по
    //    реальному времени Device.dwTimeContinual (работает идентично на полной
    //    паузе Device.Paused() == true, когда fTimeDelta == 0.0f)
    // -------------------------------------------------------------------------
    class ContinualRepeatTimer
    {
    public:
        static constexpr uint32_t kDefaultInitialDelayMs = 320;
        static constexpr uint32_t kDefaultStepMs         = 90;

        ContinualRepeatTimer(
            uint32_t initial_delay_ms = kDefaultInitialDelayMs,
            uint32_t step_ms          = kDefaultStepMs) noexcept
            : m_initial_delay_ms(initial_delay_ms)
            , m_step_ms(step_ms > 0 ? step_ms : 1)
        {
        }

        void Reset() noexcept;

        // Обновление по текущему направлению (kInvalidNodeId / None = нейтраль) и now_continual_ms.
        // Возвращает число шагов (0 = нет шага, 1 = первичный шаг или очередной тик автоповтора,
        // >1 = компенсация длинного кадра на 30 FPS для сохранения одинаковой скорости в секунду).
        uint32_t Update(
            bool             has_dir,
            gpfocus::NavDir  dir,
            uint32_t         now_continual_ms,
            bool*            out_is_repeat = nullptr) noexcept;

        bool            IsActive() const noexcept { return m_active; }
        gpfocus::NavDir GetActiveDir() const noexcept { return m_active_dir; }
        uint32_t        GetPressTimeMs() const noexcept { return m_press_time_ms; }
        uint32_t        GetNextRepeatTimeMs() const noexcept { return m_next_repeat_time_ms; }
        uint32_t        GetTotalTriggers() const noexcept { return m_total_triggers; }
        uint32_t        GetRepeatTriggers() const noexcept { return m_repeat_triggers; }

    private:
        uint32_t        m_initial_delay_ms    = kDefaultInitialDelayMs;
        uint32_t        m_step_ms             = kDefaultStepMs;
        bool            m_active              = false;
        gpfocus::NavDir m_active_dir          = gpfocus::NavDir::Up;
        uint32_t        m_press_time_ms       = 0;
        uint32_t        m_next_repeat_time_ms = 0;
        uint32_t        m_total_triggers      = 0;
        uint32_t        m_repeat_triggers     = 0;
    };

    class ContinualSmoothScroller
    {
    public:
        static constexpr float kDefaultRowHeightPx = 32.0f;
        static constexpr float kDefaultSmoothSpeed = 18.0f; // экспоненциальное сглаживание 1/с по dt_continual
        static constexpr float kDefaultSnapEpsPx   = 0.25f;

        ContinualSmoothScroller() = default;

        void Configure(int32_t total_items, int32_t visible_rows, float row_height_px = kDefaultRowHeightPx) noexcept;
        void Reset(int32_t top_index = 0) noexcept;

        // Гарантирует, что focused_index попадает в видимое окно [m_target_top_index .. m_target_top_index + m_visible_rows - 1]
        void EnsureFocusedVisible(int32_t focused_index) noexcept;

        // Непрерывная прокрутка аналоговой осью (например, правым стиком ry в [-1..1]) со скоростью rows_per_sec по dwTimeContinual
        void AddAnalogScrollVelocity(float normalized_axis, float rows_per_sec, uint32_t dt_continual_ms) noexcept;

        // Продвигает плавную прокрутку строго по реальному времени now_continual_ms (fTimeDelta игнорируется!)
        void Step(uint32_t now_continual_ms) noexcept;

        int32_t GetTotalItems() const noexcept { return m_total_items; }
        int32_t GetVisibleRows() const noexcept { return m_visible_rows; }
        int32_t GetTargetTopIndex() const noexcept { return m_target_top_index; }
        int32_t GetCurrentTopIndex() const noexcept;
        float   GetRowHeightPx() const noexcept { return m_row_height_px; }
        float   GetTargetOffsetPx() const noexcept { return m_target_offset_px; }
        float   GetVisualOffsetPx() const noexcept { return m_visual_offset_px; }
        float   GetSubRowOffsetPx() const noexcept;
        bool    IsAnimating() const noexcept;

    private:
        void ClampTarget() noexcept;

        int32_t  m_total_items      = 0;
        int32_t  m_visible_rows     = 6;
        int32_t  m_target_top_index = 0;
        float    m_row_height_px    = kDefaultRowHeightPx;
        float    m_target_offset_px = 0.0f;
        float    m_visual_offset_px = 0.0f;
        uint32_t m_last_step_ms     = 0;
        bool     m_has_last_step    = false;
    };

    // -------------------------------------------------------------------------
    // 4. Уровень модального стека (ModalLayer) и стек модальных окон (ModalStack)
    //    - Каждое окно в стеке — отдельный контекст с собственным modal_id,
    //      depth (1..N), pause_token_id, локальным FocusGraph и снимком
    //      вызывающего узла родительского окна (CallerFocusSnapshot).
    //    - При любом Push или Pop взводится барьер consumed_until_release на все
    //      удерживаемые кнопки (A, B и др.), поэтому нажатие B (gp_ui_cancel)
    //      закрывает СТРОГО ТОЛЬКО верхнее окно и никогда не пробивает следующее
    //      окно в тот же кадр или при дальнейшем удержании кнопки.
    //    - При смерти актора, загрузке сохранения или смене уровня вызывается
    //      EmergencyClearAll(reason), закрывающий весь стек и освобождающий все токены.
    // -------------------------------------------------------------------------
    struct ModalLayer
    {
        uint32_t             modal_id          = kInvalidModalId;
        uint32_t             depth             = 0; // 1 = корневое окно, 2 = модалка поверх, 3 = 3-й уровень
        std::string          tag;                   // например, "gp_test_root", "gp_modal_actions_L2", "gp_modal_confirm_L3"
        std::string          title;
        uint32_t             pause_token_id    = 0; // токен из CPauseTokenManager (или тестового провайдера)
        uint32_t             opened_at_ms      = 0; // Device.dwTimeContinual
        CallerFocusSnapshot  caller_focus;          // куда вернуть фокус в родительском слое при PopTopByCancel (B)
        gpfocus::FocusGraph  local_graph;           // собственный граф фокуса модального окна (для уровней >= 2)
    };

    class ModalStack
    {
    public:
        ModalStack() = default;

        // Открытие нового модального окна поверх текущего:
        //   - сохраняет вызывающий узел из parent_graph (для depth=1 — из root_graph, для depth>=2 — из предыдущего слоя);
        //   - захватывает токен паузы;
        //   - взводит барьер consumed_until_release для currently_held_buttons_mask (например, кнопка A, открывшая окно,
        //     не нажмёт пункт внутри только что открытого окна до физического отпускания).
        uint32_t PushModal(
            const std::string&   tag,
            const std::string&   title,
            gpfocus::FocusGraph& root_graph,
            uint32_t             pause_token_id,
            uint32_t             currently_held_buttons_mask,
            uint32_t             now_continual_ms,
            int32_t              parent_scroll_top = 0,
            float                parent_scroll_px  = 0.0f);

        // По-кадровое обновление барьера кнопок и проверка нажатия B (gp_ui_cancel):
        //   - снимает барьер только с физически отпущенных кнопок;
        //   - если в этом кадре уже произошёл Push или Pop (m_transition_locked_frame == frame_id),
        //     повторное закрытие/открытие в тот же кадр блокируется;
        //   - возвращает эффективную маску нажатий (pressed_mask) для верхнего активного окна.
        uint32_t BeginFrameButtons(
            uint32_t raw_buttons_mask,
            uint32_t frame_id,
            uint32_t now_continual_ms) noexcept;

        bool IsButtonPressedInTopModal(ModalButton btn) const noexcept
        {
            return (m_pressed_mask & ModalButtonMask(btn)) != 0;
        }

        bool IsButtonHeldInTopModal(ModalButton btn) const noexcept
        {
            return (m_effective_down_mask & ModalButtonMask(btn)) != 0;
        }

        bool IsButtonConsumedByBarrier(ModalButton btn) const noexcept
        {
            return (m_consumed_until_release_mask & ModalButtonMask(btn)) != 0;
        }

        void ConsumeButtonUntilRelease(ModalButton btn) noexcept
        {
            m_consumed_until_release_mask |= ModalButtonMask(btn);
            m_effective_down_mask &= ~ModalButtonMask(btn);
            m_pressed_mask &= ~ModalButtonMask(btn);
        }

        void ConsumeAllHeldButtons(uint32_t raw_buttons_mask) noexcept
        {
            m_consumed_until_release_mask |= raw_buttons_mask;
            m_effective_down_mask = 0;
            m_prev_effective_down_mask = 0;
            m_pressed_mask = 0;
        }

        // Закрытие СТРОГО ТОЛЬКО верхнего модального окна (по кнопке B / gp_ui_cancel):
        //   - если стек пуст — возвращает false;
        //   - если в текущем frame_id уже был переход стека — возвращает false (защита от двойного закрытия в один кадр);
        //   - извлекает только верхний слой, записывает его pause_token_id в out_released_pause_token,
        //     восстанавливает фокус на вызвавший узел родительского графа (root_graph или новый верхний слой),
        //     взводит барьер consumed_until_release для B и всех зажатых кнопок.
        bool PopTopModal(
            gpfocus::FocusGraph& root_graph,
            uint32_t             raw_buttons_mask,
            uint32_t             frame_id,
            uint32_t             now_continual_ms,
            const char*          reason,
            uint32_t*            out_released_pause_token = nullptr,
            int32_t*             out_restored_scroll_top  = nullptr,
            float*               out_restored_scroll_px   = nullptr);

        // Controlled multi-layer pop down to target_depth (e.g. L3 -> L1 on confirmation dialog accept):
        //   - pops all layers above target_depth in LIFO order within one transition;
        //   - collects all non-zero pause_token_id values into out_released_pause_tokens;
        //   - restores caller focus at each popped level and arms the release barrier on A/B and held buttons.
        bool PopToDepth(
            size_t                 target_depth,
            gpfocus::FocusGraph&   root_graph,
            uint32_t               raw_buttons_mask,
            uint32_t               frame_id,
            uint32_t               now_continual_ms,
            const char*            reason,
            std::vector<uint32_t>* out_released_pause_tokens = nullptr,
            int32_t*               out_restored_scroll_top   = nullptr,
            float*                 out_restored_scroll_px    = nullptr);

        // Полное закрытие всего стека и сбор всех удерживаемых токенов паузы при смерти актора,
        // загрузке сохранения или смене уровня.
        size_t EmergencyClearAll(
            const char*            reason,
            uint32_t               raw_buttons_mask,
            uint32_t               now_continual_ms,
            std::vector<uint32_t>* out_released_pause_tokens = nullptr);

        bool                      IsEmpty() const noexcept { return m_layers.empty(); }
        size_t                    GetDepth() const noexcept { return m_layers.size(); }
        const ModalLayer*         GetTopLayer() const noexcept { return m_layers.empty() ? nullptr : &m_layers.back(); }
        ModalLayer*               GetTopLayerMutable() noexcept { return m_layers.empty() ? nullptr : &m_layers.back(); }
        const ModalLayer*         GetLayerAtDepth(size_t depth_1_based) const noexcept;
        ModalLayer*               GetLayerAtDepthMutable(size_t depth_1_based) noexcept;
        const std::vector<ModalLayer>& GetLayers() const noexcept { return m_layers; }

        // Активный FocusGraph для текущего верхнего уровня (для depth==1 возвращает root_graph, для depth>=2 — local_graph верхнего слоя)
        gpfocus::FocusGraph&       GetActiveFocusGraph(gpfocus::FocusGraph& root_graph) noexcept;
        const gpfocus::FocusGraph& GetActiveFocusGraph(const gpfocus::FocusGraph& root_graph) const noexcept;

        // Строковый тег контекста для D3.1 CGamepadContextDispatcher::SetLuaModalMenuTag (уникален для каждого окна в стеке!)
        std::string GetTopDispatcherContextTag() const;

        uint32_t           GetRawButtonsMask() const noexcept { return m_raw_down_mask; }
        uint32_t           GetConsumedBarrierMask() const noexcept { return m_consumed_until_release_mask; }
        uint32_t           GetEffectiveDownMask() const noexcept { return m_effective_down_mask; }
        uint32_t           GetPressedMask() const noexcept { return m_pressed_mask; }
        uint32_t           GetPushCount() const noexcept { return m_push_count; }
        uint32_t           GetPopCount() const noexcept { return m_pop_count; }
        uint32_t           GetEmergencyClearCount() const noexcept { return m_emergency_clear_count; }
        const std::string& GetLastEventReason() const noexcept { return m_last_reason; }
        uint32_t           GetLastEventTimeMs() const noexcept { return m_last_event_time_ms; }

        std::string FormatDump(const gpfocus::FocusGraph& root_graph) const;

    private:
        std::vector<ModalLayer> m_layers;
        uint32_t                m_next_modal_id               = 1;
        uint32_t                m_raw_down_mask               = 0;
        uint32_t                m_consumed_until_release_mask = 0;
        uint32_t                m_effective_down_mask         = 0;
        uint32_t                m_prev_effective_down_mask    = 0;
        uint32_t                m_pressed_mask                = 0;
        uint32_t                m_transition_locked_frame     = 0xFFFFFFFFu;
        uint32_t                m_push_count                  = 0;
        uint32_t                m_pop_count                   = 0;
        uint32_t                m_emergency_clear_count       = 0;
        uint32_t                m_last_event_time_ms          = 0;
        std::string             m_last_reason                 = "init";
    };

    // -------------------------------------------------------------------------
    // 5. Модель тестового экрана C5.1 / F1.1 / F1.2 (используется и в CUIDialogWnd,
    //    и в консольных командах для пакета F1.3, и в автономном unit-тесте):
    //    - список из N предметов с плавной прокруткой ContinualSmoothScroller;
    //    - операции удаления первого / среднего / последнего / единственного /
    //      текущего предмета с проверкой восстановления фокуса по правилам F1.1;
    //    - открытие вложенных модальных окон до 3 уровней глубины:
    //        Уровень 1: Список предметов (N элементов, прокрутка)
    //        Уровень 2: Контекстное меню выбранного предмета («Использовать»,
    //                   «Удалить предмет...», «Подробности...», «Закрыть»)
    //        Уровень 3: Диалог подтверждения («Точно удалить предмет?»: «Да, удалить» / «Отмена»)
    //    - закрытие по одному уровню кнопкой B с возвратом фокуса на вызвавший узел.
    // -------------------------------------------------------------------------
    class FocusModalTestController
    {
    public:
        static constexpr int32_t kDefaultListCount   = 18;
        static constexpr int32_t kDefaultVisibleRows = 6;

        FocusModalTestController();

        // Заполнить список из N синтетических предметов (для автономного теста и гарантированного списка из N предметов в F1.3)
        void PopulateSyntheticItems(int32_t count = kDefaultListCount, uint32_t session_gen = 1, uint32_t now_ms = 0);

        // Обновить из готового набора узлов (например, из инвентаря актора + дополнение до N предметов)
        void SetItemsFromNodes(const std::vector<gpfocus::FocusNode>& nodes, uint32_t now_ms = 0);

        // Открыть корневой экран (Уровень 1)
        uint32_t OpenRootModal(uint32_t pause_token_id, uint32_t held_buttons_mask, uint32_t now_ms);

        // Открыть модальное окно 2-го уровня («Действия с предметом») поверх уровня 1
        uint32_t OpenLevel2ActionModal(uint32_t pause_token_id, uint32_t held_buttons_mask, uint32_t now_ms, std::vector<uint32_t>* out_released_tokens = nullptr);

        // Открыть модальное окно 3-го уровня («Подтверждение операции») поверх уровня 2
        uint32_t OpenLevel3ConfirmModal(uint32_t pause_token_id, uint32_t held_buttons_mask, uint32_t now_ms, std::vector<uint32_t>* out_released_tokens = nullptr);

        // Открыть сразу цепочку до заданного уровня глубины (1..3) для быстрой проверки в F1.3
        void OpenToDepth(size_t target_depth, const std::vector<uint32_t>& token_ids, uint32_t held_buttons_mask, uint32_t now_ms);

        // Нажатие B (gp_ui_cancel): закрывает строго только верхнее окно и возвращает фокус на вызвавший элемент
        bool CancelTopModal(uint32_t raw_buttons_mask, uint32_t frame_id, uint32_t now_ms, uint32_t* out_released_token = nullptr);

        // Подтверждение действия в диалоге L2/L3 (кнопка A): закрывает дочерние модальные окна (до корня L1) за один шаг,
        // освобождает их токены паузы в порядке LIFO, восстанавливает корневой фокус и взводит барьер consumed_until_release на A
        bool ConfirmAndCloseToRoot(uint32_t raw_buttons_mask, uint32_t frame_id, uint32_t now_ms, std::vector<uint32_t>* out_released_tokens = nullptr);

        // Аварийный сброс всего стека (смерть актора / загрузка сейва / смена уровня)
        size_t EmergencyCloseAll(const char* reason, uint32_t raw_buttons_mask, uint32_t now_ms, std::vector<uint32_t>* out_released_tokens = nullptr);

        // Навигация крестовиной/стиком в активном модальном уровне + обновление прокрутки и автоповтора по dwTimeContinual
        bool StepNavigation(gpfocus::NavDir dir, uint32_t now_ms);
        uint32_t StepNavigationWithRepeat(bool has_dir, gpfocus::NavDir dir, uint32_t now_ms, bool* out_is_repeat = nullptr);
        void StepContinualScroll(uint32_t now_ms, float right_stick_y = 0.0f, uint32_t dt_continual_ms = 0);

        // Удаление элементов списка уровня 1 с проверкой восстановления фокуса по правилам F1.1:
        //   - DeleteFirstItem()  -> удаляет индекс 0
        //   - DeleteMiddleItem() -> удаляет индекс size/2
        //   - DeleteLastItem()   -> удаляет индекс size-1
        //   - DeleteSingleItem() -> оставляет 1 элемент, фокусирует его и удаляет (1 -> 0 -> EmptyStatePlaceholder)
        //   - DeleteFocusedItem()-> удаляет текущий выбранный элемент
        gpfocus::FocusRestoreReason DeleteFirstItem(uint32_t now_ms);
        gpfocus::FocusRestoreReason DeleteMiddleItem(uint32_t now_ms);
        gpfocus::FocusRestoreReason DeleteLastItem(uint32_t now_ms);
        gpfocus::FocusRestoreReason DeleteSingleItem(uint32_t now_ms);
        gpfocus::FocusRestoreReason DeleteFocusedItem(uint32_t now_ms);
        gpfocus::FocusRestoreReason DeleteItemAtListIndex(size_t idx, uint32_t now_ms, bool focus_target_before_delete = true);

        const gpfocus::FocusGraph&            GetRootGraph() const noexcept { return m_root_graph; }
        gpfocus::FocusGraph&                  GetRootGraphMutable() noexcept { return m_root_graph; }
        const gpfocus::FocusGraph&            GetActiveGraph() const noexcept { return m_modal_stack.GetActiveFocusGraph(m_root_graph); }
        gpfocus::FocusGraph&                  GetActiveGraphMutable() noexcept { return m_modal_stack.GetActiveFocusGraph(m_root_graph); }
        const ModalStack&                     GetModalStack() const noexcept { return m_modal_stack; }
        ModalStack&                           GetModalStackMutable() noexcept { return m_modal_stack; }
        const ContinualSmoothScroller&        GetScroller() const noexcept { return m_scroller; }
        ContinualSmoothScroller&              GetScrollerMutable() noexcept { return m_scroller; }
        const ContinualRepeatTimer&           GetRepeatTimer() const noexcept { return m_repeat_timer; }
        ContinualRepeatTimer&                 GetRepeatTimerMutable() noexcept { return m_repeat_timer; }
        const std::vector<gpfocus::FocusNode>& GetItems() const noexcept { return m_items; }
        gpfocus::FocusRestoreReason           GetLastDeleteRestoreReason() const noexcept { return m_last_delete_restore; }
        std::string                           GetLastOperationSummary() const { return m_last_op_summary; }

        std::string FormatStatusDump() const;

    private:
        void SyncScrollerToRootFocus(uint32_t now_ms);
        void ReindexAndRebuildRoot(uint32_t now_ms);

        gpfocus::FocusGraph             m_root_graph;
        uint32_t                        m_root_list_group_id  = 0;
        uint32_t                        m_root_equip_group_id = 1;
        std::vector<gpfocus::FocusNode> m_items;
        ModalStack                      m_modal_stack;
        ContinualSmoothScroller         m_scroller;
        ContinualRepeatTimer            m_repeat_timer;
        gpfocus::FocusRestoreReason     m_last_delete_restore = gpfocus::FocusRestoreReason::None;
        std::string                     m_last_op_summary     = "ready";
    };

    // Встроенный самотест F1.2 (вложенность 1..3, возврат фокуса, барьер B/A, аварийный сброс, прокрутка и автоповтор на паузе)
    gpfocus::SelfTestSummary RunModalStackSelfTest();

} // namespace gpmodal
