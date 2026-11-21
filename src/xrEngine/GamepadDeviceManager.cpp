#include "stdafx.h"
#pragma hdrstop

#include "GamepadDeviceManager.h"
#include "GamepadBindings.h"
#include "xr_input.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <direct.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// D1.1 / D1.1b: thresholds for "last significant input" active device selection (D36).
// Significance is evaluated by state change (button press edge, axis crossing threshold or significant delta),
// not static absolute position, so resting throttles/pedals or held sticks don't steal activity every frame.
constexpr int SIGNIFICANT_STICK_THRESHOLD = 8192;
constexpr int SIGNIFICANT_STICK_RELEASE_THRESHOLD = 6144;
constexpr int SIGNIFICANT_TRIGGER_THRESHOLD = 8192;
constexpr int SIGNIFICANT_TRIGGER_RELEASE_THRESHOLD = 6144;
constexpr int SIGNIFICANT_AXIS_DELTA = 4096;
constexpr u32 PERF_REPORT_INTERVAL_MS = 60000;
constexpr size_t MAX_POLL_SAMPLES = 4096;

const char* GetGamepadClassName(EGamepadClass cls)
{
	switch (cls)
	{
	case EGamepadClass::ClassA: return "A";
	case EGamepadClass::ClassB: return "B";
	case EGamepadClass::ClassC: return "C";
	default: return "None";
	}
}

const char* GetSdlGamepadTypeName(int type)
{
	const char* s = SDL_GetGamepadStringForType(static_cast<SDL_GamepadType>(type));
	return (s && s[0]) ? s : "unknown";
}

EGamepadClass ClassifyGamepadType(SDL_GamepadType type)
{
	switch (type)
	{
	case SDL_GAMEPAD_TYPE_XBOX360:
	case SDL_GAMEPAD_TYPE_XBOXONE:
	case SDL_GAMEPAD_TYPE_PS3:
	case SDL_GAMEPAD_TYPE_PS4:
	case SDL_GAMEPAD_TYPE_PS5:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
	case SDL_GAMEPAD_TYPE_GAMECUBE:
	case SDL_GAMEPAD_TYPE_STEAM:
		return EGamepadClass::ClassA;
	case SDL_GAMEPAD_TYPE_STANDARD:
	case SDL_GAMEPAD_TYPE_UNKNOWN:
	default:
		return EGamepadClass::ClassB;
	}
}

bool IsSonyDevice(const SGamepadDeviceEntry& dev)
{
	if (dev.vendor_id == 0x054C)
		return true;
	if (dev.sdl_gamepad_type == SDL_GAMEPAD_TYPE_PS3 ||
	    dev.sdl_gamepad_type == SDL_GAMEPAD_TYPE_PS4 ||
	    dev.sdl_gamepad_type == SDL_GAMEPAD_TYPE_PS5 ||
	    dev.sdl_real_gamepad_type == SDL_GAMEPAD_TYPE_PS3 ||
	    dev.sdl_real_gamepad_type == SDL_GAMEPAD_TYPE_PS4 ||
	    dev.sdl_real_gamepad_type == SDL_GAMEPAD_TYPE_PS5)
	{
		return true;
	}
	return false;
}

bool IsViGEmOrVirtualXbox360(const SGamepadDeviceEntry& dev)
{
	if (dev.is_virtual)
		return false; // internal SDL virtual joystick for tests
	if (dev.sdl_gamepad_type != SDL_GAMEPAD_TYPE_XBOX360 &&
	    dev.sdl_real_gamepad_type != SDL_GAMEPAD_TYPE_XBOX360)
	{
		return false;
	}
	// ViGEmBus / DS4Windows virtual XInput Xbox 360 controller typically reports VID 0x045E PID 0x028E.
	// D1.1-N3 limitation (confirmed with user in D1.1b): a physical wired Xbox 360 controller shares
	// VID 0x045E PID 0x028E and would also be suppressed if connected simultaneously with a Sony gamepad;
	// DS4Windows in DualShock 4 output mode (second Sony device) is not suppressed by this check.
	if (dev.vendor_id == 0x045E && dev.product_id == 0x028E)
		return true;
	return false;
}

bool IsNonGamepadJoystickType(SDL_JoystickType jtype)
{
	switch (jtype)
	{
	case SDL_JOYSTICK_TYPE_WHEEL:
	case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
	case SDL_JOYSTICK_TYPE_DANCE_PAD:
	case SDL_JOYSTICK_TYPE_GUITAR:
	case SDL_JOYSTICK_TYPE_DRUM_KIT:
	case SDL_JOYSTICK_TYPE_THROTTLE:
		return true;
	default:
		return false;
	}
}

double QueryPerfTicksToUs(LONGLONG ticks, LONGLONG freq)
{
	if (freq <= 0)
		return 0.0;
	return (static_cast<double>(ticks) * 1000000.0) / static_cast<double>(freq);
}

double PercentileSorted(const xr_vector<double>& sorted, double p)
{
	if (sorted.empty())
		return 0.0;
	const double idx = p * static_cast<double>(sorted.size() - 1);
	const size_t lo = static_cast<size_t>(std::floor(idx));
	const size_t hi = static_cast<size_t>(std::ceil(idx));
	const double frac = idx - static_cast<double>(lo);
	return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}
} // namespace

CGamepadDeviceManager::CGamepadDeviceManager()
{
	m_pollSamplesUs.reserve(MAX_POLL_SAMPLES);
}

CGamepadDeviceManager::~CGamepadDeviceManager()
{
	Shutdown();
}

