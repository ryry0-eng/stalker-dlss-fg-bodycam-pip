#include "stdafx.h"
#include "GamepadGlyphs.h"
#include "GamepadDeviceManager.h"
#include "xr_input.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// Explicit Windows-1251 byte sequences for Russian HUD/Menu strings
// so compiler source-charset settings cannot corrupt Cyrillic in CStringTable.
constexpr const char* kRuCross       = "[\xCA\xF0\xE5\xF1\xF2]";                   // [Крест]
constexpr const char* kRuCircle      = "[\xCA\xF0\xF3\xE3]";                       // [Круг]
constexpr const char* kRuSquare      = "[\xCA\xE2\xE0\xE4\xF0\xE0\xF2]";           // [Квадрат]
constexpr const char* kRuTriangle    = "[\xD2\xF0\xE5\xF3\xE3.]";                  // [Треуг.]
constexpr const char* kRuDPadUp      = "[\xCA\xF0\xE5\xF1\xF2.\xC2\xE2\xE5\xF0\xF5]";     // [Крест.Вверх]
constexpr const char* kRuDPadDown    = "[\xCA\xF0\xE5\xF1\xF2.\xC2\xED\xE8\xE7]";         // [Крест.Вниз]
constexpr const char* kRuDPadLeft    = "[\xCA\xF0\xE5\xF1\xF2.\xC2\xEB\xE5\xE2\xEE]";     // [Крест.Влево]
constexpr const char* kRuDPadRight   = "[\xCA\xF0\xE5\xF1\xF2.\xC2\xEF\xF0\xE0\xE2\xEE]"; // [Крест.Вправо]
constexpr const char* kRuHoldPrefix  = "\xD3\xE4\xE5\xF0\xE6. ";                   // "Удерж. "

FGamepadGlyphTranslateCallback g_glyphTranslateCb = nullptr;

const char* TranslateGlyph(const char* stringId, const char* fallbackCp1251)
{
    if (g_glyphTranslateCb)
    {
        const char* res = g_glyphTranslateCb(stringId, fallbackCp1251);
        if (res && res[0])
            return res;
    }
    return fallbackCp1251;
}

std::string TrimStr(const std::string& s)
{
    const char* ws = " \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return {};
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string LowerStr(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
class CCC_GamepadGlyphs : public IConsole_Command
{
public:
    CCC_GamepadGlyphs(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = true;
    }

    virtual void Execute(LPCSTR args) override
    {
        auto& mgr = CGamepadGlyphManager::Instance();
        mgr.EnsureInitialized();

        const std::string raw = TrimStr(args ? args : "");
        const std::string low = LowerStr(raw);

        if (low.empty() || low == "status")
        {
            mgr.RefreshFromActiveDevice("console_status");
            mgr.LogStatus();
            return;
        }
        if (low == "selftest" || low == "test")
        {
            mgr.RunSelfTest();
            return;
        }
        if (low == "reload" || low == "load")
        {
            const bool ok = mgr.LoadFromProfileLtx(nullptr);
            mgr.NotifyPromptsDirty("console_reload");
            Msg("* [Gamepad] gp_glyphs reload: %s (override='%s', effective='%s', face='%s')",
                ok ? "loaded" : "defaults",
                CGamepadGlyphManager::GlyphSetToString(mgr.GetStyleOverride()),
                CGamepadGlyphManager::GlyphSetToString(mgr.GetEffectiveGlyphSet()),
                CGamepadGlyphManager::FaceStyleToString(mgr.GetEffectiveFaceStyle()));
            return;
        }
        if (low == "save")
        {
            const bool ok = mgr.SaveToProfileLtx(nullptr);
            Msg("* [Gamepad] gp_glyphs save: %s", ok ? "ok" : "failed");
            return;
        }

        std::string styleArg = low;
        if (low.rfind("set ", 0) == 0)
            styleArg = TrimStr(low.substr(4));

        if (mgr.SetStyleOverrideByName(styleArg.c_str(), true))
        {
            return;
        }

        Msg("! [Gamepad] Usage: gp_glyphs [status | auto | xbox | playstation | nintendo | generic | reload | save | selftest]");
    }

    virtual void Save(IWriter* /*F*/) override
    {
        // D37: Do not write to user.ltx; persisted in appdata\gamepad\profile.ltx
    }

    virtual void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        tips.push_back("status");
        tips.push_back("auto");
        tips.push_back("xbox");
        tips.push_back("playstation");
        tips.push_back("nintendo");
        tips.push_back("generic");
        tips.push_back("reload");
        tips.push_back("save");
        tips.push_back("selftest");
    }
};

static CCC_GamepadGlyphs s_cmdGpGlyphs("gp_glyphs");
} // namespace

CGamepadGlyphManager& CGamepadGlyphManager::Instance()
{
    static CGamepadGlyphManager s_instance;
    return s_instance;
}

CGamepadGlyphManager::CGamepadGlyphManager()
{
    xr_strcpy(m_lastDirtyReason, "init");
}

void CGamepadGlyphManager::ResolveProfileLtxPath(string_path& outPath)
{
    outPath[0] = '\0';
    if (FS.path_exist("$app_data_root$"))
    {
        FS.update_path(outPath, "$app_data_root$", "gamepad\\profile.ltx");
    }
    else
    {
        xr_strcpy(outPath, "appdata\\gamepad\\profile.ltx");
    }
}

void CGamepadGlyphManager::EnsureInitialized()
{
    if (!m_cmdRegistered && Console)
    {
        if (!Console->GetCommand("gp_glyphs"))
        {
            Console->AddCommand(&s_cmdGpGlyphs);
        }
        m_cmdRegistered = true;
    }

    if (!m_initialized)
    {
        m_initialized = true;
        Initialize();
    }
}

void CGamepadGlyphManager::Initialize()
{
    m_initialized = true;
    if (!m_cmdRegistered && Console)
    {
        if (!Console->GetCommand("gp_glyphs"))
        {
            Console->AddCommand(&s_cmdGpGlyphs);
        }
        m_cmdRegistered = true;
    }
    LoadFromProfileLtx(nullptr);
    RefreshFromActiveDevice("init");
    m_lastBindingsHash = ComputeActiveBindingsSignature();
}

const char* CGamepadGlyphManager::GlyphSetToString(EGamepadGlyphSet set)
{
    switch (set)
    {
    case EGamepadGlyphSet::Auto:        return "auto";
    case EGamepadGlyphSet::Xbox:        return "xbox";
    case EGamepadGlyphSet::PlayStation: return "playstation";
    case EGamepadGlyphSet::Nintendo:    return "nintendo";
    case EGamepadGlyphSet::Generic:     return "generic";
    default:                            return "xbox";
    }
}

