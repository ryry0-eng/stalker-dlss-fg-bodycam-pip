#include "stdafx.h"
#include "UITalkDialogWnd.h"
#include "../GpHintXray.h"
#include "../string_table.h"

#include "xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIScrollView.h"
#include "UI3tButton.h"
#include "UITalkWnd.h"
#include "UIInventoryUtilities.h"
#include "UIBtnHint.h"
#include "../ui_base.h"

#include "../game_news.h"
#include "../level.h"
#include "../actor.h"
#include "../alife_registry_wrappers.h"
#include "dinput.h"
#include "../../xrEngine/GamepadGlyphs.h"
#include "../string_table.h"

#define				TALK_XML				"talk.xml"

CUITalkDialogWnd::CUITalkDialogWnd()
	: m_pNameTextFont(NULL)
{
	m_ClickedQuestionID = "";
	mechanic_mode = false;
}

CUITalkDialogWnd::~CUITalkDialogWnd()
{
	xr_delete(m_uiXml);
}

void CUITalkDialogWnd::InitTalkDialogWnd()
{
	m_uiXml = xr_new<CUIXml>();
	m_uiXml->Load(CONFIG_PATH, UI_PATH, TALK_XML);
	CUIXmlInit ml_init;

	CUIXmlInit::InitWindow(*m_uiXml, "main", 0, this);

	//	CUIXmlInit::InitStatic		(*m_uiXml, "right_character_icon", 0, &UIOurIcon);

	//	CUIXmlInit::InitStatic		(*m_uiXml, "left_character_icon", 0, &UIOthersIcon);

	//	UIOurIcon.AttachChild		(&UICharacterInfoLeft);
	//	UICharacterInfoLeft.InitCharacterInfo(Fvector2().set(0,0), UIOurIcon.GetWndSize(), "talk_character.xml");

	//	UIOthersIcon.AttachChild	(&UICharacterInfoRight);
	//	UICharacterInfoRight.InitCharacterInfo(Fvector2().set(0,0), UIOthersIcon.GetWndSize(), "talk_character.xml");

	//	AttachChild					(&UIOurIcon);
	//	AttachChild					(&UIOthersIcon);

	// Фрейм с нащими фразами
	//	AttachChild					(&UIDialogFrameBottom);
	//	CUIXmlInit::InitStatic		(*m_uiXml, "frame_bottom", 0, &UIDialogFrameBottom);

	//основной фрейм диалога
	//	AttachChild					(&UIDialogFrameTop);
	//	CUIXmlInit::InitStatic		(*m_uiXml, "frame_top", 0, &UIDialogFrameTop);


	//Ответы
	UIAnswersList = xr_new<CUIScrollView>();
	UIAnswersList->SetAutoDelete(true);
	//	UIDialogFrameTop.AttachChild(UIAnswersList);
	AttachChild(UIAnswersList);
	CUIXmlInit::InitScrollView(*m_uiXml, "answers_list", 0, UIAnswersList);
	UIAnswersList->SetWindowName("---UIAnswersList");

	//Вопросы
	UIQuestionsList = xr_new<CUIScrollView>();
	UIQuestionsList->SetAutoDelete(true);
	//	UIDialogFrameBottom.AttachChild(UIQuestionsList);
	AttachChild(UIQuestionsList);
	CUIXmlInit::InitScrollView(*m_uiXml, "questions_list", 0, UIQuestionsList);
	UIQuestionsList->SetWindowName("---UIQuestionsList");


	//кнопка перехода в режим торговли
	AttachChild(&UIToTradeButton);
	CUIXmlInit::Init3tButton(*m_uiXml, "button", 0, &UIToTradeButton);

	//AttachChild					(&UIToExitButton);
	//CUIXmlInit::Init3tButton	(*m_uiXml, "button_exit", 0, &UIToExitButton);

	//m_btn_pos[0]				= UIToTradeButton.GetWndPos();
	//m_btn_pos[1]				= UIToExitButton.GetWndPos();
	//m_btn_pos[2].x				= (m_btn_pos[0].x+m_btn_pos[1].x)/2.0f;
	//m_btn_pos[2].y				= m_btn_pos[0].y;
	// шрифт для индикации имени персонажа в окне разговора
	CUIXmlInit::InitFont(*m_uiXml, "font", 0, m_iNameTextColor, m_pNameTextFont);

	CGameFont* pFont = NULL;
	CUIXmlInit::InitFont(*m_uiXml, "font", 1, m_uOurReplicsColor, pFont);


	SetWindowName("----CUITalkDialogWnd");

	Register(&UIToTradeButton);
	AddCallbackStr("question_item", LIST_ITEM_CLICKED,
	               CUIWndCallback::void_function(this, &CUITalkDialogWnd::OnQuestionClicked));
	AddCallback(&UIToTradeButton, BUTTON_CLICKED,
	            CUIWndCallback::void_function(this, &CUITalkDialogWnd::OnTradeClicked));
	GpInitHints();
	//	AddCallback					(&UIToExitButton,BUTTON_CLICKED,CUIWndCallback::void_function(this, &CUITalkDialogWnd::OnExitClicked));
}


