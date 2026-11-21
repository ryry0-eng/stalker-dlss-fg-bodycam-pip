#include "stdafx.h"
#include "UIMessageBox.h"
#include "UIXmlInit.h"
#include "UI3tButton.h"
#include "UIEditBox.h"
#include "UITextureMaster.h"
#include "../string_table.h"
#include "../ui_base.h"
#include "../ai_space.h"
#include "../../xrServerEntities/script_engine.h"
#include "../../xrEngine/xr_input.h"
#include "../../xrEngine/device.h"
#include "../../xrEngine/GamepadGlyphs.h"
#include "../../Include/xrRender/UIRender.h"
#include "../../Include/xrRender/UIShader.h"

namespace
{
LPCSTR ResolveMessageBoxGlyph(LPCSTR slotName, LPCSTR fallbackXbox)
{
	if (g_ai_space)
	{
		try
		{
			::luabind::functor<LPCSTR> texFn;
			if (ai().script_engine().functor("gp_icons.tex", texFn))
			{
				LPCSTR resolved = texFn(slotName, "round");
				if (resolved && resolved[0])
					return resolved;
			}
		}
		catch (...)
		{
		}
	}

	auto& mgr = CGamepadGlyphManager::Instance();
	mgr.EnsureInitialized();
	switch (mgr.GetEffectiveGlyphSet())
	{
	case EGamepadGlyphSet::PlayStation:
		if (0 == xr_strcmp(slotName, "south")) return "ui_gp_white_ps_south";
		if (0 == xr_strcmp(slotName, "west"))  return "ui_gp_white_ps_west";
		if (0 == xr_strcmp(slotName, "east"))  return "ui_gp_white_ps_east";
		if (0 == xr_strcmp(slotName, "north")) return "ui_gp_white_ps_north";
		break;
	case EGamepadGlyphSet::Nintendo:
		if (0 == xr_strcmp(slotName, "south")) return "ui_gp_white_nin_south";
		if (0 == xr_strcmp(slotName, "west"))  return "ui_gp_white_nin_west";
		if (0 == xr_strcmp(slotName, "east"))  return "ui_gp_white_nin_east";
		if (0 == xr_strcmp(slotName, "north")) return "ui_gp_white_nin_north";
		break;
	default:
		break;
	}
	return fallbackXbox;
}

bool AreMenuGlyphHintsVisible()
{
	if (g_ai_space)
	{
		try
		{
			::luabind::functor<bool> visFn;
			if (ai().script_engine().functor("gp_icons.hints_visible", visFn))
				return visFn("menu");
		}
		catch (...)
		{
		}
	}
	return true;
}

float QueryMenuGlyphScaleK()
{
	if (g_ai_space)
	{
		try
		{
			::luabind::functor<float> scaleFn;
			if (ai().script_engine().functor("gp_icons.scale_k", scaleFn))
			{
				const float k = scaleFn();
				if (k >= 0.5f && k <= 2.5f)
					return k;
			}
		}
		catch (...)
		{
		}
	}
	return 1.0f;
}

int QueryMenuGlyphRevision()
{
	if (g_ai_space)
	{
		try
		{
			::luabind::functor<int> revFn;
			if (ai().script_engine().functor("gp_icons.revision", revFn))
				return revFn();
		}
		catch (...)
		{
		}
	}
	return 0;
}

} // namespace

