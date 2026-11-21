#include "stdafx.h"
#include "UITaskWnd.h"
#include "UIMapWnd.h"
#include "object_broker.h"
#include "UIXmlInit.h"
#include "UIStatic.h"
#include "UI3tButton.h"
#include "UIFrameLineWnd.h"
#include "UISecondTaskWnd.h"
#include "UIMapLegend.h"
#include "UIHelper.h"
#include "UIHint.h"

#include "../gametask.h"
#include "../map_location.h"
#include "../map_location_defs.h"
#include "../map_manager.h"
#include "UIInventoryUtilities.h"
#include "../string_table.h"
#include "../level.h"
#include "../gametaskmanager.h"
#include "../actor.h"
#include "UICheckButton.h"
#include "UIScrollView.h"
#include "../UICursor.h"
#include "../ui_base.h"

int g_ui_pda_task_cache = 1;

CUITaskWnd::CUITaskWnd()
{
	hint_wnd = NULL;
	m_actual_frame = u32(-1);
	m_map_locations_version = u32(-1);
}

CUITaskWnd::~CUITaskWnd()
{
	delete_data(m_pMapWnd);
}

void CUITaskWnd::Init()
{
	CUIXml xml;
	xml.Load(CONFIG_PATH, UI_PATH, PDA_TASK_XML);
	VERIFY(hint_wnd);

	CUIXmlInit::InitWindow(xml, "main_wnd", 0, this);

	m_background = UIHelper::CreateFrameWindow(xml, "background", this);

	m_cbTreasures = UIHelper::CreateCheck(xml, "filter_treasures", this);
	m_cbTreasures->SetCheck(true);
	AddCallback(m_cbTreasures, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUITaskWnd::OnShowTreasures));
	m_bTreasuresEnabled = true;

	m_cbPrimaryObjects = UIHelper::CreateCheck(xml, "filter_primary_objects", this);
	m_cbPrimaryObjects->SetCheck(true);
	AddCallback(m_cbPrimaryObjects, BUTTON_CLICKED,
	            CUIWndCallback::void_function(this, &CUITaskWnd::OnShowPrimaryObjects));
	m_bPrimaryObjectsEnabled = true;

	m_cbSecondaryTasks = UIHelper::CreateCheck(xml, "filter_secondary_tasks", this);
	m_cbSecondaryTasks->SetCheck(true);
	AddCallback(m_cbSecondaryTasks, BUTTON_CLICKED,
	            CUIWndCallback::void_function(this, &CUITaskWnd::OnShowSecondaryTasks));
	m_bSecondaryTasksEnabled = true;

	m_cbQuestNpcs = UIHelper::CreateCheck(xml, "filter_quest_npcs", this);
	m_cbQuestNpcs->SetCheck(true);
	AddCallback(m_cbQuestNpcs, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUITaskWnd::OnShowQuestNpcs));
	m_bQuestNpcsEnabled = true;

	m_pMapWnd = xr_new<CUIMapWnd>();
	m_pMapWnd->SetAutoDelete(false);
	m_pMapWnd->hint_wnd = hint_wnd;
	m_pMapWnd->Init(PDA_TASK_XML, "map_wnd");
	AttachChild(m_pMapWnd);

	m_center_background = UIHelper::CreateStatic(xml, "center_background", this);
	m_devider = UIHelper::CreateStatic(xml, "line_devider", this);

	m_pStoryLineTaskItem = xr_new<CUITaskItem>();
	m_pStoryLineTaskItem->Init(xml, "storyline_task_item");
	AttachChild(m_pStoryLineTaskItem);
	m_pStoryLineTaskItem->SetAutoDelete(true);
	AddCallback(m_pStoryLineTaskItem, WINDOW_LBUTTON_DB_CLICK,
	            CUIWndCallback::void_function(this, &CUITaskWnd::OnTask1DbClicked));

	m_btn_focus = UIHelper::Create3tButton(xml, "btn_task_focus", this);
	Register(m_btn_focus);
	AddCallback(m_btn_focus, BUTTON_DOWN, CUIWndCallback::void_function(this, &CUITaskWnd::OnTask1DbClicked));

	m_BtnTaskListWnd = UIHelper::Create3tButton(xml, "btn_second_task", this);
	AddCallback(m_BtnTaskListWnd, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUITaskWnd::OnShowTaskListWnd));

	m_task_wnd = xr_new<UITaskListWnd>();
	m_task_wnd->SetAutoDelete(true);
	m_task_wnd->hint_wnd = hint_wnd;
	m_task_wnd->init_from_xml(xml, "second_task_wnd");
	m_pMapWnd->AttachChild(m_task_wnd);
	m_task_wnd->SetMessageTarget(this);
	m_task_wnd->Show(false);
	m_task_wnd_show = false;

	m_map_legend_wnd = xr_new<UIMapLegend>();
	m_map_legend_wnd->SetAutoDelete(true);
	m_map_legend_wnd->init_from_xml(xml, "map_legend_wnd");
	m_pMapWnd->AttachChild(m_map_legend_wnd);
	m_map_legend_wnd->SetMessageTarget(this);
	m_map_legend_wnd->Show(false);
}

