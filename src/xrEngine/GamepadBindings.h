// D4.1: Gamepad layout & binding model (Single / Hold / Chord), per-context conflict detector,
// MenuUI navigation protection (Menu/Confirm/Back), versioned profile storage (D37: appdata\gamepad\profile.ltx),
// v1 -> v2 schema migration, export/import to a separate file, and emergency reset (Start+Back 5s or gp_bind_reset).
// Pure C++17 module (no engine/game header dependencies) so unit tests run standalone like D2.1 (GamepadMath).
// NOTE (D4.2/D5.1): when added to xrEngine/xrGame.vcxproj, either set "Not Using Precompiled Headers" or include stdafx.h first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef ENGINE_API
#ifdef ENGINE_BUILD
#define ENGINE_API __declspec(dllexport)
#else
#define ENGINE_API
#endif
#endif

namespace gpbind
{

// 17 virtual gamepad buttons matching CInput::EGamepadButtonBit (0..14) and CGamepadContextDispatcher::EExtendedButtonBit (15..16)
// (src/xrEngine/xr_input.h:51-68, src/xrGame/GamepadContextDispatcher.h:34-40)
enum class ButtonId : std::uint8_t
{
    South = 0,         // A / Cross (GP_BTN_SOUTH)
    East = 1,          // B / Circle (GP_BTN_EAST)
    West = 2,          // X / Square (GP_BTN_WEST)
    North = 3,         // Y / Triangle (GP_BTN_NORTH)
    Back = 4,          // View / Share / Select (GP_BTN_BACK)
    Guide = 5,         // Xbox / PS Guide (GP_BTN_GUIDE)
    Start = 6,         // Menu / Options / Start (GP_BTN_START)
    LeftStick = 7,     // L3 (GP_BTN_LEFT_STICK)
    RightStick = 8,    // R3 (GP_BTN_RIGHT_STICK)
    LeftShoulder = 9,  // LB / L1 (GP_BTN_LEFT_SHOULDER)
    RightShoulder = 10,// RB / R1 (GP_BTN_RIGHT_SHOULDER)
    DPadUp = 11,       // GP_BTN_DPAD_UP
    DPadDown = 12,     // GP_BTN_DPAD_DOWN
    DPadLeft = 13,     // GP_BTN_DPAD_LEFT
    DPadRight = 14,    // GP_BTN_DPAD_RIGHT
    LT = 15,           // VBTN_LT (digital trigger)
    RT = 16,           // VBTN_RT (digital trigger)

    Count = 17,
    None = 0xFF
};

// 12 contexts matching EGamepadContext (src/xrGame/GamepadContextDispatcher.h:12-28)
enum class ContextId : std::uint8_t
{
    Console = 0,                  // Priority 1
    TextEntry = 1,                // Priority 2
    MainMenu = 2,                 // Priority 3
    Cutscene_InputBlock = 3,      // Priority 4
    Sleep = 4,                    // Priority 5
    Dialog_NPC = 5,               // Priority 6
    RadialWheel = 6,              // Priority 7
    Placement_HideoutFurniture = 7,// Priority 8
    World_3DPDA = 8,              // Priority 9
    MenuUI = 9,                   // Priority 10 (MenuUI_Modal in dispatcher)
    World_VehicleOrHolder = 10,   // Priority 11
    World_Gameplay = 11,          // Priority 12

    Count = 12,
    Invalid = 0xFF
};

// Semantic actions across contexts (MenuUI navigation + World_Gameplay + overlays)
enum class ActionId : std::uint16_t
{
    // Protected navigation actions in MenuUI / MainMenu (D4.1 item 3, T05)
    Menu = 0,          // Open/close pause or main menu (Start)
    Confirm = 1,       // Confirm / Activate focused item (A)
    Back = 2,          // Cancel / Close modal / Back (B)

