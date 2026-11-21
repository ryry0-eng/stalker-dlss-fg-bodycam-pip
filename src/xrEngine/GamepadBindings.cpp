// D4.1: Gamepad layout & binding model implementation. See GamepadBindings.h.
// NOTE: when compiled inside xrEngine/xrGame with precompiled headers, either set
// "Not Using Precompiled Headers" for this file or define GPBIND_USE_PCH.
#ifdef GPBIND_USE_PCH
#include "stdafx.h"
#pragma hdrstop
#endif

#include "GamepadBindings.h"
#include "GamepadAtomicFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace gpbind
{
namespace
{

std::string Trim(const std::string& s)
{
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first])))
        ++first;
    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1])))
        --last;
    return s.substr(first, last - first);
}

std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool IsValidButtonId(ButtonId btn)
{
    return static_cast<std::uint8_t>(btn) < static_cast<std::uint8_t>(ButtonId::Count);
}

bool IsValidContextId(ContextId ctx)
{
    return static_cast<std::uint8_t>(ctx) < static_cast<std::uint8_t>(ContextId::Count);
}

bool IsValidActionId(ActionId act)
{
    return static_cast<std::uint16_t>(act) < static_cast<std::uint16_t>(ActionId::Count);
}

// Legacy v1 context aliases -> v2 ContextId (used during v1 -> v2 schema migration)
ContextId MapLegacyV1ContextName(const std::string& rawSection)
{
    const std::string low = ToLowerAscii(Trim(rawSection));
    if (low == "bindings_menu" || low == "menu" || low == "ui" || low == "menu_ui" || low == "bindings_menu_ui")
        return ContextId::MenuUI;
    if (low == "bindings_main_menu" || low == "main_menu")
        return ContextId::MainMenu;
    if (low == "bindings_world" || low == "world" || low == "gameplay" || low == "combat")
        return ContextId::World_Gameplay;
    if (low == "bindings_dialog" || low == "dialog")
        return ContextId::Dialog_NPC;
    if (low == "bindings_wheel" || low == "wheel")
        return ContextId::RadialWheel;
    if (low == "bindings_pda" || low == "pda")
        return ContextId::World_3DPDA;
    if (low == "bindings_placement" || low == "placement")
        return ContextId::Placement_HideoutFurniture;
    return ContextId::Invalid;
}

// Legacy v1 action names -> v2 ActionId (used during v1 -> v2 schema migration)
ActionId MapLegacyV1ActionName(const std::string& rawKey)
{
    const std::string low = ToLowerAscii(Trim(rawKey));
    if (low == "ui_accept" || low == "accept" || low == "ok")
        return ActionId::Confirm;
    if (low == "ui_cancel" || low == "cancel" || low == "close")
        return ActionId::Back;
    if (low == "ui_menu" || low == "open_menu")
        return ActionId::Menu;
    if (low == "interact_reload" || low == "use_or_reload" || low == "reload_interact")
        return ActionId::ContextUseReload;
    if (low == "tactical_reload" || low == "quick_reload")
        return ActionId::FastReload;
    if (low == "check_ammo")
        return ActionId::AmmoCheck;
    if (low == "crouch_low")
        return ActionId::LowCrouch;
    if (low == "shoot" || low == "attack")
        return ActionId::Fire;
    if (low == "ads" || low == "zoom")
        return ActionId::Aim;
    if (low == "flashlight" || low == "torch")
        return ActionId::TorchToggle;
    if (low == "nvg")
        return ActionId::NightVision;
    return BindingProfileModel::StringToAction(rawKey);
}

bool IsKnownForeignProfileSection(const std::string& lowSec)
{
    return lowSec == "profile" ||
           lowSec == "left_stick" ||
           lowSec == "right_stick" ||
           lowSec == "ui_stick" ||
           lowSec == "left_trigger" ||
           lowSec == "right_trigger" ||
           lowSec == "haptics" ||
           lowSec == "glyphs" ||
           lowSec == "source" ||      // GPSET-2: режим блокировки источника ввода (auto / gamepad_only / kbm_only)
           lowSec == "gp_icons" ||    // ICONS-1: выбранный набор иконок (gp_icons.script)
           lowSec == "cursor" ||      // CURSOR1: размер и скорость виртуального курсора (gp_cursor.script)
           lowSec == "wheel_qaw" ||   // WHEEL-QAW: слоты колеса QAW (gp_qaw_patch.script)
           lowSec == "wheel_slots";   // E5.2: слоты быстрого колеса (пишет level_script gp_wheel_write_slots)
}

bool IsOurBindingSection(const std::string& rawSection)
{
    const std::string lowSec = ToLowerAscii(Trim(rawSection));
    if (lowSec.empty() || lowSec == "profile_header" || lowSec == "header" || lowSec == "gamepad_profile")
        return true;
    if (lowSec.rfind("context_", 0) == 0)
        return true;
    if (BindingProfileModel::StringToContext(rawSection) != ContextId::Invalid)
        return true;
    if (MapLegacyV1ContextName(rawSection) != ContextId::Invalid)
        return true;
    return false;
}

} // namespace

BindingProfileModel::BindingProfileModel()
{
    ResetToDefaults();
}

void BindingProfileModel::ResetToDefaults()
{
    m_schemaVersion = limits::kCurrentProfileSchema;
    m_bindings.clear();

    auto add = [this](ContextId ctx, ActionId act, const BindingTrigger& trig) {
        BindingEntry e;
        e.context = ctx;
        e.action = act;
        e.trigger = trig;
        m_bindings.push_back(e);
    };

    // 1. MenuUI (MenuUI_Modal): protected Menu / Confirm / Back + standard UI tabs & actions
    add(ContextId::MenuUI, ActionId::Menu, BindingTrigger::MakeButton(ButtonId::Start));
    add(ContextId::MenuUI, ActionId::Confirm, BindingTrigger::MakeButton(ButtonId::South));   // A
    add(ContextId::MenuUI, ActionId::Back, BindingTrigger::MakeButton(ButtonId::East));       // B
    add(ContextId::MenuUI, ActionId::ContextMenu, BindingTrigger::MakeButton(ButtonId::West));// X
    add(ContextId::MenuUI, ActionId::QuickAction, BindingTrigger::MakeButton(ButtonId::North));// Y
    add(ContextId::MenuUI, ActionId::TabPrev, BindingTrigger::MakeButton(ButtonId::LeftShoulder));
    add(ContextId::MenuUI, ActionId::TabNext, BindingTrigger::MakeButton(ButtonId::RightShoulder));
    add(ContextId::MenuUI, ActionId::PagePrev, BindingTrigger::MakeButton(ButtonId::LT));
    add(ContextId::MenuUI, ActionId::PageNext, BindingTrigger::MakeButton(ButtonId::RT));
    add(ContextId::MenuUI, ActionId::MouseCursor, BindingTrigger::MakeButton(ButtonId::RightStick));

    // 2. MainMenu: protected Menu / Confirm / Back + tabs
    add(ContextId::MainMenu, ActionId::Menu, BindingTrigger::MakeButton(ButtonId::Start));
    add(ContextId::MainMenu, ActionId::Confirm, BindingTrigger::MakeButton(ButtonId::South));
    add(ContextId::MainMenu, ActionId::Back, BindingTrigger::MakeButton(ButtonId::East));
    add(ContextId::MainMenu, ActionId::TabPrev, BindingTrigger::MakeButton(ButtonId::LeftShoulder));
    add(ContextId::MainMenu, ActionId::TabNext, BindingTrigger::MakeButton(ButtonId::RightShoulder));

    // 3. World_Gameplay: single buttons, holds, and chords matching live game behavior (K1 / K2 item 2)
    add(ContextId::World_Gameplay, ActionId::Jump, BindingTrigger::MakeButton(ButtonId::South));
    add(ContextId::World_Gameplay, ActionId::Crouch, BindingTrigger::MakeButton(ButtonId::East));
    add(ContextId::World_Gameplay, ActionId::LowCrouch, BindingTrigger::MakeHold(ButtonId::East, 300));
    add(ContextId::World_Gameplay, ActionId::Sprint, BindingTrigger::MakeButton(ButtonId::LeftStick));
    // GAYS v0.2 (2026-11-13): RB = knife (tap strike / hold draw), R3 = lower weapon, LB + B = freelook toggle
    add(ContextId::World_Gameplay, ActionId::MeleeQuick, BindingTrigger::MakeButton(ButtonId::RightShoulder));
    add(ContextId::World_Gameplay, ActionId::KnifeDraw, BindingTrigger::MakeHold(ButtonId::RightShoulder, 300));
    add(ContextId::World_Gameplay, ActionId::WeaponLower, BindingTrigger::MakeButton(ButtonId::RightStick));
    add(ContextId::World_Gameplay, ActionId::Freelook, BindingTrigger::MakeChord(ButtonId::LeftShoulder, ButtonId::East));
    add(ContextId::World_Gameplay, ActionId::Fire, BindingTrigger::MakeButton(ButtonId::RT));
    add(ContextId::World_Gameplay, ActionId::Aim, BindingTrigger::MakeButton(ButtonId::LT));
    add(ContextId::World_Gameplay, ActionId::ContextUseReload, BindingTrigger::MakeButton(ButtonId::West));
    add(ContextId::World_Gameplay, ActionId::AmmoCheck, BindingTrigger::MakeHold(ButtonId::West, 300));
    add(ContextId::World_Gameplay, ActionId::ForceReload, BindingTrigger::MakeChord(ButtonId::LT, ButtonId::West));
    add(ContextId::World_Gameplay, ActionId::NextWeapon, BindingTrigger::MakeButton(ButtonId::North));
    add(ContextId::World_Gameplay, ActionId::WeaponSelect, BindingTrigger::MakeHold(ButtonId::North, 300));

    add(ContextId::World_Gameplay, ActionId::QuickBoltOrSlot, BindingTrigger::MakeButton(ButtonId::LeftShoulder));
    add(ContextId::World_Gameplay, ActionId::WeaponWheel, BindingTrigger::MakeHold(ButtonId::LeftShoulder, 250));
    add(ContextId::World_Gameplay, ActionId::GrenadeQuick, BindingTrigger::MakeChord(ButtonId::LeftShoulder, ButtonId::North));
    add(ContextId::World_Gameplay, ActionId::LeanLeft, BindingTrigger::MakeChord(ButtonId::LT, ButtonId::LeftShoulder));
    add(ContextId::World_Gameplay, ActionId::LeanRight, BindingTrigger::MakeChord(ButtonId::LT, ButtonId::RightShoulder));
    add(ContextId::World_Gameplay, ActionId::TorchToggle, BindingTrigger::MakeButton(ButtonId::DPadUp));
    add(ContextId::World_Gameplay, ActionId::NightVision, BindingTrigger::MakeHold(ButtonId::DPadUp, 300));
    // GAYS v0.2: 3D PDA, mask wipe and fast reload live in the LB wheel (no default button)
    add(ContextId::World_Gameplay, ActionId::DetectorToggle, BindingTrigger::MakeButton(ButtonId::DPadLeft));
    add(ContextId::World_Gameplay, ActionId::BoltThrow, BindingTrigger::MakeHold(ButtonId::DPadLeft, 300));
    add(ContextId::World_Gameplay, ActionId::FireModeNext, BindingTrigger::MakeButton(ButtonId::DPadRight));
    add(ContextId::World_Gameplay, ActionId::UnderbarrelToggle, BindingTrigger::MakeHold(ButtonId::DPadRight, 300));
    add(ContextId::World_Gameplay, ActionId::AmmoTypeNext, BindingTrigger::MakeButton(ButtonId::DPadDown));
    add(ContextId::World_Gameplay, ActionId::AmmoWheel, BindingTrigger::MakeHold(ButtonId::DPadDown, 300));
    add(ContextId::World_Gameplay, ActionId::PauseMenu, BindingTrigger::MakeButton(ButtonId::Start));
    add(ContextId::World_Gameplay, ActionId::PdaFullToggle, BindingTrigger::MakeButton(ButtonId::Back));
    add(ContextId::World_Gameplay, ActionId::PdaFlat, BindingTrigger::MakeHold(ButtonId::Back, 300));
    add(ContextId::World_Gameplay, ActionId::TutorialPrompt, BindingTrigger::MakeChord(ButtonId::LeftShoulder, ButtonId::Back));
    // FIX-LIVE2 п.15 -> FIX-LIVE3 п.18 (Миша 2026-11-16): сочетания WheelMaskClean / WheelLaser по умолчанию ПУСТЫЕ.
    // LB + D-pad теперь = закреплённые слоты крестовины (gp_qaw_patch pins, StepWorldWeaponButtons). Действия остаются в
    // списке переназначения (GetActionsForContext) — игрок может назначить их сам.

    // 4. Dialog_NPC
    add(ContextId::Dialog_NPC, ActionId::DialogSelect, BindingTrigger::MakeButton(ButtonId::South));
    add(ContextId::Dialog_NPC, ActionId::DialogExit, BindingTrigger::MakeButton(ButtonId::East));

    // 5. RadialWheel
    add(ContextId::RadialWheel, ActionId::WheelConfirm, BindingTrigger::MakeButton(ButtonId::South));
    add(ContextId::RadialWheel, ActionId::WheelCancel, BindingTrigger::MakeButton(ButtonId::East));
    add(ContextId::RadialWheel, ActionId::WheelTabPrev, BindingTrigger::MakeButton(ButtonId::LT));
    add(ContextId::RadialWheel, ActionId::WheelTabNext, BindingTrigger::MakeButton(ButtonId::RT));

    // 6. World_3DPDA
    add(ContextId::World_3DPDA, ActionId::PdaInteractiveToggle, BindingTrigger::MakeButton(ButtonId::North));
    add(ContextId::World_3DPDA, ActionId::PdaClose, BindingTrigger::MakeButton(ButtonId::East));

    // 7. Placement_HideoutFurniture
    add(ContextId::Placement_HideoutFurniture, ActionId::PlacementConfirm, BindingTrigger::MakeButton(ButtonId::South));
    add(ContextId::Placement_HideoutFurniture, ActionId::PlacementCancel, BindingTrigger::MakeButton(ButtonId::East));
    add(ContextId::Placement_HideoutFurniture, ActionId::PlacementRotate, BindingTrigger::MakeButton(ButtonId::West));

    ResetRuntimeGestureState();
}