void CUITaskWnd::Update()
{
	if (!g_ui_pda_task_cache || Level().GameTaskManager().ActualFrame() != m_actual_frame ||
		Level().MapManager().LocationsVersion() != m_map_locations_version)
	{
		ReloadTaskInfo(g_ui_pda_task_cache == 0);
	}

	if (m_pStoryLineTaskItem->show_hint && m_pStoryLineTaskItem->OwnerTask())
	{
		m_pMapWnd->ShowHintTask(m_pStoryLineTaskItem->OwnerTask(), m_pStoryLineTaskItem);
	}
	else
	{
		m_pMapWnd->HideCurHint();
	}
	inherited::Update();
}

void CUITaskWnd::Draw()
{
	inherited::Draw();
}

void CUITaskWnd::DrawHint()
{
	m_pMapWnd->DrawHint();
}


void CUITaskWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	if (msg == PDA_TASK_SET_TARGET_MAP && pData)
	{
		CGameTask* task = static_cast<CGameTask*>(pData);
		TaskSetTargetMap(task);
		return;
	}
	if (msg == PDA_TASK_SHOW_MAP_SPOT && pData && m_bSecondaryTasksEnabled)
	{
		CGameTask* task = static_cast<CGameTask*>(pData);
		TaskShowMapSpot(task, true);
		return;
	}
	if (msg == PDA_TASK_HIDE_MAP_SPOT && pData)
	{
		CGameTask* task = static_cast<CGameTask*>(pData);
		TaskShowMapSpot(task, false);
		return;
	}

	if (msg == PDA_TASK_SHOW_HINT && pData)
	{
		CGameTask* task = static_cast<CGameTask*>(pData);
		m_pMapWnd->ShowHintTask(task, pWnd);
		return;
	}
	if (msg == PDA_TASK_HIDE_HINT)
	{
		m_pMapWnd->HideCurHint();
		return;
	}

	inherited::SendMessage(pWnd, msg, pData);
	CUIWndCallback::OnEvent(pWnd, msg, pData);
}

void CUITaskWnd::ReloadTaskInfo(bool force)
{
	const u32 actual_frame = Level().GameTaskManager().ActualFrame();
	const u32 locations_version = Level().MapManager().LocationsVersion();
	if (!force && actual_frame == m_actual_frame && locations_version == m_map_locations_version)
		return;

	m_actual_frame = actual_frame;

	CGameTask* t = Level().GameTaskManager().ActiveTask();
	m_pStoryLineTaskItem->InitTask(t);

	if (t && (t->m_map_object_id == u16(-1) || t->m_map_location.size() == 0))
		m_btn_focus->Show(false);
	else
		m_btn_focus->Show(true);

	const Locations& map_locs = Level().MapManager().Locations();
	for (const SLocationKey& map_location : map_locs)
	{
		const shared_str& spot = map_location.spot_type;
		if (spot == "treasure")
			m_bTreasuresEnabled ? map_location.location->EnableSpot() : map_location.location->DisableSpot();
		else if (spot == "primary_object")
			m_bPrimaryObjectsEnabled ? map_location.location->EnableSpot() : map_location.location->DisableSpot();
		else if (spot == "secondary_task_location" || spot == "secondary_task_location_complex_timer")
			(/*b->location->SpotEnabled() && */m_bSecondaryTasksEnabled)
				? map_location.location->EnableSpot()
				: map_location.location->DisableSpot();
		else if (spot == "ui_pda2_trader_location" || spot == "ui_pda2_mechanic_location" ||
			spot == "ui_pda2_scout_location" || spot == "ui_pda2_quest_npc_location" ||
			spot == "ui_pda2_medic_location" || spot == "ui_pda2_actor_box_location" ||
			spot == "ui_pda2_actor_sleep_location")
			m_bQuestNpcsEnabled ? map_location.location->EnableSpot() : map_location.location->DisableSpot();
	}
	m_map_locations_version = Level().MapManager().LocationsVersion();

	if (!t)
		return;

	u32 task_count = Level().GameTaskManager().GetTaskCount(eTaskStateInProgress);
	if (task_count)
	{
		u32 task_index = Level().GameTaskManager().GetTaskIndex(t, eTaskStateInProgress);
		string32 buf;
		xr_sprintf(buf, sizeof(buf), "%d / %d", task_index, task_count);
	}
	if (m_task_wnd->IsShown())
		m_task_wnd->UpdateList();
}