    // Additional MenuUI actions
    ContextMenu = 3,   // Open item context menu / properties (X)
    QuickAction = 4,   // Secondary / transfer / sort (Y)
    TabPrev = 5,       // Previous category / tab (LB)
    TabNext = 6,       // Next category / tab (RB)
    PagePrev = 7,      // Previous sub-tab / page (LT)
    PageNext = 8,      // Next sub-tab / page (RT)

    // World_Gameplay actions (Stage E / docs/11-action-and-screen-registry.md)
    Jump = 9,          // kJUMP (A)
    Crouch = 10,       // kCROUCH toggle (B)
    LowCrouch = 11,    // Low crouch explicit gesture (Hold B)
    Sprint = 12,       // kSPRINT_TOGGLE (L3)
    Freelook = 13,     // kFREELOOK (Hold R3)
    MeleeQuick = 14,   // kCUSTOM24 Quick Melee (R3)
    Fire = 15,         // kWPN_FIRE (RT)
    Aim = 16,          // kWPN_ZOOM (LT)
    ContextUseReload = 17, // Contextual X (Interact / Reload / Unjam)
    AmmoCheck = 18,    // Check ammo (Hold X)
    FastReload = 19,   // Tactical fast reload (Chord LT + X)
    NextWeapon = 20,   // Swap weapon / next slot (Y)
    WeaponWheel = 21,  // Quick Action Wheel (Hold LB or RB)
    GrenadeQuick = 22, // kCUSTOM19 Quick grenade (LB)
    LeanLeft = 23,     // kL_LOOKOUT in ADS (Chord LT + LB)
    LeanRight = 24,    // kR_LOOKOUT in ADS (Chord LT + RB)
    TorchToggle = 25,  // kTORCH (DPad Up)
    NightVision = 26,  // kNIGHT_VISION (Hold DPad Up)
    DetectorToggle = 27,// kDETECTOR (DPad Left)
    Pda3DToggle = 28,  // kCUSTOM20 3D PDA (DPad Right)
    MaskClean = 29,    // kCAM_ZOOM_IN Wipe mask (DPad Down)
    UnderbarrelToggle = 30, // kWPN_FUNC / kCUSTOM21 UBGL (Hold DPad Down)
    FireModeNext = 31, // kWPN_FIREMODE_NEXT (Hold DPad Left)
    AmmoTypeNext = 32, // kWPN_NEXT / Ammo wheel (Hold DPad Right)
    PauseMenu = 33,    // kQUIT / Pause menu in world (Start)
    PdaFullToggle = 34,// Full PDA / Map (Back)

    // Dialog_NPC / RadialWheel / World_3DPDA / Placement_HideoutFurniture actions
    DialogSelect = 35,
    DialogExit = 36,
    WheelConfirm = 37,
    WheelCancel = 38,
    WheelTabPrev = 39,
    WheelTabNext = 40,
    PdaInteractiveToggle = 41,
    PdaClose = 42,
    PlacementConfirm = 43,
    PlacementCancel = 44,
    PlacementRotate = 45,
    QuickBoltOrSlot = 46, // kWPN_6 / kQUICK_USE_1..4 (Tap LB)
    SwitchPanel = 47,     // Inventory / Trade / Loot panel switch (View / Back in MenuUI)
    CancelQueue = 48,     // Cancel paused action queue (L3 in MenuUI)
    // GAYS v0.2 (2026-11-13)
    KnifeDraw = 49,       // kWPN_1 draw knife (Hold RB)
    WeaponLower = 50,     // kSAFEMODE lower / raise weapon (R3)
    PdaFlat = 51,         // kACTIVE_JOBS flat PDA (Hold View)
    AmmoWheel = 52,       // Ammo type wheel (Hold DPad Down)
    BoltThrow = 53,       // kWPN_6 bolt (Hold DPad Left)
    WeaponSelect = 54,    // Firearm triangle selector (Hold Y)
    ForceReload = 55,     // Reload / unjam ignoring use targets (Chord LT + X)
    TutorialPrompt = 56,  // Timed tutorial prompt key (Chord LB + View)
    // CURSOR1 (2026-11-14)
    MouseCursor = 57,     // Toggle virtual mouse cursor mode in unsupported modals (R3 in MenuUI)
    // FIX-LIVE2 item 15 (2026-11-16): World_Gameplay chords, also active inside the open LB wheel
    WheelMaskClean = 58,  // Wipe mask from any wheel tab (Chord LB + R3)
    WheelLaser = 59,      // Toggle weapon laser from any wheel tab (Chord LB + DPad Right)

