#include "stdafx.h"
#include "UIButton.h"
#include "UITextureMaster.h"
#include "GamepadContextDispatcher.h"
#include "UIBtnHint.h"
#include "UICursor.h"
#include "ui_base.h"
#include "../../xrEngine/xr_input.h"
#include "../../xrEngine/GamepadGlyphs.h"
#include "xr_level_controller.h"
#include "string_table.h"

// FIX-LIVE3 п.1: кольцо удержания поверх значка Dot Marks (то же, что в окнах подтверждения).
void DrawFeatheredHoldRing(const Frect& iconRectUi, float progress, bool done, float alpha, ui_shader& shader);

#define PUSH_OFFSET_RIGHT 1
#define PUSH_OFFSET_DOWN  1


CUIButton::CUIButton()
{
	m_eButtonState = BUTTON_NORMAL;
	m_bIsSwitch = false;

	m_uAccelerator[0] = 0;
	m_uAccelerator[1] = 0;
	m_uAccelerator[2] = -1;
	m_uAccelerator[3] = -1;

	TextItemControl()->SetTextComplexMode(false);
	TextItemControl()->SetTextAlignment(CGameFont::alCenter); // this will create class instance for m_pLines
	TextItemControl()->SetVTextAlignment(valCenter);
}

CUIButton::~CUIButton()
{
	xr_delete(m_menuShortcutIcon);
	xr_delete(m_menuBackHoldIcon);
	xr_delete(m_menuDotIcon);
	xr_delete(m_menuFooterArt);
}

void CUIButton::Reset()
{
	m_bCursorOverWindow = false;
	inherited::Reset();
}


void CUIButton::Enable(bool status)
{
	CUIStatic::Enable(status);

	if (!status)
		m_bCursorOverWindow = false;
}

bool CUIButton::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	if (inherited::OnMouseAction(x, y, mouse_action)) return true;

	switch (m_eButtonState)
	{
	case BUTTON_NORMAL:
		{
			if (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK)
			{
				SetButtonState(BUTTON_PUSHED);
				GetMessageTarget()->SendMessage(this, BUTTON_DOWN, NULL);
				return true;
			}
		}
		break;
	case BUTTON_PUSHED:
		{
			if (mouse_action == WINDOW_LBUTTON_UP)
			{
				if (m_bCursorOverWindow)
					OnClick();

				if (!m_bIsSwitch)
					SetButtonState(BUTTON_NORMAL);
			}
			else if (mouse_action == WINDOW_MOUSE_MOVE)
			{
				if (!m_bCursorOverWindow && !m_bIsSwitch)
					SetButtonState(BUTTON_UP);
			}
		}
		break;
	case BUTTON_UP:
		{
			if (mouse_action == WINDOW_MOUSE_MOVE)
			{
				if (m_bCursorOverWindow)
					SetButtonState(BUTTON_PUSHED);
			}
			else if (mouse_action == WINDOW_LBUTTON_UP)
			{
				SetButtonState(BUTTON_NORMAL);
			}
		}
		break;
	};
	return false;
}

void CUIButton::OnClick()
{
	GetMessageTarget()->SendMessage(this, BUTTON_CLICKED);
}

void CUIButton::DrawTexture()
{
	Frect rect;
	GetAbsoluteRect(rect);

	if (m_bTextureEnable && GetShader() && GetShader()->inited())
	{
		if (GetButtonState() == BUTTON_UP || GetButtonState() == BUTTON_NORMAL)
			m_UIStaticItem.SetPos(rect.left + m_TextureOffset.x, rect.top + m_TextureOffset.y);
		else
			m_UIStaticItem.SetPos(rect.left + PUSH_OFFSET_RIGHT + m_TextureOffset.x,
			                      rect.top + PUSH_OFFSET_DOWN + m_TextureOffset.y);

		if (GetStretchTexture())
			m_UIStaticItem.SetSize(Fvector2().set(rect.width(), rect.height()));
		else
			m_UIStaticItem.SetSize(Fvector2().set(m_UIStaticItem.GetTextureRect().width(),
			                                      m_UIStaticItem.GetTextureRect().height()));

		if (Heading())
			m_UIStaticItem.Render(GetHeading());
		else
			m_UIStaticItem.Render();
	}
}