void CUITaskWnd::Show(bool status)
{
	inherited::Show(status);
	m_pMapWnd->Show(status);
	m_pMapWnd->HideCurHint();
	m_map_legend_wnd->Show(false);
	if (status)
	{
		ReloadTaskInfo(true);
		m_task_wnd->Show(m_task_wnd_show);
	}
	else
	{
		//		m_task_wnd_show = false;
		m_task_wnd->Show(false);
	}
}

void CUITaskWnd::Reset()
{
	inherited::Reset();
}

void CUITaskWnd::OnNextTaskClicked()
{
}

void CUITaskWnd::OnPrevTaskClicked()
{
}

void CUITaskWnd::OnShowTaskListWnd(CUIWindow* w, void* d)
{
	m_task_wnd_show = !m_task_wnd_show;
	m_task_wnd->Show(!m_task_wnd->IsShown());
}

void CUITaskWnd::Show_TaskListWnd(bool status)
{
	m_task_wnd->Show(status);
	m_task_wnd_show = status;
}

void CUITaskWnd::TaskSetTargetMap(CGameTask* task)
{
	if (!task || !m_bSecondaryTasksEnabled)
	{
		return;
	}

	TaskShowMapSpot(task, true);
	CMapLocation* ml = task->LinkedMapLocation();
	if (ml && ml->SpotEnabled())
	{
		ml->CalcPosition();
		m_pMapWnd->SetTargetMap(ml->GetLevelName(), ml->GetPosition(), true);
	}
}

void CUITaskWnd::TaskShowMapSpot(CGameTask* task, bool show)
{
	if (!task || !m_bSecondaryTasksEnabled)
	{
		return;
	}

	CMapLocation* ml = task->LinkedMapLocation();
	if (ml)
	{
		if (show)
		{
			ml->EnableSpot();
			ml->CalcPosition();
			m_pMapWnd->SetTargetMap(ml->GetLevelName(), ml->GetPosition(), true);
		}
		else
		{
			ml->DisableSpot();
		}
	}
}

void CUITaskWnd::OnTask1DbClicked(CUIWindow* ui, void* d)
{
	CGameTask* task = Level().GameTaskManager().ActiveTask();
	TaskSetTargetMap(task);
}

void CUITaskWnd::ShowMapLegend(bool status)
{
	m_map_legend_wnd->Show(status);
}

void CUITaskWnd::Switch_ShowMapLegend()
{
	m_map_legend_wnd->Show(!m_map_legend_wnd->IsShown());
}

void CUITaskWnd::OnShowTreasures(CUIWindow* ui, void* d)
{
	m_bTreasuresEnabled = !m_bTreasuresEnabled;
	ReloadTaskInfo(true);
}

void CUITaskWnd::OnShowPrimaryObjects(CUIWindow* ui, void* d)
{
	m_bPrimaryObjectsEnabled = !m_bPrimaryObjectsEnabled;
	ReloadTaskInfo(true);
}

void CUITaskWnd::OnShowSecondaryTasks(CUIWindow* ui, void* d)
{
	m_bSecondaryTasksEnabled = !m_bSecondaryTasksEnabled;
	ReloadTaskInfo(true);
}

void CUITaskWnd::OnShowQuestNpcs(CUIWindow* ui, void* d)
{
	m_bQuestNpcsEnabled = !m_bQuestNpcsEnabled;
	ReloadTaskInfo(true);
}

// --------------------------------------------------------------------------------------------------
CUITaskItem::CUITaskItem() :
	m_owner(NULL),
	m_hint_wt(500),
	show_hint(false),
	show_hint_can(false)
{
}