    Count = 60,
    Invalid = 0xFFFF
};

// Gesture trigger type (D4.1 item 1)
enum class TriggerType : std::uint8_t
{
    Button = 0, // Single button press (Tap / Press)
    Hold = 1,   // Button held for >= hold_threshold_ms
    Chord = 2   // Modifier button held + primary button pressed
};

namespace limits
{
constexpr std::uint16_t kDefaultHoldMs = 300;
constexpr std::uint16_t kMinHoldMs = 100;
constexpr std::uint16_t kMaxHoldMs = 2000;
constexpr std::uint32_t kEmergencyResetHoldMs = 5000; // Start + Back 5s in any context (D4.1 item 5)
constexpr std::uint32_t kProfileSchemaV1 = 1;
constexpr std::uint32_t kProfileSchemaV2 = 2;
constexpr std::uint32_t kCurrentProfileSchema = kProfileSchemaV2;
} // namespace limits

struct BindingTrigger
{
    TriggerType type = TriggerType::Button;
    ButtonId button = ButtonId::None;
    ButtonId modifier = ButtonId::None; // Used only when type == TriggerType::Chord
    std::uint16_t hold_ms = 0;          // Used only when type == TriggerType::Hold (0 for Button/Chord)

    static BindingTrigger MakeButton(ButtonId btn)
    {
        BindingTrigger t;
        t.type = TriggerType::Button;
        t.button = btn;
        t.modifier = ButtonId::None;
        t.hold_ms = 0;
        return t;
    }

    static BindingTrigger MakeHold(ButtonId btn, std::uint16_t thresholdMs = limits::kDefaultHoldMs)
    {
        BindingTrigger t;
        t.type = TriggerType::Hold;
        t.button = btn;
        t.modifier = ButtonId::None;
        t.hold_ms = thresholdMs;
        return t;
    }

    static BindingTrigger MakeChord(ButtonId modBtn, ButtonId primaryBtn)
    {
        BindingTrigger t;
        t.type = TriggerType::Chord;
        t.button = primaryBtn;
        t.modifier = modBtn;
        t.hold_ms = 0;
        return t;
    }

    bool operator==(const BindingTrigger& o) const
    {
        if (type != o.type || button != o.button)
            return false;
        if (type == TriggerType::Chord)
            return modifier == o.modifier;
        if (type == TriggerType::Hold)
            return hold_ms == o.hold_ms;
        return true;
    }

    bool operator!=(const BindingTrigger& o) const { return !(*this == o); }
};

struct BindingEntry
{
    ContextId context = ContextId::World_Gameplay;
    ActionId action = ActionId::Invalid;
    BindingTrigger trigger;
};

// Types of conflicts within the same context (D4.1 item 2)
enum class ConflictKind : std::uint8_t
{
    None = 0,
    ExactDuplicate = 1,       // Identical gesture (same Button, same Hold on same button, or same Chord mod+btn) in the same context
    HoldOverlap = 2,          // Two Hold bindings on the same button in the same context (even if hold_ms differ)
    ChordModifierShadow = 3   // Optional warning: button is used as a Chord modifier and also as a standalone Button/Hold in the same context
};

struct ConflictInfo
{
    ConflictKind kind = ConflictKind::None;
    ContextId context = ContextId::Invalid;
    ActionId existing_action = ActionId::Invalid;
    ActionId candidate_action = ActionId::Invalid;
    BindingTrigger existing_trigger;
    BindingTrigger candidate_trigger;
    bool is_blocking = false; // ExactDuplicate and HoldOverlap are true conflicts
};

enum class EditStatus : std::uint8_t
{
    Ok = 0,
    OkWithSwappedProtected = 1,        // Protected action reassigned by swapping with another action to keep navigation valid
    ConflictDetected = 2,              // Blocking conflict inside the same context (when allowOverwriteOrSwap == false)
    ProtectedActionCannotUnbind = 3,   // Menu / Confirm / Back in MenuUI cannot be unbound (D4.1 item 3)
    ProtectedActionInvalidTrigger = 4, // Protected navigation action requires a valid working button (not None, not self-chord, not leaving 0 working pairs)
    ProtectedNavigationBroken = 5,     // Change would leave MenuUI without a working Confirm+Back pair or Menu button
    InvalidContextOrAction = 6,
    InvalidTrigger = 7
};

struct EditResult
{
    EditStatus status = EditStatus::Ok;
    std::vector<ConflictInfo> conflicts;
    ActionId swapped_with = ActionId::Invalid;
    std::string message;