namespace
{
// MENU-FOCUS-4 P13: Bahnschrift SemiBold labels, 2x the 2559x1079 reference frame.
struct FooterGlyph
{
	short x, y, width, height, left, top, advance;
	int mass, moment;
};
constexpr FooterGlyph FooterGlyphs[] = {
#include "UIButtonFooterGlyphsP13.inc"
};
constexpr float FooterGlyphScale = 2.0f;
// Reference frame metrics (px): button 43 high, icon 29 with 7 above/below, 10 from icon to first stroke.
constexpr float RefButton = 43.0f, RefIcon = 29.0f, RefIconTop = 7.0f, RefIconGap = 10.0f, RefCap = 7.0f;
constexpr float FooterStripWidth = 783.0f, FooterStripHeight = 43.0f, FooterStripPitch = 44.0f;
}

void CUIButton::DrawText()
{
	auto& gamepad = CGamepadContextDispatcher::Instance();
	const int shortcut = gamepad.GetMenuShortcutIndex(this);
	if (shortcut < 0)
		CUIStatic::DrawText();
	else
	{
		gamepad.ArrangeMenuShortcutRow(this);
		if (!m_menuShortcutIcon)
		{
			m_menuShortcutIcon = xr_new<CUIStaticItem>();
			CUITextureMaster::InitTexture(R"(ui\ui_gp_xbox_graphite_p12)", m_menuShortcutIcon);
		}
		if (!m_menuFooterArt)
		{
			m_menuFooterArt = xr_new<CUIStaticItem>();
			CUITextureMaster::InitTexture(R"(ui\ui_gp_menu_footer_p13)", m_menuFooterArt);
		}
		auto draw = [](CUIStaticItem& part, float x, float y, float w, float h, float tx, float ty, float tw, float th, u32 color)
		{
			if (w <= 0.0f || h <= 0.0f)
				return;
			part.SetTextureRect(Frect().set(tx, ty, tx + tw, ty + th));
			part.SetSize(Fvector2().set(w, h));
			part.SetPos(x, y);
			part.SetColor(color);
			part.Render();
		};
		Frect bounds;
		GetAbsoluteRect(bounds);
		const float scaleX = UI().ClientToScreenScaledX(1.0f);
		const float scaleY = UI().ClientToScreenScaledY(1.0f);
		if (bounds.width() <= EPS_S || bounds.height() <= EPS_S || scaleX <= EPS_S || scaleY <= EPS_S)
		{
			CUIStatic::DrawText();
			return;
		}
		const float aspect = scaleY / scaleX;
		// One reference pixel in UI units.
		const float py = bounds.height() / RefButton;
		const float px = py * aspect;
		const float widthRef = bounds.width() / px;

		// Face: the dark notify strip at its own scale, a 7 px recessed edge on each side of every button.
		const float faceY = !IsEnabled() ? 2.0f * FooterStripPitch : CursorOverWindow() ? FooterStripPitch : 0.0f;
		CUIStaticItem& art = *m_menuFooterArt;
		const float cap = _min(RefCap, widthRef * 0.5f);
		const float middle = _max(0.0f, widthRef - 2.0f * cap);
		const float source = _min(middle, FooterStripWidth - 2.0f * RefCap);
		draw(art, bounds.x1, bounds.y1, cap * px, bounds.height(), 0, faceY, cap, FooterStripHeight, 0xffffffff);
		draw(art, bounds.x1 + cap * px, bounds.y1, middle * px, bounds.height(),
			(FooterStripWidth - source) * 0.5f, faceY, source, FooterStripHeight, 0xffffffff);
		draw(art, bounds.x2 - cap * px, bounds.y1, cap * px, bounds.height(),
			FooterStripWidth - cap, faceY, cap, FooterStripHeight, 0xffffffff);

		const bool showIcon = pInput && pInput->IsGamepadActiveSource();
		const LPCSTR rawLabel = m_pTextControl ? m_pTextControl->GetText() : nullptr;
		const auto* label = reinterpret_cast<const unsigned char*>(rawLabel);
		bool supported = label && *label;
		float pen = 0.0f, inkLeft = 0.0f, inkRight = 0.0f;
		double mass = 0.0, moment = 0.0;
		bool first = true;
		for (auto p = label; supported && *p; ++p)
		{
			const auto& glyph = FooterGlyphs[*p];
			supported &= glyph.advance > 0 || glyph.width > 0;
			if (glyph.width)
			{
				if (first)
					inkLeft = pen + glyph.left;
				first = false;
				inkRight = pen + glyph.left + glyph.width;
				mass += glyph.mass;
				moment += glyph.moment;
			}
			pen += glyph.advance;
		}
		supported &= mass > 0.0;
		const float inkWidth = supported ? (inkRight - inkLeft) / FooterGlyphScale : 0.0f;
		const float iconRef = showIcon ? RefIcon + RefIconGap : 0.0f;
		const float group = iconRef + inkWidth;
		float x = bounds.x1 + (widthRef - group) * 0.5f * px;
		if (showIcon)
		{
			// Apply/Load, Reset/Delete, Cancel, Defaults. Letters muted to 86%.
			const float xs[] = {128,256,384,256};
			// INV-REWORK-4 + CR1-11: avoid static shared_str (stale on ReloadLanguage + atexit after Core._destroy)
			const LPCSTR ownLabel = rawLabel;
			const bool isLoadBtn = (shortcut == 0 && ownLabel != nullptr &&
			                        xr_strcmp(ownLabel, CStringTable().translate("ui_mm_load").c_str()) == 0);
			const float iconX = isLoadBtn ? 0.0f : xs[shortcut];
			const int backFrame = shortcut == 2 ? gamepad.GetMenuBackHoldFrame(this) : 0;
			const int frame = backFrame ? backFrame : shortcut == 3 ? gamepad.GetMenuDefaultHoldFrame() : 0;
			auto* icon = m_menuShortcutIcon;
			if (backFrame)
			{
				if (!m_menuBackHoldIcon)
				{
					m_menuBackHoldIcon = xr_new<CUIStaticItem>();
					CUITextureMaster::InitTexture(R"(ui\ui_gp_xbox_back_hold_p15)", m_menuBackHoldIcon);
				}
				icon = m_menuBackHoldIcon;
			}
			const float frameTop = backFrame ? 0.0f : 896.0f;
			const float size = frame ? 64.0f : 128.0f;
			// FIX-LIVE2 п.1: без анимации удержания — значок Dot Marks из CGamepadGlyphManager (семейство
			// Xbox/PS/Nintendo, стиль white/dark/color, как в остальных подсказках). Кадры удержания — прежний атлас.
			// FIX-LIVE3 п.1: при удержании значок остаётся Dot Marks, прогресс — тонкое кольцо поверх
			// (DrawFeatheredHoldRing, как в окнах подтверждения), старый атлас — только запасной путь.
			LPCSTR dotTex = nullptr;
			{
				static const gpbind::ButtonId kBtn[] = {gpbind::ButtonId::West, gpbind::ButtonId::North,
				                                        gpbind::ButtonId::East, gpbind::ButtonId::North};
				dotTex = CGamepadGlyphManager::Instance().GetButtonTextureId(
					isLoadBtn ? gpbind::ButtonId::South : kBtn[shortcut]);
			}
			if (dotTex && dotTex[0])
			{
				if (!m_menuDotIcon || xr_strcmp(m_menuDotTex, dotTex) != 0)
				{
					if (!m_menuDotIcon)
						m_menuDotIcon = xr_new<CUIStaticItem>();
					CUITextureMaster::InitTexture(dotTex, m_menuDotIcon);
					xr_strcpy(m_menuDotTex, dotTex);
				}
				const Frect tr = CUITextureMaster::GetTextureRect(dotTex);
				draw(*m_menuDotIcon, x, bounds.y1 + RefIconTop * py, RefIcon * px, RefIcon * py,
					tr.x1, tr.y1, tr.width(), tr.height(), color_argb(IsEnabled() ? 255 : 125, 255, 255, 255));
				if (frame > 0)
				{
					Frect ringRect;
					ringRect.set(x, bounds.y1 + RefIconTop * py, x + RefIcon * px, bounds.y1 + (RefIconTop + RefIcon) * py);
					const float progress = _min(1.0f, float(frame) / 31.0f);
					DrawFeatheredHoldRing(ringRect, progress, progress >= 1.0f, 1.0f, m_holdRingShader);
				}
			}
			else
			draw(*icon, x, bounds.y1 + RefIconTop * py, RefIcon * px, RefIcon * py,
				frame ? float(frame % 16 * 64) : iconX, frame ? frameTop + frame / 16 * 64 : 512.0f,
				size, size, color_argb(IsEnabled() ? 255 : 125, 219, 219, 219));
			x += iconRef * px;
		}
		if (supported)
		{
			// Optical centre of the strokes sits on the icon axis.
			const float axis = bounds.y1 + (RefIconTop + RefIcon * 0.5f) * py;
			const float baseline = axis - float(moment / mass) / FooterGlyphScale * py;
			const u32 color = IsEnabled() ? color_argb(255,192,193,186) : color_argb(255,120,120,116);
			const float gx = px / FooterGlyphScale, gy = py / FooterGlyphScale;
			float cursor = x - inkLeft * gx;
			for (auto p = label; *p; ++p)
			{
				const auto& glyph = FooterGlyphs[*p];
				if (glyph.width && glyph.height)
					draw(art, cursor + glyph.left * gx, baseline + glyph.top * gy, glyph.width * gx, glyph.height * gy,
						glyph.x, glyph.y, glyph.width, glyph.height, color);
				cursor += glyph.advance * gx;
			}
		}
		else
		{
			const Fvector2 savedOffset = m_pTextControl->m_TextOffset;
			m_pTextControl->m_TextOffset.x += iconRef * px * 0.5f;
			CUIStatic::DrawText();
			m_pTextControl->m_TextOffset = savedOffset;
		}
	}
	if (g_btnHint->Owner() == this)
		g_btnHint->Draw_();
}