void CUITalkDialogWnd::Show()
{
	InventoryUtilities::SendInfoToActor("ui_talk_show");
	InventoryUtilities::SendInfoToLuaScripts("ui_talk_show");
	inherited::Show(true);
	inherited::Enable(true);

	ResetAll();
}

void CUITalkDialogWnd::Hide()
{
	InventoryUtilities::SendInfoToActor("ui_talk_hide");
	InventoryUtilities::SendInfoToLuaScripts("ui_talk_hide");
	inherited::Show(false);
	inherited::Enable(false);
	g_btnHint->Discard();
}

void CUITalkDialogWnd::OnQuestionClicked(CUIWindow* w, void*)
{
	m_ClickedQuestionID = ((CUIQuestionItem*)w)->m_s_value;
	GetMessageTarget()->SendMessage(this, TALK_DIALOG_QUESTION_CLICKED);
}

void CUITalkDialogWnd::OnExitClicked(CUIWindow* w, void*)
{
	m_pParent->StopTalk();
}

void CUITalkDialogWnd::OnTradeClicked(CUIWindow* w, void*)
{
	if (mechanic_mode)
	{
		GetTop()->SendMessage(this, TALK_DIALOG_UPGRADE_BUTTON_CLICKED);
	}
	else
	{
		GetTop()->SendMessage(this, TALK_DIALOG_TRADE_BUTTON_CLICKED);
	}
}

void CUITalkDialogWnd::OnUpgradeClicked(CUIWindow* w, void*)
{
	GetTop()->SendMessage(this, TALK_DIALOG_UPGRADE_BUTTON_CLICKED);
}

void CUITalkDialogWnd::SetTradeMode()
{
	OnTradeClicked(&UIToTradeButton, 0);
}

//пересылаем сообщение родительскому окну для обработки
//и фильтруем если оно пришло от нашего дочернего окна
void CUITalkDialogWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	CUIWndCallback::OnEvent(pWnd, msg, pData);
}

// ---- INV-REWORK-21: выбор реплик геймпадом ----
namespace
{
	const float kGpShiftPx = 4.0f;  // на сколько выбранная реплика уезжает вправо (UI-пиксели 1024x768)
}

bool CUITalkDialogWnd::GpHasQuestions()
{
	return UIQuestionsList && UIQuestionsList->GetSize() > 0;
}

void CUITalkDialogWnd::GpStep(int dir)
{
	if (!UIQuestionsList)
		return;
	const int n = (int)UIQuestionsList->GetSize();
	if (n <= 0)
	{
		m_gpSel = 0;
		return;
	}
	m_gpSel = _max(0, _min(n - 1, m_gpSel + dir));
}