CUITaskItem::~CUITaskItem()
{
}

CUIStatic* init_static_field(CUIXml& uiXml, LPCSTR path, LPCSTR path2);

void CUITaskItem::Init(CUIXml& uiXml, LPCSTR path)
{
	CUIXmlInit::InitWindow(uiXml, path, 0, this);
	m_hint_wt = uiXml.ReadAttribInt(path, 0, "hint_wt", 500);

	string256 buff;
	CUIStatic* S = NULL;

	strconcat(sizeof(buff), buff, path, ":t_icon");
	if (uiXml.NavigateToNode(buff))
	{
		S = init_static_field(uiXml, path, "t_icon");
		AttachChild(S);
	}
	m_info["t_icon"] = S;

	strconcat(sizeof(buff), buff, path, ":t_icon_over");
	if (uiXml.NavigateToNode(buff))
	{
		S = init_static_field(uiXml, path, "t_icon_over");
		AttachChild(S);
	}
	m_info["t_icon_over"] = S;

	S = init_static_field(uiXml, path, "t_caption");
	AttachChild(S);
	m_info["t_caption"] = S;

	show_hint_can = false;
	show_hint = false;
}

void CUITaskItem::InitTask(CGameTask* task)
{
	m_owner = task;
	CUIStatic* S = m_info["t_icon"];
	if (S)
	{
		if (task)
		{
			S->InitTexture(task->m_icon_texture_name.c_str());
			S->SetStretchTexture(true);
			m_info["t_icon_over"]->Show(true);
		}
		else
		{
			S->TextureOff();
			m_info["t_icon_over"]->Show(false);
		}
	}

	S = m_info["t_caption"];
	S->TextItemControl()->SetTextST((task) ? task->m_Title.c_str() : "");
}

void CUITaskItem::OnFocusReceive()
{
	inherited::OnFocusReceive();
	show_hint_can = true;
	show_hint = false;
}

void CUITaskItem::OnFocusLost()
{
	inherited::OnFocusLost();
	show_hint_can = false;
	show_hint = false;
}

void CUITaskItem::Update()
{
	inherited::Update();
	if (m_owner && m_bCursorOverWindow && show_hint_can)
	{
		if (Device.dwTimeGlobal > (m_dwFocusReceiveTime + m_hint_wt))
		{
			show_hint = true;
			return;
		}
	}
}

void CUITaskItem::OnMouseScroll(float iDirection)
{
}

bool CUITaskItem::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	if (inherited::OnMouseAction(x, y, mouse_action))
	{
		//return true;
	}

	switch (mouse_action)
	{
	case WINDOW_LBUTTON_DOWN:
	case WINDOW_RBUTTON_DOWN:
	case BUTTON_DOWN:
		show_hint_can = false;
		show_hint = false;
		break;
	} //switch

	return true;
}

void CUITaskItem::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	inherited::SendMessage(pWnd, msg, pData);
}
#include "UIPropertiesBox.h"
#include "UIListBoxItem.h"

static void PushShownWindowRect(CUIWindow* w, xr_vector<Frect>& outRects)
{
	if (!w || !w->IsShown())
		return;
	Frect r;
	w->GetAbsoluteRect(r);
	if (r.width() > 2.0f && r.height() > 2.0f)
		outRects.push_back(r);
}

// FIX-LIVE3 p.22: one gamepad frame per task row = union of the name button and the "center" button.
static void PushTaskRowRect(UITaskListWndItem* taskItm, xr_vector<Frect>& outRects)
{
	if (!taskItm)
		return;
	Frect u;
	bool have = false;
	CUIWindow* parts[2] = {taskItm->GetNameBtn(), taskItm->GetFocusBtn()};
	for (CUIWindow* w : parts)
	{
		if (!w || !w->IsShown())
			continue;
		Frect r;
		w->GetAbsoluteRect(r);
		if (r.width() <= 2.0f || r.height() <= 2.0f)
			continue;
		if (!have)
		{
			u = r;
			have = true;
		}
		else
		{
			u.x1 = _min(u.x1, r.x1);
			u.y1 = _min(u.y1, r.y1);
			u.x2 = _max(u.x2, r.x2);
			u.y2 = _max(u.y2, r.y2);
		}
	}
	if (have)
		outRects.push_back(u);
}