bool is_in2(const Frect& b1, const Frect& b2)
{
	return (b1.x1 < b2.x1) && (b1.x2 > b2.x2) && (b1.y1 < b2.y1) && (b1.y2 > b2.y2);
}

void CUIButton::Update()
{
	inherited::Update();

	if (CursorOverWindow() && m_hint_text.size() && !g_btnHint->Owner() && Device.dwTimeGlobal > m_dwFocusReceiveTime +
		700)
	{
		g_btnHint->SetHintText(this, m_hint_text.c_str());

		Fvector2 c_pos = GetUICursor().GetCursorPosition();
		Frect vis_rect;
		vis_rect.set(0, 0,UI_BASE_WIDTH, UI_BASE_HEIGHT);

		//select appropriate position
		Frect r;
		r.set(0.0f, 0.0f, g_btnHint->GetWidth(), g_btnHint->GetHeight());
		r.add(c_pos.x, c_pos.y);

		r.sub(0.0f, r.height());
		if (false == is_in2(vis_rect, r))
			r.sub(r.width(), 0.0f);
		if (false == is_in2(vis_rect, r))
			r.add(0.0f, r.height());

		if (false == is_in2(vis_rect, r))
			r.add(r.width(), 45.0f);

		g_btnHint->SetWndPos(r.lt);
	}
}