// INV-REWORK-22 (Миша 2026-11-12: «в диалоге с НПС нет подсказок»): строка подсказок — детьми этого окна, чтобы
// рисоваться поверх него (Lua-окно в HUD рисуется под TalkMenu). Значки — графитовые круглые из атласа
// ui_gp_xbox_graphite_p12 (как в нижней полосе инвентаря), квадратные в пикселях экрана. Подписи — cp1251 байтами \xNN
// (INV-REWORK-24: литералы в UTF-8 исходнике без /utf-8 давали крякозябры; движок ждёт cp1251).
namespace
{
	struct SGpHint { LPCSTR tex; LPCSTR id; LPCSTR fallback; };
	const SGpHint kGpHints[4] = {
		{ "ui_gp_xbox_south", "ui_gp_eng_talk_select", "\xC2\xFB\xE1\xF0\xE0\xF2\xFC" },     // A Выбрать
		{ "ui_gp_xbox_east",  "ui_gp_eng_talk_exit",   "\xC2\xFB\xE9\xF2\xE8" },               // B Выйти
		{ "ui_gp_xbox_west",  "ui_gp_eng_talk_trade",  "\xD2\xEE\xF0\xE3\xEE\xE2\xE0\xF2\xFC" }, // X Торговать
		{ "ui_gp_xbox_rs",    "ui_gp_eng_talk_scroll", "\xCB\xE8\xF1\xF2\xE0\xF2\xFC" },     // RS Листать
	};

	LPCSTR ResolveTalkHintText(const SGpHint& h)
	{
		if (CStringTable::getPData())
		{
			const shared_str tr = CStringTable().translate(h.id);
			if (tr.size() && 0 != xr_strcmp(tr.c_str(), h.id))
				return tr.c_str();
		}
		return h.fallback;
	}
	// INV-REWORK-23 (Миша 2026-11-12, макет gp-screens): строка — в нижней светлой кромке окна диалога, справа.
	// Кромка на кадре 2560x1080: y 1019..1049 px (середина 1034 px = 735,3 ед. UI), внутренний правый край рамки
	// x 2520 px = 1008 ед.; строка кончается в 30 px от него. Значок 22 px, значок->подпись 11 px, шаг 30 px.
	const float kGpHintFrameR = 1008.0f; // inner right edge of the dialog frame (UI units 1024x768)
	const float kGpHintRightPx = 30.0f;  // row ends this many screen pixels (at 1080p) left of it
	const float kGpHintCY = 735.3f;      // midline of the light bottom edge (UI units)
	const float kGpHintIconPx = 22.0f;   // icon side in pixels at screen height 1080
	const float kGpHintTextPx = 11.0f, kGpHintGapPx = 30.0f;
	// W4-ICONS2-FIELD: Dot Marks (ui_gp_dm.xml, PAD=3 px around 22 px circle => 28 px sprite) & dynamic X caption
	const float kGpHintPadPx = 3.0f;
	const float kGpHintTexPx = kGpHintIconPx + 2.0f * kGpHintPadPx;

	LPCSTR GpResolveHintTex(int idx)
	{
		CGamepadGlyphManager& gm = CGamepadGlyphManager::Instance();
		LPCSTR tex = nullptr;
		switch (idx)
		{
		case 0: tex = gm.GetButtonTextureId(gpbind::ButtonId::South); break;
		case 1: tex = gm.GetButtonTextureId(gpbind::ButtonId::East);  break;
		case 2: tex = gm.GetButtonTextureId(gpbind::ButtonId::West);  break;
		case 3: tex = gm.GetEngineActionTextureId("left");            break;
		default: break;
		}
		return (tex && tex[0]) ? tex : kGpHints[idx].tex;
	}
}

void CUITalkDialogWnd::GpInitHints()
{
	for (int i = 0; i < 4; ++i)
	{
		m_gpHintIcon[i] = xr_new<CUIStatic>();
		m_gpHintIcon[i]->SetAutoDelete(true);
		m_gpHintIcon[i]->Enable(false);
		m_gpHintIcon[i]->SetStretchTexture(true);
		m_gpHintIcon[i]->InitTexture(GpResolveHintTex(i));
		AttachChild(m_gpHintIcon[i]);
		m_gpHintText[i] = xr_new<CUITextWnd>();
		m_gpHintText[i]->SetAutoDelete(true);
		m_gpHintText[i]->SetFont(UI().Font().pFontLetterica18Russian);
		m_gpHintText[i]->SetTextColor(color_argb(235, 220, 220, 220));
		m_gpHintText[i]->SetVTextAlignment(valCenter);
		m_gpHintText[i]->SetText(ResolveTalkHintText(kGpHints[i]));
		AttachChild(m_gpHintText[i]);
		m_gpHintIcon[i]->Show(false);
		m_gpHintText[i]->Show(false);
	}
}