// FIX-LIVE3 п.1/2: общий для окон подтверждения и кнопок нижней строки настроек (CUIButton).
void DrawFeatheredHoldRing(const Frect& iconRectUi, float progress, bool done, float alpha, ui_shader& shader)
{
	if (progress <= 0.0f || alpha <= 0.0f)
		return;
	if (!shader->inited())
		shader->create("hud\\crosshair");

	Fvector2 tl, br;
	UI().ClientToScreenScaled(tl, iconRectUi.x1, iconRectUi.y1);
	UI().ClientToScreenScaled(br, iconRectUi.x2, iconRectUi.y2);
	const float cx = (tl.x + br.x) * 0.5f;
	const float cy = (tl.y + br.y) * 0.5f;
	const float k = float(Device.dwHeight) / UI_BASE_HEIGHT;
	const float line = _max(1.0f, floorf(k + 0.5f));
	const float half = _min(br.x - tl.x, br.y - tl.y) * 0.5f;
	const float r1 = half * (60.0f / 64.0f) + line;
	const float r0 = _max(1.0f, r1 - line * 1.5f);
	const u32 c = done
		? color_argb(u32(245.0f * alpha), 240, 240, 240)
		: color_argb(u32(230.0f * alpha), 240, 240, 240);
	const u32 cz = c & 0x00FFFFFFu;
	const float rr[4] = { _max(0.0f, r0 - 1.0f), r0, r1, r1 + 1.0f };
	const u32 cc[4] = { cz, c, c, cz };

	constexpr int SegsFull = 128;
	const int n = _max(1, int(ceilf(SegsFull * progress)));
	const float sweep = PI_MUL_2 * progress;
	UIRender->SetShader(*shader);
	UIRender->StartPrimitive(n * 18, IUIRender::ptTriList, UI().m_currentPointType);
	for (int i = 0; i < n; ++i)
	{
		const float a0 = sweep * float(i) / n - PI_DIV_2;
		const float a1 = sweep * float(i + 1) / n - PI_DIV_2;
		const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
		for (int b = 0; b < 3; ++b)
		{
			const float ri = rr[b], ro = rr[b + 1];
			const u32 ci = cc[b], co = cc[b + 1];
			UIRender->PushPoint(cx + c0 * ri, cy + s0 * ri, 0.0f, ci, 0.0f, 0.0f);
			UIRender->PushPoint(cx + c0 * ro, cy + s0 * ro, 0.0f, co, 0.0f, 0.0f);
			UIRender->PushPoint(cx + c1 * ro, cy + s1 * ro, 0.0f, co, 0.0f, 0.0f);
			UIRender->PushPoint(cx + c0 * ri, cy + s0 * ri, 0.0f, ci, 0.0f, 0.0f);
			UIRender->PushPoint(cx + c1 * ro, cy + s1 * ro, 0.0f, co, 0.0f, 0.0f);
			UIRender->PushPoint(cx + c1 * ri, cy + s1 * ri, 0.0f, ci, 0.0f, 0.0f);
		}
	}
	UIRender->FlushPrimitive();
}

namespace
{

// Keep the XML button face and localized font. Only the label's horizontal offset
// changes while drawing the active Dot Marks glyph and hold ring beside it.
class CUIMessageChoiceButton final : public CUI3tButton
{
public:
	// ok == true: окно с одной кнопкой «OK» (INV-REWORK-22 / В3: значок A, закрывается A или B)
	CUIMessageChoiceButton(CUIMessageBox* owner, bool yes, bool ok = false, LPCSTR customSlot = nullptr, int choiceIndex = -1)
		: m_owner(owner)
		, m_ok(ok)
		, m_choiceIndex(choiceIndex >= 0 ? choiceIndex : (ok ? 0 : (yes ? 0 : 2)))
		, m_slotName(customSlot ? customSlot : (ok ? "south" : (yes ? "west" : "east")))
		, m_fallbackGlyphId(customSlot && 0 == xr_strcmp(customSlot, "north")
			? "ui_gp_xbox_north"
			: (ok ? "ui_gp_xbox_south" : (yes ? "ui_gp_xbox_west" : "ui_gp_xbox_east")))
	{
	}

	// FIX-LIVE6 (2026-11-16): Lua (gp_icons.*) опрашивается только здесь, в Update главного потока.
	// Раньше вызовы шли из DrawText (поток рендера) параллельно с CLevel::LuaGC (EngineThreading seqLuaGC):
	// вылет lj_gc.c gc_onestep при «Выйти в Windows» и откат на старые значки ui_gp_xbox_* при сбое functor.
	void Update() override
	{
		CUI3tButton::Update();
		m_cacheHintsVisible = AreMenuGlyphHintsVisible();
		m_cacheScaleK = QueryMenuGlyphScaleK();
		m_cacheRev = QueryMenuGlyphRevision();
		m_cacheTex = ResolveMessageBoxGlyph(m_slotName, m_fallbackGlyphId);
		m_cacheValid = true;
	}