void BindingProfileModel::ResetContextToDefaults(ContextId ctx)
{
    BindingProfileModel defaults;
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(), [ctx](const BindingEntry& e) { return e.context == ctx; }),
        m_bindings.end());
    for (const auto& defEntry : defaults.GetBindingsForContext(ctx))
    {
        m_bindings.push_back(defEntry);
    }
    EnsureProtectedMenuDefaults();
}

std::vector<ContextId> BindingProfileModel::GetCustomizableContexts()
{
    return {
        ContextId::World_Gameplay,
        ContextId::MenuUI,
        ContextId::MainMenu,
        ContextId::Dialog_NPC,
        ContextId::RadialWheel,
        ContextId::World_3DPDA,
        ContextId::Placement_HideoutFurniture
    };
}

std::vector<ActionId> BindingProfileModel::GetCanonicalActionsForContext(ContextId ctx)
{
    switch (ctx)
    {
    case ContextId::MenuUI:
        return {
            ActionId::Menu,
            ActionId::Confirm,
            ActionId::Back,
            ActionId::ContextMenu,
            ActionId::QuickAction,
            ActionId::TabPrev,
            ActionId::TabNext,
            ActionId::PagePrev,
            ActionId::PageNext,
            ActionId::SwitchPanel,
            ActionId::CancelQueue,
            ActionId::MouseCursor
        };
    case ContextId::MainMenu:
        return {
            ActionId::Menu,
            ActionId::Confirm,
            ActionId::Back,
            ActionId::TabPrev,
            ActionId::TabNext
        };
    case ContextId::World_Gameplay:
        return {
            ActionId::Jump,
            ActionId::Crouch,
            ActionId::LowCrouch,
            ActionId::Sprint,
            ActionId::MeleeQuick,
            ActionId::Freelook,
            ActionId::Fire,
            ActionId::Aim,
            ActionId::ContextUseReload,
            ActionId::AmmoCheck,
            ActionId::FastReload,
            ActionId::NextWeapon,
            ActionId::QuickBoltOrSlot,
            ActionId::WeaponWheel,
            ActionId::GrenadeQuick,
            ActionId::LeanLeft,
            ActionId::LeanRight,
            ActionId::TorchToggle,
            ActionId::NightVision,
            ActionId::Pda3DToggle,
            ActionId::MaskClean,
            ActionId::DetectorToggle,
            ActionId::UnderbarrelToggle,
            ActionId::FireModeNext,
            ActionId::AmmoTypeNext,
            ActionId::PauseMenu,
            ActionId::PdaFullToggle,
            ActionId::KnifeDraw,
            ActionId::WeaponLower,
            ActionId::PdaFlat,
            ActionId::AmmoWheel,
            ActionId::BoltThrow,
            ActionId::WeaponSelect,
            ActionId::ForceReload,
            ActionId::TutorialPrompt,
            ActionId::WheelMaskClean,
            ActionId::WheelLaser
        };
    case ContextId::Dialog_NPC:
        return {
            ActionId::DialogSelect,
            ActionId::DialogExit
        };
    case ContextId::RadialWheel:
        return {
            ActionId::WheelConfirm,
            ActionId::WheelCancel,
            ActionId::WheelTabPrev,
            ActionId::WheelTabNext
        };
    case ContextId::World_3DPDA:
        return {
            ActionId::PdaInteractiveToggle,
            ActionId::PdaClose
        };
    case ContextId::Placement_HideoutFurniture:
        return {
            ActionId::PlacementConfirm,
            ActionId::PlacementCancel,
            ActionId::PlacementRotate
        };
    default:
        return {};
    }
}

std::vector<BindingEntry> BindingProfileModel::GetBindingsForContext(ContextId ctx) const
{
    std::vector<BindingEntry> result;
    for (const auto& e : m_bindings)
    {
        if (e.context == ctx)
            result.push_back(e);
    }
    return result;
}

const BindingTrigger* BindingProfileModel::FindBinding(ContextId ctx, ActionId act) const
{
    for (const auto& e : m_bindings)
    {
        if (e.context == ctx && e.action == act)
            return &e.trigger;
    }
    return nullptr;
}

bool BindingProfileModel::IsProtectedMenuNavigationAction(ContextId ctx, ActionId act)
{
    if (ctx != ContextId::MenuUI && ctx != ContextId::MainMenu)
        return false;
    return act == ActionId::Menu || act == ActionId::Confirm || act == ActionId::Back;
}

bool BindingProfileModel::IsValidTriggerForProtectedMenuAction(const BindingTrigger& trig)
{
    if (!IsValidButtonId(trig.button))
        return false;
    // Guide button is reserved by OS/Steam overlays on many systems; Menu/Confirm/Back must use a standard reachable button
    if (trig.button == ButtonId::Guide)
        return false;
    // Protected Menu/Confirm/Back must be a direct Button press or a valid Hold (never an unreachable multi-button Chord)
    if (trig.type == TriggerType::Chord)
        return false;
    if (trig.type == TriggerType::Hold)
    {
        if (trig.hold_ms < limits::kMinHoldMs || trig.hold_ms > limits::kMaxHoldMs)
            return false;
    }
    return true;
}

bool BindingProfileModel::VerifyMenuNavigationInvariant(std::string* outReason) const
{
    const ContextId navContexts[] = {ContextId::MenuUI, ContextId::MainMenu};
    for (ContextId ctx : navContexts)
    {
        const BindingTrigger* menuTrig = FindBinding(ctx, ActionId::Menu);
        const BindingTrigger* confirmTrig = FindBinding(ctx, ActionId::Confirm);
        const BindingTrigger* backTrig = FindBinding(ctx, ActionId::Back);

        if (!menuTrig || !IsValidTriggerForProtectedMenuAction(*menuTrig))
        {
            if (outReason)
                *outReason = std::string(ContextToString(ctx)) + ": missing or invalid protected 'Menu' binding";
            return false;
        }
        if (!confirmTrig || !IsValidTriggerForProtectedMenuAction(*confirmTrig))
        {
            if (outReason)
                *outReason = std::string(ContextToString(ctx)) + ": missing or invalid protected 'Confirm' binding";
            return false;
        }
        if (!backTrig || !IsValidTriggerForProtectedMenuAction(*backTrig))
        {
            if (outReason)
                *outReason = std::string(ContextToString(ctx)) + ": missing or invalid protected 'Back' binding";
            return false;
        }

        // Confirm and Back must remain a working pair on distinct buttons/triggers so the user can both enter and exit
        if (confirmTrig->button == backTrig->button)
        {
            if (outReason)
                *outReason = std::string(ContextToString(ctx)) + ": protected 'Confirm' and 'Back' cannot share the same button";
            return false;
        }
        // Menu must not collide with Confirm or Back on the exact same trigger
        if (*menuTrig == *confirmTrig || *menuTrig == *backTrig)
        {
            if (outReason)
                *outReason = std::string(ContextToString(ctx)) + ": protected 'Menu' collides with 'Confirm' or 'Back'";
            return false;
        }
    }
    return true;
}

bool BindingProfileModel::SanitizeTrigger(BindingTrigger& trig, std::uint32_t* outSanitizeCount)
{
    std::uint32_t fixes = 0;
    if (static_cast<std::uint8_t>(trig.type) > static_cast<std::uint8_t>(TriggerType::Chord))
    {
        trig.type = TriggerType::Button;
        ++fixes;
    }

    if (!IsValidButtonId(trig.button))
    {
        if (outSanitizeCount)
            *outSanitizeCount += fixes;
        return false;
    }

    if (trig.type == TriggerType::Button)
    {
        if (trig.modifier != ButtonId::None)
        {
            trig.modifier = ButtonId::None;
            ++fixes;
        }
        if (trig.hold_ms != 0)
        {
            trig.hold_ms = 0;
            ++fixes;
        }
    }
    else if (trig.type == TriggerType::Hold)
    {
        if (trig.modifier != ButtonId::None)
        {
            trig.modifier = ButtonId::None;
            ++fixes;
        }
        if (trig.hold_ms == 0)
        {
            trig.hold_ms = limits::kDefaultHoldMs;
            ++fixes;
        }
        else if (trig.hold_ms < limits::kMinHoldMs)
        {
            trig.hold_ms = limits::kMinHoldMs;
            ++fixes;
        }
        else if (trig.hold_ms > limits::kMaxHoldMs)
        {
            trig.hold_ms = limits::kMaxHoldMs;
            ++fixes;
        }
    }
    else if (trig.type == TriggerType::Chord)
    {
        if (!IsValidButtonId(trig.modifier) || trig.modifier == trig.button)
        {
            if (outSanitizeCount)
                *outSanitizeCount += fixes;
            return false;
        }
        if (trig.hold_ms != 0)
        {
            trig.hold_ms = 0;
            ++fixes;
        }
    }

    if (outSanitizeCount)
        *outSanitizeCount += fixes;
    return true;
}

