#pragma once

// D4.2 / D39: Native C++ Gamepad Remapping Screen & Class C Device Mapping Wizard (CUIGamepadRemapWnd)
// Works on pause via Device.dwTimeContinual and C4.1 pause token ("gp_remap").
// Features:
// 1. Per-context action remapping (Single Button, Hold, Chord) with conflict detection & swap confirmation modal.
// 2. Protected MenuUI / MainMenu navigation (Menu, Confirm, Back): blocks unbind/invalid triggers, allows safe swap.
// 3. Export / Import bindings to a separate file and Emergency Reset (Start + Back 5s or gp_bind_reset).
// 4. Step-by-step Class C Mapping Wizard for unmapped SDL_Joystick devices -> builds SDL mapping string,
//    calls SDL_AddGamepadMapping, promotes Class C -> Class B, and saves to $app_data_root$\gamepad\user_mappings.txt.

class CGamepadRemapUI
{
public:
	static CGamepadRemapUI& Instance();

	void Open(LPCSTR sourceTag = "cmd", bool openInWizardMode = false);
	void Close(LPCSTR reason = "close");
	void Toggle(LPCSTR sourceTag = "toggle");
	bool IsOpen() const;

	void OpenWizard(LPCSTR sourceTag = "wizard");

	void LogStatus() const;
	bool RunSelfTest();
	void LogActionsUnreached(LPCSTR args = "") const;

private:
	CGamepadRemapUI() = default;
	~CGamepadRemapUI() = default;
	CGamepadRemapUI(const CGamepadRemapUI&) = delete;
	CGamepadRemapUI& operator=(const CGamepadRemapUI&) = delete;
};