void CGamepadDeviceManager::Initialize()
{
	if (m_bInitialized)
		return;

	// D40: Enable HIDAPI Xbox / Xbox One drivers before SDL_Init so trigger rumble is available
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "1");

	if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
	{
		Msg("! [Gamepad] SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK) failed: %s", SDL_GetError());
		return;
	}

	m_bInitialized = true;
	Msg("* [Gamepad] SDL3 gamepad subsystem initialized (v%d.%d.%d, HIDAPI_XBOX=1, HIDAPI_XBOX_ONE=1)",
	    SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);

	m_loadedMappingsCount = LoadMappingsFromSearchPaths();

	// Initial enumeration at startup (hotplug during frame loop uses only ADDED/REMOVED events)
	int joyCount = 0;
	SDL_JoystickID* joys = SDL_GetJoysticks(&joyCount);
	if (joys && joyCount > 0)
	{
		for (int i = 0; i < joyCount; ++i)
		{
			OpenAndRegisterDevice(joys[i], 0);
		}
	}
	if (joys)
		SDL_free(joys);

	RefreshDs4WindowsDuplicateSuppression();
}

void CGamepadDeviceManager::Shutdown()
{
	for (auto& dev : m_devices)
	{
		if (dev.gamepad)
		{
			SDL_RumbleGamepad(dev.gamepad, 0, 0, 0);
			SDL_RumbleGamepadTriggers(dev.gamepad, 0, 0, 0);
			SDL_CloseGamepad(dev.gamepad);
			dev.gamepad = nullptr;
			dev.joystick = nullptr;
		}
		else if (dev.joystick)
		{
			SDL_RumbleJoystick(dev.joystick, 0, 0, 0);
			SDL_CloseJoystick(dev.joystick);
			dev.joystick = nullptr;
		}
	}
	m_devices.clear();
	m_activeInstanceId = 0;

	if (m_bInitialized)
	{
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
		m_bInitialized = false;
	}
}

int CGamepadDeviceManager::LoadMappingsFromSearchPaths()
{
	struct SPathCandidate
	{
		LPCSTR root;
		LPCSTR rel;
	};

	const SPathCandidate candidates[] = {
		{ "$app_data_root$", "gamepad\\gamecontrollerdb.txt" },
		{ "$app_data_root$", "gamepad\\user_mappings.txt" },
		{ "$game_config$",   "gamepad\\gamecontrollerdb.txt" },
		{ "$game_data$",     "gamecontrollerdb.txt" }
	};

	int totalAdded = 0;
	bool anyFound = false;

	for (const auto& c : candidates)
	{
		if (!FS.path_exist(c.root))
			continue;

		string_path fullPath;
		FS.update_path(fullPath, c.root, c.rel);

		IReader* reader = FS.r_open(fullPath);
		if (!reader)
			continue;
		FS.r_close(reader);

		anyFound = true;
		const int added = SDL_AddGamepadMappingsFromFile(fullPath);
		if (added >= 0)
		{
			totalAdded += added;
			Msg("* [Gamepad] Loaded %d mapping(s) from '%s'", added, fullPath);
		}
		else
		{
			Msg("! [Gamepad] Failed to load mappings from '%s': %s", fullPath, SDL_GetError());
		}
	}

	if (!anyFound)
	{
		Msg("* [Gamepad] Optional gamecontrollerdb.txt not found in mod paths (using built-in SDL3 database)");
	}

	return totalAdded;
}

SGamepadDeviceEntry* CGamepadDeviceManager::FindDevice(u32 instanceId)
{
	for (auto& dev : m_devices)
	{
		if (dev.instance_id == instanceId)
			return &dev;
	}
	return nullptr;
}

const SGamepadDeviceEntry* CGamepadDeviceManager::FindDevice(u32 instanceId) const
{
	for (const auto& dev : m_devices)
	{
		if (dev.instance_id == instanceId)
			return &dev;
	}
	return nullptr;
}

const SGamepadDeviceEntry* CGamepadDeviceManager::GetActiveDevice() const
{
	if (m_activeInstanceId == 0)
		return nullptr;
	return FindDevice(m_activeInstanceId);
}

