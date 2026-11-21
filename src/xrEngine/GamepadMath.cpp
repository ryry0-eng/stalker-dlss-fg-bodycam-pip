#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
// D2.1 + D2.2: pure gamepad stick/trigger math and D37 calibration profile persistence.
// Compiled with PrecompiledHeader=NotUsing in xrEngine.vcxproj so it also builds standalone in tools/tests/gamepad_math.
#include "GamepadMath.h"
#include "GamepadAtomicFile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <string>
#include <vector>
#include <limits>
#include <direct.h>

namespace gpmath
{
namespace
{
const StickConfig kDefaultStick{};
const TriggerConfig kDefaultTrigger{};
const CalibrationProfile kDefaultProfile{};

CalibrationProfile g_activeProfile{};
CalibrationLoadReport g_lastLoadReport{};
char g_profilePathBuf[520] = "appdata\\gamepad\\profile.ltx";
LogCallbackFn g_logCallback = nullptr;

inline void EmitLog(const char* fmt, ...)
{
    if (!g_logCallback || !fmt)
        return;
    char buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_logCallback(buf);
}

inline bool IsFinite(float v) { return std::isfinite(v); }

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float Clamp01(float v)
{
    if (!(v > 0.0f)) // also catches NaN
        return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

// Fix one field: non-finite -> default, otherwise clamp. Sets flag if changed.
inline void FixField(float& v, float def, float lo, float hi, std::uint32_t flag, std::uint32_t& out)
{
    // the default is clamped too: dependent ranges (release < press, saturation > deadzone)
    // may exclude it (found by D2.1 fuzz: NaN release -> 0.25 > press 0.05)
    const float fixed = Clamp(IsFinite(v) ? v : def, lo, hi);
    if (fixed != v)
    {
        v = fixed;
        out |= flag;
    }
}

// Deadzone + saturation remap of a non-negative magnitude to [0,1].
inline float Remap(float mag, float dz, float sat)
{
    if (mag <= dz)
        return 0.0f;
    return Clamp01((mag - dz) / (sat - dz)); // sanitizer guarantees sat - dz >= kMinSpan
}

inline float SafeInput(float v) { return IsFinite(v) ? Clamp(v, -1.0f, 1.0f) : 0.0f; }

inline std::string TrimAscii(const std::string& s)
{
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

inline std::string ToLowerAscii(const std::string& s)
{
    std::string r = s;
    for (char& ch : r)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return r;
}

inline bool IsOurCalibrationSection(const std::string& secLower)
{
    return secLower == "profile" ||
           secLower == "left_stick" ||
           secLower == "right_stick" ||
           secLower == "ui_stick" ||
           secLower == "left_trigger" ||
           secLower == "right_trigger";
}

inline bool TryParseStrictFloat(const std::string& valStr, float& outVal)
{
    if (valStr.empty())
        return false;
    // Reject literal "nan" / "inf" strings as parse errors
    const std::string low = ToLowerAscii(valStr);
    if (low.find("nan") != std::string::npos || low.find("inf") != std::string::npos)
        return false;
    char* endPtr = nullptr;
    const float v = std::strtof(valStr.c_str(), &endPtr);
    if (endPtr == valStr.c_str() || (endPtr && *endPtr != '\0') || !std::isfinite(v))
        return false;
    outVal = v;
    return true;
}

inline bool TryParseStrictUint(const std::string& valStr, std::uint32_t& outVal)
{
    if (valStr.empty() || valStr[0] == '-')
        return false;
    char* endPtr = nullptr;
    const unsigned long v = std::strtoul(valStr.c_str(), &endPtr, 10);
    if (endPtr == valStr.c_str() || (endPtr && *endPtr != '\0') || v > 0xFFFFFFFFul)
        return false;
    outVal = static_cast<std::uint32_t>(v);
    return true;
}

inline bool TryParseBool(const std::string& valStr, bool& outVal)
{
    const std::string low = ToLowerAscii(valStr);
    if (low == "1" || low == "true" || low == "on" || low == "yes")
    {
        outVal = true;
        return true;
    }
    if (low == "0" || low == "false" || low == "off" || low == "no")
    {
        outVal = false;
        return true;
    }
    return false;
}

inline void ParseStickKey(StickConfig& stk,
                          const std::string& key,
                          const std::string& val,
                          CalibrationLoadReport& rep,
                          bool isRightStick,
                          CalibrationProfile& prof)
{
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    if (key == "shape")
    {
        const std::string low = ToLowerAscii(val);
        if (low == "radial" || low == "0")
            stk.shape = DeadzoneShape::Radial;
        else if (low == "axial" || low == "1")
            stk.shape = DeadzoneShape::Axial;
        else
        {
            stk.shape = static_cast<DeadzoneShape>(0xFF);
            ++rep.parse_warnings;
        }
    }
    else if (key == "curve" || key == "curve_type")
    {
        const std::string low = ToLowerAscii(val);
        if (low == "linear" || low == "0")
            stk.curve.type = CurveType::Linear;
        else if (low == "power" || low == "1")
            stk.curve.type = CurveType::Power;
        else if (low == "scurve" || low == "s_curve" || low == "2")
            stk.curve.type = CurveType::SCurve;
        else if (low == "twozone" || low == "two_zone" || low == "3")
            stk.curve.type = CurveType::TwoZone;
        else
        {
            stk.curve.type = static_cast<CurveType>(0xFF);
            ++rep.parse_warnings;
        }
    }
    else if (key == "deadzone")
    {
        if (!TryParseStrictFloat(val, stk.deadzone))
        {
            stk.deadzone = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "saturation")
    {
        if (!TryParseStrictFloat(val, stk.saturation))
        {
            stk.saturation = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "gamma")
    {
        if (!TryParseStrictFloat(val, stk.curve.gamma))
        {
            stk.curve.gamma = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "s_exponent")
    {
        if (!TryParseStrictFloat(val, stk.curve.s_exponent))
        {
            stk.curve.s_exponent = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "knee_in")
    {
        if (!TryParseStrictFloat(val, stk.curve.knee_in))
        {
            stk.curve.knee_in = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "knee_out")
    {
        if (!TryParseStrictFloat(val, stk.curve.knee_out))
        {
            stk.curve.knee_out = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "invert_x")
    {
        if (!TryParseBool(val, stk.invert_x))
        {
            stk.invert_x = false;
            ++rep.parse_warnings;
        }
    }
    else if (key == "invert_y")
    {
        if (!TryParseBool(val, stk.invert_y))
        {
            stk.invert_y = false;
            ++rep.parse_warnings;
        }
    }
    else if (isRightStick && key == "omega_horz")
    {
        if (!TryParseStrictFloat(val, prof.omega_horz))
        {
            prof.omega_horz = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (isRightStick && key == "omega_vert")
    {
        if (!TryParseStrictFloat(val, prof.omega_vert))
        {
            prof.omega_vert = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (isRightStick && key == "ads_sens_mult")
    {
        if (!TryParseStrictFloat(val, prof.ads_sens_mult))
        {
            prof.ads_sens_mult = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (isRightStick && key == "turn_accel")
    {
        if (!TryParseStrictFloat(val, prof.turn_accel))
        {
            prof.turn_accel = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (isRightStick && key == "disable_aa_on_scope")
    {
        if (!TryParseBool(val, prof.disable_aa_on_scope))
        {
            prof.disable_aa_on_scope = true;
            ++rep.parse_warnings;
        }
    }
}

inline void ParseTriggerKey(TriggerConfig& trg,
                            const std::string& key,
                            const std::string& val,
                            CalibrationLoadReport& rep)
{
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    if (key == "deadzone")
    {
        if (!TryParseStrictFloat(val, trg.deadzone))
        {
            trg.deadzone = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "saturation")
    {
        if (!TryParseStrictFloat(val, trg.saturation))
        {
            trg.saturation = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "press")
    {
        if (!TryParseStrictFloat(val, trg.press))
        {
            trg.press = nanVal;
            ++rep.parse_warnings;
        }
    }
    else if (key == "release")
    {
        if (!TryParseStrictFloat(val, trg.release))
        {
            trg.release = nanVal;
            ++rep.parse_warnings;
        }
    }
}

inline void EnsureParentDirectories(const char* fullPath)
{
    if (!fullPath || !fullPath[0])
        return;
    char dirBuf[520] = {};
    std::strncpy(dirBuf, fullPath, sizeof(dirBuf) - 1);
    for (char* p = dirBuf; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            const char saved = *p;
            *p = '\0';
            if (dirBuf[0] && std::strlen(dirBuf) > 2)
                _mkdir(dirBuf);
            *p = saved;
        }
    }
}

inline void ResolveDefaultProfilePath()
{
}
} // namespace

void SetCalibrationProfilePath(const char* fullPath)
{
    if (fullPath && fullPath[0])
    {
        std::strncpy(g_profilePathBuf, fullPath, sizeof(g_profilePathBuf) - 1);
        g_profilePathBuf[sizeof(g_profilePathBuf) - 1] = '\0';
    }
}

void SetCalibrationLogCallback(LogCallbackFn cb)
{
    g_logCallback = cb;
}

const char* GetDeadzoneShapeName(DeadzoneShape s)
{
    switch (s)
    {
    case DeadzoneShape::Axial: return "axial";
    case DeadzoneShape::Radial:
    default: return "radial";
    }
}

const char* GetCurveTypeName(CurveType c)
{
    switch (c)
    {
    case CurveType::Power: return "power";
    case CurveType::SCurve: return "scurve";
    case CurveType::TwoZone: return "twozone";
    case CurveType::Linear:
    default: return "linear";
    }
}

std::uint32_t SanitizeStick(StickConfig& cfg)
{
    using namespace limits;
    std::uint32_t out = 0;

    if (static_cast<std::uint8_t>(cfg.shape) > static_cast<std::uint8_t>(DeadzoneShape::Axial))
    {
        cfg.shape = kDefaultStick.shape;
        out |= kFixShape;
    }
    if (static_cast<std::uint8_t>(cfg.curve.type) > static_cast<std::uint8_t>(CurveType::TwoZone))
    {
        cfg.curve.type = kDefaultStick.curve.type;
        out |= kFixCurveType;
    }

    FixField(cfg.deadzone, kDefaultStick.deadzone, kDeadzoneMin, kDeadzoneMax, kFixDeadzone, out);
    // saturation must stay above deadzone by kMinSpan; deadzone <= 0.90 so the range is never empty
    FixField(cfg.saturation, kDefaultStick.saturation, cfg.deadzone + kMinSpan, kSaturationMax, kFixSaturation, out);

    FixField(cfg.curve.gamma, kDefaultStick.curve.gamma, kExpMin, kExpMax, kFixGamma, out);
    FixField(cfg.curve.s_exponent, kDefaultStick.curve.s_exponent, kExpMin, kExpMax, kFixSExponent, out);
    FixField(cfg.curve.knee_in, kDefaultStick.curve.knee_in, kKneeMin, kKneeMax, kFixKneeIn, out);
    FixField(cfg.curve.knee_out, kDefaultStick.curve.knee_out, kKneeMin, kKneeMax, kFixKneeOut, out);
    return out;
}

std::uint32_t SanitizeTrigger(TriggerConfig& cfg)
{
    using namespace limits;
    std::uint32_t out = 0;
    FixField(cfg.deadzone, kDefaultTrigger.deadzone, kDeadzoneMin, kDeadzoneMax, kFixDeadzone, out);
    FixField(cfg.saturation, kDefaultTrigger.saturation, cfg.deadzone + kMinSpan, kSaturationMax, kFixSaturation, out);
    // press in [kTriggerMinGap, 1]; release in [0, press - gap]
    FixField(cfg.press, kDefaultTrigger.press, kTriggerMinGap, 1.0f, kFixPress, out);
    FixField(cfg.release, kDefaultTrigger.release, 0.0f, cfg.press - kTriggerMinGap, kFixRelease, out);
    return out;
}

std::uint32_t SanitizeProfile(CalibrationProfile& prof, CalibrationLoadReport* outReport)
{
    using namespace limits;
    std::uint32_t pflags = 0;

    if (prof.version != kProfileFormatVersion)
    {
        prof.version = kProfileFormatVersion;
        pflags |= kProfileFixVersion;
    }

    const std::uint32_t lsFlags = SanitizeStick(prof.left_stick);
    if (lsFlags != 0)
        pflags |= kProfileFixLeftStick;

    const std::uint32_t rsFlags = SanitizeStick(prof.right_stick);
    if (rsFlags != 0)
        pflags |= kProfileFixRightStick;

    const std::uint32_t uiFlags = SanitizeStick(prof.ui_stick);
    if (uiFlags != 0)
        pflags |= kProfileFixUiStick;

    const std::uint32_t ltFlags = SanitizeTrigger(prof.left_trigger);
    if (ltFlags != 0)
        pflags |= kProfileFixLeftTrigger;

    const std::uint32_t rtFlags = SanitizeTrigger(prof.right_trigger);
    if (rtFlags != 0)
        pflags |= kProfileFixRightTrigger;

    FixField(prof.omega_horz, kDefaultProfile.omega_horz, kOmegaMin, kOmegaMax, kProfileFixOmegaHorz, pflags);
    FixField(prof.omega_vert, kDefaultProfile.omega_vert, kOmegaMin, kOmegaMax, kProfileFixOmegaVert, pflags);
    FixField(prof.ads_sens_mult, kDefaultProfile.ads_sens_mult, kAdsSensMultMin, kAdsSensMultMax, kProfileFixAdsSensMult, pflags);
    FixField(prof.turn_accel, kDefaultProfile.turn_accel, kTurnAccelMin, kTurnAccelMax, kProfileFixTurnAccel, pflags);

    if (outReport)
    {
        outReport->left_stick_flags = lsFlags;
        outReport->right_stick_flags = rsFlags;
        outReport->ui_stick_flags = uiFlags;
        outReport->left_trigger_flags = ltFlags;
        outReport->right_trigger_flags = rtFlags;
        if (outReport->parse_warnings > 0)
            pflags |= kProfileFixParseError;
        outReport->profile_flags = pflags;
    }
    return pflags;
}

float NormalizeStickAxis(std::int16_t raw) { return Clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f); }

float NormalizeTriggerAxis(std::int16_t raw) { return Clamp01(static_cast<float>(raw) / 32767.0f); }

float ApplyCurve(const CurveConfig& c, float t)
{
    t = Clamp01(t);
    switch (c.type)
    {
    case CurveType::Power: return std::pow(t, c.gamma);
    case CurveType::SCurve:
    {
        const float a = std::pow(t, c.s_exponent);
        const float b = std::pow(1.0f - t, c.s_exponent);
        const float d = a + b; // > 0 for any t in [0,1] (one of t, 1-t is >= 0.5)
        return Clamp01(a / d);
    }
    case CurveType::TwoZone:
        if (t <= c.knee_in)
            return t * (c.knee_out / c.knee_in);
        return Clamp01(c.knee_out + (t - c.knee_in) * ((1.0f - c.knee_out) / (1.0f - c.knee_in)));
    case CurveType::Linear:
    default: return t;
    }
}

Vec2 ProcessStick(const StickConfig& cfg, float x, float y)
{
    x = SafeInput(x);
    y = SafeInput(y);
    Vec2 r{0.0f, 0.0f};

    if (cfg.shape == DeadzoneShape::Axial)
    {
        const float ax = ApplyCurve(cfg.curve, Remap(std::fabs(x), cfg.deadzone, cfg.saturation));
        const float ay = ApplyCurve(cfg.curve, Remap(std::fabs(y), cfg.deadzone, cfg.saturation));
        r.x = x < 0.0f ? -ax : ax;
        r.y = y < 0.0f ? -ay : ay;
        const float len = std::sqrt(r.x * r.x + r.y * r.y);
        if (len > 1.0f)
        {
            r.x /= len;
            r.y /= len;
        }
    }
    else
    {
        const float mag = std::sqrt(x * x + y * y); // up to sqrt(2) at the diagonal corner
        const float t = Remap(mag, cfg.deadzone, cfg.saturation);
        if (t > 0.0f) // mag > deadzone >= 0 here, so division is safe
        {
            const float k = ApplyCurve(cfg.curve, t) / mag;
            r.x = x * k;
            r.y = y * k;
        }
    }

    if (cfg.invert_x)
        r.x = -r.x;
    if (cfg.invert_y)
        r.y = -r.y;
    return r;
}

float ProcessTrigger(const TriggerConfig& cfg, float v)
{
    return Remap(Clamp01(v), cfg.deadzone, cfg.saturation);
}

bool UpdateTriggerButton(const TriggerConfig& cfg, bool was_pressed, float v)
{
    v = Clamp01(v);
    if (was_pressed)
        return !(v <= cfg.release);
    return v >= cfg.press;
}

float StepTurnAcceleration(float& inOutRamp, float processedMag, float turnAccel, float dt)
{
    using namespace limits;
    if (!IsFinite(inOutRamp))
        inOutRamp = 0.0f;
    if (!IsFinite(dt) || dt <= 0.0f)
    {
        inOutRamp = Clamp01(inOutRamp);
        const float safeAccel0 = IsFinite(turnAccel) ? Clamp(turnAccel, kTurnAccelMin, kTurnAccelMax) : 0.0f;
        return 1.0f + safeAccel0 * inOutRamp;
    }

    const float safeDt = Clamp(dt, 0.0f, 0.25f);
    const float safeMag = IsFinite(processedMag) ? Clamp01(processedMag) : 0.0f;
    const float safeAccel = IsFinite(turnAccel) ? Clamp(turnAccel, kTurnAccelMin, kTurnAccelMax) : 0.0f;

    if (safeAccel <= 1.0e-5f)
    {
        inOutRamp = 0.0f;
        return 1.0f;
    }

    if (safeMag >= kTurnAccelThreshold)
    {
        const float edgeDepth = Clamp01((safeMag - kTurnAccelThreshold) / (1.0f - kTurnAccelThreshold));
        const float rate = kTurnAccelRampRate * (0.5f + 0.5f * edgeDepth);
        inOutRamp = Clamp01(inOutRamp + rate * safeDt);
    }
    else
    {
        inOutRamp = Clamp01(inOutRamp - kTurnAccelDecayRate * safeDt);
    }

    return 1.0f + safeAccel * inOutRamp;
}

float ComputeLookSpeedScale(float adsSensMult, bool isAds, float turnAccel, float turnAccelRamp)
{
    using namespace limits;
    const float safeAds = IsFinite(adsSensMult) ? Clamp(adsSensMult, kAdsSensMultMin, kAdsSensMultMax) : kDefaultProfile.ads_sens_mult;
    const float safeAccel = IsFinite(turnAccel) ? Clamp(turnAccel, kTurnAccelMin, kTurnAccelMax) : kDefaultProfile.turn_accel;
    const float safeRamp = IsFinite(turnAccelRamp) ? Clamp01(turnAccelRamp) : 0.0f;
    const float adsK = isAds ? safeAds : 1.0f;
    const float accelK = 1.0f + safeAccel * safeRamp;
    return adsK * accelK;
}

CalibrationLoadReport ParseCalibrationProfileLtx(const char* ltxText, CalibrationProfile& outProf)
{
    outProf = CalibrationProfile{};
    CalibrationLoadReport rep{};
    if (!ltxText)
    {
        ++rep.parse_warnings;
        SanitizeProfile(outProf, &rep);
        return rep;
    }

    std::string curSection;
    const char* p = ltxText;
    while (*p)
    {
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r')
            ++lineEnd;

        std::string line(p, static_cast<std::size_t>(lineEnd - p));
        p = lineEnd;
        while (*p == '\r' || *p == '\n')
            ++p;

        // Strip comments (';' or '//')
        std::size_t commentPos = line.find(';');
        const std::size_t slashPos = line.find("//");
        if (slashPos != std::string::npos && (commentPos == std::string::npos || slashPos < commentPos))
            commentPos = slashPos;
        if (commentPos != std::string::npos)
            line.erase(commentPos);

        line = TrimAscii(line);
        if (line.empty())
            continue;

        if (line[0] == '[')
        {
            const std::size_t closeBracket = line.find(']');
            if (closeBracket == std::string::npos || closeBracket <= 1)
            {
                ++rep.parse_warnings;
                curSection.clear();
                continue;
            }
            curSection = ToLowerAscii(TrimAscii(line.substr(1, closeBracket - 1)));
            continue;
        }

        // Only parse keys in our known calibration sections; ignore foreign sections (e.g. D4.1 bindings / D5.2 haptics)
        if (!IsOurCalibrationSection(curSection))
            continue;

        const std::size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            ++rep.parse_warnings;
            continue;
        }

        const std::string key = ToLowerAscii(TrimAscii(line.substr(0, eqPos)));
        const std::string val = TrimAscii(line.substr(eqPos + 1));
        if (key.empty())
        {
            ++rep.parse_warnings;
            continue;
        }

        if (curSection == "profile")
        {
            if (key == "version")
            {
                std::uint32_t ver = 0;
                if (!TryParseStrictUint(val, ver))
                {
                    outProf.version = 0xFFFFFFFFu;
                    ++rep.parse_warnings;
                }
                else
                {
                    outProf.version = ver;
                }
            }
            else if (key == "omega_horz")
            {
                if (!TryParseStrictFloat(val, outProf.omega_horz))
                {
                    outProf.omega_horz = std::numeric_limits<float>::quiet_NaN();
                    ++rep.parse_warnings;
                }
            }
            else if (key == "omega_vert")
            {
                if (!TryParseStrictFloat(val, outProf.omega_vert))
                {
                    outProf.omega_vert = std::numeric_limits<float>::quiet_NaN();
                    ++rep.parse_warnings;
                }
            }
            else if (key == "ads_sens_mult")
            {
                if (!TryParseStrictFloat(val, outProf.ads_sens_mult))
                {
                    outProf.ads_sens_mult = std::numeric_limits<float>::quiet_NaN();
                    ++rep.parse_warnings;
                }
            }
            else if (key == "turn_accel")
            {
                if (!TryParseStrictFloat(val, outProf.turn_accel))
                {
                    outProf.turn_accel = std::numeric_limits<float>::quiet_NaN();
                    ++rep.parse_warnings;
                }
            }
            else if (key == "disable_aa_on_scope")
            {
                if (!TryParseBool(val, outProf.disable_aa_on_scope))
                {
                    outProf.disable_aa_on_scope = true;
                    ++rep.parse_warnings;
                }
            }
        }
        else if (curSection == "left_stick")
        {
            ParseStickKey(outProf.left_stick, key, val, rep, false, outProf);
        }
        else if (curSection == "right_stick")
        {
            ParseStickKey(outProf.right_stick, key, val, rep, true, outProf);
        }
        else if (curSection == "ui_stick")
        {
            ParseStickKey(outProf.ui_stick, key, val, rep, false, outProf);
        }
        else if (curSection == "left_trigger")
        {
            ParseTriggerKey(outProf.left_trigger, key, val, rep);
        }
        else if (curSection == "right_trigger")
        {
            ParseTriggerKey(outProf.right_trigger, key, val, rep);
        }
    }

    SanitizeProfile(outProf, &rep);
    rep.loaded_ok = true;
    return rep;
}

const CalibrationProfile& GetCalibrationProfile()
{
    return g_activeProfile;
}

CalibrationProfile& GetMutableCalibrationProfile()
{
    return g_activeProfile;
}

void SetCalibrationProfile(const CalibrationProfile& prof)
{
    g_activeProfile = prof;
    SanitizeProfile(g_activeProfile, nullptr);
}

void ResetCalibrationProfileToDefaults()
{
    g_activeProfile = CalibrationProfile{};
    SanitizeProfile(g_activeProfile, nullptr);
}

float GetCalibrationStickDeadzone()
{
    const float dzL = g_activeProfile.left_stick.deadzone;
    const float dzR = g_activeProfile.right_stick.deadzone;
    return dzL > dzR ? dzL : dzR;
}

const char* GetCalibrationProfilePath()
{
    ResolveDefaultProfilePath();
    return g_profilePathBuf;
}

const CalibrationLoadReport& GetLastCalibrationLoadReport()
{
    return g_lastLoadReport;
}

bool LoadCalibrationProfileFromPath(const char* fullPath, CalibrationLoadReport* outReport)
{
    CalibrationLoadReport rep{};
    if (!fullPath || !fullPath[0])
    {
        ResetCalibrationProfileToDefaults();
        if (outReport)
            *outReport = rep;
        return false;
    }

    std::FILE* fp = std::fopen(fullPath, "rb");
    if (!fp)
    {
        rep.file_existed = false;
        rep.loaded_ok = false;
        ResetCalibrationProfileToDefaults();
        g_lastLoadReport = rep;
        if (outReport)
            *outReport = rep;
        return false;
    }

    rep.file_existed = true;
    std::fseek(fp, 0, SEEK_END);
    const long fsize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    std::string content;
    if (fsize > 0 && fsize < 1024 * 1024)
    {
        content.resize(static_cast<std::size_t>(fsize));
        const std::size_t bytesRead = std::fread(&content[0], 1, content.size(), fp);
        content.resize(bytesRead);
    }
    std::fclose(fp);

    CalibrationProfile parsed{};
    rep = ParseCalibrationProfileLtx(content.c_str(), parsed);
    rep.file_existed = true;
    g_activeProfile = parsed;
    g_lastLoadReport = rep;

    if (rep.profile_flags != 0 || rep.parse_warnings != 0)
    {
        EmitLog("! [Gamepad] Calibration profile '%s' had invalid/corrupted values (profile_flags=0x%X, ls=0x%X, rs=0x%X, ui=0x%X, lt=0x%X, rt=0x%X, parse_warnings=%u); sanitized to safe bounds",
                fullPath,
                rep.profile_flags,
                rep.left_stick_flags,
                rep.right_stick_flags,
                rep.ui_stick_flags,
                rep.left_trigger_flags,
                rep.right_trigger_flags,
                rep.parse_warnings);
    }
    else
    {
        EmitLog("* [Gamepad] Calibration profile loaded from '%s' (version=%u, L_dz=%.2f, R_dz=%.2f, LT_press=%.2f, RT_press=%.2f)",
                fullPath,
                g_activeProfile.version,
                g_activeProfile.left_stick.deadzone,
                g_activeProfile.right_stick.deadzone,
                g_activeProfile.left_trigger.press,
                g_activeProfile.right_trigger.press);
    }

    if (outReport)
        *outReport = rep;
    return true;
}

bool SaveCalibrationProfileToPath(const char* fullPath, const CalibrationProfile& profIn)
{
    if (!fullPath || !fullPath[0])
        return false;

    CalibrationProfile prof = profIn;
    SanitizeProfile(prof, nullptr);

    // Preserve any non-calibration sections already present in fullPath (e.g. D4.1 bindings or D5.2 haptics)
    std::string preservedSections;
    if (std::FILE* existingFp = std::fopen(fullPath, "rb"))
    {
        std::fseek(existingFp, 0, SEEK_END);
        const long fsize = std::ftell(existingFp);
        std::fseek(existingFp, 0, SEEK_SET);
        std::string existingContent;
        if (fsize > 0 && fsize < 1024 * 1024)
        {
            existingContent.resize(static_cast<std::size_t>(fsize));
            const std::size_t rd = std::fread(&existingContent[0], 1, existingContent.size(), existingFp);
            existingContent.resize(rd);
        }
        std::fclose(existingFp);

        bool inForeignSection = false;
        const char* p = existingContent.c_str();
        while (*p)
        {
            const char* lineEnd = p;
            while (*lineEnd && *lineEnd != '\n' && *lineEnd != '\r')
                ++lineEnd;
            std::string rawLine(p, static_cast<std::size_t>(lineEnd - p));
            p = lineEnd;
            while (*p == '\r' || *p == '\n')
                ++p;

            std::string trimmed = TrimAscii(rawLine);
            if (!trimmed.empty() && trimmed[0] == '[')
            {
                const std::size_t closePos = trimmed.find(']');
                if (closePos != std::string::npos && closePos > 1)
                {
                    const std::string secName = ToLowerAscii(TrimAscii(trimmed.substr(1, closePos - 1)));
                    inForeignSection = !IsOurCalibrationSection(secName);
                    if (inForeignSection && !preservedSections.empty())
                        preservedSections += "\n";
                }
            }
            if (inForeignSection)
            {
                preservedSections += rawLine;
                preservedSections += "\n";
            }
        }
    }

    EnsureParentDirectories(fullPath);
    // FR.1 №2: пишем во временный файл, затем атомарно заменяем целевой
    const std::string tmpPath = GpTempPathFor(fullPath);
    std::FILE* fp = std::fopen(tmpPath.c_str(), "wb");
    if (!fp)
    {
        EmitLog("! [Gamepad] Failed to open calibration profile for writing: '%s'", tmpPath.c_str());
        return false;
    }

    auto writeStickSection = [&](const char* secName, const StickConfig& stk, bool writeOmega)
    {
        std::fprintf(fp, "[%s]\n", secName);
        std::fprintf(fp, "shape = %s\n", GetDeadzoneShapeName(stk.shape));
        std::fprintf(fp, "deadzone = %.4f\n", stk.deadzone);
        std::fprintf(fp, "saturation = %.4f\n", stk.saturation);
        std::fprintf(fp, "curve = %s\n", GetCurveTypeName(stk.curve.type));
        std::fprintf(fp, "gamma = %.4f\n", stk.curve.gamma);
        std::fprintf(fp, "s_exponent = %.4f\n", stk.curve.s_exponent);
        std::fprintf(fp, "knee_in = %.4f\n", stk.curve.knee_in);
        std::fprintf(fp, "knee_out = %.4f\n", stk.curve.knee_out);
        std::fprintf(fp, "invert_x = %d\n", stk.invert_x ? 1 : 0);
        std::fprintf(fp, "invert_y = %d\n", stk.invert_y ? 1 : 0);
        if (writeOmega)
        {
            std::fprintf(fp, "omega_horz = %.4f\n", prof.omega_horz);
            std::fprintf(fp, "omega_vert = %.4f\n", prof.omega_vert);
            std::fprintf(fp, "ads_sens_mult = %.4f\n", prof.ads_sens_mult);
            std::fprintf(fp, "turn_accel = %.4f\n", prof.turn_accel);
            std::fprintf(fp, "disable_aa_on_scope = %d\n", prof.disable_aa_on_scope ? 1 : 0);
        }
        std::fprintf(fp, "\n");
    };

    auto writeTriggerSection = [&](const char* secName, const TriggerConfig& trg)
    {
        std::fprintf(fp, "[%s]\n", secName);
        std::fprintf(fp, "deadzone = %.4f\n", trg.deadzone);
        std::fprintf(fp, "saturation = %.4f\n", trg.saturation);
        std::fprintf(fp, "press = %.4f\n", trg.press);
        std::fprintf(fp, "release = %.4f\n\n", trg.release);
    };

    std::fprintf(fp, "; GAMMA Gamepad Calibration Profile (D37 / D2.2)\n");
    std::fprintf(fp, "[profile]\n");
    std::fprintf(fp, "version = %u\n\n", prof.version);

    writeStickSection("left_stick", prof.left_stick, false);
    writeStickSection("right_stick", prof.right_stick, true);
    writeStickSection("ui_stick", prof.ui_stick, false);
    writeTriggerSection("left_trigger", prof.left_trigger);
    writeTriggerSection("right_trigger", prof.right_trigger);

    if (!preservedSections.empty())
    {
        std::fwrite(preservedSections.data(), 1, preservedSections.size(), fp);
    }

    const bool writeOk = (std::fflush(fp) == 0) && !std::ferror(fp);
    const bool closeOk = (std::fclose(fp) == 0);
    if (!writeOk || !closeOk)
    {
        std::remove(tmpPath.c_str());
        EmitLog("! [Gamepad] I/O error while writing calibration profile: '%s'", tmpPath.c_str());
        return false;
    }
    if (!GpCommitTempFile(tmpPath.c_str(), fullPath))
    {
        EmitLog("! [Gamepad] Failed to replace calibration profile: '%s'", fullPath);
        return false;
    }
    return true;
}

bool LoadCalibrationProfile(CalibrationLoadReport* outReport)
{
    ResolveDefaultProfilePath();
    CalibrationLoadReport rep{};
    const bool loaded = LoadCalibrationProfileFromPath(g_profilePathBuf, &rep);
    if (!loaded && !rep.file_existed)
    {
        // First startup: initialize safe defaults and write profile.ltx to disk
        ResetCalibrationProfileToDefaults();
        SaveCalibrationProfileToPath(g_profilePathBuf, g_activeProfile);
        rep.loaded_ok = true;
        g_lastLoadReport = rep;
        EmitLog("* [Gamepad] Created default calibration profile at '%s' (version=%u, L_dz=%.2f, R_dz=%.2f)",
                g_profilePathBuf,
                g_activeProfile.version,
                g_activeProfile.left_stick.deadzone,
                g_activeProfile.right_stick.deadzone);
    }
    if (outReport)
        *outReport = rep;
    return rep.loaded_ok;
}

bool SaveCalibrationProfile()
{
    ResolveDefaultProfilePath();
    SanitizeProfile(g_activeProfile, nullptr);
    const bool ok = SaveCalibrationProfileToPath(g_profilePathBuf, g_activeProfile);
    if (ok)
    {
        EmitLog("* [Gamepad] Saved calibration profile to '%s' (version=%u, L_dz=%.2f, L_sat=%.2f, L_curve=%s, R_dz=%.2f, R_sat=%.2f, R_curve=%s, LT_press=%.2f, RT_press=%.2f)",
                g_profilePathBuf,
                g_activeProfile.version,
                g_activeProfile.left_stick.deadzone,
                g_activeProfile.left_stick.saturation,
                GetCurveTypeName(g_activeProfile.left_stick.curve.type),
                g_activeProfile.right_stick.deadzone,
                g_activeProfile.right_stick.saturation,
                GetCurveTypeName(g_activeProfile.right_stick.curve.type),
                g_activeProfile.left_trigger.press,
                g_activeProfile.right_trigger.press);
    }
    return ok;
}

void LogCalibrationProfileStatus()
{
    ResolveDefaultProfilePath();
    const CalibrationProfile& p = g_activeProfile;
    EmitLog("* [Gamepad] gp_calib status: path='%s' version=%u arb_stick_dz=%.2f",
            g_profilePathBuf, p.version, GetCalibrationStickDeadzone());
    EmitLog("* [Gamepad]   left_stick : shape=%s dz=%.3f sat=%.3f curve=%s(gamma=%.2f s_exp=%.2f knee=%.2f/%.2f) inv=(%d,%d)",
            GetDeadzoneShapeName(p.left_stick.shape), p.left_stick.deadzone, p.left_stick.saturation,
            GetCurveTypeName(p.left_stick.curve.type), p.left_stick.curve.gamma, p.left_stick.curve.s_exponent,
            p.left_stick.curve.knee_in, p.left_stick.curve.knee_out,
            p.left_stick.invert_x ? 1 : 0, p.left_stick.invert_y ? 1 : 0);
    EmitLog("* [Gamepad]   right_stick: shape=%s dz=%.3f sat=%.3f curve=%s(gamma=%.2f s_exp=%.2f knee=%.2f/%.2f) inv=(%d,%d) omega=(%.2f,%.2f) ads_mult=%.2f turn_accel=%.2f disable_aa_scope=%d",
            GetDeadzoneShapeName(p.right_stick.shape), p.right_stick.deadzone, p.right_stick.saturation,
            GetCurveTypeName(p.right_stick.curve.type), p.right_stick.curve.gamma, p.right_stick.curve.s_exponent,
            p.right_stick.curve.knee_in, p.right_stick.curve.knee_out,
            p.right_stick.invert_x ? 1 : 0, p.right_stick.invert_y ? 1 : 0,
            p.omega_horz, p.omega_vert, p.ads_sens_mult, p.turn_accel, p.disable_aa_on_scope ? 1 : 0);
    EmitLog("* [Gamepad]   ui_stick   : shape=%s dz=%.3f sat=%.3f curve=%s",
            GetDeadzoneShapeName(p.ui_stick.shape), p.ui_stick.deadzone, p.ui_stick.saturation,
            GetCurveTypeName(p.ui_stick.curve.type));
    EmitLog("* [Gamepad]   triggers   : LT(dz=%.3f sat=%.3f press=%.3f rel=%.3f) RT(dz=%.3f sat=%.3f press=%.3f rel=%.3f)",
            p.left_trigger.deadzone, p.left_trigger.saturation, p.left_trigger.press, p.left_trigger.release,
            p.right_trigger.deadzone, p.right_trigger.saturation, p.right_trigger.press, p.right_trigger.release);
    EmitLog("* [Gamepad]   last_load  : existed=%d ok=%d profile_flags=0x%X parse_warnings=%u",
            g_lastLoadReport.file_existed ? 1 : 0, g_lastLoadReport.loaded_ok ? 1 : 0,
            g_lastLoadReport.profile_flags, g_lastLoadReport.parse_warnings);
}

bool RunCalibrationSelfTest()
{
    const CalibrationProfile savedProfile = g_activeProfile;
    const CalibrationLoadReport savedReport = g_lastLoadReport;

    int passed = 0;
    int total = 0;

    auto check = [&](bool cond, const char* name)
    {
        ++total;
        if (cond)
        {
            ++passed;
        }
        else
        {
            EmitLog("! [Gamepad] gp_calib selftest FAIL: %s", name);
        }
    };

    // 1. Numerical equivalence with C2.1 / C3.1 on default settings
    ResetCalibrationProfileToDefaults();
    const StickConfig& defL = g_activeProfile.left_stick;
    float maxDiffC21 = 0.0f;
    const std::int16_t samples[] = { -32768, -32767, -24000, -16384, -8000, -4915, 0, 4915, 8000, 16384, 24000, 32767 };
    for (std::int16_t sx : samples)
    {
        for (std::int16_t sy : samples)
        {
            const float rx = NormalizeStickAxis(sx);
            const float ry = -NormalizeStickAxis(sy);
            const Vec2 newOut = ProcessStick(defL, rx, ry);

            // C2.1 / C3.1 prototype formula
            Vec2 oldOut{ 0.0f, 0.0f };
            const float rawMag = std::sqrt(rx * rx + ry * ry);
            if (rawMag > 0.15f)
            {
                const float normMag = Clamp01((rawMag - 0.15f) / (0.95f - 0.15f));
                const float invRawMag = 1.0f / rawMag;
                oldOut.x = rx * invRawMag * normMag;
                oldOut.y = ry * invRawMag * normMag;
            }
            const float dx = std::fabs(newOut.x - oldOut.x);
            const float dy = std::fabs(newOut.y - oldOut.y);
            if (dx > maxDiffC21) maxDiffC21 = dx;
            if (dy > maxDiffC21) maxDiffC21 = dy;
        }
    }
    check(maxDiffC21 <= 1.0e-6f, "c21_c31_default_numerical_equivalence");

    // 2. Corrupted LTX test: negative numbers, text instead of number, threshold > 1, invalid version, broken section
    const char* corruptedLtx =
        "[profile]\n"
        "version = 999\n"
        "[broken_section_without_closing_bracket\n"
        "[left_stick]\n"
        "shape = invalid_shape\n"
        "deadzone = -0.35\n"
        "saturation = text_instead_of_number\n"
        "curve = unknown_curve\n"
        "gamma = -5.0\n"
        "s_exponent = 99.0\n"
        "knee_in = nan\n"
        "knee_out = 2.50\n"
        "invert_x = maybe\n"
        "[right_stick]\n"
        "deadzone = 1.50\n"
        "saturation = 0.20\n"
        "omega_horz = -10.0\n"
        "omega_vert = abc\n"
        "ads_sens_mult = 5.0\n"
        "turn_accel = -0.5\n"
        "disable_aa_on_scope = maybe\n"
        "[left_trigger]\n"
        "deadzone = -0.20\n"
        "saturation = 1.80\n"
        "press = 1.40\n"
        "release = 0.99\n"
        "[right_trigger]\n"
        "press = 0.20\n"
        "release = 0.80\n";

    CalibrationProfile corrProf{};
    const CalibrationLoadReport corrRep = ParseCalibrationProfileLtx(corruptedLtx, corrProf);
    check(corrRep.loaded_ok, "corrupted_ltx_no_crash_loaded_ok");
    check((corrRep.profile_flags & kProfileFixVersion) != 0 && corrProf.version == kProfileFormatVersion, "corrupted_version_reset");
    check((corrRep.profile_flags & kProfileFixParseError) != 0 && corrRep.parse_warnings >= 5, "corrupted_parse_warnings_detected");
    check((corrRep.left_stick_flags & (kFixShape | kFixDeadzone | kFixSaturation | kFixCurveType | kFixGamma | kFixSExponent | kFixKneeIn | kFixKneeOut)) != 0,
          "corrupted_left_stick_flags_recorded");
    check(corrProf.left_stick.deadzone == 0.0f && corrProf.left_stick.saturation == 0.95f, "corrupted_left_stick_clamped_safe");
    check(corrProf.right_stick.deadzone == 0.90f && corrProf.right_stick.saturation >= 0.95f, "corrupted_right_stick_gt1_clamped");
    check(corrProf.left_trigger.press == 1.0f && corrProf.left_trigger.release <= 0.95f, "corrupted_left_trigger_gt1_clamped");
    check(corrProf.right_trigger.release <= corrProf.right_trigger.press - limits::kTriggerMinGap + 1e-5f, "corrupted_right_trigger_hysteresis_gap");
    check(SanitizeProfile(corrProf, nullptr) == 0, "corrupted_profile_idempotent_after_sanitize");

    // 3. Round-trip save & load with foreign section preservation
    ResolveDefaultProfilePath();
    char tempPath[520] = {};
    std::snprintf(tempPath, sizeof(tempPath), "%s.selftest.tmp", g_profilePathBuf);

    // Pre-seed a foreign section in tempPath
    EnsureParentDirectories(tempPath);
    if (std::FILE* seedFp = std::fopen(tempPath, "wb"))
    {
        const char* seedTxt = "[bindings_test_preserve]\ncustom_key = 42\n";
        std::fwrite(seedTxt, 1, std::strlen(seedTxt), seedFp);
        std::fclose(seedFp);
    }

    CalibrationProfile customProf{};
    customProf.left_stick.shape = DeadzoneShape::Axial;
    customProf.left_stick.deadzone = 0.22f;
    customProf.left_stick.saturation = 0.88f;
    customProf.left_stick.curve.type = CurveType::SCurve;
    customProf.left_stick.curve.s_exponent = 2.4f;
    customProf.right_stick.deadzone = 0.18f;
    customProf.right_stick.curve.type = CurveType::TwoZone;
    customProf.right_stick.curve.knee_in = 0.60f;
    customProf.right_stick.curve.knee_out = 0.30f;
    customProf.right_stick.invert_y = true;
    customProf.omega_horz = 5.5f;
    customProf.omega_vert = 3.2f;
    customProf.ads_sens_mult = 0.85f;
    customProf.turn_accel = 0.45f;
    customProf.disable_aa_on_scope = false;
    customProf.left_trigger.press = 0.45f;
    customProf.left_trigger.release = 0.30f;

    const bool saveOk = SaveCalibrationProfileToPath(tempPath, customProf);
    check(saveOk, "roundtrip_save_ok");

    CalibrationLoadReport rtRep{};
    const bool loadOk = LoadCalibrationProfileFromPath(tempPath, &rtRep);
    check(loadOk && rtRep.profile_flags == 0 && rtRep.parse_warnings == 0, "roundtrip_load_clean");
    check(g_activeProfile.left_stick.shape == DeadzoneShape::Axial &&
          std::fabs(g_activeProfile.left_stick.deadzone - 0.22f) < 1e-4f &&
          g_activeProfile.left_stick.curve.type == CurveType::SCurve &&
          std::fabs(g_activeProfile.left_stick.curve.s_exponent - 2.4f) < 1e-4f &&
          g_activeProfile.right_stick.invert_y == true &&
          std::fabs(g_activeProfile.omega_horz - 5.5f) < 1e-4f &&
          std::fabs(g_activeProfile.ads_sens_mult - 0.85f) < 1e-4f &&
          std::fabs(g_activeProfile.turn_accel - 0.45f) < 1e-4f &&
          g_activeProfile.disable_aa_on_scope == false &&
          std::fabs(g_activeProfile.left_trigger.press - 0.45f) < 1e-4f,
          "roundtrip_fields_match");

    // K6b: StepTurnAcceleration & ComputeLookSpeedScale checks
    float ramp = 0.0f;
    const float m0 = StepTurnAcceleration(ramp, 0.50f, 0.50f, 0.10f);
    check(std::fabs(m0 - 1.0f) < 1e-5f && ramp == 0.0f, "k6b_turn_accel_below_threshold_zero");
    for (int i = 0; i < 10; ++i)
        StepTurnAcceleration(ramp, 1.0f, 0.50f, 0.05f);
    const float mEdge = ComputeLookSpeedScale(0.70f, false, 0.50f, ramp);
    const float mAds = ComputeLookSpeedScale(0.70f, true, 0.50f, ramp);
    check(ramp >= 0.99f && std::fabs(mEdge - 1.50f) < 1e-4f && std::fabs(mAds - 1.05f) < 1e-4f, "k6b_turn_accel_and_ads_scale");

    // D1.2-N5 check: GetCalibrationStickDeadzone() == max(0.22, 0.18) == 0.22
    check(std::fabs(GetCalibrationStickDeadzone() - 0.22f) < 1e-4f, "d1_2_n5_arb_deadzone_from_profile");

    // Verify foreign section was preserved
    bool foreignPreserved = false;
    if (std::FILE* checkFp = std::fopen(tempPath, "rb"))
    {
        char buf[2048] = {};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, checkFp);
        buf[n] = '\0';
        std::fclose(checkFp);
        foreignPreserved = (std::strstr(buf, "[bindings_test_preserve]") != nullptr &&
                            std::strstr(buf, "custom_key = 42") != nullptr);
    }
    check(foreignPreserved, "foreign_section_preserved_on_save");
    std::remove(tempPath);

    // Restore active profile & report
    g_activeProfile = savedProfile;
    g_lastLoadReport = savedReport;

    EmitLog("* [Gamepad] gp_calib selftest: %s (%d/%d passed, max_diff_c21=%.2e)",
            (passed == total) ? "PASS" : "FAIL", passed, total, maxDiffC21);
    return passed == total;
}

RadialSectorResult SelectRadialSector(const RadialSectorConfig& cfg,
                                      float rx,
                                      float ry,
                                      int sector_count,
                                      int prev_sector,
                                      bool was_stick_active)
{
    RadialSectorResult out{};
    if (sector_count <= 0 || !std::isfinite(rx) || !std::isfinite(ry))
    {
        out.selected_sector = -1;
        out.stick_active = false;
        return out;
    }

    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kPi = 3.14159265358979323846f;

    const float enterTh = Clamp(cfg.enter_threshold, 0.05f, 0.95f);
    const float exitTh = Clamp(cfg.exit_threshold, 0.02f, enterTh);
    const float mag = Clamp01(std::sqrt(rx * rx + ry * ry));
    out.magnitude = mag;

    const float threshold = was_stick_active ? exitTh : enterTh;
    if (mag < threshold)
    {
        // Central deadzone: stick is inactive, preserve previously selected sector
        out.stick_active = false;
        out.selected_sector = (prev_sector >= 0 && prev_sector < sector_count) ? prev_sector : -1;
        if (out.selected_sector >= 0)
        {
            out.angle_rad = static_cast<float>(out.selected_sector) * (kTwoPi / static_cast<float>(sector_count));
        }
        return out;
    }

    out.stick_active = true;

    // Clockwise angle from 12 o'clock UP (+Y): atan2(rx, ry) wrapped to [0, 2*PI)
    float angle = std::atan2(rx, ry);
    if (angle < 0.0f)
        angle += kTwoPi;
    if (angle >= kTwoPi)
        angle -= kTwoPi;
    out.angle_rad = angle;

    if (sector_count == 1)
    {
        out.selected_sector = 0;
        return out;
    }

    const float step = kTwoPi / static_cast<float>(sector_count);
    const float halfStep = step * 0.5f;
    int candidate = static_cast<int>(std::floor((angle + halfStep) / step)) % sector_count;
    if (candidate < 0)
        candidate += sector_count;

    out.selected_sector = candidate;
    if (prev_sector >= 0 && prev_sector < sector_count && candidate != prev_sector)
    {
        const float prevCenter = static_cast<float>(prev_sector) * step;
        float diff = std::fmod(angle - prevCenter, kTwoPi);
        if (diff > kPi)
            diff -= kTwoPi;
        else if (diff < -kPi)
            diff += kTwoPi;
        const float absDiff = std::fabs(diff);
        const float maxHyst = halfStep * 0.45f;
        const float hyst = (cfg.angular_hysteresis_rad < maxHyst) ? std::max(0.0f, cfg.angular_hysteresis_rad) : maxHyst;
        if (absDiff <= (halfStep + hyst))
        {
            out.selected_sector = prev_sector;
        }
    }

    return out;
}
} // namespace gpmath