bool BindingProfileModel::DoTriggersConflict(const BindingTrigger& a,
                                             const BindingTrigger& b,
                                             ConflictKind& outKind,
                                             bool& outIsBlocking,
                                             bool includeModifierShadowWarnings)
{
    outKind = ConflictKind::None;
    outIsBlocking = false;

    // 1. Both are single Button presses on the same button -> ExactDuplicate (blocking)
    if (a.type == TriggerType::Button && b.type == TriggerType::Button)
    {
        if (a.button == b.button)
        {
            outKind = ConflictKind::ExactDuplicate;
            outIsBlocking = true;
            return true;
        }
        return false;
    }

    // 2. Both are Hold triggers on the same button -> ExactDuplicate if same threshold, or HoldOverlap if different (both blocking)
    if (a.type == TriggerType::Hold && b.type == TriggerType::Hold)
    {
        if (a.button == b.button)
        {
            outKind = (a.hold_ms == b.hold_ms) ? ConflictKind::ExactDuplicate : ConflictKind::HoldOverlap;
            outIsBlocking = true;
            return true;
        }
        return false;
    }

    // 3. Both are Chords -> ExactDuplicate if same (modifier, button) or reversed (button, modifier)
    if (a.type == TriggerType::Chord && b.type == TriggerType::Chord)
    {
        if (a.modifier == b.modifier && a.button == b.button)
        {
            outKind = ConflictKind::ExactDuplicate;
            outIsBlocking = true;
            return true;
        }
        return false;
    }

    // Note: Button(X) + Hold(X) on the same button is a standard intentional pattern in G.A.M.M.A.
    // (e.g., Tap B = Crouch, Hold B = LowCrouch; Tap X = Use/Reload, Hold X = AmmoCheck; Tap DPadUp = Torch, Hold DPadUp = NVG),
    // so Button vs Hold on the same button is NOT a blocking conflict.

    // 4. Optional non-blocking warning: a Chord uses modifier M while another action in the same context uses M as Button/Hold
    if (includeModifierShadowWarnings)
    {
        if (a.type == TriggerType::Chord && (b.type == TriggerType::Button || b.type == TriggerType::Hold) &&
            a.modifier == b.button)
        {
            outKind = ConflictKind::ChordModifierShadow;
            outIsBlocking = false;
            return true;
        }
        if (b.type == TriggerType::Chord && (a.type == TriggerType::Button || a.type == TriggerType::Hold) &&
            b.modifier == a.button)
        {
            outKind = ConflictKind::ChordModifierShadow;
            outIsBlocking = false;
            return true;
        }
    }

    return false;
}

std::vector<ConflictInfo> BindingProfileModel::DetectConflictsForCandidate(ContextId ctx,
                                                                           ActionId act,
                                                                           const BindingTrigger& candidate,
                                                                           bool includeModifierShadowWarnings) const
{
    std::vector<ConflictInfo> conflicts;
    for (const auto& entry : m_bindings)
    {
        // D4.1 item 2: bindings in DIFFERENT contexts never conflict!
        if (entry.context != ctx)
            continue;
        if (entry.action == act)
            continue;

        ConflictKind kind = ConflictKind::None;
        bool isBlocking = false;
        if (DoTriggersConflict(entry.trigger, candidate, kind, isBlocking, includeModifierShadowWarnings))
        {
            ConflictInfo info;
            info.kind = kind;
            info.context = ctx;
            info.existing_action = entry.action;
            info.candidate_action = act;
            info.existing_trigger = entry.trigger;
            info.candidate_trigger = candidate;
            info.is_blocking = isBlocking;
            conflicts.push_back(info);
        }
    }
    return conflicts;
}

std::vector<ConflictInfo> BindingProfileModel::DetectAllConflictsInContext(ContextId ctx,
                                                                           bool includeModifierShadowWarnings) const
{
    std::vector<ConflictInfo> conflicts;
    for (std::size_t i = 0; i < m_bindings.size(); ++i)
    {
        if (m_bindings[i].context != ctx)
            continue;
        for (std::size_t j = i + 1; j < m_bindings.size(); ++j)
        {
            if (m_bindings[j].context != ctx)
                continue;
            ConflictKind kind = ConflictKind::None;
            bool isBlocking = false;
            if (DoTriggersConflict(m_bindings[i].trigger, m_bindings[j].trigger, kind, isBlocking, includeModifierShadowWarnings))
            {
                ConflictInfo info;
                info.kind = kind;
                info.context = ctx;
                info.existing_action = m_bindings[i].action;
                info.candidate_action = m_bindings[j].action;
                info.existing_trigger = m_bindings[i].trigger;
                info.candidate_trigger = m_bindings[j].trigger;
                info.is_blocking = isBlocking;
                conflicts.push_back(info);
            }
        }
    }
    return conflicts;
}

std::vector<ConflictInfo> BindingProfileModel::DetectAllConflicts(bool includeModifierShadowWarnings) const
{
    std::vector<ConflictInfo> all;
    for (std::uint8_t c = 0; c < static_cast<std::uint8_t>(ContextId::Count); ++c)
    {
        auto sub = DetectAllConflictsInContext(static_cast<ContextId>(c), includeModifierShadowWarnings);
        all.insert(all.end(), sub.begin(), sub.end());
    }
    return all;
}

EditResult BindingProfileModel::SetBinding(ContextId ctx,
                                           ActionId act,
                                           const BindingTrigger& rawNewTrigger,
                                           bool allowConflictResolution)
{
    EditResult res;
    if (!IsValidContextId(ctx) || !IsValidActionId(act))
    {
        res.status = EditStatus::InvalidContextOrAction;
        res.message = "Invalid context or action ID";
        return res;
    }

    BindingTrigger newTrigger = rawNewTrigger;
    if (!SanitizeTrigger(newTrigger))
    {
        res.status = EditStatus::InvalidTrigger;
        res.message = "Invalid button or chord trigger specification";
        return res;
    }

    const bool bIsProtected = IsProtectedMenuNavigationAction(ctx, act);
    if (bIsProtected && !IsValidTriggerForProtectedMenuAction(newTrigger))
    {
        res.status = EditStatus::ProtectedActionInvalidTrigger;
        res.message = "Protected MenuUI navigation action (Menu/Confirm/Back) requires a direct button or hold on a standard button";
        return res;
    }

    // Check conflicts within the same context (D4.1 item 2)
    res.conflicts = DetectConflictsForCandidate(ctx, act, newTrigger, false);
    bool hasBlockingConflict = false;
    for (const auto& c : res.conflicts)
    {
        if (c.is_blocking)
        {
            hasBlockingConflict = true;
            break;
        }
    }

    // Also check if assigning newTrigger to a non-protected or protected action in MenuUI/MainMenu
    // collides with Confirm/Back on the same button (since Confirm and Back must stay distinct)
    if (ctx == ContextId::MenuUI || ctx == ContextId::MainMenu)
    {
        for (const auto& entry : m_bindings)
        {
            if (entry.context != ctx || entry.action == act)
                continue;
            if (IsProtectedMenuNavigationAction(ctx, entry.action) && entry.trigger.button == newTrigger.button)
            {
                bool alreadyListed = false;
                for (const auto& c : res.conflicts)
                {
                    if (c.existing_action == entry.action)
                    {
                        alreadyListed = true;
                        break;
                    }
                }
                if (!alreadyListed)
                {
                    ConflictInfo ci;
                    ci.kind = ConflictKind::ExactDuplicate;
                    ci.context = ctx;
                    ci.existing_action = entry.action;
                    ci.candidate_action = act;
                    ci.existing_trigger = entry.trigger;
                    ci.candidate_trigger = newTrigger;
                    ci.is_blocking = true;
                    res.conflicts.push_back(ci);
                }
                hasBlockingConflict = true;
            }
        }
    }

    if (hasBlockingConflict && !allowConflictResolution)
    {
        res.status = EditStatus::ConflictDetected;
        res.message = "Conflict detected within context; pass allowConflictResolution=true to swap or overwrite";
        return res;
    }

    // Work on a transactional copy so we can verify MenuUI navigation invariants before committing
    std::vector<BindingEntry> candidateBindings = m_bindings;
    const BindingTrigger* prevTriggerPtr = FindBinding(ctx, act);
    const bool hadPrevTrigger = (prevTriggerPtr != nullptr);
    const BindingTrigger prevTrigger = hadPrevTrigger ? *prevTriggerPtr : BindingTrigger{};

    ActionId swappedAction = ActionId::Invalid;
    bool swappedProtected = false;

    if (hasBlockingConflict)
    {
        for (const auto& c : res.conflicts)
        {
            if (!c.is_blocking)
                continue;
            const bool otherProtected = IsProtectedMenuNavigationAction(ctx, c.existing_action);
            if (otherProtected || bIsProtected)
            {
                // Protected actions can never be unbound — they can ONLY be swapped if the current action had a valid trigger!
                if (!hadPrevTrigger || !IsValidTriggerForProtectedMenuAction(prevTrigger))
                {
                    res.status = EditStatus::ProtectedActionCannotUnbind;
                    res.message = "Cannot overwrite protected Menu/Confirm/Back without a valid swap button";
                    return res;
                }
                for (auto& e : candidateBindings)
                {
                    if (e.context == ctx && e.action == c.existing_action)
                    {
                        e.trigger = prevTrigger;
                        swappedAction = c.existing_action;
                        swappedProtected = true;
                        break;
                    }
                }
            }
            else
            {
                // Non-protected conflict: swap if hadPrevTrigger, otherwise remove conflicting binding
                if (hadPrevTrigger)
                {
                    for (auto& e : candidateBindings)
                    {
                        if (e.context == ctx && e.action == c.existing_action)
                        {
                            e.trigger = prevTrigger;
                            swappedAction = c.existing_action;
                            break;
                        }
                    }
                }
                else
                {
                    candidateBindings.erase(
                        std::remove_if(candidateBindings.begin(), candidateBindings.end(),
                                       [&](const BindingEntry& e) {
                                           return e.context == ctx && e.action == c.existing_action;
                                       }),
                        candidateBindings.end());
                }
            }
        }
    }

    bool updatedExisting = false;
    for (auto& e : candidateBindings)
    {
        if (e.context == ctx && e.action == act)
        {
            e.trigger = newTrigger;
            updatedExisting = true;
            break;
        }
    }
    if (!updatedExisting)
    {
        BindingEntry ne;
        ne.context = ctx;
        ne.action = act;
        ne.trigger = newTrigger;
        candidateBindings.push_back(ne);
    }

    // Verify MenuUI/MainMenu navigation invariant on the candidate state
    BindingProfileModel tempModel = *this;
    tempModel.m_bindings = candidateBindings;
    std::string navErr;
    if (!tempModel.VerifyMenuNavigationInvariant(&navErr))
    {
        res.status = EditStatus::ProtectedNavigationBroken;
        res.message = navErr;
        return res;
    }

    m_bindings = std::move(candidateBindings);
    res.swapped_with = swappedAction;
    res.status = swappedProtected ? EditStatus::OkWithSwappedProtected : EditStatus::Ok;
    res.message = swappedProtected ? "Protected navigation binding swapped safely" : "Binding updated";
    return res;
}

EditResult BindingProfileModel::UnbindAction(ContextId ctx, ActionId act)
{
    EditResult res;
    if (!IsValidContextId(ctx) || !IsValidActionId(act))
    {
        res.status = EditStatus::InvalidContextOrAction;
        res.message = "Invalid context or action ID";
        return res;
    }

    if (IsProtectedMenuNavigationAction(ctx, act))
    {
        res.status = EditStatus::ProtectedActionCannotUnbind;
        res.message = "Protected navigation action (Menu/Confirm/Back) cannot be unbound; it can only be swapped with another button";
        return res;
    }

    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
                       [&](const BindingEntry& e) { return e.context == ctx && e.action == act; }),
        m_bindings.end());

    res.status = EditStatus::Ok;
    res.message = "Action unbound";
    return res;
}