	void DrawText() override
	{
		const bool validMode = m_ok
			? m_owner->HasGamepadOk()
			: (m_owner->HasGamepadConfirmation() || m_owner->HasGamepadFatalErrorChoice());
		if (!pInput || !pInput->IsGamepadActiveSource() || !validMode || !m_pTextControl || !m_cacheValid || !m_cacheHintsVisible)
		{
			CUI3tButton::DrawText();
			return;
		}

		auto& label = *m_pTextControl;
		auto* font = label.GetFont();
		const float scaleX = UI().ClientToScreenScaledX(1.0f);
		const float scaleY = UI().ClientToScreenScaledY(1.0f);
		if (!font || scaleX <= 0.0f || scaleY <= 0.0f)
		{
			CUI3tButton::DrawText();
			return;
		}

		Frect bounds;
		GetAbsoluteRect(bounds);
		const float iconScale = _min(1.25f, m_cacheScaleK);
		const float rawPxH = _max(10.0f, floorf(bounds.height() * (26.0f / 43.0f) * iconScale * scaleY + 0.5f));
		const float iconHeight = rawPxH / scaleY;
		const float iconWidth = rawPxH / scaleX;
		const float gap = iconWidth * (8.0f / 26.0f);
		const float labelWidth = font->SizeOf_(label.GetText()) / scaleX;
		const float groupWidth = iconWidth + gap + labelWidth;
		if (iconWidth <= 0.0f || groupWidth >= bounds.width())
		{
			CUI3tButton::DrawText();
			return;
		}

		const int rev = m_cacheRev;
		const shared_str wantedTex = m_cacheTex;
		if (!m_glyphInitialized || m_lastRev != rev || m_activeTex != wantedTex)
		{
			CUITextureMaster::InitTexture(wantedTex.c_str(), &m_glyph);
			m_activeTex = wantedTex;
			m_lastRev = rev;
			m_glyphInitialized = true;
		}

		const float shakeDx = m_owner->SampleGamepadShakeOffset(m_choiceIndex);
		const float startX = bounds.x1 + (bounds.width() - groupWidth) * 0.5f + shakeDx;
		const float iconY = floorf((bounds.y1 + (bounds.height() - iconHeight) * 0.5f) * scaleY + 0.5f) / scaleY;
		const float labelCenterX = startX + iconWidth + gap + labelWidth * 0.5f;
		const Fvector2 offset = label.m_TextOffset;
		const auto alignment = label.GetTextAlignment();
		label.SetTextAlignment(CGameFont::alCenter);
		label.m_TextOffset.x = labelCenterX - (bounds.x1 + bounds.x2) * 0.5f;
		CUI3tButton::DrawText();
		label.m_TextOffset = offset;
		label.SetTextAlignment(alignment);

		m_glyph.SetSize(Fvector2().set(iconWidth, iconHeight));
		m_glyph.SetPos(startX, iconY);
		m_glyph.SetColor(color_argb(IsEnabled() ? 255 : 125, 235, 235, 235));
		m_glyph.Render();

		if (m_owner->GetGamepadHoldChoice() == m_choiceIndex)
		{
			float progress = m_owner->GetGamepadHoldProgress();
			float alpha = 1.0f;
			const bool done = m_owner->IsGamepadHoldDone();
			if (done)
			{
				const u32 doneAt = m_owner->GetGamepadHoldDoneAt();
				const u32 now = Device.dwTimeContinual;
				if (!doneAt || now < doneAt || now - doneAt >= 300)
					progress = 0.0f;
				else
				{
					progress = 1.0f;
					const float t = float(now - doneAt) / 300.0f;
					alpha = 1.0f - (t * t * (3.0f - 2.0f * t));
				}
			}
			if (progress > 0.0f)
			{
				Frect iconRect;
				iconRect.set(startX, iconY, startX + iconWidth, iconY + iconHeight);
				DrawFeatheredHoldRing(iconRect, progress, done, alpha, m_ringShader);
			}
		}
	}

private:
	CUIMessageBox* m_owner;
	bool m_ok;
	int m_choiceIndex;
	LPCSTR m_slotName;
	LPCSTR m_fallbackGlyphId;
	shared_str m_activeTex;
	int m_lastRev = -1;
	CUIStaticItem m_glyph;
	ui_shader m_ringShader;
	bool m_glyphInitialized = false;
	bool m_cacheValid = false;
	bool m_cacheHintsVisible = true;
	float m_cacheScaleK = 1.0f;
	int m_cacheRev = 0;
	shared_str m_cacheTex;
};
} // namespace

CUIMessageBox::CUIMessageBox()
{
	m_UIButtonYesOk = NULL;
	m_UIButtonNo = NULL;
	m_UIButtonCancel = NULL;
	m_UIButtonCopy = NULL;
	m_UIStaticPicture = NULL;
	m_UIStaticText = NULL;

	m_UIEditPass = NULL;
	m_UIEditUserPass = NULL;
	m_UIEditURL = NULL;
	m_UIEditHost = NULL;
	m_UIStaticPass = NULL;
	m_UIStaticUserPass = NULL;
	m_UIStaticHost = NULL;
}

CUIMessageBox::~CUIMessageBox()
{
	Clear();
}

