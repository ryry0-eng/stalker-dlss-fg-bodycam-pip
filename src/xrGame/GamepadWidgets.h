#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GamepadFocusGraph.h"
#include "GamepadModalStack.h"

namespace gpwidgets
{

// ============================================================================
// F2.1: Базовые виджеты интерфейса (D09, D10, D20, D24, D38, D50–D53)
//   1. ItemListGridWidget   — двухрежимный список/сетка предметов (D10)
//   2. ComparisonCardWidget — карточка сравнения со статами Accurate Defense Values
//   3. TabStripWidget       — полоса вкладок LB/RB (разделы) + LT/RT (категории)
//   4. QuantityModalWidget  — модальный выбор количества (◄/► ±1, LB/RB ±10, LT/RT Мин/Макс)
//   5. ConfirmDialogWidget  — модальный диалог подтверждения ([Подтвердить] / [Отмена])
//   6. FooterGlyphBarWidget — нижняя панель динамических глифов (D5.1)
//
// Инвариант F2.1:
//   - Каждый виджет представлен узлом в графе фокуса gpfocus::FocusGraph.
//   - Все данные — строго только чтение (read-only снимок через gp_bridge.script);
//     ни один виджет не мутирует инвентарь, слоты или состояние актора напрямую.
// ============================================================================

enum class TopTabId : uint8_t
{
    Equipment = 0,
    Backpack  = 1,
    Health    = 2,
    Magazines = 3,
    PDA       = 4,
    Skills    = 5,
    Settings  = 6,
    Count     = 7
};

enum class CategoryFilterId : uint8_t
{
    All            = 0,
    Weapons        = 1,
    Ammo           = 2,
    Meds           = 3,
    Food           = 4,
    ArmorArtefacts = 5,
    ToolsParts     = 6,
    Misc           = 7,
    Count          = 8
};

enum class ItemViewMode : uint8_t
{
    List1D = 0, // По умолчанию (D10): категоризированный список с полными названиями и полосками
    Grid2D = 1  // Компактная сетка карточек 4xN (D10)
};

enum class WidgetGlyphStyle : uint8_t
{
    Xbox        = 0,
    PlayStation = 1,
    Nintendo    = 2,
    Generic     = 3
};

enum class WidgetFaceStyle : uint8_t
{
    ABXY     = 0,
    Sony     = 1,
    BAYX     = 2,
    AXBY     = 3,
    Numbered = 4
};

enum class LogicalPromptButton : uint8_t
{
    South      = 0, // A / Cross / B
    East       = 1, // B / Circle / A
    West       = 2, // X / Square / Y
    North      = 3, // Y / Triangle / X
    LB         = 4,
    RB         = 5,
    LBRB       = 6,
    LT         = 7,
    RT         = 8,
    LTRT       = 9,
    DPadLR     = 10,
    DPadUD     = 11,
    R3         = 12,
    RightStick = 13,
    View       = 14
};

struct AccurateDefenseSnapshot
{
    // Показатели предмета из G.A.M.M.A. Accurate Defense Values (grok_actor_damage_balancer)
    float    fire_wound_prot        = 0.0f; // Баллистическая защита (0..1)
    float    ap_res                 = 0.0f; // Стойкость к бронебойным пулям (1 - hit_fraction_actor)
    int32_t  br_class               = 0;    // Класс брони предмета (0..100)
    float    burn_prot              = 0.0f;
    float    shock_prot             = 0.0f;
    float    acid_prot              = 0.0f;
    float    radia_prot             = 0.0f;
    float    psi_prot               = 0.0f;
    float    wound_prot             = 0.0f;
    float    strike_prot            = 0.0f;
    float    expl_prot              = 0.0f;

    // Суммарная защита актора (read-only расчёт get_protection для bip01_spine=13 и bip01_head=19)
    int32_t  body_br_class          = 0;
    int32_t  body_premitigation_pct = 0;
    float    body_fire_wound        = 0.0f;
    float    body_wound             = 0.0f;
    float    body_explosion         = 0.0f;
    float    body_strike            = 0.0f;
    float    body_burn              = 0.0f;
    float    body_shock             = 0.0f;
    float    body_acid              = 0.0f;
    float    body_psi               = 0.0f;
    float    body_radia             = 0.0f;

