#include "GamepadWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace gpwidgets
{

namespace
{
FWidgetTranslateCallback g_widgetTranslateCb = nullptr;
bool g_inWidgetsSelfTest = false;

const char* TrWid(const char* stringId, const char* fallbackText)
{
    if (!g_inWidgetsSelfTest && g_widgetTranslateCb)
    {
        const char* res = g_widgetTranslateCb(stringId, fallbackText);
        if (res && res[0])
            return res;
    }
    return fallbackText;
}
} // namespace

void SetWidgetTranslateCallback(FWidgetTranslateCallback cb)
{
    g_widgetTranslateCb = cb;
}

static uint32_t FindOrAddGroup(gpfocus::FocusGraph& graph,
                               const char* name,
                               gpfocus::FocusLayoutMode mode,
                               uint8_t bag_id,
                               bool wrap)
{
    for (const gpfocus::FocusGroup& g : graph.GetGroups())
    {
        if (g.name == name)
            return g.group_id;
    }
    return graph.AddGroup(name, mode, bag_id, wrap);
}

const char* TopTabToLabelRU(TopTabId tab)
{
    switch (tab)
    {
    case TopTabId::Equipment: return TrWid("ui_gp_eng_wid_tab_equipment", "СНАРЯЖЕНИЕ");
    case TopTabId::Backpack:  return TrWid("ui_gp_eng_wid_tab_backpack", "РЮКЗАК");
    case TopTabId::Health:    return TrWid("ui_gp_eng_wid_tab_health", "ЗДОРОВЬЕ");
    case TopTabId::Magazines: return TrWid("ui_gp_eng_wid_tab_magazines", "МАГАЗИНЫ");
    case TopTabId::PDA:       return TrWid("ui_gp_eng_wid_tab_pda", "КПК");
    case TopTabId::Skills:    return TrWid("ui_gp_eng_wid_tab_skills", "НАВЫКИ");
    case TopTabId::Settings:  return TrWid("ui_gp_eng_wid_tab_settings", "НАСТРОЙКИ");
    default:                  return TrWid("ui_gp_eng_wid_tab_backpack", "РЮКЗАК");
    }
}

const char* CategoryFilterToLabelRU(CategoryFilterId cat)
{
    switch (cat)
    {
    case CategoryFilterId::All:            return TrWid("ui_gp_eng_wid_cat_all", "Все");
    case CategoryFilterId::Weapons:        return TrWid("ui_gp_eng_wid_cat_weapons", "Оружие");
    case CategoryFilterId::Ammo:           return TrWid("ui_gp_eng_wid_cat_ammo", "Боеприпасы");
    case CategoryFilterId::Meds:           return TrWid("ui_gp_eng_wid_cat_meds", "Медицина");
    case CategoryFilterId::Food:           return TrWid("ui_gp_eng_wid_cat_food", "Провизия");
    case CategoryFilterId::ArmorArtefacts: return TrWid("ui_gp_eng_wid_cat_armor_af", "Броня/Арт.");
    case CategoryFilterId::ToolsParts:     return TrWid("ui_gp_eng_wid_cat_tools", "Инструменты");
    case CategoryFilterId::Misc:           return TrWid("ui_gp_eng_wid_cat_misc", "Разное");
    default:                               return TrWid("ui_gp_eng_wid_cat_all", "Все");
    }
}

const char* CategoryFilterToBridgeId(CategoryFilterId cat)
{
    switch (cat)
    {
    case CategoryFilterId::All:            return "all";
    case CategoryFilterId::Weapons:        return "weapons";
    case CategoryFilterId::Ammo:           return "ammo";
    case CategoryFilterId::Meds:           return "meds";
    case CategoryFilterId::Food:           return "food";
    case CategoryFilterId::ArmorArtefacts: return "armor_af";
    case CategoryFilterId::ToolsParts:     return "tools_parts";
    case CategoryFilterId::Misc:           return "misc";
    default:                               return "all";
    }
}

const char* ViewModeToLabelRU(ItemViewMode mode)
{
    return (mode == ItemViewMode::Grid2D)
        ? TrWid("ui_gp_eng_wid_view_grid", "Сетка 4xN")
        : TrWid("ui_gp_eng_wid_view_list", "Список");
}

// ============================================================================
// 1. TabStripWidget
// ============================================================================

void TabStripWidget::InitializeInGraph(gpfocus::FocusGraph& graph)
{
    group_id_ = FindOrAddGroup(graph, "WidgetTabStrip", gpfocus::FocusLayoutMode::List1D, 10, true);
    SyncGraphNodes(graph, 0);
}

void TabStripWidget::SyncGraphNodes(gpfocus::FocusGraph& graph, uint32_t now_ms)
{
    if (group_id_ == gpfocus::kInvalidGroupId)
        return;

    std::vector<gpfocus::FocusNode> nodes;
    nodes.reserve(2);

    gpfocus::FocusNode topNode;
    topNode.node_id        = kTopTabNodeId;
    topNode.kind           = gpfocus::FocusNodeKind::GroupHeader;
    topNode.key.section    = "tab_header_lb_rb";
    topNode.key.list_index = 0;
    topNode.key.cell_indx  = static_cast<int32_t>(active_top_tab_);
    topNode.key.tab_id     = static_cast<uint8_t>(active_top_tab_);
    topNode.label          = std::string("[LB/RB] ") + TopTabToLabelRU(active_top_tab_);
    nodes.push_back(std::move(topNode));

    gpfocus::FocusNode catNode;
    catNode.node_id        = kCategoryTabNodeId;
    catNode.kind           = gpfocus::FocusNodeKind::GroupHeader;
    catNode.key.section    = "tab_filter_lt_rt";
    catNode.key.list_index = 1;
    catNode.key.cell_indx  = static_cast<int32_t>(active_category_);
    catNode.key.tab_id     = static_cast<uint8_t>(active_category_);
    catNode.label          = std::string("[LT/RT] ") + CategoryFilterToLabelRU(active_category_);
    nodes.push_back(std::move(catNode));

    graph.RebuildGroupItems(group_id_, nodes, static_cast<uint8_t>(active_category_), now_ms);
}

TopTabId TabStripWidget::StepTopTab(int delta, bool wrap)
{
    const int count = static_cast<int>(TopTabId::Count);
    int cur = static_cast<int>(active_top_tab_) + delta;
    if (wrap)
    {
        cur = ((cur % count) + count) % count;
    }
    else
    {
        cur = std::max(0, std::min(count - 1, cur));
    }
    active_top_tab_ = static_cast<TopTabId>(cur);
    return active_top_tab_;
}

CategoryFilterId TabStripWidget::StepCategoryFilter(int delta, bool wrap)
{
    const int count = static_cast<int>(CategoryFilterId::Count);
    int cur = static_cast<int>(active_category_) + delta;
    if (wrap)
    {
        cur = ((cur % count) + count) % count;
    }
    else
    {
        cur = std::max(0, std::min(count - 1, cur));
    }
    active_category_ = static_cast<CategoryFilterId>(cur);
    return active_category_;
}

void TabStripWidget::SetActorTelemetry(float current_weight_kg, float max_weight_kg, uint32_t money_ru, bool pause_active)
{
    current_weight_kg_ = current_weight_kg;
    max_weight_kg_     = max_weight_kg;
    money_ru_          = money_ru;
    pause_active_      = pause_active;
}

std::string TabStripWidget::FormatTopBarLine() const
{
    std::ostringstream oss;
    oss << "[LB] ";
    for (uint8_t i = 0; i < static_cast<uint8_t>(TopTabId::Count); ++i)
    {
        const TopTabId t = static_cast<TopTabId>(i);
        if (i > 0)
            oss << "  ";
        if (t == active_top_tab_)
            oss << "[" << TopTabToLabelRU(t) << "]";
        else
            oss << TopTabToLabelRU(t);
    }
    const char* pauseLbl = pause_active_
        ? TrWid("ui_gp_eng_wid_pause_on", "[ПАУЗА АКТИВНА]")
        : TrWid("ui_gp_eng_wid_pause_off", "[МИР АКТИВЕН]");
    char telBuf[256];
    std::snprintf(telBuf, sizeof(telBuf),
                  TrWid("ui_gp_eng_wid_topbar_telemetry_fmt", " [RB]  | Вес: %.1f/%.1f кг | %u RU | %s"),
                  current_weight_kg_, max_weight_kg_, money_ru_, pauseLbl);
    oss << telBuf;
    return oss.str();
}

std::string TabStripWidget::FormatCategoryFilterLine(ItemViewMode view_mode, size_t visible_items_count) const
{
    std::ostringstream oss;
    oss << "[LT] ";
    for (uint8_t i = 0; i < static_cast<uint8_t>(CategoryFilterId::Count); ++i)
    {
        const CategoryFilterId c = static_cast<CategoryFilterId>(i);
        if (i > 0)
            oss << " | ";
        if (c == active_category_)
            oss << "[" << CategoryFilterToLabelRU(c) << "]";
        else
            oss << CategoryFilterToLabelRU(c);
    }
    const char* viewLbl = ViewModeToLabelRU(view_mode);
    char catBuf[256];
    std::snprintf(catBuf, sizeof(catBuf),
                  TrWid("ui_gp_eng_wid_catbar_suffix_fmt", " [RT]   (Вид: %s, предм.: %zu)"),
                  viewLbl, visible_items_count);
    oss << catBuf;
    return oss.str();
}

// ============================================================================
// 2. ItemListGridWidget (D10)
// ============================================================================

void ItemListGridWidget::InitializeInGraph(gpfocus::FocusGraph& graph)
{
    group_id_ = FindOrAddGroup(graph, "WidgetItemCatalog", gpfocus::FocusLayoutMode::List1D, 1, false);
    scroller_.Configure(1, kListVisibleRows);
}

void ItemListGridWidget::SetCatalogReadOnly(const std::vector<WidgetItemSnapshot>& items,
                                            CategoryFilterId filter,
                                            gpfocus::FocusGraph& graph,
                                            uint32_t now_ms)
{
    all_items_     = items;
    active_filter_ = filter;
    RebuildGraphNodes(graph, now_ms);
}

gpfocus::FocusRestoreReason ItemListGridWidget::ApplyCategoryFilter(CategoryFilterId filter,
                                                                    gpfocus::FocusGraph& graph,
                                                                    uint32_t now_ms)
{
    active_filter_ = filter;
    return RebuildGraphNodes(graph, now_ms);
}

gpfocus::FocusRestoreReason ItemListGridWidget::ToggleViewMode(gpfocus::FocusGraph& graph, uint32_t now_ms)
{
    const ItemViewMode nextMode = (view_mode_ == ItemViewMode::List1D) ? ItemViewMode::Grid2D : ItemViewMode::List1D;
    return SetViewMode(nextMode, graph, now_ms);
}

gpfocus::FocusRestoreReason ItemListGridWidget::SetViewMode(ItemViewMode mode,
                                                            gpfocus::FocusGraph& graph,
                                                            uint32_t now_ms)
{
    view_mode_ = mode;
    return RebuildGraphNodes(graph, now_ms);
}

gpfocus::FocusRestoreReason ItemListGridWidget::RebuildGraphNodes(gpfocus::FocusGraph& graph, uint32_t now_ms)
{
    if (group_id_ == gpfocus::kInvalidGroupId)
        InitializeInGraph(graph);

    filtered_items_.clear();
    for (const WidgetItemSnapshot& itm : all_items_)
    {
        if (active_filter_ == CategoryFilterId::All || itm.category == active_filter_)
        {
            filtered_items_.push_back(itm);
        }
    }

    gpfocus::FocusGroup* grp = graph.GetGroupMutable(group_id_);
    if (grp)
    {
        grp->layout_mode = (view_mode_ == ItemViewMode::Grid2D)
            ? gpfocus::FocusLayoutMode::Grid2D
            : gpfocus::FocusLayoutMode::List1D;
    }

    std::vector<gpfocus::FocusNode> nodes;
    nodes.reserve(filtered_items_.size());

    for (size_t i = 0; i < filtered_items_.size(); ++i)
    {
        const WidgetItemSnapshot& src = filtered_items_[i];
        gpfocus::FocusNode n;
        n.node_id           = static_cast<uint32_t>(1000u + i);
        n.kind              = gpfocus::FocusNodeKind::Item;
        n.stable_id         = src.stable_id;
        n.key.section       = src.section;
        n.key.cond_permille = src.cond_permille;
        n.key.uses_or_ammo  = src.uses_or_ammo;
        n.key.list_index    = static_cast<int32_t>(i);
        n.key.cell_indx     = static_cast<int32_t>(i);
        n.key.bag_id        = 1;
        n.key.tab_id        = static_cast<uint8_t>(active_filter_);

        if (view_mode_ == ItemViewMode::Grid2D)
        {
            n.key.grid_x = static_cast<int16_t>(static_cast<int32_t>(i) % kGridColumns);
            n.key.grid_y = static_cast<int16_t>(static_cast<int32_t>(i) / kGridColumns);
            n.key.grid_w = 1;
            n.key.grid_h = 1;
        }
        else
        {
            n.key.grid_x = 0;
            n.key.grid_y = static_cast<int16_t>(i);
            n.key.grid_w = 1;
            n.key.grid_h = 1;
        }

        n.label = src.display_name.empty() ? src.section : src.display_name;
        nodes.push_back(std::move(n));
    }

    const gpfocus::FocusRestoreReason reason = graph.RebuildGroupItems(
        group_id_,
        nodes,
        static_cast<uint8_t>(active_filter_),
        now_ms);

    const int32_t totalRows = (view_mode_ == ItemViewMode::Grid2D)
        ? static_cast<int32_t>((filtered_items_.size() + static_cast<size_t>(kGridColumns) - 1u) / static_cast<size_t>(kGridColumns))
        : static_cast<int32_t>(filtered_items_.size());
    const int32_t visRows = (view_mode_ == ItemViewMode::Grid2D) ? kGridVisibleRows : kListVisibleRows;
    scroller_.Configure(std::max(1, totalRows), visRows);
    SyncScrollToFocusedNode(graph, now_ms);

    return reason;
}

int32_t ItemListGridWidget::GetFocusedItemIndex(const gpfocus::FocusGraph& graph) const
{
    if (group_id_ == gpfocus::kInvalidGroupId)
        return -1;
    const gpfocus::FocusNode* fn = graph.GetGroupFocusedNode(group_id_);
    if (!fn || fn->kind != gpfocus::FocusNodeKind::Item)
        return -1;
    return fn->key.list_index;
}

void ItemListGridWidget::SyncScrollToFocusedNode(const gpfocus::FocusGraph& graph, uint32_t now_ms)
{
    const int32_t idx = GetFocusedItemIndex(graph);
    if (idx < 0)
        return;

    const int32_t focusedRow = (view_mode_ == ItemViewMode::Grid2D)
        ? (idx / static_cast<int32_t>(kGridColumns))
        : idx;
    scroller_.EnsureFocusedVisible(focusedRow);
    scroller_.Step(now_ms);
}

void ItemListGridWidget::TickSmoothScroll(uint32_t now_ms)
{
    scroller_.Step(now_ms);
}

const WidgetItemSnapshot* ItemListGridWidget::GetFocusedItem(const gpfocus::FocusGraph& graph) const
{
    if (group_id_ == gpfocus::kInvalidGroupId)
        return nullptr;

    const gpfocus::FocusNode* fn = graph.GetGroupFocusedNode(group_id_);
    if (!fn || fn->kind != gpfocus::FocusNodeKind::Item)
        return nullptr;

    if (fn->key.list_index >= 0 && static_cast<size_t>(fn->key.list_index) < filtered_items_.size())
        return &filtered_items_[static_cast<size_t>(fn->key.list_index)];

    return FindItemByStableId(fn->stable_id);
}

const WidgetItemSnapshot* ItemListGridWidget::FindItemByStableId(const gpfocus::StableItemID& id) const
{
    if (!id.IsValid())
        return nullptr;
    for (const WidgetItemSnapshot& itm : all_items_)
    {
        if (itm.stable_id == id)
            return &itm;
    }
    return nullptr;
}

const WidgetItemSnapshot* ItemListGridWidget::FindEquippedItemForSlot(uint8_t equip_slot) const
{
    if (equip_slot == 0)
        return nullptr;
    for (const WidgetItemSnapshot& itm : all_items_)
    {
        if (itm.is_equipped && itm.equip_slot == equip_slot)
            return &itm;
    }
    return nullptr;
}

std::string ItemListGridWidget::FormatConditionBar(uint16_t cond_permille, int width_chars)
{
    const int clamped = std::max(0, std::min(1000, static_cast<int>(cond_permille)));
    const int pct     = (clamped + 5) / 10;
    const int filled  = (clamped * width_chars + 500) / 1000;
    std::string bar   = "[";
    for (int i = 0; i < width_chars; ++i)
    {
        bar += (i < filled) ? "#" : ".";
    }
    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), " %3d%%]", pct);
    bar += pctBuf;
    return bar;
}