#define BUTTON_UP_OFFSET 75
#define BUTTON_WIDTH 140

void CUIMessageBox::Clear()
{
	xr_delete(m_UIButtonYesOk);
	xr_delete(m_UIButtonNo);
	xr_delete(m_UIButtonCancel);
	xr_delete(m_UIButtonCopy);
	xr_delete(m_UIStaticPicture);
	xr_delete(m_UIStaticText);
	xr_delete(m_UIEditPass);
	xr_delete(m_UIEditUserPass);
	xr_delete(m_UIEditURL);
	xr_delete(m_UIEditHost);
	xr_delete(m_UIStaticPass);
	xr_delete(m_UIStaticUserPass);
	xr_delete(m_UIStaticHost);
}

bool CUIMessageBox::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	return inherited::OnMouseAction(x, y, mouse_action);
}

void CUIMessageBox::InitMessageBox(LPCSTR box_template)
{
	Clear();
	m_boxTemplate = box_template ? box_template : "";
	m_gpHoldChoice = -1;
	m_gpHoldProgress = 0.0f;
	m_gpHoldDone = false;
	m_gpHoldDoneAt = 0;
	m_gpShakeChoice = -1;
	m_gpShakeAt = 0;
	CUIXml uiXml;
	uiXml.Load(CONFIG_PATH, UI_PATH, "message_box.xml");
	CUIXmlInit xml_init;

	string512 str;

	strconcat(sizeof(str), str, box_template, ":picture");
	if (uiXml.NavigateToNode(str, 0))
	{
		m_UIStaticPicture = xr_new<CUIStatic>();
		AttachChild(m_UIStaticPicture);
		xml_init.InitStatic(uiXml, str, 0, m_UIStaticPicture);
	}

	strconcat(sizeof(str), str, box_template, ":message_text");
	if (uiXml.NavigateToNode(str, 0))
	{
		m_UIStaticText = xr_new<CUITextWnd>();
		AttachChild(m_UIStaticText);
		xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticText);
	}

	xr_strcpy(str, box_template);
	xml_init.InitStatic(uiXml, str, 0, this);

	LPCSTR _type = uiXml.ReadAttrib(str, 0, "type",NULL);
	R_ASSERT(_type);

	m_eMessageBoxStyle = MESSAGEBOX_OK;
	if (0 == stricmp(_type, "ok") || 0 == stricmp(_type, "ok_win"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_OK;
	}
	else if (0 == stricmp(_type, "yes_no"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_YES_NO;
	}
	else if (0 == stricmp(_type, "yes_no_cancel"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_YES_NO_CANCEL;
	}
	else if (0 == stricmp(_type, "yes_no_copy"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_YES_NO_COPY;
	}
	else if (0 == stricmp(_type, "direct_ip"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_DIRECT_IP;
	}
	else if (0 == stricmp(_type, "ra_login"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_RA_LOGIN;
	}
	else if (0 == stricmp(_type, "password"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_PASSWORD;
	}
	else if (0 == stricmp(_type, "quit_windows"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_QUIT_WINDOWS;
	}
	else if (0 == stricmp(_type, "quit_game"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_QUIT_GAME;
	}
	else if (0 == stricmp(_type, "info"))
	{
		m_eMessageBoxStyle = MESSAGEBOX_INFO;
	};


	switch (m_eMessageBoxStyle)
	{
	case MESSAGEBOX_OK:
		{
			strconcat(sizeof(str), str, box_template, ":button_ok");
			m_UIButtonYesOk = xr_new<CUIMessageChoiceButton>(this, true, true);
			AttachChild(m_UIButtonYesOk);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);
		}
		break;
	case MESSAGEBOX_INFO:
		{
		}
		break;

	case MESSAGEBOX_DIRECT_IP:
		strconcat(sizeof(str), str, box_template, ":cap_host");
		m_UIStaticHost = xr_new<CUITextWnd>();
		AttachChild(m_UIStaticHost);
		xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticHost);

		strconcat(sizeof(str), str, box_template, ":edit_host");
		m_UIEditHost = xr_new<CUIEditBox>();
		AttachChild(m_UIEditHost);
		xml_init.InitEditBox(uiXml, str, 0, m_UIEditHost);

		strconcat(sizeof(str), str, box_template, ":cap_password");
		m_UIStaticPass = xr_new<CUITextWnd>();
		AttachChild(m_UIStaticPass);
		xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticPass);

		strconcat(sizeof(str), str, box_template, ":edit_password");
		m_UIEditPass = xr_new<CUIEditBox>();
		AttachChild(m_UIEditPass);
		xml_init.InitEditBox(uiXml, str, 0, m_UIEditPass);

		strconcat(sizeof(str), str, box_template, ":button_yes");
		m_UIButtonYesOk = xr_new<CUI3tButton>();
		AttachChild(m_UIButtonYesOk);
		xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);

		strconcat(sizeof(str), str, box_template, ":button_no");
		m_UIButtonNo = xr_new<CUI3tButton>();
		AttachChild(m_UIButtonNo);
		xml_init.Init3tButton(uiXml, str, 0, m_UIButtonNo);
		//m_message_box_yes_no->func_on_ok = CUIWndCallback::void_function( this, &CUIActorMenu::OnMesBoxYes );

		break;
	case MESSAGEBOX_PASSWORD:
		{
			strconcat(sizeof(str), str, box_template, ":cap_user_password");
			m_UIStaticUserPass = xr_new<CUITextWnd>();
			AttachChild(m_UIStaticUserPass);
			xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticUserPass);

			strconcat(sizeof(str), str, box_template, ":cap_password");
			m_UIStaticPass = xr_new<CUITextWnd>();
			AttachChild(m_UIStaticPass);
			xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticPass);

			strconcat(sizeof(str), str, box_template, ":edit_user_password");
			m_UIEditUserPass = xr_new<CUIEditBox>();
			AttachChild(m_UIEditUserPass);
			xml_init.InitEditBox(uiXml, str, 0, m_UIEditUserPass);

			strconcat(sizeof(str), str, box_template, ":edit_password");
			m_UIEditPass = xr_new<CUIEditBox>();
			AttachChild(m_UIEditPass);
			xml_init.InitEditBox(uiXml, str, 0, m_UIEditPass);

			strconcat(sizeof(str), str, box_template, ":button_yes");
			m_UIButtonYesOk = xr_new<CUI3tButton>();
			AttachChild(m_UIButtonYesOk);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);

			strconcat(sizeof(str), str, box_template, ":button_no");
			m_UIButtonNo = xr_new<CUI3tButton>();
			AttachChild(m_UIButtonNo);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonNo);
		}
		break;

	case MESSAGEBOX_RA_LOGIN:
		strconcat(sizeof(str), str, box_template, ":cap_login");
		m_UIStaticUserPass = xr_new<CUITextWnd>();
		AttachChild(m_UIStaticUserPass);
		xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticUserPass);

		strconcat(sizeof(str), str, box_template, ":cap_password");
		m_UIStaticPass = xr_new<CUITextWnd>();
		AttachChild(m_UIStaticPass);
		xml_init.InitTextWnd(uiXml, str, 0, m_UIStaticPass);

		strconcat(sizeof(str), str, box_template, ":edit_login");
		m_UIEditUserPass = xr_new<CUIEditBox>();
		AttachChild(m_UIEditUserPass);
		xml_init.InitEditBox(uiXml, str, 0, m_UIEditUserPass);

		strconcat(sizeof(str), str, box_template, ":edit_password");
		m_UIEditPass = xr_new<CUIEditBox>();
		AttachChild(m_UIEditPass);
		xml_init.InitEditBox(uiXml, str, 0, m_UIEditPass);

		m_UIEditUserPass->SetNextFocusCapturer(m_UIEditPass);
		m_UIEditPass->SetNextFocusCapturer(m_UIEditUserPass);
		m_UIEditUserPass->CaptureFocus(true);

	case MESSAGEBOX_QUIT_WINDOWS:
	case MESSAGEBOX_QUIT_GAME:
	case MESSAGEBOX_YES_NO:
		{
			strconcat(sizeof(str), str, box_template, ":button_yes");
			m_UIButtonYesOk = xr_new<CUIMessageChoiceButton>(this, true);
			AttachChild(m_UIButtonYesOk);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);

			strconcat(sizeof(str), str, box_template, ":button_no");
			m_UIButtonNo = xr_new<CUIMessageChoiceButton>(this, false);
			AttachChild(m_UIButtonNo);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonNo);
		}
		break;

	case MESSAGEBOX_YES_NO_CANCEL:
		{
			strconcat(sizeof(str), str, box_template, ":button_yes");
			m_UIButtonYesOk = xr_new<CUI3tButton>();
			AttachChild(m_UIButtonYesOk);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);

			strconcat(sizeof(str), str, box_template, ":button_no");
			m_UIButtonNo = xr_new<CUI3tButton>();
			AttachChild(m_UIButtonNo);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonNo);

			strconcat(sizeof(str), str, box_template, ":button_cancel");
			m_UIButtonCancel = xr_new<CUI3tButton>();
			AttachChild(m_UIButtonCancel);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonCancel);
		}
		break;

	case MESSAGEBOX_YES_NO_COPY:
		{
			strconcat(sizeof(str), str, box_template, ":button_yes");
			m_UIButtonYesOk = xr_new<CUIMessageChoiceButton>(this, true, false, "west", 0);
			AttachChild(m_UIButtonYesOk);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonYesOk);

			strconcat(sizeof(str), str, box_template, ":button_no");
			m_UIButtonNo = xr_new<CUIMessageChoiceButton>(this, false, false, "east", 2);
			AttachChild(m_UIButtonNo);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonNo);

			strconcat(sizeof(str), str, box_template, ":button_copy");
			m_UIButtonCopy = xr_new<CUIMessageChoiceButton>(this, false, false, "north", 1);
			AttachChild(m_UIButtonCopy);
			xml_init.Init3tButton(uiXml, str, 0, m_UIButtonCopy);

			strconcat(sizeof(str), str, box_template, ":edit_url");
			if (uiXml.NavigateToNode(str))
			{
				m_UIEditURL = xr_new<CUIEditBox>();
				AttachChild(m_UIEditURL);
				xml_init.InitEditBox(uiXml, str, 0, m_UIEditURL);
				//				m_UIEditURL->read_only
			}
		}
		break;
	};
}