    int32_t  head_br_class          = 0;
    int32_t  head_premitigation_pct = 0;
    float    head_fire_wound        = 0.0f;
    float    head_psi               = 0.0f;
};

struct StatCompareRow
{
    std::string stat_id;
    std::string label;
    std::string unit;
    float       value         = 0.0f;
    float       compare_value = 0.0f;
    float       delta         = 0.0f;
    bool        has_compare   = false;
    char        delta_sign    = '='; // '+', '-', '='
};

struct WidgetItemSnapshot
{
    gpfocus::StableItemID        stable_id{};
    std::string                  section;
    std::string                  display_name;
    std::string                  ammo_or_caliber;
    CategoryFilterId             category               = CategoryFilterId::Misc;
    std::string                  stats_group            = "misc"; // "weapon", "outfit", "helmet", "artefact", "booster", "misc"
    uint16_t                     cond_permille          = 1000;   // 0..1000 (820 = 82.0%)
    uint16_t                     stack_count            = 1;
    uint16_t                     uses_or_ammo           = 1;
    int16_t                      grid_w                 = 1;
    int16_t                      grid_h                 = 1;
    float                        unit_weight_kg         = 0.5f;
    uint32_t                     unit_cost_ru           = 100;
    uint8_t                      equip_slot             = 0;      // 2=Слот 1, 3=Слот 2, 7=Броня, 12=Шлем
    bool                         is_equipped            = false;
    bool                         is_favorite            = false;
    bool                         is_junk                = false;
    bool                         is_quest               = false;
    bool                         has_mag_vest_warning   = false;  // Предупреждение mags_patches при смене брони
    std::string                  wpo_parts_summary;
    AccurateDefenseSnapshot      adv{};
    AccurateDefenseSnapshot      adv_full_100{};
    std::vector<StatCompareRow>  base_stats;
};

const char* TopTabToLabelRU(TopTabId tab);
const char* CategoryFilterToLabelRU(CategoryFilterId cat);
const char* CategoryFilterToBridgeId(CategoryFilterId cat);
const char* ViewModeToLabelRU(ItemViewMode mode);

// ============================================================================
// 1. Полоса вкладок LB/RB + LT/RT (TabStripWidget)
// ============================================================================
class TabStripWidget
{
public:
    static constexpr uint32_t kTopTabNodeId      = 9001u;
    static constexpr uint32_t kCategoryTabNodeId = 9002u;

    void InitializeInGraph(gpfocus::FocusGraph& graph);
    void SyncGraphNodes(gpfocus::FocusGraph& graph, uint32_t now_ms);

    TopTabId StepTopTab(int delta, bool wrap = true);
    CategoryFilterId StepCategoryFilter(int delta, bool wrap = true);

    void SetTopTab(TopTabId tab) { active_top_tab_ = tab; }
    void SetCategoryFilter(CategoryFilterId cat) { active_category_ = cat; }

    TopTabId GetActiveTopTab() const { return active_top_tab_; }
    CategoryFilterId GetActiveCategoryFilter() const { return active_category_; }
    uint32_t GetFocusGroupId() const { return group_id_; }

    void SetActorTelemetry(float current_weight_kg, float max_weight_kg, uint32_t money_ru, bool pause_active);

    std::string FormatTopBarLine() const;
    std::string FormatCategoryFilterLine(ItemViewMode view_mode, size_t visible_items_count) const;

private:
    uint32_t         group_id_          = gpfocus::kInvalidGroupId;
    TopTabId         active_top_tab_    = TopTabId::Backpack;
    CategoryFilterId active_category_   = CategoryFilterId::All;
    float            current_weight_kg_ = 24.8f;
    float            max_weight_kg_     = 45.0f;
    uint32_t         money_ru_          = 48250u;
    bool             pause_active_      = true;
};

// ============================================================================
// 2. Двухрежимный виджет предметов: Список и Сетка (ItemListGridWidget, D10)
// ============================================================================
class ItemListGridWidget
{
public:
    static constexpr int16_t kGridColumns     = 4;
    static constexpr int32_t kListVisibleRows = 6;
    static constexpr int32_t kGridVisibleRows = 3;

    void InitializeInGraph(gpfocus::FocusGraph& graph);

    // Загрузка read-only каталога предметов (не мутирует инвентарь)
    void SetCatalogReadOnly(const std::vector<WidgetItemSnapshot>& items,
                            CategoryFilterId filter,
                            gpfocus::FocusGraph& graph,
                            uint32_t now_ms);