std::string ItemListGridWidget::FormatListRow(size_t filtered_index, bool is_focused) const
{
    if (filtered_index >= filtered_items_.size())
    {
        return is_focused
            ? TrWid("ui_gp_eng_wid_empty_focused", " > [Пусто] В категории нет предметов — нажмите [LT/RT] для смены категории")
            : TrWid("ui_gp_eng_wid_empty_unfocused", "   [Пусто] В категории нет предметов");
    }

    const WidgetItemSnapshot& itm = filtered_items_[filtered_index];
    std::ostringstream oss;
    oss << (is_focused ? " > " : "   ");
    if (itm.is_favorite) oss << "[*] ";
    if (itm.is_equipped) oss << TrWid("ui_gp_eng_wid_tag_equipped", "[Надето] ");
    if (itm.is_junk)     oss << TrWid("ui_gp_eng_wid_tag_junk", "[Хлам] ");
    if (itm.is_quest)    oss << TrWid("ui_gp_eng_wid_tag_quest", "[Квест] ");

    oss << (itm.display_name.empty() ? itm.section : itm.display_name);
    if (itm.stack_count > 1)
        oss << " x" << itm.stack_count;

    oss << "  " << FormatConditionBar(itm.cond_permille, 8);
    if (!itm.ammo_or_caliber.empty())
        oss << "  [" << itm.ammo_or_caliber << "]";

    char wBuf[64];
    std::snprintf(wBuf, sizeof(wBuf), TrWid("ui_gp_eng_wid_weight_kg_fmt", "  %.2f кг"),
                  itm.unit_weight_kg * static_cast<float>(std::max<uint16_t>(1, itm.stack_count)));
    oss << wBuf;
    return oss.str();
}

std::string ItemListGridWidget::FormatGridCard(size_t filtered_index, bool is_focused) const
{
    if (filtered_index >= filtered_items_.size())
        return TrWid("ui_gp_eng_wid_empty_short", "[Пусто]");

    const WidgetItemSnapshot& itm = filtered_items_[filtered_index];
    const int pct = (static_cast<int>(itm.cond_permille) + 5) / 10;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s[%s%s %d%% x%u]%s",
                  is_focused ? "{" : " ",
                  itm.is_favorite ? "*" : "",
                  itm.section.c_str(),
                  pct,
                  static_cast<unsigned>(itm.stack_count),
                  is_focused ? "}" : " ");
    return std::string(buf);
}

// ============================================================================
// 3. ComparisonCardWidget (Accurate Defense Values)
// ============================================================================

void ComparisonCardWidget::InitializeInGraph(gpfocus::FocusGraph& graph)
{
    group_id_ = FindOrAddGroup(graph, "WidgetComparisonCard", gpfocus::FocusLayoutMode::List1D, 11, false);

    std::vector<gpfocus::FocusNode> nodes;
    gpfocus::FocusNode cardNode;
    cardNode.node_id        = kComparisonCardNodeId;
    cardNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    cardNode.key.section    = "comparison_card_adv";
    cardNode.key.list_index = 0;
    cardNode.label          = TrWid("ui_gp_eng_wid_card_node_title", "Карточка сравнения (Accurate Defense Values)");
    nodes.push_back(std::move(cardNode));

    graph.RebuildGroupItems(group_id_, nodes, 0, 0);
    scroller_.Configure(8, kVisibleStatLines);
}

uint8_t ComparisonCardWidget::ToggleWeaponCompareSlot(const WidgetItemSnapshot* focused_item,
                                                      const ItemListGridWidget& catalog,
                                                      uint32_t now_ms)
{
    weapon_compare_slot_ = (weapon_compare_slot_ == 2) ? 3 : 2;
    RefreshFromFocusedItem(focused_item, catalog, now_ms);
    return weapon_compare_slot_;
}

bool ComparisonCardWidget::ToggleFullStatsPreview(const WidgetItemSnapshot* focused_item,
                                                  const ItemListGridWidget& catalog,
                                                  uint32_t now_ms)
{
    show_full_stats_100_ = !show_full_stats_100_;
    RefreshFromFocusedItem(focused_item, catalog, now_ms);
    return show_full_stats_100_;
}