void CUITalkDialogWnd::GpShowHints(bool on)
{
	static u32 s_lastGlyphRev = 0;
	CGamepadGlyphManager& gm = CGamepadGlyphManager::Instance();
	if (!on || !gm.AreHintsVisible(true))
	{
		if (m_gpHintsShown)
		{
			for (int i = 0; i < 4; ++i)
			{
				m_gpHintIcon[i]->Show(false);
				m_gpHintText[i]->Show(false);
			}
			m_gpHintsShown = false;
		}
		if (!on)
			s_lastGlyphRev = 0;
		return;
	}
	const u32 rev = gm.GetPromptRevision();
	if (!m_gpHintsShown || s_lastGlyphRev != rev)
	{
		for (int i = 0; i < 4; ++i)
			m_gpHintIcon[i]->InitTexture(GpResolveHintTex(i));
		s_lastGlyphRev = rev;
	}
	if (Device.dwWidth == 0 || Device.dwHeight == 0)
		return;
	const float W = (float)Device.dwWidth, H = (float)Device.dwHeight;
	const float ux = H / 1080.0f * 1024.0f / W; // one 1080p pixel in horizontal UI units
	const float scale = clampr(gm.GetIconScaleFactor(), 0.5f, 1.5f);
	// INV-REWORK-24: Dot Marks 28 px = 22 visible + 2*3 PAD at scale 1.0, whole screen pixels
	const float iconPx = _max(8.0f, floorf(kGpHintTexPx * scale * H / 1080.0f + 0.5f));
	const float pad1080 = kGpHintPadPx * scale;
	const float iconW = iconPx * 1024.0f / W;
	const float iconH = iconPx * 768.0f / H;
	const float tgap = _max(2.0f, kGpHintTextPx - pad1080) * ux;
	const float gap = _max(4.0f, kGpHintGapPx - pad1080) * ux;
	const float rightGap = _max(2.0f, kGpHintRightPx - pad1080) * ux;
	const float y = floorf(kGpHintCY * H / 768.0f - iconPx * 0.5f + 0.5f) * 768.0f / H;
	// label box: its lower-case middle (x-height, Letterica18) on the icon middle, not the box middle
	const float ty = y - GpHintXray::XMidBelowCellCenterUi(UI().Font().pFontLetterica18Russian);
	const bool trade = UIToTradeButton.IsShown();
	float tw[4] = {};
	float total = 0.0f;
	int shown = 0;
	for (int i = 0; i < 4; ++i)
	{
		if (i == 2 && !trade)
			continue;
		// Refresh localized text before measuring width so live reload_language() updates layout
		m_gpHintText[i]->SetText(ResolveTalkHintText(kGpHints[i]));
		// real text width from the font, so the gaps are even
		m_gpHintText[i]->AdjustWidthToText();
		tw[i] = m_gpHintText[i]->GetWidth();
		total += iconW + tgap + tw[i];
		if (shown++)
			total += gap;
	}
	if (trade)
	{
		LPCSTR xLabel = UIToTradeButton.TextItemControl()->GetText();
		if ((!xLabel || !xLabel[0]) && CStringTable::getPData())
			xLabel = CStringTable().translate(mechanic_mode ? "ui_st_upgrade" : "ui_st_trade").c_str();
		if (!xLabel || !xLabel[0])
		{
			static const SGpHint kRepairHint = { "ui_gp_xbox_west", "ui_gp_eng_talk_repair", "\xD0\xE5\xEC\xEE\xED\xF2" };
			xLabel = mechanic_mode ? ResolveTalkHintText(kRepairHint) : ResolveTalkHintText(kGpHints[2]);
		}
		if (0 != xr_strcmp(m_gpHintText[2]->GetText(), xLabel))
		{
			const float oldW = tw[2];
			m_gpHintText[2]->SetText(xLabel);
			m_gpHintText[2]->AdjustWidthToText();
			tw[2] = m_gpHintText[2]->GetWidth();
			total += (tw[2] - oldW);
		}
	}
	float x = kGpHintFrameR - rightGap - total;
	for (int i = 0; i < 4; ++i)
	{
		const bool vis = !(i == 2 && !trade);
		m_gpHintIcon[i]->Show(vis);
		m_gpHintText[i]->Show(vis);
		if (!vis)
			continue;
		// icon and text share one midline: both boxes are iconH tall around kGpHintCY, text centred vertically
		m_gpHintIcon[i]->SetWndPos(Fvector2().set(x, y));
		m_gpHintIcon[i]->SetWndSize(Fvector2().set(iconW, iconH));
		m_gpHintText[i]->SetWndPos(Fvector2().set(x + iconW + tgap, ty));
		m_gpHintText[i]->SetWndSize(Fvector2().set(tw[i] + 2.0f, iconH));
		x += iconW + tgap + tw[i] + gap;
	}
	m_gpHintsShown = true;
}

