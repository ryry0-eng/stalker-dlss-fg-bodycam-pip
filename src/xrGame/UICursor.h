#pragma once

#include "ui_base.h"
class CUIStatic;

class CUICursor : public pureRender,
                  public pureScreenResolutionChanged
{
	bool bVisible;
	Fvector2 vPos;
	Fvector2 vPrevPos;
	bool m_b_use_win_cursor;
	CUIStatic* m_static;
	ui_shader m_shaderGamepad;
	ui_shader m_shaderCursorGlass;
	bool m_bCursorGlassChecked = false;
	bool m_bCursorGlassAvailable = false;
	void InitInternal();
	void RenderGamepadCursor();
	void RenderMouseModeHintBar();
	void RenderGamepadMenuFocus();
	void RenderGamepadInventoryFocus(bool pending);
	void RenderPdaMapEye();
public:
	enum class ECursorVisualMode : u8
	{
		StandardMouse = 0,
		GamepadIdle = 1,
		GamepadHover = 2,
		GamepadPressed = 3
	};

	CUICursor();
	virtual ~CUICursor();
	virtual void OnRender();

	Fvector2 GetCursorPositionDelta();

	Fvector2 GetCursorPosition();
	void SetUICursorPosition(Fvector2 pos);
	bool IsMenuFocusTransitionActive(const void* root, u32 now) const;
	void UpdateCursorPosition(int _dx, int _dy);
	virtual void OnScreenResolutionChanged();

	bool IsUsingWinCursor() const { return m_b_use_win_cursor; }
	static ECursorVisualMode EvaluateVisualMode(bool isGamepadActiveSource, bool isHovered, bool isPressed);
	ECursorVisualMode GetCurrentVisualMode() const;

	bool IsVisible() { return bVisible; }
	void Show() { bVisible = true; }
	void Hide() { bVisible = false; }
};