void ComparisonCardWidget::RefreshFromFocusedItem(const WidgetItemSnapshot* focused_item,
                                                  const ItemListGridWidget& catalog,
                                                  uint32_t now_ms)
{
    computed_rows_.clear();
    formatted_lines_.clear();

    if (!focused_item)
    {
        has_focused_item_  = false;
        has_compared_item_ = false;
        formatted_lines_.push_back(TrWid("ui_gp_eng_wid_card_empty_1", "Нет выбранного предмета (пустая категория)"));
        formatted_lines_.push_back(TrWid("ui_gp_eng_wid_card_empty_2", "Используйте [LT/RT] для выбора категории с предметами."));
        scroller_.Configure(static_cast<int32_t>(formatted_lines_.size()), kVisibleStatLines);
        return;
    }

    has_focused_item_ = true;
    current_item_     = *focused_item;
    displayed_adv_    = show_full_stats_100_ ? focused_item->adv_full_100 : focused_item->adv;

    // Определяем целевой слот для сравнения (без мутации инвентаря)
    uint8_t targetSlot = focused_item->equip_slot;
    if (focused_item->stats_group == "weapon")
    {
        targetSlot = weapon_compare_slot_;
    }
    else if (focused_item->stats_group == "outfit")
    {
        targetSlot = 7;
    }
    else if (focused_item->stats_group == "helmet")
    {
        targetSlot = 12;
    }

    const WidgetItemSnapshot* eqItem = catalog.FindEquippedItemForSlot(targetSlot);
    if (eqItem && !(eqItem->stable_id == focused_item->stable_id))
    {
        has_compared_item_ = true;
        compared_item_     = *eqItem;
    }
    else
    {
        has_compared_item_ = false;
        compared_item_     = WidgetItemSnapshot{};
    }

    const AccurateDefenseSnapshot cmpAdv = has_compared_item_
        ? (show_full_stats_100_ ? compared_item_.adv_full_100 : compared_item_.adv)
        : AccurateDefenseSnapshot{};

    auto pushCompareRow = [&](const std::string& id,
                              const std::string& lbl,
                              const std::string& unit,
                              float val,
                              float cmpVal,
                              bool canCompare)
    {
        StatCompareRow r;
        r.stat_id       = id;
        r.label         = lbl;
        r.unit          = unit;
        r.value         = val;
        r.compare_value = cmpVal;
        r.has_compare   = canCompare;
        r.delta         = canCompare ? (val - cmpVal) : 0.0f;
        if (canCompare && std::fabs(r.delta) > 0.05f)
            r.delta_sign = (r.delta > 0.0f) ? '+' : '-';
        else
            r.delta_sign = '=';
        computed_rows_.push_back(r);
    };

    const float condScale = show_full_stats_100_
        ? (1000.0f / static_cast<float>(std::max<uint16_t>(1, focused_item->cond_permille)))
        : 1.0f;

    for (const StatCompareRow& baseRow : focused_item->base_stats)
    {
        float val = baseRow.value;
        if (show_full_stats_100_ && (focused_item->stats_group == "outfit" || focused_item->stats_group == "helmet"))
            val = std::min(100.0f, baseRow.value * condScale);

        float cmpVal = baseRow.compare_value;
        bool hasCmp  = baseRow.has_compare;
        if (has_compared_item_)
        {
            for (const StatCompareRow& cr : compared_item_.base_stats)
            {
                if (cr.stat_id == baseRow.stat_id)
                {
                    cmpVal = cr.value;
                    if (show_full_stats_100_ && (compared_item_.stats_group == "outfit" || compared_item_.stats_group == "helmet"))
                    {
                        const float cmpCondScale = 1000.0f / static_cast<float>(std::max<uint16_t>(1, compared_item_.cond_permille));
                        cmpVal = std::min(100.0f, cr.value * cmpCondScale);
                    }
                    hasCmp = true;
                    break;
                }
            }
        }
        pushCompareRow(baseRow.stat_id, baseRow.label, baseRow.unit, val, cmpVal, hasCmp);
    }

    // Добавляем статы Accurate Defense Values для брони / шлемов / артефактов
    if (focused_item->stats_group == "outfit" || focused_item->stats_group == "helmet" || focused_item->stats_group == "artefact")
    {
        const std::string unitClass = TrWid("ui_gp_eng_wid_adv_unit_class", "кл.");
        pushCompareRow("adv_br_class",   TrWid("ui_gp_eng_wid_adv_br_class", "ADV Класс брони (BR)"),        unitClass, static_cast<float>(displayed_adv_.br_class), static_cast<float>(cmpAdv.br_class), has_compared_item_);
        pushCompareRow("adv_fire_wound", TrWid("ui_gp_eng_wid_adv_fire_wound", "ADV Баллистика (FireWound)"),  "%",     displayed_adv_.fire_wound_prot * 100.0f,     cmpAdv.fire_wound_prot * 100.0f,     has_compared_item_);
        pushCompareRow("adv_ap_res",     TrWid("ui_gp_eng_wid_adv_ap_res", "ADV Стойкость к ББ (AP Res)"), "%",         displayed_adv_.ap_res * 100.0f,              cmpAdv.ap_res * 100.0f,              has_compared_item_);
        pushCompareRow("adv_wound",      TrWid("ui_gp_eng_wid_adv_wound", "ADV Разрыв (Wound)"),          "%",           displayed_adv_.wound_prot * 100.0f,          cmpAdv.wound_prot * 100.0f,          has_compared_item_);
        pushCompareRow("adv_strike",     TrWid("ui_gp_eng_wid_adv_strike", "ADV Удар (Strike)"),           "%",          displayed_adv_.strike_prot * 100.0f,         cmpAdv.strike_prot * 100.0f,         has_compared_item_);
        pushCompareRow("adv_explosion",  TrWid("ui_gp_eng_wid_adv_explosion", "ADV Взрыв (Explosion)"),       "%",       displayed_adv_.expl_prot * 100.0f,           cmpAdv.expl_prot * 100.0f,           has_compared_item_);
        pushCompareRow("adv_burn",       TrWid("ui_gp_eng_wid_adv_burn", "ADV Ожог (Burn)"),             "%",            displayed_adv_.burn_prot * 100.0f,           cmpAdv.burn_prot * 100.0f,           has_compared_item_);
        pushCompareRow("adv_shock",      TrWid("ui_gp_eng_wid_adv_shock", "ADV Электрошок (Shock)"),      "%",           displayed_adv_.shock_prot * 100.0f,          cmpAdv.shock_prot * 100.0f,          has_compared_item_);
        pushCompareRow("adv_acid",       TrWid("ui_gp_eng_wid_adv_acid", "ADV Хим. ожог (Acid)"),        "%",            displayed_adv_.acid_prot * 100.0f,           cmpAdv.acid_prot * 100.0f,           has_compared_item_);
        pushCompareRow("adv_psi",        TrWid("ui_gp_eng_wid_adv_psi", "ADV Пси-защита (Psi)"),        "%",             displayed_adv_.psi_prot * 100.0f,            cmpAdv.psi_prot * 100.0f,            has_compared_item_);
        pushCompareRow("adv_radia",      TrWid("ui_gp_eng_wid_adv_radia", "ADV Радиация (Radia)"),        "%",           displayed_adv_.radia_prot * 100.0f,          cmpAdv.radia_prot * 100.0f,          has_compared_item_);
    }

    const char* modeLbl = show_full_stats_100_
        ? TrWid("ui_gp_eng_wid_mode_100", "[РЕЖИМ 100%]")
        : TrWid("ui_gp_eng_wid_mode_cur", "[ТЕКУЩЕЕ]");
    char hdrBuf[512];
    std::snprintf(hdrBuf, sizeof(hdrBuf),
                  TrWid("ui_gp_eng_wid_card_hdr_fmt", "%s (%s) | Сост.: %d%% %s | Вес: %.2f кг | %u RU"),
                  focused_item->display_name.c_str(),
                  focused_item->section.c_str(),
                  (static_cast<int>(focused_item->cond_permille) + 5) / 10,
                  modeLbl,
                  focused_item->unit_weight_kg,
                  focused_item->unit_cost_ru);
    formatted_lines_.push_back(hdrBuf);

    if (has_compared_item_)
    {
        std::snprintf(hdrBuf, sizeof(hdrBuf),
                      TrWid("ui_gp_eng_wid_card_cmp_item_fmt", "Сравнение со слотом #%u: %s (%d%%)  [R3 — сменить слот / 100%%]"),
                      static_cast<unsigned>(targetSlot),
                      compared_item_.display_name.c_str(),
                      (static_cast<int>(compared_item_.cond_permille) + 5) / 10);
        formatted_lines_.push_back(hdrBuf);
    }
    else
    {
        const char* slotStateLbl = focused_item->is_equipped
            ? TrWid("ui_gp_eng_wid_card_slot_equipped", "этот предмет уже надет")
            : TrWid("ui_gp_eng_wid_card_slot_empty", "слот свободен");
        std::snprintf(hdrBuf, sizeof(hdrBuf),
                      TrWid("ui_gp_eng_wid_card_cmp_slot_fmt", "Сравнение: слот #%u (%s)"),
                      static_cast<unsigned>(targetSlot),
                      slotStateLbl);
        formatted_lines_.push_back(hdrBuf);
    }

    for (const StatCompareRow& r : computed_rows_)
    {
        char lineBuf[256];
        if (r.has_compare && r.delta_sign != '=')
        {
            std::snprintf(lineBuf, sizeof(lineBuf), "  %-28s : %6.1f%s  (%c%.1f%s %s)",
                          r.label.c_str(),
                          r.value,
                          r.unit.c_str(),
                          r.delta_sign,
                          std::fabs(r.delta),
                          r.unit.c_str(),
                          (r.delta_sign == '+') ? "▲" : "▼");
        }
        else
        {
            std::snprintf(lineBuf, sizeof(lineBuf), "  %-28s : %6.1f%s",
                          r.label.c_str(),
                          r.value,
                          r.unit.c_str());
        }
        formatted_lines_.push_back(lineBuf);
    }

    if (!focused_item->wpo_parts_summary.empty())
    {
        formatted_lines_.push_back(std::string(TrWid("ui_gp_eng_wid_wpo_prefix", "Детали WPO: ")) + focused_item->wpo_parts_summary);
    }

    char actorAdvBuf[512];
    std::snprintf(actorAdvBuf, sizeof(actorAdvBuf),
                  TrWid("ui_gp_eng_wid_actor_body_fmt", "Актор ADV [Корпус]: BR %d (премитигация <%d%%) | Огнестрел %.0f%% | Разрыв %.0f%% | Хим %.0f%% | Пси %.0f%%"),
                  displayed_adv_.body_br_class,
                  displayed_adv_.body_premitigation_pct,
                  displayed_adv_.body_fire_wound * 100.0f,
                  displayed_adv_.body_wound * 100.0f,
                  displayed_adv_.body_acid * 100.0f,
                  displayed_adv_.body_psi * 100.0f);
    formatted_lines_.push_back(actorAdvBuf);

    std::snprintf(actorAdvBuf, sizeof(actorAdvBuf),
                  TrWid("ui_gp_eng_wid_actor_head_fmt", "Актор ADV [Голова]: BR %d (премитигация <%d%%) | Огнестрел %.0f%% | Пси %.0f%%"),
                  displayed_adv_.head_br_class,
                  displayed_adv_.head_premitigation_pct,
                  displayed_adv_.head_fire_wound * 100.0f,
                  displayed_adv_.head_psi * 100.0f);
    formatted_lines_.push_back(actorAdvBuf);

    scroller_.Configure(static_cast<int32_t>(formatted_lines_.size()), kVisibleStatLines);
    scroller_.Step(now_ms);
}

void ComparisonCardWidget::ScrollByStick(float right_stick_y, uint32_t dt_continual_ms, uint32_t now_ms)
{
    if (std::fabs(right_stick_y) > 0.25f)
    {
        scroller_.AddAnalogScrollVelocity(right_stick_y, 8.0f, dt_continual_ms);
    }
    scroller_.Step(now_ms);
}

// ============================================================================
// 4. QuantityModalWidget
// ============================================================================

bool QuantityModalWidget::OpenOnStack(gpmodal::ModalStack& modal_stack,
                                      gpfocus::FocusGraph& root_graph,
                                      const WidgetItemSnapshot& item,
                                      int32_t max_by_weight,
                                      int32_t max_by_money,
                                      uint32_t pause_token_id,
                                      uint32_t currently_held_buttons_mask,
                                      uint32_t now_ms)
{
    item_ = item;
    int32_t maxQty = std::max<int32_t>(1, static_cast<int32_t>(item.stack_count));
    if (max_by_weight > 0)
        maxQty = std::min(maxQty, max_by_weight);
    if (max_by_money > 0)
        maxQty = std::min(maxQty, max_by_money);
    effective_max_qty_ = std::max<int32_t>(1, maxQty);
    current_qty_       = 1;
    pack_step_         = 10;
    repeat_timer_.Reset();

    const uint32_t modalId = modal_stack.PushModal(
        "modal_quantity_picker",
        TrWid("ui_gp_eng_wid_qty_title", "Выбор количества"),
        root_graph,
        pause_token_id,
        currently_held_buttons_mask,
        now_ms);
    if (modalId == gpmodal::kInvalidModalId)
        return false;

    gpmodal::ModalLayer* top = modal_stack.GetTopLayerMutable();
    if (!top)
        return false;

    top->local_graph.Clear();
    const uint32_t grpId = top->local_graph.AddGroup("QuantityModalGroup", gpfocus::FocusLayoutMode::List1D, 20, false);
    std::vector<gpfocus::FocusNode> nodes;

    gpfocus::FocusNode sliderNode;
    sliderNode.node_id        = kSliderNodeId;
    sliderNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    sliderNode.stable_id      = item.stable_id;
    sliderNode.key.section    = item.section;
    sliderNode.key.list_index = 0;
    sliderNode.label          = TrWid("ui_gp_eng_wid_qty_slider", "Ползунок количества [< / > +-1 | LB/RB +-10 | LT/RT Мин/Макс]");
    nodes.push_back(std::move(sliderNode));

    gpfocus::FocusNode okNode;
    okNode.node_id        = kConfirmNodeId;
    okNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    okNode.stable_id      = item.stable_id;
    okNode.key.section    = "qty_confirm_btn";
    okNode.key.list_index = 1;
    okNode.label          = TrWid("ui_gp_eng_wid_qty_confirm", "[A] Принять выбранное количество");
    nodes.push_back(std::move(okNode));

    gpfocus::FocusNode cancelNode;
    cancelNode.node_id        = kCancelNodeId;
    cancelNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    cancelNode.stable_id      = item.stable_id;
    cancelNode.key.section    = "qty_cancel_btn";
    cancelNode.key.list_index = 2;
    cancelNode.label          = TrWid("ui_gp_eng_wid_qty_cancel", "[B] Отмена");
    nodes.push_back(std::move(cancelNode));

    top->local_graph.RebuildGroupItems(grpId, nodes, 0, now_ms);
    top->local_graph.SwitchToGroup(grpId, now_ms);
    return true;
}

int32_t QuantityModalWidget::StepQuantity(int32_t delta)
{
    current_qty_ = std::max<int32_t>(1, std::min<int32_t>(effective_max_qty_, current_qty_ + delta));
    return current_qty_;
}

int32_t QuantityModalWidget::StepByPack(int32_t direction)
{
    const int32_t step = (direction >= 0) ? pack_step_ : -pack_step_;
    return StepQuantity(step);
}

int32_t QuantityModalWidget::SetToMin()
{
    current_qty_ = 1;
    return current_qty_;
}

int32_t QuantityModalWidget::SetToMax()
{
    current_qty_ = effective_max_qty_;
    return current_qty_;
}

int32_t QuantityModalWidget::UpdateHoldRepeat(gpfocus::NavDir held_dir, bool is_held, uint32_t now_ms)
{
    const bool validDir = is_held && (held_dir == gpfocus::NavDir::Left || held_dir == gpfocus::NavDir::Right);
    const uint32_t pulses = repeat_timer_.Update(validDir, held_dir, now_ms);
    if (pulses > 0 && validDir)
    {
        const int32_t delta = (held_dir == gpfocus::NavDir::Right) ? 1 : -1;
        StepQuantity(delta * static_cast<int32_t>(pulses));
    }
    return current_qty_;
}

QuantitySelectionResult QuantityModalWidget::ConfirmSelection() const
{
    QuantitySelectionResult res;
    res.confirmed         = true;
    res.cancelled         = false;
    res.stable_id         = item_.stable_id;
    res.section           = item_.section;
    res.selected_quantity = current_qty_;
    res.max_quantity      = effective_max_qty_;
    res.total_weight_kg   = GetTotalWeightKg();
    res.total_cost_ru     = GetTotalCostRu();
    return res;
}

QuantitySelectionResult QuantityModalWidget::CancelSelection() const
{
    QuantitySelectionResult res;
    res.confirmed         = false;
    res.cancelled         = true;
    res.stable_id         = item_.stable_id;
    res.section           = item_.section;
    res.selected_quantity = 0;
    res.max_quantity      = effective_max_qty_;
    res.total_weight_kg   = 0.0f;
    res.total_cost_ru     = 0u;
    return res;
}

