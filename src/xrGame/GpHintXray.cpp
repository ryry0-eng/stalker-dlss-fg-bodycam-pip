#include "stdafx.h"
#include "GpHintXray.h"
#include "UIDialogHolder.h"
#include "ui/UIWindow.h"
#include "ui/UIStatic.h"
#include "ui/UILines.h"
#include "ui_base.h"
#include "../xrEngine/GameFont.h"
#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/UIShader.h"
#include <dinput.h>
#include <cstdio>
#include <typeinfo>

// INV-REWORK-24: see GpHintXray.h. Colours: icon box yellow, label glyph cell cyan, lower-case middle
// magenta, icon middle green (red when it misses the lower-case middle by more than 0.75 px).
// Text above each icon: "<dy px> <w>x<h>" (dy = icon middle - lower-case middle, + = icon lower).

namespace GpHintXray
{
namespace
{
bool s_enabled = false;
u32 s_nextDump = 0;
ui_shader* s_wire = nullptr;
CInifile* s_metrics = nullptr;
bool s_metricsTried = false;
// Misha 2026-11-13: line through the middle of the capital letter ("cap", default) or of the lower-case ("x");
// [gp_hint_align] line in gp_font_metrics.ltx, re-read on every F8 toggle.
bool s_lineCap = true;

struct FontMetrics
{
	bool ok;
	float cellH, xTop, xBot, capTop, capBot;
};

struct Item
{
	bool icon;
	Frect r; // UI units, absolute
	xr_string name, tex, text, fontSect, fontTex;
	float cellTop, cellH; // UI
	float gl, gr; // UI, glyph run edges
	float xMid, capH; // UI; xMid < 0 -> no metrics for this font
};

LPCSTR BaseName(LPCSTR p)
{
	if (!p)
		return "";
	LPCSTR s = strrchr(p, '\\');
	return s ? s + 1 : p;
}

FontMetrics GetMetrics(CGameFont* f)
{
	FontMetrics m{false, 0, 0, 0, 0, 0};
	if (!f)
		return m;
	if (!s_metricsTried)
	{
		s_metricsTried = true;
		string_path fn;
		if (FS.exist(fn, "$game_config$", "ui\\gp_font_metrics.ltx"))
		{
			s_metrics = xr_new<CInifile>(fn);
			s_lineCap = !(s_metrics->line_exist("gp_hint_align", "line") &&
			              !xr_strcmp(s_metrics->r_string("gp_hint_align", "line"), "x"));
			Msg("* [gp_xray] gp_font_metrics.ltx loaded, line = %s", s_lineCap ? "cap" : "x");
		}
		else
			Msg("~ [gp_xray] configs\\ui\\gp_font_metrics.ltx not found");
	}
	LPCSTR sect = BaseName(f->m_gp_texture.c_str());
	if (!s_metrics || !sect[0] || !s_metrics->section_exist(sect))
		return m;
	m.cellH = s_metrics->r_float(sect, "cell_h");
	m.xTop = s_metrics->r_float(sect, "x_top");
	m.xBot = s_metrics->r_float(sect, "x_bot");
	m.capTop = s_metrics->r_float(sect, "cap_top");
	m.capBot = s_metrics->r_float(sect, "cap_bot");
	if (s_lineCap)
	{
		// the alignment line is the capital-letter middle: xTop/xBot below mean "line range"
		m.xTop = m.capTop;
		m.xBot = m.capBot;
	}
	m.ok = m.cellH > 0.f;
	return m;
}

bool IsPadIcon(LPCSTR s, const Frect& r)
{
	if (!s)
		return false;
	if (!(strstr(s, "ui_gp_") == s || strstr(s, "ui_gamepad") == s))
		return false;
	static LPCSTR bad[] = {"solid", "strip", "footer", "toast", "frame", "focus", "lettergrid", "pad_", "native_pad"};
	for (LPCSTR b : bad)
		if (strstr(s, b))
			return false;
	const float w = r.width(), h = r.height();
	return w >= 3.f && h >= 3.f && w <= 80.f && h <= 80.f;
}

float PxToUiW(float px)
{
	UI().ClientToScreenScaledWidth(px);
	return px;
}
float PxToUiH(float px)
{
	UI().ClientToScreenScaledHeight(px);
	return px;
}
float UiToPxX(float ui) { return UI().ClientToScreenScaledX(ui); }
float UiToPxY(float ui) { return UI().ClientToScreenScaledY(ui); }

xr_string FirstLinePlain(LPCSTR s)
{
	xr_string out;
	for (LPCSTR p = s; p && *p && *p != '\n' && *p != '\r'; ++p)
	{
		if (p[0] == '%' && p[1] == 'c' && p[2] == '[')
		{
			LPCSTR e = strchr(p, ']');
			if (e)
			{
				p = e;
				continue;
			}
		}
		out += *p;
	}
	return out;
}

xr_string WndName(CUIWindow* w)
{
	LPCSTR n = w->WindowName().c_str();
	return n ? n : "";
}

void AddText(CUIWindow* w, CUILines& l, const Frect& r, xr_vector<Item>& out)
{
	LPCSTR txt = l.GetText();
	CGameFont* f = l.GetFont();
	if (!f || !txt || !txt[0])
		return;
	Item it;
	it.icon = false;
	it.r = r;
	it.name = WndName(w);
	it.text = FirstLinePlain(txt);
	if (it.text.empty())
		return;
	it.fontSect = f->m_gp_section.c_str() ? f->m_gp_section.c_str() : "";
	it.fontTex = f->m_gp_texture.c_str() ? f->m_gp_texture.c_str() : "";
	it.cellH = PxToUiH(f->GetHeight());
	it.cellTop = r.y1 + l.GpTextTop();
	const float wdt = PxToUiW(f->SizeOf_(it.text.c_str()));
	const float ind = r.x1 + l.GpTextIndent();
	const int al = int(l.GetTextAlignment());
	it.gl = ind - wdt * (al == CGameFont::alCenter ? 0.5f : (al == CGameFont::alRight ? 1.f : 0.f));
	it.gr = it.gl + wdt;
	const FontMetrics m = GetMetrics(f);
	if (m.ok)
	{
		const float k = it.cellH / m.cellH;
		it.xMid = it.cellTop + (m.xTop + m.xBot) * 0.5f * k;
		it.capH = (m.capBot - m.capTop) * k;
	}
	else
	{
		it.xMid = -1.f;
		it.capH = 0.f;
	}
	out.push_back(it);
}

void Walk(CUIWindow* w, xr_vector<Item>& out, int depth)
{
	if (!w || !w->IsShown() || depth > 48)
		return;
	Frect r;
	w->GetAbsoluteRect(r);
	if (CUIStatic* s = smart_cast<CUIStatic*>(w))
	{
		if (IsPadIcon(s->m_TextureName.c_str(), r))
		{
			Item it;
			it.icon = true;
			it.r = r;
			it.name = WndName(w);
			it.tex = s->m_TextureName.c_str();
			it.cellTop = it.cellH = it.gl = it.gr = it.xMid = it.capH = 0.f;
			out.push_back(it);
		}
		if (CUILines* l = s->GpTextControlIfAny())
			AddText(w, *l, r, out);
	}
	else if (CUITextWnd* t = smart_cast<CUITextWnd*>(w))
		AddText(w, t->TextItemControl(), r, out);
	for (CUIWindow* c : w->GetChildWndList())
		Walk(c, out, depth + 1);
}

// label of an icon: nearest glyph run to the right (gap up to 40 UI), else to the left;
// the label's glyph cell must overlap the icon vertically
int FindLabel(const Item& ic, const xr_vector<Item>& items)
{
	int best = -1;
	float bestGap = 1e9f;
	for (int pass = 0; pass < 2 && best < 0; ++pass)
		for (u32 i = 0; i < items.size(); ++i)
		{
			const Item& t = items[i];
			if (t.icon)
				continue;
			if (t.cellTop > ic.r.y2 || t.cellTop + t.cellH < ic.r.y1)
				continue;
			const float gap = pass == 0 ? t.gl - ic.r.x2 : ic.r.x1 - t.gr;
			if (gap < -2.f || gap > 40.f)
				continue;
			if (gap < bestGap)
			{
				bestGap = gap;
				best = int(i);
			}
		}
	return best;
}

void Line(float x1, float y1, float x2, float y2, u32 c)
{
	Fvector2 a, b;
	UI().ClientToScreenScaled(a, x1, y1);
	UI().ClientToScreenScaled(b, x2, y2);
	UIRender->PushPoint(a.x, a.y, 0.f, c, 0.f, 0.f);
	UIRender->PushPoint(b.x, b.y, 0.f, c, 0.f, 0.f);
}

void Box(const Frect& r, u32 c)
{
	Line(r.x1, r.y1, r.x2, r.y1, c);
	Line(r.x2, r.y1, r.x2, r.y2, c);
	Line(r.x2, r.y2, r.x1, r.y2, c);
	Line(r.x1, r.y2, r.x1, r.y1, c);
}

xr_string DialogKey(const xr_vector<dlgItem>& dialogs)
{
	xr_string k;
	for (const dlgItem& d : dialogs)
	{
		if (!d.enabled || !d.wnd || !d.wnd->IsShown())
			continue;
		LPCSTR n = d.wnd->WindowName().c_str();
		xr_string part = (n && n[0]) ? n : typeid(*d.wnd).name();
		for (char& ch : part)
			if (!isalnum((unsigned char)ch))
				ch = '_';
		if (!k.empty())
			k += "+";
		k += part;
		if (k.size() > 80)
			break;
	}
	return k.empty() ? xr_string("none") : k;
}

void Dump(const xr_vector<dlgItem>& dialogs, const xr_vector<Item>& items, const xr_vector<int>& pairOf)
{
	const xr_string key = DialogKey(dialogs);
	string_path fn, name;
	xr_sprintf(name, "gamepad\\hint_xray_%s.txt", key.substr(0, 90).c_str());
	FS.update_path(fn, "$app_data_root$", name);
	FILE* f = fopen(fn, "wb");
	if (!f)
		return;
	fprintf(f, "# hint_xray screen=%ux%u dialogs=%s frame=%u (px from the top-left of the screen)\n", Device.dwWidth,
	        Device.dwHeight, key.c_str(), Device.dwFrame);
	fprintf(f, "kind\tidx\tpair\tname\ttex_or_text\tx1\ty1\tx2\ty2\tfont\tfont_tex\tcell_top\tcell_h\tglyph_l\tglyph_r\tx_mid\tcap_h\n");
	for (u32 i = 0; i < items.size(); ++i)
	{
		const Item& it = items[i];
		const bool ic = it.icon;
		fprintf(f, "%s\t%u\t%d\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%.2f\t%.2f\t%.1f\t%.1f\t%.2f\t%.2f\n",
		        ic ? "icon" : "text", i, pairOf[i], it.name.c_str(), ic ? it.tex.c_str() : it.text.c_str(),
		        UiToPxX(it.r.x1), UiToPxY(it.r.y1), UiToPxX(it.r.x2), UiToPxY(it.r.y2), it.fontSect.c_str(),
		        it.fontTex.c_str(), ic ? 0.f : UiToPxY(it.cellTop),
		        ic ? 0.f : UiToPxY(it.cellTop + it.cellH) - UiToPxY(it.cellTop), ic ? 0.f : UiToPxX(it.gl),
		        ic ? 0.f : UiToPxX(it.gr), (ic || it.xMid < 0.f) ? -1.f : UiToPxY(it.xMid),
		        ic ? 0.f : UiToPxY(it.cellTop + it.capH) - UiToPxY(it.cellTop));
	}
	fclose(f);
	string_path last;
	FS.update_path(last, "$app_data_root$", "gamepad\\hint_xray.txt");
	CopyFileA(fn, last, FALSE);
}
} // namespace

float XMidBelowCellCenterUi(CGameFont* f)
{
	const FontMetrics m = GetMetrics(f);
	if (!m.ok)
		return 0.f;
	const float cellUi = PxToUiH(f->GetHeight());
	return ((m.xTop + m.xBot) * 0.5f / m.cellH - 0.5f) * cellUi;
}

bool Allowed() { return strstr(Core.Params, "-dbg") != nullptr; }
bool Enabled() { return s_enabled; }

void Toggle()
{
	if (!Allowed())
		return;
	s_enabled = !s_enabled;
	s_nextDump = 0;
	// re-read gp_font_metrics.ltx (metrics and line mode can be edited without leaving the game)
	if (s_metrics)
		xr_delete(s_metrics);
	s_metricsTried = false;
	Msg("* [gp_xray] %s", s_enabled ? "on" : "off");
}

bool OnKey(int dik)
{
	if (dik != DIK_F8 || !Allowed())
		return false;
	Toggle();
	return true;
}

void RenderDialogs(const xr_vector<dlgItem>& dialogs)
{
	if (!s_enabled)
		return;
	xr_vector<Item> items;
	for (const dlgItem& d : dialogs)
		if (d.enabled && d.wnd && d.wnd->IsShown())
			Walk(d.wnd, items, 0);
	if (items.empty())
		return;

	xr_vector<int> pairOf(items.size(), -1);
	for (u32 i = 0; i < items.size(); ++i)
		if (items[i].icon)
		{
			const int t = FindLabel(items[i], items);
			pairOf[i] = t;
			if (t >= 0 && pairOf[t] < 0)
				pairOf[t] = int(i);
		}

	if (!s_wire)
	{
		s_wire = xr_new<ui_shader>();
		(*s_wire)->create("hud\\crosshair");
	}
	const float px1 = PxToUiH(1.f);
	UIRender->SetShader(**s_wire);
	UIRender->StartPrimitive(u32(items.size()) * 24 + 8, IUIRender::ptLineList, UI().m_currentPointType);
	for (u32 i = 0; i < items.size(); ++i)
	{
		const Item& it = items[i];
		if (it.icon)
		{
			Box(it.r, color_rgba(255, 230, 0, 255));
			const float cy = (it.r.y1 + it.r.y2) * 0.5f;
			const int t = pairOf[i];
			float x2 = it.r.x2 + 6.f, x1 = it.r.x1 - 2.f;
			u32 c = color_rgba(0, 255, 0, 255);
			if (t >= 0)
			{
				const Item& tx = items[t];
				x1 = _min(x1, tx.gl);
				x2 = _max(x2, tx.gr);
				if (tx.xMid >= 0.f && _abs(UiToPxY(cy) - UiToPxY(tx.xMid)) > 0.75f)
					c = color_rgba(255, 40, 40, 255);
			}
			Line(x1, cy, x2, cy, c);
		}
		else if (pairOf[i] >= 0)
		{
			Box(Frect().set(it.gl, it.cellTop, it.gr, it.cellTop + it.cellH), color_rgba(0, 220, 255, 255));
			if (it.xMid >= 0.f)
				Line(it.gl, it.xMid + px1 * 0.5f, it.gr, it.xMid + px1 * 0.5f, color_rgba(255, 0, 255, 255));
		}
	}
	UIRender->FlushPrimitive();

	CGameFont* F = UI().Font().pFontArial14;
	if (F)
	{
		F->SetAligment(CGameFont::alLeft);
		for (u32 i = 0; i < items.size(); ++i)
		{
			const Item& it = items[i];
			const int t = pairOf[i];
			if (!it.icon || t < 0 || items[t].xMid < 0.f)
				continue;
			const float dy = UiToPxY((it.r.y1 + it.r.y2) * 0.5f) - UiToPxY(items[t].xMid);
			const float w = UiToPxX(it.r.x2) - UiToPxX(it.r.x1), h = UiToPxY(it.r.y2) - UiToPxY(it.r.y1);
			F->SetColor(_abs(dy) > 0.75f || _abs(w - h) > 1.f ? color_rgba(255, 80, 80, 255) : color_rgba(120, 255, 120, 255));
			F->Out(UiToPxX(it.r.x1), UiToPxY(it.r.y1) - 14.f, "%+.1f %.0fx%.0f", dy, w, h);
		}
		F->OnRender();
	}

	if (Device.dwTimeGlobal >= s_nextDump)
	{
		s_nextDump = Device.dwTimeGlobal + 1000;
		Dump(dialogs, items, pairOf);
	}
}
} // namespace GpHintXray
