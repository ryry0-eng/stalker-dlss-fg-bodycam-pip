#include "stdafx.h"
#include "UIPdaWnd.h"
#include "../Pda.h"

#include "xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIInventoryUtilities.h"

#include "../level.h"
#include "UIGameCustom.h"

#include "UIStatic.h"
#include "UIFrameWindow.h"
#include "UITabControl.h"
#include "UIMapWnd.h"
#include "UIFrameLineWnd.h"
#include "object_broker.h"
#include "UIMessagesWindow.h"
#include "UIMainIngameWnd.h"
#include "UITabButton.h"
#include "UIAnimatedStatic.h"

#include "UIHelper.h"
#include "UIHint.h"
#include "UIBtnHint.h"
#include "UITaskWnd.h"
#include "UIRankingWnd.h"
#include "UILogsWnd.h"

#include "UIScriptWnd.h"

#include "Actor.h"
#include "Inventory.h"
#include "../xrEngine/XR_IOConsole.h"
#include "ui\UIProgressBar.h"
#include "player_hud.h"

u32 g_pda_info_state = 0;

void RearrangeTabButtons(CUITabControl* pTab);

CUIPdaWnd::CUIPdaWnd()
{
	pUITaskWnd = NULL;
	pUIRankingWnd = NULL;
	pUILogsWnd = NULL;
	m_hint_wnd = NULL;
	m_battery_bar = NULL;
	m_power = 0.f;
	last_cursor_pos.set(UI_BASE_WIDTH / 2.f, UI_BASE_HEIGHT / 2.f);
	m_cursor_box.set(117.f, 39.f, UI_BASE_WIDTH - 121.f, UI_BASE_HEIGHT - 37.f);
	g_pda_xml_path = "pda.xml";
	Init();
}

CUIPdaWnd::~CUIPdaWnd()
{
	delete_data(pUITaskWnd);
	delete_data(pUIRankingWnd);
	delete_data(pUILogsWnd);
	delete_data(m_hint_wnd);
	delete_data(UINoice);
}

void CUIPdaWnd::Init()
{
	CUIXml uiXml;
	uiXml.Load(CONFIG_PATH, UI_PATH, GetPdaXml());

	m_pActiveDialog = NULL;
	m_sActiveSection = "";

	CUIXmlInit::InitWindow(uiXml, "main", 0, this);

	UIMainPdaFrame = UIHelper::CreateStatic(uiXml, "background_static", this);
	m_caption = UIHelper::CreateTextWnd(uiXml, "caption_static", this);
	m_caption_const = (m_caption->GetText());
	m_clock = UIHelper::CreateTextWnd(uiXml, "clock_wnd", this);

	/*
		m_anim_static			= xr_new<CUIAnimatedStatic>();
		AttachChild				(m_anim_static);
		m_anim_static->SetAutoDelete(true);
		CUIXmlInit::InitAnimatedStatic(uiXml, "anim_static", 0, m_anim_static);
	*/
	//m_btn_close = UIHelper::Create3tButton(uiXml, "close_button", this);
	m_hint_wnd = UIHelper::CreateHint(uiXml, "hint_wnd");

	m_battery_bar = xr_new<CUIProgressBar>();
	m_battery_bar->SetAutoDelete(true);
	AttachChild(m_battery_bar);
	CUIXmlInit::InitProgressBar(uiXml, "battery_bar", 0, m_battery_bar);
	m_battery_bar->Show(true);

	if (IsGameTypeSingle())
	{
		pUITaskWnd = xr_new<CUITaskWnd>();
		pUITaskWnd->hint_wnd = m_hint_wnd;
		pUITaskWnd->Init();

		//-		pUIFactionWarWnd				= xr_new<CUIFactionWarWnd>();
		//-		pUIFactionWarWnd->hint_wnd		= m_hint_wnd;
		//-		pUIFactionWarWnd->Init			();

		pUIRankingWnd = xr_new<CUIRankingWnd>();
		pUIRankingWnd->Init();

		pUILogsWnd = xr_new<CUILogsWnd>();
		pUILogsWnd->Init();
	}

	UITabControl = xr_new<CUITabControl>();
	UITabControl->SetAutoDelete(true);
	AttachChild(UITabControl);
	CUIXmlInit::InitTabControl(uiXml, "tab", 0, UITabControl);
	UITabControl->SetMessageTarget(this);

	RebuildExtraTabs();

	UINoice = xr_new<CUIStatic>();
	UINoice->SetAutoDelete(true);
	CUIXmlInit::InitStatic(uiXml, "noice_static", 0, UINoice);

	//	RearrangeTabButtons		(UITabControl);
}