bool CUIMessageBox::HasGamepadConfirmation()
{
	if (m_eMessageBoxStyle != MESSAGEBOX_YES_NO && m_eMessageBoxStyle != MESSAGEBOX_QUIT_GAME &&
		m_eMessageBoxStyle != MESSAGEBOX_QUIT_WINDOWS)
		return false;
	if (!IsShown() || !m_UIButtonYesOk || !m_UIButtonYesOk->IsShown() || !m_UIButtonNo || !m_UIButtonNo->IsShown())
		return false;
	if ((m_UIButtonCancel && m_UIButtonCancel->IsShown()) || (m_UIButtonCopy && m_UIButtonCopy->IsShown()))
		return false;

	const auto* yes = m_UIButtonYesOk->TextItemControl()->GetText();
	const auto* no = m_UIButtonNo->TextItemControl()->GetText();
	return yes && yes[0] && no && no[0];
}

bool CUIMessageBox::HasGamepadOk()
{
	return m_eMessageBoxStyle == MESSAGEBOX_OK && IsShown() &&
		m_UIButtonYesOk && m_UIButtonYesOk->IsShown() &&
		(!m_UIButtonNo || !m_UIButtonNo->IsShown()) &&
		(!m_UIButtonCancel || !m_UIButtonCancel->IsShown());
}