    // Смена фильтра категории LT/RT с пересборкой узлов и детерминированным сохранением фокуса
    gpfocus::FocusRestoreReason ApplyCategoryFilter(CategoryFilterId filter,
                                                    gpfocus::FocusGraph& graph,
                                                    uint32_t now_ms);

    // Мгновенное переключение Список <-> Сетка (D10) с сохранением фокуса по StableItemID
    gpfocus::FocusRestoreReason ToggleViewMode(gpfocus::FocusGraph& graph, uint32_t now_ms);
    gpfocus::FocusRestoreReason SetViewMode(ItemViewMode mode, gpfocus::FocusGraph& graph, uint32_t now_ms);

    void SyncScrollToFocusedNode(const gpfocus::FocusGraph& graph, uint32_t now_ms);
    void TickSmoothScroll(uint32_t now_ms);

    ItemViewMode GetViewMode() const { return view_mode_; }
    CategoryFilterId GetActiveFilter() const { return active_filter_; }
    uint32_t GetFocusGroupId() const { return group_id_; }
    int32_t GetFocusedItemIndex(const gpfocus::FocusGraph& graph) const;
    const std::vector<WidgetItemSnapshot>& GetAllItems() const { return all_items_; }
    const std::vector<WidgetItemSnapshot>& GetFilteredItems() const { return filtered_items_; }

    const WidgetItemSnapshot* GetFocusedItem(const gpfocus::FocusGraph& graph) const;
    const WidgetItemSnapshot* FindItemByStableId(const gpfocus::StableItemID& id) const;
    const WidgetItemSnapshot* FindEquippedItemForSlot(uint8_t equip_slot) const;

    const gpmodal::ContinualSmoothScroller& GetScroller() const { return scroller_; }
    uint32_t GetInventoryMutationCount() const { return inventory_mutation_count_; }

    static std::string FormatConditionBar(uint16_t cond_permille, int width_chars = 8);
    std::string FormatListRow(size_t filtered_index, bool is_focused) const;
    std::string FormatGridCard(size_t filtered_index, bool is_focused) const;

private:
    gpfocus::FocusRestoreReason RebuildGraphNodes(gpfocus::FocusGraph& graph, uint32_t now_ms);

    uint32_t                         group_id_                 = gpfocus::kInvalidGroupId;
    ItemViewMode                     view_mode_                = ItemViewMode::List1D;
    CategoryFilterId                 active_filter_            = CategoryFilterId::All;
    std::vector<WidgetItemSnapshot>  all_items_;
    std::vector<WidgetItemSnapshot>  filtered_items_;
    gpmodal::ContinualSmoothScroller scroller_;
    uint32_t                         inventory_mutation_count_ = 0; // Инвариант: всегда 0
};

// ============================================================================
// 3. Карточка сравнения со статами Accurate Defense Values (ComparisonCardWidget)
// ============================================================================
class ComparisonCardWidget
{
public:
    static constexpr uint32_t kComparisonCardNodeId = 9101u;
    static constexpr int32_t  kVisibleStatLines     = 8;

    void InitializeInGraph(gpfocus::FocusGraph& graph);

    // Обновление карточки строго в режиме чтения по текущему сфокусированному предмету
    void RefreshFromFocusedItem(const WidgetItemSnapshot* focused_item,
                                const ItemListGridWidget& catalog,
                                uint32_t now_ms);

    // Переключение слота сравнения оружия (Слот 1 <-> Слот 2 по R3) без сброса фокуса списка
    uint8_t ToggleWeaponCompareSlot(const WidgetItemSnapshot* focused_item,
                                    const ItemListGridWidget& catalog,
                                    uint32_t now_ms);

    // Переключение показа статов при 100% состоянии (zz_ui_inventory_better_stats_bars.show_full_stats)
    bool ToggleFullStatsPreview(const WidgetItemSnapshot* focused_item,
                                const ItemListGridWidget& catalog,
                                uint32_t now_ms);

    void ScrollByStick(float right_stick_y, uint32_t dt_continual_ms, uint32_t now_ms);

