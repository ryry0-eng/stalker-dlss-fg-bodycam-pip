#include "StdAfx.h"
#include "UI3tButton.h"
#include "UIXmlInit.h"
#include "UIHint.h"
#include "UITextureMaster.h"
#include "../GamepadContextDispatcher.h"

CUI3tButton::CUI3tButton()
{
	m_bTextureEnable = false;
	m_bUseTextColor[S_Disabled] = true;
	m_bUseTextColor[S_Highlighted] = false;
	m_bUseTextColor[S_Touched] = false;

	m_dwTextColor[S_Enabled] = 0xFFFFFFFF;
	m_dwTextColor[S_Disabled] = 0xFFAAAAAA;
	m_dwTextColor[S_Highlighted] = 0xFFFFFFFF;
	m_dwTextColor[S_Touched] = 0xFFFFFFFF;

	m_background = NULL;
	m_back_frameline = NULL;
	m_back_framewindow = NULL;
	m_frameline_mode = Framemode_None;
}


CUI3tButton::~CUI3tButton()
{
}

void CUI3tButton::OnClick()
{
	CUIButton::OnClick();
	PlaySoundT();
}

bool CUI3tButton::OnMouseDown(int mouse_btn)
{
	return CUIButton::OnMouseDown(mouse_btn);
}

void CUI3tButton::OnFocusLost()
{
	inherited::OnFocusLost();
}

void CUI3tButton::OnFocusReceive()
{
	inherited::OnFocusReceive();
	PlaySoundH();
}

void CUI3tButton::InitSoundH(LPCSTR sound_file)
{
	::Sound->create(m_sound_h, sound_file, st_Effect, sg_SourceType);
}

void CUI3tButton::InitSoundT(LPCSTR sound_file)
{
	::Sound->create(m_sound_t, sound_file, st_Effect, sg_SourceType);
}

void CUI3tButton::PlaySoundT()
{
	if (m_sound_t._handle())
		m_sound_t.play(NULL, sm_2D);
}

void CUI3tButton::PlaySoundH()
{
	if (m_sound_h._handle())
		m_sound_h.play(NULL, sm_2D);
}

void CUI3tButton::InitButton(Fvector2 pos, Fvector2 size)
{
	// Custom draw: the background is painted by DrawTexture, so keep it out of the child list, which
	// runs after DrawTexture and would paint it a second time over anything layered on the face.
	if (m_frameline_mode == Framemode_Line)
	{
		if (!m_back_frameline)
		{
			m_back_frameline = xr_new<CUI_IB_FrameLineWnd>();
			m_back_frameline->SetAutoDelete(true);
			m_back_frameline->SetCustomDraw(true);
			AttachChild(m_back_frameline);
		}
		m_back_frameline->SetWndPos(Fvector2().set(0, 0));
		m_back_frameline->SetWndSize(size);
	}
	else if (m_frameline_mode == Framemode_Window)
	{
		if (!m_back_framewindow)
		{
			m_back_framewindow = xr_new<CUI_IB_FrameWindow>();
			m_back_framewindow->SetAutoDelete(true);
			AttachChild(m_back_framewindow);
		}
		m_back_framewindow->SetWndPos(Fvector2().set(0, 0));
		m_back_framewindow->SetWndSize(size);
	}
	else
	{
		if (!m_background)
		{
			m_background = xr_new<CUI_IB_Static>();
			m_background->SetAutoDelete(true);
			m_background->SetCustomDraw(true);
			AttachChild(m_background);
		}
		m_background->SetWndPos(Fvector2().set(0, 0));
		m_background->SetWndSize(size);
	}
	CUIButton::SetWndPos(pos);
	CUIButton::SetWndSize(size);
}

void CUI3tButton::SetWidth(float width)
{
	CUIButton::SetWidth(width);
	if (m_background) { m_background->SetWidth(width); }
	else if (m_back_frameline) { m_back_frameline->SetWidth(width); }
	else if (m_back_framewindow) { m_back_framewindow->SetWidth(width); }
}

void CUI3tButton::SetHeight(float height)
{
	CUIButton::SetHeight(height);
	if (m_background) { m_background->SetHeight(height); }
	else if (m_back_frameline) { m_back_frameline->SetHeight(height); }
	else if (m_back_framewindow) { m_back_framewindow->SetHeight(height); }
}

void CUI3tButton::InitTexture(LPCSTR tex_name)
{
	string_path tex_enabled;
	string_path tex_disabled;
	string_path tex_touched;
	string_path tex_highlighted;

	// enabled state texture
	xr_strcpy(tex_enabled, tex_name);
	xr_strcat(tex_enabled, "_e");

	// pressed state texture
	xr_strcpy(tex_disabled, tex_name);
	xr_strcat(tex_disabled, "_d");

	// touched state texture
	xr_strcpy(tex_touched, tex_name);
	xr_strcat(tex_touched, "_t");

	// touched state texture
	xr_strcpy(tex_highlighted, tex_name);
	xr_strcat(tex_highlighted, "_h");

	this->InitTexture(tex_enabled, tex_disabled, tex_touched, tex_highlighted);
}

