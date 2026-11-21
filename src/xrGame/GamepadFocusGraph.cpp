#include "GamepadFocusGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace gpfocus
{
    uint16_t FocusItemKey::QuantizeCondition(float cond_0_to_1) noexcept
    {
        if (cond_0_to_1 <= 0.0f)
            return 0;
        if (cond_0_to_1 >= 1.0f)
            return 1000;
        return static_cast<uint16_t>( std::lround(cond_0_to_1 * 1000.0f) );
    }

    const char* NavDirToString(NavDir dir) noexcept
    {
        switch (dir)
        {
        case NavDir::Up:    return "Up";
        case NavDir::Down:  return "Down";
        case NavDir::Left:  return "Left";
        case NavDir::Right: return "Right";
        default:            return "Unknown";
        }
    }

    const char* FocusNodeKindToString(FocusNodeKind kind) noexcept
    {
        switch (kind)
        {
        case FocusNodeKind::Item:                  return "Item";
        case FocusNodeKind::GroupHeader:           return "GroupHeader";
        case FocusNodeKind::EmptyStatePlaceholder: return "EmptyStatePlaceholder";
        case FocusNodeKind::ControlWidget:         return "ControlWidget";
        default:                                   return "Unknown";
        }
    }

    const char* FocusRestoreReasonToString(FocusRestoreReason reason) noexcept
    {
        switch (reason)
        {
        case FocusRestoreReason::None:                   return "None";
        case FocusRestoreReason::SameItem:               return "SameItem";
        case FocusRestoreReason::StackPromotedChild:     return "StackPromotedChild";
        case FocusRestoreReason::SpawnAntifreezeSettled: return "SpawnAntifreezeSettled";
        case FocusRestoreReason::SimilarNearby:          return "SimilarNearby";
        case FocusRestoreReason::NeighborBelow:          return "NeighborBelow";
        case FocusRestoreReason::NeighborAbove:          return "NeighborAbove";
        case FocusRestoreReason::FirstInGroup:           return "FirstInGroup";
        case FocusRestoreReason::EmptyGroupHeader:       return "EmptyGroupHeader";
        default:                                         return "Unknown";
        }
    }

    const FocusNode* FocusGroup::FindNode(uint32_t node_id) const noexcept
    {
        if (node_id == kInvalidNodeId)
            return nullptr;
        for (const FocusNode& n : nodes)
        {
            if (n.node_id == node_id)
                return &n;
        }
        return nullptr;
    }

    FocusNode* FocusGroup::FindNodeMutable(uint32_t node_id) noexcept
    {
        if (node_id == kInvalidNodeId)
            return nullptr;
        for (FocusNode& n : nodes)
        {
            if (n.node_id == node_id)
                return &n;
        }
        return nullptr;
    }

    size_t FocusGroup::CountItemNodes() const noexcept
    {
        size_t count = 0;
        for (const FocusNode& n : nodes)
        {
            if (n.kind == FocusNodeKind::Item)
                ++count;
        }
        return count;
    }

    void FocusGraph::Clear()
    {
        m_groups.clear();
        m_active_group_id = kInvalidGroupId;
    }

    uint32_t FocusGraph::AddGroup(const std::string& name, FocusLayoutMode mode, uint8_t bag_id, bool wrap)
    {
        const uint32_t new_id = static_cast<uint32_t>(m_groups.size());
        FocusGroup grp;
        grp.group_id        = new_id;
        grp.name            = name;
        grp.layout_mode     = mode;
        grp.bag_id          = bag_id;
        grp.wrap_navigation = wrap;

        // Инициализируем группу узлом пустого состояния (чтобы фокус никогда не был висячим)
        FocusNode empty_node;
        empty_node.node_id  = 0;
        empty_node.group_id = new_id;
        empty_node.kind     = FocusNodeKind::EmptyStatePlaceholder;
        empty_node.label    = name + " [Empty]";
        empty_node.key.bag_id = bag_id;
        grp.nodes.push_back(empty_node);
        grp.current_node_id     = empty_node.node_id;
        grp.last_valid_node_id  = empty_node.node_id;
        grp.last_restore_reason = FocusRestoreReason::EmptyGroupHeader;

        m_groups.push_back(std::move(grp));
        if (m_active_group_id == kInvalidGroupId)
            m_active_group_id = new_id;

        return new_id;
    }

    FocusGroup* FocusGraph::GetGroupMutable(uint32_t group_id) noexcept
    {
        for (FocusGroup& g : m_groups)
        {
            if (g.group_id == group_id)
                return &g;
        }
        return nullptr;
    }

    const FocusGroup* FocusGraph::GetGroup(uint32_t group_id) const noexcept
    {
        for (const FocusGroup& g : m_groups)
        {
            if (g.group_id == group_id)
                return &g;
        }
        return nullptr;
    }

    const FocusGroup* FocusGraph::GetActiveGroup() const noexcept
    {
        return GetGroup(m_active_group_id);
    }

    const FocusNode* FocusGraph::GetFocusedNode() const noexcept
    {
        const FocusGroup* g = GetActiveGroup();
        if (!g)
            return nullptr;
        return g->FindNode(g->current_node_id);
    }

    const FocusNode* FocusGraph::GetGroupFocusedNode(uint32_t group_id) const noexcept
    {
        const FocusGroup* g = GetGroup(group_id);
        if (!g)
            return nullptr;
        return g->FindNode(g->current_node_id);
    }

    void FocusGraph::SyncAnchorFromNode(FocusGroup& group, const FocusNode& node)
    {
        group.current_node_id    = node.node_id;
        group.last_valid_node_id = node.node_id;
        if (node.kind == FocusNodeKind::Item)
        {
            group.anchor_stable_id = node.stable_id;
            group.anchor_key       = node.key;
            group.has_anchor       = true;
        }
    }

    bool FocusGraph::SetFocusedNode(uint32_t group_id, uint32_t node_id, bool update_anchor)
    {
        FocusGroup* g = GetGroupMutable(group_id);
        if (!g)
            return false;
        const FocusNode* n = g->FindNode(node_id);
        if (!n)
            return false;

        g->current_node_id    = n->node_id;
        g->last_valid_node_id = n->node_id;
        if (update_anchor && n->kind == FocusNodeKind::Item)
        {
            g->anchor_stable_id = n->stable_id;
            g->anchor_key       = n->key;
            g->has_anchor       = true;
            g->preferred_col    = n->key.grid_x;
            g->pending_anchor.Clear();
        }
        return true;
    }

    namespace
    {
        // Горизонтальное перекрытие [x1, x1 + w1) и [x2, x2 + w2)
        int ComputeIntervalOverlap(int a_start, int a_len, int b_start, int b_len) noexcept
        {
            const int a_end = a_start + std::max(1, a_len);
            const int b_end = b_start + std::max(1, b_len);
            const int left  = std::max(a_start, b_start);
            const int right = std::min(a_end, b_end);
            return (right > left) ? (right - left) : 0;
        }

        // Расстояние между интервалами [a_start, a_end) и [b_start, b_end) (0, если пересекаются)
        int ComputeIntervalGap(int a_start, int a_len, int b_start, int b_len) noexcept
        {
            const int a_end = a_start + std::max(1, a_len);
            const int b_end = b_start + std::max(1, b_len);
            if (a_end <= b_start)
                return b_start - a_end;
            if (b_end <= a_start)
                return a_start - b_end;
            return 0;
        }

        // Расстояние от точки p до полуинтервала [start, start + len) (0, если внутри)
        int PointToIntervalDist(int p, int start, int len) noexcept
        {
            const int end = start + std::max(1, len);
            if (p < start)
                return start - p;
            if (p >= end)
                return p - (end - 1);
            return 0;
        }
    } // anonymous namespace

    bool FocusGraph::MoveFocus(NavDir dir, uint32_t /*now_ms*/)
    {
        FocusGroup* g = GetGroupMutable(m_active_group_id);
        if (!g)
            return false;

        const FocusNode* cur = g->FindNode(g->current_node_id);
        if (!cur)
            return false;

        uint32_t target_id = cur->GetNeighbor(dir);

        // В 2D-сетке при движении Up/Down через многоклеточные предметы (например, винтовку 5x2)
        // учитываем запомненную колонку preferred_col, если несколько соседей лежат на целевой строке.
        if (g->layout_mode == FocusLayoutMode::Grid2D &&
            (dir == NavDir::Up || dir == NavDir::Down) &&
            cur->kind == FocusNodeKind::Item)
        {
            const int cur_top    = cur->key.grid_y;
            const int cur_bottom = cur->key.grid_y + std::max<int16_t>(1, cur->key.grid_h);

            int      best_primary_dy  = std::numeric_limits<int>::max();
            int      best_pref_dist   = std::numeric_limits<int>::max();
            int      best_overlap     = -1;
            int      best_x           = std::numeric_limits<int>::max();
            uint32_t best_pref_target = kInvalidNodeId;

            for (const FocusNode& cand : g->nodes)
            {
                if (cand.node_id == cur->node_id || cand.kind != FocusNodeKind::Item)
                    continue;

                const int cand_top    = cand.key.grid_y;
                const int cand_bottom = cand.key.grid_y + std::max<int16_t>(1, cand.key.grid_h);

                int dy = 0;
                if (dir == NavDir::Down)
                {
                    if (cand_top < cur_bottom && !(cand_top > cur_top))
                        continue;
                    dy = (cand_top >= cur_bottom)
                        ? (cand_top - cur_bottom)
                        : (cand_top - cur_top);
                }
                else // NavDir::Up
                {
                    if (cand_bottom > cur_top && !(cand_top < cur_top))
                        continue;
                    dy = (cand_bottom <= cur_top)
                        ? (cur_top - cand_bottom)
                        : (cur_top - cand_top);
                }

                const int overlap = ComputeIntervalOverlap(
                    cur->key.grid_x, cur->key.grid_w,
                    cand.key.grid_x, cand.key.grid_w);
                if (overlap <= 0)
                    continue;

                const int pref_dist = PointToIntervalDist(
                    g->preferred_col, cand.key.grid_x, cand.key.grid_w);

                if (dy < best_primary_dy ||
                    (dy == best_primary_dy && pref_dist < best_pref_dist) ||
                    (dy == best_primary_dy && pref_dist == best_pref_dist && overlap > best_overlap) ||
                    (dy == best_primary_dy && pref_dist == best_pref_dist && overlap == best_overlap && cand.key.grid_x < best_x))
                {
                    best_primary_dy  = dy;
                    best_pref_dist   = pref_dist;
                    best_overlap     = overlap;
                    best_x           = cand.key.grid_x;
                    best_pref_target = cand.node_id;
                }
            }

            if (best_pref_target != kInvalidNodeId)
                target_id = best_pref_target;
        }

        if (target_id == kInvalidNodeId || target_id == cur->node_id)
            return false;

        const FocusNode* next_node = g->FindNode(target_id);
        if (!next_node)
            return false;

        SyncAnchorFromNode(*g, *next_node);

        // При горизонтальном шаге обновляем preferred_col; при вертикальном — сохраняем,
        // если текущий предмет покрывает preferred_col, иначе прижимаем к границам предмета.
        if (next_node->kind == FocusNodeKind::Item)
        {
            if (dir == NavDir::Left || dir == NavDir::Right)
            {
                g->preferred_col = next_node->key.grid_x;
            }
            else
            {
                const int x_start = next_node->key.grid_x;
                const int x_end   = x_start + std::max<int16_t>(1, next_node->key.grid_w) - 1;
                if (g->preferred_col < x_start)
                    g->preferred_col = static_cast<int16_t>(x_start);
                else if (g->preferred_col > x_end)
                    g->preferred_col = static_cast<int16_t>(x_end);
            }
        }

        // Ручное перемещение курсора отменяет ожидание антифриз-доспавна старого предмета
        g->pending_anchor.Clear();
        return true;
    }

    bool FocusGraph::SwitchToGroup(uint32_t target_group_id, uint32_t now_ms)
    {
        FocusGroup* target = GetGroupMutable(target_group_id);
        if (!target)
            return false;

        if (FocusGroup* prev = GetGroupMutable(m_active_group_id))
        {
            if (prev->FindNode(prev->current_node_id) != nullptr)
                prev->last_valid_node_id = prev->current_node_id;
        }

        m_active_group_id = target_group_id;

        // Восстанавливаем запомненный узел в целевой группе (D24)
        if (const FocusNode* remembered = target->FindNode(target->last_valid_node_id))
        {
            target->current_node_id = remembered->node_id;
            if (remembered->kind == FocusNodeKind::Item)
            {
                target->anchor_stable_id = remembered->stable_id;
                target->anchor_key       = remembered->key;
                target->has_anchor       = true;
            }
            return true;
        }

        // Если запомненный узел устарел, запускаем детерминированное восстановление
        RestoreFocusInternal(*target, false, now_ms, kDefaultSettleWindowMs);
        return true;
    }

    uint32_t FocusGraph::SwitchGroupNext(uint32_t now_ms)
    {
        if (m_groups.empty())
            return kInvalidGroupId;

        size_t cur_idx = 0;
        for (size_t i = 0; i < m_groups.size(); ++i)
        {
            if (m_groups[i].group_id == m_active_group_id)
            {
                cur_idx = i;
                break;
            }
        }

        const size_t next_idx = (cur_idx + 1) % m_groups.size();
        const uint32_t next_id = m_groups[next_idx].group_id;
        SwitchToGroup(next_id, now_ms);
        return m_active_group_id;
    }

    bool FocusGraph::BuildListNeighbors(uint32_t group_id, bool wrap)
    {
        FocusGroup* g = GetGroupMutable(group_id);
        if (!g)
            return false;

        std::vector<FocusNode*> items;
        items.reserve(g->nodes.size());
        for (FocusNode& n : g->nodes)
        {
            n.SetNeighbor(NavDir::Up,    kInvalidNodeId);
            n.SetNeighbor(NavDir::Down,  kInvalidNodeId);
            n.SetNeighbor(NavDir::Left,  kInvalidNodeId);
            n.SetNeighbor(NavDir::Right, kInvalidNodeId);
            if (n.kind == FocusNodeKind::Item)
                items.push_back(&n);
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            items[i]->key.list_index = static_cast<int32_t>(i);
            items[i]->key.grid_x     = 0;
            items[i]->key.grid_y     = static_cast<int16_t>(i);
            items[i]->key.grid_w     = 1;
            items[i]->key.grid_h     = 1;

            if (i > 0)
                items[i]->SetNeighbor(NavDir::Up, items[i - 1]->node_id);
            else if (wrap && items.size() > 1)
                items[i]->SetNeighbor(NavDir::Up, items.back()->node_id);

            if (i + 1 < items.size())
                items[i]->SetNeighbor(NavDir::Down, items[i + 1]->node_id);
            else if (wrap && items.size() > 1)
                items[i]->SetNeighbor(NavDir::Down, items.front()->node_id);
        }

        return true;
    }

    bool FocusGraph::BuildGridNeighbors(uint32_t group_id, bool wrap)
    {
        FocusGroup* g = GetGroupMutable(group_id);
        if (!g)
            return false;

        std::vector<FocusNode*> items;
        items.reserve(g->nodes.size());
        for (FocusNode& n : g->nodes)
        {
            n.SetNeighbor(NavDir::Up,    kInvalidNodeId);
            n.SetNeighbor(NavDir::Down,  kInvalidNodeId);
            n.SetNeighbor(NavDir::Left,  kInvalidNodeId);
            n.SetNeighbor(NavDir::Right, kInvalidNodeId);
            if (n.kind == FocusNodeKind::Item)
                items.push_back(&n);
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i]->key.list_index < 0)
                items[i]->key.list_index = static_cast<int32_t>(i);
            if (items[i]->key.grid_w < 1)
                items[i]->key.grid_w = 1;
            if (items[i]->key.grid_h < 1)
                items[i]->key.grid_h = 1;
        }

        auto findBestNeighbor = [&](const FocusNode& src, NavDir dir) -> uint32_t
        {
            const int sx = src.key.grid_x;
            const int sy = src.key.grid_y;
            const int sw = src.key.grid_w;
            const int sh = src.key.grid_h;

            uint32_t best_in_band  = kInvalidNodeId;
            int      best_band_pri = std::numeric_limits<int>::max();
            int      best_band_ovl = -1;
            int      best_band_sec = std::numeric_limits<int>::max();

            uint32_t best_fallback = kInvalidNodeId;
            int      best_fb_pri   = std::numeric_limits<int>::max();
            int      best_fb_sec   = std::numeric_limits<int>::max();
            int      best_fb_tie   = std::numeric_limits<int>::max();

            for (const FocusNode* cand_ptr : items)
            {
                const FocusNode& cand = *cand_ptr;
                if (cand.node_id == src.node_id)
                    continue;

                const int cx = cand.key.grid_x;
                const int cy = cand.key.grid_y;
                const int cw = cand.key.grid_w;
                const int ch = cand.key.grid_h;

                int pri_dist = 0;
                int overlap  = 0;
                int sec_gap  = 0;
                int tie_coord = 0;

                switch (dir)
                {
                case NavDir::Down:
                    if (cy < sy + sh && !(cy > sy))
                        continue;
                    pri_dist  = (cy >= sy + sh) ? (cy - (sy + sh)) : (cy - sy);
                    overlap   = ComputeIntervalOverlap(sx, sw, cx, cw);
                    sec_gap   = ComputeIntervalGap(sx, sw, cx, cw);
                    tie_coord = cx;
                    break;

                case NavDir::Up:
                    if (cy + ch > sy && !(cy < sy))
                        continue;
                    pri_dist  = (cy + ch <= sy) ? (sy - (cy + ch)) : (sy - cy);
                    overlap   = ComputeIntervalOverlap(sx, sw, cx, cw);
                    sec_gap   = ComputeIntervalGap(sx, sw, cx, cw);
                    tie_coord = cx;
                    break;

                case NavDir::Right:
                    if (cx < sx + sw && !(cx > sx))
                        continue;
                    pri_dist  = (cx >= sx + sw) ? (cx - (sx + sw)) : (cx - sx);
                    overlap   = ComputeIntervalOverlap(sy, sh, cy, ch);
                    sec_gap   = ComputeIntervalGap(sy, sh, cy, ch);
                    tie_coord = cy;
                    break;

                case NavDir::Left:
                    if (cx + cw > sx && !(cx < sx))
                        continue;
                    pri_dist  = (cx + cw <= sx) ? (sx - (cx + cw)) : (sx - cx);
                    overlap   = ComputeIntervalOverlap(sy, sh, cy, ch);
                    sec_gap   = ComputeIntervalGap(sy, sh, cy, ch);
                    tie_coord = cy;
                    break;

                default:
                    continue;
                }

                if (overlap > 0)
                {
                    if (pri_dist < best_band_pri ||
                        (pri_dist == best_band_pri && overlap > best_band_ovl) ||
                        (pri_dist == best_band_pri && overlap == best_band_ovl && tie_coord < best_band_sec))
                    {
                        best_band_pri = pri_dist;
                        best_band_ovl = overlap;
                        best_band_sec = tie_coord;
                        best_in_band  = cand.node_id;
                    }
                }
                else
                {
                    if (pri_dist < best_fb_pri ||
                        (pri_dist == best_fb_pri && sec_gap < best_fb_sec) ||
                        (pri_dist == best_fb_pri && sec_gap == best_fb_sec && tie_coord < best_fb_tie))
                    {
                        best_fb_pri   = pri_dist;
                        best_fb_sec   = sec_gap;
                        best_fb_tie   = tie_coord;
                        best_fallback = cand.node_id;
                    }
                }
            }

            if (best_in_band != kInvalidNodeId)
                return best_in_band;
            return best_fallback;
        };

        for (FocusNode* src : items)
        {
            src->SetNeighbor(NavDir::Up,    findBestNeighbor(*src, NavDir::Up));
            src->SetNeighbor(NavDir::Down,  findBestNeighbor(*src, NavDir::Down));
            src->SetNeighbor(NavDir::Left,  findBestNeighbor(*src, NavDir::Left));
            src->SetNeighbor(NavDir::Right, findBestNeighbor(*src, NavDir::Right));

            if (wrap && items.size() > 1)
            {
                if (src->GetNeighbor(NavDir::Left) == kInvalidNodeId && src->key.list_index > 0)
                {
                    for (FocusNode* c : items)
                    {
                        if (c->key.list_index == src->key.list_index - 1)
                        {
                            src->SetNeighbor(NavDir::Left, c->node_id);
                            break;
                        }
                    }
                }
                if (src->GetNeighbor(NavDir::Right) == kInvalidNodeId)
                {
                    for (FocusNode* c : items)
                    {
                        if (c->key.list_index == src->key.list_index + 1)
                        {
                            src->SetNeighbor(NavDir::Right, c->node_id);
                            break;
                        }
                    }
                }
            }
        }

        return true;
    }

    FocusRestoreReason FocusGraph::RebuildGroupItems(
        uint32_t                      group_id,
        const std::vector<FocusNode>& item_nodes,
        uint8_t                       new_tab_id,
        uint32_t                      now_ms,
        uint32_t                      settle_window_ms)
    {
        FocusGroup* g = GetGroupMutable(group_id);
        if (!g)
            return FocusRestoreReason::None;

        // Перед очисткой узлов синхронизируем якорь с текущим узлом, если он валиден
        if (const FocusNode* cur = g->FindNode(g->current_node_id))
        {
            if (cur->kind == FocusNodeKind::Item)
            {
                g->anchor_stable_id = cur->stable_id;
                g->anchor_key       = cur->key;
                g->has_anchor       = true;
            }
        }

        const bool tab_changed = (g->active_tab_id != new_tab_id);
        g->active_tab_id       = new_tab_id;
        ++g->rebuild_count;

        g->nodes.clear();

        if (item_nodes.empty())
        {
            FocusNode empty_node;
            empty_node.node_id    = 0;
            empty_node.group_id   = group_id;
            empty_node.kind       = FocusNodeKind::EmptyStatePlaceholder;
            empty_node.label      = g->name + " [Empty]";
            empty_node.key.bag_id = g->bag_id;
            empty_node.key.tab_id = new_tab_id;
            g->nodes.push_back(empty_node);
        }
        else
        {
            g->nodes.reserve(item_nodes.size());
            for (size_t i = 0; i < item_nodes.size(); ++i)
            {
                FocusNode n    = item_nodes[i];
                n.node_id      = (n.node_id != kInvalidNodeId) ? n.node_id : static_cast<uint32_t>(i + 1);
                n.group_id     = group_id;
                n.kind         = FocusNodeKind::Item;
                n.key.bag_id   = g->bag_id;
                n.key.tab_id   = new_tab_id;
                n.key.list_index = static_cast<int32_t>(i);
                if (g->layout_mode == FocusLayoutMode::List1D)
                {
                    n.key.grid_x = 0;
                    n.key.grid_y = static_cast<int16_t>(i);
                    n.key.grid_w = 1;
                    n.key.grid_h = 1;
                }
                g->nodes.push_back(std::move(n));
            }

            if (g->layout_mode == FocusLayoutMode::Grid2D)
                BuildGridNeighbors(group_id, g->wrap_navigation);
            else if (g->layout_mode == FocusLayoutMode::List1D)
                BuildListNeighbors(group_id, g->wrap_navigation);
        }

        return RestoreFocusInternal(*g, tab_changed, now_ms, settle_window_ms);
    }

    FocusRestoreReason FocusGraph::RestoreFocusInternal(
        FocusGroup& group,
        bool        tab_changed,
        uint32_t    now_ms,
        uint32_t    settle_window_ms)
    {
        // Шаг 6 (крайний случай пустой группы): если предметов 0, фокус становится на EmptyStatePlaceholder / GroupHeader
        if (group.CountItemNodes() == 0)
        {
            // Если до опустошения был валидный предмет, сохраняем его в pending_anchor на окно SPAWN_ANTIFREEZE
            if (group.has_anchor && !group.pending_anchor.active && settle_window_ms > 0)
            {
                group.pending_anchor.active          = true;
                group.pending_anchor.prev_stable_id  = group.anchor_stable_id;
                group.pending_anchor.prev_key        = group.anchor_key;
                group.pending_anchor.settle_until_ms = now_ms + settle_window_ms;
            }

            const uint32_t header_id = group.nodes.empty() ? kInvalidNodeId : group.nodes.front().node_id;
            group.current_node_id     = header_id;
            group.last_valid_node_id  = header_id;
            group.last_restore_reason = FocusRestoreReason::EmptyGroupHeader;
            return group.last_restore_reason;
        }

        // Проверяем истечение окна SPAWN_ANTIFREEZE (с защитой от переполнения uint32_t через знаковую разность)
        const auto isSettleActiveAt = [](const PendingFocusAnchor& pa, uint32_t t_ms) noexcept -> bool
        {
            if (!pa.active)
                return false;
            if (t_ms == 0)
                return true;
            return static_cast<int32_t>(t_ms - pa.settle_until_ms) <= 0;
        };

        if (group.pending_anchor.active && !isSettleActiveAt(group.pending_anchor, now_ms))
        {
            group.pending_anchor.Clear();
        }

        // Если в группе ещё никогда не было выбранного предмета — выбираем первый предмет в группе
        if (!group.has_anchor)
        {
            for (const FocusNode& n : group.nodes)
            {
                if (n.kind == FocusNodeKind::Item)
                {
                    SyncAnchorFromNode(group, n);
                    group.preferred_col       = n.key.grid_x;
                    group.last_restore_reason = FocusRestoreReason::FirstInGroup;
                    return group.last_restore_reason;
                }
            }
        }

        // ---------------------------------------------------------------------
        // Приоритет 0 (SPAWN_ANTIFREEZE доспавн в окне settle_until_ms):
        // Если на предыдущем кадре предмет исчез и запустил pending_anchor,
        // а сейчас (now_ms <= settle_until_ms) спавнился предмет той же секции
        // на той же ячейке/координате (или тот же StableItemID) — возвращаем фокус ему!
        // ---------------------------------------------------------------------
        if (isSettleActiveAt(group.pending_anchor, now_ms) && !tab_changed)
        {
            const FocusItemKey& pk = group.pending_anchor.prev_key;
            for (const FocusNode& n : group.nodes)
            {
                if (n.kind != FocusNodeKind::Item)
                    continue;
                const bool exact_id = group.pending_anchor.prev_stable_id.IsValid() && (n.stable_id == group.pending_anchor.prev_stable_id);
                const bool same_slot_section = !pk.section.empty() && (n.key.section == pk.section) &&
                    ((pk.cell_indx >= 0 && n.key.cell_indx == pk.cell_indx) ||
                     (n.key.grid_x == pk.grid_x && n.key.grid_y == pk.grid_y));

                if (exact_id || same_slot_section)
                {
                    SyncAnchorFromNode(group, n);
                    group.pending_anchor.Clear();
                    group.last_restore_reason = FocusRestoreReason::SpawnAntifreezeSettled;
                    return group.last_restore_reason;
                }
            }
        }

        // ---------------------------------------------------------------------
        // Приоритет 1a (Тот же предмет — SameItem):
        // Точное совпадение StableItemID {session_gen, object_id, instance_gen}
        // ---------------------------------------------------------------------
        if (group.anchor_stable_id.IsValid())
        {
            for (const FocusNode& n : group.nodes)
            {
                if (n.kind == FocusNodeKind::Item && n.stable_id == group.anchor_stable_id)
                {
                    SyncAnchorFromNode(group, n);
                    group.pending_anchor.Clear();
                    group.last_restore_reason = FocusRestoreReason::SameItem;
                    return group.last_restore_reason;
                }
            }
        }

        // Если была смена вкладки (LT/RT) и текущего предмета на новой вкладке нет —
        // по правилу UI инвентаря переходим сразу на первый предмет новой вкладки (FirstInGroup),
        // чтобы не прыгать на случайную координату от старого фильтра.
        if (tab_changed)
        {
            for (const FocusNode& n : group.nodes)
            {
                if (n.kind == FocusNodeKind::Item)
                {
                    SyncAnchorFromNode(group, n);
                    group.preferred_col       = n.key.grid_x;
                    group.pending_anchor.Clear();
                    group.last_restore_reason = FocusRestoreReason::FirstInGroup;
                    return group.last_restore_reason;
                }
            }
        }

        const FocusItemKey prev_key       = group.anchor_key;
        const StableItemID prev_stable_id = group.anchor_stable_id;

        // ---------------------------------------------------------------------
        // Приоритет 1b (Остаток стака в той же ячейке после UICellItem:ResetToChild — StackPromotedChild):
        // В UIInventory / utils_ui.script (UICellItem:ResetToChild, utils_ui.script:1268-1296)
        // при расходовании ведущего предмета пачки вызывается ResetToChild(), меняющий ID ячейки
        // на дочерний предмет той же секции в том же cell_indx / (grid_x, grid_y).
        // ---------------------------------------------------------------------
        if (!prev_key.section.empty())
        {
            for (const FocusNode& n : group.nodes)
            {
                if (n.kind != FocusNodeKind::Item || n.key.section != prev_key.section)
                    continue;

                const bool same_cell_indx = (prev_key.cell_indx >= 0 && n.key.cell_indx == prev_key.cell_indx);
                const bool same_grid_pos  = (group.layout_mode == FocusLayoutMode::Grid2D &&
                                             n.key.grid_x == prev_key.grid_x &&
                                             n.key.grid_y == prev_key.grid_y);
                if (same_cell_indx || same_grid_pos)
                {
                    SyncAnchorFromNode(group, n);
                    group.pending_anchor.Clear();
                    group.last_restore_reason = FocusRestoreReason::StackPromotedChild;
                    return group.last_restore_reason;
                }
            }
        }

        // ---------------------------------------------------------------------
        // Приоритет 2 (Такой же предмет рядом — SimilarNearby):
        // Ищем предмет с той же section: минимальное манхэттенское расстояние
        // |dx| + |dy| (в 2D-сетке) или |d(list_index)| (в 1D-списке),
        // при равенстве — минимальная разница кондиции |d(cond_permille)|,
        // затем меньший list_index.
        // ---------------------------------------------------------------------
        if (!prev_key.section.empty())
        {
            const FocusNode* best_similar      = nullptr;
            int              best_manhattan    = std::numeric_limits<int>::max();
            int              best_cond_delta   = std::numeric_limits<int>::max();
            int              best_list_idx     = std::numeric_limits<int>::max();

            for (const FocusNode& n : group.nodes)
            {
                if (n.kind != FocusNodeKind::Item || n.key.section != prev_key.section)
                    continue;

                const int manhattan = (group.layout_mode == FocusLayoutMode::Grid2D)
                    ? (std::abs(static_cast<int>(n.key.grid_x) - static_cast<int>(prev_key.grid_x)) +
                       std::abs(static_cast<int>(n.key.grid_y) - static_cast<int>(prev_key.grid_y)))
                    : std::abs(static_cast<int>(n.key.list_index) - static_cast<int>(prev_key.list_index));

                const int cond_delta = std::abs(
                    static_cast<int>(n.key.cond_permille) - static_cast<int>(prev_key.cond_permille));

                if (manhattan < best_manhattan ||
                    (manhattan == best_manhattan && cond_delta < best_cond_delta) ||
                    (manhattan == best_manhattan && cond_delta == best_cond_delta && n.key.list_index < best_list_idx))
                {
                    best_manhattan  = manhattan;
                    best_cond_delta = cond_delta;
                    best_list_idx   = n.key.list_index;
                    best_similar    = &n;
                }
            }

            if (best_similar != nullptr)
            {
                SyncAnchorFromNode(group, *best_similar);
                group.pending_anchor.Clear();
                group.last_restore_reason = FocusRestoreReason::SimilarNearby;
                return group.last_restore_reason;
            }
        }

        // Если ни тот же предмет, ни предмет той же секции не найдены (переходим на соседа другой секции),
        // запоминаем pending_anchor на случай доспавна замены через 1..3 кадра (SPAWN_ANTIFREEZE)
        if (!group.pending_anchor.active && settle_window_ms > 0)
        {
            group.pending_anchor.active          = true;
            group.pending_anchor.prev_stable_id  = prev_stable_id;
            group.pending_anchor.prev_key        = prev_key;
            group.pending_anchor.settle_until_ms = now_ms + settle_window_ms;
        }

        // ---------------------------------------------------------------------
        // Приоритет 3 (Сосед снизу — NeighborBelow) и
        // Приоритет 4 (Сосед сверху — NeighborAbove):
        // В 1D-списке при удалении элемента с индексом K на место K встаёт
        // бывший сосед снизу (K+1 -> теперь K). Если удалён последний элемент
        // (K >= N_new), берём соседа сверху (N_new - 1).
        // В 2D-сетке сначала ищем предмет на той же строке правее / ниже (grid_y >= prev_y),
        // а если удалён самый нижний/правый — предмет выше/левее (NeighborAbove).
        // ---------------------------------------------------------------------
        if (group.layout_mode == FocusLayoutMode::List1D)
        {
            if (prev_key.list_index >= 0)
            {
                const FocusNode* below_candidate = nullptr;
                const FocusNode* above_candidate = nullptr;

                for (const FocusNode& n : group.nodes)
                {
                    if (n.kind != FocusNodeKind::Item)
                        continue;
                    if (n.key.list_index >= prev_key.list_index && below_candidate == nullptr)
                        below_candidate = &n;
                    if (n.key.list_index < prev_key.list_index)
                        above_candidate = &n;
                }

                if (below_candidate != nullptr)
                {
                    SyncAnchorFromNode(group, *below_candidate);
                    group.last_restore_reason = FocusRestoreReason::NeighborBelow;
                    return group.last_restore_reason;
                }
                if (above_candidate != nullptr)
                {
                    SyncAnchorFromNode(group, *above_candidate);
                    group.last_restore_reason = FocusRestoreReason::NeighborAbove;
                    return group.last_restore_reason;
                }
            }
        }
        else // Grid2D / Custom
        {
            const int px = prev_key.grid_x;
            const int py = prev_key.grid_y;

            const FocusNode* best_below    = nullptr;
            int              best_below_dy = std::numeric_limits<int>::max();
            int              best_below_dx = std::numeric_limits<int>::max();

            const FocusNode* best_above    = nullptr;
            int              best_above_dy = std::numeric_limits<int>::max();
            int              best_above_dx = std::numeric_limits<int>::max();

            for (const FocusNode& n : group.nodes)
            {
                if (n.kind != FocusNodeKind::Item)
                    continue;

                const int nx = n.key.grid_x;
                const int ny = n.key.grid_y;

                // Считаем «ниже/дальше по потоку чтения» клетки с (ny > py) или (ny == py && nx >= px)
                if (ny > py || (ny == py && nx >= px))
                {
                    const int dy = ny - py;
                    const int dx = PointToIntervalDist(px, nx, n.key.grid_w);
                    if (dy < best_below_dy || (dy == best_below_dy && dx < best_below_dx))
                    {
                        best_below_dy = dy;
                        best_below_dx = dx;
                        best_below    = &n;
                    }
                }
                else
                {
                    const int dy = py - ny;
                    const int dx = PointToIntervalDist(px, nx, n.key.grid_w);
                    if (dy < best_above_dy || (dy == best_above_dy && dx < best_above_dx))
                    {
                        best_above_dy = dy;
                        best_above_dx = dx;
                        best_above    = &n;
                    }
                }
            }

            if (best_below != nullptr)
            {
                SyncAnchorFromNode(group, *best_below);
                group.last_restore_reason = FocusRestoreReason::NeighborBelow;
                return group.last_restore_reason;
            }
            if (best_above != nullptr)
            {
                SyncAnchorFromNode(group, *best_above);
                group.last_restore_reason = FocusRestoreReason::NeighborAbove;
                return group.last_restore_reason;
            }
        }

        // ---------------------------------------------------------------------
        // Приоритет 5 (Первый в группе — FirstInGroup)
        // ---------------------------------------------------------------------
        for (const FocusNode& n : group.nodes)
        {
            if (n.kind == FocusNodeKind::Item)
            {
                SyncAnchorFromNode(group, n);
                group.last_restore_reason = FocusRestoreReason::FirstInGroup;
                return group.last_restore_reason;
            }
        }

        // ---------------------------------------------------------------------
        // Приоритет 6 (Заголовок / пустое состояние — EmptyGroupHeader)
        // ---------------------------------------------------------------------
        const uint32_t fallback_id = group.nodes.empty() ? kInvalidNodeId : group.nodes.front().node_id;
        group.current_node_id      = fallback_id;
        group.last_valid_node_id   = fallback_id;
        group.last_restore_reason  = FocusRestoreReason::EmptyGroupHeader;
        return group.last_restore_reason;
    }

    uint32_t FocusGraph::RestoreFocusNaiveOldBehavior(
        const std::vector<FocusNode>& new_item_nodes,
        const StableItemID&           prev_stable_id,
        const FocusItemKey&           prev_key,
        uint8_t                       prev_tab_id,
        uint8_t                       new_tab_id)
    {
        // Воспроизводит поведение БЕЗ графа фокуса и FocusKey:
        // 1. В пустом контейнере нет узла EmptyStatePlaceholder -> возвращает kInvalidNodeId (висячий фокус).
        if (new_item_nodes.empty())
            return kInvalidNodeId;

        // 2. В ванильном UICellContainer:RemoveIndex (utils_ui.script:1500-1509)
        //    при расходовании ведущего предмета стака (ResetToChild, utils_ui.script:1268-1296) старый ID удаляется,
        //    вызывается self:On_Select(false) и self.selected = nil (utils_ui.script:2024) -> фокус сбрасывается в kInvalidNodeId!
        for (const FocusNode& n : new_item_nodes)
        {
            if (prev_key.cell_indx >= 0 &&
                n.key.cell_indx == prev_key.cell_indx &&
                n.key.section == prev_key.section &&
                n.stable_id != prev_stable_id)
            {
                return kInvalidNodeId;
            }
        }

        // 3. При смене вкладки пытается удержать старый индекс вместо перехода на 1-й элемент вкладки,
        //    либо теряет фокус при выходе индекса за границу.
        (void)prev_tab_id;
        (void)new_tab_id;

        // 4. Наивное удержание старого индекса / координаты:
        //    - если удалён последний элемент (prev_key.list_index == new_item_nodes.size()),
        //      индекс выходит за пределы массива -> возвращает kInvalidNodeId!
        //    - если список пересортирован (SortingPlus), остаётся на старом индексе,
        //      теряя сам выбранный предмет.
        if (prev_key.list_index >= 0 &&
            static_cast<size_t>(prev_key.list_index) < new_item_nodes.size())
        {
            return new_item_nodes[static_cast<size_t>(prev_key.list_index)].node_id;
        }

        return kInvalidNodeId;
    }

    std::string FocusGraph::FormatDump() const
    {
        std::ostringstream oss;
        oss << "[gp_focus_dump] groups=" << m_groups.size()
            << " active_group=" << (m_active_group_id == kInvalidGroupId ? -1 : static_cast<int>(m_active_group_id))
            << "\n";

        for (const FocusGroup& g : m_groups)
        {
            const bool is_active = (g.group_id == m_active_group_id);
            oss << "  Group[" << g.group_id << "] '" << g.name << "'"
                << (is_active ? " [ACTIVE]" : "")
                << " mode=" << (g.layout_mode == FocusLayoutMode::Grid2D ? "Grid2D" :
                               (g.layout_mode == FocusLayoutMode::List1D ? "List1D" : "Custom"))
                << " tab=" << static_cast<int>(g.active_tab_id)
                << " items=" << g.CountItemNodes()
                << " cur_node=" << (g.current_node_id == kInvalidNodeId ? -1 : static_cast<int>(g.current_node_id))
                << " last_valid=" << (g.last_valid_node_id == kInvalidNodeId ? -1 : static_cast<int>(g.last_valid_node_id))
                << " pref_col=" << g.preferred_col
                << " last_restore=" << FocusRestoreReasonToString(g.last_restore_reason)
                << " rebuilds=" << g.rebuild_count
                << "\n";

            if (g.has_anchor)
            {
                oss << "    Anchor: stable=(" << g.anchor_stable_id.session_gen
                    << ":" << g.anchor_stable_id.object_id
                    << ":" << g.anchor_stable_id.instance_gen << ")"
                    << " key={sec='" << g.anchor_key.section << "'"
                    << " cond=" << g.anchor_key.cond_permille
                    << " uses=" << g.anchor_key.uses_or_ammo
                    << " grid=(" << g.anchor_key.grid_x << "," << g.anchor_key.grid_y
                    << " " << g.anchor_key.grid_w << "x" << g.anchor_key.grid_h << ")"
                    << " idx=" << g.anchor_key.list_index
                    << " cell=" << g.anchor_key.cell_indx << "}\n";
            }

            for (const FocusNode& n : g.nodes)
            {
                const bool is_cur = (n.node_id == g.current_node_id);
                oss << "    " << (is_cur ? "-> " : "   ")
                    << "Node[" << n.node_id << "] kind=" << FocusNodeKindToString(n.kind)
                    << " label='" << n.label << "'"
                    << " stable=(" << n.stable_id.session_gen << ":" << n.stable_id.object_id << ":" << n.stable_id.instance_gen << ")"
                    << " sec='" << n.key.section << "'"
                    << " pos=(" << n.key.grid_x << "," << n.key.grid_y << " " << n.key.grid_w << "x" << n.key.grid_h << ")"
                    << " idx=" << n.key.list_index
                    << " nbr=[U:" << (n.GetNeighbor(NavDir::Up) == kInvalidNodeId ? -1 : static_cast<int>(n.GetNeighbor(NavDir::Up)))
                    << " D:" << (n.GetNeighbor(NavDir::Down) == kInvalidNodeId ? -1 : static_cast<int>(n.GetNeighbor(NavDir::Down)))
                    << " L:" << (n.GetNeighbor(NavDir::Left) == kInvalidNodeId ? -1 : static_cast<int>(n.GetNeighbor(NavDir::Left)))
                    << " R:" << (n.GetNeighbor(NavDir::Right) == kInvalidNodeId ? -1 : static_cast<int>(n.GetNeighbor(NavDir::Right)))
                    << "]\n";
            }
        }
        return oss.str();
    }

    namespace
    {
        FocusNode MakeTestItem(
            uint32_t           node_id,
            uint16_t           obj_id,
            uint64_t           inst_gen,
            const std::string& section,
            int32_t            list_idx,
            int16_t            gx         = 0,
            int16_t            gy         = 0,
            int16_t            gw         = 1,
            int16_t            gh         = 1,
            uint16_t           cond_perm  = 1000,
            uint16_t           uses       = 1,
            int32_t            cell_indx  = -1,
            uint32_t           sess_gen   = 1)
        {
            FocusNode n;
            n.node_id                = node_id;
            n.kind                   = FocusNodeKind::Item;
            n.stable_id.session_gen  = sess_gen;
            n.stable_id.object_id    = obj_id;
            n.stable_id.instance_gen = inst_gen;
            n.key.section            = section;
            n.key.cond_permille      = cond_perm;
            n.key.uses_or_ammo       = uses;
            n.key.list_index         = list_idx;
            n.key.grid_x             = gx;
            n.key.grid_y             = gy;
            n.key.grid_w             = gw;
            n.key.grid_h             = gh;
            n.key.cell_indx          = (cell_indx >= 0) ? cell_indx : list_idx;
            n.label                  = section + "#" + std::to_string(obj_id);
            return n;
        }
    } // anonymous namespace

    SelfTestSummary RunFocusGraphSelfTest()
    {
        SelfTestSummary summary;
        std::ostringstream log;

        auto check = [&](const char* name, bool cond, bool old_caught_regression = false)
        {
            ++summary.total_cases;
            if (cond)
            {
                ++summary.passed_cases;
                if (old_caught_regression)
                    ++summary.old_behavior_regressions;
            }
            else
            {
                ++summary.failed_cases;
                log << "  [FAIL] " << name << "\n";
            }
        };

        // 1. Удаление первого / среднего / последнего / единственного / всех в 1D-списке
        {
            FocusGraph fg;
            uint32_t g0 = fg.AddGroup("PlayerBag", FocusLayoutMode::List1D, 1, false);
            std::vector<FocusNode> init = {
                MakeTestItem(1, 101, 1, "bandage",     0),
                MakeTestItem(2, 102, 1, "medkit",      1),
                MakeTestItem(3, 103, 1, "antirad",     2),
                MakeTestItem(4, 104, 1, "conserva",    3)
            };
            fg.RebuildGroupItems(g0, init, 0, 100);
            fg.SetFocusedNode(g0, 1);

            // Удаление первого -> сосед снизу (medkit, node 2)
            std::vector<FocusNode> after_del_first = { init[1], init[2], init[3] };
            auto r1 = fg.RebuildGroupItems(g0, after_del_first, 0, 120);
            check("1D_delete_first", r1 == FocusRestoreReason::NeighborBelow && fg.GetFocusedNode()->stable_id.object_id == 102);

            // Удаление среднего (antirad) -> сосед снизу (conserva, obj 104)
            fg.SetFocusedNode(g0, 3);
            std::vector<FocusNode> after_del_mid = { init[1], init[3] };
            auto r2 = fg.RebuildGroupItems(g0, after_del_mid, 0, 140);
            check("1D_delete_middle", r2 == FocusRestoreReason::NeighborBelow && fg.GetFocusedNode()->stable_id.object_id == 104);

            // Удаление последнего (conserva) -> сосед сверху (medkit, obj 102)
            uint32_t old_last = FocusGraph::RestoreFocusNaiveOldBehavior({ init[1] }, init[3].stable_id, fg.GetGroup(g0)->anchor_key, 0, 0);
            std::vector<FocusNode> after_del_last = { init[1] };
            auto r3 = fg.RebuildGroupItems(g0, after_del_last, 0, 160);
            check("1D_delete_last", r3 == FocusRestoreReason::NeighborAbove && fg.GetFocusedNode()->stable_id.object_id == 102, old_last == kInvalidNodeId);

            // Удаление единственного -> EmptyGroupHeader
            uint32_t old_single = FocusGraph::RestoreFocusNaiveOldBehavior({}, init[1].stable_id, fg.GetGroup(g0)->anchor_key, 0, 0);
            auto r4 = fg.RebuildGroupItems(g0, {}, 0, 180);
            check("1D_delete_single_to_empty",
                r4 == FocusRestoreReason::EmptyGroupHeader &&
                fg.GetFocusedNode() != nullptr &&
                fg.GetFocusedNode()->kind == FocusNodeKind::EmptyStatePlaceholder,
                old_single == kInvalidNodeId);
        }

        // 2. Расходование стака (ResetToChild)
        {
            FocusGraph fg;
            uint32_t g0 = fg.AddGroup("PlayerBag", FocusLayoutMode::Grid2D, 1, false);
            std::vector<FocusNode> before_use = {
                MakeTestItem(1, 201, 1, "medkit_army", 0, 0, 0, 1, 1, 1000, 3, 5),
                MakeTestItem(2, 202, 1, "wpn_pm",      1, 1, 0, 2, 1, 950,  1, 6)
            };
            fg.RebuildGroupItems(g0, before_use, 0, 200);
            fg.SetFocusedNode(g0, 1);

            // В ячейке cell_indx=5 объект 201 удалён, ведущим стал дочерний объект 205 той же секции medkit_army
            std::vector<FocusNode> after_use = {
                MakeTestItem(10, 205, 2, "medkit_army", 0, 0, 0, 1, 1, 1000, 2, 5),
                MakeTestItem(2,  202, 1, "wpn_pm",      1, 1, 0, 2, 1, 950,  1, 6)
            };
            uint32_t old_stack = FocusGraph::RestoreFocusNaiveOldBehavior(after_use, before_use[0].stable_id, before_use[0].key, 0, 0);
            auto r = fg.RebuildGroupItems(g0, after_use, 0, 220);
            check("stack_reset_to_child",
                r == FocusRestoreReason::StackPromotedChild &&
                fg.GetFocusedNode()->stable_id.object_id == 205,
                old_stack == kInvalidNodeId);
        }

        // 3. Пересортировка (SortingPlus) и переключение колонок по View (D24)
        {
            FocusGraph fg;
            uint32_t g_left  = fg.AddGroup("ActorBag", FocusLayoutMode::Grid2D, 1, false);
            uint32_t g_right = fg.AddGroup("LootBag",  FocusLayoutMode::Grid2D, 2, false);

            std::vector<FocusNode> left_items = {
                MakeTestItem(1, 301, 1, "wpn_ak74",    0, 0, 0, 5, 2, 820, 1, 0),
                MakeTestItem(2, 302, 1, "ammo_5.45",   1, 0, 2, 2, 1, 1000, 30, 1)
            };
            std::vector<FocusNode> right_items = {
                MakeTestItem(1, 401, 1, "af_medusa",   0, 0, 0, 1, 1, 1000, 1, 0),
                MakeTestItem(2, 402, 1, "af_cristall", 1, 1, 0, 1, 1, 1000, 1, 1)
            };
            fg.RebuildGroupItems(g_left,  left_items,  0, 300);
            fg.RebuildGroupItems(g_right, right_items, 0, 300);

            fg.SetFocusedNode(g_left, 2);  // ammo_5.45 в левой колонке
            fg.SwitchToGroup(g_right, 310);
            fg.SetFocusedNode(g_right, 2); // af_cristall в правой колонке
            fg.SwitchGroupNext(320);       // обратно в левую колонку по View

            check("view_column_switch_memory",
                fg.GetActiveGroupId() == g_left &&
                fg.GetFocusedNode() != nullptr &&
                fg.GetFocusedNode()->stable_id.object_id == 302);

            // Пересортировка левой колонки: ammo_5.45 переезжает на (0,0) с индексом 0, а ak74 на (0,1) с индексом 1
            std::vector<FocusNode> resorted = {
                MakeTestItem(10, 302, 1, "ammo_5.45", 0, 0, 0, 2, 1, 1000, 30, 0),
                MakeTestItem(11, 301, 1, "wpn_ak74",  1, 0, 1, 5, 2, 820,  1,  1)
            };
            uint32_t old_sort = FocusGraph::RestoreFocusNaiveOldBehavior(resorted, left_items[1].stable_id, left_items[1].key, 0, 0);
            auto r_sort = fg.RebuildGroupItems(g_left, resorted, 0, 330);
            check("resort_keeps_same_item",
                r_sort == FocusRestoreReason::SameItem &&
                fg.GetFocusedNode()->stable_id.object_id == 302,
                old_sort != 10);
        }

        log << "[gp_focus_dump selftest] total=" << summary.total_cases
            << " passed=" << summary.passed_cases
            << " failed=" << summary.failed_cases
            << " old_regressions_caught=" << summary.old_behavior_regressions;
        summary.report = log.str();
        return summary;
    }

} // namespace gpfocus