void CUITalkDialogWnd::GpReset()
{
	GpShowHints(false);
	m_gpAnsAccum = 0.0f;
	if (!UIQuestionsList || m_gpBaseX < 0.0f)
		return;
	const u32 n = UIQuestionsList->GetSize();
	for (u32 i = 0; i < n; ++i)
	{
		CUIQuestionItem* it = smart_cast<CUIQuestionItem*>(UIQuestionsList->GetItem(i));
		if (!it || !it->m_text)
			continue;
		Fvector2 p = it->m_text->GetWndPos();
		p.x = m_gpBaseX;
		it->m_text->SetWndPos(p);
		it->m_text->SetTextBlend(-1.0f);
	}
}

void CUITalkDialogWnd::GpGetListRect(Frect& out)
{
	out.set(0, 0, 0, 0);
	if (UIAnswersList)
		UIAnswersList->GetAbsoluteRect(out);
}

bool CUITalkDialogWnd::GpSelectedCenter(Fvector2& out)
{
	if (!UIQuestionsList)
		return false;
	CUIQuestionItem* it = smart_cast<CUIQuestionItem*>(UIQuestionsList->GetItem((u32)m_gpSel));
	if (!it || !it->m_text)
		return false;
	Frect r;
	it->m_text->GetAbsoluteRect(r);
	out.set((r.x1 + r.x2) * 0.5f, (r.y1 + r.y2) * 0.5f);
	return true;
}

void CUITalkDialogWnd::GpUpdate(float dtMs, float rsY)
{
	if (!UIQuestionsList || !UIAnswersList)
		return;
	GpShowHints(true);
	if (m_gpBaseX < 0.0f)
		m_gpBaseX = m_uiXml ? m_uiXml->ReadAttribFlt("question_item:content_text", 0, "x", 15.0f) : 15.0f;
	const int n = (int)UIQuestionsList->GetSize();
	if (m_gpSel >= n)
		m_gpSel = _max(0, n - 1);

	const float k = 1.0f - expf(-_max(dtMs, 1.0f) / 55.0f); // плавное скольжение (~0,15 с)
	for (int i = 0; i < n; ++i)
	{
		CUIQuestionItem* it = smart_cast<CUIQuestionItem*>(UIQuestionsList->GetItem((u32)i));
		if (!it || !it->m_text)
			continue;
		Fvector2 p = it->m_text->GetWndPos();
		const float target = m_gpBaseX + ((i == m_gpSel) ? kGpShiftPx : 0.0f);
		p.x += (target - p.x) * k;
		if (_abs(target - p.x) < 0.25f)
			p.x = target;
		it->m_text->SetWndPos(p);
		it->m_text->SetTextBlend(clampr((p.x - m_gpBaseX) / kGpShiftPx, 0.0f, 1.0f)); // цвет плывёт вместе со сдвигом
	}

	// выбранная реплика всегда в видимой части списка
	if (n > 0)
	{
		CUIWindow* sel = UIQuestionsList->GetItem((u32)m_gpSel);
		if (sel)
		{
			const float y1 = sel->GetWndPos().y;
			const float y2 = y1 + sel->GetHeight();
			const float h = UIQuestionsList->GetHeight();
			const int cur = UIQuestionsList->GetCurrentScrollPos();
			if (y1 < (float)cur)
				UIQuestionsList->SetScrollPos(iFloor(y1));
			else if (y2 > (float)cur + h)
				UIQuestionsList->SetScrollPos(iCeil(y2 - h));
		}
	}

	// правый стик: история листается попиксельно (стик вниз = вниз), ~450 px/с при полном отклонении
	if (_abs(rsY) >= 0.08f)
	{
		m_gpAnsAccum += rsY * 0.45f * dtMs;
		const int whole = (int)m_gpAnsAccum;
		if (whole != 0)
		{
			m_gpAnsAccum -= (float)whole;
			UIAnswersList->SetScrollPos(UIAnswersList->GetCurrentScrollPos() + whole);
		}
	}
	else
		m_gpAnsAccum = 0.0f;
}