static UITaskListWndItem* TaskItemAt(CUIScrollView* sv, int idx)
{
	if (!sv || idx < 0 || idx >= int(sv->GetSize()))
		return nullptr;
	return smart_cast<UITaskListWndItem*>(sv->GetItem(u32(idx)));
}

bool CUITaskWnd::GamepadTaskStep(int dir, Fvector2* outSnapPos)
{
	if (!m_task_wnd)
		return false;
	CUIScrollView* sv = m_task_wnd->GetTaskList();
	const bool wasOpen = m_task_wnd->IsShown();
	if (!wasOpen)
	{
		if (m_map_legend_wnd && m_map_legend_wnd->IsShown())
			m_map_legend_wnd->Show(false);
		Show_TaskListWnd(true);
	}
	if (!sv || sv->GetSize() == 0)
		return wasOpen != m_task_wnd->IsShown();
	const int n = int(sv->GetSize());
	if (!wasOpen || m_gpTaskIdx < 0 || m_gpTaskIdx >= n)
	{
		m_gpTaskIdx = 0;
		CGameTask* active = Level().GameTaskManager().ActiveTask();
		for (int i = 0; i < n; ++i)
		{
			UITaskListWndItem* it = TaskItemAt(sv, i);
			if (it && active && it->GetTask() == active)
			{
				m_gpTaskIdx = i;
				break;
			}
		}
	}
	else
	{
		m_gpTaskIdx = clampr(m_gpTaskIdx + dir, 0, n - 1);
	}
	UITaskListWndItem* item = TaskItemAt(sv, m_gpTaskIdx);
	if (!item)
		return true;
	// keep the selected row inside the visible part of the list
	Frect svr, ir;
	sv->GetAbsoluteRect(svr);
	item->GetAbsoluteRect(ir);
	int pos = sv->GetCurrentScrollPos();
	if (ir.y1 < svr.y1)
		pos -= iCeil(svr.y1 - ir.y1);
	else if (ir.y2 > svr.y2)
		pos += iCeil(ir.y2 - svr.y2);
	pos = clampr(pos, sv->GetMinScrollPos(), _max(sv->GetMinScrollPos(), sv->GetMaxScrollPos()));
	if (pos != sv->GetCurrentScrollPos())
		sv->SetScrollPos(pos);
	if (outSnapPos)
	{
		xr_vector<Frect> rr;
		PushTaskRowRect(item, rr);
		if (!rr.empty())
			outSnapPos->set((rr[0].x1 + rr[0].x2) * 0.5f, (rr[0].y1 + rr[0].y2) * 0.5f);
		else
		{
			item->GetAbsoluteRect(ir);
			outSnapPos->set((ir.x1 + ir.x2) * 0.5f, (ir.y1 + ir.y2) * 0.5f);
		}
	}
	return true;
}

bool CUITaskWnd::GamepadTaskPick()
{
	if (!m_task_wnd || !m_task_wnd->IsShown())
		return false;
	CUIScrollView* sv = m_task_wnd->GetTaskList();
	if (!sv)
		return false;
	// row under the cursor wins (LS/mouse may have moved it); else the D-pad selection
	UITaskListWndItem* item = nullptr;
	const Fvector2 cur = GetUICursor().GetCursorPosition();
	for (int i = 0, n = int(sv->GetSize()); i < n; ++i)
	{
		UITaskListWndItem* it = TaskItemAt(sv, i);
		if (!it)
			continue;
		xr_vector<Frect> rr;
		PushTaskRowRect(it, rr);
		if (!rr.empty() && rr[0].in(cur.x, cur.y))
		{
			item = it;
			m_gpTaskIdx = i;
			break;
		}
	}
	if (!item)
		item = TaskItemAt(sv, m_gpTaskIdx);
	if (!item || !item->GetTask())
		return false;
	CGameTask* task = item->GetTask();
	Level().GameTaskManager().SetActiveTask(task);
	Show_TaskListWnd(false);
	TaskSetTargetMap(task);
	return true;
}

bool CUITaskWnd::CloseTopTaskOverlay()
{
	if (m_pMapWnd && m_pMapWnd->CloseGamepadPropertiesBox())
		return true;
	if (m_task_wnd && m_task_wnd->IsShown())
	{
		m_task_wnd_show = false;
		m_task_wnd->Show(false);
		return true;
	}
	if (m_map_legend_wnd && m_map_legend_wnd->IsShown())
	{
		m_map_legend_wnd->Show(false);
		return true;
	}
	return false;
}