EditResult BindingProfileModel::SwapBindings(ContextId ctx, ActionId actA, ActionId actB)
{
    EditResult res;
    if (!IsValidContextId(ctx) || !IsValidActionId(actA) || !IsValidActionId(actB))
    {
        res.status = EditStatus::InvalidContextOrAction;
        res.message = "Invalid context or action ID";
        return res;
    }
    if (actA == actB)
    {
        res.status = EditStatus::Ok;
        return res;
    }

    const BindingTrigger* trigA = FindBinding(ctx, actA);
    const BindingTrigger* trigB = FindBinding(ctx, actB);
    if (!trigA || !trigB)
    {
        res.status = EditStatus::ProtectedActionCannotUnbind;
        res.message = "Both actions must have existing bindings to swap";
        return res;
    }

    const BindingTrigger copyA = *trigA;
    const BindingTrigger copyB = *trigB;

    if (IsProtectedMenuNavigationAction(ctx, actA) && !IsValidTriggerForProtectedMenuAction(copyB))
    {
        res.status = EditStatus::ProtectedActionInvalidTrigger;
        res.message = "Target trigger is not valid for protected action";
        return res;
    }
    if (IsProtectedMenuNavigationAction(ctx, actB) && !IsValidTriggerForProtectedMenuAction(copyA))
    {
        res.status = EditStatus::ProtectedActionInvalidTrigger;
        res.message = "Source trigger is not valid for protected action";
        return res;
    }

    std::vector<BindingEntry> candidateBindings = m_bindings;
    for (auto& e : candidateBindings)
    {
        if (e.context == ctx && e.action == actA)
            e.trigger = copyB;
        else if (e.context == ctx && e.action == actB)
            e.trigger = copyA;
    }

    BindingProfileModel tempModel = *this;
    tempModel.m_bindings = candidateBindings;
    std::string navErr;
    if (!tempModel.VerifyMenuNavigationInvariant(&navErr))
    {
        res.status = EditStatus::ProtectedNavigationBroken;
        res.message = navErr;
        return res;
    }

    m_bindings = std::move(candidateBindings);
    res.swapped_with = actB;
    res.status = (IsProtectedMenuNavigationAction(ctx, actA) || IsProtectedMenuNavigationAction(ctx, actB))
                     ? EditStatus::OkWithSwappedProtected
                     : EditStatus::Ok;
    res.message = "Bindings swapped";
    return res;
}