EGamepadGlyphSet CGamepadGlyphManager::StringToGlyphSet(const char* str, bool* outValid)
{
    if (outValid)
        *outValid = false;
    if (!str || !str[0])
        return EGamepadGlyphSet::Auto;

    const std::string s = LowerStr(TrimStr(str));
    if (s == "auto" || s == "default" || s == "sdl")
    {
        if (outValid) *outValid = true;
        return EGamepadGlyphSet::Auto;
    }
    if (s == "xbox" || s == "xb" || s == "xinput" || s == "abxy")
    {
        if (outValid) *outValid = true;
        return EGamepadGlyphSet::Xbox;
    }
    if (s == "playstation" || s == "ps" || s == "ps4" || s == "ps5" || s == "sony" || s == "dualshock" || s == "dualsense")
    {
        if (outValid) *outValid = true;
        return EGamepadGlyphSet::PlayStation;
    }
    if (s == "nintendo" || s == "switch" || s == "nx" || s == "bayx")
    {
        if (outValid) *outValid = true;
        return EGamepadGlyphSet::Nintendo;
    }
    if (s == "generic" || s == "classb" || s == "numbered" || s == "standard")
    {
        if (outValid) *outValid = true;
        return EGamepadGlyphSet::Generic;
    }
    return EGamepadGlyphSet::Auto;
}

const char* CGamepadGlyphManager::FaceStyleToString(EGamepadFaceStyle face)
{
    switch (face)
    {
    case EGamepadFaceStyle::ABXY:     return "ABXY";
    case EGamepadFaceStyle::Sony:     return "Sony";
    case EGamepadFaceStyle::BAYX:     return "BAYX";
    case EGamepadFaceStyle::AXBY:     return "AXBY";
    case EGamepadFaceStyle::Numbered: return "Numbered";
    default:                          return "ABXY";
    }
}

const char* CGamepadGlyphManager::IconStyleToString(EGamepadIconStyle iconStyle)
{
    switch (iconStyle)
    {
    case EGamepadIconStyle::Dark:  return "dark";
    case EGamepadIconStyle::Color: return "color";
    case EGamepadIconStyle::White:
    default:                       return "white";
    }
}

CGamepadGlyphManager::EGamepadIconStyle CGamepadGlyphManager::StringToIconStyle(const char* str, bool* outValid)
{
    if (outValid) *outValid = false;
    if (!str)
        return EGamepadIconStyle::White;
    const std::string s = LowerStr(TrimStr(str));
    if (s == "white" || s == "light")
    {
        if (outValid) *outValid = true;
        return EGamepadIconStyle::White;
    }
    if (s == "dark" || s == "bw")
    {
        if (outValid) *outValid = true;
        return EGamepadIconStyle::Dark;
    }
    if (s == "color")
    {
        if (outValid) *outValid = true;
        return EGamepadIconStyle::Color;
    }
    return EGamepadIconStyle::White;
}

const char* CGamepadGlyphManager::HintModeToString(EGamepadHintMode mode)
{
    switch (mode)
    {
    case EGamepadHintMode::Menu:   return "menu";
    case EGamepadHintMode::Off:    return "off";
    case EGamepadHintMode::Always:
    default:                       return "always";
    }
}

CGamepadGlyphManager::EGamepadHintMode CGamepadGlyphManager::StringToHintMode(const char* str, bool* outValid)
{
    if (outValid) *outValid = false;
    if (!str)
        return EGamepadHintMode::Always;
    const std::string s = LowerStr(TrimStr(str));
    if (s == "always")
    {
        if (outValid) *outValid = true;
        return EGamepadHintMode::Always;
    }
    if (s == "menu")
    {
        if (outValid) *outValid = true;
        return EGamepadHintMode::Menu;
    }
    if (s == "off")
    {
        if (outValid) *outValid = true;
        return EGamepadHintMode::Off;
    }
    return EGamepadHintMode::Always;
}

bool CGamepadGlyphManager::SetIconStyle(EGamepadIconStyle style, bool saveToDisk)
{
    if (m_iconStyle != style)
    {
        m_iconStyle = style;
        NotifyPromptsDirty("icon_style_changed");
    }
    return saveToDisk ? SaveToProfileLtx() : true;
}

bool CGamepadGlyphManager::SetIconStyleByName(const char* styleName, bool saveToDisk)
{
    bool valid = false;
    const EGamepadIconStyle parsed = StringToIconStyle(styleName, &valid);
    if (!valid)
        return false;
    return SetIconStyle(parsed, saveToDisk);
}

bool CGamepadGlyphManager::SetIconScale(int percent, bool saveToDisk)
{
    const int clamped = _max(50, _min(150, percent));
    if (m_iconScale != clamped)
    {
        m_iconScale = clamped;
        NotifyPromptsDirty("icon_scale_changed");
    }
    return saveToDisk ? SaveToProfileLtx() : true;
}

bool CGamepadGlyphManager::SetHintMode(EGamepadHintMode mode, bool saveToDisk)
{
    if (m_hintMode != mode)
    {
        m_hintMode = mode;
        NotifyPromptsDirty("hint_mode_changed");
    }
    return saveToDisk ? SaveToProfileLtx() : true;
}

bool CGamepadGlyphManager::SetHintModeByName(const char* modeName, bool saveToDisk)
{
    bool valid = false;
    const EGamepadHintMode parsed = StringToHintMode(modeName, &valid);
    if (!valid)
        return false;
    return SetHintMode(parsed, saveToDisk);
}

bool CGamepadGlyphManager::AreHintsVisible(bool inMenu) const
{
    switch (m_hintMode)
    {
    case EGamepadHintMode::Off:  return false;
    case EGamepadHintMode::Menu: return inMenu;
    case EGamepadHintMode::Always:
    default:                     return true;
    }
}

