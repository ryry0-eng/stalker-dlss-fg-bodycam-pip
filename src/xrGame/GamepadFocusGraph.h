#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpfocus
{
    constexpr uint32_t kInvalidNodeId  = 0xFFFFFFFFu;
    constexpr uint32_t kInvalidGroupId = 0xFFFFFFFFu;
    constexpr uint16_t kInvalidObjId   = 0xFFFFu;

    // -------------------------------------------------------------------------
    // 1. StableItemID — строгая идентичность серверного/клиентского объекта
    //    для транзакций (use / move / drop / disassemble) и точного совпадения.
    //    Включает:
    //      - session_gen  : поколение игровой сессии (инкрементируется при
    //                       net_Stop / перезагрузке уровня)
    //      - object_id    : 16-битный ID объекта X-Ray (0..65534, 0xFFFF = invalid)
    //      - instance_gen : 64-битный монотонный счётчик спавна на слот object_id
    //                       (защита от LIFO-переиспользования CID_Generator в тот же кадр)
    // -------------------------------------------------------------------------
    struct StableItemID
    {
        uint32_t session_gen  = 0;
        uint16_t object_id    = kInvalidObjId;
        uint64_t instance_gen = 0;

        constexpr bool IsValid() const noexcept
        {
            return session_gen != 0 && object_id != kInvalidObjId && instance_gen != 0;
        }

        constexpr bool operator==(const StableItemID& other) const noexcept
        {
            return session_gen == other.session_gen
                && object_id == other.object_id
                && instance_gen == other.instance_gen;
        }

        constexpr bool operator!=(const StableItemID& other) const noexcept
        {
            return !(*this == other);
        }
    };

    // -------------------------------------------------------------------------
    // 2. FocusItemKey (FocusKey) — эвристический ключ фокуса, отделённый от
    //    транзакционного StableItemID. Переживает:
    //      - удаление/перенос предмета (сохраняет старые координаты grid_x/grid_y
    //        и list_index для перехода к соседу снизу/сверху),
    //      - расходование пачки в UIInventory (UICellItem:ResetToChild меняет ID
    //        ведущего предмета ячейки cell_indx при той же секции section),
    //      - SPAWN_ANTIFREEZE разрыв 1..3+ кадра (alife_release + alife_create),
    //      - пересортировку (SortingPlus меняет координаты, но сохраняет
    //        StableItemID либо подбирает ближайший такой же предмет по секции и кондиции).
    // -------------------------------------------------------------------------
    struct FocusItemKey
    {
        std::string section;             // секция предмета (например, "medkit_army", "wpn_ak74")
        uint16_t    cond_permille = 1000;// квантованная кондиция round(cond * 1000) в промилле [0..1000]
        uint16_t    uses_or_ammo  = 1;   // оставшиеся применения / пачка / патроны
        int16_t     grid_x        = 0;   // колонка сетки (1-based или 0-based; в 1D-списке = 0)
        int16_t     grid_y        = 0;   // строка сетки (в 1D-списке = list_index)
        int16_t     grid_w        = 1;   // ширина в ячейках (>= 1)
        int16_t     grid_h        = 1;   // высота в ячейках (>= 1)
        int32_t     list_index    = -1;  // порядковый индекс среди предметов группы (0..N-1)
        int32_t     cell_indx     = -1;  // индекс ячейки UICellContainer (ci.indx) для перехвата ResetToChild
        uint8_t     bag_id        = 0;   // идентификатор контейнера/сумки (EDDListType)
        uint8_t     tab_id        = 0;   // активная вкладка фильтра (SortingPlus / категории)

        bool HasItemIdentity() const noexcept
        {
            return !section.empty() || list_index >= 0;
        }

        static uint16_t QuantizeCondition(float cond_0_to_1) noexcept;
    };

    // -------------------------------------------------------------------------
    // 3. Направления навигации, типы узлов и причины восстановления фокуса
    // -------------------------------------------------------------------------
    enum class NavDir : uint8_t
    {
        Up    = 0,
        Down  = 1,
        Left  = 2,
        Right = 3,
        Count = 4
    };

    enum class FocusNodeKind : uint8_t
    {
        Item                  = 0, // обычный предмет в списке или сетке
        GroupHeader           = 1, // заголовок колонки / категории
        EmptyStatePlaceholder = 2, // узел «Пусто» (docs/12-ui-wireframes.md §2 строка 63)
        ControlWidget         = 3  // сервисная кнопка / элемент управления
    };

    enum class FocusLayoutMode : uint8_t
    {
        List1D = 0, // вертикальный 1D-список
        Grid2D = 1, // 2D-сетка (с поддержкой многоклеточных предметов w x h)
        Custom = 2  // ручная таблица соседей
    };

    enum class FocusRestoreReason : uint8_t
    {
        None                   = 0,
        SameItem               = 1, // тот же предмет по StableItemID
        StackPromotedChild     = 2, // остаток стака после UICellItem:ResetToChild в той же ячейке
        SpawnAntifreezeSettled = 3, // предмет пересоздан через alife в окне settle_until_ms
        SimilarNearby          = 4, // такой же предмет (та же section) рядом по Манхэттену и кондиции
        NeighborBelow          = 5, // сосед снизу (сдвинувшийся на место удалённого / ниже по Y)
        NeighborAbove          = 6, // сосед сверху (при удалении последнего в списке/колонке)
        FirstInGroup           = 7, // первый предмет в группе (смена вкладки / выход за пределы)
        EmptyGroupHeader       = 8  // заголовок / узел пустого состояния, когда в группе 0 предметов
    };

    const char* NavDirToString(NavDir dir) noexcept;
    const char* FocusNodeKindToString(FocusNodeKind kind) noexcept;
    const char* FocusRestoreReasonToString(FocusRestoreReason reason) noexcept;

    // -------------------------------------------------------------------------
    // 4. Узел графа фокуса (FocusNode) — 4 явных соседа Up/Down/Left/Right
    // -------------------------------------------------------------------------
    struct FocusNode
    {
        uint32_t      node_id      = kInvalidNodeId;
        uint32_t      group_id     = kInvalidGroupId;
        FocusNodeKind kind         = FocusNodeKind::Item;
        StableItemID  stable_id;
        FocusItemKey  key;
        uint32_t      neighbors[4] = { kInvalidNodeId, kInvalidNodeId, kInvalidNodeId, kInvalidNodeId };
        std::string   label;

        uint32_t GetNeighbor(NavDir dir) const noexcept
        {
            return neighbors[static_cast<size_t>(dir)];
        }

        void SetNeighbor(NavDir dir, uint32_t target_node_id) noexcept
        {
            neighbors[static_cast<size_t>(dir)] = target_node_id;
        }

        bool IsItem() const noexcept
        {
            return kind == FocusNodeKind::Item;
        }
    };

    // -------------------------------------------------------------------------
    // 5. PendingFocusAnchor — анти-прыжковый якорь на окно SPAWN_ANTIFREEZE
    //    (когда предмет удаляется на кадре N, а новый спавнится на кадре N+1..N+3)
    // -------------------------------------------------------------------------
    struct PendingFocusAnchor
    {
        bool         active          = false;
        StableItemID prev_stable_id;
        FocusItemKey prev_key;
        uint32_t     settle_until_ms = 0;

        void Clear() noexcept
        {
            active          = false;
            prev_stable_id  = StableItemID{};
            prev_key        = FocusItemKey{};
            settle_until_ms = 0;
        }
    };

    // -------------------------------------------------------------------------
    // 6. FocusGroup — группа узлов (колонка/панель/список) с памятью
    //    последнего валидного узла и якорём восстановления
    // -------------------------------------------------------------------------
    struct FocusGroup
    {
        uint32_t           group_id            = 0;
        std::string        name;
        FocusLayoutMode    layout_mode         = FocusLayoutMode::List1D;
        uint8_t            bag_id              = 0;
        uint8_t            active_tab_id       = 0;
        bool               wrap_navigation     = false;

        std::vector<FocusNode> nodes;

        uint32_t           current_node_id     = kInvalidNodeId;
        uint32_t           last_valid_node_id  = kInvalidNodeId; // память для кнопки View (D24)
        StableItemID       anchor_stable_id;
        FocusItemKey       anchor_key;
        bool               has_anchor          = false;
        PendingFocusAnchor pending_anchor;

        int16_t            preferred_col       = 0; // запоминаемая колонка при вертикальном ходе через широкие предметы
        FocusRestoreReason last_restore_reason = FocusRestoreReason::None;
        uint32_t           rebuild_count       = 0;

        const FocusNode* FindNode(uint32_t node_id) const noexcept;
        FocusNode*       FindNodeMutable(uint32_t node_id) noexcept;
        size_t           CountItemNodes() const noexcept;
    };

    // -------------------------------------------------------------------------
    // 7. FocusGraph — полный граф фокуса экрана
    // -------------------------------------------------------------------------
    class FocusGraph
    {
    public:
        static constexpr uint32_t kDefaultSettleWindowMs = 180;

        FocusGraph() = default;

        void Clear();

        // Создание и доступ к группам
        uint32_t          AddGroup(const std::string& name, FocusLayoutMode mode, uint8_t bag_id = 0, bool wrap = false);
        FocusGroup*       GetGroupMutable(uint32_t group_id) noexcept;
        const FocusGroup* GetGroup(uint32_t group_id) const noexcept;
        const std::vector<FocusGroup>& GetGroups() const noexcept { return m_groups; }

        uint32_t          GetActiveGroupId() const noexcept { return m_active_group_id; }
        const FocusGroup* GetActiveGroup() const noexcept;
        const FocusNode*  GetFocusedNode() const noexcept;
        const FocusNode*  GetGroupFocusedNode(uint32_t group_id) const noexcept;

        // Навигация крестовиной / левым стиком внутри активной группы
        bool MoveFocus(NavDir dir, uint32_t now_ms = 0);

        // Принудительная установка фокуса на конкретный узел в группе
        bool SetFocusedNode(uint32_t group_id, uint32_t node_id, bool update_anchor = true);

        // Переключение активной группы (кнопка View по решению D24)
        // Сохраняет память последнего узла в покидаемой группе и возвращает фокус
        // на запомненный узел в целевой группе (или детерминированно восстанавливает его).
        bool SwitchToGroup(uint32_t target_group_id, uint32_t now_ms = 0);
        uint32_t SwitchGroupNext(uint32_t now_ms = 0);

        // Построение связей 4 соседей (Up/Down/Left/Right)
        bool BuildListNeighbors(uint32_t group_id, bool wrap = false);
        bool BuildGridNeighbors(uint32_t group_id, bool wrap = false);

        // Событийная пересборка группы предметов + детерминированное восстановление фокуса:
        //   1. тот же предмет (SameItem / SpawnAntifreezeSettled / StackPromotedChild)
        //   2. такой же предмет рядом (SimilarNearby — та же секция, мин. Манхэттен, близкая кондиция)
        //   3. сосед снизу (NeighborBelow)
        //   4. сосед сверху (NeighborAbove)
        //   5. первый в группе (FirstInGroup)
        //   6. заголовок / пустое состояние (EmptyGroupHeader)
        FocusRestoreReason RebuildGroupItems(
            uint32_t                      group_id,
            const std::vector<FocusNode>& item_nodes,
            uint8_t                       new_tab_id       = 0,
            uint32_t                      now_ms           = 0,
            uint32_t                      settle_window_ms = kDefaultSettleWindowMs);

        // Старое наивное поведение (без многоступенчатого восстановления FocusKey) —
        // используется в unit-тесте для доказательства перехвата регрессий:
        //   - при ResetToChild (смена ID в ячейке при удалении верхнего элемента стака) сбрасывает фокус в invalid
        //     (воспроизводит баг UICellContainer:RemoveIndex в utils_ui.script:1500-1509 -> self.selected = nil на L2024);
        //   - при удалении последнего/единственного предмета не делает шаг вверх и не создаёт EmptyGroupHeader;
        //   - при пересортировке оставляет старый индекс вместо следования за предметом.
        static uint32_t RestoreFocusNaiveOldBehavior(
            const std::vector<FocusNode>& new_item_nodes,
            const StableItemID&           prev_stable_id,
            const FocusItemKey&           prev_key,
            uint8_t                       prev_tab_id,
            uint8_t                       new_tab_id);

        // Форматированный текстовый дамп для консольной команды gp_focus_dump
        std::string FormatDump() const;

    private:
        static void SyncAnchorFromNode(FocusGroup& group, const FocusNode& node);

        FocusRestoreReason RestoreFocusInternal(
            FocusGroup& group,
            bool        tab_changed,
            uint32_t    now_ms,
            uint32_t    settle_window_ms);

        std::vector<FocusGroup> m_groups;
        uint32_t                m_active_group_id = kInvalidGroupId;
    };

    // Встроенный самотест для вызова из gp_focus_dump selftest и офлайн-раннера
    struct SelfTestSummary
    {
        uint32_t total_cases             = 0;
        uint32_t passed_cases            = 0;
        uint32_t failed_cases            = 0;
        uint32_t old_behavior_regressions = 0;
        std::string report;
    };

    SelfTestSummary RunFocusGraphSelfTest();

} // namespace gpfocus