void CUIPdaWnd::RebuildExtraTabs()
{
	if (!UITabControl)
		return;

	shared_str active = UITabControl->GetActiveId();

	UITabControl->RemoveDynamicTabs();
	::luabind::functor<void> build_tabs;
	if (ai().script_engine().functor("pda_dynamic_tabs.build_extra_tabs", build_tabs))
		build_tabs(UITabControl);
	UITabControl->RecalcScroll();

	if (active.size())
	{
		CUITabButton* btn = UITabControl->GetButtonById(active);
		if (btn && !btn->m_dynamic)
		{
			UITabControl->EnsureVisible(active);
			return;
		}
		if (!btn)
		{
			TABS_VECTOR* tabs = UITabControl->GetButtonsVector();
			active = tabs->empty() ? "" : (*tabs)[0]->m_btn_id;
		}
		if (active.size())
		{
			UITabControl->ResetTab();
			UITabControl->SetActiveTab(active);
		}
	}
}

void CUIPdaWnd::SetPdaXml(LPCSTR xml)
{
	if (m_pActiveDialog) {
		if (UIMainPdaFrame->IsChild(m_pActiveDialog)) {
			UIMainPdaFrame->DetachChild(m_pActiveDialog);
		}
		m_pActiveDialog->Show(false);
	}

	g_pda_xml_path = xml;
	Init();
}

void CUIPdaWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	switch (msg)
	{
	case TAB_CHANGED:
		{
			if (pWnd == UITabControl)
			{
				SetActiveSubdialog(UITabControl->GetActiveId());
			}
			break;
		}
	case BUTTON_CLICKED:
		{
			if (pWnd == m_btn_close)
			{
				if (smart_cast<CPda*>(Actor()->inventory().ActiveItem()))
					Actor()->inventory().Activate(NO_ACTIVE_SLOT);
			}
			break;
		}
	default:
		{
			if (m_pActiveDialog)
				m_pActiveDialog->SendMessage(pWnd, msg, pData);
		}
	};
}

bool CUIPdaWnd::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	switch (mouse_action)
	{
	case WINDOW_LBUTTON_DOWN:
	case WINDOW_RBUTTON_DOWN:
	case WINDOW_LBUTTON_UP:
	case WINDOW_RBUTTON_UP:
	{
		CPda* pda = smart_cast<CPda*>(Actor()->inventory().ActiveItem());
		if (pda)
		{
			if (pda->IsPending())
				return true;

			if (mouse_action == WINDOW_LBUTTON_DOWN)
				bButtonL = true;
			else if (mouse_action == WINDOW_RBUTTON_DOWN)
				bButtonR = true;
			else if (mouse_action == WINDOW_LBUTTON_UP)
				bButtonL = false;
			else if (mouse_action == WINDOW_RBUTTON_UP)
				bButtonR = false;
		}
		break;
	}
	}
	CUIDialogWnd::OnMouseAction(x, y, mouse_action);
	return true; //always true because StopAnyMove() == false
}

