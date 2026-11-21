#include "stdafx.h"
#include "uicursor.h"

#include "ui/UIStatic.h"
#include "ui/UIBtnHint.h"
#include "xrEngine/IInputReceiver.h"
#include "xrEngine/xr_input.h"
#include "GamepadContextDispatcher.h"
#include "Actor_Flags.h"
#include "../Include/xrRender/UIRender.h"

#define C_DEFAULT	D3DCOLOR_XRGB(0xff,0xff,0xff)

float ps_gp_cursor_size = 1.5f;
float ps_gp_cursor_speed = 1.0f;
float ps_gp_cursor_brightness = 1.35f;
int ps_gp_inv_focus_frame = 1; // INV-REWORK-1: 1 = в инвентаре при геймпаде белая рамка вокруг цели вместо курсора
int ps_gp_pda_reticle_size = 32; // FIX-LIVE3 п.13: размер метки карты КПК (px UI 1024x768), 20..64
int ps_gp_pda_reticle_shape = 0; // FIX-LIVE2 п.8: форма метки карты КПК: 0 = квадрат с прорезями, 1 = круг, 2 = крест
int ps_gp_pda_eye_mode = 0; // PDA1-E: 0 = квадрат по высоте (ClientToScreenScaledHeight), 1 = ClientToScreenScaled по обеим осям

