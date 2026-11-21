#ifndef __GAMEPAD_HAPTICS_H__
#define __GAMEPAD_HAPTICS_H__

#pragma once

// D5.2: Gamepad Haptics mixer (CGamepadHaptics)
// - 5 event channels: recoil, damage, anomaly, jam, explosion
// - Per-channel base strength (low_freq, high_freq, trigger_rt), duration_ms, and profile slider [0.0 .. 1.0]
// - Max-per-channel mixing + master slider [0.0 .. 1.0]
// - Trigger rumble always enabled (D40): on recoil, routes to RT trigger motor if SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN
//   is supported on the active device; otherwise falls back to body rumble
// - Immediate stop (SDL_RumbleGamepad(0,0,0) + SDL_RumbleGamepadTriggers(0,0,0)):
//   active pause token, focus loss / Alt-Tab, device disconnect, game exit / disconnect, save load
// - Console command gp_rumble_test <channel|stop|status|save|load> and profile storage in appdata\gamepad\profile.ltx (D37)

enum class EGamepadHapticChannel : u8
{
	Recoil = 0,
	Damage = 1,
	Anomaly = 2,
	Jam = 3,
	Explosion = 4,

	Count = 5
};

struct SGamepadHapticChannelConfig
{
	float slider = 1.0f;       // Per-channel volume slider in profile [0.0 .. 1.0]
	float base_low = 0.5f;     // Base low-frequency (left heavy motor) amplitude [0.0 .. 1.0]
	float base_high = 0.5f;    // Base high-frequency (right fine motor) amplitude [0.0 .. 1.0]
	float base_trigger = 0.0f; // Base right-trigger motor amplitude for recoil (D40) [0.0 .. 1.0]
	u32 duration_ms = 100;     // Default pulse duration in milliseconds [10 .. 5000]
};

struct SGamepadActiveHapticEvent
{
	bool active = false;
	u32 start_time_ms = 0;
	u32 end_time_ms = 0;
	float scale = 1.0f;        // Dynamic event intensity scale [0.0 .. 2.0]
	bool bypass_gates = false; // True only for explicit console test when user tests from console/menu
};

class ENGINE_API CGamepadHaptics
{
public:
	static constexpr u32 PROFILE_SCHEMA_VERSION = 2;

	CGamepadHaptics();
	~CGamepadHaptics();

	void Initialize();
	void Shutdown();

	// Reset all sliders and channel parameters to safe defaults
	void ResetToDefaults();

	// Load / save [haptics] section in appdata\gamepad\profile.ltx (D37) without overwriting other sections
	bool LoadFromProfile();
	bool SaveToProfile() const;
	void SyncSlidersFromCVarsAndSave();

	// Trigger a haptic event on one of the 5 channels.
	// intensityScale scales the channel's configured strength (clamped to [0.05 .. 1.50]).
	// durationOverrideMs == 0 uses the channel's configured duration_ms.
	void TriggerChannel(EGamepadHapticChannel channel,
	                    float intensityScale = 1.0f,
	                    u32 durationOverrideMs = 0,
	                    LPCSTR sourceTag = nullptr,
	                    bool isConsoleTest = false);

	// MENU-FOCUS P16: one short weak pulse for menu feedback (hold finished, quick save, save refused).
	// Works while the game is paused; respects the master slider; the hardware timer ends it.
	void PulseMenu(LPCSTR sourceTag = nullptr);
	// INV-REWORK-15: soft click (fine motor only, shorter and weaker than PulseMenu "deny").
	void PulseSoftClick(LPCSTR sourceTag = nullptr);

	// Per-frame mixer update called from CInput::GamepadUpdate()
	void OnFrame(u32 dwCurTime, bool bFocused);

	// External gate notifications from xrGame (PauseTokenManager, Level/GamePersistent)
	void SetPauseTokenGate(bool hasActivePauseToken);
	bool GetPauseTokenGate() const { return m_bPauseTokenActive; }

	// Immediate stop of all motors and active channel timers (reason logged to telemetry)
	void StopAll(LPCSTR reason);

	// Sliders & channel config accessors (for profile UI and console)
	float GetMasterSlider() const { return m_masterSlider; }
	void SetMasterSlider(float val, bool saveProfile = false);

	float GetChannelSlider(EGamepadHapticChannel ch) const;
	void SetChannelSlider(EGamepadHapticChannel ch, float val, bool saveProfile = false);

	const SGamepadHapticChannelConfig& GetChannelConfig(EGamepadHapticChannel ch) const;
	void SetChannelConfig(EGamepadHapticChannel ch, const SGamepadHapticChannelConfig& cfg, bool saveProfile = false);

	// Telemetry & diagnostics
	double GetLastRumbleCallUs() const { return m_lastRumbleCallUs; }
	double GetMaxRumbleCallUs() const { return m_maxRumbleCallUs; }
	u32 GetTotalRumbleCalls() const { return m_totalRumbleCalls; }
	u32 GetTotalStops() const { return m_totalStops; }

	void LogStatus() const;
	bool ExecuteConsoleTestCommand(LPCSTR args);

	static LPCSTR GetChannelName(EGamepadHapticChannel ch);
	static bool ParseChannelName(LPCSTR name, EGamepadHapticChannel& outCh);

private:
	void ApplyHardwareRumble(u32 dwCurTime,
	                         u16 lowFreq16,
	                         u16 highFreq16,
	                         u16 leftTrig16,
	                         u16 rightTrig16,
	                         u32 durationMs,
	                         LPCSTR reasonTag);
	void RecordRumbleCallUs(double us, u32 dwCurTime, LPCSTR callKind);
	static float SanitizeSlider(float v, float fallback = 1.0f);
	static u32 SanitizeDurationMs(int v, u32 fallback = 100);

private:
	SGamepadHapticChannelConfig m_configs[static_cast<size_t>(EGamepadHapticChannel::Count)];
	SGamepadActiveHapticEvent m_active[static_cast<size_t>(EGamepadHapticChannel::Count)];

	float m_masterSlider = 1.0f;
	bool m_bInitialized = false;
	bool m_bPauseTokenActive = false;

	// Last sent hardware state to avoid redundant SDL rumble calls on every frame
	u32 m_lastSentInstanceId = 0;
	u16 m_lastSentLow16 = 0;
	u16 m_lastSentHigh16 = 0;
	u16 m_lastSentLeftTrig16 = 0;
	u16 m_lastSentRightTrig16 = 0;
	u32 m_lastSendTimeMs = 0;
	bool m_bMotorsCurrentlyActive = false;

	// Timing telemetry (D5.2 stop condition: rumble call must not block frame > 1 ms = 1000 us)
	xr_vector<double> m_callSamplesUs;
	u32 m_lastPerfReportTimeMs = 0;
	double m_lastRumbleCallUs = 0.0;
	double m_maxRumbleCallUs = 0.0;
	u32 m_totalRumbleCalls = 0;
	u32 m_totalStops = 0;
};

// Extern console-bound variables for quick inspection / tweaking
extern ENGINE_API float ps_gp_haptics_master;
extern ENGINE_API float ps_gp_haptics_recoil;
extern ENGINE_API float ps_gp_haptics_damage;
extern ENGINE_API float ps_gp_haptics_anomaly;
extern ENGINE_API float ps_gp_haptics_jam;
extern ENGINE_API float ps_gp_haptics_explosion;

#endif // __GAMEPAD_HAPTICS_H__
