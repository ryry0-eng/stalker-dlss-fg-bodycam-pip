#include "stdafx.h"
#pragma hdrstop

#include "GamepadHaptics.h"
#include "GamepadDeviceManager.h"
#include "xr_input.h"
#include "device.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

ENGINE_API float ps_gp_haptics_master = 1.0f;
ENGINE_API float ps_gp_haptics_recoil = 1.0f;
ENGINE_API float ps_gp_haptics_damage = 1.0f;
ENGINE_API float ps_gp_haptics_anomaly = 1.0f;
ENGINE_API float ps_gp_haptics_jam = 1.0f;
ENGINE_API float ps_gp_haptics_explosion = 1.0f;

namespace
{
constexpr u32 MAX_RUMBLE_PERF_SAMPLES = 2048;
constexpr u32 RUMBLE_PERF_REPORT_INTERVAL_MS = 60000;
constexpr double RUMBLE_WARN_THRESHOLD_US = 1000.0; // D5.2 stop condition: 1 ms = 1000 us

double QueryPerfTicksToUs(LONGLONG ticks, LONGLONG freq)
{
	if (freq <= 0)
		return 0.0;
	return (static_cast<double>(ticks) * 1000000.0) / static_cast<double>(freq);
}

std::string TrimStr(const std::string& s)
{
	const size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	const size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

std::string ToLowerStr(std::string s)
{
	for (char& c : s)
	{
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return s;
}

bool ResolveGamepadProfilePath(string_path& outPath)
{
	outPath[0] = 0;
	if (!FS.path_exist("$app_data_root$"))
		return false;

	FS.update_path(outPath, "$app_data_root$", "gamepad\\profile.ltx");
	std::string low = ToLowerStr(outPath);
	std::replace(low.begin(), low.end(), '/', '\\');
	// Safety invariant: never write to D:\Stalker, user.ltx, or axr_options.ltx
	if (low.find("d:\\stalker") != std::string::npos ||
	    low.find("user.ltx") != std::string::npos ||
	    low.find("axr_options.ltx") != std::string::npos)
	{
		return false;
	}
	return true;
}
} // namespace

CGamepadHaptics::CGamepadHaptics()
{
	ResetToDefaults();
	m_callSamplesUs.reserve(MAX_RUMBLE_PERF_SAMPLES);
}

CGamepadHaptics::~CGamepadHaptics()
{
	Shutdown();
}

void CGamepadHaptics::Initialize()
{
	if (m_bInitialized)
		return;

	ResetToDefaults();
	LoadFromProfile();
	m_bInitialized = true;

	Msg("* [Gamepad] CGamepadHaptics initialized: master=%.2f recoil=%.2f(%ums) damage=%.2f(%ums) anomaly=%.2f(%ums) jam=%.2f(%ums) explosion=%.2f(%ums)",
	    m_masterSlider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].duration_ms,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].duration_ms,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].duration_ms,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].duration_ms,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider,
	    m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].duration_ms);
}

void CGamepadHaptics::Shutdown()
{
	StopAll("shutdown");
	m_bInitialized = false;
}

void CGamepadHaptics::ResetToDefaults()
{
	m_masterSlider = 1.0f;

	// Channel 0: recoil (D40: sharp right trigger pulse if CAP_TRIGGER_RUMBLE, else crisp body pulse)
	{
		auto& c = m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)];
		c.slider = 1.0f;
		c.base_low = 0.45f;
		c.base_high = 0.75f;
		c.base_trigger = 0.85f;
		c.duration_ms = 65;
	}

	// Channel 1: damage (heavy impact on actor hit)
	{
		auto& c = m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)];
		c.slider = 1.0f;
		c.base_low = 0.85f;
		c.base_high = 0.65f;
		c.base_trigger = 0.0f;
		c.duration_ms = 220;
	}

	// Channel 2: anomaly (zone contact / anomaly field pulse)
	{
		auto& c = m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)];
		c.slider = 1.0f;
		c.base_low = 0.50f;
		c.base_high = 0.60f;
		c.base_trigger = 0.0f;
		c.duration_ms = 180;
	}

	// Channel 3: jam (sharp metallic click/jolt on weapon misfire or jam)
	{
		auto& c = m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)];
		c.slider = 1.0f;
		c.base_low = 0.60f;
		c.base_high = 0.95f;
		c.base_trigger = 0.0f;
		c.duration_ms = 150;
	}

	// Channel 4: explosion (deep blast wave rumble, scaled by distance)
	{
		auto& c = m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)];
		c.slider = 1.0f;
		c.base_low = 1.00f;
		c.base_high = 0.80f;
		c.base_trigger = 0.0f;
		c.duration_ms = 400;
	}

	for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
	{
		m_active[i] = SGamepadActiveHapticEvent{};
	}

	ps_gp_haptics_master = m_masterSlider;
	ps_gp_haptics_recoil = m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider;
	ps_gp_haptics_damage = m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider;
	ps_gp_haptics_anomaly = m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider;
	ps_gp_haptics_jam = m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider;
	ps_gp_haptics_explosion = m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider;
}

float CGamepadHaptics::SanitizeSlider(float v, float fallback)
{
	if (!std::isfinite(v))
		return std::clamp(fallback, 0.0f, 1.0f);
	return std::clamp(v, 0.0f, 1.0f);
}