void CUI3tButton::InitTexture(LPCSTR tex_enabled,
                              LPCSTR tex_disabled,
                              LPCSTR tex_touched,
                              LPCSTR tex_highlighted)
{
	if (m_background)
	{
		m_background->InitState(S_Enabled, tex_enabled);
		m_background->InitState(S_Disabled, tex_disabled);
		m_background->InitState(S_Touched, tex_touched);
		m_background->InitState(S_Highlighted, tex_highlighted);
	}
	else if (m_back_frameline)
	{
		m_back_frameline->InitState(S_Enabled, tex_enabled);
		m_back_frameline->InitState(S_Disabled, tex_disabled);
		m_back_frameline->InitState(S_Touched, tex_touched);
		m_back_frameline->InitState(S_Highlighted, tex_highlighted);
	}
	else if (m_back_framewindow)
	{
		m_back_framewindow->InitState(S_Enabled, tex_enabled);
		m_back_framewindow->InitState(S_Disabled, tex_disabled);
		m_back_framewindow->InitState(S_Touched, tex_touched);
		m_back_framewindow->InitState(S_Highlighted, tex_highlighted);
	}

	this->m_bTextureEnable = true;
}

void CUI3tButton::SetTextureOffset(float x, float y)
{
	if (m_background)
	{
		this->m_background->SetTextureOffset(x, y);
	}
}

LPCSTR CUI3tButton::GamepadFocusArt()
{
	if (!m_bTextureEnable || !m_background)
		return nullptr;
	const auto* face = m_background->Get(S_Enabled);
	const LPCSTR name = face ? face->m_TextureName.c_str() : nullptr;
	const auto& focus = CGamepadContextDispatcher::Instance();
	LPCSTR art = nullptr;
	if (name && xr_strcmp(name, "ui_inGame2_checkbox_e") == 0 && focus.HasMenuFocus(this))
		art = GetButtonState() == BUTTON_PUSHED ? "ui_gp_menu_focus_check_on" : "ui_gp_menu_focus_check_off";
	else if (name && xr_strcmp(name, "ui_inGame2_opt_slider_box_e") == 0 && focus.HasMenuFocus(GetParent()))
		art = "ui_gp_menu_focus_slider_p13";
	else if (name && xr_strcmp(name, "ui_radio_e") == 0 && focus.HasMenuFocus(this))
		// P14: same native ring, only lit (no shape swap -> no jump between states).
		art = GetButtonState() == BUTTON_PUSHED ? "ui_gp_menu_focus_radio_t_p14" : "ui_gp_menu_focus_radio_e_p14";
	if (art && CUITextureMaster::FindItem(art).file.size())
		return art;
	return nullptr;
}

void CUI3tButton::Draw()
{
	// MT-MAY: на базе 3d4dcca52 фон (m_background) не помечен SetCustomDraw и рисуется ещё раз как дочернее
	// окно после DrawTexture(). Пока активна подсветка фокуса геймпада, дочерний фон пропускаем на этот кадр,
	// чтобы слои были как в PORT-G/PORT-MT: подсветка -> дети -> текст. Без геймпадного фокуса — как в оригинале.
	if (m_background && !m_background->GetCustomDraw() && GamepadFocusArt())
	{
		m_background->SetCustomDraw(true);
		inherited::Draw();
		m_background->SetCustomDraw(false);
		return;
	}
	inherited::Draw();
}

void CUI3tButton::DrawTexture()
{
	if (m_bTextureEnable)
	{
		if (m_background)
		{
			if (const LPCSTR art = GamepadFocusArt())
			{
				Frect bounds;
				m_background->GetAbsoluteRect(bounds);
				CUIStaticItem item;
				CUITextureMaster::InitTexture(art, &item);
				item.SetPos(bounds.x1, bounds.y1);
				item.SetSize(Fvector2().set(bounds.width(), bounds.height()));
				item.Render();
				return;
			}
			m_background->SetStretchTexture(true);
			m_background->Draw();
		}
		else if (m_back_frameline)
		{
			m_back_frameline->Draw();
		}
		else if (m_back_framewindow)
		{
			m_back_framewindow->Draw();
		}
	}
}

void CUI3tButton::Update()
{
	inherited::Update();

	const IBState state = CurrentIBState();

	if (m_bTextureEnable)
	{
		if (m_background) { m_background->SetCurrentState(state); }
		else if (m_back_frameline) { m_back_frameline->SetCurrentState(state); }
		else if (m_back_framewindow) { m_back_framewindow->SetCurrentState(state); }
	}

	if (m_fTextBlend >= 0.0f)
	{
		const u32 c0 = StateTextColor(S_Enabled), c1 = StateTextColor(S_Highlighted);
		const float t = clampr(m_fTextBlend, 0.0f, 1.0f);
		auto mix = [&](u32 sh) { return u32(float((c0 >> sh) & 0xFF) * (1.0f - t) + float((c1 >> sh) & 0xFF) * t + 0.5f); };
		TextItemControl()->SetTextColor(color_argb(mix(24), mix(16), mix(8), mix(0)));
		return;
	}
	TextItemControl()->SetTextColor(StateTextColor(state));
}