void CGamepadDeviceManager::OpenAndRegisterDevice(u32 instanceId, u32 dwCurTime)
{
	if (instanceId == 0)
		return;

	SGamepadDeviceEntry* existing = FindDevice(instanceId);
	if (existing)
	{
		// D1.1-N5: Handle REMAPPED for Class A/B or promotion from Class C to SDL_Gamepad
		if (SDL_IsGamepad(instanceId))
		{
			if (!existing->gamepad)
			{
				SDL_Gamepad* gp = SDL_OpenGamepad(instanceId);
				if (gp)
				{
					if (existing->joystick)
					{
						SDL_CloseJoystick(existing->joystick);
					}
					existing->gamepad = gp;
					existing->joystick = SDL_GetGamepadJoystick(gp);
					existing->has_baseline = false;
				}
			}

			if (existing->gamepad)
			{
				SDL_Gamepad* gp = existing->gamepad;
				const SDL_GamepadType gpType = SDL_GetGamepadType(gp);
				const SDL_GamepadType realType = SDL_GetRealGamepadType(gp);
				existing->sdl_gamepad_type = static_cast<int>(gpType);
				existing->sdl_real_gamepad_type = static_cast<int>(realType);
				existing->device_class = ClassifyGamepadType(gpType);
				existing->vendor_id = SDL_GetGamepadVendor(gp);
				existing->product_id = SDL_GetGamepadProduct(gp);

				const char* gpName = SDL_GetGamepadName(gp);
				if (gpName && gpName[0])
					xr_strcpy(existing->name, sizeof(existing->name), gpName);

				const SDL_PropertiesID props = SDL_GetGamepadProperties(gp);
				existing->cap_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
				existing->cap_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
				existing->cap_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false) ||
				                    SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN, false);
				existing->cap_gyro = SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO);
				existing->cap_accel = SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL);
			}
		}
		return;
	}

	SGamepadDeviceEntry entry;
	entry.instance_id = instanceId;
	entry.last_input_time_ms = dwCurTime;
	entry.is_virtual = SDL_IsJoystickVirtual(instanceId);

	const SDL_GUID guid = SDL_GetJoystickGUIDForID(instanceId);
	SDL_GUIDToString(guid, entry.guid_str, sizeof(entry.guid_str));

	// Non-gamepad peripherals (wheels, flight sticks, throttles, music/dance controllers) stay Class C
	// even if matched by a user/third-party mapping, so they never hijack active gamepad input.
	const SDL_JoystickType joyType = SDL_GetJoystickTypeForID(instanceId);
	const bool isGamepad = SDL_IsGamepad(instanceId) && !IsNonGamepadJoystickType(joyType);
	if (isGamepad)
	{
		SDL_Gamepad* gp = SDL_OpenGamepad(instanceId);
		if (!gp)
		{
			Msg("! [Gamepad] SDL_OpenGamepad(%u) failed: %s", instanceId, SDL_GetError());
			return;
		}
		entry.gamepad = gp;
		entry.joystick = SDL_GetGamepadJoystick(gp);

		const SDL_GamepadType gpType = SDL_GetGamepadType(gp);
		const SDL_GamepadType realType = SDL_GetRealGamepadType(gp);
		entry.sdl_gamepad_type = static_cast<int>(gpType);
		entry.sdl_real_gamepad_type = static_cast<int>(realType);
		entry.device_class = ClassifyGamepadType(gpType);
		entry.vendor_id = SDL_GetGamepadVendor(gp);
		entry.product_id = SDL_GetGamepadProduct(gp);

		const char* gpName = SDL_GetGamepadName(gp);
		xr_strcpy(entry.name, sizeof(entry.name), (gpName && gpName[0]) ? gpName : "SDL Gamepad");

		const SDL_PropertiesID props = SDL_GetGamepadProperties(gp);
		entry.cap_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
		entry.cap_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
		entry.cap_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false) ||
		                SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN, false);
		entry.cap_gyro = SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO);
		entry.cap_accel = SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL);
	}
	else
	{
		SDL_Joystick* joy = SDL_OpenJoystick(instanceId);
		if (!joy)
		{
			Msg("! [Gamepad] SDL_OpenJoystick(%u) failed: %s", instanceId, SDL_GetError());
			return;
		}
		entry.gamepad = nullptr;
		entry.joystick = joy;
		entry.sdl_gamepad_type = static_cast<int>(SDL_GAMEPAD_TYPE_UNKNOWN);
		entry.sdl_real_gamepad_type = static_cast<int>(SDL_GAMEPAD_TYPE_UNKNOWN);
		entry.device_class = EGamepadClass::ClassC;
		entry.vendor_id = SDL_GetJoystickVendor(joy);
		entry.product_id = SDL_GetJoystickProduct(joy);

		const char* joyName = SDL_GetJoystickName(joy);
		xr_strcpy(entry.name, sizeof(entry.name), (joyName && joyName[0]) ? joyName : "Raw HID Joystick");

		const SDL_PropertiesID props = SDL_GetJoystickProperties(joy);
		entry.cap_rumble = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
		entry.cap_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
		entry.cap_led = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN, false) ||
		                SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_MONO_LED_BOOLEAN, false);
		entry.cap_gyro = false;
		entry.cap_accel = false;
	}

	m_devices.push_back(entry);

	Msg("* [Gamepad] Device added: id=%u name='%s' class=%s type=%s real_type=%s vid=0x%04X pid=0x%04X rumble=%d trig_rumble=%d gyro=%d virtual=%d",
	    entry.instance_id,
	    entry.name,
	    GetGamepadClassName(entry.device_class),
	    GetSdlGamepadTypeName(entry.sdl_gamepad_type),
	    GetSdlGamepadTypeName(entry.sdl_real_gamepad_type),
	    entry.vendor_id,
	    entry.product_id,
	    entry.cap_rumble ? 1 : 0,
	    entry.cap_trigger_rumble ? 1 : 0,
	    entry.cap_gyro ? 1 : 0,
	    entry.is_virtual ? 1 : 0);

	if (pInput && ps_gp_telemetry)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_device_added,%u,id=%u,class=%s,type=%s,vid=0x%04X,pid=0x%04X,trig_rumble=%d",
		           dwCurTime, entry.instance_id, GetGamepadClassName(entry.device_class),
		           GetSdlGamepadTypeName(entry.sdl_gamepad_type), entry.vendor_id, entry.product_id,
		           entry.cap_trigger_rumble ? 1 : 0);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

void CGamepadDeviceManager::CloseAndRemoveDevice(u32 instanceId, bool& outWasActive)
{
	outWasActive = false;
	for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
	{
		if (it->instance_id == instanceId)
		{
			if (m_activeInstanceId == instanceId)
			{
				outWasActive = true;
				m_activeInstanceId = 0;
			}

			// Immediate rumble stop stub (D1.1 p.4 / D5.2)
			if (it->gamepad)
			{
				SDL_RumbleGamepad(it->gamepad, 0, 0, 0);
				SDL_RumbleGamepadTriggers(it->gamepad, 0, 0, 0);
				SDL_CloseGamepad(it->gamepad);
				it->gamepad = nullptr;
				it->joystick = nullptr;
			}
			else if (it->joystick)
			{
				SDL_RumbleJoystick(it->joystick, 0, 0, 0);
				SDL_CloseJoystick(it->joystick);
				it->joystick = nullptr;
			}

			Msg("* [Gamepad] Device removed: id=%u name='%s' was_active=%d",
			    instanceId, it->name, outWasActive ? 1 : 0);

			m_devices.erase(it);
			return;
		}
	}
}