u32 CGamepadHaptics::SanitizeDurationMs(int v, u32 fallback)
{
	if (v < 10 || v > 5000)
		return std::clamp<u32>(fallback, 10u, 5000u);
	return static_cast<u32>(v);
}

LPCSTR CGamepadHaptics::GetChannelName(EGamepadHapticChannel ch)
{
	switch (ch)
	{
	case EGamepadHapticChannel::Recoil:    return "recoil";
	case EGamepadHapticChannel::Damage:    return "damage";
	case EGamepadHapticChannel::Anomaly:   return "anomaly";
	case EGamepadHapticChannel::Jam:       return "jam";
	case EGamepadHapticChannel::Explosion: return "explosion";
	default:                               return "unknown";
	}
}

bool CGamepadHaptics::ParseChannelName(LPCSTR name, EGamepadHapticChannel& outCh)
{
	if (!name || !name[0])
		return false;

	if (0 == _stricmp(name, "recoil") || 0 == _stricmp(name, "shot") || 0 == _stricmp(name, "fire"))
	{
		outCh = EGamepadHapticChannel::Recoil;
		return true;
	}
	if (0 == _stricmp(name, "damage") || 0 == _stricmp(name, "hit"))
	{
		outCh = EGamepadHapticChannel::Damage;
		return true;
	}
	if (0 == _stricmp(name, "anomaly") || 0 == _stricmp(name, "zone"))
	{
		outCh = EGamepadHapticChannel::Anomaly;
		return true;
	}
	if (0 == _stricmp(name, "jam") || 0 == _stricmp(name, "misfire"))
	{
		outCh = EGamepadHapticChannel::Jam;
		return true;
	}
	if (0 == _stricmp(name, "explosion") || 0 == _stricmp(name, "explode") || 0 == _stricmp(name, "blast"))
	{
		outCh = EGamepadHapticChannel::Explosion;
		return true;
	}
	return false;
}

bool CGamepadHaptics::LoadFromProfile()
{
	string_path profilePath;
	if (!ResolveGamepadProfilePath(profilePath))
		return false;

	std::ifstream ifs(profilePath, std::ios::binary);
	if (!ifs)
		return false;

	std::string line;
	std::string curSection;
	bool foundHapticsSection = false;

	while (std::getline(ifs, line))
	{
		const size_t semi = line.find(';');
		if (semi != std::string::npos)
			line = line.substr(0, semi);

		line = TrimStr(line);
		if (line.empty())
			continue;

		if (line.front() == '[' && line.back() == ']')
		{
			curSection = ToLowerStr(TrimStr(line.substr(1, line.size() - 2)));
			if (curSection == "haptics")
				foundHapticsSection = true;
			continue;
		}

		if (curSection != "haptics")
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		const std::string key = ToLowerStr(TrimStr(line.substr(0, eq)));
		const std::string val = TrimStr(line.substr(eq + 1));
		if (key.empty() || val.empty())
			continue;

		char* endPtr = nullptr;
		const float fVal = std::strtof(val.c_str(), &endPtr);
		const bool validFloat = (endPtr != val.c_str()) && std::isfinite(fVal);

		if (key == "master_slider" || key == "master")
		{
			if (validFloat)
				m_masterSlider = SanitizeSlider(fVal, 1.0f);
			continue;
		}

		for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
		{
			const EGamepadHapticChannel ch = static_cast<EGamepadHapticChannel>(i);
			const std::string chPrefix = std::string(GetChannelName(ch)) + "_";
			if (key.rfind(chPrefix, 0) == 0)
			{
				const std::string subKey = key.substr(chPrefix.size());
				auto& cfg = m_configs[i];
				if (subKey == "slider" && validFloat)
					cfg.slider = SanitizeSlider(fVal, cfg.slider);
				else if (subKey == "low" && validFloat)
					cfg.base_low = SanitizeSlider(fVal, cfg.base_low);
				else if (subKey == "high" && validFloat)
					cfg.base_high = SanitizeSlider(fVal, cfg.base_high);
				else if (subKey == "trigger" && validFloat)
					cfg.base_trigger = SanitizeSlider(fVal, cfg.base_trigger);
				else if (subKey == "duration_ms" && validFloat)
					cfg.duration_ms = SanitizeDurationMs(static_cast<int>(fVal), cfg.duration_ms);
				break;
			}
		}
	}

	ps_gp_haptics_master = m_masterSlider;
	ps_gp_haptics_recoil = m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider;
	ps_gp_haptics_damage = m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider;
	ps_gp_haptics_anomaly = m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider;
	ps_gp_haptics_jam = m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider;
	ps_gp_haptics_explosion = m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider;

	return foundHapticsSection;
}

