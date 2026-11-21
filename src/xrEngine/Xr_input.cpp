#include "stdafx.h"
#pragma hdrstop

#include "xr_input.h"
#include "IInputReceiver.h"
#include "GamepadDeviceManager.h"
#include "GamepadHaptics.h"
#include "GamepadMath.h"
#include "GamepadBindings.h"
#include "GamepadAtomicFile.h"
#include "GamepadErrorGuard.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <share.h>
//#include "../include/editor/ide.hpp"

#ifndef _EDITOR
# include "xr_input_xinput.h"
#endif
CInput* pInput = NULL;
IInputReceiver dummyController;
ENGINE_API int ps_gp_telemetry = 0;
ENGINE_API volatile bool g_gp_in_sdl_update = false;
ENGINE_API volatile u32 g_gp_wndproc_in_sdl_count = 0;
ENGINE_API UINT g_gp_wndproc_in_sdl_msgs[GAMEPAD_WNDPROC_RING_SIZE] = {};

// D1.2 / D2.2 (D1.2-N5): параметры гистерезиса арбитража Клавиатура/Мышь ↔ Геймпад.
// Базовый порог мёртвой зоны стика берётся из профиля калибровки (gpmath::GetCalibrationStickDeadzone()).
ENGINE_API float ps_gp_source_stick_hysteresis = 0.15f;
ENGINE_API int ps_gp_source_stick_hold_ms = 100;
ENGINE_API int ps_gp_source_mouse_px_threshold = 4;
ENGINE_API int ps_gp_source_cooldown_ms = 500;

static void GamepadCalibrationLogMsg(const char* text)
{
	if (text)
		Msg("%s", text);
}

ENGINE_API float psMouseSens = 1.f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API float psMouseSensVerticalK = 1.f;
ENGINE_API Flags32 psMouseInvert = {FALSE};

float stop_vibration_time = flt_max;

// demonized: configurable buffer sizes for mouse and kb
int MOUSEBUFFERSIZE = 1024;
int KEYBOARDBUFFERSIZE = 128;

#define _KEYDOWN(name,key) ( name[key] & 0x80 )

static bool g_exclusive = false;
extern u32 g_screenmode;

static void on_error_dialog(bool before)
{
#ifdef INGAME_EDITOR
    if (Device.editor())
        return;
#endif // #ifdef INGAME_EDITOR
	if (!pInput || !g_exclusive)
		return;

	if (before)
	{
		pInput->unacquire();
		return;
	}

	pInput->acquire(true);
}

CInput::CInput(BOOL bExclusive, int deviceForInit)
{
	g_exclusive = !!bExclusive;

	Log("Starting INPUT device...");

	pDI = NULL;
	pMouse = NULL;
	pKeyboard = NULL;

	//=====================Mouse
	mouse_property.mouse_dt = 25;

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(m_gamepadInjectedKBState, sizeof(m_gamepadInjectedKBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));

	// DirectInput buffers are reused for the lifetime of the input manager.
	// Avoiding two heap allocations per frame removes allocator contention from
	// the latency-sensitive input callback.
	m_keyboard_events.resize(static_cast<u32>(_max(KEYBOARDBUFFERSIZE, 1)));
	m_mouse_events.resize(static_cast<u32>(_max(MOUSEBUFFERSIZE, 1)));

	//===================== Dummy pack
	iCapture(&dummyController);

	if (!pDI)
		CHK_DX(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&pDI, NULL));

	//. u32 kb_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;
	u32 kb_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;

	//. u32 mouse_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY,
	u32 mouse_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY;

	// KEYBOARD
	if (deviceForInit & keyboard_device_key)
		CHK_DX(CreateInputDevice(
		&pKeyboard, GUID_SysKeyboard, &c_dfDIKeyboard,
		kb_input_flags,
		KEYBOARDBUFFERSIZE));

	// MOUSE
	if (deviceForInit & mouse_device_key)
		CHK_DX(CreateInputDevice(
		&pMouse, GUID_SysMouse, &c_dfDIMouse2,
		mouse_input_flags,
		MOUSEBUFFERSIZE));

	// C1.1 / D1.1 / D5.2: SDL3 gamepad initialization after DirectInput8 devices
	m_pGamepadDeviceMgr = xr_new<CGamepadDeviceManager>();
	m_pGamepadHaptics = xr_new<CGamepadHaptics>();
	m_pGamepad = nullptr;
	m_gamepadInstanceId = 0;
	m_bSdlGamepadInitialized = false;
	m_bGamepadNeedReleaseAll = false;
	m_bGamepadLatchPending = false;
	m_gamepadReleaseMask = 0;
	m_bGamepadHasLoggedState = false;
	m_bGamepadTelemetryHeaderWritten = false;
	m_pGamepadTelemetryFile = nullptr;
	m_gamepadTelemetryLastFlushMs = 0;
	ZeroMemory(&m_gamepadState, sizeof(m_gamepadState));
	ZeroMemory(&m_gamepadLastLoggedState, sizeof(m_gamepadLastLoggedState));

	m_pGamepadDeviceMgr->Initialize();
	m_pGamepadHaptics->Initialize();
	{
		gpmath::SetCalibrationLogCallback(&GamepadCalibrationLogMsg);
		string_path calibPath = {};
		FS.update_path(calibPath, "$app_data_root$", "gamepad\\profile.ltx");
		gpmath::SetCalibrationProfilePath(calibPath);
		gpmath::LoadCalibrationProfile();

		gpbind::SetBindingLogCallback(&GamepadCalibrationLogMsg);
		gpbind::SetActiveBindingProfilePath(calibPath);
		string_path exportPath = {};
		FS.update_path(exportPath, "$app_data_root$", "gamepad\\exports\\bindings_export.ltx");
		gpbind::SetDefaultExportProfilePath(exportPath);
		gpbind::LoadActiveBindingProfile();
		LoadSourceLockModeFromProfile();

		// S3: configure gamepad_last_error.log paths ($app_data_root$ and bench root) and recovery callback
		string_path errAppDataPath = {};
		FS.update_path(errAppDataPath, "$app_data_root$", "gamepad_last_error.log");
		std::string errRootPathStr;
		{
			std::error_code ec;
			std::filesystem::path appLogP(errAppDataPath);
			if (appLogP.has_parent_path() && appLogP.parent_path().has_parent_path())
			{
				errRootPathStr = (appLogP.parent_path().parent_path() / "gamepad_last_error.log").string();
			}
		}
		gperr::SetDefaultErrorLogPaths(errAppDataPath, errRootPathStr.empty() ? nullptr : errRootPathStr.c_str());
		gperr::SetEngineErrorRecoveryCallback(&CInput::StaticGamepadErrorRecoveryCallback, this);
	}
	m_bSdlGamepadInitialized = m_pGamepadDeviceMgr->IsInitialized();
	if (const SGamepadDeviceEntry* activeDev = m_pGamepadDeviceMgr->GetActiveDevice())
	{
		m_pGamepad = activeDev->gamepad;
		m_gamepadInstanceId = activeDev->instance_id;
		m_bGamepadNeedReleaseAll = true;
		m_bGamepadLatchPending = true;
		if (m_sourceLockMode == EInputSourceLockMode::GamepadOnly &&
		    activeDev->device_class != EGamepadClass::ClassC && activeDev->gamepad != nullptr)
		{
			m_activeInputSource = EActiveInputSource::Gamepad;
			m_bPrevGamepadConnectedForSource = true;
		}
	}

	Debug.set_on_dialog(&on_error_dialog);

#ifdef ENGINE_BUILD
	Device.seqAppActivate.Add(this);
	Device.seqAppDeactivate.Add(this, REG_PRIORITY_HIGH);
	// Run input polling before IGame_Persistent (REG_PRIORITY_HIGH + 1) so actor movement
	// and GamepadContextDispatcher consume frame N state instead of frame N-1.
	Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 100);
#endif
}

CInput::~CInput(void)
{
	gperr::SetEngineErrorRecoveryCallback(nullptr, nullptr);
	CloseGamepadTelemetryFile();
#ifdef ENGINE_BUILD
	Device.seqFrame.Remove(this);
	Device.seqAppDeactivate.Remove(this);
	Device.seqAppActivate.Remove(this);
#endif
	//_______________________

	if (m_pGamepadHaptics)
	{
		m_pGamepadHaptics->Shutdown();
		xr_delete(m_pGamepadHaptics);
	}
	if (m_pGamepadDeviceMgr)
	{
		m_pGamepadDeviceMgr->Shutdown();
		xr_delete(m_pGamepadDeviceMgr);
	}
	m_pGamepad = nullptr;
	m_gamepadInstanceId = 0;
	m_bSdlGamepadInitialized = false;

	// Unacquire and release the device's interfaces
	if (pMouse)
	{
		pMouse->Unacquire();
		_RELEASE(pMouse);
	}

	if (pKeyboard)
	{
		pKeyboard->Unacquire();
		_RELEASE(pKeyboard);
	}

	_SHOW_REF("Input: ", pDI);
	_RELEASE(pDI);
}

//-----------------------------------------------------------------------------
// Name: CreateInputDevice()
// Desc: Create a DirectInput device.
//-----------------------------------------------------------------------------
HRESULT CInput::CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice, const DIDATAFORMAT* pdidDataFormat,
                                  u32 dwFlags, u32 buf_size)
{
	// Obtain an interface to the input device
	//. CHK_DX( pDI->CreateDeviceEx( guidDevice, IID_IDirectInputDevice8, (void**)device, NULL ) );
	CHK_DX(pDI->CreateDevice(guidDevice, /*IID_IDirectInputDevice8,*/ device, NULL));

	// Set the device data format. Note: a data format specifies which
	// controls on a device we are interested in, and how they should be
	// reported.
	CHK_DX((*device)->SetDataFormat(pdidDataFormat));

	// Set the cooperativity level to let DirectInput know how this device
	// should interact with the system and with other DirectInput applications.
#ifdef INGAME_EDITOR
    if (!Device.editor())
#endif // #ifdef INGAME_EDITOR
	{
		HRESULT _hr = (*device)->SetCooperativeLevel(RDEVICE.m_hWnd, dwFlags);
		if (FAILED(_hr) && (_hr == E_NOTIMPL)) Msg("! INPUT: Can't set coop level. Emulation???");
		else
			R_CHK(_hr);
	}

	// setup the buffer size for the keyboard data
	DIPROPDWORD dipdw;
	dipdw.diph.dwSize = sizeof(DIPROPDWORD);
	dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	dipdw.diph.dwObj = 0;
	dipdw.diph.dwHow = DIPH_DEVICE;
	dipdw.dwData = buf_size;

	CHK_DX((*device)->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph));

	return S_OK;
}

//-----------------------------------------------------------------------

void CInput::SetAllAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

void CInput::SetMouseAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
}