void CGamepadDeviceManager::RefreshDs4WindowsDuplicateSuppression()
{
	// D41: If a Sony DualShock/DualSense device is open via HIDAPI alongside a ViGEm Xbox 360 clone
	// (DS4Windows without HidHide), suppress the duplicate so they are not treated as two players.
	bool hasSonyPhysical = false;
	for (const auto& dev : m_devices)
	{
		if (IsSonyDevice(dev) && !dev.is_virtual)
		{
			hasSonyPhysical = true;
			break;
		}
	}

	for (auto& dev : m_devices)
	{
		const bool shouldSuppress = hasSonyPhysical && IsViGEmOrVirtualXbox360(dev);
		if (dev.is_ds4w_suppressed != shouldSuppress)
		{
			dev.is_ds4w_suppressed = shouldSuppress;
			Msg("* [Gamepad] DS4Windows duplicate filter: id=%u ('%s') suppressed=%d",
			    dev.instance_id, dev.name, shouldSuppress ? 1 : 0);
			if (shouldSuppress && m_activeInstanceId == dev.instance_id)
			{
				m_activeInstanceId = 0;
			}
		}
	}
}

void CGamepadDeviceManager::ReadDeviceRawInputs(SGamepadDeviceEntry& dev)
{
	if (dev.has_baseline)
	{
		for (int i = 0; i < 6; ++i)
			dev.prev_axes[i] = dev.raw_axes[i];
		dev.prev_buttons = dev.raw_buttons;
	}

	ZeroMemory(dev.raw_axes, sizeof(dev.raw_axes));
	dev.raw_buttons = 0;

	if (dev.gamepad)
	{
		dev.raw_axes[0] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_LEFTX);
		dev.raw_axes[1] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_LEFTY);
		dev.raw_axes[2] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
		dev.raw_axes[3] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
		dev.raw_axes[4] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
		dev.raw_axes[5] = SDL_GetGamepadAxis(dev.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

		const int btnLimit = (SDL_GAMEPAD_BUTTON_COUNT < 32) ? SDL_GAMEPAD_BUTTON_COUNT : 32;
		for (int b = 0; b < btnLimit; ++b)
		{
			if (SDL_GetGamepadButton(dev.gamepad, static_cast<SDL_GamepadButton>(b)))
				dev.raw_buttons |= (1u << b);
		}
	}
	else if (dev.joystick)
	{
		const int numAxes = SDL_GetNumJoystickAxes(dev.joystick);
		const int axisLimit = (numAxes < 6) ? numAxes : 6;
		for (int a = 0; a < axisLimit; ++a)
		{
			dev.raw_axes[a] = SDL_GetJoystickAxis(dev.joystick, a);
		}
		// D1.1-N5: Normalize full raw joystick trigger range (-32768..32767) into 0..32767 without losing half travel
		for (int t = 4; t < 6 && t < axisLimit; ++t)
		{
			const int rawVal = static_cast<int>(dev.raw_axes[t]);
			const int normVal = ((rawVal + 32768) * 32767) / 65535;
			dev.raw_axes[t] = static_cast<s16>(std::clamp(normVal, 0, 32767));
		}

		const int numBtns = SDL_GetNumJoystickButtons(dev.joystick);
		const int btnLimit = (numBtns < 32) ? numBtns : 32;
		for (int b = 0; b < btnLimit; ++b)
		{
			if (SDL_GetJoystickButton(dev.joystick, b))
				dev.raw_buttons |= (1u << b);
		}
	}

	if (!dev.has_baseline)
	{
		for (int i = 0; i < 6; ++i)
		{
			dev.prev_axes[i] = dev.raw_axes[i];
			const int thr = (i < 4) ? SIGNIFICANT_STICK_THRESHOLD : SIGNIFICANT_TRIGGER_THRESHOLD;
			const int mag = (i < 4) ? std::abs(static_cast<int>(dev.raw_axes[i])) : static_cast<int>(dev.raw_axes[i]);
			dev.axis_active[i] = (mag >= thr);
		}
		dev.prev_buttons = dev.raw_buttons;
		dev.has_baseline = true;
	}
}

bool CGamepadDeviceManager::HasSignificantInput(SGamepadDeviceEntry& dev)
{
	// D1.1-N1: Evaluate significance by state change with hysteresis (new button press, axis entering >= 8192
	// after dropping below 6144, or large delta >= 4096 above threshold), so static resting axes on throttles/pedals,
	// continuously held buttons, or micro-jitter around 8192 don't steal active device every frame.
	if (!dev.has_baseline)
		return false;

	bool significant = false;

	const u32 newlyPressedButtons = dev.raw_buttons & ~dev.prev_buttons;
	if (newlyPressedButtons != 0)
		significant = true;

	for (int i = 0; i < 4; ++i)
	{
		const int cur = std::abs(static_cast<int>(dev.raw_axes[i]));
		const int delta = std::abs(static_cast<int>(dev.raw_axes[i]) - static_cast<int>(dev.prev_axes[i]));
		if (!dev.axis_active[i])
		{
			if (cur >= SIGNIFICANT_STICK_THRESHOLD)
			{
				dev.axis_active[i] = true;
				significant = true;
			}
		}
		else
		{
			if (cur < SIGNIFICANT_STICK_RELEASE_THRESHOLD)
			{
				dev.axis_active[i] = false;
			}
			else if (cur >= SIGNIFICANT_STICK_THRESHOLD && delta >= SIGNIFICANT_AXIS_DELTA)
			{
				significant = true;
			}
		}
	}

	for (int i = 4; i < 6; ++i)
	{
		const int cur = static_cast<int>(dev.raw_axes[i]);
		const int prev = static_cast<int>(dev.prev_axes[i]);
		const int delta = std::abs(cur - prev);
		if (!dev.axis_active[i])
		{
			if (cur >= SIGNIFICANT_TRIGGER_THRESHOLD)
			{
				dev.axis_active[i] = true;
				significant = true;
			}
		}
		else
		{
			if (cur < SIGNIFICANT_TRIGGER_RELEASE_THRESHOLD)
			{
				dev.axis_active[i] = false;
			}
			else if (cur >= SIGNIFICANT_TRIGGER_THRESHOLD && delta >= SIGNIFICANT_AXIS_DELTA)
			{
				significant = true;
			}
		}
	}

	return significant;
}