void CUIPdaWnd::MouseMovement(float x, float y)
{
	CPda* pda = smart_cast<CPda*>(Actor()->inventory().ActiveItem());
	if (!pda) return;

	x *= .1f;
	y *= .1f;
	clamp(x, -.15f, .15f);
	clamp(y, -.15f, .15f);

	if (_abs(x) < .05f)
		x = 0.f;

	if (_abs(y) < .05f)
		y = 0.f;

	bool buttonpressed = (bButtonL || bButtonR);

	target_buttonpress = (buttonpressed ? -.0015f : 0.f);
	target_joystickrot.set(x*-.75f, 0.f, y*.75f);

	x += y * pda->m_thumb_rot[0];
	y += x * pda->m_thumb_rot[1];

	g_player_hud->m_bone_callback_params[r_finger0]->m_target.set(y*.15f, y*-.05f, (x*-.15f) + (buttonpressed ? .002f : 0.f));
	g_player_hud->m_bone_callback_params[r_finger01]->m_target.set(0.f, 0.f, (x*-.25f) + (buttonpressed ? .01f : 0.f));
	g_player_hud->m_bone_callback_params[r_finger02]->m_target.set(0.f, 0.f, (x*.75f) + (buttonpressed ? .025f : 0.f));
}

void CUIPdaWnd::Show(bool status)
{
	inherited::Show(status);
	if (status)
	{
		InventoryUtilities::SendInfoToActor("ui_pda");

		if (m_sActiveSection == NULL || strcmp(m_sActiveSection.c_str(), "") == 0)
		{
			SetActiveSubdialog("eptTasks");
			UITabControl->SetActiveTab("eptTasks");
		}
		else
			SetActiveSubdialog(m_sActiveSection);

		CurrentGameUI()->HideActorMenu();
	}
	else
	{
		InventoryUtilities::SendInfoToActor("ui_pda_hide");
		CurrentGameUI()->UIMainIngameWnd->SetFlashIconState_(CUIMainIngameWnd::efiPdaTask, false);
		if (m_pActiveDialog)
		{
			m_pActiveDialog->Show(false);
			m_pActiveDialog = pUITaskWnd; //hack for script window
		}
		g_btnHint->Discard();
		g_statHint->Discard();
	}
}

void CUIPdaWnd::Update()
{
	inherited::Update();
	if (m_pActiveDialog)
		m_pActiveDialog->Update();

	m_clock->TextItemControl().SetText(
		InventoryUtilities::GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str());

	m_battery_bar->SetProgressPos(m_power);

	pUILogsWnd->PerformWork();
}

void CUIPdaWnd::SetActiveSubdialog(const shared_str& section)
{
	if (m_pActiveDialog)
	{
		//if (m_sActiveSection == section) return;
		if (UIMainPdaFrame->IsChild(m_pActiveDialog))
			UIMainPdaFrame->DetachChild(m_pActiveDialog);
		m_pActiveDialog->Show(false);
	}

	//if (UITabControl->GetActiveId() != section)
	//{
	//	UITabControl->SetActiveTab(section);
	//}

	if (section == "eptTasks")
	{
		m_pActiveDialog = pUITaskWnd;
	}
	else if (section == "eptRanking")
	{
		m_pActiveDialog = pUIRankingWnd;
	}
	else if (section == "eptLogs")
	{
		m_pActiveDialog = pUILogsWnd;
	}

	::luabind::functor<CUIDialogWndEx*> funct;
	if (ai().script_engine().functor("pda.set_active_subdialog", funct))
	{
		CUIDialogWndEx* ret = funct((LPCSTR)section.c_str());
		CUIWindow* pScriptWnd = ret ? smart_cast<CUIWindow*>(ret) : (0);
		if (pScriptWnd)
			m_pActiveDialog = pScriptWnd;
	}

	if (m_pActiveDialog)
	{
		if (!UIMainPdaFrame->IsChild(m_pActiveDialog))
			UIMainPdaFrame->AttachChild(m_pActiveDialog);
		m_pActiveDialog->Show(true);
		m_sActiveSection = section;
	}
	else
	{
		m_sActiveSection = "";
	}

	SetActiveCaption();
}

void CUIPdaWnd::SetActiveCaption()
{
	TABS_VECTOR* btn_vec = UITabControl->GetButtonsVector();
	TABS_VECTOR::iterator it_b = btn_vec->begin();
	TABS_VECTOR::iterator it_e = btn_vec->end();
	for (; it_b != it_e; ++it_b)
	{
		if ((*it_b)->m_btn_id == m_sActiveSection)
		{
			LPCSTR cur = (*it_b)->TextItemControl()->GetText();
			string256 buf;
			strconcat(sizeof(buf), buf, m_caption_const.c_str(), cur);
			SetCaption(buf);
			UITabControl->Show(true);
			m_clock->Show(true);
			m_caption->Show(true);
			m_battery_bar->Show(true);
			return;
		}
	}

	UITabControl->Show(false);
	//m_clock->Show(false);
	m_caption->Show(false);
	//m_battery_bar->Show(false);
}