bool QuantityModalWidget::IsOpen(const gpmodal::ModalStack& modal_stack) const
{
    const gpmodal::ModalLayer* top = modal_stack.GetTopLayer();
    return top && top->tag == "modal_quantity_picker";
}

std::string QuantityModalWidget::FormatProgressBar(int width_chars) const
{
    const int filled = (effective_max_qty_ > 0)
        ? ((current_qty_ * width_chars + effective_max_qty_ / 2) / effective_max_qty_)
        : width_chars;
    std::string bar = "[";
    for (int i = 0; i < width_chars; ++i)
    {
        bar += (i < filled) ? "=" : ".";
    }
    bar += "]";
    return bar;
}

std::string QuantityModalWidget::FormatModalSummaryLine() const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  TrWid("ui_gp_eng_wid_qty_summary_fmt", "Выбор количества: %s | %s %d / %d шт. | Вес: %.2f кг | Сумма: %u RU"),
                  item_.display_name.empty() ? item_.section.c_str() : item_.display_name.c_str(),
                  FormatProgressBar(12).c_str(),
                  current_qty_,
                  effective_max_qty_,
                  GetTotalWeightKg(),
                  GetTotalCostRu());
    return std::string(buf);
}

// ============================================================================
// 5. ConfirmDialogWidget
// ============================================================================

bool ConfirmDialogWidget::OpenOnStack(gpmodal::ModalStack& modal_stack,
                                      gpfocus::FocusGraph& root_graph,
                                      const gpfocus::StableItemID& subject_id,
                                      const std::string& title,
                                      const std::string& warning_message,
                                      const std::string& action_tag,
                                      bool default_focus_cancel,
                                      uint32_t pause_token_id,
                                      uint32_t currently_held_buttons_mask,
                                      uint32_t now_ms)
{
    subject_id_      = subject_id;
    title_           = title;
    warning_message_ = warning_message;
    action_tag_      = action_tag;

    const uint32_t modalId = modal_stack.PushModal(
        "modal_confirm_dialog",
        title,
        root_graph,
        pause_token_id,
        currently_held_buttons_mask,
        now_ms);
    if (modalId == gpmodal::kInvalidModalId)
        return false;

    gpmodal::ModalLayer* top = modal_stack.GetTopLayerMutable();
    if (!top)
        return false;

    top->local_graph.Clear();
    const uint32_t grpId = top->local_graph.AddGroup("ConfirmDialogGroup", gpfocus::FocusLayoutMode::List1D, 21, false);
    std::vector<gpfocus::FocusNode> nodes;

    gpfocus::FocusNode yesNode;
    yesNode.node_id        = kConfirmYesNodeId;
    yesNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    yesNode.stable_id      = subject_id;
    yesNode.key.section    = "confirm_yes";
    yesNode.key.list_index = 0;
    yesNode.label          = TrWid("ui_gp_eng_wid_confirm_yes", "[Да, подтвердить]");
    nodes.push_back(std::move(yesNode));

    gpfocus::FocusNode noNode;
    noNode.node_id        = kConfirmNoNodeId;
    noNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    noNode.stable_id      = subject_id;
    noNode.key.section    = "confirm_cancel";
    noNode.key.list_index = 1;
    noNode.label          = TrWid("ui_gp_eng_wid_confirm_no", "[Отмена]");
    nodes.push_back(std::move(noNode));

    top->local_graph.RebuildGroupItems(grpId, nodes, 0, now_ms);
    top->local_graph.SwitchToGroup(grpId, now_ms);
    if (default_focus_cancel)
    {
        top->local_graph.SetFocusedNode(grpId, kConfirmNoNodeId, true);
    }
    return true;
}

bool ConfirmDialogWidget::IsOpen(const gpmodal::ModalStack& modal_stack) const
{
    const gpmodal::ModalLayer* top = modal_stack.GetTopLayer();
    return top && top->tag == "modal_confirm_dialog";
}

bool ConfirmDialogWidget::IsYesFocused(const gpmodal::ModalStack& modal_stack) const
{
    if (!IsOpen(modal_stack))
        return false;
    const gpfocus::FocusNode* fn = modal_stack.GetTopLayer()->local_graph.GetFocusedNode();
    return fn && fn->node_id == kConfirmYesNodeId;
}

ConfirmDialogResult ConfirmDialogWidget::ActivateFocusedChoice(const gpmodal::ModalStack& modal_stack) const
{
    ConfirmDialogResult res;
    res.subject_id = subject_id_;
    res.action_tag = action_tag_;
    if (IsYesFocused(modal_stack))
    {
        res.confirmed = true;
        res.cancelled = false;
    }
    else
    {
        res.confirmed = false;
        res.cancelled = true;
    }
    return res;
}

ConfirmDialogResult ConfirmDialogWidget::CancelDialog() const
{
    ConfirmDialogResult res;
    res.confirmed  = false;
    res.cancelled  = true;
    res.subject_id = subject_id_;
    res.action_tag = action_tag_;
    return res;
}

std::string ConfirmDialogWidget::FormatDialogLine(const gpmodal::ModalStack& modal_stack) const
{
    const bool yesFocused = IsYesFocused(modal_stack);
    const std::string yesLbl = TrWid("ui_gp_eng_wid_confirm_yes", "[Да, подтвердить]");
    const std::string noLbl  = TrWid("ui_gp_eng_wid_confirm_no", "[Отмена]");
    std::ostringstream oss;
    oss << title_ << " - " << warning_message_ << "   "
        << (yesFocused ? (">> " + yesLbl + " <<") : ("   " + yesLbl + "   "))
        << "  "
        << (!yesFocused ? (">> " + noLbl + " <<") : ("   " + noLbl + "   "));
    return oss.str();
}

// ============================================================================
// 6. FooterGlyphBarWidget (D5.1, D20, D38)
// ============================================================================

void FooterGlyphBarWidget::InitializeInGraph(gpfocus::FocusGraph& graph)
{
    group_id_ = FindOrAddGroup(graph, "WidgetFooterGlyphBar", gpfocus::FocusLayoutMode::List1D, 12, false);

    std::vector<gpfocus::FocusNode> nodes;
    gpfocus::FocusNode barNode;
    barNode.node_id        = kFooterBarNodeId;
    barNode.kind           = gpfocus::FocusNodeKind::ControlWidget;
    barNode.key.section    = "footer_glyph_bar_d5_1";
    barNode.key.list_index = 0;
    barNode.label          = "Нижняя панель динамических глифов (D5.1)";
    nodes.push_back(std::move(barNode));

    graph.RebuildGroupItems(group_id_, nodes, 0, 0);
}

void FooterGlyphBarWidget::SetGlyphStyle(WidgetGlyphStyle style, WidgetFaceStyle face_style, bool is_russian)
{
    glyph_style_ = style;
    face_style_  = face_style;
    is_russian_  = is_russian;
}

const char* FooterGlyphBarWidget::ResolveButtonBadge(LogicalPromptButton btn,
                                                     WidgetGlyphStyle style,
                                                     WidgetFaceStyle face_style,
                                                     bool /*is_russian*/)
{
    switch (btn)
    {
    case LogicalPromptButton::South:
        if (style == WidgetGlyphStyle::PlayStation || face_style == WidgetFaceStyle::Sony) return "[Cross]";
        if (style == WidgetGlyphStyle::Nintendo    || face_style == WidgetFaceStyle::BAYX) return "[B]";
        if (style == WidgetGlyphStyle::Generic     || face_style == WidgetFaceStyle::Numbered) return "[B1]";
        return "[A]";
    case LogicalPromptButton::East:
        if (style == WidgetGlyphStyle::PlayStation || face_style == WidgetFaceStyle::Sony) return "[Circle]";
        if (style == WidgetGlyphStyle::Nintendo    || face_style == WidgetFaceStyle::BAYX) return "[A]";
        if (face_style == WidgetFaceStyle::AXBY) return "[X]";
        if (style == WidgetGlyphStyle::Generic     || face_style == WidgetFaceStyle::Numbered) return "[B2]";
        return "[B]";
    case LogicalPromptButton::West:
        if (style == WidgetGlyphStyle::PlayStation || face_style == WidgetFaceStyle::Sony) return "[Square]";
        if (style == WidgetGlyphStyle::Nintendo    || face_style == WidgetFaceStyle::BAYX) return "[Y]";
        if (face_style == WidgetFaceStyle::AXBY) return "[B]";
        if (style == WidgetGlyphStyle::Generic     || face_style == WidgetFaceStyle::Numbered) return "[B3]";
        return "[X]";
    case LogicalPromptButton::North:
        if (style == WidgetGlyphStyle::PlayStation || face_style == WidgetFaceStyle::Sony) return "[Triangle]";
        if (style == WidgetGlyphStyle::Nintendo    || face_style == WidgetFaceStyle::BAYX) return "[X]";
        if (style == WidgetGlyphStyle::Generic     || face_style == WidgetFaceStyle::Numbered) return "[B4]";
        return "[Y]";
    case LogicalPromptButton::LB:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[L1]";
        if (style == WidgetGlyphStyle::Nintendo) return "[L]";
        return "[LB]";
    case LogicalPromptButton::RB:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[R1]";
        if (style == WidgetGlyphStyle::Nintendo) return "[R]";
        return "[RB]";
    case LogicalPromptButton::LBRB:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[L1/R1]";
        if (style == WidgetGlyphStyle::Nintendo) return "[L/R]";
        return "[LB/RB]";
    case LogicalPromptButton::LT:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[L2]";
        if (style == WidgetGlyphStyle::Nintendo) return "[ZL]";
        return "[LT]";
    case LogicalPromptButton::RT:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[R2]";
        if (style == WidgetGlyphStyle::Nintendo) return "[ZR]";
        return "[RT]";
    case LogicalPromptButton::LTRT:
        if (style == WidgetGlyphStyle::PlayStation || style == WidgetGlyphStyle::Generic) return "[L2/R2]";
        if (style == WidgetGlyphStyle::Nintendo) return "[ZL/ZR]";
        return "[LT/RT]";
    case LogicalPromptButton::DPadLR:
        return "[◄/►]";
    case LogicalPromptButton::DPadUD:
        return "[▲/▼]";
    case LogicalPromptButton::R3:
        return "[R3]";
    case LogicalPromptButton::RightStick:
        return "[RS]";
    case LogicalPromptButton::View:
        if (style == WidgetGlyphStyle::PlayStation) return "[Share]";
        if (style == WidgetGlyphStyle::Nintendo)    return "[-]";
        if (style == WidgetGlyphStyle::Generic)     return "[Back]";
        return "[View]";
    default:
        return "[?]";
    }
}

const char* FooterGlyphBarWidget::ResolveButtonTextureId(LogicalPromptButton btn,
                                                         WidgetGlyphStyle style,
                                                         WidgetFaceStyle /*face_style*/)
{
    const char* prefix = "xbox";
    switch (style)
    {
    case WidgetGlyphStyle::PlayStation: prefix = "ps";       break;
    case WidgetGlyphStyle::Nintendo:    prefix = "nintendo"; break;
    case WidgetGlyphStyle::Generic:     prefix = "generic";  break;
    case WidgetGlyphStyle::Xbox:
    default:                            prefix = "xbox";     break;
    }

    const char* suffix = "dpad";
    switch (btn)
    {
    case LogicalPromptButton::South:      suffix = "south";     break;
    case LogicalPromptButton::East:       suffix = "east";      break;
    case LogicalPromptButton::West:       suffix = "west";      break;
    case LogicalPromptButton::North:      suffix = "north";     break;
    case LogicalPromptButton::LB:
    case LogicalPromptButton::LBRB:       suffix = "lshoulder"; break;
    case LogicalPromptButton::LT:
    case LogicalPromptButton::LTRT:       suffix = "ltrigger";  break;
    case LogicalPromptButton::R3:         suffix = "rstick";    break;
    case LogicalPromptButton::RightStick: suffix = "rs";        break;
    case LogicalPromptButton::View:       suffix = "back";      break;
    default:                              suffix = "dpad";      break;
    }

    static thread_local char s_widgetTexBuf[64];
    std::snprintf(s_widgetTexBuf, sizeof(s_widgetTexBuf), "ui_gp_white_%s_%s", prefix, suffix);
    return s_widgetTexBuf;
}