    bool Succeeded() const
    {
        return status == EditStatus::Ok || status == EditStatus::OkWithSwappedProtected;
    }
};

enum class LoadStatus : std::uint8_t
{
    Ok = 0,
    MigratedV1ToV2 = 1,
    RecoveredFromCorrupt = 2, // Malformed lines / invalid tokens sanitized & missing protected defaults restored
    FileNotFoundUsedDefaults = 3,
    ForbiddenTargetPath = 4,  // Refuses to read/write user.ltx or axr_options.ltx
    IoError = 5
};

struct LoadReport
{
    LoadStatus status = LoadStatus::Ok;
    std::uint32_t source_version = limits::kCurrentProfileSchema;
    std::uint32_t target_version = limits::kCurrentProfileSchema;
    std::uint32_t loaded_bindings = 0;
    std::uint32_t migrated_entries = 0;
    std::uint32_t corrupted_lines_skipped = 0;
    std::uint32_t sanitized_fields = 0;
    bool protected_menu_repaired = false;
    std::string detail;
};

// Runtime gesture & emergency reset tracker
struct GestureEvent
{
    ContextId context = ContextId::Invalid;
    ActionId action = ActionId::Invalid;
    TriggerType trigger_type = TriggerType::Button;
};

class BindingProfileModel
{
public:
    ENGINE_API BindingProfileModel();

    // Reset all contexts to the canonical default layout
    ENGINE_API void ResetToDefaults();
    ENGINE_API void ResetContextToDefaults(ContextId ctx);

    // Query bindings
    const std::vector<BindingEntry>& GetAllBindings() const { return m_bindings; }
    ENGINE_API std::vector<BindingEntry> GetBindingsForContext(ContextId ctx) const;
    ENGINE_API const BindingTrigger* FindBinding(ContextId ctx, ActionId act) const;
    ENGINE_API static std::vector<ContextId> GetCustomizableContexts();
    ENGINE_API static std::vector<ActionId> GetCanonicalActionsForContext(ContextId ctx);

    // Navigation protection queries (D4.1 item 3)
    ENGINE_API static bool IsProtectedMenuNavigationAction(ContextId ctx, ActionId act);
    ENGINE_API static bool IsValidTriggerForProtectedMenuAction(const BindingTrigger& trig);
    ENGINE_API bool VerifyMenuNavigationInvariant(std::string* outReason = nullptr) const;

    // Sanitize a trigger (clamps hold_ms into [100, 2000], checks button ranges, rejects modifier == button)
    ENGINE_API static bool SanitizeTrigger(BindingTrigger& trig, std::uint32_t* outSanitizeCount = nullptr);

    // Conflict detection within the same context; bindings in different contexts NEVER conflict (D4.1 item 2)
    ENGINE_API std::vector<ConflictInfo> DetectConflictsForCandidate(ContextId ctx,
                                                                     ActionId act,
                                                                     const BindingTrigger& candidate,
                                                                     bool includeModifierShadowWarnings = false) const;
    ENGINE_API std::vector<ConflictInfo> DetectAllConflictsInContext(ContextId ctx,
                                                                     bool includeModifierShadowWarnings = false) const;
    ENGINE_API std::vector<ConflictInfo> DetectAllConflicts(bool includeModifierShadowWarnings = false) const;

