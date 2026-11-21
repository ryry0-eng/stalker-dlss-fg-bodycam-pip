#ifndef __GAMEPAD_DEVICE_MANAGER_H__
#define __GAMEPAD_DEVICE_MANAGER_H__

#pragma once

struct SDL_Gamepad;
struct SDL_Joystick;

enum class EGamepadClass : u8
{
	None = 0,
	ClassA = 1, // SDL_Gamepad with known console type (Xbox, PS, Nintendo)
	ClassB = 2, // SDL_Gamepad with STANDARD or UNKNOWN type (mapped via db)
	ClassC = 3  // Raw SDL_Joystick without gamepad mapping (requires D4.2 wizard)
};

struct SGamepadDeviceEntry
{
	u32 instance_id = 0;
	SDL_Gamepad* gamepad = nullptr;
	SDL_Joystick* joystick = nullptr;
	EGamepadClass device_class = EGamepadClass::None;
	int sdl_gamepad_type = 0;      // SDL_GamepadType
	int sdl_real_gamepad_type = 0; // SDL_GamepadType
	u16 vendor_id = 0;
	u16 product_id = 0;
	bool cap_rumble = false;
	bool cap_trigger_rumble = false;
	bool cap_led = false;
	bool cap_gyro = false;
	bool cap_accel = false;
	bool is_virtual = false;
	bool is_ds4w_suppressed = false;
	bool has_baseline = false;
	bool axis_active[6] = {};
	u32 last_input_time_ms = 0;
	s16 raw_axes[6] = {};
	s16 prev_axes[6] = {};
	u32 raw_buttons = 0;
	u32 prev_buttons = 0;
	string128 name = {};
	string64 guid_str = {};
};

struct SRawJoystickSnapshot
{
	static constexpr int MAX_RAW_AXES = 16;
	static constexpr int MAX_RAW_HATS = 4;

	u32 instance_id = 0;
	bool valid = false;
	EGamepadClass device_class = EGamepadClass::None;
	int num_buttons = 0;
	int num_axes = 0;
	int num_hats = 0;
	u32 buttons_mask = 0;
	s16 axes[MAX_RAW_AXES] = {};
	u8 hats[MAX_RAW_HATS] = {}; // SDL hat bitmask: 0x01=UP, 0x02=RIGHT, 0x04=DOWN, 0x08=LEFT
	string128 name = {};
	string64 guid_str = {};
};

class ENGINE_API CGamepadDeviceManager
{
public:
	CGamepadDeviceManager();
	~CGamepadDeviceManager();

	void Initialize();
	void Shutdown();

	bool IsInitialized() const { return m_bInitialized; }

	// Called once per frame inside CInput::GamepadUpdate()
	// Returns true if the active device was disconnected or switched this frame
	void PollHotplugAndSelectActive(u32 dwCurTime, bool bFocused, bool& outActiveRemoved, bool& outActiveChanged);

	const SGamepadDeviceEntry* GetActiveDevice() const;
	u32 GetActiveInstanceId() const { return m_activeInstanceId; }
	const xr_vector<SGamepadDeviceEntry>& GetDevices() const { return m_devices; }

	// D4.2: Class C Mapping Wizard support
	const SGamepadDeviceEntry* FindFirstClassCDevice() const;
	const SGamepadDeviceEntry* GetDeviceByInstanceId(u32 instanceId) const { return FindDevice(instanceId); }
	bool SampleRawJoystick(u32 instanceId, SRawJoystickSnapshot& outSnap) const;
	bool ApplyAndSaveCustomMapping(u32 instanceId, LPCSTR mappingString, bool saveToUserFile, LPSTR outSavedPath = nullptr, u32 outSavedPathCap = 0);
	u32 AttachVirtualClassCDevice(LPCSTR customName = "GAMMA Unmapped Class C Test Pad");
	bool SetVirtualClassCInput(u32 instanceId, int buttonIdx, bool pressed, int axisIdx, s16 axisVal, int hatIdx, u8 hatVal);
	void DetachVirtualClassCDevice(u32 instanceId);

	// Stub until D5.2 Haptics mixer: immediately stops rumble on the device (or all devices if id == 0)
	void StopDeviceRumble(u32 instanceId = 0);

	// Console commands
	void LogDevices() const;
	void RunVirtualHotplugTest(int cycles);

	// Frame poll timing stats (microseconds)
	double GetLastPollUs() const { return m_lastPollUs; }

private:
	SGamepadDeviceEntry* FindDevice(u32 instanceId);
	const SGamepadDeviceEntry* FindDevice(u32 instanceId) const;

	void OpenAndRegisterDevice(u32 instanceId, u32 dwCurTime);
	void CloseAndRemoveDevice(u32 instanceId, bool& outWasActive);
	void RefreshDs4WindowsDuplicateSuppression();
	void ReadDeviceRawInputs(SGamepadDeviceEntry& dev);
	bool HasSignificantInput(SGamepadDeviceEntry& dev);
	int LoadMappingsFromSearchPaths();

	void RecordPollSampleUs(double us, u32 dwCurTime);

private:
	xr_vector<SGamepadDeviceEntry> m_devices;
	u32 m_activeInstanceId = 0;
	bool m_bInitialized = false;
	int m_loadedMappingsCount = 0;

	// Poll duration telemetry window (60s rolling / up to 4096 samples)
	xr_vector<double> m_pollSamplesUs;
	u32 m_lastPerfReportTimeMs = 0;
	double m_lastPollUs = 0.0;
};

#endif // __GAMEPAD_DEVICE_MANAGER_H__