void BindingProfileModel::EnsureProtectedMenuDefaults(bool* outRepaired)
{
    bool repaired = false;
    const ContextId navContexts[] = {ContextId::MenuUI, ContextId::MainMenu};

    for (ContextId ctx : navContexts)
    {
        auto ensureOne = [&](ActionId act, ButtonId defaultBtn, ButtonId fallbackBtn1, ButtonId fallbackBtn2) {
            const BindingTrigger* cur = FindBinding(ctx, act);
            if (!cur || !IsValidTriggerForProtectedMenuAction(*cur))
            {
                repaired = true;
                // Choose a button not currently used by the other two protected actions in ctx
                const ButtonId candidates[] = {defaultBtn, fallbackBtn1, fallbackBtn2};
                ButtonId chosen = defaultBtn;
                for (ButtonId cand : candidates)
                {
                    bool usedByOtherProtected = false;
                    for (const auto& e : m_bindings)
                    {
                        if (e.context == ctx && e.action != act && IsProtectedMenuNavigationAction(ctx, e.action) &&
                            e.trigger.button == cand)
                        {
                            usedByOtherProtected = true;
                            break;
                        }
                    }
                    if (!usedByOtherProtected)
                    {
                        chosen = cand;
                        break;
                    }
                }

                bool foundEntry = false;
                for (auto& e : m_bindings)
                {
                    if (e.context == ctx && e.action == act)
                    {
                        e.trigger = BindingTrigger::MakeButton(chosen);
                        foundEntry = true;
                        break;
                    }
                }
                if (!foundEntry)
                {
                    BindingEntry ne;
                    ne.context = ctx;
                    ne.action = act;
                    ne.trigger = BindingTrigger::MakeButton(chosen);
                    m_bindings.push_back(ne);
                }
            }
        };

        ensureOne(ActionId::Menu, ButtonId::Start, ButtonId::Back, ButtonId::North);
        ensureOne(ActionId::Confirm, ButtonId::South, ButtonId::West, ButtonId::North);
        ensureOne(ActionId::Back, ButtonId::East, ButtonId::North, ButtonId::West);

        // If Confirm and Back ended up on the same button (e.g., from a hand-edited corrupt file), repair Back
        const BindingTrigger* cTrig = FindBinding(ctx, ActionId::Confirm);
        const BindingTrigger* bTrig = FindBinding(ctx, ActionId::Back);
        const BindingTrigger* mTrig = FindBinding(ctx, ActionId::Menu);
        if (cTrig && bTrig && cTrig->button == bTrig->button)
        {
            repaired = true;
            const ButtonId altBtns[] = {ButtonId::East, ButtonId::South, ButtonId::West, ButtonId::North};
            for (ButtonId alt : altBtns)
            {
                if (alt != cTrig->button && (!mTrig || alt != mTrig->button))
                {
                    for (auto& e : m_bindings)
                    {
                        if (e.context == ctx && e.action == ActionId::Back)
                        {
                            e.trigger = BindingTrigger::MakeButton(alt);
                            break;
                        }
                    }
                    break;
                }
            }
        }
        if (mTrig && cTrig && bTrig && (mTrig->button == cTrig->button || mTrig->button == bTrig->button))
        {
            repaired = true;
            const ButtonId altMenuBtns[] = {ButtonId::Start, ButtonId::Back, ButtonId::North, ButtonId::West};
            for (ButtonId alt : altMenuBtns)
            {
                if (alt != cTrig->button && alt != bTrig->button)
                {
                    for (auto& e : m_bindings)
                    {
                        if (e.context == ctx && e.action == ActionId::Menu)
                        {
                            e.trigger = BindingTrigger::MakeButton(alt);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }

    if (outRepaired)
        *outRepaired = repaired;
}

std::string BindingProfileModel::SerializeToText() const
{
    std::ostringstream oss;
    oss << "; G.A.M.M.A. Native Gamepad Profile (D37: appdata\\gamepad\\profile.ltx)\n";
    oss << "; Do not edit user.ltx or axr_options.ltx — gamepad bindings are stored here.\n";
    oss << "[profile_header]\n";
    oss << "schema_version = " << limits::kCurrentProfileSchema << "\n\n";

    for (std::uint8_t c = 0; c < static_cast<std::uint8_t>(ContextId::Count); ++c)
    {
        const ContextId ctx = static_cast<ContextId>(c);
        const auto ctxBindings = GetBindingsForContext(ctx);
        const auto canonicalActions = GetCanonicalActionsForContext(ctx);
        if (ctxBindings.empty() && canonicalActions.empty())
            continue;

        oss << "[context_" << ContextToString(ctx) << "]\n";
        for (const auto& entry : ctxBindings)
        {
            oss << ActionToString(entry.action) << " = " << FormatTriggerSpec(entry.trigger) << "\n";
        }
        // Persist explicitly unbound canonical actions so reload preserves their unbound state
        for (const ActionId canonAct : canonicalActions)
        {
            if (FindBinding(ctx, canonAct) == nullptr)
            {
                oss << ActionToString(canonAct) << " = unbound\n";
            }
        }
        oss << "\n";
    }
    return oss.str();
}

LoadReport BindingProfileModel::DeserializeFromText(const std::string& content)
{
    LoadReport rep;
    rep.source_version = limits::kCurrentProfileSchema;
    rep.target_version = limits::kCurrentProfileSchema;

    // Start from default bindings so any unmentioned contexts/actions keep safe defaults
    ResetToDefaults();

    std::istringstream iss(content);
    std::string line;
    std::string currentSection;
    bool sawHeaderVersion = false;
    std::uint32_t parsedVersion = limits::kCurrentProfileSchema;
    bool sawQuickBoltOrSlot = false;
    int preK2DpadMatches = 0;
    bool sawPreK2WheelRb = false;
    bool sawPreK2GrenadeLb = false;

    while (std::getline(iss, line))
    {
        // Strip comments (; or //)
        std::size_t semiPos = line.find(';');
        const std::size_t slashPos = line.find("//");
        if (slashPos != std::string::npos && (semiPos == std::string::npos || slashPos < semiPos))
            semiPos = slashPos;
        if (semiPos != std::string::npos)
            line = line.substr(0, semiPos);

        line = Trim(line);
        if (line.empty())
            continue;

        if (line.front() == '[')
        {
            const std::size_t closePos = line.find(']');
            if (closePos == std::string::npos || closePos <= 1)
            {
                ++rep.corrupted_lines_skipped;
                continue;
            }
            currentSection = Trim(line.substr(1, closePos - 1));
            continue;
        }

        const std::string lowSec = ToLowerAscii(currentSection);
        // Ignore known foreign sections in shared profile.ltx (D2.2 calibration, D5.2 haptics, D5.1 glyphs)
        if (IsKnownForeignProfileSection(lowSec))
        {
            continue;
        }

        const std::size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            ++rep.corrupted_lines_skipped;
            continue;
        }

        const std::string key = Trim(line.substr(0, eqPos));
        const std::string val = Trim(line.substr(eqPos + 1));
        if (key.empty() || val.empty())
        {
            ++rep.corrupted_lines_skipped;
            continue;
        }

        const std::string lowKey = ToLowerAscii(key);

        if (lowSec == "profile_header" || lowSec == "header" || lowSec == "gamepad_profile" || currentSection.empty())
        {
            if (lowKey == "schema_version" || lowKey == "version" || lowKey == "format_version")
            {
                char* endPtr = nullptr;
                const long v = std::strtol(val.c_str(), &endPtr, 10);
                if (endPtr == val.c_str() || v <= 0 || v > 99)
                {
                    ++rep.corrupted_lines_skipped;
                }
                else
                {
                    parsedVersion = static_cast<std::uint32_t>(v);
                    sawHeaderVersion = true;
                    rep.source_version = parsedVersion;
                }
                continue;
            }
        }

        // Determine target ContextId (supports v2 "[context_<Name>]" and v1 legacy section names)
        ContextId ctx = ContextId::Invalid;
        bool migratedLine = false;
        if (lowSec.rfind("context_", 0) == 0)
        {
            ctx = StringToContext(currentSection.substr(8));
        }
        else
        {
            ctx = StringToContext(currentSection);
            if (ctx == ContextId::Invalid)
            {
                ctx = MapLegacyV1ContextName(currentSection);
                if (ctx != ContextId::Invalid)
                    migratedLine = true;
            }
        }

        if (ctx == ContextId::Invalid)
        {
            ++rep.corrupted_lines_skipped;
            continue;
        }

        ActionId act = StringToAction(key);
        if (act == ActionId::Invalid)
        {
            act = MapLegacyV1ActionName(key);
            if (act != ActionId::Invalid)
                migratedLine = true;
        }
        if (act == ActionId::Invalid)
        {
            ++rep.corrupted_lines_skipped;
            continue;
        }

        const std::string lowVal = ToLowerAscii(val);
        if (lowVal == "unbound" || lowVal == "none")
        {
            const EditResult uer = UnbindAction(ctx, act);
            if (uer.Succeeded())
            {
                ++rep.loaded_bindings;
            }
            else
            {
                ++rep.corrupted_lines_skipped;
            }
            continue;
        }

        BindingTrigger trig;
        std::uint32_t sanCount = 0;
        if (!TryParseTriggerSpec(val, trig, &sanCount))
        {
            ++rep.corrupted_lines_skipped;
            continue;
        }
        rep.sanitized_fields += sanCount;

        // Detect legacy v1 syntax ("hold:B" without explicit ms, or "chord:LT+X" with '+' separator)
        if (parsedVersion == limits::kProfileSchemaV1 || val.find('+') != std::string::npos)
            migratedLine = true;

        if (ctx == ContextId::World_Gameplay)
        {
            if (act == ActionId::QuickBoltOrSlot)
                sawQuickBoltOrSlot = true;
            else if (act == ActionId::Pda3DToggle && trig == BindingTrigger::MakeButton(ButtonId::DPadRight))
                ++preK2DpadMatches;
            else if (act == ActionId::MaskClean && trig == BindingTrigger::MakeButton(ButtonId::DPadDown))
                ++preK2DpadMatches;
            else if (act == ActionId::UnderbarrelToggle && trig == BindingTrigger::MakeHold(ButtonId::DPadDown, 300))
                ++preK2DpadMatches;
            else if (act == ActionId::FireModeNext && trig == BindingTrigger::MakeHold(ButtonId::DPadLeft, 300))
                ++preK2DpadMatches;
            else if (act == ActionId::WeaponWheel && trig == BindingTrigger::MakeHold(ButtonId::RightShoulder, 220))
                sawPreK2WheelRb = true;
            else if (act == ActionId::GrenadeQuick && trig == BindingTrigger::MakeButton(ButtonId::LeftShoulder))
                sawPreK2GrenadeLb = true;
        }

        // Apply binding with conflict resolution enabled so loaded user bindings override defaults cleanly
        const EditResult er = SetBinding(ctx, act, trig, true);
        if (er.Succeeded())
        {
            ++rep.loaded_bindings;
            if (migratedLine || parsedVersion < limits::kCurrentProfileSchema)
                ++rep.migrated_entries;
        }
        else
        {
            ++rep.corrupted_lines_skipped;
        }
    }

    // K2 migration: if a pre-K2 profile.ltx (without QuickBoltOrSlot) had the old uncalibrated D4.1 DPad defaults,
    // upgrade those unmodified DPad / LB-RB bindings to match live gameplay (3D-PDA on DPadDown, etc.).
    if (!sawQuickBoltOrSlot && preK2DpadMatches == 4)
    {
        SetBinding(ContextId::World_Gameplay, ActionId::Pda3DToggle, BindingTrigger::MakeButton(ButtonId::DPadDown), true);
        SetBinding(ContextId::World_Gameplay, ActionId::MaskClean, BindingTrigger::MakeHold(ButtonId::DPadDown, 300), true);
        SetBinding(ContextId::World_Gameplay, ActionId::UnderbarrelToggle, BindingTrigger::MakeHold(ButtonId::DPadLeft, 300), true);
        SetBinding(ContextId::World_Gameplay, ActionId::FireModeNext, BindingTrigger::MakeButton(ButtonId::DPadRight), true);
        if (sawPreK2WheelRb && sawPreK2GrenadeLb)
        {
            SetBinding(ContextId::World_Gameplay, ActionId::GrenadeQuick, BindingTrigger::MakeButton(ButtonId::RightShoulder), true);
            SetBinding(ContextId::World_Gameplay, ActionId::QuickBoltOrSlot, BindingTrigger::MakeButton(ButtonId::LeftShoulder), true);
            SetBinding(ContextId::World_Gameplay, ActionId::WeaponWheel, BindingTrigger::MakeHold(ButtonId::LeftShoulder, 250), true);
        }
    }

    // Ensure protected MenuUI / MainMenu navigation actions are valid even if the file was truncated or corrupted
    bool repairedNav = false;
    // FIX-LIVE3 п.18: старые профили хранят прежние сочетания по умолчанию LB + R3 (маска) / LB + D-pad вправо (ЛЦУ),
    // которые теперь заняты слотами крестовины — снимаем ровно эти сочетания.
    {
        const BindingTrigger* m = FindBinding(ContextId::World_Gameplay, ActionId::WheelMaskClean);
        if (m && *m == BindingTrigger::MakeChord(ButtonId::LeftShoulder, ButtonId::RightStick))
            UnbindAction(ContextId::World_Gameplay, ActionId::WheelMaskClean);
        const BindingTrigger* l = FindBinding(ContextId::World_Gameplay, ActionId::WheelLaser);
        if (l && *l == BindingTrigger::MakeChord(ButtonId::LeftShoulder, ButtonId::DPadRight))
            UnbindAction(ContextId::World_Gameplay, ActionId::WheelLaser);
    }
    EnsureProtectedMenuDefaults(&repairedNav);
    rep.protected_menu_repaired = repairedNav;

    m_schemaVersion = limits::kCurrentProfileSchema;

    if (!sawHeaderVersion && rep.loaded_bindings == 0 && rep.corrupted_lines_skipped > 0)
    {
        rep.status = LoadStatus::RecoveredFromCorrupt;
        rep.detail = "Input was completely corrupt; restored safe default bindings";
    }
    else if (rep.corrupted_lines_skipped > 0 || rep.sanitized_fields > 0 || rep.protected_menu_repaired)
    {
        rep.status = LoadStatus::RecoveredFromCorrupt;
        rep.detail = "Sanitized corrupt/out-of-range entries and verified protected MenuUI navigation";
    }
    else if (sawHeaderVersion && parsedVersion < limits::kCurrentProfileSchema)
    {
        rep.status = LoadStatus::MigratedV1ToV2;
        rep.detail = "Migrated profile schema v1 -> v2";
    }
    else
    {
        rep.status = LoadStatus::Ok;
        rep.detail = "Profile loaded cleanly";
    }

    return rep;
}

bool BindingProfileModel::IsPathForbiddenForWrite(const std::string& path, std::string* outReason)
{
    if (path.empty())
    {
        if (outReason)
            *outReason = "Empty file path";
        return true;
    }

    std::string norm = ToLowerAscii(path);
    std::replace(norm.begin(), norm.end(), '/', '\\');

    // D4.1 stop condition & AGENTS.md: never touch D:\Stalker, user.ltx, or axr_options.ltx
    if (norm.find("d:\\stalker") != std::string::npos)
    {
        if (outReason)
            *outReason = "Writing inside production D:\\Stalker is strictly forbidden";
        return true;
    }

    const std::size_t lastSlash = norm.find_last_of('\\');
    const std::string baseName = (lastSlash == std::string::npos) ? norm : norm.substr(lastSlash + 1);
    if (baseName == "user.ltx" || baseName == "axr_options.ltx" || baseName == "gamecontrollerdb.txt")
    {
        if (outReason)
            *outReason = "Gamepad profile must not write to " + baseName + " (decision D37)";
        return true;
    }

    return false;
}

std::string BindingProfileModel::GetDefaultProfileRelativePath()
{
    return "gamepad\\profile.ltx";
}

bool BindingProfileModel::SaveToFile(const std::string& filePath, std::string* outError) const
{
    std::string reason;
    if (IsPathForbiddenForWrite(filePath, &reason))
    {
        if (outError)
            *outError = reason;
        return false;
    }

    try
    {
        const std::filesystem::path p(filePath);
        if (p.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }

        // Preserve any non-binding sections already present in filePath (D2.2 calibration, D5.2 haptics, D5.1 glyphs)
        std::string preservedForeignSections;
        {
            std::ifstream existingIfs(filePath, std::ios::binary);
            if (existingIfs)
            {
                std::string rawLine;
                bool inForeignSection = false;
                while (std::getline(existingIfs, rawLine))
                {
                    if (!rawLine.empty() && rawLine.back() == '\r')
                        rawLine.pop_back();
                    const std::string trimmed = Trim(rawLine);
                    if (!trimmed.empty() && trimmed.front() == '[')
                    {
                        const std::size_t closePos = trimmed.find(']');
                        if (closePos != std::string::npos && closePos > 1)
                        {
                            const std::string secName = Trim(trimmed.substr(1, closePos - 1));
                            inForeignSection = !IsOurBindingSection(secName);
                            if (inForeignSection && !preservedForeignSections.empty())
                                preservedForeignSections += "\n";
                        }
                    }
                    if (inForeignSection)
                    {
                        preservedForeignSections += rawLine;
                        preservedForeignSections += "\n";
                    }
                }
            }
        }

        {
            std::error_code dirEc;
            const std::filesystem::path parentDir = std::filesystem::path(filePath).parent_path();
            if (!parentDir.empty())
                std::filesystem::create_directories(parentDir, dirEc);
        }

        const std::string tmpPath = GpTempPathFor(filePath.c_str());
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs)
            {
                if (outError)
                    *outError = "Failed to open temp file for writing: " + tmpPath;
                return false;
            }
            const std::string text = SerializeToText();
            ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!preservedForeignSections.empty())
            {
                ofs.write(preservedForeignSections.data(), static_cast<std::streamsize>(preservedForeignSections.size()));
            }
            ofs.flush();
            if (!ofs.good())
            {
                ofs.close();
#ifdef _WIN32
                DeleteFileA(tmpPath.c_str());
#else
                std::remove(tmpPath.c_str());
#endif
                if (outError)
                    *outError = "I/O error while writing profile: " + tmpPath;
                return false;
            }
        }

        if (!GpCommitTempFile(tmpPath.c_str(), filePath.c_str()))
        {
            if (outError)
                *outError = "Failed to atomically replace profile: " + filePath;
            return false;
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        if (outError)
            *outError = ex.what();
        return false;
    }
}

LoadReport BindingProfileModel::LoadFromFile(const std::string& filePath)
{
    LoadReport rep;
    std::string reason;
    if (IsPathForbiddenForWrite(filePath, &reason))
    {
        rep.status = LoadStatus::ForbiddenTargetPath;
        rep.detail = reason;
        return rep;
    }

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs)
    {
        ResetToDefaults();
        rep.status = LoadStatus::FileNotFoundUsedDefaults;
        rep.detail = "File not found; initialized with default bindings";
        return rep;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    rep = DeserializeFromText(oss.str());
    return rep;
}

bool BindingProfileModel::ExportToFile(const std::string& exportFilePath, std::string* outError) const
{
    return SaveToFile(exportFilePath, outError);
}

LoadReport BindingProfileModel::ImportFromFile(const std::string& importFilePath,
                                               const std::string& activeProfileSavePath)
{
    LoadReport rep = LoadFromFile(importFilePath);
    if (rep.status != LoadStatus::ForbiddenTargetPath && rep.status != LoadStatus::FileNotFoundUsedDefaults &&
        rep.status != LoadStatus::IoError)
    {
        if (!activeProfileSavePath.empty())
        {
            std::string saveErr;
            if (!SaveToFile(activeProfileSavePath, &saveErr))
            {
                rep.status = LoadStatus::IoError;
                rep.detail = "Imported bindings but failed to persist active profile: " + saveErr;
            }
        }
    }
    return rep;
}

bool BindingProfileModel::UpdateEmergencyResetHold(std::uint32_t rawButtonsMask,
                                                   std::uint32_t dtContinualMs,
                                                   const std::string& activeProfileSavePath)
{
    const std::uint32_t startBit = (1u << static_cast<std::uint32_t>(ButtonId::Start));
    const std::uint32_t backBit = (1u << static_cast<std::uint32_t>(ButtonId::Back));
    const std::uint32_t comboMask = startBit | backBit;

    const bool bothHeld = ((rawButtonsMask & comboMask) == comboMask);
    if (!bothHeld)
    {
        m_emergencyHoldElapsedMs = 0;
        m_emergencyResetLatchedUntilRelease = false;
        return false;
    }

    if (m_emergencyResetLatchedUntilRelease)
        return false;

    m_emergencyHoldElapsedMs += dtContinualMs;
    if (m_emergencyHoldElapsedMs >= limits::kEmergencyResetHoldMs)
    {
        m_emergencyResetLatchedUntilRelease = true;
        ExecuteEmergencyReset("start_plus_back_5s", activeProfileSavePath);
        return true;
    }
    return false;
}

void BindingProfileModel::ExecuteEmergencyReset(const char* reason, const std::string& activeProfileSavePath)
{
    ResetToDefaults();
    ++m_emergencyResetCount;
    m_lastEmergencyResetReason = (reason && reason[0]) ? reason : "gp_bind_reset";
    if (!activeProfileSavePath.empty())
    {
        SaveToFile(activeProfileSavePath, nullptr);
    }
}

void BindingProfileModel::ResetRuntimeGestureState()
{
    m_runtimePrevCtx = ContextId::Invalid;
    m_runtimePrevDownMask = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(ButtonId::Count); ++i)
    {
        m_runtimeHoldElapsedMs[i] = 0;
        m_runtimeHoldFired[i] = false;
        m_runtimeUsedInChordOrMod[i] = false;
    }
}

std::vector<GestureEvent> BindingProfileModel::EvaluateContextGestures(ContextId ctx,
                                                                       std::uint32_t effectiveDownMask,
                                                                       std::uint32_t dtContinualMs)
{
    std::vector<GestureEvent> events;
    if (!IsValidContextId(ctx))
        return events;

    if (ctx != m_runtimePrevCtx)
    {
        ResetRuntimeGestureState();
        m_runtimePrevCtx = ctx;
    }

    const std::uint32_t pressedMask = effectiveDownMask & ~m_runtimePrevDownMask;
    const std::uint32_t releasedMask = ~effectiveDownMask & m_runtimePrevDownMask;

    // 1. Evaluate Chord bindings first when primary button is just pressed while modifier is held
    for (const auto& entry : m_bindings)
    {
        if (entry.context != ctx || entry.trigger.type != TriggerType::Chord)
            continue;
        const std::uint32_t modBit = (1u << static_cast<std::uint32_t>(entry.trigger.modifier));
        const std::uint32_t btnBit = (1u << static_cast<std::uint32_t>(entry.trigger.button));
        if ((effectiveDownMask & modBit) != 0 && (pressedMask & btnBit) != 0)
        {
            GestureEvent ev;
            ev.context = ctx;
            ev.action = entry.action;
            ev.trigger_type = TriggerType::Chord;
            events.push_back(ev);

            m_runtimeUsedInChordOrMod[static_cast<std::size_t>(entry.trigger.modifier)] = true;
            m_runtimeUsedInChordOrMod[static_cast<std::size_t>(entry.trigger.button)] = true;
        }
    }

    // 2. Update per-button hold timers and evaluate Hold bindings vs Single Button bindings
    for (std::uint8_t b = 0; b < static_cast<std::uint8_t>(ButtonId::Count); ++b)
    {
        const ButtonId btnId = static_cast<ButtonId>(b);
        const std::uint32_t bit = (1u << b);
        const bool isDown = (effectiveDownMask & bit) != 0;
        const bool justPressed = (pressedMask & bit) != 0;
        const bool justReleased = (releasedMask & bit) != 0;

        // Check if this button has any Hold binding in ctx
        const BindingEntry* holdEntry = nullptr;
        const BindingEntry* buttonEntry = nullptr;
        for (const auto& entry : m_bindings)
        {
            if (entry.context != ctx || entry.trigger.button != btnId)
                continue;
            if (entry.trigger.type == TriggerType::Hold)
                holdEntry = &entry;
            else if (entry.trigger.type == TriggerType::Button)
                buttonEntry = &entry;
        }

        if (justPressed)
        {
            m_runtimeHoldElapsedMs[b] = 0;
            m_runtimeHoldFired[b] = false;
            // If no Hold binding shares this button and it wasn't just consumed by a Chord, fire Button immediately on press
            if (buttonEntry && !holdEntry && !m_runtimeUsedInChordOrMod[b])
            {
                GestureEvent ev;
                ev.context = ctx;
                ev.action = buttonEntry->action;
                ev.trigger_type = TriggerType::Button;
                events.push_back(ev);
            }
        }

        if (isDown)
        {
            m_runtimeHoldElapsedMs[b] += dtContinualMs;
            if (holdEntry && !m_runtimeHoldFired[b] && !m_runtimeUsedInChordOrMod[b])
            {
                const std::uint32_t threshold =
                    (holdEntry->trigger.hold_ms > 0) ? holdEntry->trigger.hold_ms : limits::kDefaultHoldMs;
                if (m_runtimeHoldElapsedMs[b] >= threshold)
                {
                    m_runtimeHoldFired[b] = true;
                    GestureEvent ev;
                    ev.context = ctx;
                    ev.action = holdEntry->action;
                    ev.trigger_type = TriggerType::Hold;
                    events.push_back(ev);
                }
            }
        }

        if (justReleased)
        {
            // If both Button and Hold share this button (e.g. Tap B = Crouch, Hold B = LowCrouch),
            // fire the tap Button action on release if the Hold threshold was not reached and no Chord used it.
            if (buttonEntry && holdEntry && !m_runtimeHoldFired[b] && !m_runtimeUsedInChordOrMod[b])
            {
                GestureEvent ev;
                ev.context = ctx;
                ev.action = buttonEntry->action;
                ev.trigger_type = TriggerType::Button;
                events.push_back(ev);
            }
            m_runtimeHoldElapsedMs[b] = 0;
            m_runtimeHoldFired[b] = false;
            m_runtimeUsedInChordOrMod[b] = false;
        }
    }

    m_runtimePrevDownMask = effectiveDownMask;
    return events;
}

const char* BindingProfileModel::ButtonToString(ButtonId btn)
{
    switch (btn)
    {
    case ButtonId::South: return "A";
    case ButtonId::East: return "B";
    case ButtonId::West: return "X";
    case ButtonId::North: return "Y";
    case ButtonId::Back: return "Back";
    case ButtonId::Guide: return "Guide";
    case ButtonId::Start: return "Start";
    case ButtonId::LeftStick: return "L3";
    case ButtonId::RightStick: return "R3";
    case ButtonId::LeftShoulder: return "LB";
    case ButtonId::RightShoulder: return "RB";
    case ButtonId::DPadUp: return "DPadUp";
    case ButtonId::DPadDown: return "DPadDown";
    case ButtonId::DPadLeft: return "DPadLeft";
    case ButtonId::DPadRight: return "DPadRight";
    case ButtonId::LT: return "LT";
    case ButtonId::RT: return "RT";
    default: return "None";
    }
}

ButtonId BindingProfileModel::StringToButton(const std::string& raw)
{
    const std::string s = ToLowerAscii(Trim(raw));
    if (s == "a" || s == "south" || s == "cross") return ButtonId::South;
    if (s == "b" || s == "east" || s == "circle") return ButtonId::East;
    if (s == "x" || s == "west" || s == "square") return ButtonId::West;
    if (s == "y" || s == "north" || s == "triangle") return ButtonId::North;
    if (s == "back" || s == "view" || s == "select" || s == "share") return ButtonId::Back;
    if (s == "guide" || s == "home") return ButtonId::Guide;
    if (s == "start" || s == "menu" || s == "options") return ButtonId::Start;
    if (s == "l3" || s == "leftstick" || s == "ls") return ButtonId::LeftStick;
    if (s == "r3" || s == "rightstick" || s == "rs") return ButtonId::RightStick;
    if (s == "lb" || s == "l1" || s == "leftshoulder") return ButtonId::LeftShoulder;
    if (s == "rb" || s == "r1" || s == "rightshoulder") return ButtonId::RightShoulder;
    if (s == "dpadup" || s == "dpad_up" || s == "up") return ButtonId::DPadUp;
    if (s == "dpaddown" || s == "dpad_down" || s == "down") return ButtonId::DPadDown;
    if (s == "dpadleft" || s == "dpad_left" || s == "left") return ButtonId::DPadLeft;
    if (s == "dpadright" || s == "dpad_right" || s == "right") return ButtonId::DPadRight;
    if (s == "lt" || s == "l2" || s == "lefttrigger") return ButtonId::LT;
    if (s == "rt" || s == "r2" || s == "righttrigger") return ButtonId::RT;
    return ButtonId::None;
}

const char* BindingProfileModel::ContextToString(ContextId ctx)
{
    switch (ctx)
    {
    case ContextId::Console: return "Console";
    case ContextId::TextEntry: return "TextEntry";
    case ContextId::MainMenu: return "MainMenu";
    case ContextId::Cutscene_InputBlock: return "Cutscene_InputBlock";
    case ContextId::Sleep: return "Sleep";
    case ContextId::Dialog_NPC: return "Dialog_NPC";
    case ContextId::RadialWheel: return "RadialWheel";
    case ContextId::Placement_HideoutFurniture: return "Placement_HideoutFurniture";
    case ContextId::World_3DPDA: return "World_3DPDA";
    case ContextId::MenuUI: return "MenuUI";
    case ContextId::World_VehicleOrHolder: return "World_VehicleOrHolder";
    case ContextId::World_Gameplay: return "World_Gameplay";
    default: return "Invalid";
    }
}

ContextId BindingProfileModel::StringToContext(const std::string& raw)
{
    const std::string s = ToLowerAscii(Trim(raw));
    if (s == "console") return ContextId::Console;
    if (s == "textentry" || s == "text_entry") return ContextId::TextEntry;
    if (s == "mainmenu" || s == "main_menu") return ContextId::MainMenu;
    if (s == "cutscene_inputblock" || s == "cutscene") return ContextId::Cutscene_InputBlock;
    if (s == "sleep") return ContextId::Sleep;
    if (s == "dialog_npc") return ContextId::Dialog_NPC;
    if (s == "radialwheel" || s == "radial_wheel") return ContextId::RadialWheel;
    if (s == "placement_hideoutfurniture" || s == "placement_hideout_furniture") return ContextId::Placement_HideoutFurniture;
    if (s == "world_3dpda" || s == "world_3d_pda") return ContextId::World_3DPDA;
    if (s == "menuui" || s == "menu_ui" || s == "menuui_modal" || s == "menu_ui_modal") return ContextId::MenuUI;
    if (s == "world_vehicleorholder" || s == "world_vehicle_or_holder") return ContextId::World_VehicleOrHolder;
    if (s == "world_gameplay") return ContextId::World_Gameplay;
    return ContextId::Invalid;
}

const char* BindingProfileModel::ActionToString(ActionId act)
{
    switch (act)
    {
    case ActionId::Menu: return "Menu";
    case ActionId::Confirm: return "Confirm";
    case ActionId::Back: return "Back";
    case ActionId::ContextMenu: return "ContextMenu";
    case ActionId::QuickAction: return "QuickAction";
    case ActionId::TabPrev: return "TabPrev";
    case ActionId::TabNext: return "TabNext";
    case ActionId::PagePrev: return "PagePrev";
    case ActionId::PageNext: return "PageNext";
    case ActionId::Jump: return "Jump";
    case ActionId::Crouch: return "Crouch";
    case ActionId::LowCrouch: return "LowCrouch";
    case ActionId::Sprint: return "Sprint";
    case ActionId::Freelook: return "Freelook";
    case ActionId::MeleeQuick: return "MeleeQuick";
    case ActionId::Fire: return "Fire";
    case ActionId::Aim: return "Aim";
    case ActionId::ContextUseReload: return "ContextUseReload";
    case ActionId::AmmoCheck: return "AmmoCheck";
    case ActionId::FastReload: return "FastReload";
    case ActionId::NextWeapon: return "NextWeapon";
    case ActionId::WeaponWheel: return "WeaponWheel";
    case ActionId::GrenadeQuick: return "GrenadeQuick";
    case ActionId::LeanLeft: return "LeanLeft";
    case ActionId::LeanRight: return "LeanRight";
    case ActionId::TorchToggle: return "TorchToggle";
    case ActionId::NightVision: return "NightVision";
    case ActionId::DetectorToggle: return "DetectorToggle";
    case ActionId::Pda3DToggle: return "Pda3DToggle";
    case ActionId::MaskClean: return "MaskClean";
    case ActionId::UnderbarrelToggle: return "UnderbarrelToggle";
    case ActionId::FireModeNext: return "FireModeNext";
    case ActionId::AmmoTypeNext: return "AmmoTypeNext";
    case ActionId::PauseMenu: return "PauseMenu";
    case ActionId::PdaFullToggle: return "PdaFullToggle";
    case ActionId::DialogSelect: return "DialogSelect";
    case ActionId::DialogExit: return "DialogExit";
    case ActionId::WheelConfirm: return "WheelConfirm";
    case ActionId::WheelCancel: return "WheelCancel";
    case ActionId::WheelTabPrev: return "WheelTabPrev";
    case ActionId::WheelTabNext: return "WheelTabNext";
    case ActionId::PdaInteractiveToggle: return "PdaInteractiveToggle";
    case ActionId::PdaClose: return "PdaClose";
    case ActionId::PlacementConfirm: return "PlacementConfirm";
    case ActionId::PlacementCancel: return "PlacementCancel";
    case ActionId::PlacementRotate: return "PlacementRotate";
    case ActionId::QuickBoltOrSlot: return "QuickBoltOrSlot";
    case ActionId::SwitchPanel: return "SwitchPanel";
    case ActionId::CancelQueue: return "CancelQueue";
    case ActionId::KnifeDraw: return "KnifeDraw";
    case ActionId::WeaponLower: return "WeaponLower";
    case ActionId::PdaFlat: return "PdaFlat";
    case ActionId::AmmoWheel: return "AmmoWheel";
    case ActionId::BoltThrow: return "BoltThrow";
    case ActionId::WeaponSelect: return "WeaponSelect";
    case ActionId::ForceReload: return "ForceReload";
    case ActionId::TutorialPrompt: return "TutorialPrompt";
    case ActionId::MouseCursor: return "MouseCursor";
    case ActionId::WheelMaskClean: return "WheelMaskClean";
    case ActionId::WheelLaser: return "WheelLaser";
    default: return "Invalid";
    }
}

ActionId BindingProfileModel::StringToAction(const std::string& raw)
{
    const std::string s = ToLowerAscii(Trim(raw));
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(ActionId::Count); ++i)
    {
        const ActionId candidate = static_cast<ActionId>(i);
        if (ToLowerAscii(ActionToString(candidate)) == s)
            return candidate;
    }
    if (s == "panelswitch" || s == "panel_switch" || s == "switch_panel")
        return ActionId::SwitchPanel;
    if (s == "cancel_queue" || s == "queue_cancel")
        return ActionId::CancelQueue;
    if (s == "quick_bolt_or_slot" || s == "quickbolend" || s == "bolt")
        return ActionId::QuickBoltOrSlot;
    if (s == "mouse_cursor")
        return ActionId::MouseCursor;
    return ActionId::Invalid;
}