void FooterGlyphBarWidget::RebuildPrompts(const gpfocus::FocusGraph& root_graph,
                                          const gpmodal::ModalStack& modal_stack,
                                          const ItemListGridWidget& item_widget,
                                          const ComparisonCardWidget& comparison_widget)
{
    prompts_.clear();

    auto addPrompt = [&](LogicalPromptButton btn, const std::string& label)
    {
        GlyphPromptEntry e;
        e.button       = btn;
        e.badge_text   = ResolveButtonBadge(btn, glyph_style_, face_style_, is_russian_);
        e.texture_id   = ResolveButtonTextureId(btn, glyph_style_, face_style_);
        e.action_label = label;
        prompts_.push_back(std::move(e));
    };

    if (modal_stack.GetDepth() >= 2u)
    {
        const gpmodal::ModalLayer* top = modal_stack.GetTopLayer();
        if (top && top->tag == "modal_confirm_dialog")
        {
            addPrompt(LogicalPromptButton::South,  TrWid("ui_gp_eng_wid_prompt_confirm_choice", "Подтвердить выбор"));
            addPrompt(LogicalPromptButton::DPadUD, TrWid("ui_gp_eng_wid_prompt_select_btn", "Выбор кнопки"));
            addPrompt(LogicalPromptButton::East,   TrWid("ui_gp_eng_wid_prompt_cancel_back", "Отмена (назад)"));
            return;
        }
        if (top && top->tag == "modal_quantity_picker")
        {
            addPrompt(LogicalPromptButton::DPadLR, TrWid("ui_gp_eng_wid_prompt_qty_step", "Кол-во +-1 (удерж.)"));
            addPrompt(LogicalPromptButton::LBRB,   TrWid("ui_gp_eng_wid_prompt_step_10", "Шаг +-10"));
            addPrompt(LogicalPromptButton::LTRT,   TrWid("ui_gp_eng_wid_prompt_min_max", "Мин / Макс"));
            addPrompt(LogicalPromptButton::South,  TrWid("ui_gp_eng_wid_prompt_accept", "Принять"));
            addPrompt(LogicalPromptButton::East,   TrWid("ui_gp_eng_wid_prompt_cancel", "Отмена"));
            return;
        }
    }

    const WidgetItemSnapshot* focusedItem = item_widget.GetFocusedItem(root_graph);
    const std::string viewToggleLabel = (item_widget.GetViewMode() == ItemViewMode::List1D)
        ? TrWid("ui_gp_eng_wid_prompt_view_grid", "Вид: Сетка 4xN")
        : TrWid("ui_gp_eng_wid_prompt_view_list", "Вид: Список");

    // Запрет мёртвых подсказок (docs/12-ui-wireframes.md:36):
    // Если категория пуста (фокус на EmptyStatePlaceholder), подсказки предметных действий (A, X, R3) НЕ показываются!
    if (focusedItem != nullptr)
    {
        addPrompt(LogicalPromptButton::South, TrWid("ui_gp_eng_wid_prompt_confirm_dlg", "Диалог подтверждения"));
        if (focusedItem->stack_count > 1)
        {
            addPrompt(LogicalPromptButton::West, TrWid("ui_gp_eng_wid_prompt_select_qty", "Выбор количества"));
        }
        addPrompt(LogicalPromptButton::North, viewToggleLabel);
        addPrompt(LogicalPromptButton::LBRB,  TrWid("ui_gp_eng_wid_prompt_section", "Раздел"));
        addPrompt(LogicalPromptButton::LTRT,  TrWid("ui_gp_eng_wid_prompt_category", "Категория"));
        if (focusedItem->stats_group == "weapon")
        {
            addPrompt(LogicalPromptButton::R3,
                      (comparison_widget.GetActiveWeaponCompareSlot() == 2)
                          ? TrWid("ui_gp_eng_wid_prompt_cmp_slot2", "Сравн.: Слот 2")
                          : TrWid("ui_gp_eng_wid_prompt_cmp_slot1", "Сравн.: Слот 1"));
        }
        else
        {
            addPrompt(LogicalPromptButton::R3,
                      comparison_widget.IsFullStatsPreviewEnabled()
                          ? TrWid("ui_gp_eng_wid_prompt_stats_cur", "Статы: Текущие")
                          : TrWid("ui_gp_eng_wid_prompt_stats_100", "Статы: 100%"));
        }
        addPrompt(LogicalPromptButton::RightStick, TrWid("ui_gp_eng_wid_prompt_scroll_stats", "Прокрутка статов"));
        addPrompt(LogicalPromptButton::East,       TrWid("ui_gp_eng_wid_prompt_close", "Закрыть"));
    }
    else
    {
        addPrompt(LogicalPromptButton::LTRT,  TrWid("ui_gp_eng_wid_prompt_switch_cat", "Сменить категорию"));
        addPrompt(LogicalPromptButton::LBRB,  TrWid("ui_gp_eng_wid_prompt_section", "Раздел"));
        addPrompt(LogicalPromptButton::North, viewToggleLabel);
        addPrompt(LogicalPromptButton::East,  TrWid("ui_gp_eng_wid_prompt_close", "Закрыть"));
    }
}

std::string FooterGlyphBarWidget::FormatFooterLine() const
{
    std::ostringstream oss;
    for (size_t i = 0; i < prompts_.size(); ++i)
    {
        if (i > 0)
            oss << "   ";
        oss << prompts_[i].badge_text << " " << prompts_[i].action_label;
    }
    return oss.str();
}

// ============================================================================
// WidgetShowcaseController
// ============================================================================

std::vector<WidgetItemSnapshot> WidgetShowcaseController::BuildDefaultShowcaseCatalog(uint32_t session_gen)
{
    std::vector<WidgetItemSnapshot> items;

    // 1. АК-74М (надет в Слот 1 = equip_slot 2)
    {
        WidgetItemSnapshot w1;
        w1.stable_id                 = { session_gen, 501, 1 };
        w1.section                   = "wpn_ak74m";
        w1.display_name              = "АК-74М (прицел 1П78)";
        w1.ammo_or_caliber           = "5.45x39 БП: 30+1";
        w1.category                  = CategoryFilterId::Weapons;
        w1.stats_group               = "weapon";
        w1.cond_permille             = 820;
        w1.stack_count               = 1;
        w1.uses_or_ammo              = 31;
        w1.grid_w                    = 5;
        w1.grid_h                    = 2;
        w1.unit_weight_kg            = 3.60f;
        w1.unit_cost_ru              = 34500u;
        w1.equip_slot                = 2;
        w1.is_equipped               = true;
        w1.is_favorite               = true;
        w1.wpo_parts_summary         = "Ствол 86% | Затвор 91% | УСМ 78% | Газоотвод 84%";
        w1.adv.body_br_class          = 55;
        w1.adv.body_premitigation_pct = 38;
        w1.adv.body_fire_wound        = 0.52f;
        w1.adv.body_wound             = 0.48f;
        w1.adv.body_acid              = 0.30f;
        w1.adv.body_psi               = 0.35f;
        w1.adv.head_br_class          = 40;
        w1.adv.head_premitigation_pct = 27;
        w1.adv.head_fire_wound        = 0.39f;
        w1.adv.head_psi               = 0.42f;
        w1.adv_full_100               = w1.adv;
        w1.base_stats = {
            { "damage",      "Урон",             "",    38.0f, 38.0f, 0.0f, true, '=' },
            { "accuracy",    "Точность",         "%",   74.0f, 74.0f, 0.0f, true, '=' },
            { "handling",    "Удобство",         "%",   68.0f, 68.0f, 0.0f, true, '=' },
            { "rpm",         "Скорострельность", "в/м",600.0f,600.0f, 0.0f, true, '=' },
            { "reliability", "Надёжность",       "%",   82.0f, 82.0f, 0.0f, true, '=' },
        };
        items.push_back(std::move(w1));
    }

    // 2. Сайга-12К (надета в Слот 2 = equip_slot 3)
    {
        WidgetItemSnapshot w2;
        w2.stable_id         = { session_gen, 502, 1 };
        w2.section           = "wpn_saiga12s";
        w2.display_name      = "Сайга-12К Тактика";
        w2.ammo_or_caliber   = "12x70 Картечь: 8+1";
        w2.category          = CategoryFilterId::Weapons;
        w2.stats_group       = "weapon";
        w2.cond_permille     = 880;
        w2.stack_count       = 1;
        w2.uses_or_ammo      = 9;
        w2.grid_w            = 4;
        w2.grid_h            = 2;
        w2.unit_weight_kg    = 3.80f;
        w2.unit_cost_ru      = 39000u;
        w2.equip_slot        = 3;
        w2.is_equipped       = true;
        w2.wpo_parts_summary = "Ствол 90% | Затвор 89% | УСМ 85% | Газоотвод 88%";
        w2.adv               = items[0].adv;
        w2.adv_full_100      = w2.adv;
        w2.base_stats = {
            { "damage",      "Урон",             "",    64.0f, 38.0f, +26.0f, true, '+' },
            { "accuracy",    "Точность",         "%",   42.0f, 74.0f, -32.0f, true, '-' },
            { "handling",    "Удобство",         "%",   61.0f, 68.0f,  -7.0f, true, '-' },
            { "rpm",         "Скорострельность", "в/м",240.0f,600.0f,-360.0f, true, '-' },
            { "reliability", "Надёжность",       "%",   88.0f, 82.0f,  +6.0f, true, '+' },
        };
        items.push_back(std::move(w2));
    }

    // 3. АН-94 «Абакан» (в рюкзаке — сравнивается со Слотом 1 или Слотом 2 по R3)
    {
        WidgetItemSnapshot w3;
        w3.stable_id         = { session_gen, 503, 1 };
        w3.section           = "wpn_abakan";
        w3.display_name      = "АН-94 «Абакан»";
        w3.ammo_or_caliber   = "5.45x39 7Н10: 30/30";
        w3.category          = CategoryFilterId::Weapons;
        w3.stats_group       = "weapon";
        w3.cond_permille     = 910;
        w3.stack_count       = 1;
        w3.uses_or_ammo      = 30;
        w3.grid_w            = 5;
        w3.grid_h            = 2;
        w3.unit_weight_kg    = 3.90f;
        w3.unit_cost_ru      = 44000u;
        w3.equip_slot        = 2;
        w3.is_equipped       = false;
        w3.is_favorite       = true;
        w3.wpo_parts_summary = "Ствол 94% | Затвор 92% | УСМ 89% | Возвратный мех. 90%";
        w3.adv               = items[0].adv;
        w3.adv_full_100      = w3.adv;
        w3.base_stats = {
            { "damage",      "Урон",             "",    40.0f, 38.0f,  +2.0f, true, '+' },
            { "accuracy",    "Точность",         "%",   83.0f, 74.0f,  +9.0f, true, '+' },
            { "handling",    "Удобство",         "%",   64.0f, 68.0f,  -4.0f, true, '-' },
            { "rpm",         "Скорострельность", "в/м",650.0f,600.0f, +50.0f, true, '+' },
            { "reliability", "Надёжность",       "%",   91.0f, 82.0f,  +9.0f, true, '+' },
        };
        items.push_back(std::move(w3));
    }

    // 4. Бронекостюм «Берилл-5М» (надет в Слот 7 = equip_slot 7)
    {
        WidgetItemSnapshot o1;
        o1.stable_id                  = { session_gen, 504, 1 };
        o1.section                    = "military_outfit";
        o1.display_name               = "Бронекостюм «Берилл-5М» (надет)";
        o1.ammo_or_caliber            = "Плиты кл. IV | 3 слота пояса";
        o1.category                   = CategoryFilterId::ArmorArtefacts;
        o1.stats_group                = "outfit";
        o1.cond_permille              = 750;
        o1.stack_count                = 1;
        o1.grid_w                     = 2;
        o1.grid_h                     = 3;
        o1.unit_weight_kg             = 11.0f;
        o1.unit_cost_ru               = 52000u;
        o1.equip_slot                 = 7;
        o1.is_equipped                = true;
        o1.has_mag_vest_warning       = true;
        o1.adv.br_class               = 45;
        o1.adv.fire_wound_prot        = 0.42f;
        o1.adv.ap_res                 = 0.45f;
        o1.adv.wound_prot             = 0.44f;
        o1.adv.strike_prot            = 0.48f;
        o1.adv.expl_prot              = 0.40f;
        o1.adv.burn_prot              = 0.28f;
        o1.adv.shock_prot             = 0.24f;
        o1.adv.acid_prot              = 0.26f;
        o1.adv.psi_prot               = 0.20f;
        o1.adv.radia_prot             = 0.32f;
        o1.adv.body_br_class          = 55;
        o1.adv.body_premitigation_pct = 38;
        o1.adv.body_fire_wound        = 0.52f;
        o1.adv.body_wound             = 0.48f;
        o1.adv.body_acid              = 0.30f;
        o1.adv.body_psi               = 0.35f;
        o1.adv.head_br_class          = 40;
        o1.adv.head_premitigation_pct = 27;
        o1.adv.head_fire_wound        = 0.39f;
        o1.adv.head_psi               = 0.42f;

        o1.adv_full_100                 = o1.adv;
        o1.adv_full_100.br_class        = 60;
        o1.adv_full_100.fire_wound_prot = 0.56f;
        o1.adv_full_100.wound_prot      = 0.58f;
        o1.adv_full_100.strike_prot     = 0.64f;
        o1.adv_full_100.expl_prot       = 0.53f;
        items.push_back(std::move(o1));
    }

    // 5. Экзоскелет «Носорог» (в рюкзаке — кандидат на замену брони в слоте 7 со статами Accurate Defense Values)
    {
        WidgetItemSnapshot o2;
        o2.stable_id            = { session_gen, 505, 1 };
        o2.section              = "exo_nosorog_outfit";
        o2.display_name         = "Экзоскелет «Носорог» Мк.II";
        o2.ammo_or_caliber      = "Плиты кл. VI | 5 слотов пояса";
        o2.category             = CategoryFilterId::ArmorArtefacts;
        o2.stats_group          = "outfit";
        o2.cond_permille        = 920;
        o2.stack_count          = 1;
        o2.grid_w               = 2;
        o2.grid_h               = 3;
        o2.unit_weight_kg       = 21.5f;
        o2.unit_cost_ru         = 145000u;
        o2.equip_slot           = 7;
        o2.is_equipped          = false;
        o2.is_favorite          = true;
        o2.has_mag_vest_warning = true;
        o2.adv                  = items[3].adv;
        o2.adv.br_class         = 68;
        o2.adv.fire_wound_prot  = 0.64f;
        o2.adv.ap_res           = 0.62f;
        o2.adv.wound_prot       = 0.66f;
        o2.adv.strike_prot      = 0.72f;
        o2.adv.expl_prot        = 0.65f;
        o2.adv.burn_prot        = 0.42f;
        o2.adv.shock_prot       = 0.38f;
        o2.adv.acid_prot        = 0.44f;
        o2.adv.psi_prot         = 0.40f;
        o2.adv.radia_prot       = 0.55f;

        o2.adv_full_100                 = o2.adv;
        o2.adv_full_100.br_class        = 74;
        o2.adv_full_100.fire_wound_prot = 0.70f;
        o2.adv_full_100.wound_prot      = 0.72f;
        o2.adv_full_100.strike_prot     = 0.78f;
        o2.adv_full_100.expl_prot       = 0.71f;
        items.push_back(std::move(o2));
    }

    // 6. Патроны 5.45x39 БП (стак 120 шт. для проверки модального выбора количества)
    {
        WidgetItemSnapshot a1;
        a1.stable_id       = { session_gen, 506, 1 };
        a1.section         = "ammo_5.45x39_ap";
        a1.display_name    = "Патроны 5.45x39 мм БП (7Н22)";
        a1.ammo_or_caliber = "Бронебойные | БР IV";
        a1.category        = CategoryFilterId::Ammo;
        a1.stats_group     = "ammo";
        a1.cond_permille   = 1000;
        a1.stack_count     = 120;
        a1.uses_or_ammo    = 120;
        a1.grid_w          = 2;
        a1.grid_h          = 1;
        a1.unit_weight_kg  = 0.012f;
        a1.unit_cost_ru    = 85u;
        a1.adv             = items[0].adv;
        a1.adv_full_100    = a1.adv;
        items.push_back(std::move(a1));
    }

    // 7. Армейская аптечка (стак 6 шт., категория Медицина)
    {
        WidgetItemSnapshot m1;
        m1.stable_id       = { session_gen, 507, 1 };
        m1.section         = "medkit_army";
        m1.display_name    = "Армейская аптечка АИ-3ВС";
        m1.ammo_or_caliber = "Зарядов: 3/3";
        m1.category        = CategoryFilterId::Meds;
        m1.stats_group     = "booster";
        m1.cond_permille   = 1000;
        m1.stack_count     = 6;
        m1.uses_or_ammo    = 3;
        m1.grid_w          = 1;
        m1.grid_h          = 1;
        m1.unit_weight_kg  = 0.28f;
        m1.unit_cost_ru    = 3200u;
        m1.adv             = items[0].adv;
        m1.adv_full_100    = m1.adv;
        items.push_back(std::move(m1));
    }

    // 8. Набор для чистки оружия (категория Инструменты)
    {
        WidgetItemSnapshot t1;
        t1.stable_id       = { session_gen, 508, 1 };
        t1.section         = "cleaning_kit_r7";
        t1.display_name    = "Набор для чистки средних винтовок";
        t1.ammo_or_caliber = "Мин. сост.: 65% | Зарядов: 4/4";
        t1.category        = CategoryFilterId::ToolsParts;
        t1.stats_group     = "tools";
        t1.cond_permille   = 1000;
        t1.stack_count     = 3;
        t1.uses_or_ammo    = 4;
        t1.grid_w          = 2;
        t1.grid_h          = 1;
        t1.unit_weight_kg  = 0.45f;
        t1.unit_cost_ru    = 4600u;
        t1.adv             = items[0].adv;
        t1.adv_full_100    = t1.adv;
        items.push_back(std::move(t1));
    }

    return items;
}