bool CUIMessageBox::ActivateGamepadConfirmation(bool yes)
{
	if (!HasGamepadConfirmation() || !GetMessageTarget())
		return false;

	auto* button = yes ? m_UIButtonYesOk : m_UIButtonNo;
	if (!button->IsEnabled())
		return false;

	button->OnClick();
	return true;
}

// FIX-LIVE3 п.24: подтверждение выхода с геймпада. «ОК» — тот же путь, что щелчок мыши по кнопке
// (OnYesOk -> MESSAGE_BOX_QUIT_WIN_CLICKED / QUIT_GAME_CLICKED -> Lua main_menu:OnMessageQuitWin), но без
// проверок текста и IsEnabled кнопки. «Отмена» — как щелчок по button_no, без кнопки — просто скрыть окно.
bool CUIMessageBox::ActivateGamepadQuit(bool yes)
{
	if (!IsGamepadQuitConfirmation() || !GetMessageTarget())
		return false;
	if (yes)
	{
		OnYesOk();
		return true;
	}
	if (m_UIButtonNo)
	{
		SendMessage(m_UIButtonNo, BUTTON_CLICKED, nullptr);
		return true;
	}
	return false;
}

bool CUIMessageBox::ActivateGamepadOk()
{
	if (!HasGamepadOk() || !GetMessageTarget())
		return false;

	if (!m_UIButtonYesOk->IsEnabled())
		return false;

	m_UIButtonYesOk->OnClick();
	return true;
}

bool CUIMessageBox::IsGamepadQuitConfirmation() const
{
	if (m_eMessageBoxStyle == MESSAGEBOX_QUIT_WINDOWS || m_eMessageBoxStyle == MESSAGEBOX_QUIT_GAME)
		return true;
	const char* tpl = m_boxTemplate.c_str();
	return tpl && tpl[0] && (0 == stricmp(tpl, "message_box_quit_windows") || 0 == stricmp(tpl, "message_box_quit_game"));
}