namespace
{
	void PushRectFillPx(float x0, float y0, float x1, float y1, u32 color)
	{
		UIRender->PushPoint(x0, y0, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(x1, y0, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(x1, y1, 0.0f, color, 0.0f, 0.0f);

		UIRender->PushPoint(x0, y0, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(x1, y1, 0.0f, color, 0.0f, 0.0f);
		UIRender->PushPoint(x0, y1, 0.0f, color, 0.0f, 0.0f);
	}
} // namespace

CUICursor::CUICursor()
	: m_static(NULL), m_b_use_win_cursor(false)
{
	bVisible = false;
	vPrevPos.set(0.0f, 0.0f);
	vPos.set(0.f, 0.f);
	InitInternal();
	Device.seqRender.Add(this, -3/*2*/);
	Device.seqResolutionChanged.Add(this);
}

//--------------------------------------------------------------------
CUICursor::~CUICursor()
{
	xr_delete(m_static);
	Device.seqRender.Remove(this);
	Device.seqResolutionChanged.Remove(this);
}

void CUICursor::OnScreenResolutionChanged()
{
	xr_delete(m_static);
	InitInternal();
}

void CUICursor::InitInternal()
{
	m_static = xr_new<CUIStatic>();
	m_static->InitTextureEx("ui\\ui_ani_cursor", "hud\\cursor");
	Frect rect;
	rect.set(0.0f, 0.0f, 40.0f, 40.0f);
	m_static->SetTextureRect(rect);
	Fvector2 sz;
	sz.set(rect.rb);
	sz.x *= UI().get_current_kx();

	m_static->SetWndSize(sz);
	m_static->SetStretchTexture(true);

	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	m_bCursorGlassChecked = false;
	m_bCursorGlassAvailable = false;
	m_shaderCursorGlass = ui_shader();

	u32 screen_size_x = GetSystemMetrics(SM_CXSCREEN);
	u32 screen_size_y = GetSystemMetrics(SM_CYSCREEN);
	m_b_use_win_cursor = (screen_size_y >= Device.dwHeight && screen_size_x >= Device.dwWidth);
}

CUICursor::ECursorVisualMode CUICursor::EvaluateVisualMode(bool isGamepadActiveSource, bool isHovered, bool isPressed)
{
	if (!isGamepadActiveSource)
		return ECursorVisualMode::StandardMouse;
	if (isPressed)
		return ECursorVisualMode::GamepadPressed;
	if (isHovered)
		return ECursorVisualMode::GamepadHover;
	return ECursorVisualMode::GamepadIdle;
}

CUICursor::ECursorVisualMode CUICursor::GetCurrentVisualMode() const
{
	const bool bGpActive = (pInput != nullptr) && pInput->IsGamepadActiveSource();
	if (!bGpActive)
		return ECursorVisualMode::StandardMouse;

	const auto& disp = CGamepadContextDispatcher::Instance();
	const bool bHovered = disp.IsCursorOverInteractiveTarget();
	const bool bPressed = disp.IsButtonPressed(CInput::GP_BTN_SOUTH) || disp.IsButtonHeld(CInput::GP_BTN_SOUTH) ||
	                      disp.IsButtonPressed(CGamepadContextDispatcher::VBTN_RT) || disp.IsButtonHeld(CGamepadContextDispatcher::VBTN_RT) ||
	                      (disp.IsGamepadMouseMode() && (disp.IsButtonPressed(CInput::GP_BTN_WEST) || disp.IsButtonHeld(CInput::GP_BTN_WEST)));
	return EvaluateVisualMode(true, bHovered, bPressed);
}

void CUICursor::RenderGamepadCursor()
{
	if (!m_bCursorGlassChecked)
	{
		m_bCursorGlassChecked = true;
		m_bCursorGlassAvailable = false;
		if (FS.exist("$game_textures$", "ui\\ui_gp_cursor_glass.dds"))
		{
			m_shaderCursorGlass->create("hud\\default", "ui\\ui_gp_cursor_glass");
			m_bCursorGlassAvailable = m_shaderCursorGlass->inited();
		}
	}

	const ECursorVisualMode mode = GetCurrentVisualMode();
	const bool bHovered = (mode == ECursorVisualMode::GamepadHover);
	const bool bPressed = (mode == ECursorVisualMode::GamepadPressed);
	const int stateIdx = bPressed ? 2 : (bHovered ? 1 : 0);

	Fvector2 centerPx;
	UI().ClientToScreenScaled(centerPx, vPos.x, vPos.y);

	float scalePx = 1.0f;
	UI().ClientToScreenScaledHeight(scalePx);
	if (scalePx < 0.5f)
		scalePx = 1.0f;

	const float userSize = clampr(ps_gp_cursor_size, 0.5f, 3.0f);
	const float br = clampr(ps_gp_cursor_brightness, 0.5f, 2.0f);

	// CURSOR1: integer-snapped odd diameter so the logical center cell (14,14) of the 29x29 grid lands on (cx, cy) without blur
	const float rawDiamPx = 29.0f * scalePx * (userSize / 1.5f);
	int diamPx = _max(15, int(floorf(rawDiamPx + 0.5f)));
	if ((diamPx & 1) == 0)
		++diamPx;

	const float cx = floorf(centerPx.x + 0.5f);
	const float cy = floorf(centerPx.y + 0.5f);
	const float x0 = cx - float((diamPx - 1) / 2);
	const float y0 = cy - float((diamPx - 1) / 2);
	const float x1 = x0 + float(diamPx);
	const float y1 = y0 + float(diamPx);

	const float brScale = clampr(br / 1.35f, 0.55f, 1.15f);

	// 1. Primary path: crisp multi-tier emerald-glass atlas ui\ui_gp_cursor_glass.dds (512x256, mips=1, tiers 1x=29, 2x=58, 3x=87, 4x=116)
	if (m_bCursorGlassAvailable && m_shaderCursorGlass->inited())
	{
		float tx = 0.0f, ty = 0.0f, tw = 116.0f, th = 116.0f;
		if (diamPx <= 34)
		{
			tw = 29.0f;
			th = 29.0f;
			ty = 0.0f;
			tx = (stateIdx == 0) ? 384.0f : ((stateIdx == 1) ? 416.0f : 448.0f);
		}
		else if (diamPx <= 68)
		{
			tw = 58.0f;
			th = 58.0f;
			ty = 128.0f;
			tx = (stateIdx == 0) ? 288.0f : ((stateIdx == 1) ? 352.0f : 416.0f);
		}
		else if (diamPx <= 98)
		{
			tw = 87.0f;
			th = 87.0f;
			ty = 128.0f;
			tx = (stateIdx == 0) ? 0.0f : ((stateIdx == 1) ? 96.0f : 192.0f);
		}
		else
		{
			tw = 116.0f;
			th = 116.0f;
			ty = 0.0f;
			tx = (stateIdx == 0) ? 0.0f : ((stateIdx == 1) ? 128.0f : 256.0f);
		}

		const float u0 = tx / 512.0f;
		const float v0 = ty / 256.0f;
		const float u1 = (tx + tw) / 512.0f;
		const float v1 = (ty + th) / 256.0f;
		const u32 cBoost = (u32)clampr(iFloor(255.0f * _min(brScale, 1.0f) + 0.5f), 0, 255);
		const u32 col = color_argb(255, cBoost, cBoost, cBoost);

		UIRender->SetShader(*m_shaderCursorGlass);
		UIRender->StartPrimitive(6, IUIRender::ptTriList, UI().m_currentPointType);
		UIRender->PushPoint(x0, y0, 0.0f, col, u0, v0);
		UIRender->PushPoint(x1, y0, 0.0f, col, u1, v0);
		UIRender->PushPoint(x1, y1, 0.0f, col, u1, v1);
		UIRender->PushPoint(x0, y0, 0.0f, col, u0, v0);
		UIRender->PushPoint(x1, y1, 0.0f, col, u1, v1);
		UIRender->PushPoint(x0, y1, 0.0f, col, u0, v1);
		UIRender->FlushPrimitive();
		return;
	}

	// 2. Procedural fallback (when ui_gp_cursor_glass.dds is absent): stepped pixel-art emerald glass circle on a 29x29 integer grid
	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	auto scaleByte = [brScale](int c) -> u32 {
		return (u32)clampr(iFloor(float(c) * brScale + 0.5f), 0, 255);
	};

	const u32 colOutline = color_argb((u32)clampr(iFloor(210.0f * brScale + 0.5f), 0, 235), 14, 24, 18);
	const u32 colGlass = bPressed
	                         ? color_argb((u32)clampr(iFloor(205.0f * brScale + 0.5f), 0, 235), scaleByte(96), scaleByte(224), scaleByte(152))
	                         : (bHovered
	                                ? color_argb((u32)clampr(iFloor(190.0f * brScale + 0.5f), 0, 225), scaleByte(86), scaleByte(214), scaleByte(142))
	                                : color_argb((u32)clampr(iFloor(175.0f * brScale + 0.5f), 0, 215), scaleByte(78), scaleByte(202), scaleByte(134)));
	const u32 colSpecular = color_argb((u32)clampr(iFloor(240.0f * brScale + 0.5f), 0, 250), scaleByte(232), scaleByte(255), scaleByte(242));
	const u32 colDot = color_argb((u32)clampr(iFloor(250.0f * brScale + 0.5f), 0, 255), scaleByte(236), scaleByte(255), scaleByte(244));

	const float pxStep = _max(1.0f, floorf(float(diamPx) / 29.0f + 0.5f));
	const float gridX0 = floorf(cx - 14.5f * pxStep + 0.5f);
	const float gridY0 = floorf(cy - 14.5f * pxStep + 0.5f);

	const int rInnerOutMin2 = bPressed ? 52 : 70;
	const int rGlassMin2 = bPressed ? 70 : 92;
	const int rGlassMax2 = bPressed ? 118 : 145;
	const int rOuterOutMax2 = bPressed ? 148 : 180;
	const int rSpecMin2 = bPressed ? 80 : 104;
	const int rSpecMax2 = bPressed ? 114 : 140;

	auto sampleCellColor = [&](int gx, int gy) -> u32
	{
		const int dx = gx - 14;
		const int dy = gy - 14;
		const int d2 = dx * dx + dy * dy;
		if (d2 <= 1)
			return colDot;
		if (d2 <= 5)
			return colOutline;
		if (d2 >= rInnerOutMin2 && d2 < rGlassMin2)
			return colOutline;
		if (d2 >= rGlassMin2 && d2 < rGlassMax2)
		{
			if (dy <= -5 && dx <= 1 && dx >= -10 && (dx - dy) <= 4 && d2 >= rSpecMin2 && d2 <= rSpecMax2)
				return colSpecular;
			return colGlass;
		}
		if (d2 >= rGlassMax2 && d2 <= rOuterOutMax2)
			return colOutline;
		return 0u;
	};

	UIRender->SetShader(*m_shaderGamepad);
	UIRender->StartPrimitive(768, IUIRender::ptTriList, UI().m_currentPointType);
	for (int gy = 0; gy < 29; ++gy)
	{
		const float ry0 = gridY0 + float(gy) * pxStep;
		const float ry1 = ry0 + pxStep;
		int gx = 0;
		while (gx < 29)
		{
			const u32 c = sampleCellColor(gx, gy);
			if (c == 0u)
			{
				++gx;
				continue;
			}
			int runEnd = gx + 1;
			while (runEnd < 29 && sampleCellColor(runEnd, gy) == c)
				++runEnd;
			PushRectFillPx(gridX0 + float(gx) * pxStep, ry0, gridX0 + float(runEnd) * pxStep, ry1, c);
			gx = runEnd;
		}
	}
	UIRender->FlushPrimitive();
}

void CUICursor::RenderMouseModeHintBar()
{
	LPCSTR hintText = CGamepadContextDispatcher::Instance().GetMouseModeHintBarText();
	if (!hintText || !hintText[0])
		return;

	CGameFont* font = UI().Font().pFontLetterica16Russian;
	if (!font)
		font = UI().Font().pFontMedium;
	if (!font)
		return;

	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	// UI y = 728..756 out of 768, horizontally centered on the screen and clamped inside [16 .. dwWidth - 16] (16:9 & 21:9 safe)
	Fvector2 barTopPx, barBotPx;
	UI().ClientToScreenScaled(barTopPx, 512.0f, 728.0f);
	UI().ClientToScreenScaled(barBotPx, 512.0f, 756.0f);

	const float screenW = (Device.dwWidth > 0) ? float(Device.dwWidth) : 1024.0f;
	const float barY0 = floorf(barTopPx.y + 0.5f);
	const float barY1 = _max(barY0 + 20.0f, floorf(barBotPx.y + 0.5f));
	const float barCenterY = floorf((barY0 + barY1) * 0.5f + 0.5f);
	const float cellH = _max(12.0f, font->CurrentHeight_());
	// gp_font_metrics.ltx: (cap_top + cap_bot) / 2 == 0.50 * cell_h for Letterica16 across 800 / 1024 / 1600
	const float textY = floorf(barCenterY - cellH * 0.5f + 0.5f);

	struct SHintToken
	{
		bool isBadge;
		char text[96];
		float widthPx;
	};
	SHintToken tokens[16];
	int tokenCount = 0;

	const float scaleH = _max(1.0f, barY1 - barY0) / 28.0f;
	const float badgePadX = floorf(6.0f * scaleH + 0.5f);
	const float badgeGapRight = floorf(5.0f * scaleH + 0.5f);
	const float groupGap = floorf(14.0f * scaleH + 0.5f);

	const char* p = hintText;
	bool sawAnyBadge = false;
	while (*p && tokenCount < 16)
	{
		while (*p == ' ')
			++p;
		if (!*p)
			break;

		if (*p == '[')
		{
			const char* close = strchr(p + 1, ']');
			if (close && close > p + 1 && (close - (p + 1)) < 32)
			{
				SHintToken& tk = tokens[tokenCount++];
				tk.isBadge = true;
				const size_t len = size_t(close - (p + 1));
				memcpy(tk.text, p + 1, len);
				tk.text[len] = '\0';
				tk.widthPx = ceilf(font->SizeOf_(tk.text)) + badgePadX * 2.0f;
				sawAnyBadge = true;
				p = close + 1;
				continue;
			}
		}

		// Read label up to the next '[' or 2+ spaces separator
		const char* start = p;
		while (*p && *p != '[')
		{
			if (p[0] == ' ' && p[1] == ' ')
				break;
			++p;
		}
		size_t len = size_t(p - start);
		while (len > 0 && start[len - 1] == ' ')
			--len;
		if (len > 0)
		{
			if (len >= sizeof(tokens[0].text))
				len = sizeof(tokens[0].text) - 1;
			SHintToken& tk = tokens[tokenCount++];
			tk.isBadge = false;
			memcpy(tk.text, start, len);
			tk.text[len] = '\0';
			tk.widthPx = ceilf(font->SizeOf_(tk.text));
		}
	}

	const float padOuterX = floorf(16.0f * scaleH + 0.5f);
	const float maxBarW = _max(240.0f, screenW - 32.0f);

	float contentW = 0.0f;
	if (sawAnyBadge && tokenCount > 0)
	{
		for (int i = 0; i < tokenCount; ++i)
		{
			contentW += tokens[i].widthPx;
			if (i + 1 < tokenCount)
				contentW += tokens[i].isBadge ? badgeGapRight : groupGap;
		}
	}
	else
	{
		contentW = ceilf(font->SizeOf_(hintText));
	}

	const bool fitBadges = sawAnyBadge && tokenCount > 0 && (contentW + padOuterX * 2.0f <= maxBarW);
	if (!fitBadges)
		contentW = _min(ceilf(font->SizeOf_(hintText)), maxBarW - padOuterX * 2.0f);

	const float barW = _min(maxBarW, contentW + padOuterX * 2.0f);
	const float barX0 = floorf((screenW - barW) * 0.5f + 0.5f);
	const float barX1 = barX0 + barW;

	const u32 colBackdrop = color_argb(216, 18, 22, 20);
	const u32 colTopLine = color_argb(150, 92, 168, 122);
	const u32 colBadgeBg = color_argb(232, 30, 44, 36);
	const u32 colBadgeBorder = color_argb(175, 92, 168, 122);
	const u32 colBadgeText = color_argb(250, 236, 252, 242);
	const u32 colLabelText = color_argb(242, 226, 234, 228);

	const float badgeH = _min(barY1 - barY0 - 6.0f, _max(16.0f, floorf(cellH * 0.86f + 0.5f)));
	const float badgeY0 = floorf(barCenterY - badgeH * 0.5f + 0.5f);
	const float badgeY1 = badgeY0 + badgeH;

	UIRender->SetShader(*m_shaderGamepad);
	UIRender->StartPrimitive(12 + (fitBadges ? tokenCount * 30 : 0), IUIRender::ptTriList, UI().m_currentPointType);
	PushRectFillPx(barX0, barY0, barX1, barY1, colBackdrop);
	PushRectFillPx(barX0, barY0, barX1, barY0 + 1.0f, colTopLine);

	if (fitBadges)
	{
		float curX = floorf(barX0 + (barW - contentW) * 0.5f + 0.5f);
		for (int i = 0; i < tokenCount; ++i)
		{
			const SHintToken& tk = tokens[i];
			if (tk.isBadge)
			{
				const float bx0 = curX;
				const float bx1 = curX + tk.widthPx;
				PushRectFillPx(bx0, badgeY0, bx1, badgeY1, colBadgeBg);
				PushRectFillPx(bx0, badgeY0, bx1, badgeY0 + 1.0f, colBadgeBorder);
				PushRectFillPx(bx0, badgeY1 - 1.0f, bx1, badgeY1, colBadgeBorder);
				PushRectFillPx(bx0, badgeY0 + 1.0f, bx0 + 1.0f, badgeY1 - 1.0f, colBadgeBorder);
				PushRectFillPx(bx1 - 1.0f, badgeY0 + 1.0f, bx1, badgeY1 - 1.0f, colBadgeBorder);
			}
			curX += tk.widthPx + ((i + 1 < tokenCount) ? (tk.isBadge ? badgeGapRight : groupGap) : 0.0f);
		}
	}
	UIRender->FlushPrimitive();

	const CGameFont::EAligment prevAlign = CGameFont::alLeft;
	(void)prevAlign;
	if (fitBadges)
	{
		float curX = floorf(barX0 + (barW - contentW) * 0.5f + 0.5f);
		for (int i = 0; i < tokenCount; ++i)
		{
			const SHintToken& tk = tokens[i];
			if (tk.isBadge)
			{
				font->SetAligment(CGameFont::alCenter);
				font->SetColor(colBadgeText);
				font->Out(floorf(curX + tk.widthPx * 0.5f + 0.5f), textY, "%s", tk.text);
			}
			else
			{
				font->SetAligment(CGameFont::alLeft);
				font->SetColor(colLabelText);
				font->Out(curX, textY, "%s", tk.text);
			}
			curX += tk.widthPx + ((i + 1 < tokenCount) ? (tk.isBadge ? badgeGapRight : groupGap) : 0.0f);
		}
	}
	else
	{
		font->SetAligment(CGameFont::alCenter);
		font->SetColor(colLabelText);
		font->Out(floorf((barX0 + barX1) * 0.5f + 0.5f), textY, "%s", hintText);
	}
	font->SetAligment(CGameFont::alLeft);
	font->OnRender();
}

// MENU-FOCUS-1 (docs/ui/menu-focus/BRIEF.md, эскиз docs/ui/menu-focus/sketch/sketch-v1.png, утверждён Мишей 2026-11-10):
// в меню (контекст MainMenu: главное меню, Настройки, MCM, загрузка/сохранение) при геймпаде курсор не рисуется.
// Вместо ромба и золотых скобок — тонкая белая рамка по прямоугольнику выбранной цели + заливка ~15% белого.
// Толщина 2 px задана для экрана высотой 1080 и масштабируется по Device.dwHeight; обводка идёт ровно по краю элемента без отступа и заливки, цвет 60% белого.
// Пункты самого главного меню рамку не получают: там остаётся ванильная «лупа» CUIMMShniaga (решение 6 брифа).
namespace
{
void DrawMenuFocusEllipse(float x0, float y0, float x1, float y1, float thickness, u32 color)
{
	constexpr int segments = 48;
	const float cx = (x0 + x1) * 0.5f;
	const float cy = (y0 + y1) * 0.5f;
	const float rx = (x1 - x0) * 0.5f;
	const float ry = (y1 - y0) * 0.5f;
	const float ix = _max(0.0f, rx - thickness);
	const float iy = _max(0.0f, ry - thickness);
	UIRender->StartPrimitive(segments * 6, IUIRender::ptTriList, UI().m_currentPointType);
	for (int i = 0; i < segments; ++i)
	{
		const float a = PI_MUL_2 * i / segments;
		const float b = PI_MUL_2 * (i + 1) / segments;
		const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
		UIRender->PushPoint(cx + rx * ca, cy + ry * sa, 0, color, 0, 0);
		UIRender->PushPoint(cx + rx * cb, cy + ry * sb, 0, color, 0, 0);
		UIRender->PushPoint(cx + ix * ca, cy + iy * sa, 0, color, 0, 0);
		UIRender->PushPoint(cx + rx * cb, cy + ry * sb, 0, color, 0, 0);
		UIRender->PushPoint(cx + ix * cb, cy + iy * sb, 0, color, 0, 0);
		UIRender->PushPoint(cx + ix * ca, cy + iy * sa, 0, color, 0, 0);
	}
	UIRender->FlushPrimitive();
}

// MENU-FOCUS P15: the same mark travels between tab columns and morphs into the row caret.
constexpr u32 MenuFocusMoveMs = 150;
constexpr u32 MenuFocusBreathMs = 3200;

struct SMenuFocusMark
{
	Frect rect;
	float fill; // 0 = rectangular outline, 1 = solid caret
};

struct SMenuFocusMotion
{
	const void* root = nullptr;
	const void* panel = nullptr;
	const void* key = nullptr;
	float gap = 0.0f;
	SMenuFocusMark target{};
	SMenuFocusMark offset{};
	u32 width = 0, height = 0;
	u32 lastSeen = 0, moveStart = 0, breatheStart = 0;
	bool moving = false;
	bool sticky = false;

	SMenuFocusMark Sample(u32 now) const
	{
		if (!moving || now - moveStart >= MenuFocusMoveMs)
			return target;

		const float t = float(now - moveStart) / MenuFocusMoveMs;
		const float u = t - 1.0f;
		const float ease = sticky ? 1.0f + 2.2f * u * u * u + 1.2f * u * u
			: t * t * (3.0f - 2.0f * t);
		const float weight = 1.0f - ease;
		SMenuFocusMark mark;
		mark.rect.set(target.rect.x1 + weight * offset.rect.x1, target.rect.y1 + weight * offset.rect.y1,
			target.rect.x2 + weight * offset.rect.x2, target.rect.y2 + weight * offset.rect.y2);
		mark.fill = target.fill + weight * offset.fill;
		return mark;
	}
};

SMenuFocusMotion g_menuFocusMotion;

using MenuFocusDraw = CGamepadContextDispatcher::SMenuFocusDraw;

void DrawMenuFocusMark(const SMenuFocusMark& mark, float brightness, float linePx, float alpha = 1.0f)
{
	const auto& r = mark.rect;
	const float x0 = floorf(r.x1 + mark.fill * 0.5f);
	const float y0 = floorf(r.y1 + mark.fill * 0.5f);
	const float x1 = floorf(r.x2 + 0.5f);
	const float y1 = floorf(r.y2 + 0.5f);
	const float line = _min(linePx, _min(x1 - x0, y1 - y0) * 0.5f);
	const bool outline = mark.fill < 1.0f;
	const bool fill = mark.fill > 0.0f;
	UIRender->StartPrimitive((outline ? 24 : 0) + (fill ? 6 : 0), IUIRender::ptTriList, UI().m_currentPointType);
	if (fill)
	{
		const u32 color = color_argb(u32(mark.fill * brightness * alpha * 255.0f + 0.5f), 255, 255, 255);
		PushRectFillPx(x0, y0, x1, y1, color);
	}
	if (outline)
	{
		const u32 color = color_argb(u32((1.0f - mark.fill) * alpha * 255.0f + 0.5f), 153, 153, 153);
		PushRectFillPx(x0, y0, x1, y0 + line, color);
		PushRectFillPx(x0, y1 - line, x1, y1, color);
		PushRectFillPx(x0, y0 + line, x0 + line, y1 - line, color);
		PushRectFillPx(x1 - line, y0 + line, x1, y1 - line, color);
	}
	UIRender->FlushPrimitive();
}

void DrawAnimatedMenuFocus(const MenuFocusDraw& focus, const SMenuFocusMark& target, float linePx)
{
	auto& motion = g_menuFocusMotion;
	const u32 now = Device.dwTimeContinual;
	const bool continuous = focus.transitionRoot && focus.transitionRoot == motion.root &&
		motion.width == Device.dwWidth && motion.height == Device.dwHeight && now - motion.lastSeen <= 250;
	const bool changed = focus.transitionPanel != motion.panel || focus.caretKey != motion.key ||
		target.fill != motion.target.fill || focus.caretGapPx != motion.gap;

	if (!continuous)
	{
		motion.moving = false;
		motion.breatheStart = now;
	}
	else if (changed)
	{
		const bool bothCarets = target.fill == 1.0f && motion.target.fill == 1.0f;
		const bool betweenPanels = focus.transitionPanel && motion.panel && focus.transitionPanel != motion.panel;
		const bool switchingShape = target.fill != motion.target.fill;
		const bool inFlight = motion.moving && now - motion.moveStart < MenuFocusMoveMs;
		const auto from = motion.Sample(now);
		motion.moving = bothCarets || betweenPanels || switchingShape || inFlight;
		motion.sticky = bothCarets && from.fill == 1.0f;
		motion.offset.rect.set(from.rect.x1 - target.rect.x1, from.rect.y1 - target.rect.y1,
			from.rect.x2 - target.rect.x2, from.rect.y2 - target.rect.y2);
		motion.offset.fill = from.fill - target.fill;
		motion.moveStart = now;
		motion.breatheStart = now;
	}
	else if (motion.moving && now - motion.moveStart >= MenuFocusMoveMs)
	{
		motion.moving = false;
		motion.breatheStart = motion.moveStart + MenuFocusMoveMs;
	}

	motion.root = focus.transitionRoot;
	motion.panel = focus.transitionPanel;
	motion.key = focus.caretKey;
	motion.gap = focus.caretGapPx;
	motion.width = Device.dwWidth;
	motion.height = Device.dwHeight;
	motion.lastSeen = now;
	motion.target = target;

	const float phase = motion.moving ? 0.0f : float((now - motion.breatheStart) % MenuFocusBreathMs) / MenuFocusBreathMs;
	DrawMenuFocusMark(motion.Sample(now), 0.75f + 0.25f * cosf(PI_MUL_2 * phase), linePx);
}
}

void CUICursor::RenderGamepadMenuFocus()
{
	const auto& disp = CGamepadContextDispatcher::Instance();
	MenuFocusDraw focus;
	if (disp.IsMenuFocusNativeHighlightTarget() || !disp.GetMenuFocusDraw(focus))
	{
		g_menuFocusMotion.root = nullptr;
		return;
	}

	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	const float k = Device.dwHeight > 0 ? float(Device.dwHeight) / 1080.0f : 1.0f;
	const float linePx = _max(1.0f, floorf(2.0f * k + 0.5f));
	const Frect& rect = focus.frame;
	Fvector2 tl, br;
	UI().ClientToScreenScaled(tl, rect.x1, rect.y1);
	UI().ClientToScreenScaled(br, rect.x2, rect.y2);
	UIRender->SetShader(*m_shaderGamepad);

	SMenuFocusMark target{};
	target.rect.set(tl.x, tl.y, br.x, br.y);
	if (focus.shape == MenuFocusDraw::Shape::Ellipse)
	{
		g_menuFocusMotion.root = nullptr;
		DrawMenuFocusEllipse(tl.x, tl.y, br.x, br.y, linePx, color_argb(255, 153, 153, 153));
		return;
	}
	if (focus.shape == MenuFocusDraw::Shape::Caret)
	{
		const float caretScale = Device.dwHeight > 0 ? float(Device.dwHeight) / 1079.0f : 1.0f;
		const float w = _max(2.0f, floorf(3.0f * caretScale + 0.5f));
		const float h = _max(8.0f, floorf(26.0f * caretScale + 0.5f));
		const float left = tl.x - focus.caretGapPx * caretScale;
		target.rect.set(left, tl.y - h * 0.5f, left + w, tl.y + h * 0.5f);
		target.fill = 1.0f;
	}
	if (focus.transitionRoot)
	{
		DrawAnimatedMenuFocus(focus, target, linePx);
		return;
	}
	g_menuFocusMotion.root = nullptr;
	DrawMenuFocusMark(target, 1.0f, linePx);
}

// INV-REWORK-1: инвентарь/обыск/торговля/ремонт при геймпаде — белая рамка вокруг текущей цели
// (прямоугольник цели приходит из gp_bridge.cursor_query_target) вместо золотого курсора.
// INV-REWORK-2 (решения Миши 2026-11-11): золотого курсора в инвентаре нет совсем;
//  * при открытии/появлении рамка проявляется из прозрачной за InvFocusFadeInMs прямо на цели;
//  * переезд на новую цель — быстрее меню (InvFocusMoveMs), с «прилипанием» как в меню (sticky ease
//    SMenuFocusMotion: лёгкий проскок и возврат), размер плавно перетекает под новую цель;
//  * цели нет (раздел пуст) — рамка гаснет за InvFocusFadeOutMs.
namespace
{
constexpr u32 InvFocusMoveMs = 100;
constexpr u32 InvFocusFadeInMs = 150;
constexpr u32 InvFocusFadeOutMs = 120;

struct SInvFocusMotion
{
	Frect target{};
	Frect offset{};
	Frect last{};
	u32 moveStart = 0, fadeStart = 0, hideStart = 0, lastSeen = 0;
	u32 width = 0, height = 0, moveDuration = InvFocusMoveMs;
	bool moving = false;
	bool visible = false;
	bool hasLast = false;
	float lastAlpha = 0.0f;

	Frect Sample(u32 now) const
	{
		if (!moving || now - moveStart >= moveDuration)
			return target;
		const float t = float(now - moveStart) / moveDuration;
		const float u = t - 1.0f;
		const float ease = 1.0f + 2.2f * u * u * u + 1.2f * u * u;
		const float w = 1.0f - ease;
		Frect r;
		r.set(target.x1 + w * offset.x1, target.y1 + w * offset.y1, target.x2 + w * offset.x2, target.y2 + w * offset.y2);
		return r;
	}
};

SInvFocusMotion g_invFocusMotion;

float SmoothStep01(float t)
{
	clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
}

// INV-REWORK-4: «нельзя» — рамка инвентаря трясётся по горизонтали, как подсказка быстрого сохранения
// в меню паузы (ui_gp_pause.script shake_offset: 500 мс, 8 полупериодов, затухание). Время ставит
// Lua через game.gamepad_inv_focus_deny() в игровом потоке; рендер только читает число.
u32 g_gpInvFocusDenyAt = 0;
u32 g_gpInvFocusHoldStart = 0;
u32 g_gpInvFocusHoldFillMs = 800; // INV-REWORK-16: A hold fills in 700 ms after a 300 ms pause
constexpr u32 InvFocusDenyMs = 500;
// INV-REWORK-15 (Misha 2026-11-12, mockup docs/ui/mockups/inv-rework/view-hold variant 4 "corners"):
// View hold draws four growing corner brackets around the stats / stack panel (rect in UI units,
// set by the dispatcher from gp_inv_rework.view_hold_rect). After firing they stay full and fade out.
u32 g_gpInvViewHoldStart = 0;
u32 g_gpInvViewHoldDoneAt = 0;
Frect g_gpInvViewHoldRect = { 0, 0, 0, 0 };
constexpr u32 InvViewHoldFadeMs = 250;

// INV-REWORK-17 (Misha 2026-11-12): instead of corners - two vertical bars on the left and right
// sides of the panel, growing from the vertical centre to the top and bottom at once. The dispatcher
// sets the start after the same 300 ms pause as A; the fill takes g_gpInvViewHoldFillMs (700 ms).
u32 g_gpInvViewHoldFillMs = 400; // INV-REWORK-20: 700 - 300 мс

static void RenderViewHoldCorners(float linePx)
{
	const u32 now = Device.dwTimeContinual;
	float progress = 0.0f, alpha = 1.0f;
	if (g_gpInvViewHoldStart && now >= g_gpInvViewHoldStart)
	{
		const u32 fillMs = _max(1u, g_gpInvViewHoldFillMs);
		const int frame = int(std::min<u32>(fillMs, now - g_gpInvViewHoldStart) * 31 / fillMs);
		progress = float(frame) / 31.0f;
	}
	else if (g_gpInvViewHoldStart)
		return; // still in the pause before the animation
	else if (g_gpInvViewHoldDoneAt && now >= g_gpInvViewHoldDoneAt && now - g_gpInvViewHoldDoneAt < InvViewHoldFadeMs)
	{
		progress = 1.0f;
		alpha = 1.0f - SmoothStep01(float(now - g_gpInvViewHoldDoneAt) / InvViewHoldFadeMs);
	}
	else
	{
		g_gpInvViewHoldDoneAt = 0;
		return;
	}
	if (progress <= 0.0f || alpha <= 0.0f)
		return;
	Fvector2 tl, br;
	UI().ClientToScreenScaled(tl, g_gpInvViewHoldRect.x1, g_gpInvViewHoldRect.y1);
	UI().ClientToScreenScaled(br, g_gpInvViewHoldRect.x2, g_gpInvViewHoldRect.y2);
	const float w = br.x - tl.x, h = br.y - tl.y;
	if (w < 4.0f || h < 4.0f)
		return;
	const float line = _max(2.0f, linePx);
	const float cy = (tl.y + br.y) * 0.5f;
	const float half = h * 0.5f * progress;
	const u32 c = color_argb(u32(235.0f * alpha), 236, 236, 236);
	UIRender->StartPrimitive(12, IUIRender::ptTriList, UI().m_currentPointType);
	PushRectFillPx(tl.x, cy - half, tl.x + line, cy + half, c);
	PushRectFillPx(br.x - line, cy - half, br.x, cy + half, c);
	UIRender->FlushPrimitive();
}

// INV-REWORK-17 (Misha 2026-11-12): the same hold also runs a ring around the button badge in the
// footer (A "Use/Action", View "Section"), together with the frame/bars. Light-grey stroke clockwise
// from the top; when the hold fires the whole ring turns green (A) or white (View) and fades out.
// Rect in UI units from gp_inv_rework.hold_icon_rect; Start already includes the 300 ms pause.
u32 g_gpInvHoldIconStart = 0;
u32 g_gpInvHoldIconFillMs = 700;
u32 g_gpInvHoldIconDoneAt = 0;
int g_gpInvHoldIconKind = 0; // 0 = A (green at the end), 1 = View (white)
// INV-REWORK-27 (Misha 2026-11-13: "the fill is crooked and not on the circle itself; fill the outline, a bit
// grey and thinner"): 1 = the badge is a round Xbox icon - the ring lies exactly on the icon's own outline
// (atlas ui_gp_xbox_graphite_p12: outline at 3.5..7.5 px of the 128 px cell -> radius 0.88..0.945 of the
// half side); 0 = not round (LB/RB pill) - thin ring just outside the cell.
int g_gpInvHoldIconRound = 1;
constexpr float InvHoldRingOuterK = 0.945f;
Frect g_gpInvHoldIconRect = { 0, 0, 0, 0 };
constexpr u32 InvHoldIconFadeMs = 300;

static void RenderHoldIconRing(float linePx)
{
	const u32 now = Device.dwTimeContinual;
	if (g_gpInvHoldIconRect.x2 - g_gpInvHoldIconRect.x1 < 1.0f)
		return;
	float progress = 0.0f, alpha = 1.0f;
	bool done = false;
	if (g_gpInvHoldIconStart)
	{
		if (now < g_gpInvHoldIconStart)
			return;
		// INV-REWORK-27: smooth progress (was quantised to 31 frames -> jerky)
		const u32 fillMs = _max(1u, g_gpInvHoldIconFillMs);
		progress = float(std::min<u32>(fillMs, now - g_gpInvHoldIconStart)) / float(fillMs);
	}
	else if (g_gpInvHoldIconDoneAt && now >= g_gpInvHoldIconDoneAt && now - g_gpInvHoldIconDoneAt < InvHoldIconFadeMs)
	{
		progress = 1.0f;
		done = true;
		alpha = 1.0f - SmoothStep01(float(now - g_gpInvHoldIconDoneAt) / InvHoldIconFadeMs);
	}
	else
	{
		g_gpInvHoldIconDoneAt = 0;
		return;
	}
	if (progress <= 0.0f || alpha <= 0.0f)
		return;
	Fvector2 tl, br;
	UI().ClientToScreenScaled(tl, g_gpInvHoldIconRect.x1, g_gpInvHoldIconRect.y1);
	UI().ClientToScreenScaled(br, g_gpInvHoldIconRect.x2, g_gpInvHoldIconRect.y2);
	const float cx = (tl.x + br.x) * 0.5f, cy = (tl.y + br.y) * 0.5f;
	// INV-REWORK-27: thin stroke (1 px at 1080, linePx is 2 px there) on the icon's own outline
	const float line = _max(1.0f, floorf(linePx * 0.5f + 0.5f));
	const float half = _min(br.x - tl.x, br.y - tl.y) * 0.5f;
	const float r1 = g_gpInvHoldIconRound ? half * InvHoldRingOuterK : _max(br.x - tl.x, br.y - tl.y) * 0.5f + line;
	const float r0 = r1 - line;
	u32 c;
	if (!done)
		c = color_argb(u32(215.0f * alpha), 196, 196, 196); // INV-REWORK-27: light grey while filling
	else
		c = color_argb(u32(235.0f * alpha), 228, 228, 228); // fired: a little lighter, then fades
	// INV-REWORK-27b (Misha: "the ring is crooked"): a 1 px hard-edged triangle band looks jagged on a small
	// round icon. The band is drawn as three rings: a solid core r0..r1 plus 1 px feathers on both sides
	// (alpha -> 0), i.e. an anti-aliased stroke; 128 segments for a full circle.
	const u32 cz = c & 0x00FFFFFFu;
	const float rr[4] = { _max(0.0f, r0 - 1.0f), r0, r1, r1 + 1.0f };
	const u32 cc[4] = { cz, c, c, cz };
	constexpr int SegsFull = 128;
	const int n = _max(1, int(ceilf(SegsFull * progress)));
	const float sweep = PI_MUL_2 * progress;
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

void CUICursor::RenderGamepadInventoryFocus(bool pending)
{
	g_menuFocusMotion.root = nullptr;
	auto& m = g_invFocusMotion;
	const u32 now = Device.dwTimeContinual;
	const auto& disp = CGamepadContextDispatcher::Instance();
	const Fvector2 c = disp.GetCursorTargetCenter();
	const Fvector2 sz = disp.GetCursorTargetSize();
	// INV-REWORK-16 (Misha 2026-11-12): context menu rows keep only GAMMA's own row glow, no frame
	const bool onProp = disp.GetCursorTargetKind() && 0 == xr_strcmp(disp.GetCursorTargetKind(), "prop");
	const bool valid = !pending && !onProp && disp.IsCursorOverInteractiveTarget() && sz.x >= 2.0f && sz.y >= 2.0f;

	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	const float k = Device.dwHeight > 0 ? float(Device.dwHeight) / 1080.0f : 1.0f;
	const float linePx = _max(1.0f, floorf(2.0f * k + 0.5f));

	UIRender->SetShader(*m_shaderGamepad);
	RenderViewHoldCorners(linePx);
	if (!(psActorFlags.test(AF_3D_PDA) && disp.IsPdaInteractiveActive()))
		RenderHoldIconRing(linePx);

	if (!valid)
	{
		if (m.visible)
		{
			m.visible = false;
			m.hideStart = now;
			m.last = m.Sample(now);
			m.hasLast = true;
		}
		if (m.hasLast && now - m.hideStart < InvFocusFadeOutMs)
		{
			const float a = m.lastAlpha * (1.0f - SmoothStep01(float(now - m.hideStart) / InvFocusFadeOutMs));
			SMenuFocusMark mark{};
			mark.rect = m.last;
			mark.fill = 0.12f;
			UIRender->SetShader(*m_shaderGamepad);
			DrawMenuFocusMark(mark, 1.0f, linePx, a);
		}
		else
			m.hasLast = false;
		return;
	}

	Fvector2 tl, br;
	UI().ClientToScreenScaled(tl, c.x - sz.x * 0.5f, c.y - sz.y * 0.5f);
	UI().ClientToScreenScaled(br, c.x + sz.x * 0.5f, c.y + sz.y * 0.5f);
	Frect tgt;
	tgt.set(tl.x, tl.y, br.x, br.y);

	const bool workshop = disp.IsWorkshopModalActive();
	const bool pda = disp.IsPdaInteractiveActive();
	const bool brieflyHidden = (workshop || pda) && m.hasLast && now - m.hideStart < InvFocusFadeOutMs;
	const bool continuous = (m.visible || brieflyHidden) && m.width == Device.dwWidth &&
	                        m.height == Device.dwHeight && now - m.lastSeen <= 250;
	if (!continuous)
	{
		m.visible = true;
		m.moving = false;
		m.fadeStart = now;
		m.target = tgt;
	}
	else if (brieflyHidden || _abs(tgt.x1 - m.target.x1) > 0.5f || _abs(tgt.y1 - m.target.y1) > 0.5f ||
	         _abs(tgt.x2 - m.target.x2) > 0.5f || _abs(tgt.y2 - m.target.y2) > 0.5f)
	{
		const Frect from = brieflyHidden ? m.last : m.Sample(now);
		const float dx = _abs((from.x1 + from.x2 - tgt.x1 - tgt.x2) * 0.5f);
		const float dy = _abs((from.y1 + from.y2 - tgt.y1 - tgt.y2) * 0.5f);
		// A change of workshop or PDA panels should read as a visible 300 ms move across the window.
		m.moveDuration = (workshop || pda) && (dx >= 0.10f * Device.dwWidth || dy >= 0.14f * Device.dwHeight)
			? 2 * MenuFocusMoveMs : InvFocusMoveMs;
		m.visible = true;
		m.offset.set(from.x1 - tgt.x1, from.y1 - tgt.y1, from.x2 - tgt.x2, from.y2 - tgt.y2);
		m.target = tgt;
		m.moving = true;
		m.moveStart = now;
	}
	else if (m.moving && now - m.moveStart >= m.moveDuration)
		m.moving = false;
	m.lastSeen = now;
	m.width = Device.dwWidth;
	m.height = Device.dwHeight;
	m.hasLast = false;

	const float alpha = SmoothStep01(float(now - m.fadeStart) / InvFocusFadeInMs);
	m.lastAlpha = alpha;

	// Макет Миши 2026-11-11: «рамка + заливка» — серо-белая рамка как в меню и слабая белая заливка.
	SMenuFocusMark mark{};
	mark.rect = m.Sample(now);
	mark.fill = 0.12f;
	const u32 denyAt = g_gpInvFocusDenyAt;
	if (denyAt && now >= denyAt && now - denyAt < InvFocusDenyMs)
	{
		const float t = float(now - denyAt) / InvFocusDenyMs;
		const float dx = 6.0f * k * (1.0f - t) * sinf(t * PI * 8.0f);
		mark.rect.x1 += dx;
		mark.rect.x2 += dx;
	}
	const u32 phaseMs = now % MenuFocusBreathMs;
	const float breath = m.moving ? 1.0f : 0.75f + 0.25f * cosf(PI_MUL_2 * float(phaseMs) / MenuFocusBreathMs);
	UIRender->SetShader(*m_shaderGamepad);
	DrawMenuFocusMark(mark, breath, linePx, alpha);
	if (g_gpInvFocusHoldStart && now >= g_gpInvFocusHoldStart)
	{
		// Match the menu's 800 ms / 31 frames. Fill the A focus outline green clockwise.
		const u32 fillMs = _max(1u, g_gpInvFocusHoldFillMs);
		const int frame = int(std::min<u32>(fillMs, now - g_gpInvFocusHoldStart) * 31 / fillMs);
		const float progress = float(frame) / 31.0f;
		const auto& r = mark.rect;
		const float w = _max(0.0f, r.x2 - r.x1), h = _max(0.0f, r.y2 - r.y1);
		const float perimeter = 2.0f * (w + h);
		float remaining = perimeter * progress;
		const float line = _max(2.0f, linePx);
		const u32 green = color_argb(235, 245, 245, 245); // INV-REWORK-23: white (name kept)
		UIRender->StartPrimitive(24, IUIRender::ptTriList, UI().m_currentPointType);
		const float top = _min(remaining, w); remaining -= top;
		if (top > 0) PushRectFillPx(r.x1, r.y1, r.x1 + top, r.y1 + line, green);
		const float right = _min(remaining, h); remaining -= right;
		if (right > 0) PushRectFillPx(r.x2 - line, r.y1, r.x2, r.y1 + right, green);
		const float bottom = _min(remaining, w); remaining -= bottom;
		if (bottom > 0) PushRectFillPx(r.x2 - bottom, r.y2 - line, r.x2, r.y2, green);
		const float left = _min(remaining, h);
		if (left > 0) PushRectFillPx(r.x1, r.y2 - left, r.x1 + line, r.y2, green);
		UIRender->FlushPrimitive();
	}
}

bool CUICursor::IsMenuFocusTransitionActive(const void* root, u32 now) const
{
	const auto& motion = g_menuFocusMotion;
	return root && root == motion.root && motion.moving &&
		now - motion.moveStart < MenuFocusMoveMs;
}

void CUICursor::RenderPdaMapEye()
{
	if (!m_shaderGamepad->inited())
		m_shaderGamepad->create("hud\\crosshair");

	const auto& disp = CGamepadContextDispatcher::Instance();
	const u32 now = Device.dwTimeContinual;
	const Fvector2 curPos = GetCursorPosition();

	const bool hovered = disp.IsPdaEyeSpotHit();
	const Fvector2 spotCenter = disp.GetPdaEyeSpotCenter();
	const Fvector2 spotSize = disp.GetPdaEyeSpotSize();

	// FIX-LIVE2 п.7: метка неподвижна и не меняет размер; над значком задания только подсвечивается.
	(void)spotCenter;
	(void)spotSize;
	const float tgtW = float(clampr(ps_gp_pda_reticle_size, 20, 64)); // FIX-LIVE3 п.13
	const float tgtH = tgtW;
	const float tgtFill = hovered ? 0.28f : (31.0f / 255.0f);
	const float tgtDot = 1.0f;

	struct SPdaEyeMorph
	{
		bool inited = false;
		bool lastHovered = false;
		u32 lastSeen = 0;
		u32 moveStart = 0;
		bool moving = false;
		float targetW = 32.0f, targetH = 32.0f, targetFill = 31.0f / 255.0f, targetDot = 1.0f;
		float offW = 0.0f, offH = 0.0f, offFill = 0.0f, offDot = 0.0f;

		void Sample(u32 tNow, float& outW, float& outH, float& outFill, float& outDot) const
		{
			if (!moving || tNow - moveStart >= InvFocusMoveMs)
			{
				outW = targetW;
				outH = targetH;
				outFill = targetFill;
				outDot = targetDot;
				return;
			}
			const float t = float(tNow - moveStart) / float(InvFocusMoveMs);
			const float u = t - 1.0f;
			const float ease = 1.0f + 2.2f * u * u * u + 1.2f * u * u;
			const float w = 1.0f - clampr(ease, 0.0f, 1.25f);
			outW = _max(12.0f, targetW + w * offW);
			outH = _max(12.0f, targetH + w * offH);
			outFill = clampr(targetFill + w * offFill, 0.0f, 1.0f);
			outDot = clampr(targetDot + w * offDot, 0.0f, 1.0f);
		}
	};
	static SPdaEyeMorph s_eye;

	if (!s_eye.inited || now - s_eye.lastSeen > 250)
	{
		s_eye.inited = true;
		s_eye.lastHovered = hovered;
		s_eye.moving = false;
		s_eye.targetW = tgtW;
		s_eye.targetH = tgtH;
		s_eye.targetFill = tgtFill;
		s_eye.targetDot = tgtDot;
	}
	else if (hovered != s_eye.lastHovered || _abs(tgtW - s_eye.targetW) > 0.5f || _abs(tgtH - s_eye.targetH) > 0.5f)
	{
		float curW, curH, curFill, curDot;
		s_eye.Sample(now, curW, curH, curFill, curDot);
		s_eye.offW = curW - tgtW;
		s_eye.offH = curH - tgtH;
		s_eye.offFill = curFill - tgtFill;
		s_eye.offDot = curDot - tgtDot;
		s_eye.targetW = tgtW;
		s_eye.targetH = tgtH;
		s_eye.targetFill = tgtFill;
		s_eye.targetDot = tgtDot;
		s_eye.lastHovered = hovered;
		s_eye.moveStart = now;
		s_eye.moving = true;
	}
	else if (s_eye.moving && now - s_eye.moveStart >= InvFocusMoveMs)
	{
		s_eye.moving = false;
	}
	s_eye.lastSeen = now;

	float drawW, drawH, drawFill, drawDot;
	s_eye.Sample(now, drawW, drawH, drawFill, drawDot);
	const float uiCx = curPos.x;
	const float uiCy = curPos.y;

	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	float cxPx = 0.0f, cyPx = 0.0f;
	float lineX = 2.0f, lineY = 2.0f, dotW = 3.0f, dotH = 3.0f;

	if (ps_gp_pda_eye_mode == 1)
	{
		Fvector2 tlPx, brPx, cPx, lPx, dPx;
		UI().ClientToScreenScaled(tlPx, uiCx - drawW * 0.5f, uiCy - drawH * 0.5f);
		UI().ClientToScreenScaled(brPx, uiCx + drawW * 0.5f, uiCy + drawH * 0.5f);
		UI().ClientToScreenScaled(cPx, uiCx, uiCy);
		UI().ClientToScreenScaled(lPx, 2.0f, 2.0f);
		UI().ClientToScreenScaled(dPx, 3.0f, 3.0f);
		cxPx = cPx.x;
		cyPx = cPx.y;
		x0 = floorf(tlPx.x + 0.5f);
		y0 = floorf(tlPx.y + 0.5f);
		x1 = floorf(brPx.x + 0.5f);
		y1 = floorf(brPx.y + 0.5f);
		lineX = _max(1.0f, floorf(lPx.x + 0.5f));
		lineY = _max(1.0f, floorf(lPx.y + 0.5f));
		dotW = _max(1.0f, floorf(dPx.x + 0.5f));
		dotH = _max(1.0f, floorf(dPx.y + 0.5f));
	}
	else
	{
		Fvector2 cPx;
		UI().ClientToScreenScaled(cPx, uiCx, uiCy);
		float hScale = 1.0f;
		UI().ClientToScreenScaledHeight(hScale);
		cxPx = cPx.x;
		cyPx = cPx.y;
		const float wPx = drawW * hScale;
		const float hPx = drawH * hScale;
		x0 = floorf(cxPx - wPx * 0.5f + 0.5f);
		y0 = floorf(cyPx - hPx * 0.5f + 0.5f);
		x1 = floorf(cxPx + wPx * 0.5f + 0.5f);
		y1 = floorf(cyPx + hPx * 0.5f + 0.5f);
		lineX = lineY = _max(1.0f, floorf(2.0f * hScale + 0.5f));
		dotW = dotH = _max(1.0f, floorf(3.0f * hScale + 0.5f));
	}

	const float wPx = x1 - x0;
	const float hPx = y1 - y0;
	if (wPx < 4.0f || hPx < 4.0f)
		return;

	const float armX = _max(lineX, floorf(wPx / 3.0f + 0.5f));
	const float armY = _max(lineY, floorf(hPx / 3.0f + 0.5f));

	UIRender->SetShader(*m_shaderGamepad);
	if (!(psActorFlags.test(AF_3D_PDA) && disp.IsPdaInteractiveActive()))
	{
		const float k = Device.dwHeight > 0 ? float(Device.dwHeight) / 1080.0f : 1.0f;
		const float linePx = _max(1.0f, floorf(2.0f * k + 0.5f));
		RenderHoldIconRing(linePx);
	}

	UIRender->StartPrimitive(256, IUIRender::ptTriList, UI().m_currentPointType);

	const u32 colLine = color_argb(255, 255, 255, 255);
	const int shape = ps_gp_pda_reticle_shape;
	if (shape == 1)
	{
		// Круг: кольцо из 28 сегментов той же толщины, подсветка — внутренний квадрат-заливка в круге
		const float rx = wPx * 0.5f, ry = hPx * 0.5f;
		if (drawFill > 0.005f)
		{
			const u32 colFill = color_argb(u32(drawFill * 255.0f + 0.5f), 255, 255, 255);
			PushRectFillPx(floorf(cxPx - rx * 0.7f + 0.5f), floorf(cyPx - ry * 0.7f + 0.5f),
			               floorf(cxPx + rx * 0.7f + 0.5f), floorf(cyPx + ry * 0.7f + 0.5f), colFill);
		}
		for (int i = 0; i < 28; ++i)
		{
			const float a = float(i) * (PI_MUL_2 / 28.0f);
			const float px = floorf(cxPx + _cos(a) * (rx - lineX * 0.5f) - lineX * 0.5f + 0.5f);
			const float py = floorf(cyPx + _sin(a) * (ry - lineY * 0.5f) - lineY * 0.5f + 0.5f);
			PushRectFillPx(px, py, px + lineX + 1.0f, py + lineY + 1.0f, colLine);
		}
	}
	else if (shape == 2)
	{
		// Крест: четыре луча с зазором у центра, подсветка — квадрат-заливка
		if (drawFill > 0.005f)
		{
			const u32 colFill = color_argb(u32(drawFill * 255.0f + 0.5f), 255, 255, 255);
			PushRectFillPx(x0 + armX, y0 + armY, x1 - armX, y1 - armY, colFill);
		}
		const float gx = floorf(wPx / 6.0f + 0.5f), gy = floorf(hPx / 6.0f + 0.5f);
		const float hx = floorf(cxPx - lineX * 0.5f + 0.5f), hy = floorf(cyPx - lineY * 0.5f + 0.5f);
		PushRectFillPx(x0, hy, cxPx - gx, hy + lineY, colLine);
		PushRectFillPx(cxPx + gx, hy, x1, hy + lineY, colLine);
		PushRectFillPx(hx, y0, hx + lineX, cyPx - gy, colLine);
		PushRectFillPx(hx, cyPx + gy, hx + lineX, y1, colLine);
	}
	else
	{
	if (drawFill > 0.005f)
	{
		const u32 colFill = color_argb(u32(drawFill * 255.0f + 0.5f), 255, 255, 255);
		PushRectFillPx(x0, y0, x1, y1, colFill);
	}
	PushRectFillPx(x0, y0, x0 + armX, y0 + lineY, colLine);
	PushRectFillPx(x0, y0 + lineY, x0 + lineX, y0 + armY, colLine);
	PushRectFillPx(x1 - armX, y0, x1, y0 + lineY, colLine);
	PushRectFillPx(x1 - lineX, y0 + lineY, x1, y0 + armY, colLine);
	PushRectFillPx(x0, y1 - lineY, x0 + armX, y1, colLine);
	PushRectFillPx(x0, y1 - armY, x0 + lineX, y1 - lineY, colLine);
	PushRectFillPx(x1 - armX, y1 - lineY, x1, y1, colLine);
	PushRectFillPx(x1 - lineX, y1 - armY, x1, y1 - lineY, colLine);
	}

	if (drawDot > 0.01f)
	{
		const u32 colDot = color_argb(u32(drawDot * 255.0f + 0.5f), 255, 255, 255);
		const float dx0 = floorf(cxPx - dotW * 0.5f + 0.5f);
		const float dy0 = floorf(cyPx - dotH * 0.5f + 0.5f);
		PushRectFillPx(dx0, dy0, dx0 + dotW, dy0 + dotH, colDot);
	}

	UIRender->FlushPrimitive();
}

//--------------------------------------------------------------------
u32 last_render_frame = 0;

void CUICursor::OnRender()
{
	g_btnHint->OnRender();
	g_statHint->OnRender();

	if (!IsVisible() && !(pInput && pInput->IsGamepadActiveSource() && CGamepadContextDispatcher::Instance().IsGamepadMouseMode()))
	{
		g_menuFocusMotion.root = nullptr;
		return;
	}
	// CR1-10: evaluate 3D-PDA second pass before DEBUG cursor printout (no single-pass VERIFY assert)
	u32 curFrame = Device.dwFrame;
	if (curFrame == last_render_frame)
	{
		// PDA1-E: в 3D-КПК первый проход OnRender идёт в текстуру rtPDA, а второй — поверх плоского 2D HUD.
		// Кольцо удержания на нижней 2D-плашке рисуем во втором проходе, чтобы оно не наклонялось вместе с экраном КПК.
		if (psActorFlags.test(AF_3D_PDA) &&
		    CGamepadContextDispatcher::Instance().IsPdaInteractiveActive() &&
		    pInput && pInput->IsGamepadActiveSource())
		{
			if (!m_shaderGamepad->inited())
				m_shaderGamepad->create("hud\\crosshair");
			const float k = Device.dwHeight > 0 ? float(Device.dwHeight) / 1080.0f : 1.0f;
			const float linePx = _max(1.0f, floorf(2.0f * k + 0.5f));
			UIRender->SetShader(*m_shaderGamepad);
			RenderHoldIconRing(linePx);
		}
		return;
	}

#ifdef DEBUG
	if (bDebug)
	{
		CGameFont* F = UI().Font().pFontDI;
		F->SetAligment(CGameFont::alCenter);
		F->SetHeightI(0.02f);
		F->OutSetI(0.f, -0.9f);
		F->SetColor(0xffffffff);
		Fvector2 pt = GetCursorPosition();
		F->OutNext("%f-%f", pt.x, pt.y);
	}
#endif

	if (pInput && pInput->IsGamepadActiveSource())
	{
		if (CGamepadContextDispatcher::Instance().IsGamepadMouseMode())
		{
			g_menuFocusMotion.root = nullptr;
			g_invFocusMotion.visible = false;
			g_invFocusMotion.hasLast = false;
			RenderMouseModeHintBar();
			RenderGamepadCursor();
		}
		else if (CGamepadContextDispatcher::Instance().IsMenuFocusMode())
			RenderGamepadMenuFocus();
		else if (CGamepadContextDispatcher::Instance().IsDialogSelectMode())
		{
			// INV-REWORK-21: диалог с НПС — ни курсора, ни рамки (выбранная реплика белая и сдвинута)
			g_menuFocusMotion.root = nullptr;
			g_invFocusMotion.visible = false;
			g_invFocusMotion.hasLast = false;
		}
		else if (CGamepadContextDispatcher::Instance().IsInventoryFocusFrameMode())
			RenderGamepadInventoryFocus(false);
		else if (CGamepadContextDispatcher::Instance().IsInventoryFocusFramePending())
			RenderGamepadInventoryFocus(true); // INV-REWORK-2: первые кадры после открытия — без курсора
		else if (CGamepadContextDispatcher::Instance().IsPdaMapEyeActive())
		{
			g_menuFocusMotion.root = nullptr;
			g_invFocusMotion.visible = false;
			g_invFocusMotion.hasLast = false;
			RenderPdaMapEye();
		}
		else if (CGamepadContextDispatcher::Instance().IsVcLocked() || CGamepadContextDispatcher::Instance().IsVcHidden() ||
		         CGamepadContextDispatcher::Instance().IsPdaInteractiveActive())
		{
			// INV-REWORK-16 + PDA1-E: окно «Подробно» и КПК — без курсора
			g_invFocusMotion.visible = false;
			g_invFocusMotion.hasLast = false;
		}
		else
		{
			g_menuFocusMotion.root = nullptr;
			g_invFocusMotion.visible = false;
			g_invFocusMotion.hasLast = false;
			RenderGamepadCursor();
		}
	}
	else
	{
		g_menuFocusMotion.root = nullptr;
		m_static->SetWndPos(vPos);
		m_static->Update();
		m_static->Draw();
	}

	last_render_frame = curFrame;
}

Fvector2 CUICursor::GetCursorPosition()
{
	return vPos;
}

Fvector2 CUICursor::GetCursorPositionDelta()
{
	Fvector2 res_delta;

	res_delta.x = vPos.x - vPrevPos.x;
	res_delta.y = vPos.y - vPrevPos.y;
	return res_delta;
}

void CUICursor::UpdateCursorPosition(int _dx, int _dy)
{
	Fvector2 p;
	vPrevPos = vPos;
	if (m_b_use_win_cursor)
	{
		Ivector2 pti;
		IInputReceiver::IR_GetMousePosReal(pti);
		p.x = (float)pti.x;
		p.y = (float)pti.y;
		if (Device.clientWidth > 0 && Device.clientHeight > 0)
		{
			vPos.x = p.x * (UI_BASE_WIDTH / (float)Device.clientWidth);
			vPos.y = p.y * (UI_BASE_HEIGHT / (float)Device.clientHeight);
		}
	}
	else
	{
		float sens = 1.0f;
		vPos.x += _dx * sens;
		vPos.y += _dy * sens;
	}
	clamp(vPos.x, 0.f, UI_BASE_WIDTH);
	clamp(vPos.y, 0.f, UI_BASE_HEIGHT);
}

void CUICursor::SetUICursorPosition(Fvector2 pos)
{
	vPrevPos = vPos;
	vPos = pos;
	clamp(vPos.x, 0.f, UI_BASE_WIDTH);
	clamp(vPos.y, 0.f, UI_BASE_HEIGHT);

	if (Device.clientWidth == 0 || Device.clientHeight == 0)
		return;

	// Sync Win32 OS cursor position only when the game window is the active foreground window,
	// so m_b_use_win_cursor (GetCursorPos in UpdateCursorPosition) never snaps vPos back on the next mouse event.
	if (Device.m_hWnd != NULL && GetForegroundWindow() == Device.m_hWnd && !IsIconic(Device.m_hWnd))
	{
		POINT p;
		p.x = iFloor(vPos.x / (UI_BASE_WIDTH / (float)Device.clientWidth));
		p.y = iFloor(vPos.y / (UI_BASE_HEIGHT / (float)Device.clientHeight));
		if (m_b_use_win_cursor)
			ClientToScreen(Device.m_hWnd, (LPPOINT)&p);
		SetCursorPos(p.x, p.y);
	}
}