bool CGamepadHaptics::SaveToProfile() const
{
	string_path profilePath;
	if (!ResolveGamepadProfilePath(profilePath))
		return false;

	try
	{
		const std::filesystem::path p(profilePath);
		if (p.has_parent_path())
		{
			std::error_code ec;
			std::filesystem::create_directories(p.parent_path(), ec);
		}

		// Preserve all other sections in appdata\gamepad\profile.ltx (e.g. [calibration] from D2.2, [context_*] from D4.1)
		std::string existingBeforeHaptics;
		std::string existingAfterHaptics;
		bool hadHeader = false;

		{
			std::ifstream ifs(profilePath, std::ios::binary);
			if (ifs)
			{
				std::string line;
				bool inHaptics = false;
				bool seenHaptics = false;
				while (std::getline(ifs, line))
				{
					std::string trimmed = TrimStr(line);
					if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
					{
						std::string secName = ToLowerStr(TrimStr(trimmed.substr(1, trimmed.size() - 2)));
						if (secName == "profile_header" || secName == "header")
							hadHeader = true;

						if (secName == "haptics")
						{
							inHaptics = true;
							seenHaptics = true;
							continue;
						}
						else
						{
							inHaptics = false;
						}
					}

					if (!inHaptics)
					{
						if (!seenHaptics)
							existingBeforeHaptics += line + "\n";
						else
							existingAfterHaptics += line + "\n";
					}
				}
			}
		}

		std::ostringstream oss;
		if (!hadHeader && existingBeforeHaptics.empty())
		{
			oss << "[profile_header]\n";
			oss << "schema_version = " << PROFILE_SCHEMA_VERSION << "\n\n";
		}
		else if (!existingBeforeHaptics.empty())
		{
			oss << existingBeforeHaptics;
			if (existingBeforeHaptics.back() != '\n')
				oss << "\n";
		}

		oss << "[haptics]\n";
		char buf[128];
		xr_sprintf(buf, sizeof(buf), "master_slider = %.3f\n", SanitizeSlider(m_masterSlider, 1.0f));
		oss << buf;

		for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
		{
			const EGamepadHapticChannel ch = static_cast<EGamepadHapticChannel>(i);
			const LPCSTR chName = GetChannelName(ch);
			const auto& cfg = m_configs[i];

			xr_sprintf(buf, sizeof(buf), "%s_slider = %.3f\n", chName, SanitizeSlider(cfg.slider, 1.0f));
			oss << buf;
			xr_sprintf(buf, sizeof(buf), "%s_low = %.3f\n", chName, SanitizeSlider(cfg.base_low, 0.5f));
			oss << buf;
			xr_sprintf(buf, sizeof(buf), "%s_high = %.3f\n", chName, SanitizeSlider(cfg.base_high, 0.5f));
			oss << buf;
			if (ch == EGamepadHapticChannel::Recoil)
			{
				xr_sprintf(buf, sizeof(buf), "%s_trigger = %.3f\n", chName, SanitizeSlider(cfg.base_trigger, 0.85f));
				oss << buf;
			}
			xr_sprintf(buf, sizeof(buf), "%s_duration_ms = %u\n", chName, SanitizeDurationMs(static_cast<int>(cfg.duration_ms), 100));
			oss << buf;
		}
		oss << "\n";

		if (!existingAfterHaptics.empty())
			oss << existingAfterHaptics;

		std::string tempPathStr = std::string(profilePath) + ".tmp";
		{
			std::ofstream ofs(tempPathStr, std::ios::binary | std::ios::trunc);
			if (!ofs)
				return false;

			const std::string outText = oss.str();
			ofs.write(outText.data(), static_cast<std::streamsize>(outText.size()));
			if (!ofs.good())
			{
				ofs.close();
				DeleteFileA(tempPathStr.c_str());
				return false;
			}
		}

		if (!MoveFileExA(tempPathStr.c_str(), profilePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileA(tempPathStr.c_str());
			return false;
		}
		return true;
	}
	catch (...)
	{
		return false;
	}
}

void CGamepadHaptics::SetMasterSlider(float val, bool saveProfile)
{
	m_masterSlider = SanitizeSlider(val, 1.0f);
	ps_gp_haptics_master = m_masterSlider;
	if (m_masterSlider <= 0.0001f)
		StopAll("master_slider_zero");
	if (saveProfile)
		SaveToProfile();
}

void CGamepadHaptics::SyncSlidersFromCVarsAndSave()
{
	m_masterSlider = SanitizeSlider(ps_gp_haptics_master, m_masterSlider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider =
	    SanitizeSlider(ps_gp_haptics_recoil, m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider =
	    SanitizeSlider(ps_gp_haptics_damage, m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider =
	    SanitizeSlider(ps_gp_haptics_anomaly, m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider =
	    SanitizeSlider(ps_gp_haptics_jam, m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider =
	    SanitizeSlider(ps_gp_haptics_explosion, m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider);
	if (m_masterSlider <= 0.0001f)
		StopAll("master_slider_zero");
	SaveToProfile();
}

float CGamepadHaptics::GetChannelSlider(EGamepadHapticChannel ch) const
{
	const size_t idx = static_cast<size_t>(ch);
	if (idx >= static_cast<size_t>(EGamepadHapticChannel::Count))
		return 0.0f;
	return m_configs[idx].slider;
}

void CGamepadHaptics::SetChannelSlider(EGamepadHapticChannel ch, float val, bool saveProfile)
{
	const size_t idx = static_cast<size_t>(ch);
	if (idx >= static_cast<size_t>(EGamepadHapticChannel::Count))
		return;

	const float clean = SanitizeSlider(val, 1.0f);
	m_configs[idx].slider = clean;

	switch (ch)
	{
	case EGamepadHapticChannel::Recoil:    ps_gp_haptics_recoil = clean; break;
	case EGamepadHapticChannel::Damage:    ps_gp_haptics_damage = clean; break;
	case EGamepadHapticChannel::Anomaly:   ps_gp_haptics_anomaly = clean; break;
	case EGamepadHapticChannel::Jam:       ps_gp_haptics_jam = clean; break;
	case EGamepadHapticChannel::Explosion: ps_gp_haptics_explosion = clean; break;
	default: break;
	}

	if (saveProfile)
		SaveToProfile();
}

const SGamepadHapticChannelConfig& CGamepadHaptics::GetChannelConfig(EGamepadHapticChannel ch) const
{
	const size_t idx = static_cast<size_t>(ch);
	if (idx >= static_cast<size_t>(EGamepadHapticChannel::Count))
		return m_configs[0];
	return m_configs[idx];
}

void CGamepadHaptics::SetChannelConfig(EGamepadHapticChannel ch, const SGamepadHapticChannelConfig& cfg, bool saveProfile)
{
	const size_t idx = static_cast<size_t>(ch);
	if (idx >= static_cast<size_t>(EGamepadHapticChannel::Count))
		return;

	SGamepadHapticChannelConfig clean = cfg;
	clean.slider = SanitizeSlider(cfg.slider, 1.0f);
	clean.base_low = SanitizeSlider(cfg.base_low, 0.5f);
	clean.base_high = SanitizeSlider(cfg.base_high, 0.5f);
	clean.base_trigger = SanitizeSlider(cfg.base_trigger, 0.0f);
	clean.duration_ms = SanitizeDurationMs(static_cast<int>(cfg.duration_ms), 100);
	m_configs[idx] = clean;
	SetChannelSlider(ch, clean.slider, saveProfile);
}

void CGamepadHaptics::SetPauseTokenGate(bool hasActivePauseToken)
{
	const bool prev = m_bPauseTokenActive;
	m_bPauseTokenActive = hasActivePauseToken;
	if (!prev && hasActivePauseToken)
	{
		StopAll("pause_token_active");
	}
}

void CGamepadHaptics::TriggerChannel(EGamepadHapticChannel channel,
                                     float intensityScale,
                                     u32 durationOverrideMs,
                                     LPCSTR sourceTag,
                                     bool isConsoleTest)
{
	const size_t idx = static_cast<size_t>(channel);
	if (idx >= static_cast<size_t>(EGamepadHapticChannel::Count))
		return;

	// Sync console CVars into slider values before evaluating
	m_masterSlider = SanitizeSlider(ps_gp_haptics_master, m_masterSlider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider =
	    SanitizeSlider(ps_gp_haptics_recoil, m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider =
	    SanitizeSlider(ps_gp_haptics_damage, m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider =
	    SanitizeSlider(ps_gp_haptics_anomaly, m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider =
	    SanitizeSlider(ps_gp_haptics_jam, m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider =
	    SanitizeSlider(ps_gp_haptics_explosion, m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider);

	const bool bFocused = (RDEVICE.m_hWnd != NULL) &&
	                      (GetForegroundWindow() == RDEVICE.m_hWnd) &&
	                      !IsIconic(RDEVICE.m_hWnd);
	if (!bFocused)
		return;

	// Gate check: do not start gameplay vibration while paused, holding a pause token, or when active input source is KBM (unless explicit gp_rumble_test)
	if (!isConsoleTest)
	{
		if (m_bPauseTokenActive || RDEVICE.Paused())
			return;
		if (pInput && !pInput->IsGamepadActiveSource())
			return;
	}

	const auto& cfg = m_configs[idx];
	if (m_masterSlider <= 0.0001f || cfg.slider <= 0.0001f)
		return;

	const float clampedScale = std::isfinite(intensityScale) ? std::clamp(intensityScale, 0.05f, 1.50f) : 1.0f;
	const u32 durMs = (durationOverrideMs > 0)
	                      ? SanitizeDurationMs(static_cast<int>(durationOverrideMs), cfg.duration_ms)
	                      : cfg.duration_ms;

	const u32 nowMs = pInput ? pInput->dwCurTime : RDEVICE.TimerAsync_MMT();
	auto& ev = m_active[idx];

	// If channel is already active, take the maximum of remainingscale and new scale, and extend duration
	if (ev.active && nowMs < ev.end_time_ms)
	{
		ev.scale = std::max(ev.scale, clampedScale);
		ev.end_time_ms = std::max(ev.end_time_ms, nowMs + durMs);
		ev.bypass_gates = ev.bypass_gates || isConsoleTest;
	}
	else
	{
		ev.active = true;
		ev.start_time_ms = nowMs;
		ev.end_time_ms = nowMs + durMs;
		ev.scale = clampedScale;
		ev.bypass_gates = isConsoleTest;
	}

	if (pInput && ps_gp_telemetry)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_haptics_trigger,%u,ch=%s,scale=%.2f,dur_ms=%u,slider=%.2f,master=%.2f,src=%s",
		           nowMs,
		           GetChannelName(channel),
		           ev.scale,
		           durMs,
		           cfg.slider,
		           m_masterSlider,
		           (sourceTag && sourceTag[0]) ? sourceTag : "game");
		pInput->WriteGamepadTelemetryLine(line);
	}

	// Immediately evaluate and push to hardware so the pulse starts on the exact frame of the event
	OnFrame(nowMs, bFocused);
}

void CGamepadHaptics::PulseMenu(LPCSTR sourceTag)
{
	m_masterSlider = SanitizeSlider(ps_gp_haptics_master, m_masterSlider);
	if (!m_bInitialized || !pInput || !pInput->IsGamepadActiveSource() || !pInput->GetGamepadDeviceManager() ||
		m_masterSlider <= 0.0001f)
		return;
	const SGamepadDeviceEntry* dev = pInput->GetGamepadDeviceManager()->GetActiveDevice();
	if (!dev || !dev->gamepad || !dev->cap_rumble || dev->device_class == EGamepadClass::ClassC)
		return;
	const float k = std::clamp(m_masterSlider, 0.0f, 1.0f);
	const u16 low = static_cast<u16>(std::lround(0.16f * k * 65535.0f));
	const u16 high = static_cast<u16>(std::lround(0.24f * k * 65535.0f));
	// Not recorded as mixer state: the next mixer update (if any) simply takes over.
	SDL_RumbleGamepad(dev->gamepad, low, high, 70);
	(void)sourceTag;
}

void CGamepadHaptics::PulseSoftClick(LPCSTR sourceTag)
{
	m_masterSlider = SanitizeSlider(ps_gp_haptics_master, m_masterSlider);
	if (!m_bInitialized || !pInput || !pInput->IsGamepadActiveSource() || !pInput->GetGamepadDeviceManager() ||
		m_masterSlider <= 0.0001f)
		return;
	const SGamepadDeviceEntry* dev = pInput->GetGamepadDeviceManager()->GetActiveDevice();
	if (!dev || !dev->gamepad || !dev->cap_rumble || dev->device_class == EGamepadClass::ClassC)
		return;
	const float k = std::clamp(m_masterSlider, 0.0f, 1.0f);
	// INV-REWORK-15 (Misha 2026-11-12): View hold finished -> soft click, unlike the deny pulse
	// (PulseMenu: low 0.16 + high 0.24, 70 ms). Fine motor only, 0.20, 30 ms.
	const u16 high = static_cast<u16>(std::lround(0.20f * k * 65535.0f));
	SDL_RumbleGamepad(dev->gamepad, 0, high, 30);
	(void)sourceTag;
}

void CGamepadHaptics::StopAll(LPCSTR reason)
{
	LPCSTR safeReason = (reason && reason[0]) ? reason : "stop";

	bool hadActiveChannel = false;
	for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
	{
		if (m_active[i].active)
			hadActiveChannel = true;
		m_active[i] = SGamepadActiveHapticEvent{};
	}

	const bool wasRunning = m_bMotorsCurrentlyActive || hadActiveChannel;
	const bool forceHardwareZero =
	    wasRunning ||
	    (0 == _stricmp(safeReason, "console_stop")) ||
	    (0 == _stricmp(safeReason, "app_deactivate")) ||
	    (0 == _stricmp(safeReason, "focus_lost_in_sdl")) ||
	    (0 == _stricmp(safeReason, "device_removed")) ||
	    (0 == _stricmp(safeReason, "active_device_changed")) ||
	    (0 == _stricmp(safeReason, "shutdown"));

	if (!forceHardwareZero)
	{
		m_lastSentLow16 = 0;
		m_lastSentHigh16 = 0;
		m_lastSentLeftTrig16 = 0;
		m_lastSentRightTrig16 = 0;
		m_bMotorsCurrentlyActive = false;
		return;
	}

	const u32 nowMs = pInput ? pInput->dwCurTime : RDEVICE.TimerAsync_MMT();

	// Always issue hardware stop across all connected devices via DeviceManager (or active device)
	// when motors were active or on explicit safety stop events
	if (pInput && pInput->GetGamepadDeviceManager())
	{
		LARGE_INTEGER qpcFreq, qpcStart, qpcEnd;
		QueryPerformanceFrequency(&qpcFreq);
		QueryPerformanceCounter(&qpcStart);

		CGamepadDeviceManager* mgr = pInput->GetGamepadDeviceManager();
		for (const auto& dev : mgr->GetDevices())
		{
			if (dev.gamepad)
			{
				SDL_RumbleGamepad(dev.gamepad, 0, 0, 0);
				if (dev.cap_trigger_rumble)
					SDL_RumbleGamepadTriggers(dev.gamepad, 0, 0, 0);
			}
			else if (dev.joystick)
			{
				SDL_RumbleJoystick(dev.joystick, 0, 0, 0);
			}
		}

		QueryPerformanceCounter(&qpcEnd);
		const double callUs = QueryPerfTicksToUs(qpcEnd.QuadPart - qpcStart.QuadPart, qpcFreq.QuadPart);
		RecordRumbleCallUs(callUs, nowMs, "stop_all");
	}

	m_lastSentLow16 = 0;
	m_lastSentHigh16 = 0;
	m_lastSentLeftTrig16 = 0;
	m_lastSentRightTrig16 = 0;
	m_bMotorsCurrentlyActive = false;
	++m_totalStops;

	if (wasRunning && pInput && ps_gp_telemetry)
	{
		string256 line;
		xr_sprintf(line, sizeof(line), "gp_haptics_stop,%u,reason=%s,call_us=%.2f",
		           nowMs, safeReason, m_lastRumbleCallUs);
		pInput->WriteGamepadTelemetryLine(line);
	}
}

void CGamepadHaptics::OnFrame(u32 dwCurTime, bool bFocused)
{
	if (!m_bInitialized || !pInput || !pInput->GetGamepadDeviceManager())
		return;

	// Sync console CVars
	m_masterSlider = SanitizeSlider(ps_gp_haptics_master, m_masterSlider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider =
	    SanitizeSlider(ps_gp_haptics_recoil, m_configs[static_cast<size_t>(EGamepadHapticChannel::Recoil)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider =
	    SanitizeSlider(ps_gp_haptics_damage, m_configs[static_cast<size_t>(EGamepadHapticChannel::Damage)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider =
	    SanitizeSlider(ps_gp_haptics_anomaly, m_configs[static_cast<size_t>(EGamepadHapticChannel::Anomaly)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider =
	    SanitizeSlider(ps_gp_haptics_jam, m_configs[static_cast<size_t>(EGamepadHapticChannel::Jam)].slider);
	m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider =
	    SanitizeSlider(ps_gp_haptics_explosion, m_configs[static_cast<size_t>(EGamepadHapticChannel::Explosion)].slider);

	// 1. Focus / Alt-Tab gate
	if (!bFocused)
	{
		if (m_bMotorsCurrentlyActive)
			StopAll("focus_lost");
		return;
	}

	CGamepadDeviceManager* devMgr = pInput->GetGamepadDeviceManager();
	const SGamepadDeviceEntry* activeDev = devMgr->GetActiveDevice();
	const bool hasValidGamepad = (activeDev && activeDev->device_class != EGamepadClass::ClassC && activeDev->gamepad != nullptr);

	// 2. Device disconnect / active device switch check
	if (!hasValidGamepad)
	{
		if (m_bMotorsCurrentlyActive)
			StopAll("no_active_gamepad");
		return;
	}

	if (m_lastSentInstanceId != 0 && m_lastSentInstanceId != activeDev->instance_id && m_bMotorsCurrentlyActive)
	{
		devMgr->StopDeviceRumble(m_lastSentInstanceId);
		m_lastSentLow16 = 0;
		m_lastSentHigh16 = 0;
		m_lastSentLeftTrig16 = 0;
		m_lastSentRightTrig16 = 0;
		m_bMotorsCurrentlyActive = false;
	}

	// 3. Evaluate active channels and expire finished pulses
	bool anyBypassGates = false;
	for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
	{
		auto& ev = m_active[i];
		if (!ev.active)
			continue;

		if (dwCurTime >= ev.end_time_ms)
		{
			ev.active = false;
			ev.bypass_gates = false;
			continue;
		}
		if (ev.bypass_gates)
			anyBypassGates = true;
	}

	// 4. Pause token / engine pause / KBM source gate (stops all non-test channels immediately)
	if (!anyBypassGates)
	{
		if (m_bPauseTokenActive || RDEVICE.Paused())
		{
			if (m_bMotorsCurrentlyActive)
				StopAll(m_bPauseTokenActive ? "pause_token_gate" : "engine_paused");
			return;
		}
		if (!pInput->IsGamepadActiveSource())
		{
			if (m_bMotorsCurrentlyActive)
				StopAll("active_source_kbm");
			return;
		}
	}

	// 5. Max-per-channel mixing + master slider (D5.2 item 1)
	// Also implements D40 (D5.2 item 4): for recoil, if activeDev->cap_trigger_rumble is true,
	// route recoil to right trigger (RT); otherwise route recoil to body motors.
	float mixedLow = 0.0f;
	float mixedHigh = 0.0f;
	float mixedLeftTrig = 0.0f;
	float mixedRightTrig = 0.0f;
	u32 maxRemainingMs = 0;

	const float master = std::clamp(m_masterSlider, 0.0f, 1.0f);

	for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
	{
		const auto& ev = m_active[i];
		if (!ev.active || dwCurTime >= ev.end_time_ms)
			continue;

		const auto& cfg = m_configs[i];
		const float chGain = std::clamp(cfg.slider, 0.0f, 1.0f) * ev.scale * master;
		if (chGain <= 0.0001f)
			continue;

		const u32 remMs = ev.end_time_ms - dwCurTime;
		if (remMs > maxRemainingMs)
			maxRemainingMs = remMs;

		const EGamepadHapticChannel ch = static_cast<EGamepadHapticChannel>(i);
		if (ch == EGamepadHapticChannel::Recoil && activeDev->cap_trigger_rumble)
		{
			// D40: trigger rumble always enabled for shooting on supported gamepads (RT motor)
			const float rtAmp = std::clamp(cfg.base_trigger * chGain, 0.0f, 1.0f);
			mixedRightTrig = std::max(mixedRightTrig, rtAmp);
		}
		else
		{
			// Body rumble (or D40 fallback when device lacks SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN)
			const float lowAmp = std::clamp(cfg.base_low * chGain, 0.0f, 1.0f);
			const float highAmp = std::clamp(cfg.base_high * chGain, 0.0f, 1.0f);
			mixedLow = std::max(mixedLow, lowAmp);
			mixedHigh = std::max(mixedHigh, highAmp);
		}
	}

	const u16 targetLow16 = static_cast<u16>(std::clamp(static_cast<int>( std::lround(mixedLow * 65535.0f)), 0, 65535));
	const u16 targetHigh16 = static_cast<u16>(std::clamp(static_cast<int>(std::lround(mixedHigh * 65535.0f)), 0, 65535));
	const u16 targetLeftTrig16 = static_cast<u16>(std::clamp(static_cast<int>(std::lround(mixedLeftTrig * 65535.0f)), 0, 65535));
	const u16 targetRightTrig16 = static_cast<u16>(std::clamp(static_cast<int>(std::lround(mixedRightTrig * 65535.0f)), 0, 65535));

	const bool wantAnyMotor = (targetLow16 > 0 || targetHigh16 > 0 || targetLeftTrig16 > 0 || targetRightTrig16 > 0) && (maxRemainingMs > 0);

	if (!wantAnyMotor)
	{
		if (m_bMotorsCurrentlyActive)
		{
			ApplyHardwareRumble(dwCurTime, 0, 0, 0, 0, 0, "pulse_expired");
		}
		return;
	}

	// Avoid spamming SDL_RumbleGamepad every single frame if amplitudes haven't changed and hardware timer is still fresh
	const bool amplitudesChanged =
	    (targetLow16 != m_lastSentLow16) ||
	    (targetHigh16 != m_lastSentHigh16) ||
	    (targetLeftTrig16 != m_lastSentLeftTrig16) ||
	    (targetRightTrig16 != m_lastSentRightTrig16) ||
	    (activeDev->instance_id != m_lastSentInstanceId) ||
	    !m_bMotorsCurrentlyActive;

	const bool needsKeepAlive = (dwCurTime - m_lastSendTimeMs) >= 200;

	if (amplitudesChanged || needsKeepAlive)
	{
		ApplyHardwareRumble(dwCurTime,
		                    targetLow16,
		                    targetHigh16,
		                    targetLeftTrig16,
		                    targetRightTrig16,
		                    maxRemainingMs + 30, // small grace margin; OnFrame explicitly stops on exact end_time_ms
		                    "mixer_update");
	}
}

void CGamepadHaptics::ApplyHardwareRumble(u32 dwCurTime,
                                          u16 lowFreq16,
                                          u16 highFreq16,
                                          u16 leftTrig16,
                                          u16 rightTrig16,
                                          u32 durationMs,
                                          LPCSTR reasonTag)
{
	if (!pInput || !pInput->GetGamepadDeviceManager())
		return;

	const SGamepadDeviceEntry* activeDev = pInput->GetGamepadDeviceManager()->GetActiveDevice();
	if (!activeDev || !activeDev->gamepad || activeDev->device_class == EGamepadClass::ClassC)
		return;

	LARGE_INTEGER qpcFreq, qpcStart, qpcEnd;
	QueryPerformanceFrequency(&qpcFreq);
	QueryPerformanceCounter(&qpcStart);

	if (activeDev->cap_rumble || (lowFreq16 == 0 && highFreq16 == 0))
	{
		SDL_RumbleGamepad(activeDev->gamepad, lowFreq16, highFreq16, durationMs);
	}

	if (activeDev->cap_trigger_rumble)
	{
		SDL_RumbleGamepadTriggers(activeDev->gamepad, leftTrig16, rightTrig16, durationMs);
	}

	QueryPerformanceCounter(&qpcEnd);
	const double callUs = QueryPerfTicksToUs(qpcEnd.QuadPart - qpcStart.QuadPart, qpcFreq.QuadPart);
	RecordRumbleCallUs(callUs, dwCurTime, reasonTag);

	m_lastSentInstanceId = activeDev->instance_id;
	m_lastSentLow16 = lowFreq16;
	m_lastSentHigh16 = highFreq16;
	m_lastSentLeftTrig16 = leftTrig16;
	m_lastSentRightTrig16 = rightTrig16;
	m_lastSendTimeMs = dwCurTime;
	m_bMotorsCurrentlyActive = (lowFreq16 > 0 || highFreq16 > 0 || leftTrig16 > 0 || rightTrig16 > 0);
}

void CGamepadHaptics::RecordRumbleCallUs(double us, u32 dwCurTime, LPCSTR callKind)
{
	m_lastRumbleCallUs = us;
	if (us > m_maxRumbleCallUs)
		m_maxRumbleCallUs = us;
	++m_totalRumbleCalls;

	if (us > RUMBLE_WARN_THRESHOLD_US)
	{
		Msg("! [Gamepad] WARNING: SDL rumble call (%s) took %.2f us (> 1000 us threshold)",
		    (callKind && callKind[0]) ? callKind : "rumble", us);
		if (pInput && ps_gp_telemetry)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_haptics_slow_call,%u,kind=%s,us=%.2f",
			           dwCurTime, (callKind && callKind[0]) ? callKind : "rumble", us);
			pInput->WriteGamepadTelemetryLine(line);
		}
	}

	if (!ps_gp_telemetry)
		return;

	if (m_callSamplesUs.size() < MAX_RUMBLE_PERF_SAMPLES)
		m_callSamplesUs.push_back(us);

	if (m_lastPerfReportTimeMs == 0)
		m_lastPerfReportTimeMs = dwCurTime;

	if ((dwCurTime - m_lastPerfReportTimeMs) >= RUMBLE_PERF_REPORT_INTERVAL_MS && !m_callSamplesUs.empty())
	{
		xr_vector<double> sorted = m_callSamplesUs;
		std::sort(sorted.begin(), sorted.end());
		const size_t n = sorted.size();
		const double p50 = sorted[n / 2];
		const double p99 = sorted[std::min(n - 1, static_cast<size_t>(std::ceil(0.99 * static_cast<double>(n))) - 1)];
		const double maxUs = sorted.back();

		if (pInput)
		{
			string256 line;
			xr_sprintf(line, sizeof(line), "gp_haptics_perf_60s,%u,samples=%zu,p50_us=%.2f,p99_us=%.2f,max_us=%.2f",
			           dwCurTime, n, p50, p99, maxUs);
			pInput->WriteGamepadTelemetryLine(line);
		}

		m_callSamplesUs.clear();
		m_lastPerfReportTimeMs = dwCurTime;
	}
}

void CGamepadHaptics::LogStatus() const
{
	const SGamepadDeviceEntry* activeDev = (pInput && pInput->GetGamepadDeviceManager())
	                                           ? pInput->GetGamepadDeviceManager()->GetActiveDevice()
	                                           : nullptr;

	Msg("* [Gamepad] gp_rumble_test status: initialized=%d active_dev=%u('%s') cap_rumble=%d cap_trig_rumble=%d pause_gate=%d motors_active=%d master=%.2f calls=%u stops=%u last_call_us=%.2f max_call_us=%.2f",
	    m_bInitialized ? 1 : 0,
	    activeDev ? activeDev->instance_id : 0,
	    activeDev ? activeDev->name : "none",
	    (activeDev && activeDev->cap_rumble) ? 1 : 0,
	    (activeDev && activeDev->cap_trigger_rumble) ? 1 : 0,
	    m_bPauseTokenActive ? 1 : 0,
	    m_bMotorsCurrentlyActive ? 1 : 0,
	    m_masterSlider,
	    m_totalRumbleCalls,
	    m_totalStops,
	    m_lastRumbleCallUs,
	    m_maxRumbleCallUs);

	for (size_t i = 0; i < static_cast<size_t>(EGamepadHapticChannel::Count); ++i)
	{
		const EGamepadHapticChannel ch = static_cast<EGamepadHapticChannel>(i);
		const auto& cfg = m_configs[i];
		const auto& ev = m_active[i];
		Msg("* [Gamepad]   channel[%zu] %-9s: slider=%.2f low=%.2f high=%.2f trig=%.2f dur=%u ms active=%d",
		    i,
		    GetChannelName(ch),
		    cfg.slider,
		    cfg.base_low,
		    cfg.base_high,
		    cfg.base_trigger,
		    cfg.duration_ms,
		    ev.active ? 1 : 0);
	}
}

bool CGamepadHaptics::ExecuteConsoleTestCommand(LPCSTR args)
{
	if (!args || !args[0] || 0 == _stricmp(args, "status"))
	{
		LogStatus();
		return true;
	}

	string64 token1 = {};
	float scaleArg = 1.0f;
	u32 durArg = 0;
	const int parsed = sscanf(args, "%63s %f %u", token1, &scaleArg, &durArg);
	if (parsed < 1)
	{
		LogStatus();
		return false;
	}

	if (0 == _stricmp(token1, "stop") || 0 == _stricmp(token1, "off") || 0 == _stricmp(token1, "0"))
	{
		StopAll("console_stop");
		Msg("* [Gamepad] gp_rumble_test: all haptics stopped immediately (last_call_us=%.2f)", m_lastRumbleCallUs);
		return true;
	}

	if (0 == _stricmp(token1, "save"))
	{
		const bool ok = SaveToProfile();
		Msg("* [Gamepad] gp_rumble_test save -> appdata\\gamepad\\profile.ltx: ok=%d", ok ? 1 : 0);
		return ok;
	}

	if (0 == _stricmp(token1, "load"))
	{
		const bool ok = LoadFromProfile();
		Msg("* [Gamepad] gp_rumble_test load <- appdata\\gamepad\\profile.ltx: ok=%d", ok ? 1 : 0);
		LogStatus();
		return ok;
	}

	if (0 == _stricmp(token1, "reset"))
	{
		ResetToDefaults();
		SaveToProfile();
		Msg("* [Gamepad] gp_rumble_test reset: restored default haptics sliders and saved to profile.ltx");
		LogStatus();
		return true;
	}

	EGamepadHapticChannel ch = EGamepadHapticChannel::Recoil;
	if (!ParseChannelName(token1, ch))
	{
		Msg("! [Gamepad] Usage: gp_rumble_test <recoil|damage|anomaly|jam|explosion|stop|status|save|load|reset> [scale] [duration_ms]");
		return false;
	}

	const float scale = (parsed >= 2) ? std::clamp(scaleArg, 0.05f, 1.50f) : 1.0f;
	const u32 dur = (parsed >= 3) ? durArg : 0;

	TriggerChannel(ch, scale, dur, "gp_rumble_test", true);

	const SGamepadDeviceEntry* activeDev = (pInput && pInput->GetGamepadDeviceManager())
	                                           ? pInput->GetGamepadDeviceManager()->GetActiveDevice()
	                                           : nullptr;
	const bool routedToTrigger = (ch == EGamepadHapticChannel::Recoil) && activeDev && activeDev->cap_trigger_rumble;

	Msg("* [Gamepad] gp_rumble_test: triggered channel='%s' scale=%.2f dur=%u ms route=%s low16=%u high16=%u rt16=%u call_us=%.2f",
	    GetChannelName(ch),
	    scale,
	    (dur > 0) ? dur : GetChannelConfig(ch).duration_ms,
	    routedToTrigger ? "RT_trigger(D40)" : "body_motors",
	    m_lastSentLow16,
	    m_lastSentHigh16,
	    m_lastSentRightTrig16,
	    m_lastRumbleCallUs);

	return true;
}