bool CUIMessageBox::IsGamepadDangerousConfirmation() const
{
	if (m_eMessageBoxStyle == MESSAGEBOX_QUIT_WINDOWS || m_eMessageBoxStyle == MESSAGEBOX_QUIT_GAME)
		return true;
	const char* tpl = m_boxTemplate.c_str();
	if (!tpl || !tpl[0])
		return false;
	return 0 == stricmp(tpl, "message_box_quit_windows") ||
		0 == stricmp(tpl, "message_box_quit_game") ||
		0 == stricmp(tpl, "message_box_change_level") ||
		0 == stricmp(tpl, "message_box_delete_file_name") ||
		0 == stricmp(tpl, "message_box_file_already_exist") ||
		0 == stricmp(tpl, "message_box_confirm_load_save");
}

bool CUIMessageBox::HasGamepadFatalErrorChoice() const
{
	return m_eMessageBoxStyle == MESSAGEBOX_YES_NO_COPY && const_cast<CUIMessageBox*>(this)->IsShown() &&
		m_UIButtonYesOk && m_UIButtonYesOk->IsShown() &&
		m_UIButtonNo && m_UIButtonNo->IsShown() &&
		m_UIButtonCopy && m_UIButtonCopy->IsShown();
}

CUI3tButton* CUIMessageBox::GetGamepadChoiceButton(int choice) const
{
	if (choice == 0) return m_UIButtonYesOk;
	if (choice == 1) return m_UIButtonCopy;
	if (choice == 2) return m_UIButtonNo;
	return nullptr;
}

bool CUIMessageBox::ActivateGamepadFatalChoice(int choice)
{
	if (!HasGamepadFatalErrorChoice() || !GetMessageTarget())
		return false;
	auto* btn = GetGamepadChoiceButton(choice);
	if (!btn || !btn->IsEnabled())
		return false;
	btn->OnClick();
	return true;
}

void CUIMessageBox::SetGamepadHoldState(int targetChoice, float progress, bool done)
{
	m_gpHoldChoice = targetChoice;
	m_gpHoldProgress = progress;
	if (done && !m_gpHoldDone)
		m_gpHoldDoneAt = Device.dwTimeContinual;
	else if (!done)
		m_gpHoldDoneAt = 0;
	m_gpHoldDone = done;
}

void CUIMessageBox::TriggerGamepadDenyShake(int targetChoice)
{
	m_gpShakeChoice = targetChoice;
	m_gpShakeAt = Device.dwTimeContinual;
}

float CUIMessageBox::SampleGamepadShakeOffset(int targetChoice) const
{
	if (m_gpShakeChoice != targetChoice || m_gpShakeAt == 0)
		return 0.0f;
	const u32 now = Device.dwTimeContinual;
	if (now < m_gpShakeAt || now - m_gpShakeAt >= 500)
		return 0.0f;
	const float t = float(now - m_gpShakeAt) / 500.0f;
	return 4.0f * (1.0f - t) * sinf(t * PI * 8.0f);
}

void CUIMessageBox::OnYesOk()
{
	switch (m_eMessageBoxStyle)
	{
	case MESSAGEBOX_OK:
	case MESSAGEBOX_INFO:
		GetMessageTarget()->SendMessage(m_UIButtonYesOk, MESSAGE_BOX_OK_CLICKED);
		GetMessageTarget()->SendMessage(this, MESSAGE_BOX_OK_CLICKED);
		break;

	case MESSAGEBOX_DIRECT_IP:
	case MESSAGEBOX_RA_LOGIN:
	case MESSAGEBOX_YES_NO_CANCEL:
	case MESSAGEBOX_YES_NO_COPY:
	case MESSAGEBOX_PASSWORD:
	case MESSAGEBOX_YES_NO:
		GetMessageTarget()->SendMessage(m_UIButtonYesOk, MESSAGE_BOX_YES_CLICKED);
		GetMessageTarget()->SendMessage(this, MESSAGE_BOX_YES_CLICKED);
		break;
	case MESSAGEBOX_QUIT_WINDOWS:
		GetMessageTarget()->SendMessage(this, MESSAGE_BOX_QUIT_WIN_CLICKED);
		break;
	case MESSAGEBOX_QUIT_GAME:
		GetMessageTarget()->SendMessage(this, MESSAGE_BOX_QUIT_GAME_CLICKED);
		break;
	};
}