const char* BindingProfileModel::TriggerTypeToString(TriggerType t)
{
    switch (t)
    {
    case TriggerType::Button: return "button";
    case TriggerType::Hold: return "hold";
    case TriggerType::Chord: return "chord";
    default: return "unknown";
    }
}

std::string BindingProfileModel::FormatTriggerSpec(const BindingTrigger& trig)
{
    std::ostringstream oss;
    if (trig.type == TriggerType::Button)
    {
        oss << "button:" << ButtonToString(trig.button);
    }
    else if (trig.type == TriggerType::Hold)
    {
        oss << "hold:" << ButtonToString(trig.button) << ":" << trig.hold_ms;
    }
    else if (trig.type == TriggerType::Chord)
    {
        oss << "chord:" << ButtonToString(trig.modifier) << "+" << ButtonToString(trig.button);
    }
    return oss.str();
}

bool BindingProfileModel::TryParseTriggerSpec(const std::string& spec,
                                              BindingTrigger& outTrig,
                                              std::uint32_t* outSanitized)
{
    const std::string s = Trim(spec);
    if (s.empty())
        return false;

    const std::string low = ToLowerAscii(s);

    // 1. "button:<Btn>" or bare "<Btn>" (v1 shorthand)
    if (low.rfind("button:", 0) == 0 || low.rfind("press:", 0) == 0 || low.rfind("tap:", 0) == 0)
    {
        const std::size_t colonPos = s.find(':');
        const ButtonId btn = StringToButton(s.substr(colonPos + 1));
        if (btn == ButtonId::None)
            return false;
        outTrig = BindingTrigger::MakeButton(btn);
        return SanitizeTrigger(outTrig, outSanitized);
    }

    // 2. "hold:<Btn>:<ms>" or v1 "hold:<Btn>" (defaults to 300 ms)
    if (low.rfind("hold:", 0) == 0)
    {
        const std::string rest = s.substr(5);
        const std::size_t secondColon = rest.find(':');
        if (secondColon == std::string::npos)
        {
            const ButtonId btn = StringToButton(rest);
            if (btn == ButtonId::None)
                return false;
            outTrig = BindingTrigger::MakeHold(btn, limits::kDefaultHoldMs);
            return SanitizeTrigger(outTrig, outSanitized);
        }

        const ButtonId btn = StringToButton(rest.substr(0, secondColon));
        if (btn == ButtonId::None)
            return false;

        const std::string msStr = Trim(rest.substr(secondColon + 1));
        char* endPtr = nullptr;
        const long msVal = std::strtol(msStr.c_str(), &endPtr, 10);
        if (endPtr == msStr.c_str())
        {
            // Non-numeric hold_ms in corrupt file -> sanitize to kDefaultHoldMs
            outTrig.type = TriggerType::Hold;
            outTrig.button = btn;
            outTrig.modifier = ButtonId::None;
            outTrig.hold_ms = 0; // SanitizeTrigger will set kDefaultHoldMs and increment outSanitized
            return SanitizeTrigger(outTrig, outSanitized);
        }

        outTrig.type = TriggerType::Hold;
        outTrig.button = btn;
        outTrig.modifier = ButtonId::None;
        if (msVal < static_cast<long>(limits::kMinHoldMs))
        {
            outTrig.hold_ms = limits::kMinHoldMs;
            if (outSanitized)
                ++(*outSanitized);
        }
        else if (msVal > static_cast<long>(limits::kMaxHoldMs))
        {
            outTrig.hold_ms = limits::kMaxHoldMs;
            if (outSanitized)
                ++(*outSanitized);
        }
        else
        {
            outTrig.hold_ms = static_cast<std::uint16_t>(msVal);
        }
        return SanitizeTrigger(outTrig, outSanitized);
    }

    // 3. "chord:<Mod>+<Btn>" or "chord:<Mod>:<Btn>" or bare "<Mod>+<Btn>" (v1 shorthand)
    if (low.rfind("chord:", 0) == 0 || s.find('+') != std::string::npos)
    {
        const std::string rest = (low.rfind("chord:", 0) == 0) ? s.substr(6) : s;
        std::size_t sep = rest.find('+');
        if (sep == std::string::npos)
            sep = rest.find(':');
        if (sep == std::string::npos)
            return false;

        const ButtonId modBtn = StringToButton(rest.substr(0, sep));
        const ButtonId priBtn = StringToButton(rest.substr(sep + 1));
        if (modBtn == ButtonId::None || priBtn == ButtonId::None)
            return false;
        outTrig = BindingTrigger::MakeChord(modBtn, priBtn);
        return SanitizeTrigger(outTrig, outSanitized);
    }

    // 4. Bare button name (v1 shorthand, e.g. "A" or "RT")
    const ButtonId bareBtn = StringToButton(s);
    if (bareBtn != ButtonId::None)
    {
        outTrig = BindingTrigger::MakeButton(bareBtn);
        return SanitizeTrigger(outTrig, outSanitized);
    }

    return false;
}

