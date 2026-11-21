#pragma once

// D5.1: Gamepad Button Glyphs Manager (D09, D37, D38)
// - Auto-detection of glyph set & face button layout via SDL_GetGamepadType +
//   SDL_GetGamepadButtonLabel / SDL_GetGamepadButtonLabelForType (R4 glyph_logic):
//     * Xbox (ABXY, LB/RB, LT/RT, View/Menu)
//     * PlayStation (Cross/Circle/Square/Triangle, L1/R1, L2/R2, Share/Options)
//     * Nintendo Switch (BAYX: South=B, East=A, West=Y, North=X, L/R, ZL/ZR, -/+)
//     * GameCube face override (AXBY: South=A, East=X, West=B, North=Y)
//     * Generic / Class B (B1/B2/B3/B4, L1/R1, L2/R2, Back/Start)
// - Manual style override ("auto" | "xbox" | "playstation" | "nintendo" | "generic")
//   persisted in $app_data_root$\gamepad\profile.ltx under [glyphs] (D37) without
//   touching user.ltx or axr_options.ltx and preserving all sibling sections.
// - Dynamic prompt formatting for $$ACTION_<name>$$ (CStringTable::ParseLine /
//   GetActionAllBinding) from active D4.1 bindings (gpbind::GetActiveBindingProfile),
//   plus texture icon ID lookup (ui_gp_<set>_<btn> in ui_gamepad_glyphs.xml/.dds).
// - Monotonic prompt revision counter & callback so HUD/Menu prompts re-parse and
//   refresh immediately on KBM <-> Gamepad switch, device hotplug, style override,
//   or binding remap without restarting the game.

#include "GamepadBindings.h"

struct SGamepadDeviceEntry;

enum class EGamepadGlyphSet : u8
{
    Auto        = 0,
    Xbox        = 1,
    PlayStation = 2,
    Nintendo    = 3,
    Generic     = 4
};

enum class EGamepadFaceStyle : u8
{
    ABXY     = 0, // South=A, East=B, West=X, North=Y (Xbox / Steam)
    Sony     = 1, // South=Cross, East=Circle, West=Square, North=Triangle (PS3/PS4/PS5)
    BAYX     = 2, // South=B, East=A, West=Y, North=X (Nintendo Switch)
    AXBY     = 3, // South=A, East=X, West=B, North=Y (GameCube)
    Numbered = 4  // South=B1, East=B2, West=B3, North=B4 (Generic Class B)
};

typedef void (*FGamepadGlyphsDirtyCallback)(u32 newRevision, const char* reason);
typedef const char* (*FGamepadGlyphTranslateCallback)(const char* stringId, const char* fallbackCp1251);

class ENGINE_API CGamepadGlyphManager
{
public:
    static CGamepadGlyphManager& Instance();

    CGamepadGlyphManager();
    ~CGamepadGlyphManager() = default;

    // Lifecycle & profile persistence ($app_data_root$\gamepad\profile.ltx, [glyphs] section, D37)
    void Initialize();
    void EnsureInitialized();
    bool LoadFromProfileLtx(const char* explicitPath = nullptr);
    bool SaveToProfileLtx(const char* explicitPath = nullptr) const;

    // Style override ("auto", "xbox", "playstation", "nintendo", "generic")
    EGamepadGlyphSet GetStyleOverride() const { return m_styleOverride; }
    bool SetStyleOverride(EGamepadGlyphSet style, bool saveToDisk = true);
    bool SetStyleOverrideByName(const char* styleName, bool saveToDisk = true);

    enum class EGamepadIconStyle : u8
    {
        White = 0,
        Dark  = 1,
        Color = 2
    };

    enum class EGamepadHintMode : u8
    {
        Always = 0,
        Menu   = 1,
        Off    = 2
    };

    EGamepadIconStyle GetIconStyle() const { return m_iconStyle; }
    bool SetIconStyle(EGamepadIconStyle style, bool saveToDisk = true);
    bool SetIconStyleByName(const char* styleName, bool saveToDisk = true);

    int GetIconScale() const { return m_iconScale; }
    float GetIconScaleFactor() const { return static_cast<float>(m_iconScale) * 0.01f; }
    bool SetIconScale(int percent, bool saveToDisk = true);

    EGamepadHintMode GetHintMode() const { return m_hintMode; }
    bool SetHintMode(EGamepadHintMode mode, bool saveToDisk = true);
    bool SetHintModeByName(const char* modeName, bool saveToDisk = true);
    bool AreHintsVisible(bool inMenu = false) const;

    // Effective resolved set & face style (from manual override or active SDL3 device)
    EGamepadGlyphSet GetEffectiveGlyphSet() const { return m_effectiveSet; }
    EGamepadFaceStyle GetEffectiveFaceStyle() const { return m_effectiveFaceStyle; }

    // Refresh effective glyph set from active device in CGamepadDeviceManager;
    // bumps revision if effective set or face style changed.
    bool RefreshFromActiveDevice(const char* reason = "device_check");