void WidgetShowcaseController::EnsureRootModalLayerOnStack(uint32_t now_ms)
{
    if (modal_stack_.IsEmpty())
    {
        modal_stack_.PushModal("gp_widgets_showcase_root", "Витрина базовых виджетов F2.1", root_graph_, 0u, 0u, now_ms);
    }
}

void WidgetShowcaseController::InitializeDefaultShowcase(uint32_t now_ms)
{
    root_graph_.Clear();
    modal_stack_.EmergencyClearAll("reinit_showcase", 0u, now_ms, nullptr);

    tab_strip_.InitializeInGraph(root_graph_);
    item_catalog_.InitializeInGraph(root_graph_);
    comparison_card_.InitializeInGraph(root_graph_);
    footer_bar_.InitializeInGraph(root_graph_);

    tab_strip_.SetTopTab(TopTabId::Backpack);
    tab_strip_.SetCategoryFilter(CategoryFilterId::All);
    tab_strip_.SetActorTelemetry(24.8f, 45.0f, 48250u, true);

    item_catalog_.SetCatalogReadOnly(BuildDefaultShowcaseCatalog(1u), CategoryFilterId::All, root_graph_, now_ms);
    root_graph_.SwitchToGroup(item_catalog_.GetFocusGroupId(), now_ms);

    EnsureRootModalLayerOnStack(now_ms);
    RefreshAllWidgets(now_ms);
    last_status_ = "F2.1 Showcase initialized: 6 base widgets active in FocusGraph (read-only mode)";
}

const gpfocus::FocusGraph& WidgetShowcaseController::GetActiveGraph() const
{
    return modal_stack_.GetActiveFocusGraph(root_graph_);
}

void WidgetShowcaseController::RefreshAllWidgets(uint32_t now_ms)
{
    tab_strip_.SyncGraphNodes(root_graph_, now_ms);
    item_catalog_.SyncScrollToFocusedNode(root_graph_, now_ms);
    item_catalog_.TickSmoothScroll(now_ms);
    comparison_card_.RefreshFromFocusedItem(item_catalog_.GetFocusedItem(root_graph_), item_catalog_, now_ms);
    footer_bar_.RebuildPrompts(root_graph_, modal_stack_, item_catalog_, comparison_card_);
}

bool WidgetShowcaseController::MoveFocus(gpfocus::NavDir dir, bool /*is_repeat*/, uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u)
    {
        gpmodal::ModalLayer* top = modal_stack_.GetTopLayerMutable();
        if (top && top->tag == "modal_quantity_picker")
        {
            if (dir == gpfocus::NavDir::Left || dir == gpfocus::NavDir::Right)
            {
                const int32_t delta = (dir == gpfocus::NavDir::Right) ? 1 : -1;
                quantity_modal_.StepQuantity(delta);
                last_status_ = quantity_modal_.FormatModalSummaryLine();
                RefreshAllWidgets(now_ms);
                return true;
            }
        }
        if (top)
        {
            const bool moved = top->local_graph.MoveFocus(dir, now_ms);
            RefreshAllWidgets(now_ms);
            return moved;
        }
    }

    const uint32_t catGrp = item_catalog_.GetFocusGroupId();
    if (catGrp != gpfocus::kInvalidGroupId && root_graph_.GetActiveGroupId() != catGrp)
        root_graph_.SwitchToGroup(catGrp, now_ms);

    const bool moved = root_graph_.MoveFocus(dir, now_ms);
    RefreshAllWidgets(now_ms);
    return moved;
}

void WidgetShowcaseController::StepTopTab(int delta, uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u && quantity_modal_.IsOpen(modal_stack_))
    {
        quantity_modal_.StepByPack(delta);
        last_status_ = quantity_modal_.FormatModalSummaryLine();
        RefreshAllWidgets(now_ms);
        return;
    }
    if (modal_stack_.GetDepth() >= 2u)
        return;

    const TopTabId newTab = tab_strip_.StepTopTab(delta, true);
    last_status_ = std::string("LB/RB -> Верхняя вкладка: ") + TopTabToLabelRU(newTab);
    RefreshAllWidgets(now_ms);
}

gpfocus::FocusRestoreReason WidgetShowcaseController::StepCategoryFilter(int delta, uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u && quantity_modal_.IsOpen(modal_stack_))
    {
        if (delta < 0)
            quantity_modal_.SetToMin();
        else
            quantity_modal_.SetToMax();
        last_status_ = quantity_modal_.FormatModalSummaryLine();
        RefreshAllWidgets(now_ms);
        return gpfocus::FocusRestoreReason::SameItem;
    }
    if (modal_stack_.GetDepth() >= 2u)
        return gpfocus::FocusRestoreReason::SameItem;

    const CategoryFilterId newCat = tab_strip_.StepCategoryFilter(delta, true);
    const gpfocus::FocusRestoreReason reason = item_catalog_.ApplyCategoryFilter(newCat, root_graph_, now_ms);
    RefreshAllWidgets(now_ms);

    std::ostringstream oss;
    oss << "LT/RT -> Категория: " << CategoryFilterToLabelRU(newCat)
        << " (предм.: " << item_catalog_.GetFilteredItems().size()
        << ", фокус: " << gpfocus::FocusRestoreReasonToString(reason) << ")";
    last_status_ = oss.str();
    return reason;
}

gpfocus::FocusRestoreReason WidgetShowcaseController::ToggleListGridView(uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u)
        return gpfocus::FocusRestoreReason::SameItem;

    const gpfocus::FocusRestoreReason reason = item_catalog_.ToggleViewMode(root_graph_, now_ms);
    RefreshAllWidgets(now_ms);

    const WidgetItemSnapshot* focused = item_catalog_.GetFocusedItem(root_graph_);
    std::ostringstream oss;
    oss << "D10 Переключение вида -> " << ViewModeToLabelRU(item_catalog_.GetViewMode())
        << " (фокус сохранён: " << (focused ? focused->section : "пусто")
        << ", причина: " << gpfocus::FocusRestoreReasonToString(reason) << ")";
    last_status_ = oss.str();
    return reason;
}

uint8_t WidgetShowcaseController::ToggleCompareSlot(uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u)
        return comparison_card_.GetActiveWeaponCompareSlot();

    const uint8_t newSlot = comparison_card_.ToggleWeaponCompareSlot(
        item_catalog_.GetFocusedItem(root_graph_), item_catalog_, now_ms);
    RefreshAllWidgets(now_ms);
    last_status_ = std::string("R3 -> Слот сравнения оружия переключён на Слот ") + (newSlot == 2 ? "1 (осн.)" : "2 (доп.)");
    return newSlot;
}

bool WidgetShowcaseController::ToggleFullStats100(uint32_t now_ms)
{
    if (modal_stack_.GetDepth() >= 2u)
        return comparison_card_.IsFullStatsPreviewEnabled();

    const bool full = comparison_card_.ToggleFullStatsPreview(
        item_catalog_.GetFocusedItem(root_graph_), item_catalog_, now_ms);
    RefreshAllWidgets(now_ms);
    last_status_ = full
        ? "Accurate Defense Values: включён предпросмотр статов при 100% состоянии (read-only)"
        : "Accurate Defense Values: показ статов при текущем состоянии предмета (read-only)";
    return full;
}

void WidgetShowcaseController::CycleGlyphStyle(uint32_t now_ms)
{
    const uint8_t next = (static_cast<uint8_t>(footer_bar_.GetGlyphStyle()) + 1u) % 4u;
    const WidgetGlyphStyle style = static_cast<WidgetGlyphStyle>(next);
    WidgetFaceStyle face = WidgetFaceStyle::ABXY;
    if (style == WidgetGlyphStyle::PlayStation) face = WidgetFaceStyle::Sony;
    else if (style == WidgetGlyphStyle::Nintendo) face = WidgetFaceStyle::BAYX;
    else if (style == WidgetGlyphStyle::Generic) face = WidgetFaceStyle::Numbered;

    footer_bar_.SetGlyphStyle(style, face, true);
    RefreshAllWidgets(now_ms);
    last_status_ = std::string("D5.1 Панель глифов -> стиль переключён: ") + footer_bar_.FormatFooterLine();
}