void CGamepadDeviceManager::StopDeviceRumble(u32 instanceId)
{
	for (auto& dev : m_devices)
	{
		if (instanceId != 0 && dev.instance_id != instanceId)
			continue;

		if (dev.gamepad)
		{
			SDL_RumbleGamepad(dev.gamepad, 0, 0, 0);
			SDL_RumbleGamepadTriggers(dev.gamepad, 0, 0, 0);
		}
		else if (dev.joystick)
		{
			SDL_RumbleJoystick(dev.joystick, 0, 0, 0);
		}
	}
}

void CGamepadDeviceManager::RecordPollSampleUs(double us, u32 dwCurTime)
{
	m_lastPollUs = us;
	if (!ps_gp_telemetry)
		return;

	if (m_pollSamplesUs.size() < MAX_POLL_SAMPLES)
		m_pollSamplesUs.push_back(us);

	if (m_lastPerfReportTimeMs == 0)
		m_lastPerfReportTimeMs = dwCurTime;

	if ((dwCurTime - m_lastPerfReportTimeMs) >= PERF_REPORT_INTERVAL_MS && !m_pollSamplesUs.empty())
	{
		xr_vector<double> sorted = m_pollSamplesUs;
		std::sort(sorted.begin(), sorted.end());
		const double p50 = PercentileSorted(sorted, 0.50);
		const double p99 = PercentileSorted(sorted, 0.99);
		const double maxUs = sorted.back();

		if (pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_poll_perf_60s,%u,samples=%zu,p50_us=%.2f,p99_us=%.2f,max_us=%.2f",
			           dwCurTime, sorted.size(), p50, p99, maxUs);
			pInput->WriteGamepadTelemetryLine(line);
		}

		m_pollSamplesUs.clear();
		m_lastPerfReportTimeMs = dwCurTime;
	}
}

void CGamepadDeviceManager::PollHotplugAndSelectActive(u32 dwCurTime, bool bFocused, bool& outActiveRemoved, bool& outActiveChanged)
{
	outActiveRemoved = false;
	outActiveChanged = false;

	if (!m_bInitialized)
		return;

	LARGE_INTEGER qpcFreq, qpcStart, qpcEnd;
	QueryPerformanceFrequency(&qpcFreq);
	QueryPerformanceCounter(&qpcStart);

	// CR1-02: ensure g_gp_in_sdl_update is always cleared even if an SEH fault occurs inside SDL/HID poll.
	[]() {
		__try
		{
			g_gp_in_sdl_update = true;
			SDL_UpdateGamepads();
		}
		__finally
		{
			g_gp_in_sdl_update = false;
		}
	}();

	bool deviceListChanged = false;
	SDL_Event ev;

	// Process gamepad hotplug events (ADDED / REMOVED / REMAPPED)
	while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMAPPED) > 0)
	{
		if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMAPPED)
		{
			OpenAndRegisterDevice(ev.gdevice.which, dwCurTime);
			deviceListChanged = true;
		}
		else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED)
		{
			bool wasActive = false;
			CloseAndRemoveDevice(ev.gdevice.which, wasActive);
			if (wasActive)
				outActiveRemoved = true;
			deviceListChanged = true;
		}
	}

	// Process raw joystick hotplug events (for Class C unmapped joysticks)
	while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_JOYSTICK_ADDED, SDL_EVENT_JOYSTICK_REMOVED) > 0)
	{
		if (ev.type == SDL_EVENT_JOYSTICK_ADDED)
		{
			OpenAndRegisterDevice(ev.jdevice.which, dwCurTime);
			deviceListChanged = true;
		}
		else if (ev.type == SDL_EVENT_JOYSTICK_REMOVED)
		{
			bool wasActive = false;
			CloseAndRemoveDevice(ev.jdevice.which, wasActive);
			if (wasActive)
				outActiveRemoved = true;
			deviceListChanged = true;
		}
	}

	// Keep SDL_EventQ bounded so hotplug events are never dropped in long sessions
	SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_JOYSTICK_ADDED - 1);
	SDL_FlushEvents(SDL_EVENT_JOYSTICK_BATTERY_UPDATED, SDL_EVENT_GAMEPAD_ADDED - 1);
	SDL_FlushEvents(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);

	if (deviceListChanged)
	{
		RefreshDs4WindowsDuplicateSuppression();
	}

	const u32 prevActiveId = m_activeInstanceId;

	// Read inputs and select active device according to D36 ("last touched").
	// D1.1-N1: Raw unmapped joysticks (Class C) are read for gp_devices / future D4.2 wizard,
	// but never participate in active gamepad selection or fallback until mapped.
	for (auto& dev : m_devices)
	{
		if (dev.is_ds4w_suppressed)
		{
			ZeroMemory(dev.raw_axes, sizeof(dev.raw_axes));
			ZeroMemory(dev.prev_axes, sizeof(dev.prev_axes));
			ZeroMemory(dev.axis_active, sizeof(dev.axis_active));
			dev.raw_buttons = 0;
			dev.prev_buttons = 0;
			dev.has_baseline = false;
			continue;
		}

		ReadDeviceRawInputs(dev);

		if (dev.device_class == EGamepadClass::ClassC || !dev.gamepad)
			continue;

		if (bFocused && HasSignificantInput(dev))
		{
			dev.last_input_time_ms = dwCurTime;
			if (m_activeInstanceId != dev.instance_id)
			{
				m_activeInstanceId = dev.instance_id;
			}
		}
	}

	// Fallback if no active device selected yet: pick first non-suppressed mapped gamepad (Class A or B)
	if (m_activeInstanceId == 0 && !outActiveRemoved)
	{
		for (const auto& dev : m_devices)
		{
			if (!dev.is_ds4w_suppressed && dev.device_class != EGamepadClass::ClassC && dev.gamepad != nullptr)
			{
				m_activeInstanceId = dev.instance_id;
				break;
			}
		}
	}

	if (m_activeInstanceId != prevActiveId)
	{
		outActiveChanged = true;
		if (m_activeInstanceId != 0)
		{
			const SGamepadDeviceEntry* activeDev = GetActiveDevice();
			Msg("* [Gamepad] Active device switched: %u -> %u ('%s', class=%s)",
			    prevActiveId,
			    m_activeInstanceId,
			    activeDev ? activeDev->name : "none",
			    activeDev ? GetGamepadClassName(activeDev->device_class) : "None");

			if (pInput && ps_gp_telemetry)
			{
				string256 line;
				xr_sprintf(line, sizeof(line), "gp_active_switched,%u,from=%u,to=%u,class=%s",
				           dwCurTime, prevActiveId, m_activeInstanceId,
				           activeDev ? GetGamepadClassName(activeDev->device_class) : "None");
				pInput->WriteGamepadTelemetryLine(line);
			}
		}
	}

	QueryPerformanceCounter(&qpcEnd);
	const double pollUs = QueryPerfTicksToUs(qpcEnd.QuadPart - qpcStart.QuadPart, qpcFreq.QuadPart);
	RecordPollSampleUs(pollUs, dwCurTime);
}