#include "UICursor.h"

void CUIPdaWnd::ResetCursor()
{
	if (!last_cursor_pos.similar({ 0.f, 0.f }))
		GetUICursor().SetUICursorPosition(last_cursor_pos);
}

void CUIPdaWnd::Show_SecondTaskWnd(bool status)
{
	if (status)
	{
		SetActiveSubdialog("eptTasks");
	}
	pUITaskWnd->Show_TaskListWnd(status);
}

void CUIPdaWnd::Show_MapLegendWnd(bool status)
{
	if (status)
	{
		SetActiveSubdialog("eptTasks");
	}
	pUITaskWnd->ShowMapLegend(status);
}

void CUIPdaWnd::Draw()
{
	if (Device.dwFrame == dwPDAFrame)
		return;

	dwPDAFrame = Device.dwFrame;

	inherited::Draw();
	//.	DrawUpdatedSections();
	DrawHint();
	UINoice->Draw(); // over all
}

void CUIPdaWnd::DrawHint()
{
	if (m_sActiveSection == "eptTasks")
	{
		pUITaskWnd->DrawHint();
	}
	else if (m_sActiveSection == "eptRanking")
	{
		if (!m_bGamepadHintsHidden)
			pUIRankingWnd->DrawHint();
	}
	else if (m_sActiveSection == "eptLogs")
	{
	}
	m_hint_wnd->Draw();
}

void CUIPdaWnd::UpdatePda()
{
	pUILogsWnd->UpdateNews();

	if (m_sActiveSection == "eptTasks")
	{
		pUITaskWnd->ReloadTaskInfo();
	}
}

void CUIPdaWnd::UpdateRankingWnd()
{
	pUIRankingWnd->Update();
}

void CUIPdaWnd::Reset()
{
	inherited::ResetAll();

	if (pUITaskWnd) pUITaskWnd->ResetAll();
	if (pUIRankingWnd) pUIRankingWnd->ResetAll();
	if (pUILogsWnd) pUILogsWnd->ResetAll();
}

void CUIPdaWnd::SetCaption(LPCSTR text)
{
	m_caption->SetText(text);
}

void RearrangeTabButtons(CUITabControl* pTab)
{
	TABS_VECTOR* btn_vec = pTab->GetButtonsVector();
	TABS_VECTOR::iterator it = btn_vec->begin();
	TABS_VECTOR::iterator it_e = btn_vec->end();

	Fvector2 pos;
	pos.set((*it)->GetWndPos());
	float size_x;

	for (; it != it_e; ++it)
	{
		(*it)->SetWndPos(pos);
		(*it)->AdjustWidthToText();
		size_x = (*it)->GetWndSize().x + 30.0f;
		(*it)->SetWidth(size_x);
		pos.x += size_x - 6.0f;
	}

	pTab->SetWidth(pos.x + 5.0f);
	pos.x = pTab->GetWndPos().x - pos.x;
	pos.y = pTab->GetWndPos().y;
	pTab->SetWndPos(pos);
}

void CUIPdaWnd::Enable(bool status)
{
	if (status)
	{
		ResetCursor();
		if (IsShown() && CurrentGameUI())
		{
			if (CurrentGameUI()->TopInputReceiver() != this)
			{
				CurrentGameUI()->SetMainInputReceiver(this, true);
				CurrentGameUI()->SetMainInputReceiver(this, false);
			}
			GetUICursor().Show();
		}
	}
	else
	{
		g_player_hud->reset_thumb(false);
		ResetJoystick(false);
		bButtonL = false;
		bButtonR = false;
	}

	inherited::Enable(status);
}

