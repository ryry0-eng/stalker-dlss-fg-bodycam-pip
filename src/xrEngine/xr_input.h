#ifndef __XR_INPUT__
#define __XR_INPUT__

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

class ENGINE_API IInputReceiver;
struct SDL_Gamepad;
class CGamepadDeviceManager;
class CGamepadHaptics;

//\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\
//описание класса
const int mouse_device_key = 1;
const int keyboard_device_key = 2;
const int all_device_key = mouse_device_key | keyboard_device_key;
const int default_key = mouse_device_key | keyboard_device_key;

class ENGINE_API CInput
#ifndef M_BORLAND
	:
	public pureFrame,
	public pureAppActivate,
	public pureAppDeactivate
#endif
{
public:
	enum
	{
		COUNT_MOUSE_BUTTONS = 8,
		COUNT_MOUSE_AXIS = 3,
		COUNT_KB_BUTTONS = 256,
		COUNT_GAMEPAD_AXIS = 6
	};

	struct sxr_mouse
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
		u32 mouse_dt;
	};

	struct sxr_key
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
	};

	// C1.1: прототип, настройки — Этап D
	enum EGamepadButtonBit : u32
	{
		GP_BTN_SOUTH = 0, // SDL_GAMEPAD_BUTTON_SOUTH (Xbox A)
		GP_BTN_EAST  = 1, // SDL_GAMEPAD_BUTTON_EAST  (Xbox B)
		GP_BTN_WEST  = 2, // SDL_GAMEPAD_BUTTON_WEST  (Xbox X)
		GP_BTN_NORTH = 3, // SDL_GAMEPAD_BUTTON_NORTH (Xbox Y)
		GP_BTN_BACK  = 4,
		GP_BTN_GUIDE = 5,
		GP_BTN_START = 6,
		GP_BTN_LEFT_STICK = 7,
		GP_BTN_RIGHT_STICK = 8,
		GP_BTN_LEFT_SHOULDER = 9,
		GP_BTN_RIGHT_SHOULDER = 10,
		GP_BTN_DPAD_UP = 11,
		GP_BTN_DPAD_DOWN = 12,
		GP_BTN_DPAD_LEFT = 13,
		GP_BTN_DPAD_RIGHT = 14
	};

	// D1.2: активный источник ввода (Клавиатура/Мышь ↔ Геймпад) с гистерезисом
	enum class EActiveInputSource : u8
	{
		KBM = 0,
		Gamepad = 1
	};

	// GPSET-2: режим переключения/блокировки источника ввода (appdata\gamepad\profile.ltx [source])
	enum class EInputSourceLockMode : u8
	{
		Auto = 0,        // Автоматическое переключение КиМ ↔ Геймпад
		GamepadOnly = 1, // «Только геймпад»: не переключаться на КиМ при случайных касаниях мыши/клавиатуры
		KbmOnly = 2      // «Отключить геймпад»: принудительно КиМ, опрос кнопок/стиков геймпада подавлен
	};

	typedef void (*FInputSourceChangedCallback)(EActiveInputSource newSource, EActiveInputSource prevSource, LPCSTR reason, u32 dwCurTime);

	struct sxr_gamepad_state
	{
		s16 axes[COUNT_GAMEPAD_AXIS];
		u32 buttons_mask;
		u32 instance_id;
		u32 timestamp_ms;
		bool connected;
		bool has_focus;
		bool need_release_all;
	};