void CGamepadDeviceManager::LogDevices() const
{
	Msg("* [Gamepad] gp_devices: count=%zu active_id=%u mappings_loaded=%d last_poll_us=%.2f",
	    m_devices.size(), m_activeInstanceId, m_loadedMappingsCount, m_lastPollUs);

	if (m_devices.empty())
	{
		Msg("* [Gamepad]   (no gamepad/joystick devices connected)");
		return;
	}

	for (size_t i = 0; i < m_devices.size(); ++i)
	{
		const auto& dev = m_devices[i];
		const bool isActive = (dev.instance_id == m_activeInstanceId);
		Msg("* [Gamepad]   [%zu] id=%u active=%d name='%s' class=%s type=%s real_type=%s vid=0x%04X pid=0x%04X guid=%s rumble=%d trig_rumble=%d led=%d gyro=%d accel=%d virtual=%d ds4w_suppressed=%d",
		    i,
		    dev.instance_id,
		    isActive ? 1 : 0,
		    dev.name,
		    GetGamepadClassName(dev.device_class),
		    GetSdlGamepadTypeName(dev.sdl_gamepad_type),
		    GetSdlGamepadTypeName(dev.sdl_real_gamepad_type),
		    dev.vendor_id,
		    dev.product_id,
		    dev.guid_str,
		    dev.cap_rumble ? 1 : 0,
		    dev.cap_trigger_rumble ? 1 : 0,
		    dev.cap_led ? 1 : 0,
		    dev.cap_gyro ? 1 : 0,
		    dev.cap_accel ? 1 : 0,
		    dev.is_virtual ? 1 : 0,
		    dev.is_ds4w_suppressed ? 1 : 0);
	}
}