bool WidgetShowcaseController::OpenQuantityModalForFocused(uint32_t pause_token_id, uint32_t held_mask, uint32_t now_ms)
{
    const WidgetItemSnapshot* focused = item_catalog_.GetFocusedItem(root_graph_);
    if (!focused)
        return false;

    EnsureRootModalLayerOnStack(now_ms);
    root_graph_.SwitchToGroup(item_catalog_.GetFocusGroupId(), now_ms);

    const bool ok = quantity_modal_.OpenOnStack(
        modal_stack_,
        root_graph_,
        *focused,
        250,
        500,
        pause_token_id,
        held_mask,
        now_ms);

    if (ok)
    {
        last_status_ = quantity_modal_.FormatModalSummaryLine();
        RefreshAllWidgets(now_ms);
    }
    return ok;
}

bool WidgetShowcaseController::OpenConfirmModalForFocused(uint32_t pause_token_id, uint32_t held_mask, uint32_t now_ms)
{
    const WidgetItemSnapshot* focused = item_catalog_.GetFocusedItem(root_graph_);
    if (!focused)
        return false;

    EnsureRootModalLayerOnStack(now_ms);
    root_graph_.SwitchToGroup(item_catalog_.GetFocusGroupId(), now_ms);

    const std::string title = TrWid("ui_gp_eng_wid_confirm_title", "Подтверждение действия");
    std::string warn;
    if (focused->has_mag_vest_warning)
    {
        warn = TrWid("ui_gp_eng_wid_confirm_mags_warn", "Внимание (mags_patches): при смене брони магазины в разгрузке будут перенесены в рюкзак. Продолжить?");
    }
    else
    {
        char warnBuf[512];
        std::snprintf(warnBuf, sizeof(warnBuf),
                      TrWid("ui_gp_eng_wid_confirm_item_fmt", "Подтвердить выбор предмета \"%s\" (read-only тест)?"),
                      focused->display_name.c_str());
        warn = warnBuf;
    }

    const bool ok = confirm_dialog_.OpenOnStack(
        modal_stack_,
        root_graph_,
        focused->stable_id,
        title,
        warn,
        "showcase_confirm_action",
        false,
        pause_token_id,
        held_mask,
        now_ms);

    if (ok)
    {
        last_status_ = confirm_dialog_.FormatDialogLine(modal_stack_);
        RefreshAllWidgets(now_ms);
    }
    return ok;
}

bool WidgetShowcaseController::StepModalPressA(uint32_t held_mask,
                                               uint32_t current_frame,
                                               uint32_t now_ms,
                                               std::vector<uint32_t>* out_released_tokens)
{
    if (out_released_tokens)
        out_released_tokens->clear();

    if (modal_stack_.GetDepth() < 2u)
        return false;

    if (confirm_dialog_.IsOpen(modal_stack_))
    {
        last_confirm_result_ = confirm_dialog_.ActivateFocusedChoice(modal_stack_);
        uint32_t poppedTok = 0;
        modal_stack_.PopTopModal(
            root_graph_,
            held_mask | gpmodal::ModalButtonMask(gpmodal::ModalButton::A),
            current_frame,
            now_ms,
            "confirm_dialog_a",
            &poppedTok);
        if (out_released_tokens && poppedTok != 0)
            out_released_tokens->push_back(poppedTok);

        last_status_ = last_confirm_result_.confirmed
            ? "Диалог подтверждения: выбрано [Да, подтвердить] (инвентарь не изменён, read-only)"
            : "Диалог подтверждения: выбрано [Отмена]";
        RefreshAllWidgets(now_ms);
        return true;
    }

    if (quantity_modal_.IsOpen(modal_stack_))
    {
        const gpmodal::ModalLayer* topLayer = modal_stack_.GetTopLayer();
        const gpfocus::FocusNode* focusedNode = topLayer ? topLayer->local_graph.GetFocusedNode() : nullptr;
        const bool isCancelFocused = (focusedNode && focusedNode->node_id == QuantityModalWidget::kCancelNodeId);
        last_qty_result_ = isCancelFocused ? quantity_modal_.CancelSelection() : quantity_modal_.ConfirmSelection();
        uint32_t poppedTok = 0;
        modal_stack_.PopTopModal(
            root_graph_,
            held_mask | gpmodal::ModalButtonMask(gpmodal::ModalButton::A),
            current_frame,
            now_ms,
            "quantity_modal_a",
            &poppedTok);
        if (out_released_tokens && poppedTok != 0)
            out_released_tokens->push_back(poppedTok);

        if (isCancelFocused)
        {
            last_status_ = "Модалка количества: отменено [Отмена] (read-only)";
        }
        else
        {
            std::ostringstream oss;
            oss << "Модалка количества: подтверждено " << last_qty_result_.selected_quantity
                << " шт. (" << last_qty_result_.section << ", read-only)";
            last_status_ = oss.str();
        }
        RefreshAllWidgets(now_ms);
        return true;
    }

    return false;
}

bool WidgetShowcaseController::StepModalCancelB(uint32_t held_mask,
                                                uint32_t current_frame,
                                                uint32_t now_ms,
                                                uint32_t* out_released_token)
{
    if (modal_stack_.GetDepth() < 2u)
        return false;

    if (confirm_dialog_.IsOpen(modal_stack_))
        last_confirm_result_ = confirm_dialog_.CancelDialog();
    else if (quantity_modal_.IsOpen(modal_stack_))
        last_qty_result_ = quantity_modal_.CancelSelection();

    const bool popped = modal_stack_.PopTopModal(
        root_graph_,
        held_mask | gpmodal::ModalButtonMask(gpmodal::ModalButton::B),
        current_frame,
        now_ms,
        "modal_cancel_b",
        out_released_token);

    if (popped)
    {
        last_status_ = "Модальное окно закрыто по [B] -> фокус возвращён на вызывающий узел";
        RefreshAllWidgets(now_ms);
    }
    return popped;
}

uint32_t WidgetShowcaseController::GetTotalInventoryMutations() const
{
    return item_catalog_.GetInventoryMutationCount()
         + comparison_card_.GetInventoryMutationCount()
         + quantity_modal_.GetInventoryMutationCount()
         + confirm_dialog_.GetInventoryMutationCount();
}

std::string WidgetShowcaseController::FormatShowcaseDump() const
{
    std::ostringstream oss;
    oss << "=== F2.1 Base Widgets Showcase Dump ===\n";
    oss << "  TopBar     : " << tab_strip_.FormatTopBarLine() << "\n";
    oss << "  CategoryBar: " << tab_strip_.FormatCategoryFilterLine(item_catalog_.GetViewMode(), item_catalog_.GetFilteredItems().size()) << "\n";
    oss << "  CatalogMode: " << ViewModeToLabelRU(item_catalog_.GetViewMode())
        << " | FilteredItems=" << item_catalog_.GetFilteredItems().size()
        << " | ScrollTop=" << item_catalog_.GetScroller().GetTargetTopIndex() << "\n";

    const int32_t focusedIdx = item_catalog_.GetFocusedItemIndex(root_graph_);
    if (item_catalog_.GetFilteredItems().empty())
    {
        oss << "  Item[0]    : " << item_catalog_.FormatListRow(0, true) << "\n";
    }
    else
    {
        for (size_t i = 0; i < item_catalog_.GetFilteredItems().size(); ++i)
        {
            const bool isFoc = (static_cast<int32_t>(i) == focusedIdx);
            oss << "  Item[" << i << "]    : "
                << ((item_catalog_.GetViewMode() == ItemViewMode::Grid2D)
                    ? item_catalog_.FormatGridCard(i, isFoc)
                    : item_catalog_.FormatListRow(i, isFoc))
                << "\n";
        }
    }

    oss << "  --- Comparison Card (Accurate Defense Values, read-only) ---\n";
    for (const std::string& line : comparison_card_.GetFormattedLines())
    {
        oss << "    " << line << "\n";
    }

    if (modal_stack_.GetDepth() >= 2u)
    {
        oss << "  --- Active Modal Overlay (depth=" << modal_stack_.GetDepth() << ") ---\n";
        if (quantity_modal_.IsOpen(modal_stack_))
            oss << "    " << quantity_modal_.FormatModalSummaryLine() << "\n";
        if (confirm_dialog_.IsOpen(modal_stack_))
            oss << "    " << confirm_dialog_.FormatDialogLine(modal_stack_) << "\n";
    }

    oss << "  FooterBar  : " << footer_bar_.FormatFooterLine() << "\n";
    oss << "  Mutations  : " << GetTotalInventoryMutations() << " (strictly read-only)\n";
    return oss.str();
}

// ============================================================================
// RunWidgetsSelfTest: полная самопроверка всех 6 базовых виджетов F2.1
// ============================================================================