void CGamepadGlyphManager::ResolveForSDLTypeAndLabels(int sdlGamepadType,
                                                      int deviceClass,
                                                      int lblSouth,
                                                      int lblEast,
                                                      int lblWest,
                                                      int lblNorth,
                                                      EGamepadGlyphSet styleOverride,
                                                      EGamepadGlyphSet& outEffectiveSet,
                                                      EGamepadFaceStyle& outFaceStyle)
{
    (void)lblNorth;

    // 1. Manual override takes precedence over SDL auto-detection (D5.1 item 1)
    if (styleOverride != EGamepadGlyphSet::Auto)
    {
        outEffectiveSet = styleOverride;
        switch (styleOverride)
        {
        case EGamepadGlyphSet::PlayStation:
            outFaceStyle = EGamepadFaceStyle::Sony;
            break;
        case EGamepadGlyphSet::Nintendo:
            outFaceStyle = EGamepadFaceStyle::BAYX;
            break;
        case EGamepadGlyphSet::Generic:
            outFaceStyle = EGamepadFaceStyle::Numbered;
            break;
        case EGamepadGlyphSet::Xbox:
        default:
            outFaceStyle = EGamepadFaceStyle::ABXY;
            break;
        }
        return;
    }

    // 2. Auto-detection per R4 glyph_logic:
    // Step 2a: Base set from SDL_GamepadType and device class (ClassA / ClassB)
    switch (static_cast<SDL_GamepadType>(sdlGamepadType))
    {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        outEffectiveSet = EGamepadGlyphSet::PlayStation;
        outFaceStyle    = EGamepadFaceStyle::Sony;
        break;

    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        outEffectiveSet = EGamepadGlyphSet::Nintendo;
        outFaceStyle    = EGamepadFaceStyle::BAYX;
        break;

    case SDL_GAMEPAD_TYPE_GAMECUBE:
        outEffectiveSet = EGamepadGlyphSet::Xbox;
        outFaceStyle    = EGamepadFaceStyle::AXBY;
        break;

    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
    case SDL_GAMEPAD_TYPE_STEAM:
        outEffectiveSet = EGamepadGlyphSet::Xbox;
        outFaceStyle    = EGamepadFaceStyle::ABXY;
        break;

    case SDL_GAMEPAD_TYPE_STANDARD:
    case SDL_GAMEPAD_TYPE_UNKNOWN:
    default:
        if (deviceClass == static_cast<int>(EGamepadClass::ClassB))
        {
            outEffectiveSet = EGamepadGlyphSet::Generic;
            outFaceStyle    = EGamepadFaceStyle::Numbered;
        }
        else
        {
            outEffectiveSet = EGamepadGlyphSet::Xbox;
            outFaceStyle    = EGamepadFaceStyle::ABXY;
        }
        break;
    }

    // Step 2b: Inspect SDL_GetGamepadButtonLabel / face: hint (R4 glyph_logic step 1)
    // Allows SDL mappings with face:sony / face:bayx / face:axby to refine face style even on STANDARD controllers.
    if (lblSouth == SDL_GAMEPAD_BUTTON_LABEL_CROSS || lblEast == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE ||
        lblWest == SDL_GAMEPAD_BUTTON_LABEL_SQUARE)
    {
        outEffectiveSet = EGamepadGlyphSet::PlayStation;
        outFaceStyle    = EGamepadFaceStyle::Sony;
    }
    else if (lblSouth == SDL_GAMEPAD_BUTTON_LABEL_B && lblEast == SDL_GAMEPAD_BUTTON_LABEL_A)
    {
        outEffectiveSet = EGamepadGlyphSet::Nintendo;
        outFaceStyle    = EGamepadFaceStyle::BAYX;
    }
    else if (lblEast == SDL_GAMEPAD_BUTTON_LABEL_X && lblWest == SDL_GAMEPAD_BUTTON_LABEL_B)
    {
        outFaceStyle = EGamepadFaceStyle::AXBY;
        if (outEffectiveSet == EGamepadGlyphSet::Generic)
            outEffectiveSet = EGamepadGlyphSet::Xbox;
    }
}

u32 CGamepadGlyphManager::ComputeActiveBindingsSignature() const
{
    const auto& all = gpbind::GetActiveBindingProfile().GetAllBindings();
    u32 hash = 2166136261u;
    for (const auto& e : all)
    {
        hash = (hash ^ static_cast<u32>(e.context)) * 16777619u;
        hash = (hash ^ static_cast<u32>(e.action)) * 16777619u;
        hash = (hash ^ static_cast<u32>(e.trigger.type)) * 16777619u;
        hash = (hash ^ static_cast<u32>(e.trigger.button)) * 16777619u;
        hash = (hash ^ static_cast<u32>(e.trigger.modifier)) * 16777619u;
        hash = (hash ^ static_cast<u32>(e.trigger.hold_ms)) * 16777619u;
    }
    return hash;
}