bool CUIPdaWnd::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
	if (IsShown() && (keyboard_action == WINDOW_KEY_PRESSED || keyboard_action == WINDOW_KEY_RELEASED))
	{
		if (!psActorFlags.test(AF_3D_PDA) && keyboard_action == WINDOW_KEY_PRESSED)
		{
			EGameActions action = get_binded_action(dik);

			if (action == kQUIT || action == kINVENTORY || action == kACTIVE_JOBS)
			{
				HideDialog();
				return  action == kQUIT;
			}

			return inherited::OnKeyboardAction(dik, keyboard_action);
		}

		CPda* pda = smart_cast<CPda*>(Actor()->inventory().ActiveItem());

		if (!pda)
			return inherited::OnKeyboardAction(dik, keyboard_action);

		return pda->Action(get_binded_action(dik), keyboard_action == WINDOW_KEY_PRESSED ? CMD_START : CMD_STOP);
	}

	return false;
}

#include "UIMapWnd.h"
#include "UIPropertiesBox.h"

static void PushPdaShownRect(CUIWindow* w, xr_vector<Frect>& outRects)
{
	if (!w || !w->IsShown())
		return;
	Frect r;
	w->GetAbsoluteRect(r);
	if (r.width() > 2.0f && r.height() > 2.0f)
		outRects.push_back(r);
}

bool CUIPdaWnd::CycleActiveTab(int delta)
{
	if (delta == 0 || !UITabControl || !UITabControl->IsShown())
		return false;

	TABS_VECTOR* tabs = UITabControl->GetButtonsVector();
	if (!tabs || tabs->empty())
		return false;

	const int count = static_cast<int>(tabs->size());
	const shared_str curId = UITabControl->GetActiveId();
	int curIdx = 0;
	for (int i = 0; i < count; ++i)
	{
		if ((*tabs)[i] && (*tabs)[i]->m_btn_id == curId)
		{
			curIdx = i;
			break;
		}
	}

	const int nextIdx = ((curIdx + delta) % count + count) % count;
	CUITabButton* nextBtn = (*tabs)[nextIdx];
	if (!nextBtn)
		return false;

	UITabControl->SetActiveTab(nextBtn->m_btn_id);
	return true;
}

bool CUIPdaWnd::CloseTopOverlay()
{
	if (ai().get_alife())
	{
		::luabind::functor<bool> fnPawClose;
		if (ai().script_engine().functor("gp_pda.close_paw_modal_if_open", fnPawClose))
		{
			try
			{
				if (fnPawClose())
					return true;
			}
			catch (...)
			{
			}
		}
	}

	if (pUITaskWnd && pUITaskWnd->IsShown() && pUITaskWnd->CloseTopTaskOverlay())
		return true;

	if (ai().get_alife())
	{
		::luabind::functor<bool> fnNpcBack;
		if (ai().script_engine().functor("gp_pda.handle_npc_pda_back", fnNpcBack))
		{
			try
			{
				if (fnNpcBack())
					return true;
			}
			catch (...)
			{
			}
		}
	}

	return false;
}

void CUIPdaWnd::ClosePdaFromGamepad()
{
	if (psActorFlags.test(AF_3D_PDA) && Actor())
	{
		CPda* pda = smart_cast<CPda*>(Actor()->inventory().ActiveItem());
		if (pda)
		{
			pda->Action(kQUIT, CMD_START);
			return;
		}
	}
	HideDialog();
}

void CUIPdaWnd::GatherGamepadTargets(xr_vector<Frect>& outRects) const
{
	// If map context menu (CUIPropertiesBox) is open on eptTasks, confine targets to its items
	if (pUITaskWnd && pUITaskWnd->IsShown() && pUITaskWnd->GetMapWnd() &&
		pUITaskWnd->GetMapWnd()->GetPropertiesBox() && pUITaskWnd->GetMapWnd()->GetPropertiesBox()->IsShown())
	{
		pUITaskWnd->GatherGamepadTaskTargets(outRects);
		return;
	}

	if (UITabControl && UITabControl->IsShown())
	{
		TABS_VECTOR* tabs = UITabControl->GetButtonsVector();
		if (tabs)
		{
			for (auto it = tabs->begin(); it != tabs->end(); ++it)
				PushPdaShownRect(*it, outRects);
		}
	}
	PushPdaShownRect(m_btn_close, outRects);

	if (pUITaskWnd && pUITaskWnd->IsShown())
	{
		pUITaskWnd->GatherGamepadTaskTargets(outRects);
	}
	else if (pUILogsWnd && pUILogsWnd->IsShown())
	{
		PushPdaShownRect(pUILogsWnd->GetFilterNews(), outRects);
		PushPdaShownRect(pUILogsWnd->GetFilterTalk(), outRects);
		PushPdaShownRect(pUILogsWnd->GetPrevPeriodBtn(), outRects);
		PushPdaShownRect(pUILogsWnd->GetNextPeriodBtn(), outRects);
	}
}