gpfocus::SelfTestSummary RunWidgetsSelfTest()
{
    struct ScopedWidgetsSelfTest
    {
        bool prev;
        ScopedWidgetsSelfTest() : prev(g_inWidgetsSelfTest) { g_inWidgetsSelfTest = true; }
        ~ScopedWidgetsSelfTest() { g_inWidgetsSelfTest = prev; }
    } guard;

    gpfocus::SelfTestSummary summary;
    std::ostringstream log;

    auto check = [&](bool cond, const char* name)
    {
        ++summary.total_cases;
        if (cond)
        {
            ++summary.passed_cases;
        }
        else
        {
            ++summary.failed_cases;
            log << "[FAIL: " << name << "] ";
        }
    };

    WidgetShowcaseController ctrl;
    ctrl.InitializeDefaultShowcase(1000u);

    // 1. Все 4 корневых виджета зарегистрированы как группы и узлы в FocusGraph
    check(ctrl.GetRootGraph().GetGroups().size() == 4u, "root_graph_has_4_widget_groups");
    check(ctrl.GetTabStrip().GetFocusGroupId() != gpfocus::kInvalidGroupId, "tab_strip_group_registered");
    check(ctrl.GetItemCatalog().GetFocusGroupId() != gpfocus::kInvalidGroupId, "item_catalog_group_registered");
    check(ctrl.GetComparisonCard().GetFocusGroupId() != gpfocus::kInvalidGroupId, "comparison_card_group_registered");
    check(ctrl.GetFooterGlyphBar().GetFocusGroupId() != gpfocus::kInvalidGroupId, "footer_glyph_bar_group_registered");
    check(ctrl.GetItemCatalog().GetFilteredItems().size() == 8u, "default_catalog_has_8_items");
    check(ctrl.GetItemCatalog().GetViewMode() == ItemViewMode::List1D, "default_view_mode_is_list1d_d10");

    // 2. Двухрежимный вид Список <-> Сетка 4xN (D10) с сохранением фокуса по StableItemID
    ctrl.MoveFocus(gpfocus::NavDir::Down, false, 1050u);
    ctrl.MoveFocus(gpfocus::NavDir::Down, false, 1100u); // Фокус на 3-м предмете (АН-94 «Абакан», id=503)
    const WidgetItemSnapshot* focBeforeToggle = ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph());
    check(focBeforeToggle != nullptr && focBeforeToggle->stable_id.object_id == 503u, "focused_abakan_before_grid_toggle");

    const gpfocus::FocusRestoreReason toggleToGridReason = ctrl.ToggleListGridView(1150u);
    const WidgetItemSnapshot* focInGrid = ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph());
    check(ctrl.GetItemCatalog().GetViewMode() == ItemViewMode::Grid2D, "switched_to_grid2d_mode");
    check(toggleToGridReason == gpfocus::FocusRestoreReason::SameItem, "grid_toggle_preserved_same_item");
    check(focInGrid != nullptr && focInGrid->stable_id.object_id == 503u, "abakan_still_focused_in_grid2d");

    // 2D-навигация в сетке 4xN: шаг вниз с (col=2, row=0) ведёт во 2-й ряд (col=2, row=1 -> индекс 6, id=507)
    ctrl.MoveFocus(gpfocus::NavDir::Down, false, 1200u);
    const WidgetItemSnapshot* focRow2 = ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph());
    check(focRow2 != nullptr && focRow2->stable_id.object_id == 507u, "grid2d_down_moves_to_second_row");

    // Возврат в режим Списка сохраняет сфокусированный предмет (id=507)
    const gpfocus::FocusRestoreReason toggleToListReason = ctrl.ToggleListGridView(1250u);
    const WidgetItemSnapshot* focBackInList = ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph());
    check(ctrl.GetItemCatalog().GetViewMode() == ItemViewMode::List1D, "switched_back_to_list1d");
    check(toggleToListReason == gpfocus::FocusRestoreReason::SameItem, "list_toggle_preserved_same_item");
    check(focBackInList != nullptr && focBackInList->stable_id.object_id == 507u, "medkit_still_focused_in_list1d");

    // 3. Карточка сравнения со статами Accurate Defense Values (Оружие Слот 1/2 + Броня BR/Premitigation + 100% превью)
    // Переходим на экзоскелет «Носорог» (id=505, индекс 4 — 2 шага вверх с индекса 6)
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, 1300u);
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, 1310u);
    const WidgetItemSnapshot* focExo = ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph());
    check(focExo != nullptr && focExo->stable_id.object_id == 505u, "focused_exo_nosorog");
    check(ctrl.GetComparisonCard().HasComparedItem(), "exo_compared_against_equipped_berill_slot7");
    check(ctrl.GetComparisonCard().GetComparedItem().stable_id.object_id == 504u, "compared_slot7_is_berill");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().br_class == 68, "exo_adv_br_class_current_cond_68");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().body_br_class == 55, "actor_adv_body_br_class_55");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().body_premitigation_pct == 38, "actor_adv_body_premitigation_38");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().head_br_class == 40, "actor_adv_head_br_class_40");

    // Переключение 100% состояния (zz_ui_inventory_better_stats_bars.show_full_stats)
    check(ctrl.ToggleFullStats100(1350u) == true, "toggle_full_stats_100_on");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().br_class == 74, "exo_adv_br_class_at_100pct_is_74");
    check(ctrl.ToggleFullStats100(1360u) == false, "toggle_full_stats_100_off");
    check(ctrl.GetComparisonCard().GetDisplayedAdv().br_class == 68, "exo_adv_br_class_restored_68");

    // Переключение слота сравнения оружия R3 (Слот 1 <-> Слот 2) на АН-94 «Абакан» (id=503)
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, 1380u);
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, 1390u);
    check(ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph())->stable_id.object_id == 503u, "focused_abakan_for_r3_compare");
    check(ctrl.GetComparisonCard().GetComparedItem().stable_id.object_id == 501u, "abakan_initially_compared_to_slot1_ak74m");
    check(ctrl.ToggleCompareSlot(1400u) == 3u, "r3_switches_weapon_compare_to_slot2");
    check(ctrl.GetComparisonCard().GetComparedItem().stable_id.object_id == 502u, "abakan_now_compared_to_slot2_saiga12");
    check(ctrl.ToggleCompareSlot(1410u) == 2u, "r3_switches_weapon_compare_back_to_slot1");

    // 4. Полоса вкладок LB/RB + LT/RT и узел-заглушка EmptyStatePlaceholder на пустой категории
    ctrl.StepTopTab(+1, 1450u);
    check(ctrl.GetTabStrip().GetActiveTopTab() == TopTabId::Health, "lb_rb_stepped_to_health_tab");
    ctrl.StepTopTab(-1, 1460u);
    check(ctrl.GetTabStrip().GetActiveTopTab() == TopTabId::Backpack, "lb_rb_stepped_back_to_backpack");

    // Шагаем LT/RT до категории Провизия (Food = 4), которая пуста в витрине
    ctrl.StepCategoryFilter(+1, 1500u); // Weapons (3 items)
    check(ctrl.GetItemCatalog().GetFilteredItems().size() == 3u, "lt_rt_weapons_filter_has_3_items");
    ctrl.StepCategoryFilter(+1, 1510u); // Ammo (1 item)
    check(ctrl.GetItemCatalog().GetFilteredItems().size() == 1u, "lt_rt_ammo_filter_has_1_item");
    ctrl.StepCategoryFilter(+1, 1520u); // Meds (1 item)
    check(ctrl.GetItemCatalog().GetFilteredItems().size() == 1u, "lt_rt_meds_filter_has_1_item");
    const gpfocus::FocusRestoreReason emptyReason = ctrl.StepCategoryFilter(+1, 1530u); // Food (0 items -> EmptyStatePlaceholder)
    check(ctrl.GetItemCatalog().GetFilteredItems().empty(), "lt_rt_food_filter_is_empty");
    check(emptyReason == gpfocus::FocusRestoreReason::EmptyGroupHeader, "empty_category_restores_to_empty_placeholder");
    const gpfocus::FocusNode* emptyNode = ctrl.GetRootGraph().GetGroupFocusedNode(ctrl.GetItemCatalog().GetFocusGroupId());
    check(emptyNode != nullptr && emptyNode->kind == gpfocus::FocusNodeKind::EmptyStatePlaceholder, "focus_on_empty_state_placeholder_never_lost");
    // В пустой категории нижняя панель глифов НЕ содержит мёртвых подсказок [A] или [X]
    check(ctrl.GetFooterGlyphBar().GetPrompts().size() == 4u, "empty_category_footer_has_no_dead_item_prompts");

    // Возвращаемся в категорию «Все» (All = 0) и фокусируем патроны 5.45x39 БП (id=506, стак 120 шт.)
    uint32_t stepTime = 1600u;
    while (ctrl.GetTabStrip().GetActiveCategoryFilter() != CategoryFilterId::All)
    {
        stepTime += 250u;
        ctrl.StepCategoryFilter(+1, stepTime);
    }
    check(ctrl.GetItemCatalog().GetFilteredItems().size() == 8u, "returned_to_all_category_8_items");
    // В категории «Все» правило SameItem восстановило якорь на ремнаборе (индекс 7, id=508); 2 шага вверх ведут на патроны 5.45x39 БП (индекс 5, id=506)
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, stepTime + 10u);
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, stepTime + 20u);
    check(ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph())->stable_id.object_id == 506u, "focused_ammo_stack_120");

    // 5. Модальный выбор количества (QuantityModalWidget): ±1, автоповтор на паузе, ±10 LB/RB, Мин/Макс LT/RT, A/B
    check(ctrl.OpenQuantityModalForFocused(701u, gpmodal::ModalButtonMask(gpmodal::ModalButton::A), 1600u), "open_quantity_modal_on_ammo");
    check(ctrl.GetModalStack().GetDepth() == 2u, "quantity_modal_pushed_depth_2");
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 1, "quantity_modal_initial_qty_1");
    check(ctrl.GetQuantityModal().GetEffectiveMaxQuantity() == 120, "quantity_modal_max_qty_120");

    // D-Pad Right (+1) и LB/RB (+10)
    ctrl.MoveFocus(gpfocus::NavDir::Right, false, 1610u);
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 2, "quantity_step_plus_1");
    ctrl.StepTopTab(+1, 1620u); // RB -> +10
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 12, "quantity_step_rb_plus_10");
    ctrl.StepTopTab(-1, 1630u); // LB -> -10
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 2, "quantity_step_lb_minus_10");

    // Автоповтор удержания вправо на Device.dwTimeContinual (320ms delay + 90ms * 2 = +4 в сумме)
    ctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Right, true, 2000u);  // +1 -> 3
    ctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Right, true, 2320u);  // +1 -> 4
    ctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Right, true, 2500u);  // +2 -> 6
    ctrl.GetQuantityModalMutable().UpdateHoldRepeat(gpfocus::NavDir::Right, false, 2510u); // отпускание
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 6, "quantity_hold_repeat_on_pause");

    // RT -> Макс (120), LT -> Мин (1), затем +29 -> 30 шт. и подтверждение [A]
    ctrl.StepCategoryFilter(+1, 2450u); // RT -> Max
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 120, "quantity_rt_sets_max_120");
    ctrl.StepCategoryFilter(-1, 2460u); // LT -> Min
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 1, "quantity_lt_sets_min_1");
    ctrl.GetQuantityModalMutable().StepQuantity(29);
    check(ctrl.GetQuantityModal().GetCurrentQuantity() == 30, "quantity_set_to_30");

    std::vector<uint32_t> releasedToks;
    check(ctrl.StepModalPressA(0, 100u, 2500u, &releasedToks), "quantity_modal_confirm_a");
    check(!ctrl.HasActiveOverlayModal(), "quantity_modal_closed_after_a");
    check(releasedToks.size() == 1u && releasedToks[0] == 701u, "quantity_modal_released_token_701");
    check(ctrl.GetLastQuantityResult().confirmed && ctrl.GetLastQuantityResult().selected_quantity == 30, "quantity_result_recorded_30_readonly");
    check(ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph())->stable_id.object_id == 506u, "caller_focus_restored_to_ammo_506");

    // 6. Модальный диалог подтверждения (ConfirmDialogWidget): отмена [B] и подтверждение [A]
    ctrl.MoveFocus(gpfocus::NavDir::Up, false, 2550u); // Фокус на экзоскелет «Носорог» (id=505, с предупреждением mags_patches)
    check(ctrl.OpenConfirmModalForFocused(702u, gpmodal::ModalButtonMask(gpmodal::ModalButton::A), 2600u), "open_confirm_dialog_on_exo");
    check(ctrl.GetConfirmDialog().IsOpen(ctrl.GetModalStack()), "confirm_dialog_is_open");
    check(ctrl.GetConfirmDialog().IsYesFocused(ctrl.GetModalStack()), "confirm_dialog_yes_focused_initially");

    // Переключаем фокус на [Отмена] и нажимаем B (отмена)
    ctrl.MoveFocus(gpfocus::NavDir::Down, false, 2620u);
    check(!ctrl.GetConfirmDialog().IsYesFocused(ctrl.GetModalStack()), "confirm_dialog_moved_to_cancel_btn");
    uint32_t cancelTok = 0;
    check(ctrl.StepModalCancelB(0, 105u, 2650u, &cancelTok), "confirm_dialog_cancel_b");
    check(cancelTok == 702u && !ctrl.HasActiveOverlayModal(), "confirm_dialog_closed_and_token_702_released");
    check(ctrl.GetLastConfirmResult().cancelled && !ctrl.GetLastConfirmResult().confirmed, "confirm_dialog_cancelled_result");
    check(ctrl.GetItemCatalog().GetFocusedItem(ctrl.GetRootGraph())->stable_id.object_id == 505u, "caller_focus_restored_to_exo_505");

    // Повторное открытие и подтверждение [A] на кнопке [Да, подтвердить]
    check(ctrl.OpenConfirmModalForFocused(703u, 0, 2700u), "reopen_confirm_dialog_on_exo");
    check(ctrl.StepModalPressA(0, 110u, 2750u, &releasedToks), "confirm_dialog_press_a_yes");
    check(ctrl.GetLastConfirmResult().confirmed && !ctrl.GetLastConfirmResult().cancelled, "confirm_dialog_confirmed_result");

    // 7. Нижняя панель динамических глифов (D5.1): смена наборов Xbox -> PlayStation -> Nintendo -> Generic
    check(ctrl.GetFooterGlyphBar().GetGlyphStyle() == WidgetGlyphStyle::Xbox, "footer_initial_style_xbox");
    check(ctrl.GetFooterGlyphBar().GetPrompts().front().badge_text == "[A]", "footer_xbox_south_is_A");
    ctrl.CycleGlyphStyle(2800u);
    check(ctrl.GetFooterGlyphBar().GetGlyphStyle() == WidgetGlyphStyle::PlayStation, "footer_cycled_to_ps");
    check(ctrl.GetFooterGlyphBar().GetPrompts().front().badge_text == "[Cross]", "footer_ps_south_is_Cross");
    ctrl.CycleGlyphStyle(2810u);
    check(ctrl.GetFooterGlyphBar().GetGlyphStyle() == WidgetGlyphStyle::Nintendo, "footer_cycled_to_nintendo");
    check(ctrl.GetFooterGlyphBar().GetPrompts().front().badge_text == "[B]", "footer_nx_south_is_B");
    ctrl.CycleGlyphStyle(2820u);
    check(ctrl.GetFooterGlyphBar().GetGlyphStyle() == WidgetGlyphStyle::Generic, "footer_cycled_to_generic");
    check(ctrl.GetFooterGlyphBar().GetPrompts().front().badge_text == "[B1]", "footer_generic_south_is_B1");
    ctrl.CycleGlyphStyle(2830u);
    check(ctrl.GetFooterGlyphBar().GetGlyphStyle() == WidgetGlyphStyle::Xbox, "footer_cycled_back_to_xbox");

    // 8. Строгий инвариант F2.1: ни один виджет не мутировал инвентарь (total_mutations == 0)
    check(ctrl.GetTotalInventoryMutations() == 0u, "zero_inventory_mutations_invariant");

    std::ostringstream rep;
    rep << "gp_widgets selftest: "
        << (summary.failed_cases == 0 ? "PASS" : "FAIL")
        << " (" << summary.passed_cases << "/" << summary.total_cases
        << ", failed=" << summary.failed_cases
        << ", mutations=" << ctrl.GetTotalInventoryMutations() << ")";
    if (summary.failed_cases > 0)
        rep << " " << log.str();
    summary.report = rep.str();
    return summary;
}

} // namespace gpwidgets