void CUIMessageBox::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	if (msg == BUTTON_CLICKED)
	{
		switch (m_eMessageBoxStyle)
		{
		case MESSAGEBOX_OK:
			if (pWnd == m_UIButtonYesOk)
			{
				OnYesOk();
			}
			break;
		case MESSAGEBOX_DIRECT_IP:
		case MESSAGEBOX_RA_LOGIN:
		case MESSAGEBOX_PASSWORD:
		case MESSAGEBOX_YES_NO:
		case MESSAGEBOX_QUIT_GAME:
		case MESSAGEBOX_QUIT_WINDOWS:
			if (pWnd == m_UIButtonYesOk)
			{
				OnYesOk();
			}
			else if (pWnd == m_UIButtonNo)
			{
				GetMessageTarget()->SendMessage(m_UIButtonNo, MESSAGE_BOX_NO_CLICKED);
				GetMessageTarget()->SendMessage(this, MESSAGE_BOX_NO_CLICKED);
			}
			break;
		case MESSAGEBOX_YES_NO_CANCEL:
			if (pWnd == m_UIButtonYesOk)
			{
				OnYesOk();
			}
			else if (pWnd == m_UIButtonNo)
			{
				GetMessageTarget()->SendMessage(m_UIButtonNo, MESSAGE_BOX_NO_CLICKED);
				GetMessageTarget()->SendMessage(this, MESSAGE_BOX_NO_CLICKED);
			}
			else if (pWnd == m_UIButtonCancel)
			{
				GetMessageTarget()->SendMessage(m_UIButtonCancel, MESSAGE_BOX_CANCEL_CLICKED);
				GetMessageTarget()->SendMessage(this, MESSAGE_BOX_CANCEL_CLICKED);
			}
			break;
		case MESSAGEBOX_YES_NO_COPY:
			if (pWnd == m_UIButtonYesOk)
			{
				OnYesOk();
			}
			else if (pWnd == m_UIButtonNo)
			{
				GetMessageTarget()->SendMessage(m_UIButtonNo, MESSAGE_BOX_NO_CLICKED);
				GetMessageTarget()->SendMessage(this, MESSAGE_BOX_NO_CLICKED);
			}
			else if (pWnd == m_UIButtonCopy)
			{
				GetMessageTarget()->SendMessage(m_UIButtonCopy, MESSAGE_BOX_COPY_CLICKED);
				GetMessageTarget()->SendMessage(this, MESSAGE_BOX_COPY_CLICKED);
			}
			break;
		};
	};
	inherited::SendMessage(pWnd, msg, pData);
}

void CUIMessageBox::SetText(LPCSTR str)
{
	m_UIStaticText->SetTextST(str);
}

LPCSTR CUIMessageBox::GetText()
{
	return m_UIStaticText->GetText();
}

LPCSTR CUIMessageBox::GetHost()
{
	if (m_UIEditHost)
	{
		m_ret_val.clear();
		xr_string tmp = m_UIEditHost->GetText();
		xr_string::size_type pos = tmp.find(":");

		if (xr_string::npos != pos)
		{
			m_ret_val.assign(tmp.begin(), tmp.begin() + pos);
			tmp.erase(tmp.begin(), tmp.begin() + pos + 1);

			m_ret_val += "/port=";
		}
		m_ret_val += tmp;

		return m_ret_val.c_str();
	}
	else
		return NULL;
}

LPCSTR CUIMessageBox::GetPassword()
{
	if (m_UIEditPass)
		return m_UIEditPass->GetText();
	else
		return NULL;
}

LPCSTR CUIMessageBox::GetUserPassword()
{
	if (m_UIEditUserPass)
		return m_UIEditUserPass->GetText();
	else
		return NULL;
}

void CUIMessageBox::SetTextEditURL(LPCSTR text)
{
	if (m_UIEditURL)
	{
		m_UIEditURL->SetText(text);
	}
}

LPCSTR CUIMessageBox::GetTextEditURL()
{
	if (m_UIEditURL)
	{
		return m_UIEditURL->GetText();
	}
	return NULL;
}

void CUIMessageBox::SetUserPasswordMode(bool b)
{
	if (m_UIEditUserPass)
		m_UIEditUserPass->Show(b);
	if (m_UIStaticUserPass)
		m_UIStaticUserPass->Show(b);
}

void CUIMessageBox::SetPasswordMode(bool b)
{
	if (m_UIEditPass)
		m_UIEditPass->Show(b);
	if (m_UIStaticPass)
		m_UIStaticPass->Show(b);
}
