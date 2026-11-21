// D2.1: pure gamepad stick/trigger math. No engine/game dependencies (only <cstdint>).
// Formulas and defaults: docs/kb/05 §10 (project repo), evidence d2-1-math-*.json.
// Defaults reproduce C2.1 (Actor_Movement.cpp:28-29) and C3.1 (ActorInput.cpp:484-485):
// radial deadzone 0.15, saturation 0.95, linear curve.
#pragma once

#include <cstdint>

#ifndef ENGINE_API
#ifdef ENGINE_BUILD
#define ENGINE_API __declspec(dllexport)
#else
#define ENGINE_API
#endif
#endif

namespace gpmath
{
using LogCallbackFn = void (*)(const char* msg);
constexpr std::uint32_t kProfileFormatVersion = 1;

struct Vec2
{
    float x;
    float y;
};

enum class DeadzoneShape : std::uint8_t
{
    Radial = 0, // by vector length; keeps direction (default, as C2.1/C3.1)
    Axial = 1,  // per axis; snaps to cardinal directions near the axes
};

enum class CurveType : std::uint8_t
{
    Linear = 0,  // f(t) = t
    Power = 1,   // f(t) = t^gamma
    SCurve = 2,  // f(t) = t^a / (t^a + (1-t)^a)
    TwoZone = 3, // piecewise linear through (knee_in, knee_out)
};

struct CurveConfig
{
    CurveType type = CurveType::Linear;
    float gamma = 2.0f;      // Power
    float s_exponent = 2.0f; // SCurve
    float knee_in = 0.5f;    // TwoZone
    float knee_out = 0.25f;  // TwoZone
};

struct StickConfig
{
    DeadzoneShape shape = DeadzoneShape::Radial;
    float deadzone = 0.15f;   // inner: |v| <= deadzone -> 0
    float saturation = 0.95f; // outer: |v| >= saturation -> 1
    CurveConfig curve;
    bool invert_x = false;
    bool invert_y = false;
};

struct TriggerConfig
{
    float deadzone = 0.12f;   // analog: v <= deadzone -> 0 (~XInput 30/255), proposal
    float saturation = 0.95f; // analog: v >= saturation -> 1
    float press = 0.35f;      // digital: up -> down when value >= press, proposal
    float release = 0.25f;    // digital: down -> up when value <= release (< press)
};

// Safe ranges (sanitizer clamps into them; NaN/Inf -> default value).
namespace limits
{
constexpr float kDeadzoneMin = 0.0f;
constexpr float kDeadzoneMax = 0.90f;
constexpr float kMinSpan = 0.05f; // saturation - deadzone >= kMinSpan -> no division by zero
constexpr float kSaturationMax = 1.0f;
constexpr float kExpMin = 0.5f; // gamma and s_exponent
constexpr float kExpMax = 4.0f;
constexpr float kKneeMin = 0.05f;
constexpr float kKneeMax = 0.95f;
constexpr float kTriggerMinGap = 0.05f; // press - release
constexpr float kOmegaMin = 0.5f;       // rad/s look angular speed min
constexpr float kOmegaMax = 15.0f;      // rad/s look angular speed max
constexpr float kAdsSensMultMin = 0.10f; // K6b: ADS look sensitivity multiplier min (10%)
constexpr float kAdsSensMultMax = 1.50f; // K6b: ADS look sensitivity multiplier max (150%)
constexpr float kTurnAccelMin = 0.00f;   // K6b: edge turn acceleration min (0%)
constexpr float kTurnAccelMax = 1.00f;   // K6b: edge turn acceleration max (100%)
constexpr float kTurnAccelThreshold = 0.90f; // K6b: processed stick magnitude where edge turn acceleration begins
constexpr float kTurnAccelRampRate = 2.50f;  // K6b: 1/s ramp-up rate (~0.40s to full boost at edge)
constexpr float kTurnAccelDecayRate = 10.0f; // K6b: 1/s decay rate when leaving edge
} // namespace limits

// Sanitizer result bits (which fields were corrected). 0 = config was valid.
enum SanitizeFlags : std::uint32_t
{
    kFixDeadzone = 1u << 0,
    kFixSaturation = 1u << 1,
    kFixGamma = 1u << 2,
    kFixSExponent = 1u << 3,
    kFixKneeIn = 1u << 4,
    kFixKneeOut = 1u << 5,
    kFixShape = 1u << 6,
    kFixCurveType = 1u << 7,
    kFixPress = 1u << 8,
    kFixRelease = 1u << 9,
};

// D2.2: Profile-level sanitizer result bits (D37: $app_data_root$\gamepad\profile.ltx)
enum ProfileSanitizeFlags : std::uint32_t
{
    kProfileFixVersion = 1u << 0,
    kProfileFixLeftStick = 1u << 1,
    kProfileFixRightStick = 1u << 2,
    kProfileFixUiStick = 1u << 3,
    kProfileFixLeftTrigger = 1u << 4,
    kProfileFixRightTrigger = 1u << 5,
    kProfileFixOmegaHorz = 1u << 6,
    kProfileFixOmegaVert = 1u << 7,
    kProfileFixParseError = 1u << 8,
    kProfileFixAdsSensMult = 1u << 9,
    kProfileFixTurnAccel = 1u << 10,
};

struct CalibrationProfile
{
    std::uint32_t version = kProfileFormatVersion;
    StickConfig left_stick;
    StickConfig right_stick;
    StickConfig ui_stick;
    TriggerConfig left_trigger;
    TriggerConfig right_trigger;
    float omega_horz = 4.0f;
    float omega_vert = 2.5f;
    float ads_sens_mult = 0.70f;     // K6b: ADS sensitivity multiplier (0.10..1.50, default 70%)
    float turn_accel = 0.30f;        // K6b: edge turn acceleration boost (0.00..1.00, default 30%)
    bool disable_aa_on_scope = true; // K6b: disable aim assist on >4x scopes and 3DSS (default true)
};

struct CalibrationLoadReport
{
    bool file_existed = false;
    bool loaded_ok = false;
    std::uint32_t profile_flags = 0;
    std::uint32_t left_stick_flags = 0;
    std::uint32_t right_stick_flags = 0;
    std::uint32_t ui_stick_flags = 0;
    std::uint32_t left_trigger_flags = 0;
    std::uint32_t right_trigger_flags = 0;
    std::uint32_t parse_warnings = 0;
};

ENGINE_API std::uint32_t SanitizeStick(StickConfig& cfg);
ENGINE_API std::uint32_t SanitizeTrigger(TriggerConfig& cfg);
ENGINE_API std::uint32_t SanitizeProfile(CalibrationProfile& prof, CalibrationLoadReport* outReport = nullptr);

// int16 SDL axis -> [-1, 1] (-32768 clamps to -1).
ENGINE_API float NormalizeStickAxis(std::int16_t raw);
// SDL trigger 0..32767 -> [0, 1] (negative clamps to 0).
ENGINE_API float NormalizeTriggerAxis(std::int16_t raw);

// Curve on t in [0,1] -> [0,1]; f(0)=0, f(1)=1, continuous, non-decreasing.
// Expects a sanitized CurveConfig; t outside [0,1] or NaN is clamped (NaN -> 0).
ENGINE_API float ApplyCurve(const CurveConfig& c, float t);

// Full stick pipeline: NaN/Inf input -> 0, deadzone, saturation, curve, inversion.
// Output length <= 1. Expects a sanitized config.
ENGINE_API Vec2 ProcessStick(const StickConfig& cfg, float x, float y);

// Analog trigger value in [0,1] after deadzone/saturation (linear).
ENGINE_API float ProcessTrigger(const TriggerConfig& cfg, float v);

// Digital trigger with hysteresis. Returns new pressed state.
ENGINE_API bool UpdateTriggerButton(const TriggerConfig& cfg, bool was_pressed, float v);

// K6b: Edge turn acceleration ramp & multiplier (1.0 .. 1.0 + turn_accel) when processed stick magnitude >= 0.90.
ENGINE_API float StepTurnAcceleration(float& inOutRamp, float processedMag, float turnAccel, float dt);
ENGINE_API float ComputeLookSpeedScale(float adsSensMult, bool isAds, float turnAccel, float turnAccelRamp);

// D2.2: String helpers for UI and LTX serialization
ENGINE_API const char* GetDeadzoneShapeName(DeadzoneShape s);
ENGINE_API const char* GetCurveTypeName(CurveType c);

// D2.2: Fault-tolerant in-memory LTX parser & active calibration profile management (D37)
ENGINE_API CalibrationLoadReport ParseCalibrationProfileLtx(const char* ltxText, CalibrationProfile& outProf);

ENGINE_API const CalibrationProfile& GetCalibrationProfile();
ENGINE_API CalibrationProfile& GetMutableCalibrationProfile();
ENGINE_API void SetCalibrationProfile(const CalibrationProfile& prof);
ENGINE_API void ResetCalibrationProfileToDefaults();

// D1.2-N5: active stick deadzone from calibration profile = max(left_stick.deadzone, right_stick.deadzone)
ENGINE_API float GetCalibrationStickDeadzone();

ENGINE_API bool LoadCalibrationProfileFromPath(const char* fullPath, CalibrationLoadReport* outReport = nullptr);
ENGINE_API bool SaveCalibrationProfileToPath(const char* fullPath, const CalibrationProfile& prof);

ENGINE_API bool LoadCalibrationProfile(CalibrationLoadReport* outReport = nullptr);
ENGINE_API bool SaveCalibrationProfile();
ENGINE_API void SetCalibrationProfilePath(const char* fullPath);
ENGINE_API const char* GetCalibrationProfilePath();
ENGINE_API void SetCalibrationLogCallback(LogCallbackFn cb);
ENGINE_API const CalibrationLoadReport& GetLastCalibrationLoadReport();
ENGINE_API void LogCalibrationProfileStatus();
ENGINE_API bool RunCalibrationSelfTest();

// E5.1: Radial wheel sector selection with central deadzone (enter/exit hysteresis) and angular hysteresis
struct RadialSectorConfig
{
    float enter_threshold = 0.35f;        // r >= enter_threshold activates stick selection from neutral
    float exit_threshold = 0.25f;         // r < exit_threshold returns stick to neutral (preserving last sector)
    float angular_hysteresis_rad = 0.12f; // ~6.9 deg half-band around sector boundaries
};

struct RadialSectorResult
{
    int selected_sector = -1;  // 0 .. sector_count-1, or -1 if none
    bool stick_active = false; // true if stick is outside central deadzone
    float magnitude = 0.0f;    // [0..1] radial magnitude
    float angle_rad = 0.0f;    // clockwise angle from 12 o'clock UP (+Y) in [0, 2*PI)
};

ENGINE_API RadialSectorResult SelectRadialSector(const RadialSectorConfig& cfg,
                                                 float rx,
                                                 float ry,
                                                 int sector_count,
                                                 int prev_sector,
                                                 bool was_stick_active);
} // namespace gpmath