bool CUIPdaWnd::HandleGamepadRightStick(float curX, float curY, int wheelDir)
{
	if (wheelDir == 0)
		return false;

	if (pUITaskWnd && pUITaskWnd->IsShown())
		return pUITaskWnd->HandleGamepadRightStick(curX, curY, wheelDir);

	const EUIMessages msg = (wheelDir > 0) ? WINDOW_MOUSE_WHEEL_UP : WINDOW_MOUSE_WHEEL_DOWN;

	if (pUILogsWnd && pUILogsWnd->IsShown())
	{
		CUIScrollView* sv = pUILogsWnd->GetLogsList();
		if (sv && sv->IsShown())
		{
			Frect r;
			sv->GetAbsoluteRect(r);
			sv->OnMouseAction(curX - r.x1, curY - r.y1, msg);
			return true;
		}
	}

	if (pUIRankingWnd && pUIRankingWnd->IsShown())
	{
		CUIScrollView* svAch = pUIRankingWnd->GetAchievementsList();
		CUIScrollView* svCoc = pUIRankingWnd->GetCocRankingList();
		if (svAch && svAch->IsShown())
		{
			Frect r;
			svAch->GetAbsoluteRect(r);
			if (r.in(curX, curY))
			{
				svAch->OnMouseAction(curX - r.x1, curY - r.y1, msg);
				return true;
			}
		}
		if (svCoc && svCoc->IsShown())
		{
			Frect r;
			svCoc->GetAbsoluteRect(r);
			svCoc->OnMouseAction(curX - r.x1, curY - r.y1, msg);
			return true;
		}
	}

	bool handledByLua = false;
	if (ai().get_alife())
	{
		::luabind::functor<bool> fnScroll;
		if (ai().script_engine().functor("gp_pda.on_pda_right_stick_scroll", fnScroll))
		{
			try
			{
				handledByLua = fnScroll(curX, curY, wheelDir);
			}
			catch (...)
			{
			}
		}
	}

	if (!handledByLua && m_pActiveDialog && m_pActiveDialog->IsShown())
	{
		Frect r;
		m_pActiveDialog->GetAbsoluteRect(r);
		m_pActiveDialog->OnMouseAction(curX - r.x1, curY - r.y1, msg);
		return true;
	}

	return handledByLua;
}

void CUIPdaWnd::StepGamepadMapPan(float stickX, float stickY, bool bModifierHeld, float curX, float curY, u32 dtMs)
{
	if (pUITaskWnd && pUITaskWnd->IsShown())
		pUITaskWnd->StepGamepadMapPan(stickX, stickY, bModifierHeld, curX, curY, dtMs);
}

bool CUIPdaWnd::HandleGamepadButtonX(bool bModifierHeld, Fvector2* outSnapPos)
{
	if (pUITaskWnd && pUITaskWnd->IsShown() && pUITaskWnd->GetMapWnd() && pUITaskWnd->GetMapWnd()->IsShown())
	{
		return pUITaskWnd->GetMapWnd()->HandleGamepadContextMenuOrCenter(bModifierHeld, outSnapPos);
	}
	return false;
}