namespace
{
FBindingTranslateCallback g_bindingTranslateCb = nullptr;

const char* TrBind(const char* stringId, const char* fallbackText)
{
    if (g_bindingTranslateCb)
    {
        const char* res = g_bindingTranslateCb(stringId, fallbackText);
        if (res && res[0])
            return res;
    }
    return fallbackText;
}
} // namespace

ENGINE_API void SetBindingTranslateCallback(FBindingTranslateCallback cb)
{
    g_bindingTranslateCb = cb;
}

const char* BindingProfileModel::ContextDisplayTitle(ContextId ctx)
{
    switch (ctx)
    {
    case ContextId::World_Gameplay: return TrBind("ui_gp_eng_ctx_world_gameplay", "World / Gameplay");
    case ContextId::MenuUI: return TrBind("ui_gp_eng_ctx_menu_ui", "Menu / Inventory / UI");
    case ContextId::MainMenu: return TrBind("ui_gp_eng_ctx_main_menu", "Main Menu");
    case ContextId::Dialog_NPC: return TrBind("ui_gp_eng_ctx_dialog_npc", "NPC Dialog");
    case ContextId::RadialWheel: return TrBind("ui_gp_eng_ctx_radial_wheel", "Radial Wheel");
    case ContextId::World_3DPDA: return TrBind("ui_gp_eng_ctx_world_3dpda", "3D PDA");
    case ContextId::Placement_HideoutFurniture: return TrBind("ui_gp_eng_ctx_placement", "Hideout Placement");
    case ContextId::World_VehicleOrHolder: return TrBind("ui_gp_eng_ctx_vehicle", "Vehicle / Holder");
    case ContextId::Console: return TrBind("ui_gp_eng_ctx_console", "Console");
    case ContextId::TextEntry: return TrBind("ui_gp_eng_ctx_text_entry", "Text Entry");
    case ContextId::Cutscene_InputBlock: return TrBind("ui_gp_eng_ctx_cutscene", "Cutscene");
    case ContextId::Sleep: return TrBind("ui_gp_eng_ctx_sleep", "Sleep");
    default: return TrBind("ui_gp_eng_ctx_unknown", "Unknown");
    }
}