void CUITalkDialogWnd::ClearAll()
{
	UIAnswersList->Clear();
	ClearQuestions();
}

void CUITalkDialogWnd::ClearQuestions()
{
	m_gpSel = 0;
	UIQuestionsList->Clear();
}


void CUITalkDialogWnd::AddQuestion(LPCSTR str, LPCSTR value, int number, bool b_finalizer)
{
	CUIQuestionItem* itm = xr_new<CUIQuestionItem>(m_uiXml, "question_item");
	itm->Init(value, str);
	++number; //zero-based index
	if (number <= 10)
	{
		string16 buff;
		xr_sprintf(buff, "%d.", (number == 10) ? 0 : number);
		itm->m_num_text->SetText(buff);
		itm->m_text->SetAccelerator(DIK_ESCAPE + number, 0);
	}
	if (b_finalizer)
	{
		itm->m_text->SetAccelerator(kQUIT, 2);
		itm->m_text->SetAccelerator(kUSE, 3);
	}

	itm->SetWindowName("question_item");
	UIQuestionsList->AddWindow(itm, true);
	Register(itm);
}


void CUITalkDialogWnd::AddAnswer(LPCSTR SpeakerName, LPCSTR str, bool bActor)
{
	CUIAnswerItem* itm = xr_new<CUIAnswerItem>(m_uiXml, bActor ? "actor_answer_item" : "other_answer_item");
	itm->Init(str, SpeakerName);
	UIAnswersList->AddWindow(itm, true);
	UIAnswersList->ScrollToEnd();

	GAME_NEWS_DATA news_data;
	news_data.news_caption = SpeakerName;

	xr_string res;
	res = "%c[250,255,232,208]";
	res += str;
	news_data.news_text = res.c_str();

	news_data.m_type = GAME_NEWS_DATA::eTalk;
	CUICharacterInfo& ci = bActor ? UICharacterInfoLeft : UICharacterInfoRight;

	news_data.texture_name = ci.IconName();
	news_data.receive_time = Level().GetGameTime();

	Actor()->game_news_registry->registry().objects().push_back(news_data);
}

void CUITalkDialogWnd::AddIconedAnswer(LPCSTR caption, LPCSTR text, LPCSTR texture_name, LPCSTR templ_name)
{
	CUIAnswerItemIconed* itm = xr_new<CUIAnswerItemIconed>(m_uiXml, templ_name);
	itm->Init(text, caption, texture_name);
	UIAnswersList->AddWindow(itm, true);
	UIAnswersList->ScrollToEnd();

	GAME_NEWS_DATA news_data;
	news_data.news_caption = caption;
	news_data.news_text._set(text);

	news_data.m_type = GAME_NEWS_DATA::eTalk;
	news_data.texture_name = texture_name;
	news_data.receive_time = Level().GetGameTime();

	Actor()->game_news_registry->registry().objects().push_back(news_data);
}

void CUITalkDialogWnd::SetOsoznanieMode(bool b)
{
	//	UIOurIcon.Show		(!b);
	//	UIOthersIcon.Show	(!b);

	UIAnswersList->Show(!b);
	//	UIDialogFrameTop.Show (!b);

	UIToTradeButton.Show(!b);
	if (mechanic_mode)
	{
		UIToTradeButton.m_hint_text = "ui_st_upgrade_hint";
		UIToTradeButton.TextItemControl()->SetTextST("ui_st_upgrade");
	}
	else
	{
		UIToTradeButton.m_hint_text = "ui_st_trade_hint";
		UIToTradeButton.TextItemControl()->SetTextST("ui_st_trade");
	}
}