bool CUITaskWnd::ToggleTaskListWndGamepad(Fvector2* outSnapPos)
{
	OnShowTaskListWnd(nullptr, nullptr);
	if (outSnapPos)
	{
		if (m_task_wnd && m_task_wnd->IsShown())
		{
			CUIScrollView* sv = m_task_wnd->GetTaskList();
			if (sv && sv->GetSize() > 0)
			{
				CUIWindow* firstItem = sv->GetItem(0);
				if (firstItem)
				{
					Frect r;
					firstItem->GetAbsoluteRect(r);
					outSnapPos->set((r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f);
					return true;
				}
			}
			Frect r;
			m_task_wnd->GetAbsoluteRect(r);
			outSnapPos->set((r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f);
		}
		else if (m_BtnTaskListWnd && m_BtnTaskListWnd->IsShown())
		{
			Frect r;
			m_BtnTaskListWnd->GetAbsoluteRect(r);
			outSnapPos->set((r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f);
		}
	}
	return true;
}

void CUITaskWnd::GatherGamepadTaskTargets(xr_vector<Frect>& outRects) const
{
	if (m_pMapWnd && m_pMapWnd->GetPropertiesBox() && m_pMapWnd->GetPropertiesBox()->IsShown())
	{
		CUIPropertiesBox* pbox = m_pMapWnd->GetPropertiesBox();
		for (u32 i = 0, n = pbox->GetItemsCount(); i < n; ++i)
		{
			CUIListBoxItem* itm = pbox->GetItemByIDX(i);
			PushShownWindowRect(itm, outRects);
		}
		return;
	}

	PushShownWindowRect(m_BtnTaskListWnd, outRects);
	PushShownWindowRect(m_btn_focus, outRects);
	PushShownWindowRect(m_pStoryLineTaskItem, outRects);
	PushShownWindowRect(m_cbTreasures, outRects);
	PushShownWindowRect(m_cbQuestNpcs, outRects);
	PushShownWindowRect(m_cbSecondaryTasks, outRects);
	PushShownWindowRect(m_cbPrimaryObjects, outRects);

	if (m_task_wnd && m_task_wnd->IsShown())
	{
		PushShownWindowRect(m_task_wnd->GetCloseBtn(), outRects);
		CUIScrollView* sv = m_task_wnd->GetTaskList();
		if (sv && sv->IsShown())
		{
			Frect svRect;
			sv->GetAbsoluteRect(svRect);
			for (u32 i = 0, n = sv->GetSize(); i < n; ++i)
			{
				CUIWindow* w = sv->GetItem(i);
				UITaskListWndItem* taskItm = smart_cast<UITaskListWndItem*>(w);
				if (taskItm)
				{
					Frect ir;
					taskItm->GetAbsoluteRect(ir);
					const float cy = (ir.y1 + ir.y2) * 0.5f;
					if (cy >= svRect.y1 && cy <= svRect.y2)
						PushTaskRowRect(taskItm, outRects);
				}
			}
		}
	}

	if (m_pMapWnd && m_pMapWnd->IsShown())
	{
		m_pMapWnd->GatherGamepadMapSpotTargets(outRects);
	}
}

bool CUITaskWnd::HandleGamepadRightStick(float curX, float curY, int wheelDir)
{
	if (wheelDir == 0)
		return false;
	const EUIMessages msg = (wheelDir > 0) ? WINDOW_MOUSE_WHEEL_UP : WINDOW_MOUSE_WHEEL_DOWN;

	if (m_task_wnd && m_task_wnd->IsShown())
	{
		Frect tr;
		m_task_wnd->GetAbsoluteRect(tr);
		if (tr.in(curX, curY))
		{
			CUIScrollView* sv = m_task_wnd->GetTaskList();
			if (sv)
			{
				Frect svr;
				sv->GetAbsoluteRect(svr);
				sv->OnMouseAction(curX - svr.x1, curY - svr.y1, msg);
				return true;
			}
		}
	}

	return false;
}

LPCSTR CUITaskWnd::GetGamepadOverlayName() const
{
	if (m_pMapWnd && m_pMapWnd->GetPropertiesBox() && m_pMapWnd->GetPropertiesBox()->IsShown())
		return "menu";
	if (m_task_wnd && m_task_wnd->IsShown())
		return "tasks";
	if (m_map_legend_wnd && m_map_legend_wnd->IsShown())
		return "legend";
	return "";
}

void CUITaskWnd::GatherGamepadFilterBarRects(xr_vector<Frect>& outRects) const
{
	PushShownWindowRect(m_BtnTaskListWnd, outRects);
	PushShownWindowRect(m_cbTreasures, outRects);
	PushShownWindowRect(m_cbPrimaryObjects, outRects);
	PushShownWindowRect(m_cbSecondaryTasks, outRects);
	PushShownWindowRect(m_cbQuestNpcs, outRects);
	for (size_t i = 1; i < outRects.size(); ++i)
	{
		const Frect key = outRects[i];
		size_t j = i;
		while (j > 0 && outRects[j - 1].x1 > key.x1)
		{
			outRects[j] = outRects[j - 1];
			--j;
		}
		outRects[j] = key;
	}
}

void CUITaskWnd::GatherGamepadOverlayTargets(xr_vector<Frect>& outRects) const
{
	if (m_pMapWnd && m_pMapWnd->GetPropertiesBox() && m_pMapWnd->GetPropertiesBox()->IsShown())
	{
		CUIPropertiesBox* pbox = m_pMapWnd->GetPropertiesBox();
		for (u32 i = 0, n = pbox->GetItemsCount(); i < n; ++i)
		{
			CUIListBoxItem* itm = pbox->GetItemByIDX(i);
			PushShownWindowRect(itm, outRects);
		}
		return;
	}
	if (m_task_wnd && m_task_wnd->IsShown())
	{
		PushShownWindowRect(m_task_wnd->GetCloseBtn(), outRects);
		CUIScrollView* sv = m_task_wnd->GetTaskList();
		if (sv && sv->IsShown())
		{
			Frect svRect;
			sv->GetAbsoluteRect(svRect);
			for (u32 i = 0, n = sv->GetSize(); i < n; ++i)
			{
				CUIWindow* w = sv->GetItem(i);
				UITaskListWndItem* taskItm = smart_cast<UITaskListWndItem*>(w);
				if (taskItm)
				{
					Frect ir;
					taskItm->GetAbsoluteRect(ir);
					const float cy = (ir.y1 + ir.y2) * 0.5f;
					if (cy >= svRect.y1 && cy <= svRect.y2)
						PushTaskRowRect(taskItm, outRects);
				}
			}
		}
		return;
	}
	if (m_map_legend_wnd && m_map_legend_wnd->IsShown())
	{
		PushShownWindowRect(m_map_legend_wnd->GetCloseBtn(), outRects);
	}
}

void CUITaskWnd::StepGamepadMapPan(float stickX, float stickY, bool bModifierHeld, float curX, float curY, u32 dtMs)
{
	if (!m_pMapWnd || !m_pMapWnd->IsShown())
		return;
	if (GetGamepadOverlayName()[0] != '\0')
		return;

	const float dtSec = clampr(float(dtMs) * 0.001f, 0.001f, 0.1f);
	const float panSpeed = 680.0f;

	if (bModifierHeld)
	{
		if (_abs(stickX) > 0.05f || _abs(stickY) > 0.05f)
		{
			// SDL/ui_stick: stickY < 0 is UP on stick (screen -Y); MoveMap takes map delta (+Y shifts map down)
			Fvector2 delta;
			delta.set(-stickX * panSpeed * dtSec, -stickY * panSpeed * dtSec);
			m_pMapWnd->MoveMap(delta, true);
		}
		return;
	}

	const Frect mapRect = m_pMapWnd->ActiveMapRect();
	if (!mapRect.in(curX, curY))
		return;

	const float edgeMargin = 64.0f;
	float panX = 0.0f;
	float panY = 0.0f;

	if (curX <= mapRect.x1 + edgeMargin && stickX < -0.15f)
		panX = -stickX * panSpeed * dtSec;
	else if (curX >= mapRect.x2 - edgeMargin && stickX > 0.15f)
		panX = -stickX * panSpeed * dtSec;

	if (curY <= mapRect.y1 + edgeMargin && stickY < -0.15f)
		panY = -stickY * panSpeed * dtSec;
	else if (curY >= mapRect.y2 - edgeMargin && stickY > 0.15f)
		panY = -stickY * panSpeed * dtSec;

	if (_abs(panX) > EPS_S || _abs(panY) > EPS_S)
	{
		Fvector2 delta;
		delta.set(panX, panY);
		m_pMapWnd->MoveMap(delta, true);
	}
}