void CUIButton::OnFocusLost()
{
	inherited::OnFocusLost();

	if (m_eButtonState == BUTTON_PUSHED && pInput->iGetAsyncBtnState(0) && !m_bIsSwitch)
		SetButtonState(BUTTON_NORMAL); //??? 

	if (g_btnHint->Owner() == this)
		g_btnHint->Discard();
}

bool CUIButton::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
	if (WINDOW_KEY_PRESSED == keyboard_action)
	{
		if (IsAccelerator(dik))
		{
			OnClick();
			return true;
		}
	}
	return inherited::OnKeyboardAction(dik, keyboard_action);
}

void CUIButton::SetAccelerator(int iAccel, int idx)
{
	VERIFY(idx>=0 && idx<4);
	m_uAccelerator[idx] = s16(iAccel);
}

const int CUIButton::GetAccelerator(int idx) const
{
	VERIFY(idx>=0 && idx<4);
	return m_uAccelerator[idx];
}

bool CUIButton::IsAccelerator(int iAccel) const
{
	bool res = GetAccelerator(0) == iAccel || GetAccelerator(1) == iAccel;
	if (!res)
	{
		res = ((m_uAccelerator[2] != -1) ? is_binded((EGameActions)GetAccelerator(2), iAccel) : false) ||
			((m_uAccelerator[3] != -1) ? is_binded((EGameActions)GetAccelerator(3), iAccel) : false);
	}
	return res;
}