bool CUIPdaWnd::HandleGamepadButtonY()
{
	if (!pUITaskWnd || !pUITaskWnd->IsShown() || !pUITaskWnd->GetMapWnd() || !pUITaskWnd->GetMapWnd()->IsShown())
		return false;

	u16 objId = u16(65535);
	Fvector worldPos = { 0.0f, 0.0f, 0.0f };
	u32 lvid = u32(-1);
	u32 gvid = u32(-1);
	shared_str levelName;

	if (!pUITaskWnd->GetMapWnd()->GetGamepadQuickPawTarget(objId, worldPos, lvid, gvid, levelName))
		return false;

	if (ai().get_alife())
	{
		::luabind::functor<bool> fnPaw;
		if (ai().script_engine().functor("gp_pda.on_map_quick_paw", fnPaw))
		{
			try
			{
				return fnPaw(objId, worldPos.x, worldPos.y, worldPos.z, lvid, gvid, levelName.c_str() ? levelName.c_str() : "");
			}
			catch (...)
			{
			}
		}
	}
	return false;
}

bool CUIPdaWnd::HandleGamepadButtonView(Fvector2* outSnapPos)
{
	if (pUITaskWnd && pUITaskWnd->IsShown())
	{
		return pUITaskWnd->ToggleTaskListWndGamepad(outSnapPos);
	}
	return false;
}

#include "UIScrollView.h"
#include "UISecondTaskWnd.h"
#include "UIMapLegend.h"

static bool ScrollPdaScrollViewAnalog(CUIScrollView* sv, float dy_ui)
{
	if (!sv || !sv->IsShown())
		return false;
	const int mn = sv->GetMinScrollPos();
	const int mx = sv->GetMaxScrollPos();
	if (mx <= mn)
		return false;
	static CUIScrollView* s_lastSv = nullptr;
	static float s_acc = 0.0f;
	const int cur = sv->GetCurrentScrollPos();
	if (s_lastSv != sv || iFloor(s_acc + 0.5f) != cur)
	{
		s_lastSv = sv;
		s_acc = static_cast<float>(cur);
	}
	s_acc = clampr(s_acc + dy_ui, static_cast<float>(mn), static_cast<float>(mx));
	const int ip = iFloor(s_acc + 0.5f);
	if (ip != cur)
		sv->SetScrollPos(ip);
	return true;
}

bool CUIPdaWnd::GpScrollAnalog(float dy_ui)
{
	if (_abs(dy_ui) <= EPS_S)
		return false;

	if (pUITaskWnd && pUITaskWnd->IsShown())
	{
		if (pUITaskWnd->GetMapLegendWnd() && pUITaskWnd->GetMapLegendWnd()->IsShown())
			return ScrollPdaScrollViewAnalog(pUITaskWnd->GetMapLegendWnd()->GetList(), dy_ui);
		if (pUITaskWnd->GetTaskListWnd() && pUITaskWnd->GetTaskListWnd()->IsShown())
			return ScrollPdaScrollViewAnalog(pUITaskWnd->GetTaskListWnd()->GetTaskList(), dy_ui);
		return false;
	}

	if (pUILogsWnd && pUILogsWnd->IsShown())
	{
		return ScrollPdaScrollViewAnalog(pUILogsWnd->GetLogsList(), dy_ui);
	}

	if (pUIRankingWnd && pUIRankingWnd->IsShown())
	{
		const Fvector2 cur = GetUICursor().GetCursorPosition();
		CUIScrollView* svAch = pUIRankingWnd->GetAchievementsList();
		CUIScrollView* svRank = pUIRankingWnd->GetCocRankingList();
		if (svAch && svAch->IsShown())
		{
			Frect ar;
			svAch->GetAbsoluteRect(ar);
			if (ar.in(cur.x, cur.y))
				return ScrollPdaScrollViewAnalog(svAch, dy_ui);
		}
		if (svRank && svRank->IsShown())
		{
			Frect rr;
			svRank->GetAbsoluteRect(rr);
			if (rr.in(cur.x, cur.y))
				return ScrollPdaScrollViewAnalog(svRank, dy_ui);
		}
		if (svRank && svRank->IsShown())
			return ScrollPdaScrollViewAnalog(svRank, dy_ui);
		if (svAch && svAch->IsShown())
			return ScrollPdaScrollViewAnalog(svAch, dy_ui);
	}

	return false;
}