    // Modify bindings with conflict detection and MenuUI protection (D4.1 items 1-3)
    // - If allowConflictResolution == false and a blocking conflict exists in ctx, returns ConflictDetected without mutating.
    // - If allowConflictResolution == true:
    //     * if the conflicting action in ctx is a protected MenuUI action (or act itself is protected and takes another button),
    //       automatically swaps their triggers so Menu/Confirm/Back never become unbound.
    //     * if the conflicting action is non-protected and act is non-protected, swaps their triggers (or replaces if previous was unbound).
    ENGINE_API EditResult SetBinding(ContextId ctx,
                                     ActionId act,
                                     const BindingTrigger& newTrigger,
                                     bool allowConflictResolution = false);

    // Unbind an action in a context. Refuses with ProtectedActionCannotUnbind for Menu/Confirm/Back in MenuUI/MainMenu.
    ENGINE_API EditResult UnbindAction(ContextId ctx, ActionId act);

    // Explicitly swap two actions in the same context
    ENGINE_API EditResult SwapBindings(ContextId ctx, ActionId actA, ActionId actB);

    // Serialization to/from text (appdata\gamepad\profile.ltx format, D37, D4.1 item 4)
    ENGINE_API std::string SerializeToText() const;
    ENGINE_API LoadReport DeserializeFromText(const std::string& content);

    // File I/O & Export/Import (D4.1 item 4)
    // Security guard (D4.1 stop condition): refuses any path pointing to user.ltx, axr_options.ltx, or D:\Stalker
    ENGINE_API static bool IsPathForbiddenForWrite(const std::string& path, std::string* outReason = nullptr);
    ENGINE_API static std::string GetDefaultProfileRelativePath(); // "gamepad\\profile.ltx" under $app_data_root$

    ENGINE_API bool SaveToFile(const std::string& filePath, std::string* outError = nullptr) const;
    ENGINE_API LoadReport LoadFromFile(const std::string& filePath);

    ENGINE_API bool ExportToFile(const std::string& exportFilePath, std::string* outError = nullptr) const;
    ENGINE_API LoadReport ImportFromFile(const std::string& importFilePath,
                                         const std::string& activeProfileSavePath = "");

    // Emergency Reset (D4.1 item 5):
    // - Hold Start + Back >= 5000 ms in ANY context (even Console / MenuUI / World_Gameplay, regardless of current bindings)
    // - Or console command gp_bind_reset (ExecuteConsoleReset)
    ENGINE_API bool UpdateEmergencyResetHold(std::uint32_t rawButtonsMask,
                                             std::uint32_t dtContinualMs,
                                             const std::string& activeProfileSavePath = "");
    ENGINE_API void ExecuteEmergencyReset(const char* reason, const std::string& activeProfileSavePath = "");
    std::uint32_t GetEmergencyResetHoldElapsedMs() const { return m_emergencyHoldElapsedMs; }
    std::uint32_t GetEmergencyResetCount() const { return m_emergencyResetCount; }
    const std::string& GetLastEmergencyResetReason() const { return m_lastEmergencyResetReason; }

    // Runtime gesture evaluator for a context (evaluates Button, Hold threshold, and Chord modifier+button)
    // Takes effectiveDownMask (after D3.1 consumed_until_release barrier) and frame delta ms.
    ENGINE_API void ResetRuntimeGestureState();
    ENGINE_API std::vector<GestureEvent> EvaluateContextGestures(ContextId ctx,
                                                                 std::uint32_t effectiveDownMask,
                                                                 std::uint32_t dtContinualMs);

    std::uint32_t GetSchemaVersion() const { return m_schemaVersion; }