    uint32_t GetFocusGroupId() const { return group_id_; }
    uint8_t GetActiveWeaponCompareSlot() const { return weapon_compare_slot_; }
    bool IsFullStatsPreviewEnabled() const { return show_full_stats_100_; }
    bool HasFocusedItem() const { return has_focused_item_; }
    const WidgetItemSnapshot& GetCurrentItem() const { return current_item_; }
    bool HasComparedItem() const { return has_compared_item_; }
    const WidgetItemSnapshot& GetComparedItem() const { return compared_item_; }
    const AccurateDefenseSnapshot& GetDisplayedAdv() const { return displayed_adv_; }
    const std::vector<StatCompareRow>& GetComputedRows() const { return computed_rows_; }
    const std::vector<std::string>& GetFormattedLines() const { return formatted_lines_; }
    const gpmodal::ContinualSmoothScroller& GetScroller() const { return scroller_; }
    uint32_t GetInventoryMutationCount() const { return inventory_mutation_count_; }

private:
    uint32_t                         group_id_                 = gpfocus::kInvalidGroupId;
    uint8_t                          weapon_compare_slot_      = 2; // 2 = Слот 1 (осн.), 3 = Слот 2 (доп.)
    bool                             show_full_stats_100_      = false;
    bool                             has_focused_item_         = false;
    bool                             has_compared_item_        = false;
    WidgetItemSnapshot               current_item_{};
    WidgetItemSnapshot               compared_item_{};
    AccurateDefenseSnapshot          displayed_adv_{};
    std::vector<StatCompareRow>      computed_rows_;
    std::vector<std::string>         formatted_lines_;
    gpmodal::ContinualSmoothScroller scroller_;
    uint32_t                         inventory_mutation_count_ = 0; // Инвариант: всегда 0
};

// ============================================================================
// 4. Модальный выбор количества (QuantityModalWidget)
// ============================================================================
struct QuantitySelectionResult
{
    bool                  confirmed         = false;
    bool                  cancelled         = false;
    gpfocus::StableItemID stable_id{};
    std::string           section;
    int32_t               selected_quantity = 1;
    int32_t               max_quantity      = 1;
    float                 total_weight_kg   = 0.0f;
    uint32_t              total_cost_ru     = 0;
};

class QuantityModalWidget
{
public:
    static constexpr uint32_t kSliderNodeId  = 9201u;
    static constexpr uint32_t kConfirmNodeId = 9202u;
    static constexpr uint32_t kCancelNodeId  = 9203u;

    bool OpenOnStack(gpmodal::ModalStack& modal_stack,
                     gpfocus::FocusGraph& root_graph,
                     const WidgetItemSnapshot& item,
                     int32_t max_by_weight,
                     int32_t max_by_money,
                     uint32_t pause_token_id,
                     uint32_t currently_held_buttons_mask,
                     uint32_t now_ms);

    int32_t StepQuantity(int32_t delta);
    int32_t StepByPack(int32_t direction); // LB = -10, RB = +10
    int32_t SetToMin();                    // LT = 1
    int32_t SetToMax();                    // RT = effective_max

    // Удержание ◄/► на паузе через ContinualRepeatTimer (Device.dwTimeContinual)
    int32_t UpdateHoldRepeat(gpfocus::NavDir held_dir, bool is_held, uint32_t now_ms);

    QuantitySelectionResult ConfirmSelection() const;
    QuantitySelectionResult CancelSelection() const;

    bool IsOpen(const gpmodal::ModalStack& modal_stack) const;
    int32_t GetCurrentQuantity() const { return current_qty_; }
    int32_t GetEffectiveMaxQuantity() const { return effective_max_qty_; }
    int32_t GetPackStep() const { return pack_step_; }
    float GetTotalWeightKg() const { return static_cast<float>(current_qty_) * item_.unit_weight_kg; }
    uint32_t GetTotalCostRu() const { return static_cast<uint32_t>(current_qty_) * item_.unit_cost_ru; }
    const WidgetItemSnapshot& GetItem() const { return item_; }
    uint32_t GetInventoryMutationCount() const { return inventory_mutation_count_; }