const char* BindingProfileModel::ActionDisplayTitle(ActionId act)
{
    switch (act)
    {
    case ActionId::Menu: return TrBind("ui_gp_eng_act_menu", "Pause / Menu [PROTECTED]");
    case ActionId::Confirm: return TrBind("ui_gp_eng_act_confirm", "Confirm / Select [PROTECTED]");
    case ActionId::Back: return TrBind("ui_gp_eng_act_back", "Back / Close [PROTECTED]");
    case ActionId::ContextMenu: return TrBind("ui_gp_eng_act_context_menu", "Item Context Menu");
    case ActionId::QuickAction: return TrBind("ui_gp_eng_act_quick_action", "Quick Action / Transfer");
    case ActionId::TabPrev: return TrBind("ui_gp_eng_act_tab_prev", "Previous Tab (LB)");
    case ActionId::TabNext: return TrBind("ui_gp_eng_act_tab_next", "Next Tab (RB)");
    case ActionId::PagePrev: return TrBind("ui_gp_eng_act_page_prev", "Previous Page / Sub-tab");
    case ActionId::PageNext: return TrBind("ui_gp_eng_act_page_next", "Next Page / Sub-tab");
    case ActionId::Jump: return TrBind("ui_gp_eng_act_jump", "Jump / Climb");
    case ActionId::Crouch: return TrBind("ui_gp_eng_act_crouch", "Crouch (Toggle)");
    case ActionId::LowCrouch: return TrBind("ui_gp_eng_act_low_crouch", "Low Crouch");
    case ActionId::Sprint: return TrBind("ui_gp_eng_act_sprint", "Sprint");
    case ActionId::Freelook: return TrBind("ui_gp_eng_act_freelook", "Freelook");
    case ActionId::MeleeQuick: return TrBind("ui_gp_eng_act_melee_quick", "Quick Melee");
    case ActionId::Fire: return TrBind("ui_gp_eng_act_fire", "Fire");
    case ActionId::Aim: return TrBind("ui_gp_eng_act_aim", "Aim Down Sights (ADS)");
    case ActionId::ContextUseReload: return TrBind("ui_gp_eng_act_context_use_reload", "Interact / Reload / Unjam");
    case ActionId::AmmoCheck: return TrBind("ui_gp_eng_act_ammo_check", "Check Magazine");
    case ActionId::FastReload: return TrBind("ui_gp_eng_act_fast_reload", "Tactical Fast Reload");
    case ActionId::NextWeapon: return TrBind("ui_gp_eng_act_next_weapon", "Swap Weapon");
    case ActionId::WeaponWheel: return TrBind("ui_gp_eng_act_weapon_wheel", "Weapon / Quick Wheel");
    case ActionId::GrenadeQuick: return TrBind("ui_gp_eng_act_grenade_quick", "Quick Grenade");
    case ActionId::LeanLeft: return TrBind("ui_gp_eng_act_lean_left", "Lean Left (in ADS)");
    case ActionId::LeanRight: return TrBind("ui_gp_eng_act_lean_right", "Lean Right (in ADS)");
    case ActionId::TorchToggle: return TrBind("ui_gp_eng_act_torch_toggle", "Headlamp / Torch");
    case ActionId::NightVision: return TrBind("ui_gp_eng_act_night_vision", "Night Vision (NVG)");
    case ActionId::DetectorToggle: return TrBind("ui_gp_eng_act_detector_toggle", "Detector");
    case ActionId::Pda3DToggle: return TrBind("ui_gp_eng_act_pda_3d_toggle", "3D PDA Toggle");
    case ActionId::MaskClean: return TrBind("ui_gp_eng_act_mask_clean", "Wipe Mask");
    case ActionId::UnderbarrelToggle: return TrBind("ui_gp_eng_act_underbarrel_toggle", "Underbarrel (GL)");
    case ActionId::FireModeNext: return TrBind("ui_gp_eng_act_fire_mode_next", "Fire Mode");
    case ActionId::AmmoTypeNext: return TrBind("ui_gp_eng_act_ammo_type_next", "Switch Ammo Type");
    case ActionId::PauseMenu: return TrBind("ui_gp_eng_act_pause_menu", "Pause Menu (World)");
    case ActionId::PdaFullToggle: return TrBind("ui_gp_eng_act_pda_full_toggle", "PDA / Map");
    case ActionId::DialogSelect: return TrBind("ui_gp_eng_act_dialog_select", "Select Phrase");
    case ActionId::DialogExit: return TrBind("ui_gp_eng_act_dialog_exit", "Exit Dialog");
    case ActionId::WheelConfirm: return TrBind("ui_gp_eng_act_wheel_confirm", "Confirm Slot");
    case ActionId::WheelCancel: return TrBind("ui_gp_eng_act_wheel_cancel", "Cancel Wheel");
    case ActionId::WheelTabPrev: return TrBind("ui_gp_eng_act_wheel_tab_prev", "Previous Wheel Category");
    case ActionId::WheelTabNext: return TrBind("ui_gp_eng_act_wheel_tab_next", "Next Wheel Category");
    case ActionId::PdaInteractiveToggle: return TrBind("ui_gp_eng_act_pda_interactive", "PDA Cursor / Interact");
    case ActionId::PdaClose: return TrBind("ui_gp_eng_act_pda_close", "Put Away 3D PDA");
    case ActionId::PlacementConfirm: return TrBind("ui_gp_eng_act_placement_confirm", "Place Object");
    case ActionId::PlacementCancel: return TrBind("ui_gp_eng_act_placement_cancel", "Cancel Placement");
    case ActionId::PlacementRotate: return TrBind("ui_gp_eng_act_placement_rotate", "Rotate Object");
    case ActionId::QuickBoltOrSlot: return TrBind("ui_gp_eng_act_quick_bolt_slot", "Quick Bolt / Slot");
    case ActionId::SwitchPanel: return TrBind("ui_gp_eng_act_switch_panel", "Switch Panel (Backpack / Loot)");
    case ActionId::CancelQueue: return TrBind("ui_gp_eng_act_cancel_queue", "Cancel Paused Action Queue");
    case ActionId::KnifeDraw: return TrBind("ui_gp_eng_act_knife_draw", "Draw Knife");
    case ActionId::WeaponLower: return TrBind("ui_gp_eng_act_weapon_lower", "Lower / Raise Weapon");
    case ActionId::PdaFlat: return TrBind("ui_gp_eng_act_pda_flat", "PDA (Flat)");
    case ActionId::AmmoWheel: return TrBind("ui_gp_eng_act_ammo_wheel", "Ammo Type Wheel");
    case ActionId::BoltThrow: return TrBind("ui_gp_eng_act_bolt_throw", "Bolt");
    case ActionId::WeaponSelect: return TrBind("ui_gp_eng_act_weapon_select", "Firearm Selector");
    case ActionId::ForceReload: return TrBind("ui_gp_eng_act_force_reload", "Forced Reload / Unjam");
    case ActionId::TutorialPrompt: return TrBind("ui_gp_eng_act_tutorial_prompt", "Open / Hide Tutorial Prompt");
    case ActionId::MouseCursor: return TrBind("ui_gp_eng_act_mouse_cursor", "Mouse Cursor Mode");
    case ActionId::WheelMaskClean: return TrBind("ui_gp_eng_act_wheel_mask", "Wheel: Wipe Mask");
    case ActionId::WheelLaser: return TrBind("ui_gp_eng_act_wheel_laser", "Wheel: Laser");
    default: return TrBind("ui_gp_eng_act_unknown", "Unknown Action");
    }
}

std::string BindingProfileModel::FormatTriggerHumanReadable(const BindingTrigger* trig)
{
    if (!trig || trig->button == ButtonId::None)
        return TrBind("ui_gp_eng_trig_unbound", "[Unbound]");

    char buf[256] = {};
    if (trig->type == TriggerType::Button)
    {
        std::snprintf(buf, sizeof(buf), TrBind("ui_gp_eng_trig_press_fmt", "Press [%s]"), ButtonToString(trig->button));
    }
    else if (trig->type == TriggerType::Hold)
    {
        const unsigned ms = static_cast<unsigned>((trig->hold_ms > 0) ? trig->hold_ms : limits::kDefaultHoldMs);
        std::snprintf(buf, sizeof(buf), TrBind("ui_gp_eng_trig_hold_fmt", "Hold [%s] (%u ms)"), ButtonToString(trig->button), ms);
    }
    else if (trig->type == TriggerType::Chord)
    {
        std::snprintf(buf, sizeof(buf), TrBind("ui_gp_eng_trig_chord_fmt", "Chord [%s + %s]"), ButtonToString(trig->modifier), ButtonToString(trig->button));
    }
    return std::string(buf);
}

namespace
{
LogCallbackFn g_bindingLogCb = nullptr;
std::string g_activeProfilePath = "appdata\\gamepad\\profile.ltx";
std::string g_defaultExportPath = "appdata\\gamepad\\exports\\bindings_export.ltx";
BindingProfileModel g_activeBindingModel;
std::filesystem::file_time_type g_lastSyncedProfileWriteTime = {};
bool g_hasLastSyncedWriteTime = false;

void SyncProfileDiskWriteTime()
{
    std::error_code ec;
    const auto wt = std::filesystem::last_write_time(g_activeProfilePath, ec);
    if (!ec)
    {
        g_lastSyncedProfileWriteTime = wt;
        g_hasLastSyncedWriteTime = true;
    }
}

void EmitBindingLog(const std::string& msg)
{
    if (g_bindingLogCb)
        g_bindingLogCb(msg.c_str());
}
} // namespace

void SetBindingLogCallback(LogCallbackFn cb)
{
    g_bindingLogCb = cb;
}

void SetActiveBindingProfilePath(const char* fullPath)
{
    if (fullPath && fullPath[0])
    {
        g_activeProfilePath = fullPath;
        g_hasLastSyncedWriteTime = false;
    }
}

const char* GetActiveBindingProfilePath()
{
    return g_activeProfilePath.c_str();
}

void SetDefaultExportProfilePath(const char* fullPath)
{
    if (fullPath && fullPath[0])
        g_defaultExportPath = fullPath;
}

const char* GetDefaultExportProfilePath()
{
    return g_defaultExportPath.c_str();
}

const BindingProfileModel& GetActiveBindingProfile()
{
    return g_activeBindingModel;
}

BindingProfileModel& GetMutableActiveBindingProfile()
{
    return g_activeBindingModel;
}

LoadReport LoadActiveBindingProfile()
{
    LoadReport rep = g_activeBindingModel.LoadFromFile(g_activeProfilePath);
    if (rep.status == LoadStatus::FileNotFoundUsedDefaults)
    {
        g_activeBindingModel.SaveToFile(g_activeProfilePath, nullptr);
    }
    SyncProfileDiskWriteTime();
    std::ostringstream oss;
    oss << "* [Gamepad] gpbind::LoadActiveBindingProfile: path='" << g_activeProfilePath
        << "' status=" << static_cast<int>(rep.status)
        << " ver=" << rep.source_version << "->" << rep.target_version
        << " loaded=" << rep.loaded_bindings
        << " migrated=" << rep.migrated_entries
        << " skipped=" << rep.corrupted_lines_skipped
        << " sanitized=" << rep.sanitized_fields
        << " detail='" << rep.detail << "'";
    EmitBindingLog(oss.str());
    return rep;
}

bool ReloadActiveBindingProfileIfModifiedOnDisk()
{
    std::error_code ec;
    const auto wt = std::filesystem::last_write_time(g_activeProfilePath, ec);
    if (ec)
        return false;
    if (!g_hasLastSyncedWriteTime || wt != g_lastSyncedProfileWriteTime)
    {
        LoadActiveBindingProfile();
        return true;
    }
    return false;
}

bool SaveActiveBindingProfile(std::string* outError)
{
    std::string err;
    const bool ok = g_activeBindingModel.SaveToFile(g_activeProfilePath, &err);
    if (!ok)
    {
        if (outError)
            *outError = err;
        EmitBindingLog("! [Gamepad] gpbind::SaveActiveBindingProfile failed: " + err);
    }
    else
    {
        SyncProfileDiskWriteTime();
        std::ostringstream oss;
        oss << "* [Gamepad] gpbind::SaveActiveBindingProfile: saved " << g_activeBindingModel.GetAllBindings().size()
            << " bindings to '" << g_activeProfilePath << "' (schema v" << g_activeBindingModel.GetSchemaVersion() << ")";
        EmitBindingLog(oss.str());
    }
    return ok;
}

bool ExportActiveBindingProfile(const char* customPath, std::string* outError)
{
    const std::string target = (customPath && customPath[0]) ? customPath : g_defaultExportPath;
    std::string err;
    const bool ok = g_activeBindingModel.ExportToFile(target, &err);
    if (!ok)
    {
        if (outError)
            *outError = err;
        EmitBindingLog("! [Gamepad] gpbind::ExportActiveBindingProfile failed: " + err);
    }
    else
    {
        EmitBindingLog("* [Gamepad] gpbind::ExportActiveBindingProfile: exported to '" + target + "'");
    }
    return ok;
}

LoadReport ImportActiveBindingProfile(const char* customPath)
{
    const std::string source = (customPath && customPath[0]) ? customPath : g_defaultExportPath;
    LoadReport rep = g_activeBindingModel.ImportFromFile(source, g_activeProfilePath);
    SyncProfileDiskWriteTime();
    std::ostringstream oss;
    oss << "* [Gamepad] gpbind::ImportActiveBindingProfile: from='" << source
        << "' status=" << static_cast<int>(rep.status)
        << " loaded=" << rep.loaded_bindings
        << " detail='" << rep.detail << "'";
    EmitBindingLog(oss.str());
    return rep;
}

void ResetActiveBindingProfileEmergency(const char* reason)
{
    g_activeBindingModel.ExecuteEmergencyReset(reason, g_activeProfilePath);
    SyncProfileDiskWriteTime();
    std::ostringstream oss;
    oss << "* [Gamepad] gpbind::ResetActiveBindingProfileEmergency: reason='"
        << g_activeBindingModel.GetLastEmergencyResetReason()
        << "' reset_count=" << g_activeBindingModel.GetEmergencyResetCount()
        << " saved_to='" << g_activeProfilePath << "'";
    EmitBindingLog(oss.str());
}

bool UpdateActiveEmergencyResetHold(std::uint32_t rawButtonsMask, std::uint32_t dtContinualMs)
{
    const bool triggered = g_activeBindingModel.UpdateEmergencyResetHold(rawButtonsMask, dtContinualMs, g_activeProfilePath);
    if (triggered)
    {
        std::ostringstream oss;
        oss << "* [Gamepad] EMERGENCY BINDING RESET TRIGGERED (Start+Back held 5000 ms)! Restored default layout and saved to '"
            << g_activeProfilePath << "'";
        EmitBindingLog(oss.str());
    }
    return triggered;
}

void LogActiveBindingProfileStatus()
{
    std::ostringstream oss;
    oss << "* [Gamepad] gp_remap status: path='" << g_activeProfilePath
        << "' schema=v" << g_activeBindingModel.GetSchemaVersion()
        << " total_bindings=" << g_activeBindingModel.GetAllBindings().size()
        << " menu_nav_ok=" << (g_activeBindingModel.VerifyMenuNavigationInvariant() ? 1 : 0)
        << " emergency_resets=" << g_activeBindingModel.GetEmergencyResetCount()
        << " last_reset='" << g_activeBindingModel.GetLastEmergencyResetReason() << "'";
    EmitBindingLog(oss.str());
}

} // namespace gpbind