    // String conversion helpers for LTX serialization, UI, and console
    ENGINE_API static const char* ButtonToString(ButtonId btn);
    ENGINE_API static ButtonId StringToButton(const std::string& s);

    ENGINE_API static const char* ContextToString(ContextId ctx);
    ENGINE_API static ContextId StringToContext(const std::string& s);
    ENGINE_API static const char* ContextDisplayTitle(ContextId ctx);

    ENGINE_API static const char* ActionToString(ActionId act);
    ENGINE_API static ActionId StringToAction(const std::string& s);
    ENGINE_API static const char* ActionDisplayTitle(ActionId act);

    ENGINE_API static const char* TriggerTypeToString(TriggerType t);
    ENGINE_API static bool TryParseTriggerSpec(const std::string& spec,
                                               BindingTrigger& outTrig,
                                               std::uint32_t* outSanitized = nullptr);
    ENGINE_API static std::string FormatTriggerSpec(const BindingTrigger& trig);
    ENGINE_API static std::string FormatTriggerHumanReadable(const BindingTrigger* trig);

private:
    std::uint32_t m_schemaVersion = limits::kCurrentProfileSchema;
    std::vector<BindingEntry> m_bindings;

    // Emergency reset state (Start + Back 5000 ms)
    std::uint32_t m_emergencyHoldElapsedMs = 0;
    bool m_emergencyResetLatchedUntilRelease = false;
    std::uint32_t m_emergencyResetCount = 0;
    std::string m_lastEmergencyResetReason = "none";

    // Runtime gesture state per virtual button (0..16)
    ContextId m_runtimePrevCtx = ContextId::Invalid;
    std::uint32_t m_runtimePrevDownMask = 0;
    std::uint32_t m_runtimeHoldElapsedMs[static_cast<std::size_t>(ButtonId::Count)] = {};
    bool m_runtimeHoldFired[static_cast<std::size_t>(ButtonId::Count)] = {};
    bool m_runtimeUsedInChordOrMod[static_cast<std::size_t>(ButtonId::Count)] = {};

    static bool DoTriggersConflict(const BindingTrigger& a,
                                   const BindingTrigger& b,
                                   ConflictKind& outKind,
                                   bool& outIsBlocking,
                                   bool includeModifierShadowWarnings);
    void EnsureProtectedMenuDefaults(bool* outRepaired = nullptr);
};

// Global active binding profile accessors for xrEngine / xrGame (D4.2)
using LogCallbackFn = void (*)(const char* msg);
using FBindingTranslateCallback = const char* (*)(const char* stringId, const char* fallbackText);

ENGINE_API void SetBindingLogCallback(LogCallbackFn cb);
ENGINE_API void SetBindingTranslateCallback(FBindingTranslateCallback cb);
ENGINE_API void SetActiveBindingProfilePath(const char* fullPath);
ENGINE_API const char* GetActiveBindingProfilePath();
ENGINE_API void SetDefaultExportProfilePath(const char* fullPath);
ENGINE_API const char* GetDefaultExportProfilePath();

ENGINE_API const BindingProfileModel& GetActiveBindingProfile();
ENGINE_API BindingProfileModel& GetMutableActiveBindingProfile();

ENGINE_API LoadReport LoadActiveBindingProfile();
ENGINE_API bool ReloadActiveBindingProfileIfModifiedOnDisk();
ENGINE_API bool SaveActiveBindingProfile(std::string* outError = nullptr);
ENGINE_API bool ExportActiveBindingProfile(const char* customPath = nullptr, std::string* outError = nullptr);
ENGINE_API LoadReport ImportActiveBindingProfile(const char* customPath = nullptr);
ENGINE_API void ResetActiveBindingProfileEmergency(const char* reason = "gp_bind_reset");
ENGINE_API bool UpdateActiveEmergencyResetHold(std::uint32_t rawButtonsMask, std::uint32_t dtContinualMs);
ENGINE_API void LogActiveBindingProfileStatus();

} // namespace gpbind