void CGamepadDeviceManager::RunVirtualHotplugTest(int cycles)
{
	if (!m_bInitialized || !pInput)
	{
		Msg("! [Gamepad] gp_vjoy_hotplug: subsystem not initialized");
		return;
	}

	if (cycles < 1)
		cycles = 1;
	if (cycles > 1000)
		cycles = 1000;

	LARGE_INTEGER qpcFreq;
	QueryPerformanceFrequency(&qpcFreq);

	xr_vector<double> neutralUs;
	xr_vector<double> pollUs;
	neutralUs.reserve(static_cast<size_t>(cycles));
	pollUs.reserve(static_cast<size_t>(cycles) * 2);

	int successCycles = 0;

	for (int c = 0; c < cycles; ++c)
	{
		SDL_VirtualJoystickDesc desc;
		SDL_INIT_INTERFACE(&desc);
		desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
		desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
		desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
		desc.vendor_id = 0x045E;
		desc.product_id = 0x0B12; // Xbox Series X/S VID/PID
		desc.name = "GAMMA Virtual Test Gamepad";

		const SDL_JoystickID vId = SDL_AttachVirtualJoystick(&desc);
		if (vId == 0)
		{
			Msg("! [Gamepad] gp_vjoy_hotplug: SDL_AttachVirtualJoystick failed at cycle %d: %s", c, SDL_GetError());
			break;
		}

		// Pump one frame to process ADDED and open the virtual device
		LARGE_INTEGER tPoll1Start, tPoll1End;
		QueryPerformanceCounter(&tPoll1Start);
		pInput->GamepadUpdate();
		QueryPerformanceCounter(&tPoll1End);
		pollUs.push_back(QueryPerfTicksToUs(tPoll1End.QuadPart - tPoll1Start.QuadPart, qpcFreq.QuadPart));

		SGamepadDeviceEntry* vDev = FindDevice(vId);
		if (vDev && vDev->joystick)
		{
			// Simulate held RT (axis 5 = max), forward sprint on left stick (axis 1 = -32767) and L3 + South buttons
			SDL_SetJoystickVirtualAxis(vDev->joystick, SDL_GAMEPAD_AXIS_LEFTY, -32767);
			SDL_SetJoystickVirtualAxis(vDev->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
			SDL_SetJoystickVirtualButton(vDev->joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
			SDL_SetJoystickVirtualButton(vDev->joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
		}

		// Pump frame so held inputs become active
		pInput->GamepadUpdate();
		vDev = FindDevice(vId);

		// D1.1-N4: Verify that before Detach the virtual device became active and its held RT / sprint reached snapshot.
		// When the window is focused, check pInput->GetGamepadState(); if unfocused (focus gate zeroes snapshot),
		// verify active selection and raw inputs on vDev directly.
		const auto& preSt = pInput->GetGamepadState();
		const bool preActiveOk = preSt.has_focus
		                             ? ((preSt.instance_id == vId) && (preSt.axes[5] > 0) && (preSt.axes[1] < 0))
		                             : (vDev != nullptr && (m_activeInstanceId == vId || !preSt.has_focus) &&
		                                (vDev->raw_axes[5] > 0) && (vDev->raw_axes[1] < 0));

		// Detach while held and measure single-call Detach + GamepadUpdate cost to reach neutral snapshot in the same frame
		LARGE_INTEGER tNeutralStart, tNeutralEnd;
		QueryPerformanceCounter(&tNeutralStart);
		SDL_DetachVirtualJoystick(vId);
		pInput->GamepadUpdate();
		QueryPerformanceCounter(&tNeutralEnd);

		const double dtNeutralUs = QueryPerfTicksToUs(tNeutralEnd.QuadPart - tNeutralStart.QuadPart, qpcFreq.QuadPart);
		neutralUs.push_back(dtNeutralUs);
		pollUs.push_back(m_lastPollUs);

		const auto& st = pInput->GetGamepadState();
		const bool isNeutral = (st.instance_id != vId) &&
		                       (st.axes[0] == 0 && st.axes[1] == 0 && st.axes[2] == 0 &&
		                        st.axes[3] == 0 && st.axes[4] == 0 && st.axes[5] == 0 &&
		                        st.buttons_mask == 0);
		if (preActiveOk && isNeutral)
		{
			++successCycles;
		}
	}

	std::sort(neutralUs.begin(), neutralUs.end());
	std::sort(pollUs.begin(), pollUs.end());

	const double nP50 = PercentileSorted(neutralUs, 0.50);
	const double nP99 = PercentileSorted(neutralUs, 0.99);
	const double nMax = neutralUs.empty() ? 0.0 : neutralUs.back();

	const double pP50 = PercentileSorted(pollUs, 0.50);
	const double pP99 = PercentileSorted(pollUs, 0.99);
	const double pMax = pollUs.empty() ? 0.0 : pollUs.back();

	Msg("* [Gamepad] gp_vjoy_hotplug: cycles=%d/%d neutral_us(p50=%.2f p99=%.2f max=%.2f) poll_us(p50=%.2f p99=%.2f max=%.2f)",
	    successCycles, cycles, nP50, nP99, nMax, pP50, pP99, pMax);

	string512 line;
	xr_sprintf(line, sizeof(line),
	           "gp_vjoy_hotplug,%u,cycles=%d,ok=%d,neutral_p50_us=%.2f,neutral_p99_us=%.2f,neutral_max_us=%.2f,poll_p50_us=%.2f,poll_p99_us=%.2f,poll_max_us=%.2f",
	           pInput->dwCurTime, cycles, successCycles, nP50, nP99, nMax, pP50, pP99, pMax);
	pInput->WriteGamepadTelemetryLine(line);
}

const SGamepadDeviceEntry* CGamepadDeviceManager::FindFirstClassCDevice() const
{
	for (const auto& dev : m_devices)
	{
		if (dev.device_class == EGamepadClass::ClassC)
			return &dev;
	}
	return nullptr;
}

bool CGamepadDeviceManager::SampleRawJoystick(u32 instanceId, SRawJoystickSnapshot& outSnap) const
{
	ZeroMemory(&outSnap, sizeof(outSnap));
	if (!m_bInitialized)
		return false;

	SDL_UpdateJoysticks();

	const SGamepadDeviceEntry* dev = nullptr;
	if (instanceId != 0)
	{
		dev = FindDevice(instanceId);
	}
	else
	{
		dev = FindFirstClassCDevice();
		if (!dev)
			dev = GetActiveDevice();
		if (!dev && !m_devices.empty())
			dev = &m_devices.front();
	}

	if (!dev || !dev->joystick)
		return false;

	outSnap.instance_id = dev->instance_id;
	outSnap.valid = true;
	outSnap.device_class = dev->device_class;
	xr_strcpy(outSnap.name, sizeof(outSnap.name), dev->name);
	xr_strcpy(outSnap.guid_str, sizeof(outSnap.guid_str), dev->guid_str);

	const int numBtns = SDL_GetNumJoystickButtons(dev->joystick);
	const int numAxes = SDL_GetNumJoystickAxes(dev->joystick);
	const int numHats = SDL_GetNumJoystickHats(dev->joystick);
	outSnap.num_buttons = (numBtns > 0) ? numBtns : 0;
	outSnap.num_axes = (numAxes > 0) ? numAxes : 0;
	outSnap.num_hats = (numHats > 0) ? numHats : 0;

	const int btnLimit = (outSnap.num_buttons < 32) ? outSnap.num_buttons : 32;
	for (int b = 0; b < btnLimit; ++b)
	{
		if (SDL_GetJoystickButton(dev->joystick, b))
			outSnap.buttons_mask |= (1u << b);
	}

	const int axisLimit = (outSnap.num_axes < SRawJoystickSnapshot::MAX_RAW_AXES) ? outSnap.num_axes : SRawJoystickSnapshot::MAX_RAW_AXES;
	for (int a = 0; a < axisLimit; ++a)
	{
		outSnap.axes[a] = SDL_GetJoystickAxis(dev->joystick, a);
	}

	const int hatLimit = (outSnap.num_hats < SRawJoystickSnapshot::MAX_RAW_HATS) ? outSnap.num_hats : SRawJoystickSnapshot::MAX_RAW_HATS;
	for (int h = 0; h < hatLimit; ++h)
	{
		outSnap.hats[h] = SDL_GetJoystickHat(dev->joystick, h);
	}

	return true;
}

bool CGamepadDeviceManager::ApplyAndSaveCustomMapping(u32 instanceId, LPCSTR mappingString, bool saveToUserFile, LPSTR outSavedPath, u32 outSavedPathCap)
{
	if (outSavedPath && outSavedPathCap > 0)
		outSavedPath[0] = '\0';

	if (!m_bInitialized || !mappingString || !mappingString[0])
		return false;

	const int addRes = SDL_AddGamepadMapping(mappingString);
	if (addRes < 0)
	{
		Msg("! [Gamepad] Class C Wizard: SDL_AddGamepadMapping failed: %s (mapping='%s')", SDL_GetError(), mappingString);
		return false;
	}

	++m_loadedMappingsCount;

	const u32 nowMs = pInput ? pInput->dwCurTime : 0;
	if (instanceId != 0)
	{
		OpenAndRegisterDevice(instanceId, nowMs);
		RefreshDs4WindowsDuplicateSuppression();
		const SGamepadDeviceEntry* promoted = FindDevice(instanceId);
		if (promoted && promoted->gamepad && !promoted->is_ds4w_suppressed && m_activeInstanceId == 0)
		{
			m_activeInstanceId = instanceId;
		}
		if (promoted)
		{
			Msg("* [Gamepad] Class C Wizard applied mapping (res=%d) for id=%u ('%s') -> class=%s is_gamepad=%d",
			    addRes, instanceId, promoted->name, GetGamepadClassName(promoted->device_class),
			    SDL_IsGamepad(instanceId) ? 1 : 0);
		}
	}

	if (saveToUserFile)
	{
		string_path userMapPath = {};
		if (FS.path_exist("$app_data_root$"))
		{
			FS.update_path(userMapPath, "$app_data_root$", "gamepad\\user_mappings.txt");
		}
		else
		{
			xr_strcpy(userMapPath, sizeof(userMapPath), "appdata\\gamepad\\user_mappings.txt");
		}

		std::string forbidReason;
		if (gpbind::BindingProfileModel::IsPathForbiddenForWrite(userMapPath, &forbidReason))
		{
			Msg("! [Gamepad] Class C Wizard refused forbidden path '%s': %s", userMapPath, forbidReason.c_str());
			return false;
		}

		// Ensure parent directory exists
		{
			string_path dirBuf = {};
			xr_strcpy(dirBuf, sizeof(dirBuf), userMapPath);
			for (char* p = dirBuf; *p; ++p)
			{
				if (*p == '/' || *p == '\\')
				{
					const char saved = *p;
					*p = '\0';
					if (dirBuf[0] && xr_strlen(dirBuf) > 2)
						_mkdir(dirBuf);
					*p = saved;
				}
			}
		}

		const std::string newLineStr(mappingString);
		const std::size_t commaPos = newLineStr.find(',');
		const std::string newGuidPrefix = (commaPos != std::string::npos) ? newLineStr.substr(0, commaPos + 1) : "";

		std::vector<std::string> lines;
		bool replacedExisting = false;
		{
			std::ifstream ifs(userMapPath, std::ios::binary);
			if (ifs)
			{
				std::string ln;
				while (std::getline(ifs, ln))
				{
					if (!ln.empty() && ln.back() == '\r')
						ln.pop_back();
					if (!newGuidPrefix.empty() && ln.rfind(newGuidPrefix, 0) == 0)
					{
						lines.push_back(newLineStr);
						replacedExisting = true;
					}
					else if (!ln.empty())
					{
						lines.push_back(ln);
					}
				}
			}
		}

		if (lines.empty())
		{
			lines.push_back("# GAMMA User-Defined Class C Gamepad Mappings (D4.2)");
			lines.push_back("# Stored separately from gamecontrollerdb.txt so mod updates do not overwrite user mappings.");
		}
		if (!replacedExisting)
		{
			lines.push_back(newLineStr);
		}

		std::ofstream ofs(userMapPath, std::ios::binary | std::ios::trunc);
		if (!ofs)
		{
			Msg("! [Gamepad] Class C Wizard failed to open '%s' for writing", userMapPath);
			return false;
		}
		for (const auto& l : lines)
		{
			ofs << l << "\n";
		}
		ofs.close();

		if (outSavedPath && outSavedPathCap > 0)
		{
			strncpy_s(outSavedPath, outSavedPathCap, userMapPath, _TRUNCATE);
		}

		Msg("* [Gamepad] Class C Wizard saved custom mapping to '%s' (replaced=%d)", userMapPath, replacedExisting ? 1 : 0);
	}

	return true;
}

u32 CGamepadDeviceManager::AttachVirtualClassCDevice(LPCSTR customName)
{
	if (!m_bInitialized)
		return 0;

	static u16 s_virtualClassCSeq = 1;
	const u16 pid = static_cast<u16>(0xC100u + (s_virtualClassCSeq++ & 0x0FFFu));

	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
	desc.naxes = 6;
	desc.nbuttons = 16;
	desc.nhats = 1;
	desc.vendor_id = 0xFEED;
	desc.product_id = pid;
	desc.name = (customName && customName[0]) ? customName : "GAMMA Unmapped Class C Test Pad";

	const SDL_JoystickID vId = SDL_AttachVirtualJoystick(&desc);
	if (vId == 0)
	{
		Msg("! [Gamepad] AttachVirtualClassCDevice failed: %s", SDL_GetError());
		return 0;
	}

	if (pInput)
		pInput->GamepadUpdate();

	return vId;
}

bool CGamepadDeviceManager::SetVirtualClassCInput(u32 instanceId, int buttonIdx, bool pressed, int axisIdx, s16 axisVal, int hatIdx, u8 hatVal)
{
	SGamepadDeviceEntry* dev = FindDevice(instanceId);
	if (!dev || !dev->joystick)
		return false;

	if (buttonIdx >= 0)
		SDL_SetJoystickVirtualButton(dev->joystick, buttonIdx, pressed);
	if (axisIdx >= 0)
		SDL_SetJoystickVirtualAxis(dev->joystick, axisIdx, axisVal);
	if (hatIdx >= 0)
		SDL_SetJoystickVirtualHat(dev->joystick, hatIdx, hatVal);

	SDL_UpdateJoysticks();
	return true;
}

void CGamepadDeviceManager::DetachVirtualClassCDevice(u32 instanceId)
{
	if (instanceId == 0)
		return;
	SDL_DetachVirtualJoystick(instanceId);
	if (pInput)
		pInput->GamepadUpdate();
}