    // Notify manager that active input source (KBM <-> Gamepad) or bindings changed;
    // increments revision and fires dirty callback so CStringTable::ReparseKeyBindings() runs.
    void NotifyPromptsDirty(const char* reason);

    u32 GetPromptRevision() const { return m_promptRevision; }
    const char* GetLastDirtyReason() const { return m_lastDirtyReason; }

    void SetDirtyCallback(FGamepadGlyphsDirtyCallback cb) { m_dirtyCallback = cb; }
    static void SetTranslateCallback(FGamepadGlyphTranslateCallback cb);

    // Pure deterministic resolver for any SDL_GamepadType + deviceClass + SDL_GamepadButtonLabel
    // (used both at runtime and in gp_glyphs selftest)
    static void ResolveForSDLTypeAndLabels(int sdlGamepadType,
                                           int deviceClass,
                                           int lblSouth,
                                           int lblEast,
                                           int lblWest,
                                           int lblNorth,
                                           EGamepadGlyphSet styleOverride,
                                           EGamepadGlyphSet& outEffectiveSet,
                                           EGamepadFaceStyle& outFaceStyle);

    // String names for enums
    static const char* GlyphSetToString(EGamepadGlyphSet set);
    static EGamepadGlyphSet StringToGlyphSet(const char* str, bool* outValid = nullptr);
    static const char* FaceStyleToString(EGamepadFaceStyle face);
    static const char* IconStyleToString(EGamepadIconStyle iconStyle);
    static EGamepadIconStyle StringToIconStyle(const char* str, bool* outValid = nullptr);
    static const char* HintModeToString(EGamepadHintMode mode);
    static EGamepadHintMode StringToHintMode(const char* str, bool* outValid = nullptr);

    // Button text label & texture icon ID (from ui_gp_dm.xml)
    static const char* GetButtonLabelForStyle(gpbind::ButtonId btn,
                                              EGamepadGlyphSet effectiveSet,
                                              EGamepadFaceStyle faceStyle,
                                              bool isRussian,
                                              bool compactBadge = false);

    static const char* GetButtonTextureIdForStyle(gpbind::ButtonId btn,
                                                  EGamepadGlyphSet effectiveSet,
                                                  EGamepadFaceStyle faceStyle,
                                                  EGamepadIconStyle iconStyle = EGamepadIconStyle::White,
                                                  bool keyShape = false);

    const char* GetButtonLabel(gpbind::ButtonId btn, bool isRussian = false, bool compactBadge = false) const;
    const char* GetButtonTextureId(gpbind::ButtonId btn, bool keyShape = false) const;

    // Trigger formatting (Button / Hold / Chord) using active or specified glyph style
    static void FormatTriggerPromptForStyle(const gpbind::BindingTrigger& trig,
                                            EGamepadGlyphSet effectiveSet,
                                            EGamepadFaceStyle faceStyle,
                                            bool isRussian,
                                            bool compactBadge,
                                            char* dstBuff,
                                            int dstBuffSize);

    void FormatTriggerPrompt(const gpbind::BindingTrigger& trig,
                             bool isRussian,
                             bool compactBadge,
                             char* dstBuff,
                             int dstBuffSize) const;

    // Maps engine action name (from $$ACTION_<name>$$ / actions[] in xr_level_controller.cpp,
    // e.g. "use", "USE", "sprint_toggle", "QUICK_USE_1") to active D4.1 binding prompt.
    // Returns true if mapped and written to dstBuff.
    bool FormatEngineActionPrompt(const char* engineActionName,
                                  bool isRussian,
                                  char* dstBuff,
                                  int dstBuffSize) const;

    // Returns texture icon ID (e.g. "ui_gp_white_xbox_west") for the primary button of an engine action or gpbind action
    const char* GetEngineActionTextureId(const char* engineActionName) const;

    // Maps engine action name (case-insensitive) to gpbind::ContextId + gpbind::ActionId
    static bool MapEngineActionToGamepadBinding(const char* engineActionName,
                                                gpbind::ContextId& outCtx,
                                                gpbind::ActionId& outAct,
                                                gpbind::ButtonId* outDirectButtonFallback = nullptr);

    // Console status & self-test
    void LogStatus() const;
    bool RunSelfTest() const;

    static void ResolveProfileLtxPath(string_path& outPath);

private:
    bool                        m_initialized        = false;
    bool                        m_cmdRegistered      = false;
    EGamepadGlyphSet            m_styleOverride      = EGamepadGlyphSet::Auto;
    EGamepadGlyphSet            m_effectiveSet       = EGamepadGlyphSet::Xbox;
    EGamepadFaceStyle           m_effectiveFaceStyle = EGamepadFaceStyle::ABXY;
    EGamepadIconStyle           m_iconStyle          = EGamepadIconStyle::White;
    int                         m_iconScale          = 100;
    EGamepadHintMode            m_hintMode           = EGamepadHintMode::Always;
    u32                         m_promptRevision     = 1;
    u32                         m_lastBindingsHash   = 0;
    string64                    m_lastDirtyReason    = {};
    FGamepadGlyphsDirtyCallback m_dirtyCallback      = nullptr;

    u32 ComputeActiveBindingsSignature() const;
};