private:
	BENCH_SEC_SCRAMBLEMEMBER1
	LPDIRECTINPUT8 pDI; // The DInput object
	LPDIRECTINPUTDEVICE8 pMouse; // The DIDevice7 interface
	LPDIRECTINPUTDEVICE8 pKeyboard; // The DIDevice7 interface
	//----------------------
	u32 timeStamp[COUNT_MOUSE_AXIS];
	u32 timeSave[COUNT_MOUSE_AXIS];
	int offs[COUNT_MOUSE_AXIS];
	BOOL mouseState[COUNT_MOUSE_BUTTONS];

	//----------------------
	BOOL KBState[COUNT_KB_BUTTONS];
	bool m_gamepadInjectedKBState[COUNT_KB_BUTTONS];

	HRESULT CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice,
	                          const DIDATAFORMAT* pdidDataFormat, u32 dwFlags,
	                          u32 buf_size);

	// xr_stack<IInputReceiver*> cbStack;
	xr_vector<IInputReceiver*> cbStack;
	xr_vector<DIDEVICEOBJECTDATA> m_keyboard_events;
	xr_vector<DIDEVICEOBJECTDATA> m_mouse_events;

	void MouseUpdate();
	void KeyUpdate();

	// C1.1 / D1.1 / D5.2: прототип, настройки — Этап D
	CGamepadDeviceManager* m_pGamepadDeviceMgr;
	CGamepadHaptics* m_pGamepadHaptics;
	SDL_Gamepad* m_pGamepad;
	u32 m_gamepadInstanceId;
	bool m_bSdlGamepadInitialized;
	bool m_bGamepadNeedReleaseAll;
	bool m_bGamepadLatchPending;
	u32 m_gamepadReleaseMask;
	u32 m_gamepadFocusGen = 0;
	sxr_gamepad_state m_gamepadState;
	sxr_gamepad_state m_gamepadLastLoggedState;
	bool m_bGamepadHasLoggedState;
	bool m_bGamepadTelemetryHeaderWritten;
	// FR.1 №4: файл телеметрии держится открытым, пока включён ps_gp_telemetry; fflush не чаще раза в секунду
	FILE* m_pGamepadTelemetryFile;
	u32 m_gamepadTelemetryLastFlushMs;
	void CloseGamepadTelemetryFile();
	void FlushGamepadTelemetryIfDue();

	void ResetGamepadSnapshot(bool hasFocus);
	void WriteGamepadStateTelemetryIfChanged();
	void GamepadUpdateInner();
	void UpdateInputSourceArbitration();
	void UpdateInputSourceArbitrationInner();
	void CommitActiveInputSource(EActiveInputSource newSource, LPCSTR reason, bool ignoreCooldown = false);
	void PruneSourceSwitchHistory(u32 nowMs) const;
	static void GamepadUpdateStepThunk(void* ctx);
	static void SourceArbitrationStepThunk(void* ctx);
	static bool StaticGamepadErrorRecoveryCallback(const char* stageTag,
	                                               const char* errorDetail,
	                                               u32 sehCode,
	                                               u64 sehAddr,
	                                               const char* contextOverride,
	                                               const char* recoveryOverride,
	                                               bool isLuaError,
	                                               void* userData);

	// D1.2 / D1.2b: состояние арбитража источника ввода (Клавиатура/Мышь ↔ Геймпад)
	EActiveInputSource m_activeInputSource = EActiveInputSource::KBM;
	EInputSourceLockMode m_sourceLockMode = EInputSourceLockMode::Auto;
	mutable string128 m_cachedActiveGamepadName = {};
	void LoadSourceLockModeFromProfile();
	bool SaveSourceLockModeToProfile() const;
	u32 m_lastSourceSwitchTimeMs = 0;
	u32 m_stickDeflectStartTimeMs = 0;
	bool m_bStickDeflecting = false;
	bool m_bStickRequireNeutralBeforeDeflect = false;
	bool m_bPrevTriggerOverThresh[2] = { false, false };
	bool m_bHasPendingSourceRequest = false;
	EActiveInputSource m_pendingSourceRequest = EActiveInputSource::KBM;
	LPCSTR m_pendingSourceReason = nullptr;
	bool m_bPrevGamepadConnectedForSource = false;
	bool m_bSourceSelfTestRunning = false;
	bool m_bFrameKbKeyPressed = false;
	bool m_bFrameMouseBtnPressed = false;
	int m_frameMouseWheelDelta = 0;
	int m_frameMouseDeltaX = 0;
	int m_frameMouseDeltaY = 0;
	u32 m_prevGamepadButtonsMask = 0;
	u32 m_totalSourceSwitches = 0;
	mutable xr_vector<u32> m_sourceSwitchTimestampsMs;
	xr_vector<FInputSourceChangedCallback> m_sourceChangedCallbacks;
	u32 m_lastGamepadCursorMoveMs = 0;
	u32 m_lastErrorLogWriteMs = 0;
	u32 m_errorRepeatCount = 0;
	string128 m_lastErrorStageTag = {};