void CInput::SetKBDAcquire(BOOL bAcquire)
{
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

//-----------------------------------------------------------------------
BOOL b_altF4 = FALSE;

void CInput::KeyUpdate()
{
	if (b_altF4) return;

	HRESULT hr;
	const u32 requested_elements = static_cast<u32>(_max(KEYBOARDBUFFERSIZE, 1));
	if (m_keyboard_events.size() < requested_elements)
		m_keyboard_events.resize(requested_elements);
	DWORD dwElements = requested_elements;
	DIDEVICEOBJECTDATA* const od = m_keyboard_events.data();
	DWORD key = 0;

	VERIFY(pKeyboard);

	hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), od, &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pKeyboard->Acquire();
		if (hr != S_OK)
			return;

		hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), od, &dwElements, 0);
		if (hr != S_OK)
			return;
	}

	bool b_dik_pause_was_pressed = false;
	for (u32 idx = 0; idx < dwElements; idx++)
	{
		if (od[idx].dwOfs == DIK_PAUSE)
		{
			if (od[idx].dwData & 0x80)
				b_dik_pause_was_pressed = true;

			if (b_dik_pause_was_pressed && !(od[idx].dwData & 0x80))
			{
				od[idx].uAppData = 666;
				continue; //skip one-frame pause key on-off switch
			}
		}
		KBState[od[idx].dwOfs] = od[idx].dwData & 0x80;
	}

#ifndef _EDITOR
	bool b_alt_tab = false;

	if (!b_altF4 && KBState[DIK_F4] && (KBState[DIK_RMENU] || KBState[DIK_LMENU]))
	{
		b_altF4 = TRUE;
		Engine.Event.Defer("KERNEL:disconnect");
		Engine.Event.Defer("KERNEL:quit");
	}


#endif
	if (b_altF4) return;

#ifndef _EDITOR
	if (Device.dwPrecacheFrame == 0)
#endif
	{
		for (u32 i = 0; i < dwElements; i++)
		{
			if (od[i].uAppData == 666) //ignored action
				continue;

			key = od[i].dwOfs;
			if (od[i].dwData & 0x80)
			{
				m_bFrameKbKeyPressed = true;
				cbStack.back()->IR_OnKeyboardPress(key);
			}
			else
			{
				cbStack.back()->IR_OnKeyboardRelease(key);
#ifndef _EDITOR
				if (key == DIK_TAB && (iGetAsyncKeyState(DIK_RMENU) || iGetAsyncKeyState(DIK_LMENU)))
					b_alt_tab = true;
#endif
			}
		}

		for (u32 i = 0; i < COUNT_KB_BUTTONS; i++)
			if (KBState[i])
				cbStack.back()->IR_OnKeyboardHold(i);
	}

#ifndef _EDITOR
	if (b_alt_tab) {
		BOOL fullscreen = (g_screenmode == 2);
		if (fullscreen)
			SendMessage(Device.m_hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	}
#endif
	/*
	#ifndef _EDITOR
	//update xinput if exist
	for( DWORD iUserIndex=0; iUserIndex<DXUT_MAX_CONTROLLERS; iUserIndex++ )
	{
	DXUTGetGamepadState( iUserIndex, &g_GamePads[iUserIndex], true, false );

	if( !g_GamePads[iUserIndex].bConnected )
	continue; // unplugged?

	bool new_b, old_b;
	new_b = !!(g_GamePads[iUserIndex].wPressedButtons & XINPUT_GAMEPAD_A);
	old_b = !!(g_GamePads[iUserIndex].wLastButtons & XINPUT_GAMEPAD_A);

	if(new_b != old_b)
	{
	if(old_b)
	cbStack.back()->IR_OnMousePress(0);
	else
	cbStack.back()->IR_OnMouseRelease(0);
	}
	int dx,dy;
	dx = iFloor(g_GamePads[iUserIndex].fThumbRX*6);
	dy = iFloor(g_GamePads[iUserIndex].fThumbRY*6);
	if(dx || dy)
	cbStack.back()->IR_OnMouseMove ( dx, dy );
	}

	if(Device.fTimeGlobal > stop_vibration_time)
	{
	stop_vibration_time = flt_max;
	set_vibration (0, 0);
	}
	//xinput
	#endif
	*/
}

void CInput::resetMouseState()
{
	for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++) {
		mouseState[i] = 0;
	}
}

bool CInput::get_dik_name(int dik, LPSTR dest_str, int dest_sz)
{
	DIPROPSTRING keyname;
	keyname.diph.dwSize = sizeof(DIPROPSTRING);
	keyname.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	keyname.diph.dwObj = static_cast<DWORD>(dik);
	keyname.diph.dwHow = DIPH_BYOFFSET;
	HRESULT hr = pKeyboard->GetProperty(DIPROP_KEYNAME, &keyname.diph);
	if (FAILED(hr))
		return false;

	const wchar_t* wct = keyname.wsz;
	if (0 == wcslen(wct))
		return false;

	int cnt = WideCharToMultiByte(CP_ACP, 0, keyname.wsz, -1, dest_str, dest_sz, NULL, NULL);
	if (cnt == -1)
	{
		Msg("! cant convert dik_name for dik[%d], prop=[%S]", dik, keyname.wsz);
		return false;
	}
	return (cnt != -1);
}

bool CInput::dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz)
{
	if (!dest || dest_sz <= 1)
		return false;

	dest[0] = 0;

	const UINT scan = static_cast<UINT>(dik);
	const HKL hkl = GetKeyboardLayout(0);

	// Convert scan code -> virtual key according to current layout
	const UINT vk = MapVirtualKeyEx(scan, MAPVK_VSC_TO_VK_EX, hkl);
	if (vk == 0)
		return false;

	BYTE ks[256] = {};
	if (shift) ks[VK_SHIFT] = 0x80;
	if (ctrl)  ks[VK_CONTROL] = 0x80;
	if (alt)   ks[VK_MENU] = 0x80;
	if (caps)  ks[VK_CAPITAL] = 0x01;

	// AltGr (usually Right Alt) on Windows behaves like Ctrl+Alt
	if (altgr)
	{
		ks[VK_CONTROL] = 0x80;
		ks[VK_MENU] = 0x80;
	}
    
	wchar_t wbuf[8] = {};
	const int rc = ToUnicodeEx(vk, scan, ks, wbuf, (int)std::size(wbuf), 0, hkl);

	if (rc <= 0)
	{
		// Dead key: clear keyboard state in ToUnicodeEx internal buffer
		if (rc == -1)
		{
			BYTE ks0[256] = {};
			wchar_t dummy[8] = {};
			ToUnicodeEx(vk, scan, ks0, dummy, (int)std::size(dummy), 0, hkl);
		}
		return false;
	}

	// Filter control chars
	if (wbuf[0] < 0x20)
		return false;

	// Convert produced Unicode to CP1251
	BOOL usedDefault = FALSE;
	const int mb = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wbuf, rc, dest, dest_sz - 1, "?", &usedDefault);
	if (mb <= 0)
		return false;

	// Null-terminate
	dest[mb] = 0;

	// If UI cannot represent it in CP1251, prevent cyrillic output
	// Return true to insert '?'
	if (usedDefault)
		return false;

	return true;
}

#define MOUSE_1 (0xED + 100)
#define MOUSE_8 (0xED + 107)

BOOL CInput::iGetAsyncKeyState(int dik)
{
	if (dik >= 0 && dik < COUNT_KB_BUTTONS)
		return !!(KBState[dik] || m_gamepadInjectedKBState[dik]);
	else if (dik >= MOUSE_1 && dik <= MOUSE_8)
	{
		int mk = dik - MOUSE_1;
		return iGetAsyncBtnState(mk);
	}
	else
		return FALSE; //unknown key ???
}

void CInput::SetGamepadInjectedKeyState(int dik, bool isDown)
{
	if (dik > 0 && dik < COUNT_KB_BUTTONS)
		m_gamepadInjectedKBState[dik] = isDown;
}

void CInput::ClearAllGamepadInjectedKeys()
{
	ZeroMemory(m_gamepadInjectedKBState, sizeof(m_gamepadInjectedKBState));
}

BOOL CInput::iGetAsyncBtnState(int btn)
{
	if ((btn == 0 || btn == 1) && !!GetSystemMetrics(SM_SWAPBUTTON))
		btn = btn == 0 ? 1 : 0;
	return !!mouseState[btn];
}

void CInput::MouseUpdate()
{
	HRESULT hr;
	const u32 requested_elements = static_cast<u32>(_max(MOUSEBUFFERSIZE, 1));
	if (m_mouse_events.size() < requested_elements)
		m_mouse_events.resize(requested_elements);
	DWORD dwElements = requested_elements;
	DIDEVICEOBJECTDATA* const od = m_mouse_events.data();

	VERIFY(pMouse);

	hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), od, &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pMouse->Acquire();
		if (hr != S_OK) return;
		hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), od, &dwElements, 0);
		if (hr != S_OK) return;
	};

#ifndef _EDITOR
	if (Device.dwPrecacheFrame)
		return;