void CUITalkDialogWnd::UpdateButtonsLayout(bool b_disable_break, bool trade_enabled)
{
	UIToTradeButton.Show(trade_enabled);
}

void CUIQuestionItem::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	CUIWndCallback::OnEvent(pWnd, msg, pData);
}

CUIQuestionItem::CUIQuestionItem(CUIXml* xml_doc, LPCSTR path)
{
	m_text = xr_new<CUI3tButton>();
	m_text->SetAutoDelete(true);
	AttachChild(m_text);

	string512 str;
	CUIXmlInit xml_init;

	xr_strcpy(str, path);
	xml_init.InitWindow(*xml_doc, str, 0, this);

	m_min_height = xml_doc->ReadAttribFlt(path, 0, "min_height", 15.0f);

	strconcat(sizeof(str), str, path, ":content_text");
	xml_init.Init3tButton(*xml_doc, str, 0, m_text);

	Register(m_text);
	AddCallback(m_text, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIQuestionItem::OnTextClicked));

	m_num_text = xr_new<CUITextWnd>();
	m_num_text->SetAutoDelete(true);
	AttachChild(m_num_text);
	strconcat(sizeof(str), str, path, ":num_text");
	xml_init.InitTextWnd(*xml_doc, str, 0, m_num_text);
}

void CUIQuestionItem::Init(LPCSTR val, LPCSTR text)
{
	m_s_value = val;
	m_text->TextItemControl()->SetText(text);
	m_text->AdjustHeightToText();
	float new_h = _max(m_min_height, m_text->GetWndPos().y + m_text->GetHeight());
	SetHeight(new_h);
}

void CUIQuestionItem::OnTextClicked(CUIWindow* w, void*)
{
	GetMessageTarget()->SendMessage(this, LIST_ITEM_CLICKED, (void*)this);
}


CUIAnswerItem::CUIAnswerItem(CUIXml* xml_doc, LPCSTR path)
{
	m_text = xr_new<CUITextWnd>();
	m_text->SetAutoDelete(true);
	m_name = xr_new<CUITextWnd>();
	m_name->SetAutoDelete(true);
	AttachChild(m_text);
	AttachChild(m_name);

	string512 str;
	CUIXmlInit xml_init;

	xr_strcpy(str, path);
	xml_init.InitWindow(*xml_doc, str, 0, this);

	m_min_height = xml_doc->ReadAttribFlt(path, 0, "min_height", 15.0f);
	m_bottom_footer = xml_doc->ReadAttribFlt(path, 0, "bottom_footer", 0.0f);
	strconcat(sizeof(str), str, path, ":content_text");
	xml_init.InitTextWnd(*xml_doc, str, 0, m_text);

	strconcat(sizeof(str), str, path, ":name_caption");
	xml_init.InitTextWnd(*xml_doc, str, 0, m_name);
	SetAutoDelete(true);
}

void CUIAnswerItem::Init(LPCSTR text, LPCSTR name)
{
	m_name->SetText(name);
	m_text->SetText(text);
	m_text->AdjustHeightToText();
	float new_h = _max(m_min_height, m_text->GetWndPos().y + m_text->GetHeight());
	new_h += m_bottom_footer;
	SetHeight(new_h);
}

CUIAnswerItemIconed::CUIAnswerItemIconed(CUIXml* xml_doc, LPCSTR path)
	: CUIAnswerItem(xml_doc, path)
{
	m_icon = xr_new<CUIStatic>();
	m_icon->SetAutoDelete(true);
	AttachChild(m_icon);

	string512 str;
	CUIXmlInit xml_init;

	strconcat(sizeof(str), str, path, ":msg_icon");
	xml_init.InitStatic(*xml_doc, str, 0, m_icon);
}

void CUIAnswerItemIconed::Init(LPCSTR text, LPCSTR name, LPCSTR texture_name)
{
	xr_string res;
	res += name;
	res += "\\n %c[250,255,232,208]";
	res += text;

	inherited::Init(res.c_str(), "");
	m_icon->InitTexture(texture_name);
	m_icon->TextureOn();
	m_icon->SetStretchTexture(true);
}