public:
	void GamepadUpdate();
	bool HandleGamepadFrameError(LPCSTR stageTag,
	                             LPCSTR errorDetail,
	                             u32 sehCode = 0,
	                             u64 sehAddr = 0,
	                             LPCSTR contextOverride = nullptr,
	                             LPCSTR recoveryOverride = nullptr,
	                             bool isLuaError = false);
	void NotifyGamepadCursorMoved(u32 nowMs = 0);
	bool IsGamepadCursorRecentlyMoved(u32 nowMs = 0, u32 windowMs = 60) const;
	CGamepadDeviceManager* GetGamepadDeviceManager() const { return m_pGamepadDeviceMgr; }
	CGamepadHaptics* GetGamepadHaptics() const { return m_pGamepadHaptics; }
	void LogGamepadDevices() const;
	void RunVirtualGamepadHotplugTest(int cycles);

	// D1.2: API активного источника ввода и подписка на событие смены источника (для D5.1 и gp_bridge)
	EActiveInputSource GetActiveInputSource() const { return m_activeInputSource; }
	bool IsGamepadActiveSource() const { return m_activeInputSource == EActiveInputSource::Gamepad; }
	static LPCSTR GetInputSourceName(EActiveInputSource src) { return (src == EActiveInputSource::Gamepad) ? "gamepad" : "kbm"; }
	EInputSourceLockMode GetInputSourceLockMode() const { return m_sourceLockMode; }
	bool IsGamepadOnlyMode() const { return m_sourceLockMode == EInputSourceLockMode::GamepadOnly; }
	static LPCSTR GetSourceLockModeName(EInputSourceLockMode mode);
	static bool ParseSourceLockMode(LPCSTR str, EInputSourceLockMode& outMode);
	bool SetInputSourceLockMode(EInputSourceLockMode mode, bool saveToDisk = true, LPCSTR reason = "source_mode_set");
	bool SetInputSourceLockModeByName(LPCSTR modeName, bool saveToDisk = true, LPCSTR reason = "source_mode_set");
	LPCSTR GetActiveGamepadName() const;
	LPCSTR GetActiveGamepadPowerInfo(int& outPercent) const;
	u32 GetSourceSwitchesLastMinute() const;
	u32 GetTotalSourceSwitches() const { return m_totalSourceSwitches; }
	void ForceActiveInputSource(EActiveInputSource src, LPCSTR reason = "console_force");
	void AddInputSourceChangedCallback(FInputSourceChangedCallback cb);
	void RemoveInputSourceChangedCallback(FInputSourceChangedCallback cb);
	void LogInputSourceState() const;
	void RunInputSourceSelfTest();
	void resetMouseState();
	sxr_mouse mouse_property;
	sxr_key key_property;
	u32 dwCurTime;

	const sxr_gamepad_state& GetGamepadState() const { return m_gamepadState; }
	void LogGamepadState() const;
	void WriteGamepadTelemetryLine(LPCSTR line);

	void SetAllAcquire(BOOL bAcquire = TRUE);
	void SetMouseAcquire(BOOL bAcquire);
	void SetKBDAcquire(BOOL bAcquire);

	void iCapture(IInputReceiver* pc);
	void iRelease(IInputReceiver* pc);
	BOOL iGetAsyncKeyState(int dik);
	BOOL iGetAsyncBtnState(int btn);
	void SetGamepadInjectedKeyState(int dik, bool isDown);
	void ClearAllGamepadInjectedKeys();
	void iGetLastMouseDelta(Ivector2& p) { p.set(offs[0], offs[1]); }

	CInput(BOOL bExclusive = true, int deviceForInit = default_key);
	~CInput();

	virtual void _BCL OnFrame(void);
	virtual void OnAppActivate(void);
	virtual void OnAppDeactivate(void);
	void DeactivateSoft();

	IInputReceiver* CurrentIR();

public:
	void exclusive_mode(const bool& exclusive);
	bool get_exclusive_mode();
	void unacquire();
	void acquire(const bool& exclusive);
	bool get_dik_name(int dik, LPSTR dest, int dest_sz);
	bool dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz);

	void feedback(u16 s1, u16 s2, float time);
};

extern ENGINE_API CInput* pInput;
extern ENGINE_API int ps_gp_telemetry;

// D1.2 / D2.2 (D1.2-N5): настройки гистерезиса арбитража Клавиатура/Мышь ↔ Геймпад
extern ENGINE_API float ps_gp_source_stick_hysteresis;
extern ENGINE_API int ps_gp_source_stick_hold_ms;
extern ENGINE_API int ps_gp_source_mouse_px_threshold;
extern ENGINE_API int ps_gp_source_cooldown_ms;

// C1.1b: прототип, настройки — Этап D
constexpr u32 GAMEPAD_WNDPROC_RING_SIZE = 32;
extern ENGINE_API volatile bool g_gp_in_sdl_update;
extern ENGINE_API volatile u32 g_gp_wndproc_in_sdl_count;
extern ENGINE_API UINT g_gp_wndproc_in_sdl_msgs[GAMEPAD_WNDPROC_RING_SIZE];

#endif //__XR_INPUT__