bool CGamepadGlyphManager::RefreshFromActiveDevice(const char* reason)
{
    int sdlType     = SDL_GAMEPAD_TYPE_XBOXONE;
    int deviceClass = static_cast<int>(EGamepadClass::ClassA);
    int lblSouth    = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    int lblEast     = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    int lblWest     = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    int lblNorth    = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;

    if (pInput && pInput->GetGamepadDeviceManager())
    {
        const SGamepadDeviceEntry* active = pInput->GetGamepadDeviceManager()->GetActiveDevice();
        if (active)
        {
            deviceClass = static_cast<int>(active->device_class);
            if (active->gamepad)
            {
                sdlType  = static_cast<int>(SDL_GetGamepadType(active->gamepad));
                lblSouth = static_cast<int>(SDL_GetGamepadButtonLabel(active->gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
                lblEast  = static_cast<int>(SDL_GetGamepadButtonLabel(active->gamepad, SDL_GAMEPAD_BUTTON_EAST));
                lblWest  = static_cast<int>(SDL_GetGamepadButtonLabel(active->gamepad, SDL_GAMEPAD_BUTTON_WEST));
                lblNorth = static_cast<int>(SDL_GetGamepadButtonLabel(active->gamepad, SDL_GAMEPAD_BUTTON_NORTH));
            }
            else
            {
                sdlType  = static_cast<int>(active->sdl_gamepad_type);
                const SDL_GamepadType gt = static_cast<SDL_GamepadType>(sdlType);
                lblSouth = static_cast<int>(SDL_GetGamepadButtonLabelForType(gt, SDL_GAMEPAD_BUTTON_SOUTH));
                lblEast  = static_cast<int>(SDL_GetGamepadButtonLabelForType(gt, SDL_GAMEPAD_BUTTON_EAST));
                lblWest  = static_cast<int>(SDL_GetGamepadButtonLabelForType(gt, SDL_GAMEPAD_BUTTON_WEST));
                lblNorth = static_cast<int>(SDL_GetGamepadButtonLabelForType(gt, SDL_GAMEPAD_BUTTON_NORTH));
            }
        }
    }

    EGamepadGlyphSet newSet   = EGamepadGlyphSet::Xbox;
    EGamepadFaceStyle newFace = EGamepadFaceStyle::ABXY;
    ResolveForSDLTypeAndLabels(
        sdlType, deviceClass, lblSouth, lblEast, lblWest, lblNorth, m_styleOverride, newSet, newFace);

    const u32 curBindingsHash = ComputeActiveBindingsSignature();
    const bool styleChanged   = (newSet != m_effectiveSet || newFace != m_effectiveFaceStyle);
    const bool bindsChanged   = (m_lastBindingsHash != 0 && curBindingsHash != m_lastBindingsHash);

    m_effectiveSet       = newSet;
    m_effectiveFaceStyle = newFace;
    m_lastBindingsHash   = curBindingsHash;

    if (styleChanged || bindsChanged)
    {
        NotifyPromptsDirty(bindsChanged ? "bindings_changed" : (reason ? reason : "device_style_changed"));
        return true;
    }
    return false;
}

void CGamepadGlyphManager::NotifyPromptsDirty(const char* reason)
{
    ++m_promptRevision;
    if (m_promptRevision == 0)
        m_promptRevision = 1;
    xr_strcpy(m_lastDirtyReason, reason && reason[0] ? reason : "dirty");

    if (m_dirtyCallback)
    {
        m_dirtyCallback(m_promptRevision, m_lastDirtyReason);
    }
}

bool CGamepadGlyphManager::SetStyleOverride(EGamepadGlyphSet style, bool saveToDisk)
{
    m_styleOverride = style;
    RefreshFromActiveDevice("style_override");
    NotifyPromptsDirty("style_override");
    if (saveToDisk)
    {
        SaveToProfileLtx(nullptr);
    }
    Msg("* [Gamepad] Glyph style override set to '%s' -> effective_set='%s' face='%s' (rev=%u)",
        GlyphSetToString(m_styleOverride),
        GlyphSetToString(m_effectiveSet),
        FaceStyleToString(m_effectiveFaceStyle),
        m_promptRevision);
    return true;
}

bool CGamepadGlyphManager::SetStyleOverrideByName(const char* styleName, bool saveToDisk)
{
    bool valid = false;
    const EGamepadGlyphSet parsed = StringToGlyphSet(styleName, &valid);
    if (!valid)
        return false;
    return SetStyleOverride(parsed, saveToDisk);
}

bool CGamepadGlyphManager::LoadFromProfileLtx(const char* explicitPath)
{
    string_path pathBuf = {};
    if (explicitPath && explicitPath[0])
        xr_strcpy(pathBuf, explicitPath);
    else
        ResolveProfileLtxPath(pathBuf);

    if (gpbind::BindingProfileModel::IsPathForbiddenForWrite(pathBuf, nullptr))
        return false;

    std::ifstream ifs(pathBuf, std::ios::binary);
    if (!ifs)
    {
        m_styleOverride = EGamepadGlyphSet::Auto;
        RefreshFromActiveDevice("profile_default");
        return false;
    }

    std::string line;
    std::string currentSec;
    while (std::getline(ifs, line))
    {
        const std::size_t semi = line.find(';');
        if (semi != std::string::npos)
            line = line.substr(0, semi);
        line = TrimStr(line);
        if (line.empty())
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            currentSec = LowerStr(TrimStr(line.substr(1, line.size() - 2)));
            continue;
        }

        if (currentSec != "glyphs")
            continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = LowerStr(TrimStr(line.substr(0, eq)));
        const std::string val = TrimStr(line.substr(eq + 1));
        if (key == "style_override" || key == "glyph_set" || key == "style")
        {
            bool ok = false;
            const EGamepadGlyphSet parsed = StringToGlyphSet(val.c_str(), &ok);
            m_styleOverride = ok ? parsed : EGamepadGlyphSet::Auto;
        }
    }

    RefreshFromActiveDevice("profile_loaded");
    return true;
}

bool CGamepadGlyphManager::SaveToProfileLtx(const char* explicitPath) const
{
    string_path pathBuf = {};
    if (explicitPath && explicitPath[0])
        xr_strcpy(pathBuf, explicitPath);
    else
        ResolveProfileLtxPath(pathBuf);

    std::string forbidReason;
    if (gpbind::BindingProfileModel::IsPathForbiddenForWrite(pathBuf, &forbidReason))
    {
        Msg("! [Gamepad] CGamepadGlyphManager::SaveToProfileLtx refused path '%s': %s", pathBuf, forbidReason.c_str());
        return false;
    }

    // Preserve all sibling sections ([profile], [left_stick], [haptics], [profile_header], [context_*], etc.)
    std::vector<std::string> preservedLines;
    {
        std::ifstream ifs(pathBuf, std::ios::binary);
        if (ifs)
        {
            std::string rawLine;
            bool inGlyphsSection = false;
            while (std::getline(ifs, rawLine))
            {
                if (!rawLine.empty() && rawLine.back() == '\r')
                    rawLine.pop_back();

                std::string stripped = rawLine;
                const std::size_t semi = stripped.find(';');
                if (semi != std::string::npos)
                    stripped = stripped.substr(0, semi);
                stripped = TrimStr(stripped);

                if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']')
                {
                    const std::string secName = LowerStr(TrimStr(stripped.substr(1, stripped.size() - 2)));
                    inGlyphsSection = (secName == "glyphs");
                }

                if (!inGlyphsSection)
                    preservedLines.push_back(rawLine);
            }
        }
    }

    // Ensure directory exists
    string_path dirPath = {};
    xr_strcpy(dirPath, pathBuf);
    char* lastSlash = strrchr(dirPath, '\\');
    if (!lastSlash)
        lastSlash = strrchr(dirPath, '/');
    if (lastSlash)
    {
        *lastSlash = '\0';
        CreateDirectoryA(dirPath, nullptr);
    }

    string_path tmpPath = {};
    xr_sprintf(tmpPath, "%s.tmp", pathBuf);

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs)
        {
            Msg("! [Gamepad] CGamepadGlyphManager::SaveToProfileLtx failed to open '%s'", tmpPath);
            return false;
        }

        for (const std::string& pl : preservedLines)
            ofs << pl << "\n";

        if (!preservedLines.empty() && !preservedLines.back().empty())
            ofs << "\n";

        ofs << "[glyphs]\n";
        ofs << "style_override = " << GlyphSetToString(m_styleOverride) << "\n";
        ofs << "icon_style = " << IconStyleToString(m_iconStyle) << "\n";
        ofs << "icon_scale = " << m_iconScale << "\n";
        ofs << "hint_mode = " << HintModeToString(m_hintMode) << "\n";
        ofs.flush();
        if (!ofs.good())
        {
            ofs.close();
            DeleteFileA(tmpPath);
            return false;
        }
    }

    if (!MoveFileExA(tmpPath, pathBuf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(tmpPath);
        return false;
    }
    return true;
}

void CGamepadGlyphManager::SetTranslateCallback(FGamepadGlyphTranslateCallback cb)
{
    g_glyphTranslateCb = cb;
}