    std::string FormatModalSummaryLine() const;
    std::string FormatProgressBar(int width_chars = 16) const;

private:
    WidgetItemSnapshot            item_{};
    int32_t                       current_qty_              = 1;
    int32_t                       effective_max_qty_        = 1;
    int32_t                       pack_step_                = 10;
    gpmodal::ContinualRepeatTimer repeat_timer_;
    uint32_t                      inventory_mutation_count_ = 0; // Инвариант: всегда 0
};

// ============================================================================
// 5. Модальный диалог подтверждения (ConfirmDialogWidget)
// ============================================================================
struct ConfirmDialogResult
{
    bool                  confirmed = false;
    bool                  cancelled = false;
    gpfocus::StableItemID subject_id{};
    std::string           action_tag;
};

class ConfirmDialogWidget
{
public:
    static constexpr uint32_t kConfirmYesNodeId = 9301u;
    static constexpr uint32_t kConfirmNoNodeId  = 9302u;

    bool OpenOnStack(gpmodal::ModalStack& modal_stack,
                     gpfocus::FocusGraph& root_graph,
                     const gpfocus::StableItemID& subject_id,
                     const std::string& title,
                     const std::string& warning_message,
                     const std::string& action_tag,
                     bool default_focus_cancel,
                     uint32_t pause_token_id,
                     uint32_t currently_held_buttons_mask,
                     uint32_t now_ms);

    bool IsOpen(const gpmodal::ModalStack& modal_stack) const;
    bool IsYesFocused(const gpmodal::ModalStack& modal_stack) const;

    ConfirmDialogResult ActivateFocusedChoice(const gpmodal::ModalStack& modal_stack) const;
    ConfirmDialogResult CancelDialog() const;

    const std::string& GetTitle() const { return title_; }
    const std::string& GetWarningMessage() const { return warning_message_; }
    const std::string& GetActionTag() const { return action_tag_; }
    uint32_t GetInventoryMutationCount() const { return inventory_mutation_count_; }

    std::string FormatDialogLine(const gpmodal::ModalStack& modal_stack) const;

private:
    gpfocus::StableItemID subject_id_{};
    std::string           title_;
    std::string           warning_message_;
    std::string           action_tag_;
    uint32_t              inventory_mutation_count_ = 0; // Инвариант: всегда 0
};

// ============================================================================
// 6. Нижняя панель динамических глифов (FooterGlyphBarWidget, D5.1, D20, D38)
// ============================================================================
struct GlyphPromptEntry
{
    LogicalPromptButton button       = LogicalPromptButton::South;
    std::string         badge_text;   // Напр. "[A]" / "[Cross]" / "[B]"
    std::string         texture_id;   // Напр. "ui_gp_xbox_south" / "ui_gp_ps_south"
    std::string         action_label; // Напр. "Выбрать" / "Сетка 4xN" / "Отмена"
};

class FooterGlyphBarWidget
{
public:
    static constexpr uint32_t kFooterBarNodeId = 9401u;

    void InitializeInGraph(gpfocus::FocusGraph& graph);

    void SetGlyphStyle(WidgetGlyphStyle style, WidgetFaceStyle face_style, bool is_russian = true);
    WidgetGlyphStyle GetGlyphStyle() const { return glyph_style_; }
    WidgetFaceStyle GetFaceStyle() const { return face_style_; }

    // Пересборка списка доступных подсказок строго по текущему состоянию экрана и модального стека
    void RebuildPrompts(const gpfocus::FocusGraph& root_graph,
                        const gpmodal::ModalStack& modal_stack,
                        const ItemListGridWidget& item_widget,
                        const ComparisonCardWidget& comparison_widget);

    const std::vector<GlyphPromptEntry>& GetPrompts() const { return prompts_; }
    uint32_t GetFocusGroupId() const { return group_id_; }

    static const char* ResolveButtonBadge(LogicalPromptButton btn,
                                          WidgetGlyphStyle style,
                                          WidgetFaceStyle face_style,
                                          bool is_russian);
    static const char* ResolveButtonTextureId(LogicalPromptButton btn,
                                              WidgetGlyphStyle style,
                                              WidgetFaceStyle face_style);

    std::string FormatFooterLine() const;

private:
    uint32_t                      group_id_    = gpfocus::kInvalidGroupId;
    WidgetGlyphStyle              glyph_style_ = WidgetGlyphStyle::Xbox;
    WidgetFaceStyle               face_style_  = WidgetFaceStyle::ABXY;
    bool                          is_russian_  = true;
    std::vector<GlyphPromptEntry> prompts_;
};

// ============================================================================
// Контроллер витрины базовых виджетов F2.1 (WidgetShowcaseController)
// ============================================================================
class WidgetShowcaseController
{
public:
    void InitializeDefaultShowcase(uint32_t now_ms = 1000u);
    static std::vector<WidgetItemSnapshot> BuildDefaultShowcaseCatalog(uint32_t session_gen = 1u);

