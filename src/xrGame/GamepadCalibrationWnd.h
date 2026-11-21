#pragma once

// D2.2 / D39: Native C++ Live Gamepad Calibration Window (CUIGamepadCalibrationWnd)
// Works on pause via Device.dwTimeContinual and C4.1 pause token ("gp_calibration").
// Displays Left/Right stick circles (raw & processed vectors, deadzone & saturation rings, curve preview)
// and LT/RT trigger bars with press/release hysteresis thresholds. Saves profile on exit to appdata\gamepad\profile.ltx.

class CGamepadCalibrationUI
{
public:
	static CGamepadCalibrationUI& Instance();

	void Open(LPCSTR sourceTag = "cmd");
	void Close(LPCSTR reason = "close");
	void Toggle(LPCSTR sourceTag = "toggle");
	bool IsOpen() const;

	void LogStatus() const;
	bool RunSelfTest();

private:
	CGamepadCalibrationUI() = default;
	~CGamepadCalibrationUI() = default;
	CGamepadCalibrationUI(const CGamepadCalibrationUI&) = delete;
	CGamepadCalibrationUI& operator=(const CGamepadCalibrationUI&) = delete;
};