#endif
	BOOL mouse_prev[COUNT_MOUSE_BUTTONS];

	mouse_prev[0] = mouseState[0];
	mouse_prev[1] = mouseState[1];
	mouse_prev[2] = mouseState[2];
	mouse_prev[3] = mouseState[3];
	mouse_prev[4] = mouseState[4];
	mouse_prev[5] = mouseState[5];
	mouse_prev[6] = mouseState[6];
	mouse_prev[7] = mouseState[7];

	bool bSwitched = !!GetSystemMetrics(SM_SWAPBUTTON);

	offs[0] = offs[1] = offs[2] = 0;
	for (u32 i = 0; i < dwElements; i++)
	{
		switch (od[i].dwOfs)
		{
		case DIMOFS_X:
			offs[0] += od[i].dwData;
			m_frameMouseDeltaX += static_cast<int>(od[i].dwData);
			timeStamp[0] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Y:
			offs[1] += od[i].dwData;
			m_frameMouseDeltaY += static_cast<int>(od[i].dwData);
			timeStamp[1] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Z:
			offs[2] += od[i].dwData;
			m_frameMouseWheelDelta += static_cast<int>(od[i].dwData);
			timeStamp[2] = od[i].dwTimeStamp;
			break;
		case DIMOFS_BUTTON0:
			if (od[i].dwData & 0x80)
			{
				mouseState[0] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(bSwitched ? 1 : 0);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[0] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 1 : 0);
			}
			break;
		case DIMOFS_BUTTON1:
			if (od[i].dwData & 0x80)
			{
				mouseState[1] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(bSwitched ? 0 : 1);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[1] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 0 : 1);
			}
			break;
		case DIMOFS_BUTTON2:
			if (od[i].dwData & 0x80)
			{
				mouseState[2] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(2);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[2] = FALSE;
				cbStack.back()->IR_OnMouseRelease(2);
			}
			break;
		case DIMOFS_BUTTON3:
			if (od[i].dwData & 0x80)
			{
				mouseState[3] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(3);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[3] = FALSE;
				cbStack.back()->IR_OnMouseRelease(3);
			}
			break;
		case DIMOFS_BUTTON4:
			if (od[i].dwData & 0x80)
			{
				mouseState[4] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(4);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[4] = FALSE;
				cbStack.back()->IR_OnMouseRelease(4);
			}
			break;
		case DIMOFS_BUTTON5:
			if (od[i].dwData & 0x80)
			{
				mouseState[5] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(5);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[5] = FALSE;
				cbStack.back()->IR_OnMouseRelease(5);
			}
			break;
		case DIMOFS_BUTTON6:
			if (od[i].dwData & 0x80)
			{
				mouseState[6] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(6);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[6] = FALSE;
				cbStack.back()->IR_OnMouseRelease(6);
			}
			break;
		case DIMOFS_BUTTON7:
			if (od[i].dwData & 0x80)
			{
				mouseState[7] = TRUE;
				m_bFrameMouseBtnPressed = true;
				cbStack.back()->IR_OnMousePress(7);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[7] = FALSE;
				cbStack.back()->IR_OnMouseRelease(7);
			}
			break;
		}
	}

	if (mouseState[0] && mouse_prev[0])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 1 : 0);
	}

	if (mouseState[1] && mouse_prev[1])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 0 : 1);
	}

	if (mouseState[2] && mouse_prev[2])
	{
		cbStack.back()->IR_OnMouseHold(2);
	}

	if (mouseState[3] && mouse_prev[3])
	{
		cbStack.back()->IR_OnMouseHold(3);
	}

	if (mouseState[4] && mouse_prev[4])
	{
		cbStack.back()->IR_OnMouseHold(4);
	}

	if (mouseState[5] && mouse_prev[5])
	{
		cbStack.back()->IR_OnMouseHold(5);
	}

	if (mouseState[6] && mouse_prev[6])
	{
		cbStack.back()->IR_OnMouseHold(6);
	}

	if (mouseState[7] && mouse_prev[7])
	{
		cbStack.back()->IR_OnMouseHold(7);
	}

	if (dwElements)
	{
		if (offs[0] || offs[1]) cbStack.back()->IR_OnMouseMove(offs[0], offs[1]);
		if (offs[2]) cbStack.back()->IR_OnMouseWheel(offs[2]);
	}
	else
	{
		if (timeStamp[1] && ((dwCurTime - timeStamp[1]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_Y, timeStamp[1] = 0);
		if (timeStamp[0] && ((dwCurTime - timeStamp[0]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_X, timeStamp[0] = 0);
	}
}

//-------------------------------------------------------
void CInput::iCapture(IInputReceiver* p)
{
	VERIFY(p);
	if (pMouse) MouseUpdate();
	if (pKeyboard) KeyUpdate();

	// change focus
	if (!cbStack.empty())
		cbStack.back()->IR_OnDeactivate();
	cbStack.push_back(p);
	cbStack.back()->IR_OnActivate();

	// prepare for _new_ controller
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::iRelease(IInputReceiver* p)
{
	if (p == cbStack.back())
	{
		cbStack.back()->IR_OnDeactivate();
		cbStack.pop_back();
		IInputReceiver* ir = cbStack.back();
		ir->IR_OnActivate();
	}
	else
	{
		// we are not topmost receiver, so remove the nearest one
		u32 cnt = cbStack.size();
		for (; cnt > 0; --cnt)
			if (cbStack[cnt - 1] == p)
			{
				xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
				std::advance(it, cnt - 1);
				cbStack.erase(it);
				break;
			}
	}
}

void CInput::OnAppActivate(void)
{
	if (CurrentIR())
		CurrentIR()->IR_OnActivate();

	SetAllAcquire(true);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(m_gamepadInjectedKBState, sizeof(m_gamepadInjectedKBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));

	m_bFrameKbKeyPressed = false;
	m_bFrameMouseBtnPressed = false;
	m_frameMouseWheelDelta = 0;
	m_frameMouseDeltaX = 0;
	m_frameMouseDeltaY = 0;
	m_bStickDeflecting = false;
	m_stickDeflectStartTimeMs = 0;
	m_bStickRequireNeutralBeforeDeflect = false;
	m_bPrevTriggerOverThresh[0] = false;
	m_bPrevTriggerOverThresh[1] = false;
	m_bHasPendingSourceRequest = false;
	m_pendingSourceReason = nullptr;

	// C1.1: raise release barrier on focus return
	++m_gamepadFocusGen;
	m_bGamepadNeedReleaseAll = true;
	m_bGamepadLatchPending = true;
	m_gamepadState.need_release_all = true;
}

void CInput::OnAppDeactivate(void)
{
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	SetAllAcquire(false);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(m_gamepadInjectedKBState, sizeof(m_gamepadInjectedKBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));

	m_bFrameKbKeyPressed = false;
	m_bFrameMouseBtnPressed = false;
	m_frameMouseWheelDelta = 0;
	m_frameMouseDeltaX = 0;
	m_frameMouseDeltaY = 0;
	m_bStickDeflecting = false;
	m_stickDeflectStartTimeMs = 0;
	m_bStickRequireNeutralBeforeDeflect = false;
	m_bPrevTriggerOverThresh[0] = false;
	m_bPrevTriggerOverThresh[1] = false;
	m_bHasPendingSourceRequest = false;
	m_pendingSourceReason = nullptr;
	m_prevGamepadButtonsMask = 0;

	// C1.1 / D1.1 / D5.2: zero gamepad snapshot and stop rumble on focus loss
	++m_gamepadFocusGen;
	if (m_pGamepadHaptics)
		m_pGamepadHaptics->StopAll("app_deactivate");
	else if (m_pGamepadDeviceMgr)
		m_pGamepadDeviceMgr->StopDeviceRumble(0);
	ResetGamepadSnapshot(false);
	m_bGamepadNeedReleaseAll = true;
	m_bGamepadLatchPending = true;
	m_gamepadState.need_release_all = true;
}

void CInput::DeactivateSoft()
{
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(m_gamepadInjectedKBState, sizeof(m_gamepadInjectedKBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::OnFrame(void)
{
	PROF_EVENT("CInput::OnFrame");
	RDEVICE.Statistic->Input.Begin();
	dwCurTime = RDEVICE.TimerAsync_MMT();
	m_bFrameKbKeyPressed = false;
	m_bFrameMouseBtnPressed = false;
	m_frameMouseWheelDelta = 0;
	m_frameMouseDeltaX = 0;
	m_frameMouseDeltaY = 0;
	if (pKeyboard) KeyUpdate();
	if (pMouse) MouseUpdate();
	GamepadUpdate();
	UpdateInputSourceArbitration();
	RDEVICE.Statistic->Input.End();
}

void CInput::ResetGamepadSnapshot(bool hasFocus)
{
	ZeroMemory(m_gamepadState.axes, sizeof(m_gamepadState.axes));
	m_gamepadState.buttons_mask = 0;
	m_gamepadState.instance_id = m_gamepadInstanceId;
	m_gamepadState.timestamp_ms = dwCurTime;
	m_gamepadState.connected = (m_gamepadInstanceId != 0);
	m_gamepadState.has_focus = hasFocus;
	m_gamepadState.need_release_all = m_bGamepadNeedReleaseAll;
}

void CInput::GamepadUpdateStepThunk(void* ctx)
{
	CInput* self = static_cast<CInput*>(ctx);
	if (self)
		self->GamepadUpdateInner();
}

void CInput::SourceArbitrationStepThunk(void* ctx)
{
	CInput* self = static_cast<CInput*>(ctx);
	if (self)
		self->UpdateInputSourceArbitrationInner();
}

bool CInput::StaticGamepadErrorRecoveryCallback(const char* stageTag,
                                                const char* errorDetail,
                                                u32 sehCode,
                                                u64 sehAddr,
                                                const char* contextOverride,
                                                const char* recoveryOverride,
                                                bool isLuaError,
                                                void* userData)
{
	CInput* self = static_cast<CInput*>(userData);
	if (!self)
		self = pInput;
	if (!self)
		return false;
	return self->HandleGamepadFrameError(stageTag, errorDetail, sehCode, sehAddr, contextOverride, recoveryOverride, isLuaError);
}

bool CInput::HandleGamepadFrameError(LPCSTR stageTag,
                                     LPCSTR errorDetail,
                                     u32 sehCode,
                                     u64 sehAddr,
                                     LPCSTR contextOverride,
                                     LPCSTR recoveryOverride,
                                     bool isLuaError)
{
	const LPCSTR safeStage = (stageTag && stageTag[0]) ? stageTag : "CInput::GamepadUpdate";
	const LPCSTR safeDetail = (errorDetail && errorDetail[0]) ? errorDetail : "Unexpected gamepad contour failure";

	// 1. Capture pre-reset state for gamepad_last_error.log
	gperr::ErrorContextInfo info = {};
	info.stage_tag = safeStage;
	info.error_detail = safeDetail;
	info.context_name = (contextOverride && contextOverride[0])
	                        ? contextOverride
	                        : (RDEVICE.Paused() ? "MainMenu" : "World_Gameplay");
	info.recovery_action = (recoveryOverride && recoveryOverride[0]) ? recoveryOverride : nullptr;
	info.seh_code = sehCode;
	info.seh_addr = sehAddr;
	info.frame_id = RDEVICE.dwFrame;
	info.time_continual_ms = RDEVICE.dwTimeContinual;
	info.gamepad_instance_id = m_gamepadInstanceId;
	info.buttons_mask = m_gamepadState.buttons_mask;
	for (int i = 0; i < COUNT_GAMEPAD_AXIS && i < 6; ++i)
	{
		info.axes[i] = m_gamepadState.axes[i];
	}
	info.was_gamepad_active_source = (m_activeInputSource == EActiveInputSource::Gamepad);
	info.was_device_paused = RDEVICE.Paused();
	info.pause_tokens_count = 0;
	info.is_lua_error = isLuaError;

	if (xr_strcmp(m_lastErrorStageTag, safeStage) == 0)
	{
		++m_errorRepeatCount;
	}
	else
	{
		xr_strcpy(m_lastErrorStageTag, sizeof(m_lastErrorStageTag), safeStage);
		m_errorRepeatCount = 1;
	}
	info.repeat_count = m_errorRepeatCount;

	// 2. Skip faulty gamepad frame & reset all gamepad axes/buttons/haptics to neutral + raise release barrier
	g_gp_in_sdl_update = false;
	ClearAllGamepadInjectedKeys();
	++m_gamepadFocusGen;
	if (m_pGamepadHaptics)
		m_pGamepadHaptics->StopAll("gamepad_error_guard");
	else if (m_pGamepadDeviceMgr)
		m_pGamepadDeviceMgr->StopDeviceRumble(0);

	m_bGamepadNeedReleaseAll = true;
	m_bGamepadLatchPending = true;
	ResetGamepadSnapshot(false);
	m_gamepadState.need_release_all = true;
	m_prevGamepadButtonsMask = 0;
	m_bStickDeflecting = false;
	m_stickDeflectStartTimeMs = 0;
	m_bStickRequireNeutralBeforeDeflect = true;
	m_bPrevTriggerOverThresh[0] = false;
	m_bPrevTriggerOverThresh[1] = false;
	m_bHasPendingSourceRequest = false;
	m_pendingSourceReason = nullptr;

	// 4. Write gamepad_last_error.log (rate-limited only if identical stage fires within 500 ms)
	const u32 nowMs = (dwCurTime != 0) ? dwCurTime : RDEVICE.dwTimeContinual;
	const bool shouldWriteDisk = (m_errorRepeatCount == 1) ||
	                             (m_lastErrorLogWriteMs == 0) ||
	                             (nowMs < m_lastErrorLogWriteMs) ||
	                             ((nowMs - m_lastErrorLogWriteMs) >= 500);

	bool wroteOk = false;
	if (shouldWriteDisk)
	{
		m_lastErrorLogWriteMs = (nowMs != 0) ? nowMs : 1;
		wroteOk = gperr::RecordEngineGamepadError(info);
		Msg("! [Gamepad] S3 Core Armor caught error at '%s' (detail='%s', seh=0x%08X, repeat=%u): gamepad reset to neutral, switched to KBM, report='%s' (ok=%d)",
		    safeStage,
		    safeDetail,
		    sehCode,
		    m_errorRepeatCount,
		    gperr::GetLastWrittenFilePath().c_str(),
		    wroteOk ? 1 : 0);
	}

	// 3. Switch active input source to Keyboard & Mouse immediately without cooldown.
	// CRASH-REPAIR 2026-11-11: after the report (step 4) - the switch runs listeners (glyphs, HUD, Lua);
	// if one of them crashes in a broken state, the report about the first error is already on disk.
	// S3 аварийный откат на КиМ всегда снимает блокировку «Только геймпад», чтобы игрок не остался без управления
	if (m_sourceLockMode == EInputSourceLockMode::GamepadOnly)
		m_sourceLockMode = EInputSourceLockMode::Auto;
	CommitActiveInputSource(EActiveInputSource::KBM, isLuaError ? "lua_gamepad_error_failover" : "cpp_gamepad_error_failover", true);
	return wroteOk;
}

void CInput::GamepadUpdate()
{
	u32 sehCode = 0;
	u64 sehAddr = 0;
	if (!gperr::ExecuteProtectedGamepadStepSEH(&CInput::GamepadUpdateStepThunk, this, sehCode, sehAddr))
	{
		HandleGamepadFrameError("CInput::GamepadUpdate", nullptr, sehCode, sehAddr);
	}
}

void CInput::GamepadUpdateInner()
{
	const gperr::ESimulatedFaultMode simMode = gperr::ConsumeSimulatedFaultMode();
	if (simMode == gperr::ESimulatedFaultMode::RaiseSehAccessViolationInPoll)
	{
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
	}
	else if (simMode == gperr::ESimulatedFaultMode::ThrowCppExceptionInPoll)
	{
		RaiseException(0xE06D7363u, 0, 0, nullptr);
	}

	const bool bFocused = (RDEVICE.m_hWnd != NULL) &&
	                      (GetForegroundWindow() == RDEVICE.m_hWnd) &&
	                      !IsIconic(RDEVICE.m_hWnd);

	if (!m_bSdlGamepadInitialized || !m_pGamepadDeviceMgr)
	{
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll("sdl_not_init");
		ResetGamepadSnapshot(bFocused);
		WriteGamepadStateTelemetryIfChanged();
		return;
	}

	const u32 focusGenBeforeSdl = m_gamepadFocusGen;

	bool activeRemoved = false;
	bool activeChanged = false;
	m_pGamepadDeviceMgr->PollHotplugAndSelectActive(dwCurTime, bFocused, activeRemoved, activeChanged);

	const SGamepadDeviceEntry* activeDev = m_pGamepadDeviceMgr->GetActiveDevice();
	// D1.1-N1: Never feed raw unmapped Class C joystick buttons/axes into m_gamepadState
	// GPSET-2: In KbmOnly («Отключить геймпад») mode, keep SDL hotplug/power info alive for settings UI,
	// but suppress feeding gamepad buttons/axes into gameplay and UI.
	const bool bGamepadDisabledByMode = (!m_bSourceSelfTestRunning && m_sourceLockMode == EInputSourceLockMode::KbmOnly);
	const SGamepadDeviceEntry* validGamepadDev =
	    (!bGamepadDisabledByMode && activeDev && activeDev->device_class != EGamepadClass::ClassC && activeDev->gamepad != nullptr)
	        ? activeDev
	        : nullptr;

	if (activeRemoved || !validGamepadDev)
	{
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll(activeRemoved ? "device_removed" : "no_valid_gamepad");
		// D1.1-N2: On active removal, force neutral snapshot this frame; if another gamepad took over
		// in the same frame, the next frame will see m_gamepadInstanceId (0) != validGamepadDev->instance_id
		// and raise the release barrier.
		m_pGamepad = nullptr;
		m_gamepadInstanceId = 0;
		m_gamepadReleaseMask = 0;
		m_bGamepadNeedReleaseAll = false;
		m_bGamepadLatchPending = false;
		ClearAllGamepadInjectedKeys();
		ResetGamepadSnapshot(bFocused);
	}
	else if (m_gamepadInstanceId != validGamepadDev->instance_id || activeChanged)
	{
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll("active_device_changed");
		m_pGamepad = validGamepadDev->gamepad;
		m_gamepadInstanceId = validGamepadDev->instance_id;
		m_bGamepadNeedReleaseAll = true;
		m_bGamepadLatchPending = true;
	}

	const bool bFocusedAfter = (RDEVICE.m_hWnd != NULL) &&
	                           (GetForegroundWindow() == RDEVICE.m_hWnd) &&
	                           !IsIconic(RDEVICE.m_hWnd);
	const bool bFocusGenChanged = (m_gamepadFocusGen != focusGenBeforeSdl);
	if (!bFocusedAfter || bFocusGenChanged)
	{
		if (bFocusGenChanged || (bFocused != bFocusedAfter))
		{
			string128 line;
			xr_sprintf(line, sizeof(line), "focus_changed_in_sdl,%u", dwCurTime);
			WriteGamepadTelemetryLine(line);
		}
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll("focus_lost_in_sdl");
		else
			m_pGamepadDeviceMgr->StopDeviceRumble(0);
		m_bGamepadNeedReleaseAll = true;
		m_bGamepadLatchPending = true;
		ResetGamepadSnapshot(false);
		WriteGamepadStateTelemetryIfChanged();
		return;
	}

	if (activeRemoved || !validGamepadDev)
	{
		m_gamepadReleaseMask = 0;
		m_bGamepadNeedReleaseAll = false;
		m_bGamepadLatchPending = false;
		ResetGamepadSnapshot(true);
		m_gamepadState.need_release_all = false;
	}
	else
	{
		const u32 rawButtons = validGamepadDev->raw_buttons;

		if (m_bGamepadLatchPending)
		{
			m_gamepadReleaseMask |= rawButtons;
			m_bGamepadLatchPending = false;
		}

		m_gamepadReleaseMask &= rawButtons;
		if (m_gamepadReleaseMask == 0)
			m_bGamepadNeedReleaseAll = false;

		for (int i = 0; i < COUNT_GAMEPAD_AXIS; ++i)
			m_gamepadState.axes[i] = validGamepadDev->raw_axes[i];

		m_gamepadState.buttons_mask = rawButtons & ~m_gamepadReleaseMask;
		m_gamepadState.instance_id = m_gamepadInstanceId;
		m_gamepadState.timestamp_ms = dwCurTime;
		m_gamepadState.connected = true;
		m_gamepadState.has_focus = true;
		m_gamepadState.need_release_all = m_bGamepadNeedReleaseAll;

		// D4.1 / D4.2: Emergency binding reset (hold Start + Back for 5000 ms in any context)
		static u32 s_prevEmergencyTickMs = 0;
		const u32 dtEmergencyMs = (s_prevEmergencyTickMs != 0 && dwCurTime >= s_prevEmergencyTickMs && (dwCurTime - s_prevEmergencyTickMs) <= 250)
		                              ? (dwCurTime - s_prevEmergencyTickMs)
		                              : 16;
		s_prevEmergencyTickMs = dwCurTime;
		gpbind::UpdateActiveEmergencyResetHold(rawButtons, dtEmergencyMs);
	}

	if (m_pGamepadHaptics)
	{
		m_pGamepadHaptics->OnFrame(dwCurTime, true);
	}

	WriteGamepadStateTelemetryIfChanged();
}

void CInput::LogGamepadDevices() const
{
	if (m_pGamepadDeviceMgr)
		m_pGamepadDeviceMgr->LogDevices();
}

void CInput::RunVirtualGamepadHotplugTest(int cycles)
{
	if (m_pGamepadDeviceMgr)
		m_pGamepadDeviceMgr->RunVirtualHotplugTest(cycles);
}

namespace
{
	SRWLOCK g_gpTelemetryFileLock = SRWLOCK_INIT;
}

void CInput::CloseGamepadTelemetryFile()
{
	AcquireSRWLockExclusive(&g_gpTelemetryFileLock);
	if (m_pGamepadTelemetryFile)
	{
		fclose(m_pGamepadTelemetryFile);
		m_pGamepadTelemetryFile = nullptr;
	}
	ReleaseSRWLockExclusive(&g_gpTelemetryFileLock);
}

void CInput::FlushGamepadTelemetryIfDue()
{
	AcquireSRWLockExclusive(&g_gpTelemetryFileLock);
	if (m_pGamepadTelemetryFile)
	{
		const u32 now = GetTickCount();
		if (now - m_gamepadTelemetryLastFlushMs >= 1000u)
		{
			fflush(m_pGamepadTelemetryFile);
			m_gamepadTelemetryLastFlushMs = now;
		}
	}
	ReleaseSRWLockExclusive(&g_gpTelemetryFileLock);
}

void CInput::WriteGamepadTelemetryLine(LPCSTR line)
{
	if (!ps_gp_telemetry)
	{
		// FR.1 №4: телеметрию выключили — закрываем файл со сбросом буфера
		CloseGamepadTelemetryFile();
		return;
	}
	if (!line || !line[0])
		return;

	AcquireSRWLockExclusive(&g_gpTelemetryFileLock);
	if (!m_pGamepadTelemetryFile)
	{
		string_path telemetryPath;
		FS.update_path(telemetryPath, "$logs$", "gamepad_telemetry.csv");
		m_pGamepadTelemetryFile = _fsopen(telemetryPath, "ab", _SH_DENYNO);
		if (m_pGamepadTelemetryFile)
			m_gamepadTelemetryLastFlushMs = GetTickCount();
	}

	if (FILE* f = m_pGamepadTelemetryFile)
	{
		if (!m_bGamepadTelemetryHeaderWritten)
		{
			fprintf(f, "# dwCurTime,connected,instance_id,lx,ly,rx,ry,lt,rt,buttons_mask,has_focus,need_release_all\r\n");
			m_bGamepadTelemetryHeaderWritten = true;
		}
		fprintf(f, "%s\r\n", line);
		const u32 now = GetTickCount();
		if (now - m_gamepadTelemetryLastFlushMs >= 1000u)
		{
			fflush(f);
			m_gamepadTelemetryLastFlushMs = now;
		}
	}
	ReleaseSRWLockExclusive(&g_gpTelemetryFileLock);
}

void CInput::WriteGamepadStateTelemetryIfChanged()
{
	if (!ps_gp_telemetry)
	{
		m_bGamepadHasLoggedState = false;
		CloseGamepadTelemetryFile();
		return;
	}
	FlushGamepadTelemetryIfDue();

	const bool changed = !m_bGamepadHasLoggedState ||
	                     (m_gamepadState.connected != m_gamepadLastLoggedState.connected) ||
	                     (m_gamepadState.instance_id != m_gamepadLastLoggedState.instance_id) ||
	                     (m_gamepadState.buttons_mask != m_gamepadLastLoggedState.buttons_mask) ||
	                     (m_gamepadState.has_focus != m_gamepadLastLoggedState.has_focus) ||
	                     (m_gamepadState.need_release_all != m_gamepadLastLoggedState.need_release_all) ||
	                     (memcmp(m_gamepadState.axes, m_gamepadLastLoggedState.axes, sizeof(m_gamepadState.axes)) != 0);

	if (!changed)
		return;

	m_gamepadLastLoggedState = m_gamepadState;
	m_bGamepadHasLoggedState = true;

	string256 line;
	xr_sprintf(line, sizeof(line), "%u,%d,%u,%d,%d,%d,%d,%d,%d,0x%08X,%d,%d",
	           m_gamepadState.timestamp_ms,
	           m_gamepadState.connected ? 1 : 0,
	           m_gamepadState.instance_id,
	           static_cast<int>(m_gamepadState.axes[0]),
	           static_cast<int>(m_gamepadState.axes[1]),
	           static_cast<int>(m_gamepadState.axes[2]),
	           static_cast<int>(m_gamepadState.axes[3]),
	           static_cast<int>(m_gamepadState.axes[4]),
	           static_cast<int>(m_gamepadState.axes[5]),
	           m_gamepadState.buttons_mask,
	           m_gamepadState.has_focus ? 1 : 0,
	           m_gamepadState.need_release_all ? 1 : 0);
	WriteGamepadTelemetryLine(line);
}

void CInput::LogGamepadState() const
{
	string512 ringBufStr;
	ringBufStr[0] = '\0';
	const u32 totalWndProc = g_gp_wndproc_in_sdl_count;
	const u32 ringCount = (totalWndProc < GAMEPAD_WNDPROC_RING_SIZE) ? totalWndProc : GAMEPAD_WNDPROC_RING_SIZE;
	const u32 startIdx = (totalWndProc > GAMEPAD_WNDPROC_RING_SIZE) ? (totalWndProc - GAMEPAD_WNDPROC_RING_SIZE) : 0;
	if (ringCount == 0)
	{
		xr_strcpy(ringBufStr, sizeof(ringBufStr), "none");
	}
	else
	{
		size_t offset = 0;
		for (u32 i = 0; i < ringCount; ++i)
		{
			const UINT msg = g_gp_wndproc_in_sdl_msgs[(startIdx + i) % GAMEPAD_WNDPROC_RING_SIZE];
			const int written = xr_sprintf(ringBufStr + offset, sizeof(ringBufStr) - offset,
			                               (i == 0) ? "0x%04X" : ",0x%04X", msg);
			if (written <= 0)
				break;
			offset += static_cast<size_t>(written);
			if (offset >= sizeof(ringBufStr) - 1)
				break;
		}
	}

	Msg("* [Gamepad] gp_state: dwCurTime=%u source=%s switches_1m=%u connected=%d id=%u axes=[%d,%d,%d,%d,%d,%d] buttons=0x%08X focus=%d need_release=%d telemetry=%d wndproc_in_sdl=%u wndproc_msgs=[%s]",
	    m_gamepadState.timestamp_ms,
	    GetInputSourceName(m_activeInputSource),
	    GetSourceSwitchesLastMinute(),
	    m_gamepadState.connected ? 1 : 0,
	    m_gamepadState.instance_id,
	    static_cast<int>(m_gamepadState.axes[0]),
	    static_cast<int>(m_gamepadState.axes[1]),
	    static_cast<int>(m_gamepadState.axes[2]),
	    static_cast<int>(m_gamepadState.axes[3]),
	    static_cast<int>(m_gamepadState.axes[4]),
	    static_cast<int>(m_gamepadState.axes[5]),
	    m_gamepadState.buttons_mask,
	    m_gamepadState.has_focus ? 1 : 0,
	    m_gamepadState.need_release_all ? 1 : 0,
	    ps_gp_telemetry,
	    totalWndProc,
	    ringBufStr);
}

void CInput::PruneSourceSwitchHistory(u32 nowMs) const
{
	constexpr u32 WINDOW_MS = 60000;
	while (!m_sourceSwitchTimestampsMs.empty())
	{
		const u32 ts = m_sourceSwitchTimestampsMs.front();
		if (nowMs >= ts && (nowMs - ts) > WINDOW_MS)
		{
			m_sourceSwitchTimestampsMs.erase(m_sourceSwitchTimestampsMs.begin());
		}
		else
		{
			break;
		}
	}
}

u32 CInput::GetSourceSwitchesLastMinute() const
{
	PruneSourceSwitchHistory(dwCurTime);
	return static_cast<u32>(m_sourceSwitchTimestampsMs.size());
}

void CInput::AddInputSourceChangedCallback(FInputSourceChangedCallback cb)
{
	if (!cb)
		return;
	if (std::find(m_sourceChangedCallbacks.begin(), m_sourceChangedCallbacks.end(), cb) == m_sourceChangedCallbacks.end())
	{
		m_sourceChangedCallbacks.push_back(cb);
	}
}

void CInput::RemoveInputSourceChangedCallback(FInputSourceChangedCallback cb)
{
	auto it = std::find(m_sourceChangedCallbacks.begin(), m_sourceChangedCallbacks.end(), cb);
	if (it != m_sourceChangedCallbacks.end())
	{
		m_sourceChangedCallbacks.erase(it);
	}
}

void CInput::CommitActiveInputSource(EActiveInputSource newSource, LPCSTR reason, bool ignoreCooldown)
{
	if (m_activeInputSource == newSource)
		return;

	const EActiveInputSource prevSource = m_activeInputSource;
	m_activeInputSource = newSource;
	if (!ignoreCooldown)
	{
		m_lastSourceSwitchTimeMs = dwCurTime;
	}
	m_bStickDeflecting = false;
	m_stickDeflectStartTimeMs = 0;
	// D1.2b-N3: if we just switched to KBM, require the stick to return below threshold (or change after switch)
	// before a held stick can trigger a switch back to Gamepad.
	m_bStickRequireNeutralBeforeDeflect = (newSource == EActiveInputSource::KBM);
	m_bHasPendingSourceRequest = false;
	m_pendingSourceReason = nullptr;

	++m_totalSourceSwitches;
	m_sourceSwitchTimestampsMs.push_back(dwCurTime);
	PruneSourceSwitchHistory(dwCurTime);
	const u32 switches1m = static_cast<u32>(m_sourceSwitchTimestampsMs.size());

	// D1.2b-N2: suppress real log/telemetry/callbacks while deterministic selftest is running
	if (m_bSourceSelfTestRunning)
		return;

	// D5.2 + CR1-03: Stop vibration and clear any held gamepad-injected DIK state on switch to KBM
	if (newSource == EActiveInputSource::KBM)
	{
		ClearAllGamepadInjectedKeys();
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll("source_switched_kbm");
	}

	LPCSTR safeReason = (reason && reason[0]) ? reason : "unknown";
	Msg("* [Gamepad] Active input source switched: %s -> %s (reason='%s', dwCurTime=%u, switches_1m=%u, total=%u)",
	    GetInputSourceName(prevSource),
	    GetInputSourceName(newSource),
	    safeReason,
	    dwCurTime,
	    switches1m,
	    m_totalSourceSwitches);

	if (ps_gp_telemetry)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_source_switched,%u,from=%s,to=%s,reason=%s,switches_1m=%u,total=%u",
		           dwCurTime,
		           GetInputSourceName(prevSource),
		           GetInputSourceName(newSource),
		           safeReason,
		           switches1m,
		           m_totalSourceSwitches);
		WriteGamepadTelemetryLine(line);
	}

	// Copy callback list before invocation in case a listener modifies subscriptions
	const xr_vector<FInputSourceChangedCallback> callbacksCopy = m_sourceChangedCallbacks;
	for (FInputSourceChangedCallback cb : callbacksCopy)
	{
		if (cb)
		{
			cb(newSource, prevSource, safeReason, dwCurTime);
		}
	}
}

void CInput::ForceActiveInputSource(EActiveInputSource src, LPCSTR reason)
{
	CommitActiveInputSource(src, (reason && reason[0]) ? reason : "console_force");
}

void CInput::UpdateInputSourceArbitration()
{
	u32 sehCode = 0;
	u64 sehAddr = 0;
	if (!gperr::ExecuteProtectedGamepadStepSEH(&CInput::SourceArbitrationStepThunk, this, sehCode, sehAddr))
	{
		HandleGamepadFrameError("CInput::UpdateInputSourceArbitration", nullptr, sehCode, sehAddr);
	}
}

void CInput::UpdateInputSourceArbitrationInner()
{
	// D1.2b-N7: If active gamepad is unplugged while source is Gamepad, immediately switch to KBM (no cooldown)
	const bool curConnected = m_gamepadState.connected;
	const bool wasConnected = m_bPrevGamepadConnectedForSource;
	m_bPrevGamepadConnectedForSource = curConnected;

	if (!curConnected)
	{
		m_bStickDeflecting = false;
		m_stickDeflectStartTimeMs = 0;
		m_bStickRequireNeutralBeforeDeflect = false;
		m_bPrevTriggerOverThresh[0] = false;
		m_bPrevTriggerOverThresh[1] = false;
		m_prevGamepadButtonsMask = 0;
		if (m_bHasPendingSourceRequest && m_pendingSourceRequest == EActiveInputSource::Gamepad)
		{
			m_bHasPendingSourceRequest = false;
			m_pendingSourceReason = nullptr;
		}
		if (wasConnected && m_activeInputSource == EActiveInputSource::Gamepad)
		{
			CommitActiveInputSource(EActiveInputSource::KBM, "gp_disconnected", true);
			return;
		}
	}

	// GPSET-2: обработка режимов блокировки источника ввода (кроме детерминированного selftest)
	if (!m_bSourceSelfTestRunning)
	{
		if (m_sourceLockMode == EInputSourceLockMode::KbmOnly)
		{
			m_bHasPendingSourceRequest = false;
			m_pendingSourceReason = nullptr;
			if (m_activeInputSource != EActiveInputSource::KBM)
				CommitActiveInputSource(EActiveInputSource::KBM, "source_lock_kbm_only", true);
			return;
		}
		if (m_sourceLockMode == EInputSourceLockMode::GamepadOnly && curConnected)
		{
			m_bHasPendingSourceRequest = false;
			m_pendingSourceReason = nullptr;
			m_prevGamepadButtonsMask = m_gamepadState.buttons_mask;
			if (m_activeInputSource != EActiveInputSource::Gamepad)
				CommitActiveInputSource(EActiveInputSource::Gamepad, "source_lock_gamepad_only", true);
			return;
		}
	}

	const bool bFocused = m_gamepadState.has_focus;
	if (!bFocused)
	{
		m_bStickDeflecting = false;
		m_stickDeflectStartTimeMs = 0;
		m_bStickRequireNeutralBeforeDeflect = false;
		m_bPrevTriggerOverThresh[0] = false;
		m_bPrevTriggerOverThresh[1] = false;
		m_bHasPendingSourceRequest = false;
		m_pendingSourceReason = nullptr;
		m_prevGamepadButtonsMask = m_gamepadState.buttons_mask;
		return;
	}

	// Clamp config values to safe bounds (D1.2-N5: dz comes from calibration profile)
	const float dz = std::clamp(gpmath::GetCalibrationStickDeadzone(), 0.0f, 0.70f);
	const float hyst = std::clamp(ps_gp_source_stick_hysteresis, 0.0f, 0.50f);
	const float stickThreshold = std::clamp(dz + hyst, 0.05f, 0.98f);
	const u32 holdRequiredMs = static_cast<u32>(std::clamp(ps_gp_source_stick_hold_ms, 0, 2000));
	const int mousePxThresh = std::clamp(ps_gp_source_mouse_px_threshold, 1, 200);
	const u32 cooldownMs = static_cast<u32>(std::clamp(ps_gp_source_cooldown_ms, 0, 5000));

	// 1. Evaluate Gamepad activity
	const u32 curButtons = curConnected ? m_gamepadState.buttons_mask : 0;
	const u32 newlyPressedButtons = curButtons & ~m_prevGamepadButtonsMask;
	m_prevGamepadButtonsMask = curButtons;

	// D1.2b-N3: Triggers (LT = axis 4, RT = axis 5) switch source only on rising edge across threshold,
	// while triggerHeldLevel tracks whether a trigger is currently held for menu anti-chatter.
	bool triggerRisingEdge = false;
	bool triggerHeldLevel = false;
	float maxStickMag = 0.0f;
	if (curConnected)
	{
		constexpr float INV_AXIS_MAX = 1.0f / 32767.0f;
		const float lx = std::clamp(static_cast<float>(m_gamepadState.axes[0]) * INV_AXIS_MAX, -1.0f, 1.0f);
		const float ly = std::clamp(static_cast<float>(m_gamepadState.axes[1]) * INV_AXIS_MAX, -1.0f, 1.0f);
		const float rx = std::clamp(static_cast<float>(m_gamepadState.axes[2]) * INV_AXIS_MAX, -1.0f, 1.0f);
		const float ry = std::clamp(static_cast<float>(m_gamepadState.axes[3]) * INV_AXIS_MAX, -1.0f, 1.0f);

		const float magL = std::sqrt(lx * lx + ly * ly);
		const float magR = std::sqrt(rx * rx + ry * ry);
		maxStickMag = (magL > magR) ? magL : magR;

		const float lt = std::clamp(static_cast<float>(m_gamepadState.axes[4]) * INV_AXIS_MAX, 0.0f, 1.0f);
		const float rt = std::clamp(static_cast<float>(m_gamepadState.axes[5]) * INV_AXIS_MAX, 0.0f, 1.0f);
		const bool ltOver = (lt >= stickThreshold);
		const bool rtOver = (rt >= stickThreshold);
		triggerHeldLevel = ltOver || rtOver;
		if ((ltOver && !m_bPrevTriggerOverThresh[0]) || (rtOver && !m_bPrevTriggerOverThresh[1]))
		{
			triggerRisingEdge = true;
		}
		m_bPrevTriggerOverThresh[0] = ltOver;
		m_bPrevTriggerOverThresh[1] = rtOver;
	}

	// D1.2b-N3: Stick hold is only counted if the stick deflection started after the last switch to KBM
	// (i.e. if the stick was already held when switching to KBM, it must first drop below stickThreshold).
	bool stickHeldEnough = false;
	if (curConnected && maxStickMag >= stickThreshold)
	{
		if (!m_bStickRequireNeutralBeforeDeflect)
		{
			if (!m_bStickDeflecting)
			{
				m_bStickDeflecting = true;
				m_stickDeflectStartTimeMs = dwCurTime;
			}
			const u32 elapsedStickMs = (dwCurTime >= m_stickDeflectStartTimeMs) ? (dwCurTime - m_stickDeflectStartTimeMs) : 0;
			if (elapsedStickMs >= holdRequiredMs)
			{
				stickHeldEnough = true;
			}
		}
	}
	else
	{
		m_bStickDeflecting = false;
		m_stickDeflectStartTimeMs = 0;
		m_bStickRequireNeutralBeforeDeflect = false;
	}

	const bool gamepadWantsActive = (newlyPressedButtons != 0) || triggerRisingEdge || stickHeldEnough;
	LPCSTR gamepadReason = (newlyPressedButtons != 0) ? "gp_button" : (triggerRisingEdge ? "gp_trigger" : "gp_stick_hold");

	// 2. Evaluate Keyboard/Mouse activity
	const long long dx = static_cast<long long>(m_frameMouseDeltaX);
	const long long dy = static_cast<long long>(m_frameMouseDeltaY);
	const long long distSq = dx * dx + dy * dy;
	const long long threshSq = static_cast<long long>(mousePxThresh) * static_cast<long long>(mousePxThresh);
	const bool mouseMovedOverThresh = (distSq > threshSq);
	const bool kbmDiscretePressed = m_bFrameKbKeyPressed || m_bFrameMouseBtnPressed || (m_frameMouseWheelDelta != 0);

	// Anti-chatter hysteresis in menus: if gamepad stick or button is actively held right now,
	// or virtual cursor was just moved/snapped by gamepad (FG.1 SetUICursorPosition / Soft-Snap / D-Pad Snap),
	// ignore small/concurrent mouse movement (only switch on discrete KBM press/click/wheel,
	// or physical mouse movement when gamepad and virtual cursor are at rest).
	const bool virtualCursorMovedRecently = IsGamepadCursorRecentlyMoved(dwCurTime, 60);
	const bool gamepadActivelyHeld = (curButtons != 0) || triggerHeldLevel || (maxStickMag >= dz) || virtualCursorMovedRecently;
	const bool kbmWantsActive = kbmDiscretePressed || (mouseMovedOverThresh && !gamepadActivelyHeld);
	LPCSTR kbmReason = kbmDiscretePressed
	                       ? (m_bFrameKbKeyPressed ? "kb_key" : (m_bFrameMouseBtnPressed ? "mouse_btn" : "mouse_wheel"))
	                       : "mouse_move";

	// 3. Cooldown gate + D1.2b-N4 deferred pending request:
	// Discrete button/key presses inside the 500 ms cooldown window are remembered as a pending request
	// and applied once cooldown expires (unless cancelled by a subsequent opposite input).
	const bool cooldownExpired = (m_lastSourceSwitchTimeMs == 0) ||
	                             (dwCurTime >= m_lastSourceSwitchTimeMs && (dwCurTime - m_lastSourceSwitchTimeMs) >= cooldownMs);

	if (!cooldownExpired)
	{
		if (m_activeInputSource == EActiveInputSource::KBM)
		{
			if (kbmWantsActive)
			{
				m_bHasPendingSourceRequest = false;
				m_pendingSourceReason = nullptr;
			}
			else if (gamepadWantsActive)
			{
				m_bHasPendingSourceRequest = true;
				m_pendingSourceRequest = EActiveInputSource::Gamepad;
				m_pendingSourceReason = gamepadReason;
			}
		}
		else // m_activeInputSource == EActiveInputSource::Gamepad
		{
			if (gamepadWantsActive)
			{
				m_bHasPendingSourceRequest = false;
				m_pendingSourceReason = nullptr;
			}
			else if (kbmWantsActive)
			{
				m_bHasPendingSourceRequest = true;
				m_pendingSourceRequest = EActiveInputSource::KBM;
				m_pendingSourceReason = kbmReason;
			}
		}
		return;
	}

	if (m_activeInputSource == EActiveInputSource::KBM)
	{
		if (gamepadWantsActive)
		{
			CommitActiveInputSource(EActiveInputSource::Gamepad, gamepadReason);
		}
		else if (m_bHasPendingSourceRequest && m_pendingSourceRequest == EActiveInputSource::Gamepad && !kbmWantsActive)
		{
			CommitActiveInputSource(EActiveInputSource::Gamepad, m_pendingSourceReason ? m_pendingSourceReason : "gp_button");
		}
		else
		{
			m_bHasPendingSourceRequest = false;
			m_pendingSourceReason = nullptr;
		}
	}
	else // m_activeInputSource == EActiveInputSource::Gamepad
	{
		if (kbmWantsActive)
		{
			CommitActiveInputSource(EActiveInputSource::KBM, kbmReason);
		}
		else if (m_bHasPendingSourceRequest && m_pendingSourceRequest == EActiveInputSource::KBM && !gamepadWantsActive)
		{
			CommitActiveInputSource(EActiveInputSource::KBM, m_pendingSourceReason ? m_pendingSourceReason : "kb_key");
		}
		else
		{
			m_bHasPendingSourceRequest = false;
			m_pendingSourceReason = nullptr;
		}
	}
}

void CInput::NotifyGamepadCursorMoved(u32 nowMs)
{
	const u32 t = (nowMs != 0) ? nowMs : RDEVICE.dwTimeContinual;
	m_lastGamepadCursorMoveMs = (t != 0) ? t : 1;
}

bool CInput::IsGamepadCursorRecentlyMoved(u32 nowMs, u32 windowMs) const
{
	if (m_lastGamepadCursorMoveMs == 0)
		return false;
	const u32 t = (nowMs != 0) ? nowMs : RDEVICE.dwTimeContinual;
	return (t >= m_lastGamepadCursorMoveMs) && ((t - m_lastGamepadCursorMoveMs) <= windowMs);
}

LPCSTR CInput::GetSourceLockModeName(EInputSourceLockMode mode)
{
	switch (mode)
	{
	case EInputSourceLockMode::GamepadOnly: return "gamepad_only";
	case EInputSourceLockMode::KbmOnly:     return "kbm_only";
	default:                                return "auto";
	}
}

bool CInput::ParseSourceLockMode(LPCSTR str, EInputSourceLockMode& outMode)
{
	outMode = EInputSourceLockMode::Auto;
	if (!str || !str[0])
		return false;

	std::string s(str);
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
	s = s.substr(b, e - b);
	for (char& ch : s)
		if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');

	if (s == "auto" || s == "default" || s == "0")
	{
		outMode = EInputSourceLockMode::Auto;
		return true;
	}
	if (s == "gamepad_only" || s == "only_gamepad" || s == "gamepad_lock" || s == "lock_gamepad" || s == "1")
	{
		outMode = EInputSourceLockMode::GamepadOnly;
		return true;
	}
	if (s == "kbm_only" || s == "only_kbm" || s == "off" || s == "disabled" || s == "disable_gamepad" || s == "2")
	{
		outMode = EInputSourceLockMode::KbmOnly;
		return true;
	}
	return false;
}

void CInput::LoadSourceLockModeFromProfile()
{
	m_sourceLockMode = EInputSourceLockMode::Auto;
	if (!FS.path_exist("$app_data_root$"))
		return;
	string_path pathBuf = {};
	FS.update_path(pathBuf, "$app_data_root$", "gamepad\\profile.ltx");
	std::ifstream ifs(pathBuf, std::ios::binary);
	if (!ifs)
		return;

	std::string line, currentSec;
	while (std::getline(ifs, line))
	{
		const size_t semi = line.find(';');
		if (semi != std::string::npos)
			line = line.substr(0, semi);
		size_t b = 0, e = line.size();
		while (b < e && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) ++b;
		while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) --e;
		line = line.substr(b, e - b);
		if (line.empty())
			continue;

		if (line.front() == '[' && line.back() == ']')
		{
			currentSec = line.substr(1, line.size() - 2);
			size_t sb = 0, se = currentSec.size();
			while (sb < se && (currentSec[sb] == ' ' || currentSec[sb] == '\t')) ++sb;
			while (se > sb && (currentSec[se - 1] == ' ' || currentSec[se - 1] == '\t')) --se;
			currentSec = currentSec.substr(sb, se - sb);
			for (char& ch : currentSec)
				if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
			continue;
		}
		if (currentSec != "source")
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		size_t kb = 0, ke = key.size();
		while (kb < ke && (key[kb] == ' ' || key[kb] == '\t')) ++kb;
		while (ke > kb && (key[ke - 1] == ' ' || key[ke - 1] == '\t')) --ke;
		key = key.substr(kb, ke - kb);
		for (char& ch : key)
			if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
		if (key == "mode" || key == "source_mode" || key == "lock_mode")
		{
			EInputSourceLockMode parsed = EInputSourceLockMode::Auto;
			if (ParseSourceLockMode(line.substr(eq + 1).c_str(), parsed))
				m_sourceLockMode = parsed;
		}
	}
}

bool CInput::SaveSourceLockModeToProfile() const
{
	if (!FS.path_exist("$app_data_root$"))
		return false;
	string_path pathBuf = {};
	FS.update_path(pathBuf, "$app_data_root$", "gamepad\\profile.ltx");

	std::string forbidReason;
	if (gpbind::BindingProfileModel::IsPathForbiddenForWrite(pathBuf, &forbidReason))
	{
		Msg("! [Gamepad] CInput::SaveSourceLockModeToProfile refused path '%s': %s", pathBuf, forbidReason.c_str());
		return false;
	}

	std::vector<std::string> preservedLines;
	{
		std::ifstream ifs(pathBuf, std::ios::binary);
		if (ifs)
		{
			std::string rawLine;
			bool inSourceSection = false;
			while (std::getline(ifs, rawLine))
			{
				if (!rawLine.empty() && rawLine.back() == '\r')
					rawLine.pop_back();
				std::string stripped = rawLine;
				const size_t semi = stripped.find(';');
				if (semi != std::string::npos)
					stripped = stripped.substr(0, semi);
				size_t b = 0, e = stripped.size();
				while (b < e && (stripped[b] == ' ' || stripped[b] == '\t')) ++b;
				while (e > b && (stripped[e - 1] == ' ' || stripped[e - 1] == '\t')) --e;
				stripped = stripped.substr(b, e - b);
				if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']')
				{
					std::string secName = stripped.substr(1, stripped.size() - 2);
					size_t sb = 0, se = secName.size();
					while (sb < se && (secName[sb] == ' ' || secName[sb] == '\t')) ++sb;
					while (se > sb && (secName[se - 1] == ' ' || secName[se - 1] == '\t')) --se;
					secName = secName.substr(sb, se - sb);
					for (char& ch : secName)
						if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
					inSourceSection = (secName == "source");
				}
				if (!inSourceSection)
					preservedLines.push_back(rawLine);
			}
		}
	}

	{
		std::error_code dirEc;
		const std::filesystem::path parentDir = std::filesystem::path(pathBuf).parent_path();
		if (!parentDir.empty())
			std::filesystem::create_directories(parentDir, dirEc);
	}

	const std::string tmpPath = GpTempPathFor(pathBuf);
	{
		std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
		if (!ofs)
			return false;
		for (const auto& l : preservedLines)
			ofs << l << "\n";
		if (!preservedLines.empty() && !preservedLines.back().empty())
			ofs << "\n";
		ofs << "[source]\n";
		ofs << "mode = " << GetSourceLockModeName(m_sourceLockMode) << "\n";
		ofs.flush();
		if (!ofs.good())
		{
			ofs.close();
			DeleteFileA(tmpPath.c_str());
			return false;
		}
	}
	return GpCommitTempFile(tmpPath.c_str(), pathBuf);
}

bool CInput::SetInputSourceLockMode(EInputSourceLockMode mode, bool saveToDisk, LPCSTR reason)
{
	m_sourceLockMode = mode;
	LPCSTR safeReason = (reason && reason[0]) ? reason : "source_mode_set";
	if (mode == EInputSourceLockMode::KbmOnly)
	{
		ClearAllGamepadInjectedKeys();
		if (m_pGamepadHaptics)
			m_pGamepadHaptics->StopAll("source_lock_kbm_only");
		m_gamepadReleaseMask = 0;
		m_bGamepadNeedReleaseAll = false;
		m_bGamepadLatchPending = false;
		ResetGamepadSnapshot(m_gamepadState.has_focus);
		CommitActiveInputSource(EActiveInputSource::KBM, safeReason, true);
	}
	else if (mode == EInputSourceLockMode::GamepadOnly)
	{
		const SGamepadDeviceEntry* activeDev = m_pGamepadDeviceMgr ? m_pGamepadDeviceMgr->GetActiveDevice() : nullptr;
		const bool hasValidPad = m_gamepadState.connected ||
		                         (activeDev && activeDev->device_class != EGamepadClass::ClassC && activeDev->gamepad != nullptr);
		if (hasValidPad)
			CommitActiveInputSource(EActiveInputSource::Gamepad, safeReason, true);
	}
	if (saveToDisk)
		SaveSourceLockModeToProfile();
	return true;
}

bool CInput::SetInputSourceLockModeByName(LPCSTR modeName, bool saveToDisk, LPCSTR reason)
{
	EInputSourceLockMode parsed = EInputSourceLockMode::Auto;
	if (!ParseSourceLockMode(modeName, parsed))
		return false;
	return SetInputSourceLockMode(parsed, saveToDisk, reason);
}

LPCSTR CInput::GetActiveGamepadName() const
{
	m_cachedActiveGamepadName[0] = '\0';
	if (!m_pGamepadDeviceMgr)
		return m_cachedActiveGamepadName;
	const SGamepadDeviceEntry* activeDev = m_pGamepadDeviceMgr->GetActiveDevice();
	if (!activeDev)
		return m_cachedActiveGamepadName;

	if (activeDev->gamepad)
	{
		const char* sdlName = SDL_GetGamepadName(activeDev->gamepad);
		if (sdlName && sdlName[0])
		{
			xr_strcpy(m_cachedActiveGamepadName, sdlName);
			return m_cachedActiveGamepadName;
		}
	}
	if (activeDev->name[0])
		xr_strcpy(m_cachedActiveGamepadName, activeDev->name);
	return m_cachedActiveGamepadName;
}

LPCSTR CInput::GetActiveGamepadPowerInfo(int& outPercent) const
{
	outPercent = -1;
	if (!m_pGamepadDeviceMgr)
		return "disconnected";
	const SGamepadDeviceEntry* activeDev = m_pGamepadDeviceMgr->GetActiveDevice();
	if (!activeDev)
		return "disconnected";
	if (!activeDev->gamepad)
		return activeDev->joystick ? "unknown" : "disconnected";

	int pct = -1;
	const SDL_PowerState st = SDL_GetGamepadPowerInfo(activeDev->gamepad, &pct);
	if (pct >= 0 && pct <= 100)
		outPercent = pct;

	switch (st)
	{
	case SDL_POWERSTATE_ON_BATTERY:
		return "on_battery";
	case SDL_POWERSTATE_NO_BATTERY:
		return "wired";
	case SDL_POWERSTATE_CHARGING:
		return "charging";
	case SDL_POWERSTATE_CHARGED:
		if (outPercent < 0)
			outPercent = 100;
		return "charged";
	case SDL_POWERSTATE_UNKNOWN:
	case SDL_POWERSTATE_ERROR:
	default:
		return "unknown";
	}
}

void CInput::LogInputSourceState() const
{
	const float dz = std::clamp(gpmath::GetCalibrationStickDeadzone(), 0.0f, 0.70f);
	const float hyst = std::clamp(ps_gp_source_stick_hysteresis, 0.0f, 0.50f);
	int powerPct = -1;
	LPCSTR powerSt = GetActiveGamepadPowerInfo(powerPct);
	Msg("* [Gamepad] gp_source: active=%s mode=%s pad='%s' power=%s(%d%%) switches_1m=%u total_switches=%u last_switch_ms=%u dwCurTime=%u params(dz=%.2f hyst=%.2f thresh=%.2f hold_ms=%d mouse_counts=%d cooldown_ms=%d) listeners=%zu",
	    GetInputSourceName(m_activeInputSource),
	    GetSourceLockModeName(m_sourceLockMode),
	    GetActiveGamepadName(),
	    powerSt,
	    powerPct,
	    GetSourceSwitchesLastMinute(),
	    m_totalSourceSwitches,
	    m_lastSourceSwitchTimeMs,
	    dwCurTime,
	    dz,
	    hyst,
	    dz + hyst,
	    ps_gp_source_stick_hold_ms,
	    ps_gp_source_mouse_px_threshold,
	    ps_gp_source_cooldown_ms,
	    m_sourceChangedCallbacks.size());
}

void CInput::RunInputSourceSelfTest()
{
	// Save current state
	const EActiveInputSource savedSource = m_activeInputSource;
	const u32 savedLastSwitch = m_lastSourceSwitchTimeMs;
	const u32 savedDeflectStart = m_stickDeflectStartTimeMs;
	const bool savedDeflecting = m_bStickDeflecting;
	const bool savedRequireNeutral = m_bStickRequireNeutralBeforeDeflect;
	const bool savedPrevTrig0 = m_bPrevTriggerOverThresh[0];
	const bool savedPrevTrig1 = m_bPrevTriggerOverThresh[1];
	const bool savedHasPending = m_bHasPendingSourceRequest;
	const EActiveInputSource savedPendingSrc = m_pendingSourceRequest;
	const LPCSTR savedPendingReason = m_pendingSourceReason;
	const bool savedPrevConnected = m_bPrevGamepadConnectedForSource;
	const u32 savedPrevBtns = m_prevGamepadButtonsMask;
	const u32 savedTotal = m_totalSourceSwitches;
	const xr_vector<u32> savedHistory = m_sourceSwitchTimestampsMs;
	const u32 savedLastCursorMove = m_lastGamepadCursorMoveMs;
	const sxr_gamepad_state savedGpState = m_gamepadState;
	const u32 savedCurTime = dwCurTime;

	// Save and set default parameters for deterministic test
	const gpmath::CalibrationProfile savedCalibProfile = gpmath::GetCalibrationProfile();
	const float savedHyst = ps_gp_source_stick_hysteresis;
	const int savedHoldMs = ps_gp_source_stick_hold_ms;
	const int savedMousePx = ps_gp_source_mouse_px_threshold;
	const int savedCooldownMs = ps_gp_source_cooldown_ms;
	gpmath::ResetCalibrationProfileToDefaults();
	ps_gp_source_stick_hysteresis = 0.15f;
	ps_gp_source_stick_hold_ms = 100;
	ps_gp_source_mouse_px_threshold = 4;
	ps_gp_source_cooldown_ms = 500;

	int passed = 0;
	constexpr int totalChecks = 10;

	// D1.2b-N2: suppress real callbacks, Msg, and gp_source_switched telemetry during selftest
	m_bSourceSelfTestRunning = true;

	// Reset for deterministic test
	m_activeInputSource = EActiveInputSource::KBM;
	m_lastSourceSwitchTimeMs = 1000;
	m_stickDeflectStartTimeMs = 0;
	m_bStickDeflecting = false;
	m_bStickRequireNeutralBeforeDeflect = false;
	m_bPrevTriggerOverThresh[0] = false;
	m_bPrevTriggerOverThresh[1] = false;
	m_bHasPendingSourceRequest = false;
	m_pendingSourceReason = nullptr;
	m_bPrevGamepadConnectedForSource = true;
	m_prevGamepadButtonsMask = 0;
	m_lastGamepadCursorMoveMs = 0;
	m_sourceSwitchTimestampsMs.clear();
	ZeroMemory(&m_gamepadState, sizeof(m_gamepadState));
	m_gamepadState.connected = true;
	m_gamepadState.has_focus = true;

	// Check 1: Sub-threshold stick deflection (0.25 < 0.30) for 200 ms does NOT switch KBM -> Gamepad
	dwCurTime = 2000;
	m_gamepadState.axes[0] = static_cast<s16>(0.25f * 32767.0f);
	UpdateInputSourceArbitration();
	dwCurTime = 2200;
	UpdateInputSourceArbitration();
	if (m_activeInputSource == EActiveInputSource::KBM)
		++passed;

	// Check 2: Stick deflection >= 0.30 (0.50) for 50 ms (< 100 ms) does NOT switch yet
	dwCurTime = 2300;
	m_gamepadState.axes[0] = static_cast<s16>(0.50f * 32767.0f);
	UpdateInputSourceArbitration();
	dwCurTime = 2350;
	UpdateInputSourceArbitration();
	if (m_activeInputSource == EActiveInputSource::KBM)
		++passed;

	// Check 3: Stick deflection held to 110 ms (>= 100 ms) switches KBM -> Gamepad
	dwCurTime = 2410;
	UpdateInputSourceArbitration();
	if (m_activeInputSource == EActiveInputSource::Gamepad)
		++passed;

	// Check 4 (D1.2b-N4): Cooldown (< 500 ms): key press at +200 ms does NOT switch immediately,
	// but is queued as a pending request
	m_gamepadState.axes[0] = 0;
	dwCurTime = 2600;
	m_bFrameKbKeyPressed = true;
	UpdateInputSourceArbitration();
	m_bFrameKbKeyPressed = false;
	if (m_activeInputSource == EActiveInputSource::Gamepad && m_bHasPendingSourceRequest && m_pendingSourceRequest == EActiveInputSource::KBM)
		++passed;

	// Check 5 (D1.2b-N4): Once 500 ms cooldown expires (at 2920 ms), the pending key press switches Gamepad -> KBM
	// without needing another key press
	dwCurTime = 2920;
	UpdateInputSourceArbitration();
	if (m_activeInputSource == EActiveInputSource::KBM && !m_bHasPendingSourceRequest)
		++passed;

	// Check 6 (D1.2b-N3): Trigger rising edge switches KBM -> Gamepad, and holding trigger + pressing a key
	// switches to KBM and does NOT bounce back to Gamepad while the trigger remains held
	dwCurTime = 3500;
	m_gamepadState.axes[5] = static_cast<s16>(0.80f * 32767.0f); // RT pressed
	UpdateInputSourceArbitration(); // KBM -> Gamepad at 3500
	const bool switchedOnTriggerEdge = (m_activeInputSource == EActiveInputSource::Gamepad);
	dwCurTime = 4100; // cooldown expired, RT still held at 0.80, press keyboard key
	m_bFrameKbKeyPressed = true;
	UpdateInputSourceArbitration(); // Gamepad -> KBM at 4100
	m_bFrameKbKeyPressed = false;
	const bool switchedToKbmWhileTriggerHeld = (m_activeInputSource == EActiveInputSource::KBM);
	dwCurTime = 4700; // +600 ms later, RT STILL held at 0.80 -> must NOT bounce back to Gamepad
	UpdateInputSourceArbitration();
	if (switchedOnTriggerEdge && switchedToKbmWhileTriggerHeld && m_activeInputSource == EActiveInputSource::KBM)
		++passed;
	m_gamepadState.axes[5] = 0;
	dwCurTime = 4750;
	UpdateInputSourceArbitration();

	// Check 7 (D1.2b-N3): Holding stick deflected while pressing a key switches to KBM and does NOT bounce back
	// to Gamepad until the stick is returned below threshold and deflected again
	dwCurTime = 5300;
	m_gamepadState.axes[0] = static_cast<s16>(0.60f * 32767.0f);
	UpdateInputSourceArbitration();
	dwCurTime = 5420;
	UpdateInputSourceArbitration(); // KBM -> Gamepad at 5420
	const bool stickSwitchedToGp = (m_activeInputSource == EActiveInputSource::Gamepad);
	dwCurTime = 6000; // stick STILL held at 0.60, press keyboard key -> switches to KBM
	m_bFrameKbKeyPressed = true;
	UpdateInputSourceArbitration();
	m_bFrameKbKeyPressed = false;
	const bool stickKeySwitchedToKbm = (m_activeInputSource == EActiveInputSource::KBM);
	dwCurTime = 6600; // +600 ms, stick STILL held at 0.60 -> must NOT bounce back
	UpdateInputSourceArbitration();
	dwCurTime = 6750; // +750 ms, stick STILL held -> still KBM
	UpdateInputSourceArbitration();
	if (stickSwitchedToGp && stickKeySwitchedToKbm && m_activeInputSource == EActiveInputSource::KBM)
		++passed;

	// Check 8: Returning stick to neutral and deflecting again switches KBM -> Gamepad,
	// and anti-chatter in menu blocks mouse move while stick is held
	dwCurTime = 6800;
	m_gamepadState.axes[0] = 0;
	UpdateInputSourceArbitration(); // clears m_bStickRequireNeutralBeforeDeflect
	dwCurTime = 6850;
	m_gamepadState.axes[0] = static_cast<s16>(0.50f * 32767.0f);
	UpdateInputSourceArbitration();
	dwCurTime = 6960;
	UpdateInputSourceArbitration(); // KBM -> Gamepad at 6960
	dwCurTime = 7500; // cooldown expired, stick held at 0.40, mouse moves 15 counts -> stays Gamepad
	m_gamepadState.axes[0] = static_cast<s16>(0.40f * 32767.0f);
	m_frameMouseDeltaX = 15;
	UpdateInputSourceArbitration();
	m_frameMouseDeltaX = 0;
	if (m_activeInputSource == EActiveInputSource::Gamepad)
		++passed;

	// Check 9: Mouse move > 4 counts when stick is released (at rest) switches Gamepad -> KBM
	dwCurTime = 7600;
	m_gamepadState.axes[0] = 0;
	m_frameMouseDeltaX = 15;
	UpdateInputSourceArbitration();
	m_frameMouseDeltaX = 0;
	if (m_activeInputSource == EActiveInputSource::KBM)
		++passed;

	// Check 10 (D1.2b-N7): Unplugging active gamepad while source is Gamepad immediately switches to KBM without cooldown
	dwCurTime = 8200;
	m_gamepadState.buttons_mask = (1u << GP_BTN_SOUTH);
	UpdateInputSourceArbitration(); // KBM -> Gamepad at 8200
	m_gamepadState.buttons_mask = 0;
	const bool btnSwitchedToGp = (m_activeInputSource == EActiveInputSource::Gamepad);
	dwCurTime = 8250; // only +50 ms later (< 500 ms cooldown), gamepad disconnects
	m_gamepadState.connected = false;
	UpdateInputSourceArbitration();
	if (btnSwitchedToGp && m_activeInputSource == EActiveInputSource::KBM)
		++passed;

	m_bSourceSelfTestRunning = false;

	// Restore state before logging so any listener/status sees exact pre-test state
	gpmath::SetCalibrationProfile(savedCalibProfile);
	ps_gp_source_stick_hysteresis = savedHyst;
	ps_gp_source_stick_hold_ms = savedHoldMs;
	ps_gp_source_mouse_px_threshold = savedMousePx;
	ps_gp_source_cooldown_ms = savedCooldownMs;

	m_activeInputSource = savedSource;
	m_lastSourceSwitchTimeMs = savedLastSwitch;
	m_stickDeflectStartTimeMs = savedDeflectStart;
	m_bStickDeflecting = savedDeflecting;
	m_bStickRequireNeutralBeforeDeflect = savedRequireNeutral;
	m_bPrevTriggerOverThresh[0] = savedPrevTrig0;
	m_bPrevTriggerOverThresh[1] = savedPrevTrig1;
	m_bHasPendingSourceRequest = savedHasPending;
	m_pendingSourceRequest = savedPendingSrc;
	m_pendingSourceReason = savedPendingReason;
	m_bPrevGamepadConnectedForSource = savedPrevConnected;
	m_prevGamepadButtonsMask = savedPrevBtns;
	m_totalSourceSwitches = savedTotal;
	m_sourceSwitchTimestampsMs = savedHistory;
	m_lastGamepadCursorMoveMs = savedLastCursorMove;
	m_gamepadState = savedGpState;
	dwCurTime = savedCurTime;

	Msg("* [Gamepad] gp_source selftest: %d/%d checks PASSED", passed, totalChecks);
	if (ps_gp_telemetry)
	{
		string128 line;
		xr_sprintf(line, sizeof(line), "gp_source_selftest,%u,passed=%d,total=%d", savedCurTime, passed, totalChecks);
		WriteGamepadTelemetryLine(line);
	}
}

IInputReceiver* CInput::CurrentIR()
{
	if (cbStack.size())
		return cbStack.back();
	else
		return NULL;
}

void CInput::unacquire()
{
	pKeyboard->Unacquire();
	pMouse->Unacquire();
}

void CInput::acquire(const bool& exclusive)
{
	pKeyboard->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND
	);
	pKeyboard->Acquire();

	pMouse->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY
	);
	pMouse->Acquire();
}

void CInput::exclusive_mode(const bool& exclusive)
{
	g_exclusive = exclusive;
	unacquire();
	acquire(exclusive);
}

bool CInput::get_exclusive_mode()
{
	return g_exclusive;
}

void CInput::feedback(u16 s1, u16 s2, float time)
{
	stop_vibration_time = RDEVICE.fTimeGlobal + time;
#ifndef _EDITOR
	//. set_vibration (s1, s2);
#endif
}