    bool MoveFocus(gpfocus::NavDir dir, bool is_repeat, uint32_t now_ms);
    void StepTopTab(int delta, uint32_t now_ms);
    gpfocus::FocusRestoreReason StepCategoryFilter(int delta, uint32_t now_ms);
    gpfocus::FocusRestoreReason ToggleListGridView(uint32_t now_ms);
    uint8_t ToggleCompareSlot(uint32_t now_ms);
    bool ToggleFullStats100(uint32_t now_ms);
    void CycleGlyphStyle(uint32_t now_ms);

    bool OpenQuantityModalForFocused(uint32_t pause_token_id, uint32_t held_mask, uint32_t now_ms);
    bool OpenConfirmModalForFocused(uint32_t pause_token_id, uint32_t held_mask, uint32_t now_ms);

    bool StepModalPressA(uint32_t held_mask, uint32_t current_frame, uint32_t now_ms, std::vector<uint32_t>* out_released_tokens = nullptr);
    bool StepModalCancelB(uint32_t held_mask, uint32_t current_frame, uint32_t now_ms, uint32_t* out_released_token = nullptr);

    void RefreshAllWidgets(uint32_t now_ms);

    const gpfocus::FocusGraph& GetRootGraph() const { return root_graph_; }
    gpfocus::FocusGraph& GetRootGraphMutable() { return root_graph_; }
    const gpfocus::FocusGraph& GetActiveGraph() const;
    const gpmodal::ModalStack& GetModalStack() const { return modal_stack_; }
    gpmodal::ModalStack& GetModalStackMutable() { return modal_stack_; }

    const TabStripWidget& GetTabStrip() const { return tab_strip_; }
    TabStripWidget& GetTabStripMutable() { return tab_strip_; }
    const ItemListGridWidget& GetItemCatalog() const { return item_catalog_; }
    ItemListGridWidget& GetItemCatalogMutable() { return item_catalog_; }
    const ComparisonCardWidget& GetComparisonCard() const { return comparison_card_; }
    ComparisonCardWidget& GetComparisonCardMutable() { return comparison_card_; }
    const QuantityModalWidget& GetQuantityModal() const { return quantity_modal_; }
    QuantityModalWidget& GetQuantityModalMutable() { return quantity_modal_; }
    const ConfirmDialogWidget& GetConfirmDialog() const { return confirm_dialog_; }
    ConfirmDialogWidget& GetConfirmDialogMutable() { return confirm_dialog_; }
    const FooterGlyphBarWidget& GetFooterGlyphBar() const { return footer_bar_; }
    FooterGlyphBarWidget& GetFooterGlyphBarMutable() { return footer_bar_; }

    const QuantitySelectionResult& GetLastQuantityResult() const { return last_qty_result_; }
    const ConfirmDialogResult& GetLastConfirmResult() const { return last_confirm_result_; }
    const std::string& GetLastStatusMessage() const { return last_status_; }
    uint32_t GetTotalInventoryMutations() const;
    bool HasActiveOverlayModal() const { return modal_stack_.GetDepth() >= 2u; }

    std::string FormatShowcaseDump() const;

private:
    void EnsureRootModalLayerOnStack(uint32_t now_ms);

    gpfocus::FocusGraph     root_graph_;
    gpmodal::ModalStack     modal_stack_;
    TabStripWidget          tab_strip_;
    ItemListGridWidget      item_catalog_;
    ComparisonCardWidget    comparison_card_;
    QuantityModalWidget     quantity_modal_;
    ConfirmDialogWidget     confirm_dialog_;
    FooterGlyphBarWidget    footer_bar_;
    QuantitySelectionResult last_qty_result_{};
    ConfirmDialogResult     last_confirm_result_{};
    std::string             last_status_;
};

using FWidgetTranslateCallback = const char* (*)(const char* stringId, const char* fallbackText);
void SetWidgetTranslateCallback(FWidgetTranslateCallback cb);

// Автономный самотест F2.1 (проверяет все 6 базовых виджетов и инвариант read-only)
gpfocus::SelfTestSummary RunWidgetsSelfTest();

} // namespace gpwidgets