const char* CGamepadGlyphManager::GetButtonLabelForStyle(gpbind::ButtonId btn,
                                                         EGamepadGlyphSet effectiveSet,
                                                         EGamepadFaceStyle faceStyle,
                                                         bool isRussian,
                                                         bool compactBadge)
{
    using gpbind::ButtonId;

    // 1. Face buttons (South / East / West / North)
    switch (btn)
    {
    case ButtonId::South:
        switch (faceStyle)
        {
        case EGamepadFaceStyle::Sony:     return compactBadge ? "X"  : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_ps_cross", kRuCross) : "[Cross]");
        case EGamepadFaceStyle::BAYX:     return compactBadge ? "B"  : "[B]";
        case EGamepadFaceStyle::AXBY:     return compactBadge ? "A"  : "[A]";
        case EGamepadFaceStyle::Numbered: return compactBadge ? "B1" : "[B1]";
        case EGamepadFaceStyle::ABXY:
        default:                          return compactBadge ? "A"  : "[A]";
        }

    case ButtonId::East:
        switch (faceStyle)
        {
        case EGamepadFaceStyle::Sony:     return compactBadge ? "O"  : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_ps_circle", kRuCircle) : "[Circle]");
        case EGamepadFaceStyle::BAYX:     return compactBadge ? "A"  : "[A]";
        case EGamepadFaceStyle::AXBY:     return compactBadge ? "X"  : "[X]";
        case EGamepadFaceStyle::Numbered: return compactBadge ? "B2" : "[B2]";
        case EGamepadFaceStyle::ABXY:
        default:                          return compactBadge ? "B"  : "[B]";
        }

    case ButtonId::West:
        switch (faceStyle)
        {
        case EGamepadFaceStyle::Sony:     return compactBadge ? "Sq" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_ps_square", kRuSquare) : "[Square]");
        case EGamepadFaceStyle::BAYX:     return compactBadge ? "Y"  : "[Y]";
        case EGamepadFaceStyle::AXBY:     return compactBadge ? "B"  : "[B]";
        case EGamepadFaceStyle::Numbered: return compactBadge ? "B3" : "[B3]";
        case EGamepadFaceStyle::ABXY:
        default:                          return compactBadge ? "X"  : "[X]";
        }

    case ButtonId::North:
        switch (faceStyle)
        {
        case EGamepadFaceStyle::Sony:     return compactBadge ? "Tr" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_ps_triangle", kRuTriangle) : "[Triangle]");
        case EGamepadFaceStyle::BAYX:     return compactBadge ? "X"  : "[X]";
        case EGamepadFaceStyle::AXBY:     return compactBadge ? "Y"  : "[Y]";
        case EGamepadFaceStyle::Numbered: return compactBadge ? "B4" : "[B4]";
        case EGamepadFaceStyle::ABXY:
        default:                          return compactBadge ? "Y"  : "[Y]";
        }

    // 2. Shoulders & Triggers
    case ButtonId::LeftShoulder:
        if (effectiveSet == EGamepadGlyphSet::PlayStation || effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "L1" : "[L1]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "L" : "[L]";
        return compactBadge ? "LB" : "[LB]";

    case ButtonId::RightShoulder:
        if (effectiveSet == EGamepadGlyphSet::PlayStation || effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "R1" : "[R1]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "R" : "[R]";
        return compactBadge ? "RB" : "[RB]";

    case ButtonId::LT:
        if (effectiveSet == EGamepadGlyphSet::PlayStation || effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "L2" : "[L2]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "ZL" : "[ZL]";
        return compactBadge ? "LT" : "[LT]";

    case ButtonId::RT:
        if (effectiveSet == EGamepadGlyphSet::PlayStation || effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "R2" : "[R2]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "ZR" : "[ZR]";
        return compactBadge ? "RT" : "[RT]";

    // 3. Stick clicks
    case ButtonId::LeftStick:
        return compactBadge ? "L3" : "[L3]";

    case ButtonId::RightStick:
        return compactBadge ? "R3" : "[R3]";

    // 4. System buttons
    case ButtonId::Back:
        if (effectiveSet == EGamepadGlyphSet::PlayStation)
            return compactBadge ? "Shr" : "[Share]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "-" : "[-]";
        if (effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "Bck" : "[Back]";
        return compactBadge ? "Vw" : "[View]";

    case ButtonId::Start:
        if (effectiveSet == EGamepadGlyphSet::PlayStation)
            return compactBadge ? "Opt" : "[Options]";
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "+" : "[+]";
        if (effectiveSet == EGamepadGlyphSet::Generic)
            return compactBadge ? "St" : "[Start]";
        return compactBadge ? "Mnu" : "[Menu]";

    case ButtonId::Guide:
        if (effectiveSet == EGamepadGlyphSet::Nintendo)
            return compactBadge ? "Hm" : "[Home]";
        return compactBadge ? "Gd" : "[Guide]";

    // 5. D-Pad
    case ButtonId::DPadUp:
        return compactBadge ? "D-U" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_dpad_up", kRuDPadUp) : "[D-Up]");
    case ButtonId::DPadDown:
        return compactBadge ? "D-D" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_dpad_down", kRuDPadDown) : "[D-Down]");
    case ButtonId::DPadLeft:
        return compactBadge ? "D-L" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_dpad_left", kRuDPadLeft) : "[D-Left]");
    case ButtonId::DPadRight:
        return compactBadge ? "D-R" : (isRussian ? TranslateGlyph("ui_gp_eng_glyph_dpad_right", kRuDPadRight) : "[D-Right]");

    default:
        return compactBadge ? "--" : "[--]";
    }
}

const char* CGamepadGlyphManager::GetButtonTextureIdForStyle(gpbind::ButtonId btn,
                                                             EGamepadGlyphSet effectiveSet,
                                                             EGamepadFaceStyle faceStyle,
                                                             EGamepadIconStyle iconStyle,
                                                             bool keyShape)
{
    using gpbind::ButtonId;

    // For GameCube AXBY face style, East shows X (West glyph) and West shows B (East glyph)
    if (faceStyle == EGamepadFaceStyle::AXBY)
    {
        if (btn == ButtonId::East)      btn = ButtonId::West;
        else if (btn == ButtonId::West) btn = ButtonId::East;
    }

    const char* styleStr = IconStyleToString(iconStyle);
    const char* shapeInfix = keyShape ? "_key" : "";

    const char* prefix = "xbox";
    switch (effectiveSet)
    {
    case EGamepadGlyphSet::PlayStation: prefix = "ps";       break;
    case EGamepadGlyphSet::Nintendo:    prefix = "nintendo"; break;
    case EGamepadGlyphSet::Generic:     prefix = "generic";  break;
    case EGamepadGlyphSet::Xbox:
    default:                            prefix = "xbox";     break;
    }

    const char* suffix = "south";
    switch (btn)
    {
    case ButtonId::South:         suffix = "south";     break;
    case ButtonId::East:          suffix = "east";      break;
    case ButtonId::West:          suffix = "west";      break;
    case ButtonId::North:         suffix = "north";     break;
    case ButtonId::Back:          suffix = "back";      break;
    case ButtonId::Guide:         suffix = "guide";     break;
    case ButtonId::Start:         suffix = "start";     break;
    case ButtonId::LeftStick:     suffix = "lstick";    break;
    case ButtonId::RightStick:    suffix = "rstick";    break;
    case ButtonId::LeftShoulder:  suffix = "lshoulder"; break;
    case ButtonId::RightShoulder: suffix = "rshoulder"; break;
    case ButtonId::DPadUp:        suffix = "dpup";      break;
    case ButtonId::DPadDown:      suffix = "dpdown";    break;
    case ButtonId::DPadLeft:      suffix = "dpleft";    break;
    case ButtonId::DPadRight:     suffix = "dpright";   break;
    case ButtonId::LT:            suffix = "ltrigger";  break;
    case ButtonId::RT:            suffix = "rtrigger";  break;
    default:                      return "";
    }

    static thread_local char s_texIdBuf[64];
    xr_sprintf(s_texIdBuf, sizeof(s_texIdBuf), "ui_gp_%s%s_%s_%s", styleStr, shapeInfix, prefix, suffix);
    return s_texIdBuf;
}

const char* CGamepadGlyphManager::GetButtonLabel(gpbind::ButtonId btn, bool isRussian, bool compactBadge) const
{
    return GetButtonLabelForStyle(btn, m_effectiveSet, m_effectiveFaceStyle, isRussian, compactBadge);
}

const char* CGamepadGlyphManager::GetButtonTextureId(gpbind::ButtonId btn, bool keyShape) const
{
    return GetButtonTextureIdForStyle(btn, m_effectiveSet, m_effectiveFaceStyle, m_iconStyle, keyShape);
}

void CGamepadGlyphManager::FormatTriggerPromptForStyle(const gpbind::BindingTrigger& trig,
                                                       EGamepadGlyphSet effectiveSet,
                                                       EGamepadFaceStyle faceStyle,
                                                       bool isRussian,
                                                       bool compactBadge,
                                                       char* dstBuff,
                                                       int dstBuffSize)
{
    if (!dstBuff || dstBuffSize <= 0)
        return;
    dstBuff[0] = '\0';

    if (trig.button == gpbind::ButtonId::None)
    {
        xr_strcpy(dstBuff, dstBuffSize, "--");
        return;
    }

    const char* priLbl = GetButtonLabelForStyle(trig.button, effectiveSet, faceStyle, isRussian, compactBadge);

    if (trig.type == gpbind::TriggerType::Button)
    {
        xr_strcpy(dstBuff, dstBuffSize, priLbl);
    }
    else if (trig.type == gpbind::TriggerType::Hold)
    {
        if (compactBadge)
            xr_sprintf(dstBuff, dstBuffSize, "H:%s", priLbl);
        else
            xr_sprintf(dstBuff, dstBuffSize, "%s%s", isRussian ? TranslateGlyph("ui_gp_eng_glyph_hold_prefix", kRuHoldPrefix) : "Hold ", priLbl);
    }
    else if (trig.type == gpbind::TriggerType::Chord)
    {
        const char* modLbl = GetButtonLabelForStyle(trig.modifier, effectiveSet, faceStyle, isRussian, compactBadge);
        xr_sprintf(dstBuff, dstBuffSize, "%s+%s", modLbl, priLbl);
    }
}

void CGamepadGlyphManager::FormatTriggerPrompt(const gpbind::BindingTrigger& trig,
                                               bool isRussian,
                                               bool compactBadge,
                                               char* dstBuff,
                                               int dstBuffSize) const
{
    FormatTriggerPromptForStyle(trig, m_effectiveSet, m_effectiveFaceStyle, isRussian, compactBadge, dstBuff, dstBuffSize);
}

bool CGamepadGlyphManager::MapEngineActionToGamepadBinding(const char* engineActionName,
                                                           gpbind::ContextId& outCtx,
                                                           gpbind::ActionId& outAct,
                                                           gpbind::ButtonId* outDirectButtonFallback)
{
    outCtx = gpbind::ContextId::World_Gameplay;
    outAct = gpbind::ActionId::Invalid;
    if (outDirectButtonFallback)
        *outDirectButtonFallback = gpbind::ButtonId::None;

    if (!engineActionName || !engineActionName[0])
        return false;

    const std::string s = LowerStr(TrimStr(engineActionName));

    // Quick slots 1..4 on HUD / Inventory (mapped to D-Pad directions / quick wheel slots)
    if (s == "quick_use_1")
    {
        outAct = gpbind::ActionId::TorchToggle; // DPadUp default, fallback DPadUp
        if (outDirectButtonFallback) *outDirectButtonFallback = gpbind::ButtonId::DPadUp;
        return true;
    }
    if (s == "quick_use_2")
    {
        outAct = gpbind::ActionId::Pda3DToggle; // DPadRight default, fallback DPadRight
        if (outDirectButtonFallback) *outDirectButtonFallback = gpbind::ButtonId::DPadRight;
        return true;
    }
    if (s == "quick_use_3")
    {
        if (outDirectButtonFallback) *outDirectButtonFallback = gpbind::ButtonId::DPadDown;
        return true;
    }
    if (s == "quick_use_4")
    {
        outAct = gpbind::ActionId::DetectorToggle; // DPadLeft default, fallback DPadLeft
        if (outDirectButtonFallback) *outDirectButtonFallback = gpbind::ButtonId::DPadLeft;
        return true;
    }

    // Core World_Gameplay actions used in $$ACTION_<name>$$ across G.A.M.M.A. XMLs and HUD
    if (s == "use" || s == "wpn_reload")
    {
        outAct = gpbind::ActionId::ContextUseReload;
        return true;
    }
    if (s == "sprint_toggle")
    {
        outAct = gpbind::ActionId::Sprint;
        return true;
    }
    if (s == "jump")
    {
        outAct = gpbind::ActionId::Jump;
        return true;
    }
    if (s == "crouch")
    {
        outAct = gpbind::ActionId::Crouch;
        return true;
    }
    if (s == "accel")
    {
        outAct = gpbind::ActionId::LowCrouch;
        return true;
    }
    if (s == "freelook")
    {
        outAct = gpbind::ActionId::Freelook;
        return true;
    }
    if (s == "wpn_fire")
    {
        outAct = gpbind::ActionId::Fire;
        return true;
    }
    if (s == "wpn_zoom")
    {
        outAct = gpbind::ActionId::Aim;
        return true;
    }
    if (s == "wpn_next")
    {
        outAct = gpbind::ActionId::AmmoTypeNext;
        return true;
    }
    if (s == "wpn_func")
    {
        outAct = gpbind::ActionId::UnderbarrelToggle;
        return true;
    }
    if (s == "wpn_firemode_next" || s == "wpn_firemode_prev")
    {
        outAct = gpbind::ActionId::FireModeNext;
        return true;
    }
    if (s == "next_slot" || s == "prev_slot" || s == "wpn_1" || s == "wpn_2" || s == "wpn_3")
    {
        outAct = gpbind::ActionId::NextWeapon;
        return true;
    }
    if (s == "wpn_4")
    {
        outAct = gpbind::ActionId::GrenadeQuick;
        return true;
    }
    if (s == "wpn_5" || s == "wpn_6" || s == "artefact" || s == "inventory")
    {
        outAct = gpbind::ActionId::WeaponWheel;
        return true;
    }
    if (s == "llookout")
    {
        outAct = gpbind::ActionId::LeanLeft;
        return true;
    }
    if (s == "rlookout")
    {
        outAct = gpbind::ActionId::LeanRight;
        return true;
    }
    if (s == "torch")
    {
        outAct = gpbind::ActionId::TorchToggle;
        return true;
    }
    if (s == "night_vision")
    {
        outAct = gpbind::ActionId::NightVision;
        return true;
    }
    if (s == "show_detector")
    {
        outAct = gpbind::ActionId::DetectorToggle;
        return true;
    }
    if (s == "cam_zoom_in")
    {
        outAct = gpbind::ActionId::MaskClean;
        return true;
    }
    if (s == "active_jobs" || s == "scores")
    {
        outAct = gpbind::ActionId::PdaFullToggle;
        return true;
    }
    if (s == "quit" || s == "pause")
    {
        outAct = gpbind::ActionId::PauseMenu;
        return true;
    }
    if (s == "custom19")
    {
        outAct = gpbind::ActionId::GrenadeQuick;
        return true;
    }
    if (s == "custom20")
    {
        outAct = gpbind::ActionId::Pda3DToggle;
        return true;
    }
    if (s == "custom21")
    {
        outAct = gpbind::ActionId::UnderbarrelToggle;
        return true;
    }
    if (s == "custom24")
    {
        outAct = gpbind::ActionId::MeleeQuick;
        return true;
    }

    // Also allow direct gpbind ActionId names (e.g. "ContextUseReload", "Confirm", "Back", "Menu")
    const gpbind::ActionId directAct = gpbind::BindingProfileModel::StringToAction(s);
    if (directAct != gpbind::ActionId::Invalid)
    {
        outAct = directAct;
        if (directAct == gpbind::ActionId::Menu || directAct == gpbind::ActionId::Confirm ||
            directAct == gpbind::ActionId::Back || directAct == gpbind::ActionId::ContextMenu ||
            directAct == gpbind::ActionId::QuickAction || directAct == gpbind::ActionId::TabPrev ||
            directAct == gpbind::ActionId::TabNext || directAct == gpbind::ActionId::PagePrev ||
            directAct == gpbind::ActionId::PageNext)
        {
            outCtx = gpbind::ContextId::MenuUI;
        }
        return true;
    }

    return false;
}

bool CGamepadGlyphManager::FormatEngineActionPrompt(const char* engineActionName,
                                                    bool isRussian,
                                                    char* dstBuff,
                                                    int dstBuffSize) const
{
    if (!dstBuff || dstBuffSize <= 0 || !engineActionName || !engineActionName[0])
        return false;

    const std::string lowName = LowerStr(TrimStr(engineActionName));

    // Analog movement/look actions (if queried via $$ACTION_FORWARD$$ etc.)
    if (lowName == "forward" || lowName == "back" || lowName == "lstrafe" || lowName == "rstrafe")
    {
        xr_strcpy(dstBuff, dstBuffSize, "[LS]");
        return true;
    }
    if (lowName == "left" || lowName == "right" || lowName == "up" || lowName == "down")
    {
        xr_strcpy(dstBuff, dstBuffSize, "[RS]");
        return true;
    }

    // Quick slots 1..4 use compact badge labels so they fit cleanly inside the small HUD/Inventory slot boxes
    if (lowName == "quick_use_1")
    {
        xr_strcpy(dstBuff, dstBuffSize, GetButtonLabel(gpbind::ButtonId::DPadUp, isRussian, true));
        return true;
    }
    if (lowName == "quick_use_2")
    {
        xr_strcpy(dstBuff, dstBuffSize, GetButtonLabel(gpbind::ButtonId::DPadRight, isRussian, true));
        return true;
    }
    if (lowName == "quick_use_3")
    {
        xr_strcpy(dstBuff, dstBuffSize, GetButtonLabel(gpbind::ButtonId::DPadDown, isRussian, true));
        return true;
    }
    if (lowName == "quick_use_4")
    {
        xr_strcpy(dstBuff, dstBuffSize, GetButtonLabel(gpbind::ButtonId::DPadLeft, isRussian, true));
        return true;
    }

    gpbind::ContextId ctx = gpbind::ContextId::World_Gameplay;
    gpbind::ActionId act  = gpbind::ActionId::Invalid;
    gpbind::ButtonId fallbackBtn = gpbind::ButtonId::None;

    if (!MapEngineActionToGamepadBinding(engineActionName, ctx, act, &fallbackBtn))
        return false;

    if (act != gpbind::ActionId::Invalid)
    {
        const gpbind::BindingTrigger* trig = gpbind::GetActiveBindingProfile().FindBinding(ctx, act);
        if (trig && trig->button != gpbind::ButtonId::None)
        {
            FormatTriggerPrompt(*trig, isRussian, false, dstBuff, dstBuffSize);
            return true;
        }
    }

    if (fallbackBtn != gpbind::ButtonId::None)
    {
        xr_strcpy(dstBuff, dstBuffSize, GetButtonLabel(fallbackBtn, isRussian, false));
        return true;
    }

    return false;
}

const char* CGamepadGlyphManager::GetEngineActionTextureId(const char* engineActionName) const
{
    if (!engineActionName || !engineActionName[0])
        return "";

    const char* prefix = "xbox";
    switch (m_effectiveSet)
    {
    case EGamepadGlyphSet::PlayStation: prefix = "ps";       break;
    case EGamepadGlyphSet::Nintendo:    prefix = "nintendo"; break;
    case EGamepadGlyphSet::Generic:     prefix = "generic";  break;
    case EGamepadGlyphSet::Xbox:
    default:                            prefix = "xbox";     break;
    }
    const char* styleStr = IconStyleToString(m_iconStyle);

    const std::string lowName = LowerStr(TrimStr(engineActionName));
    if (lowName == "forward" || lowName == "back" || lowName == "lstrafe" || lowName == "rstrafe")
    {
        static thread_local char s_lsBuf[64];
        xr_sprintf(s_lsBuf, sizeof(s_lsBuf), "ui_gp_%s_%s_ls", styleStr, prefix);
        return s_lsBuf;
    }
    if (lowName == "left" || lowName == "right" || lowName == "up" || lowName == "down")
    {
        static thread_local char s_rsBuf[64];
        xr_sprintf(s_rsBuf, sizeof(s_rsBuf), "ui_gp_%s_%s_rs", styleStr, prefix);
        return s_rsBuf;
    }

    // Also allow direct button name lookup (e.g. "South", "A", "LB", "RT")
    const gpbind::ButtonId directBtn = gpbind::BindingProfileModel::StringToButton(engineActionName);
    if (directBtn != gpbind::ButtonId::None)
        return GetButtonTextureId(directBtn);

    gpbind::ContextId ctx = gpbind::ContextId::World_Gameplay;
    gpbind::ActionId act  = gpbind::ActionId::Invalid;
    gpbind::ButtonId fallbackBtn = gpbind::ButtonId::None;
    if (!MapEngineActionToGamepadBinding(engineActionName, ctx, act, &fallbackBtn))
        return "";

    if (act != gpbind::ActionId::Invalid)
    {
        const gpbind::BindingTrigger* trig = gpbind::GetActiveBindingProfile().FindBinding(ctx, act);
        if (trig && trig->button != gpbind::ButtonId::None)
            return GetButtonTextureId(trig->button);
    }
    if (fallbackBtn != gpbind::ButtonId::None)
        return GetButtonTextureId(fallbackBtn);

    return "";
}

void CGamepadGlyphManager::LogStatus() const
{
    string_path ltxPath = {};
    ResolveProfileLtxPath(ltxPath);

    char usePromptEn[64]    = {};
    char sprintPromptEn[64] = {};
    char jumpPromptEn[64]   = {};
    FormatEngineActionPrompt("USE", false, usePromptEn, sizeof(usePromptEn));
    FormatEngineActionPrompt("SPRINT_TOGGLE", false, sprintPromptEn, sizeof(sprintPromptEn));
    FormatEngineActionPrompt("JUMP", false, jumpPromptEn, sizeof(jumpPromptEn));

    Msg("* [Gamepad] gp_glyphs status: override='%s' effective_set='%s' face='%s' revision=%u last_reason='%s'",
        GlyphSetToString(m_styleOverride),
        GlyphSetToString(m_effectiveSet),
        FaceStyleToString(m_effectiveFaceStyle),
        m_promptRevision,
        m_lastDirtyReason);
    Msg("* [Gamepad] gp_glyphs sample prompts: USE='%s' (tex='%s'), SPRINT_TOGGLE='%s' (tex='%s'), JUMP='%s' (tex='%s'), ltx='%s'",
        usePromptEn,
        GetEngineActionTextureId("USE"),
        sprintPromptEn,
        GetEngineActionTextureId("SPRINT_TOGGLE"),
        jumpPromptEn,
        GetEngineActionTextureId("JUMP"),
        ltxPath);
}

bool CGamepadGlyphManager::RunSelfTest() const
{
    u32 passed = 0;
    u32 failed = 0;

    auto check = [&](bool cond, const char* desc)
    {
        if (cond)
        {
            ++passed;
        }
        else
        {
            ++failed;
            Msg("! [Gamepad] gp_glyphs selftest FAIL: %s", desc);
        }
    };

    // 1. Verify SDL_GamepadType + SDL_GamepadButtonLabel resolution (R4 glyph_logic)
    EGamepadGlyphSet set = EGamepadGlyphSet::Auto;
    EGamepadFaceStyle face = EGamepadFaceStyle::ABXY;

    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_XBOXONE, static_cast<int>(EGamepadClass::ClassA),
                               SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B,
                               SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y,
                               EGamepadGlyphSet::Auto, set, face);
    check(set == EGamepadGlyphSet::Xbox && face == EGamepadFaceStyle::ABXY, "XboxOne -> Xbox/ABXY");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::West, set, face, false, false), "[X]") == 0, "Xbox West -> [X]");
    check(xr_strcmp(GetButtonTextureIdForStyle(gpbind::ButtonId::West, set, face), "ui_gp_white_xbox_west") == 0, "Xbox West tex");

    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_PS5, static_cast<int>(EGamepadClass::ClassA),
                               SDL_GAMEPAD_BUTTON_LABEL_CROSS, SDL_GAMEPAD_BUTTON_LABEL_CIRCLE,
                               SDL_GAMEPAD_BUTTON_LABEL_SQUARE, SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE,
                               EGamepadGlyphSet::Auto, set, face);
    check(set == EGamepadGlyphSet::PlayStation && face == EGamepadFaceStyle::Sony, "PS5 -> PlayStation/Sony");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::West, set, face, false, false), "[Square]") == 0, "PS5 West -> [Square]");
    check(xr_strcmp(GetButtonTextureIdForStyle(gpbind::ButtonId::West, set, face), "ui_gp_white_ps_west") == 0, "PS5 West tex");
    check(xr_strcmp(GetButtonTextureIdForStyle(gpbind::ButtonId::West, set, face, EGamepadIconStyle::Dark, true), "ui_gp_dark_key_ps_west") == 0, "PS5 Dark Key West tex");

    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, static_cast<int>(EGamepadClass::ClassA),
                               SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_A,
                               SDL_GAMEPAD_BUTTON_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_X,
                               EGamepadGlyphSet::Auto, set, face);
    check(set == EGamepadGlyphSet::Nintendo && face == EGamepadFaceStyle::BAYX, "SwitchPro -> Nintendo/BAYX");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::South, set, face, false, false), "[B]") == 0, "Switch South -> [B]");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::East, set, face, false, false), "[A]") == 0, "Switch East -> [A]");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::West, set, face, false, false), "[Y]") == 0, "Switch West -> [Y]");
    check(xr_strcmp(GetButtonTextureIdForStyle(gpbind::ButtonId::West, set, face, EGamepadIconStyle::Color, false), "ui_gp_color_nintendo_west") == 0, "Switch Color West tex");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::LT, set, face, false, false), "[ZL]") == 0, "Switch LT -> [ZL]");

    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_GAMECUBE, static_cast<int>(EGamepadClass::ClassA),
                               SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_X,
                               SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_Y,
                               EGamepadGlyphSet::Auto, set, face);
    check(face == EGamepadFaceStyle::AXBY, "GameCube -> AXBY");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::East, set, face, false, false), "[X]") == 0, "GameCube East -> [X]");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::West, set, face, false, false), "[B]") == 0, "GameCube West -> [B]");

    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_STANDARD, static_cast<int>(EGamepadClass::ClassB),
                               SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN,
                               SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN,
                               EGamepadGlyphSet::Auto, set, face);
    check(set == EGamepadGlyphSet::Generic && face == EGamepadFaceStyle::Numbered, "ClassB Standard -> Generic/Numbered");
    check(xr_strcmp(GetButtonLabelForStyle(gpbind::ButtonId::West, set, face, false, false), "[B3]") == 0, "Generic West -> [B3]");
    check(xr_strcmp(GetButtonTextureIdForStyle(gpbind::ButtonId::West, set, face), "ui_gp_white_generic_west") == 0, "Generic West tex");

    // 2. Manual override supersedes SDL auto-detection
    ResolveForSDLTypeAndLabels(SDL_GAMEPAD_TYPE_XBOXONE, static_cast<int>(EGamepadClass::ClassA),
                               SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B,
                               SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y,
                               EGamepadGlyphSet::PlayStation, set, face);
    check(set == EGamepadGlyphSet::PlayStation && face == EGamepadFaceStyle::Sony, "Manual override PlayStation on XboxOne");

    // 3. Engine action mapping for $$ACTION_USE$$, $$ACTION_SPRINT_TOGGLE$$, $$ACTION_QUICK_USE_1..4$$
    char buf[64] = {};
    check(FormatEngineActionPrompt("USE", false, buf, sizeof(buf)) && buf[0] != '\0', "$$ACTION_USE$$ mapped");
    check(FormatEngineActionPrompt("SPRINT_TOGGLE", false, buf, sizeof(buf)) && buf[0] != '\0', "$$ACTION_SPRINT_TOGGLE$$ mapped");
    check(FormatEngineActionPrompt("QUICK_USE_1", false, buf, sizeof(buf)) && xr_strcmp(buf, "D-U") == 0, "$$ACTION_QUICK_USE_1$$ mapped");
    check(FormatEngineActionPrompt("QUICK_USE_2", false, buf, sizeof(buf)) && xr_strcmp(buf, "D-R") == 0, "$$ACTION_QUICK_USE_2$$ mapped");
    check(FormatEngineActionPrompt("QUICK_USE_3", false, buf, sizeof(buf)) && xr_strcmp(buf, "D-D") == 0, "$$ACTION_QUICK_USE_3$$ mapped");
    check(FormatEngineActionPrompt("QUICK_USE_4", false, buf, sizeof(buf)) && xr_strcmp(buf, "D-L") == 0, "$$ACTION_QUICK_USE_4$$ mapped");

    Msg("* [Gamepad] gp_glyphs selftest finished: passed=%u failed=%u (%s)",
        passed, failed, (failed == 0) ? "PASS" : "FAIL");
    return failed == 0;
}
