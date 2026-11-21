#pragma warning(disable: 4995)
#ifndef GPOSK_STANDALONE_TEST
#include "stdafx.h"
#endif
#include "GamepadOnScreenKeyboard.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifndef GPOSK_STANDALONE_TEST
#include "PauseTokenManager.h"
#include "GamepadContextDispatcher.h"
#include "../xrEngine/GamepadMath.h"
#include "../xrEngine/device.h"
#include "../xrEngine/xr_input.h"
#include "UIGameCustom.h"
#include "MainMenu.h"
#include "Level.h"
#include "ai_space.h"
#include "script_engine.h"
#include "../xrEngine/GamepadGlyphs.h"
#include "ui/UITextureMaster.h"
#include "string_table.h"
#include "date_time.h"
#include "ui/UIDialogWnd.h"
#include "ui/UIStatic.h"
#include "ui/UICustomEdit.h"
#include "ui/UIEditBox.h"
#include "ui_base.h"
#include "UICursor.h"
#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/UIShader.h"
#include <dinput.h>
#endif

namespace gposk
{
    namespace
    {
        FOskTranslateCallback g_oskTranslateCb = nullptr;
        bool g_inOskSelfTest = false;

        const char* TrOsk(const char* stringId, const char* fallbackCp1251)
        {
            if (g_inOskSelfTest)
                return fallbackCp1251;
            if (g_oskTranslateCb)
            {
                const char* res = g_oskTranslateCb(stringId, fallbackCp1251);
                if (res && res[0])
                    return res;
            }
#ifndef GPOSK_STANDALONE_TEST
            if (CStringTable::getPData())
            {
                const shared_str tr = CStringTable().translate(stringId);
                if (tr.size() && 0 != xr_strcmp(tr.c_str(), stringId))
                {
                    static thread_local char s_oskRing[8][512];
                    static thread_local u32 s_oskRingIdx = 0;
                    char* slot = s_oskRing[s_oskRingIdx++ & 7u];
                    xr_strcpy(slot, 512, tr.c_str());
                    return slot;
                }
            }
#endif
            return fallbackCp1251;
        }

        TemplateItem MakeLocalizedTemplate(const char* stringId, const char* fallbackCp1251)
        {
            const char* s = TrOsk(stringId, fallbackCp1251);
            return { s, s };
        }
    } // namespace

    void SetOskTranslateCallback(FOskTranslateCallback cb)
    {
        g_oskTranslateCb = cb;
    }

    namespace
    {
        // Официальная таблица Windows-1251 для верхнего диапазона 0x80..0xBF (64 символа).
        // Диапазон 0xC0..0xFF в Windows-1251 линейно соответствует U+0410..U+044F (А..Я, а..я).
        constexpr uint16_t kCp1251High80ToBF[64] = {
            0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80..0x87
            0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 0x88..0x8F
            0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90..0x97 (0x96=–, 0x97=—)
            0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 0x98..0x9F
            0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // 0xA0..0xA7
            0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // 0xA8..0xAF (0xA8=Ё)
            0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // 0xB0..0xB7
            0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457  // 0xB8..0xBF (0xB8=ё, 0xB9=№)
        };

        // Цифровой ряд (row = 1, 12 клавиш)
        constexpr uint8_t kRowDigitsLower[12] = {
            '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '_'
        };
        constexpr uint8_t kRowDigitsUpper[12] = {
            '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', '='
        };

        // Русская раскладка ЙЦУКЕН (33 буквы: а..я + ё в нижнем и верхнем регистрах = все 66 русских букв)
        // Row 2 (верхний ряд): й ц у к е н г ш щ з х ъ
        constexpr uint8_t kRuRowTopLower[12] = {
            0xE9, 0xF6, 0xF3, 0xEA, 0xE5, 0xED, 0xE3, 0xF8, 0xF9, 0xE7, 0xF5, 0xFA
        };
        constexpr uint8_t kRuRowTopUpper[12] = {
            0xC9, 0xD6, 0xD3, 0xCA, 0xC5, 0xCD, 0xC3, 0xD8, 0xD9, 0xC7, 0xD5, 0xDA
        };

        // Row 3 (средний ряд): ф ы в а п р о л д ж э ё
        constexpr uint8_t kRuRowHomeLower[12] = {
            0xF4, 0xFB, 0xE2, 0xE0, 0xEF, 0xF0, 0xEE, 0xEB, 0xE4, 0xE6, 0xFD, 0xB8
        };
        constexpr uint8_t kRuRowHomeUpper[12] = {
            0xD4, 0xDB, 0xC2, 0xC0, 0xCF, 0xD0, 0xCE, 0xCB, 0xC4, 0xC6, 0xDD, 0xA8
        };

        // Row 4 (нижний ряд): я ч с м и т ь б ю . , ! (в верхнем регистре: Я Ч С М И Т Ь Б Ю - _ ?)
        constexpr uint8_t kRuRowBottomLower[12] = {
            0xFF, 0xF7, 0xF1, 0xEC, 0xE8, 0xF2, 0xFC, 0xE1, 0xFE, '.', ',', '!'
        };
        constexpr uint8_t kRuRowBottomUpper[12] = {
            0xDF, 0xD7, 0xD1, 0xCC, 0xC8, 0xD2, 0xDC, 0xC1, 0xDE, '-', '_', '?'
        };

        // Английская раскладка QWERTY (26 букв a..z / A..Z + знаки)
        constexpr uint8_t kEnRowTopLower[12] = {
            'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']'
        };
        constexpr uint8_t kEnRowTopUpper[12] = {
            'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '(', ')'
        };

        constexpr uint8_t kEnRowHomeLower[12] = {
            'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '/'
        };
        constexpr uint8_t kEnRowHomeUpper[12] = {
            'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '?'
        };

        constexpr uint8_t kEnRowBottomLower[12] = {
            'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', '_', '!'
        };
        constexpr uint8_t kEnRowBottomUpper[12] = {
            'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '+', '=', '#'
        };

        uint8_t LookupKeyCharCp1251(OskLanguage lang, bool upperCase, int32_t row, int32_t col) noexcept
        {
            if (col < 0 || col >= OnScreenKeyboardController::kKeyCols)
                return 0;

            if (row == OnScreenKeyboardController::kDigitRow)
                return upperCase ? kRowDigitsUpper[col] : kRowDigitsLower[col];

            if (lang == OskLanguage::RU)
            {
                if (row == OnScreenKeyboardController::kLetterRowTop)
                    return upperCase ? kRuRowTopUpper[col] : kRuRowTopLower[col];
                if (row == OnScreenKeyboardController::kLetterRowHome)
                    return upperCase ? kRuRowHomeUpper[col] : kRuRowHomeLower[col];
                if (row == OnScreenKeyboardController::kLetterRowBottom)
                    return upperCase ? kRuRowBottomUpper[col] : kRuRowBottomLower[col];
            }
            else
            {
                if (row == OnScreenKeyboardController::kLetterRowTop)
                    return upperCase ? kEnRowTopUpper[col] : kEnRowTopLower[col];
                if (row == OnScreenKeyboardController::kLetterRowHome)
                    return upperCase ? kEnRowHomeUpper[col] : kEnRowHomeLower[col];
                if (row == OnScreenKeyboardController::kLetterRowBottom)
                    return upperCase ? kEnRowBottomUpper[col] : kEnRowBottomLower[col];
            }
            return 0;
        }
    } // namespace

    // =========================================================================
    // 1. Преобразование UTF-8 <-> Windows-1251 и проверка всех 66 русских букв
    // =========================================================================

    uint32_t Cp1251ByteToCodepoint(uint8_t b) noexcept
    {
        if (b < 0x80)
            return static_cast<uint32_t>(b);
        if (b >= 0xC0)
            return 0x0410u + static_cast<uint32_t>(b - 0xC0);
        return static_cast<uint32_t>(kCp1251High80ToBF[b - 0x80]);
    }

    bool CodepointToCp1251Byte(uint32_t codepoint, uint8_t& outByte) noexcept
    {
        if (codepoint < 0x80)
        {
            outByte = static_cast<uint8_t>(codepoint);
            return true;
        }
        // Русские буквы А..Я (U+0410..U+042F -> 0xC0..0xDF) и а..я (U+0430..U+044F -> 0xE0..0xFF)
        if (codepoint >= 0x0410 && codepoint <= 0x044F)
        {
            outByte = static_cast<uint8_t>(0xC0 + (codepoint - 0x0410));
            return true;
        }
        // Ё (U+0401 -> 0xA8) и ё (U+0451 -> 0xB8)
        if (codepoint == 0x0401)
        {
            outByte = kCp1251CapitalYo;
            return true;
        }
        if (codepoint == 0x0451)
        {
            outByte = kCp1251SmallYo;
            return true;
        }
        for (uint8_t i = 0; i < 64; ++i)
        {
            if (kCp1251High80ToBF[i] == codepoint)
            {
                outByte = static_cast<uint8_t>(0x80 + i);
                return true;
            }
        }
        return false;
    }

    std::string Utf8ToCp1251(std::string_view utf8, size_t* outUnmappedCount)
    {
        size_t unmapped = 0;
        std::string result;
        result.reserve(utf8.size());

        size_t i = 0;
        const size_t n = utf8.size();
        while (i < n)
        {
            const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
            if (b0 < 0x80)
            {
                result.push_back(static_cast<char>(b0));
                ++i;
                continue;
            }

            uint32_t cp = 0;
            size_t seqLen = 0;
            if ((b0 & 0xE0) == 0xC0 && (i + 1) < n)
            {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                if ((b1 & 0xC0) == 0x80)
                {
                    cp = ((static_cast<uint32_t>(b0 & 0x1F) << 6) |
                          static_cast<uint32_t>(b1 & 0x3F));
                    if (cp >= 0x80)
                        seqLen = 2;
                }
            }
            else if ((b0 & 0xF0) == 0xE0 && (i + 2) < n)
            {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80)
                {
                    cp = ((static_cast<uint32_t>(b0 & 0x0F) << 12) |
                          (static_cast<uint32_t>(b1 & 0x3F) << 6) |
                          static_cast<uint32_t>(b2 & 0x3F));
                    if (cp >= 0x800)
                        seqLen = 3;
                }
            }
            else if ((b0 & 0xF8) == 0xF0 && (i + 3) < n)
            {
                const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
                const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80)
                {
                    cp = ((static_cast<uint32_t>(b0 & 0x07) << 18) |
                          (static_cast<uint32_t>(b1 & 0x3F) << 12) |
                          (static_cast<uint32_t>(b2 & 0x3F) << 6) |
                          static_cast<uint32_t>(b3 & 0x3F));
                    if (cp >= 0x10000)
                        seqLen = 4;
                }
            }

            if (seqLen > 0)
            {
                uint8_t mappedByte = 0;
                if (CodepointToCp1251Byte(cp, mappedByte))
                {
                    result.push_back(static_cast<char>(mappedByte));
                }
                else
                {
                    ++unmapped;
                }
                i += seqLen;
            }
            else
            {
                // Некорректная UTF-8 последовательность
                ++unmapped;
                ++i;
            }
        }

        if (outUnmappedCount)
            *outUnmappedCount = unmapped;
        return result;
    }

    std::string Cp1251ToUtf8(std::string_view cp1251)
    {
        std::string out;
        out.reserve(cp1251.size() * 2);

        for (char c : cp1251)
        {
            const uint8_t b = static_cast<uint8_t>(c);
            const uint32_t cp = Cp1251ByteToCodepoint(b);
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    bool IsValidSaveFileNameCp1251(std::string_view cp1251) noexcept
    {
        if (cp1251.empty())
            return false;

        for (char c : cp1251)
        {
            // Объединение запрещённых символов из ui_save_dialog.script:176 (/ . \ % ? * : | " < >)
            // и valid_saved_game_name в console_commands.cpp:1032 (/ \ : * ? " < > | ^ ( ) [ ] %)
            switch (c)
            {
            case '/':
            case '.':
            case '\\':
            case '%':
            case '?':
            case '*':
            case ':':
            case '|':
            case '"':
            case '<':
            case '>':
            case '^':
            case '(':
            case ')':
            case '[':
            case ']':
                return false;
            default:
                break;
            }
            if (static_cast<uint8_t>(c) < 0x20)
                return false;
        }
        return true;
    }

    std::string SanitizeSaveFileNameCp1251(std::string_view cp1251)
    {
        std::string raw;
        raw.reserve(cp1251.size());

        for (char c : cp1251)
        {
            const uint8_t b = static_cast<uint8_t>(c);
            if (b < 0x20)
                continue;

            // Заменяем разделители дат/времени ('.', ':') и слеши на дефис '-', чтобы не нарушать
            // проверку ui_save_dialog.script:176 и valid_saved_game_name (console_commands.cpp:1032)
            if (c == '.' || c == ':' || c == '/' || c == '\\')
            {
                raw.push_back('-');
                continue;
            }
            // Остальные запрещённые символы опускаем
            if (c == '%' || c == '?' || c == '*' || c == '|' || c == '"' ||
                c == '<' || c == '>' || c == '^' || c == '(' || c == ')' ||
                c == '[' || c == ']')
            {
                continue;
            }
            raw.push_back(c);
        }

        // Убираем начальные/конечные пробелы и схлопываем двойные пробелы
        std::string cleaned;
        cleaned.reserve(raw.size());
        bool prevSpace = true;
        for (char c : raw)
        {
            if (c == ' ')
            {
                if (!prevSpace)
                {
                    cleaned.push_back(' ');
                    prevSpace = true;
                }
            }
            else
            {
                cleaned.push_back(c);
                prevSpace = false;
            }
        }
        while (!cleaned.empty() && cleaned.back() == ' ')
        {
            cleaned.pop_back();
        }
        return cleaned;
    }

    bool VerifyAll66RussianLettersRoundtrip(std::string* outReport)
    {
        // Формируем эталонный массив всех 66 русских букв в Windows-1251:
        // 32 заглавные А..Я (0xC0..0xDF), 1 заглавная Ё (0xA8),
        // 32 строчные а..я (0xE0..0xFF), 1 строчная ё (0xB8).
        std::array<uint8_t, 66> expectedCp1251{};
        size_t idx = 0;
        for (uint8_t b = 0xC0; b <= 0xDF; ++b)
            expectedCp1251[idx++] = b;
        expectedCp1251[idx++] = kCp1251CapitalYo; // Ё (0xA8)
        for (uint8_t b = 0xE0; ; ++b)
        {
            expectedCp1251[idx++] = b;
            if (b == 0xFF)
                break;
        }
        expectedCp1251[idx++] = kCp1251SmallYo; // ё (0xB8)

        if (idx != 66)
            return false;

        std::string all66Cp1251(reinterpret_cast<const char*>(expectedCp1251.data()), expectedCp1251.size());

        // 1. Проверяем каждую из 66 букв по отдельности: CP1251 -> UTF-8 -> CP1251 и отсутствие '?' (0x3F)
        for (size_t i = 0; i < 66; ++i)
        {
            const uint8_t b = expectedCp1251[i];
            if (!IsRussianLetterCp1251(b))
                return false;

            const std::string singleCp(1, static_cast<char>(b));
            const std::string utf8 = Cp1251ToUtf8(singleCp);
            if (utf8.size() != 2 || utf8.find('?') != std::string::npos)
                return false;

            size_t unmapped = 999;
            const std::string roundtripCp = Utf8ToCp1251(utf8, &unmapped);
            if (unmapped != 0 || roundtripCp.size() != 1 ||
                static_cast<uint8_t>(roundtripCp[0]) != b ||
                roundtripCp.find('?') != std::string::npos)
            {
                return false;
            }
        }

        // 2. Проверяем полную 66-буквенную строку целиком туда и обратно
        const std::string all66Utf8 = Cp1251ToUtf8(all66Cp1251);
        if (all66Utf8.size() != 132 || all66Utf8.find('?') != std::string::npos)
            return false;

        size_t totalUnmapped = 999;
        const std::string roundtripAll66Cp = Utf8ToCp1251(all66Utf8, &totalUnmapped);
        if (totalUnmapped != 0 || roundtripAll66Cp != all66Cp1251 || roundtripAll66Cp.find('?') != std::string::npos)
            return false;

        const std::string roundtripAll66Utf8 = Cp1251ToUtf8(roundtripAll66Cp);
        if (roundtripAll66Utf8 != all66Utf8)
            return false;

        // 3. Проверяем преобразование регистра ToLowerCp1251 / ToUpperCp1251 для всех 33 пар букв
        for (size_t i = 0; i < 33; ++i)
        {
            const uint8_t upperB = expectedCp1251[i];
            const uint8_t lowerB = expectedCp1251[i + 33];
            if (!IsRussianUpperLetterCp1251(upperB) || !IsRussianLowerLetterCp1251(lowerB))
                return false;
            if (ToLowerCp1251(upperB) != lowerB || ToUpperCp1251(lowerB) != upperB)
                return false;
        }

        // 4. Проверяем, что все 66 русских букв присутствуют на сетке клавиатуры RU (ЙЦУКЕН)
        OnScreenKeyboardController ctrl;
        ctrl.OpenSession(OskTargetKind::GenericEdit, "", 0, 100);
        for (size_t i = 0; i < 66; ++i)
        {
            const uint8_t targetB = expectedCp1251[i];
            const bool isUpper = IsRussianUpperLetterCp1251(targetB);
            int32_t r = -1, c = -1;
            if (!ctrl.FindCharOnGrid(OskLanguage::RU, isUpper, targetB, r, c))
                return false;
        }

        if (outReport)
        {
            *outReport = "PASS: all 66 Russian letters (32 A..Ya + 32 a..ya + Yo + yo) verified in CP1251<->UTF8 and on RU grid with 0 '?'";
        }
        return true;
    }

    // =========================================================================
    // 2. Классификация целевых окон (FP.1 п. 7) и шаблоны в 1 клик (D51)
    // =========================================================================

    const char* GetTargetKindName(OskTargetKind kind) noexcept
    {
        switch (kind)
        {
        case OskTargetKind::SaveDialog:  return "SaveDialog";
        case OskTargetKind::StashDialog: return "StashDialog";
        case OskTargetKind::WaypointPin: return "WaypointPin";
        case OskTargetKind::NewGameName: return "NewGameName";
        case OskTargetKind::NumericArgb: return "NumericArgb";
        case OskTargetKind::GenericEdit: return "GenericEdit";
        default:                         return "Unknown";
        }
    }

    OskTargetKind ClassifyEditTarget(std::string_view editWindowName,
                                     std::string_view parentWindowName,
                                     std::string_view dialogWindowName,
                                     bool inMainMenu,
                                     bool numbersOnly) noexcept
    {
        if (numbersOnly ||
            editWindowName == "fld_input_a" ||
            editWindowName == "fld_input_r" ||
            editWindowName == "fld_input_g" ||
            editWindowName == "fld_input_b")
        {
            return OskTargetKind::NumericArgb;
        }

        // 1. Окно сохранения игры UISaveDialog (ui_save_dialog.script:58-60, edit_filename)
        if (editWindowName == "edit_filename" ||
            dialogWindowName == "UISaveDialog" ||
            parentWindowName == "form")
        {
            return OskTargetKind::SaveDialog;
        }

        // 2. Имя нового персонажа в начале игры UINewGame (ui_mm_faction_select.script:541-543, input_name)
        if (editWindowName == "input_name" ||
            dialogWindowName == "UINewGame" ||
            (inMainMenu && parentWindowName == "dialog"))
        {
            return OskTargetKind::NewGameName;
        }

        // 3. Метка на карте КПК / Путевая точка UIWPSettings и UIPinSettingsWnd (tasks_placeable_waypoints.script)
        if (editWindowName == "fld_input_name" ||
            editWindowName == "fld_input_desc" ||
            parentWindowName == "wp_dialog" ||
            parentWindowName == "bg_wpdesc" ||
            parentWindowName == "pin_dialog" ||
            dialogWindowName == "UIWPSettings" ||
            dialogWindowName == "UIPinSettingsWnd" ||
            (editWindowName.rfind("fld_input", 0) == 0 && editWindowName.size() > 9))
        {
            return OskTargetKind::WaypointPin;
        }

        // 4. Создание личного тайника UICreateStash (item_backpack.script:203-206, fld_input в backpack)
        if (editWindowName == "fld_input" ||
            parentWindowName == "backpack" ||
            dialogWindowName == "UICreateStash")
        {
            return OskTargetKind::StashDialog;
        }

        return OskTargetKind::GenericEdit;
    }

    std::string BuildSaveLocationDateTimeCp1251(std::string_view levelNameCp1251,
                                                std::string_view dateTimeCp1251)
    {
        // По умолчанию: "Зона" ("\xC7\xEE\xED\xE0") и "День 1 12-00" ("\xC4\xE5\xED\xFC\x20\x31\x20\x31\x32\x2D\x30\x30")
        const std::string safeLevel = levelNameCp1251.empty()
            ? SanitizeSaveFileNameCp1251(TrOsk("ui_gp_eng_osk_default_zone", "\xC7\xEE\xED\xE0"))
            : SanitizeSaveFileNameCp1251(levelNameCp1251);
        const std::string safeDate = dateTimeCp1251.empty()
            ? SanitizeSaveFileNameCp1251(TrOsk("ui_gp_eng_osk_default_day_time", "\xC4\xE5\xED\xFC\x20\x31\x20\x31\x32\x2D\x30\x30"))
            : SanitizeSaveFileNameCp1251(dateTimeCp1251);

        // Формат D51: "<Локация> — <Дата/Время>" (тире 0x97 в Windows-1251)
        std::string combined = safeLevel;
        combined.push_back(' ');
        combined.push_back(static_cast<char>(kCp1251EmDash));
        combined.push_back(' ');
        combined.append(safeDate);
        return SanitizeSaveFileNameCp1251(combined);
    }

    std::vector<TemplateItem> BuildTemplatesForTarget(OskTargetKind kind,
                                                      std::string_view dynamicLevelNameCp1251,
                                                      std::string_view dynamicDateTimeCp1251)
    {
        std::vector<TemplateItem> list;
        list.reserve(6);

        switch (kind)
        {
        case OskTargetKind::SaveDialog:
        {
            const std::string locDateVal = BuildSaveLocationDateTimeCp1251(dynamicLevelNameCp1251, dynamicDateTimeCp1251);
            // На самой плашке показываем укороченную подпись или саму строку, если она компактна
            const std::string locDateLabel = (locDateVal.size() <= 22)
                ? locDateVal
                : std::string(TrOsk("ui_gp_eng_osk_tmpl_loc_date", "\xCB\xEE\xEA\xE0\xF6\xE8\xFF\x20\x97\x20\xC4\xE0\xF2\xE0"));
            list.push_back({ locDateLabel, locDateVal });
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_before_fight", "\xCF\xE5\xF0\xE5\xE4\x20\xE1\xEE\xE5\xEC"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_after_emission", "\xCF\xEE\xF1\xEB\xE5\x20\xE2\xFB\xE1\xF0\xEE\xF1\xE0"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_campfire", "\xD3\x20\xEA\xEE\xF1\xF2\xF0\xE0"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_before_raid", "\xCF\xE5\xF0\xE5\xE4\x20\xE2\xFB\xEB\xE0\xE7\xEA\xEE\xE9"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_base_trade", "\xC1\xE0\xE7\xE0\x20\xE8\x20\xF2\xEE\xF0\xE3\xEE\xE2\xEB\xFF"));
            break;
        }
        case OskTargetKind::StashDialog:
        {
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_weapons", "\xCE\xF0\xF3\xE6\xE8\xE5"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_parts", "\xC4\xE5\xF2\xE0\xEB\xE8"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_meds", "\xCC\xE5\xE4\xE8\xF6\xE8\xED\xE0"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_ammo", "\xC1\xEE\xE5\xEF\xF0\xE8\xEF\xE0\xF1\xFB"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_armor", "\xC1\xF0\xEE\xED\xFF"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_artefacts", "\xC0\xF0\xF2\xE5\xF4\xE0\xEA\xF2\xFB"));
            break;
        }
        case OskTargetKind::WaypointPin:
        {
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_stash", "\xD2\xE0\xE9\xED\xE8\xEA"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_danger", "\xCE\xEF\xE0\xF1\xED\xEE"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_check", "\xCF\xF0\xEE\xE2\xE5\xF0\xE8\xF2\xFC"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_camp", "\xCB\xE0\xE3\xE5\xF0\xFC"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_trader", "\xD2\xEE\xF0\xE3\xEE\xE2\xE5\xF6"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_target", "\xD6\xE5\xEB\xFC"));
            break;
        }
        case OskTargetKind::NewGameName:
        {
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_stalker", "\xD1\xF2\xE0\xEB\xEA\xE5\xF0"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_marked_one", "\xCC\xE5\xF7\xE5\xED\xFB\xE9"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_strelok", "\xD1\xF2\xF0\xE5\xEB\xEE\xEA"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_degtyarev", "\xC4\xE5\xE3\xF2\xFF\xF0\xB8\xE2"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_scar", "\xD8\xF0\xE0\xEC"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_strider", "\xC1\xF0\xEE\xE4\xFF\xE3\xE0"));
            break;
        }
        case OskTargetKind::NumericArgb:
        {
            list.push_back({ "255", "255" });
            list.push_back({ "200", "200" });
            list.push_back({ "128", "128" });
            list.push_back({ "64", "64" });
            list.push_back({ "32", "32" });
            list.push_back({ "0", "0" });
            break;
        }
        case OskTargetKind::GenericEdit:
        default:
        {
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_weapons", "\xCE\xF0\xF3\xE6\xE8\xE5"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_meds", "\xCC\xE5\xE4\xE8\xF6\xE8\xED\xE0"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_parts", "\xC4\xE5\xF2\xE0\xEB\xE8"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_stash", "\xD2\xE0\xE9\xED\xE8\xEA"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_before_fight", "\xCF\xE5\xF0\xE5\xE4\x20\xE1\xEE\xE5\xEC"));
            list.push_back(MakeLocalizedTemplate("ui_gp_eng_osk_tmpl_stalker", "\xD1\xF2\xE0\xEB\xEA\xE5\xF0"));
            break;
        }
        }
        return list;
    }

    // =========================================================================
    // 3. Контроллер экранной клавиатуры (OnScreenKeyboardController)
    // =========================================================================

    void OnScreenKeyboardController::OpenSession(OskTargetKind kind,
                                                 std::string_view initialTextCp1251,
                                                 uint32_t heldButtonsMask,
                                                 uint32_t nowContinualMs,
                                                 std::string_view dynamicLevelNameCp1251,
                                                 std::string_view dynamicDateTimeCp1251,
                                                 size_t maxLen)
    {
        m_isOpen        = true;
        m_wasCommitted  = false;
        m_wasCancelled  = false;
        m_targetKind    = kind;
        m_maxLength     = (maxLen > 0) ? maxLen : kDefaultMaxLen;

        m_originalTextCp1251.assign(initialTextCp1251.data(), initialTextCp1251.size());
        if (m_originalTextCp1251.size() > m_maxLength)
            m_originalTextCp1251.resize(m_maxLength);

        m_bufferCp1251    = m_originalTextCp1251;
        m_committedTextCp1251.clear();
        m_templates       = BuildTemplatesForTarget(kind, dynamicLevelNameCp1251, dynamicDateTimeCp1251);

        // По умолчанию открываем на первой буквенной клавише ('й' / 'q'), либо на цифровом ряду для числовых полей
        m_focusRow        = (kind == OskTargetKind::NumericArgb) ? kDigitRow : kLetterRowTop;
        m_focusCol        = 0;
        m_lastKeyCol12    = 0;

        // Строим граф фокуса родительского поля (для снимка F1.2 CallerFocusSnapshot)
        m_callerFocusGraph.Clear();
        const uint32_t gid = m_callerFocusGraph.AddGroup("parent_dialog_edit", gpfocus::FocusLayoutMode::List1D);
        std::vector<gpfocus::FocusNode> editNodes(1);
        editNodes[0].node_id     = 1;
        editNodes[0].group_id    = gid;
        editNodes[0].kind        = gpfocus::FocusNodeKind::ControlWidget;
        editNodes[0].key.section = GetTargetKindName(kind);
        editNodes[0].key.list_index = 0;
        editNodes[0].label       = m_originalTextCp1251;
        m_callerFocusGraph.RebuildGroupItems(gid, editNodes, 0, nowContinualMs);
        m_callerFocusGraph.SwitchToGroup(gid, nowContinualMs);

        // Помещаем экранную клавиатуру как модальный слой в стек F1.2 с барьером consumed_until_release
        ++m_stepFrameId;
        m_modalStack = gpmodal::ModalStack{};
        m_modalStack.PushModal(
            "osk_modal",
            "OnScreenKeyboard",
            m_callerFocusGraph,
            1u,
            heldButtonsMask,
            nowContinualMs);

        m_navRepeatTimer.Reset();
        m_backspaceRepeatTimer.Reset();
        m_prevStartDown = false;
        m_prevL3Down    = false;
        m_prevR3Down    = false;
    }

    void OnScreenKeyboardController::SeedHeldExtraButtons(bool startDown, bool l3Down, bool r3Down) noexcept
    {
        m_prevStartDown = startDown;
        m_prevL3Down    = l3Down;
        m_prevR3Down    = r3Down;
    }

    int32_t OnScreenKeyboardController::GetColsForRow(int32_t row) const noexcept
    {
        if (row == kTemplateRow)
            return static_cast<int32_t>(m_templates.size());
        if (row == kActionRow)
            return kActionCols;
        if (row >= kDigitRow && row <= kLetterRowBottom)
            return kKeyCols;
        return 0;
    }

    bool OnScreenKeyboardController::SetFocusCell(int32_t row, int32_t col)
    {
        if (row < 0 || row >= kTotalRows)
            return false;
        const int32_t cols = GetColsForRow(row);
        if (cols <= 0 || col < 0 || col >= cols)
            return false;

        m_focusRow = row;
        m_focusCol = col;
        if (cols == kKeyCols)
            m_lastKeyCol12 = col;
        return true;
    }

    bool OnScreenKeyboardController::MoveFocus(gpfocus::NavDir dir)
    {
        if (!m_isOpen)
            return false;

        const int32_t curCols = GetColsForRow(m_focusRow);
        if (curCols <= 0)
            return false;

        if (dir == gpfocus::NavDir::Left)
        {
            m_focusCol = (m_focusCol - 1 + curCols) % curCols;
            if (curCols == kKeyCols)
                m_lastKeyCol12 = m_focusCol;
            return true;
        }
        if (dir == gpfocus::NavDir::Right)
        {
            m_focusCol = (m_focusCol + 1) % curCols;
            if (curCols == kKeyCols)
                m_lastKeyCol12 = m_focusCol;
            return true;
        }

        if (dir == gpfocus::NavDir::Up || dir == gpfocus::NavDir::Down)
        {
            const int32_t prevCols = curCols;
            if (prevCols == kKeyCols)
                m_lastKeyCol12 = m_focusCol;

            const int32_t step = (dir == gpfocus::NavDir::Down) ? 1 : -1;
            const int32_t nextRow = (m_focusRow + step + kTotalRows) % kTotalRows;
            const int32_t nextCols = GetColsForRow(nextRow);
            if (nextCols <= 0)
                return false;

            int32_t nextCol = m_focusCol;
            if (prevCols == kKeyCols && nextCols < kKeyCols)
            {
                nextCol = std::clamp(m_focusCol / 2, 0, nextCols - 1);
            }
            else if (prevCols < kKeyCols && nextCols == kKeyCols)
            {
                if ((m_lastKeyCol12 / 2) == m_focusCol)
                    nextCol = std::clamp(m_lastKeyCol12, 0, nextCols - 1);
                else
                    nextCol = std::clamp(m_focusCol * 2, 0, nextCols - 1);
            }
            else
            {
                nextCol = std::clamp(m_focusCol, 0, nextCols - 1);
            }

            m_focusRow = nextRow;
            m_focusCol = nextCol;
            if (nextCols == kKeyCols)
                m_lastKeyCol12 = m_focusCol;
            return true;
        }
        return false;
    }

    OskGridCellInfo OnScreenKeyboardController::GetCellInfo(int32_t row, int32_t col) const
    {
        OskGridCellInfo info{};
        info.row = row;
        info.col = col;

        if (row == kTemplateRow)
        {
            info.action = OskActionKind::ApplyTemplate;
            if (col >= 0 && static_cast<size_t>(col) < m_templates.size())
            {
                info.label_cp1251 = m_templates[static_cast<size_t>(col)].label_cp1251;
            }
            return info;
        }

        if (row >= kDigitRow && row <= kLetterRowBottom)
        {
            info.action = OskActionKind::None;
            info.char_cp1251 = LookupKeyCharCp1251(m_language, m_upperCase, row, col);
            if (info.char_cp1251 != 0)
                info.label_cp1251.assign(1, static_cast<char>(info.char_cp1251));
            return info;
        }

        if (row == kActionRow)
        {
            switch (col)
            {
            case 0:
                info.action = OskActionKind::ToggleLang;
                info.label_cp1251 = (m_language == OskLanguage::RU)
                    ? TrOsk("ui_gp_eng_osk_act_lang_ru", "\xDF\xE7\xFB\xEA\x3A\x20\x52\x55\x20\x28\x52\x33\x29")
                    : TrOsk("ui_gp_eng_osk_act_lang_en", "\xDF\xE7\xFB\xEA\x3A\x20\x45\x4E\x20\x28\x52\x33\x29");
                break;
            case 1:
                info.action = OskActionKind::ToggleCase;
                info.label_cp1251 = m_upperCase
                    ? TrOsk("ui_gp_eng_osk_act_case_upper", "\xD0\xE5\xE3\xE8\xF1\xF2\xF0\x3A\x20\xC0\xC1\xC2\x20\x28\x4C\x33\x29")
                    : TrOsk("ui_gp_eng_osk_act_case_lower", "\xD0\xE5\xE3\xE8\xF1\xF2\xF0\x3A\x20\xE0\xE1\xE2\x20\x28\x4C\x33\x29");
                break;
            case 2:
                info.action = OskActionKind::Space;
                info.label_cp1251 = TrOsk("ui_gp_eng_osk_act_space", "\xCF\xF0\xEE\xE1\xE5\xEB\x20\x28\x59\x29");
                break;
            case 3:
                info.action = OskActionKind::Backspace;
                info.label_cp1251 = TrOsk("ui_gp_eng_osk_act_backspace", "\x3C\x2D\x20\xC7\xE0\xE1\xEE\xE9\x20\x28\x58\x29");
                break;
            case 4:
                info.action = OskActionKind::Clear;
                info.label_cp1251 = TrOsk("ui_gp_eng_osk_act_clear", "\xCE\xF7\xE8\xF1\xF2\xE8\xF2\xFC");
                break;
            case 5:
            default:
                info.action = OskActionKind::Commit;
                info.label_cp1251 = TrOsk("ui_gp_eng_osk_act_done", "\xC3\xEE\xF2\xEE\xE2\xEE\x20\x28\x53\x74\x61\x72\x74\x29");
                break;
            }
        }
        return info;
    }

    bool OnScreenKeyboardController::AppendCharCp1251(uint8_t ch)
    {
        if (!m_isOpen || ch < 0x20 || m_bufferCp1251.size() >= m_maxLength)
            return false;

        if (m_targetKind == OskTargetKind::NumericArgb)
        {
            if (ch < '0' || ch > '9')
                return false;
        }
        else if (m_targetKind == OskTargetKind::SaveDialog)
        {
            const char c = static_cast<char>(ch);
            if (c == '.' || c == ':' || c == '/' || c == '\\')
            {
                ch = static_cast<uint8_t>('-');
            }
            else if (c == '%' || c == '?' || c == '*' || c == '|' || c == '"' ||
                     c == '<' || c == '>' || c == '^' || c == '(' || c == ')' ||
                     c == '[' || c == ']')
            {
                return false;
            }
        }

        m_bufferCp1251.push_back(static_cast<char>(ch));
        return true;
    }

    bool OnScreenKeyboardController::SetBufferCp1251(std::string_view textCp1251)
    {
        if (!m_isOpen)
            return false;

        std::string next(textCp1251.data(), textCp1251.size());
        if (m_targetKind == OskTargetKind::SaveDialog)
            next = SanitizeSaveFileNameCp1251(next);
        if (next.size() > m_maxLength)
            next.resize(m_maxLength);

        m_bufferCp1251 = std::move(next);
        return true;
    }

    bool OnScreenKeyboardController::PressBackspace()
    {
        if (!m_isOpen || m_bufferCp1251.empty())
            return false;
        m_bufferCp1251.pop_back();
        return true;
    }

    bool OnScreenKeyboardController::PressSpace()
    {
        if (m_targetKind == OskTargetKind::NumericArgb)
            return false;
        return AppendCharCp1251(static_cast<uint8_t>(' '));
    }

    void OnScreenKeyboardController::ToggleLanguage()
    {
        m_language = (m_language == OskLanguage::RU) ? OskLanguage::EN : OskLanguage::RU;
    }

    bool OnScreenKeyboardController::ApplyTemplateIndex(size_t idx)
    {
        if (!m_isOpen || idx >= m_templates.size())
            return false;
        return SetBufferCp1251(m_templates[idx].value_cp1251);
    }

    bool OnScreenKeyboardController::PressFocusedCell(uint32_t heldButtonsMask)
    {
        if (!m_isOpen)
            return false;

        const OskGridCellInfo cell = GetCellInfo(m_focusRow, m_focusCol);
        switch (cell.action)
        {
        case OskActionKind::ApplyTemplate:
            return ApplyTemplateIndex(static_cast<size_t>(m_focusCol));
        case OskActionKind::ToggleLang:
            ToggleLanguage();
            return true;
        case OskActionKind::ToggleCase:
            ToggleCase();
            return true;
        case OskActionKind::Space:
            return PressSpace();
        case OskActionKind::Backspace:
            return PressBackspace();
        case OskActionKind::Clear:
            ClearBuffer();
            return true;
        case OskActionKind::Commit:
            return CommitSession(heldButtonsMask);
        case OskActionKind::None:
        default:
            if (cell.char_cp1251 != 0)
                return AppendCharCp1251(cell.char_cp1251);
            return false;
        }
    }

    bool OnScreenKeyboardController::CommitSession(uint32_t heldButtonsMask)
    {
        if (!m_isOpen)
            return false;

        std::string finalStr = m_bufferCp1251;
        if (m_targetKind == OskTargetKind::SaveDialog)
        {
            finalStr = SanitizeSaveFileNameCp1251(finalStr);
        }
        else if (m_targetKind == OskTargetKind::NumericArgb)
        {
            if (finalStr.empty())
                finalStr = "0";
            else
            {
                int val = 0;
                for (char c : finalStr)
                {
                    if (c >= '0' && c <= '9')
                        val = std::min(255, val * 10 + (c - '0'));
                }
                finalStr = std::to_string(val);
            }
        }

        m_bufferCp1251        = finalStr;
        m_committedTextCp1251 = finalStr;
        m_wasCommitted        = true;
        m_wasCancelled        = false;
        m_isOpen              = false;

        const uint32_t exitMask = heldButtonsMask | gpmodal::ModalButtonMask(gpmodal::ModalButton::A);
        if (!m_modalStack.IsEmpty())
        {
            ++m_stepFrameId;
            m_modalStack.PopTopModal(
                m_callerFocusGraph,
                exitMask,
                m_stepFrameId,
                0,
                "osk_commit");
        }
        return true;
    }

    bool OnScreenKeyboardController::CancelSession(uint32_t heldButtonsMask)
    {
        if (!m_isOpen)
            return false;

        // B закрывает клавиатуру без ввода: буфер и возвращаемый текст откатываются к исходному m_originalTextCp1251
        m_bufferCp1251        = m_originalTextCp1251;
        m_committedTextCp1251 = m_originalTextCp1251;
        m_wasCommitted        = false;
        m_wasCancelled        = true;
        m_isOpen              = false;

        const uint32_t exitMask = heldButtonsMask | gpmodal::ModalButtonMask(gpmodal::ModalButton::B);
        if (!m_modalStack.IsEmpty())
        {
            ++m_stepFrameId;
            m_modalStack.PopTopModal(
                m_callerFocusGraph,
                exitMask,
                m_stepFrameId,
                0,
                "osk_cancel_B");
        }
        return true;
    }

    void OnScreenKeyboardController::StepController(uint32_t rawModalButtonsMask,
                                                    bool pressStart,
                                                    bool pressL3,
                                                    bool pressR3,
                                                    bool hasStickDir,
                                                    gpfocus::NavDir stickDir,
                                                    uint32_t nowContinualMs)
    {
        if (!m_isOpen)
            return;

        // 1. Обновляем барьер модального слоя F1.2 (зажатые при открытии кнопки игнорируются до отпускания)
        ++m_stepFrameId;
        m_modalStack.BeginFrameButtons(rawModalButtonsMask, m_stepFrameId, nowContinualMs);

        const bool startEdge = pressStart && !m_prevStartDown;
        const bool l3Edge    = pressL3 && !m_prevL3Down;
        const bool r3Edge    = pressR3 && !m_prevR3Down;
        m_prevStartDown      = pressStart;
        m_prevL3Down         = pressL3;
        m_prevR3Down         = pressR3;

        // 2. B — закрыть клавиатуру без ввода (F5.1 п. 2, D51)
        if (m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::B))
        {
            CancelSession(rawModalButtonsMask);
            return;
        }

        // 3. Start — немедленный ввод (подтверждение) из любой точки сетки (F5.1 п. 1, D51)
        if (startEdge)
        {
            CommitSession(rawModalButtonsMask);
            return;
        }

        // 4. Переключение регистра (L3 или LT) и языка (R3 или LB/RB)
        if (l3Edge || m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::LT))
        {
            ToggleCase();
        }
        if (r3Edge ||
            m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::LB) ||
            m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::RB))
        {
            ToggleLanguage();
        }

        // 5. Y — пробел (F5.1 п. 1, D51)
        if (m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::Y))
        {
            PressSpace();
        }

        // 6. X — удалить символ (Backspace) с поддержкой автоповтора 320/90 мс по Device.dwTimeContinual
        const bool xHeld = m_modalStack.IsButtonHeldInTopModal(gpmodal::ModalButton::X);
        const uint32_t xFires = m_backspaceRepeatTimer.Update(xHeld, gpfocus::NavDir::Left, nowContinualMs);
        for (uint32_t i = 0; i < xFires; ++i)
        {
            PressBackspace();
        }

        // 7. A — нажать выбранную клавишу / шаблон / кнопку «Готово»
        if (m_modalStack.IsButtonPressedInTopModal(gpmodal::ModalButton::A))
        {
            PressFocusedCell(rawModalButtonsMask);
            if (!m_isOpen)
                return;
        }

        // 8. Навигация крестовиной / левым стиком с автоповтором 320/90 мс по Device.dwTimeContinual
        bool dirActive = hasStickDir;
        gpfocus::NavDir effectiveDir = stickDir;
        if (!dirActive)
        {
            if (m_modalStack.IsButtonHeldInTopModal(gpmodal::ModalButton::Up))
            {
                dirActive = true;
                effectiveDir = gpfocus::NavDir::Up;
            }
            else if (m_modalStack.IsButtonHeldInTopModal(gpmodal::ModalButton::Down))
            {
                dirActive = true;
                effectiveDir = gpfocus::NavDir::Down;
            }
            else if (m_modalStack.IsButtonHeldInTopModal(gpmodal::ModalButton::Left))
            {
                dirActive = true;
                effectiveDir = gpfocus::NavDir::Left;
            }
            else if (m_modalStack.IsButtonHeldInTopModal(gpmodal::ModalButton::Right))
            {
                dirActive = true;
                effectiveDir = gpfocus::NavDir::Right;
            }
        }

        const uint32_t navSteps = m_navRepeatTimer.Update(dirActive, effectiveDir, nowContinualMs);
        for (uint32_t s = 0; s < navSteps; ++s)
        {
            MoveFocus(effectiveDir);
        }
    }

    bool OnScreenKeyboardController::FindCharOnGrid(OskLanguage lang,
                                                    bool upperCase,
                                                    uint8_t targetCp1251,
                                                    int32_t& outRow,
                                                    int32_t& outCol) const noexcept
    {
        for (int32_t r = kDigitRow; r <= kLetterRowBottom; ++r)
        {
            for (int32_t c = 0; c < kKeyCols; ++c)
            {
                if (LookupKeyCharCp1251(lang, upperCase, r, c) == targetCp1251)
                {
                    outRow = r;
                    outCol = c;
                    return true;
                }
            }
        }
        return false;
    }
} // namespace gposk

#ifndef GPOSK_STANDALONE_TEST
extern ENGINE_API int ps_gp_telemetry;

namespace
{
    bool IsLiveOskDialogHolder(CDialogHolder* holder)
    {
        if (!holder)
            return false;
        if (MainMenu() && static_cast<CDialogHolder*>(MainMenu()) == holder)
            return true;
        return (g_pGameLevel && CurrentGameUI() && static_cast<CDialogHolder*>(CurrentGameUI()) == holder);
    }

    CDialogHolder* GetActiveDialogHolderForOsk()
    {
        if (MainMenu() && MainMenu()->IsActive())
            return MainMenu();
        if (g_pGameLevel && CurrentGameUI())
            return CurrentGameUI();
        return nullptr;
    }

    std::string GetDynamicLevelNameCp1251()
    {
        if (g_pGameLevel && Level().name().size() > 0)
        {
            shared_str translated = CStringTable().translate(Level().name());
            if (translated.size() > 0)
                return std::string(translated.c_str());
        }
        return gposk::TrOsk("ui_gp_eng_osk_default_zone", "\xC7\xEE\xED\xE0");
    }

    std::string GetDynamicDateTimeCp1251()
    {
        if (g_pGameLevel && ai().get_alife())
        {
            u32 years = 0, months = 0, days = 0, hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
            split_time(Level().GetGameTime(), years, months, days, hours, minutes, seconds, milliseconds);
            char buf[64];
            xr_sprintf(buf, sizeof(buf), "%02u-%02u %02u-%02u", days, months, hours, minutes);
            return std::string(buf);
        }
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[64];
        xr_sprintf(buf, sizeof(buf), "%02u-%02u %02u-%02u", st.wDay, st.wMonth, st.wHour, st.wMinute);
        return std::string(buf);
    }

    const char* GetTargetTitleCp1251(gposk::OskTargetKind kind) noexcept
    {
        switch (kind)
        {
        case gposk::OskTargetKind::SaveDialog:
            return gposk::TrOsk("ui_gp_eng_osk_target_save", "\xD1\xEE\xF5\xF0\xE0\xED\xE5\xED\xE8\xE5\x20\xE8\xE3\xF0\xFB\x20\x28\x55\x49\x53\x61\x76\x65\x44\x69\x61\x6C\x6F\x67\x29");
        case gposk::OskTargetKind::StashDialog:
            return gposk::TrOsk("ui_gp_eng_osk_target_stash", "\xD1\xEE\xE7\xE4\xE0\xED\xE8\xE5\x20\xF2\xE0\xE9\xED\xE8\xEA\xE0\x20\x28\x55\x49\x43\x72\x65\x61\x74\x65\x53\x74\x61\x73\x68\x29");
        case gposk::OskTargetKind::WaypointPin:
            return gposk::TrOsk("ui_gp_eng_osk_target_waypoint", "\xCC\xE5\xF2\xEA\xE0\x20\xCA\xCF\xCA\x20\x2F\x20\xCF\xF3\xF2\xE5\xE2\xE0\xFF\x20\xF2\xEE\xF7\xEA\xE0\x20\x28\x50\x41\x57\x29");
        case gposk::OskTargetKind::NewGameName:
            return gposk::TrOsk("ui_gp_eng_osk_target_newgame", "\xC8\xEC\xFF\x20\xEF\xE5\xF0\xF1\xEE\xED\xE0\xE6\xE0\x20\x28\xCD\xEE\xE2\xE0\xFF\x20\xE8\xE3\xF0\xE0\x29");
        case gposk::OskTargetKind::NumericArgb:
            return gposk::TrOsk("ui_gp_eng_osk_target_numeric", "\xD7\xE8\xF1\xEB\xEE\xE2\xEE\xE5\x20\xE7\xED\xE0\xF7\xE5\xED\xE8\xE5\x20\x28\x30\x2E\x2E\x32\x35\x35\x29");
        case gposk::OskTargetKind::GenericEdit:
        default:
            return gposk::TrOsk("ui_gp_eng_osk_target_generic", "\xCF\xEE\xEB\xE5\x20\xE2\xE2\xEE\xE4\xE0\x20\xF2\xE5\xEA\xF1\xF2\xE0");
        }
    }

    class CUIGamepadOnScreenKeyboardWnd : public CUIDialogWnd
    {
        typedef CUIDialogWnd inherited;

    public:
        static constexpr float PANEL_X = 92.0f;
        static constexpr float PANEL_Y = 82.0f;
        static constexpr float PANEL_W = 840.0f;
        static constexpr float PANEL_H = 560.0f;

        static constexpr float GRID_X  = 118.0f;
        static constexpr float GRID_W  = 788.0f;

        static constexpr float TMPL_Y  = 212.0f;
        static constexpr float TMPL_H  = 34.0f;

        static constexpr float KEYS_Y  = 260.0f;
        static constexpr float KEY_H   = 44.0f;
        static constexpr float KEY_GAP = 4.0f;

        static constexpr float ACT_Y   = 460.0f;
        static constexpr float ACT_H   = 42.0f;

        CUIGamepadOnScreenKeyboardWnd()
        {
            SetWindowName("CUIGamepadOnScreenKeyboardWnd");
            AllowWorkInPause(true);
            AllowMovement(false);
            AllowCursor(false);
            AllowCenterCursor(false);

            SetWndRect(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
            m_shaderWire->create("hud\\crosshair");

            m_pTitle = CreateLabel(PANEL_X, PANEL_Y + 10.0f, PANEL_W, 26.0f,
                                   UI().Font().pFontLetterica18Russian,
                                   color_argb(255, 240, 240, 240), CGameFont::alCenter);
            m_pSubtitle = CreateLabel(PANEL_X, PANEL_Y + 34.0f, PANEL_W, 22.0f,
                                      UI().Font().pFontLetterica16Russian,
                                      color_argb(255, 172, 176, 182), CGameFont::alCenter);

            m_pPreviewText = CreateLabel(GRID_X + 12.0f, PANEL_Y + 68.0f, GRID_W - 24.0f, 34.0f,
                                         UI().Font().pFontLetterica18Russian,
                                         color_argb(255, 240, 240, 240), CGameFont::alLeft);

            m_pTemplateCaption = CreateLabel(GRID_X, TMPL_Y - 20.0f, GRID_W, 18.0f,
                                             UI().Font().pFontLetterica16Russian,
                                             color_argb(255, 172, 176, 182), CGameFont::alLeft);

            for (int c = 0; c < gposk::OnScreenKeyboardController::kTemplateCols; ++c)
            {
                const Frect r = GetCellRect(gposk::OnScreenKeyboardController::kTemplateRow, c);
                m_pTemplateLabels[c] = CreateLabel(r.left + 3.0f, r.top + 8.0f, r.width() - 6.0f, r.height() - 8.0f,
                                                   UI().Font().pFontLetterica16Russian,
                                                   color_argb(255, 225, 225, 225), CGameFont::alCenter);
            }

            for (int row = 0; row < 4; ++row)
            {
                const int gridRow = gposk::OnScreenKeyboardController::kDigitRow + row;
                for (int col = 0; col < gposk::OnScreenKeyboardController::kKeyCols; ++col)
                {
                    const Frect r = GetCellRect(gridRow, col);
                    m_pKeyLabels[row][col] = CreateLabel(r.left, r.top + 12.0f, r.width(), r.height() - 12.0f,
                                                         UI().Font().pFontLetterica18Russian,
                                                         color_argb(255, 228, 230, 234), CGameFont::alCenter);
                }
            }

            for (int c = 0; c < gposk::OnScreenKeyboardController::kActionCols; ++c)
            {
                const Frect r = GetCellRect(gposk::OnScreenKeyboardController::kActionRow, c);
                m_pActionLabels[c] = CreateLabel(r.left + 2.0f, r.top + 12.0f, r.width() - 4.0f, r.height() - 12.0f,
                                                 UI().Font().pFontLetterica16Russian,
                                                 color_argb(255, 225, 225, 225), CGameFont::alCenter);
            }

            m_pFooter = CreateLabel(PANEL_X + 12.0f, PANEL_Y + PANEL_H - 42.0f, PANEL_W - 24.0f, 32.0f,
                                    UI().Font().pFontLetterica16Russian,
                                    color_argb(255, 225, 225, 225), CGameFont::alCenter);
        }

        virtual ~CUIGamepadOnScreenKeyboardWnd()
        {
            ReleasePauseTokenIfHeld();
        }

        virtual bool WorkInPause() const override { return true; }
        virtual bool NeedCursor() const override { return false; }

        bool StartForEditBox(CUICustomEdit* pEdit, CUIDialogWnd* pParentDialog, const char* sourceTag)
        {
            if (!pEdit || pEdit->IsReadOnly())
                return false;

            CDialogHolder* holder = GetActiveDialogHolderForOsk();
            if (!holder)
                return false;

            m_pTargetEdit   = pEdit;
            m_pParentDialog = pParentDialog;

            const char* editName   = (pEdit->WindowName().size() > 0) ? pEdit->WindowName().c_str() : "";
            const char* parentName = (pEdit->GetParent() && pEdit->GetParent()->WindowName().size() > 0)
                                         ? pEdit->GetParent()->WindowName().c_str()
                                         : "";
            const char* dlgName    = (pParentDialog && pParentDialog->WindowName().size() > 0)
                                         ? pParentDialog->WindowName().c_str()
                                         : "";
            const bool inMainMenu  = (MainMenu() != nullptr && MainMenu()->IsActive());

            gposk::OskTargetKind kind = gposk::ClassifyEditTarget(editName, parentName, dlgName, inMainMenu, pEdit->IsNumbersOnly());
            if (kind == gposk::OskTargetKind::GenericEdit && pEdit->IsFileNameMode())
                kind = gposk::OskTargetKind::SaveDialog;
            const char* currentText = pEdit->GetText() ? pEdit->GetText() : "";

            bool startDown = false;
            bool l3Down = false;
            bool r3Down = false;
            uint32_t heldModalMask = BuildRawModalButtonsMask(&startDown, &l3Down, &r3Down);
            m_controller.OpenSession(
                kind,
                currentText,
                heldModalMask,
                Device.dwTimeContinual,
                GetDynamicLevelNameCp1251(),
                GetDynamicDateTimeCp1251());
            m_controller.SeedHeldExtraButtons(startDown, l3Down, r3Down);

            // Взводим барьер диспетчера для всех зажатых кнопок при входе в модальную клавиатуру (F1.2)
            CGamepadContextDispatcher::Instance().ConsumeAllCurrentlyHeldButtons();
            CGamepadContextDispatcher::Instance().NotifyModalLayerTransition("osk_modal", sourceTag ? sourceTag : "osk_open");

            Enable(true);
            if (IsShown() && GetHolder() != holder)
            {
                if (IsLiveOskDialogHolder(GetHolder()))
                {
                    HideDialog();
                }
                else
                {
                    SetHolder(nullptr);
                    Show(false);
                }
            }
            if (!IsShown())
            {
                holder->StartDialog(this, false);
            }
            else if (holder->TopInputReceiver() != this)
            {
                if (IsLiveOskDialogHolder(GetHolder()))
                    HideDialog();
                if (IsShown())
                {
                    SetHolder(nullptr);
                    Show(false);
                }
                holder->StartDialog(this, false);
            }
            RefreshLabels();

            Msg("* [Gamepad] gp_osk open (%s): target=%s edit=\"%s\" parent=\"%s\" initial_len=%u",
                sourceTag ? sourceTag : "focus",
                gposk::GetTargetKindName(kind),
                editName,
                parentName,
                static_cast<u32>(m_controller.GetOriginalTextCp1251().size()));

            if (ps_gp_telemetry && pInput)
            {
                string256 line;
                xr_sprintf(line, sizeof(line), "gp_osk,%u,open,kind=%s,edit=%s,len=%u",
                           Device.dwTimeContinual,
                           gposk::GetTargetKindName(kind),
                           editName[0] ? editName : "unnamed",
                           static_cast<u32>(m_controller.GetOriginalTextCp1251().size()));
                pInput->WriteGamepadTelemetryLine(line);
            }
            return true;
        }

        virtual void Show(bool status) override
        {
            const bool wasShown = IsShown();
            if (status)
                Enable(true);
            inherited::Show(status);

            if (status && !wasShown)
            {
                if (!MainMenu() || !MainMenu()->IsActive())
                {
                    if (m_pauseTokenId == 0)
                    {
                        m_pauseTokenId = CPauseTokenManager::Instance().Acquire("gp_osk");
                    }
                }
                m_lastUpdateFrame = Device.dwFrame;
                RefreshLabels();
            }
            else if (!status && wasShown)
            {
                ReleasePauseTokenIfHeld();
                CGamepadContextDispatcher::Instance().ConsumeAllCurrentlyHeldButtons();
                CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_EAST);
                CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_SOUTH);
                CGamepadContextDispatcher::Instance().ConsumeButtonUntilRelease(CInput::GP_BTN_START);
                CGamepadContextDispatcher::Instance().NotifyModalLayerTransition("parent_dialog", "osk_close");

                // Если родительский диалог использует курсор (UISaveDialog, UICreateStash, UIWPSettings, UINewGame),
                // гарантируем, что курсор остаётся видимым после закрытия экранной клавиатуры
                if (CDialogHolder* holder = GetActiveDialogHolderForOsk())
                {
                    if (CUIDialogWnd* top = holder->TopInputReceiver())
                    {
                        if (top != this && top->NeedCursor())
                            GetUICursor().Show();
                    }
                }
            }
        }

        virtual void Update() override
        {
            inherited::Update();
            if (!IsShown())
                return;

            // Защита от двойного вызова Update() в одном кадре из CDialogHolder::OnFrame (UIDialogHolder.cpp:268,275)
            if (m_lastUpdateFrame == Device.dwFrame)
                return;
            m_lastUpdateFrame = Device.dwFrame;

            if (!m_controller.IsOpen())
            {
                FinalizeAndCloseDialog();
                return;
            }

            bool pressStart   = false;
            bool pressL3      = false;
            bool pressR3      = false;
            bool hasStickDir  = false;
            gpfocus::NavDir stickDir = gpfocus::NavDir::Up;
            const uint32_t rawMask = BuildRawModalButtonsMask(&pressStart, &pressL3, &pressR3, &hasStickDir, &stickDir);

            m_controller.StepController(rawMask, pressStart, pressL3, pressR3, hasStickDir, stickDir, Device.dwTimeContinual);
            RefreshLabels();
            RefreshGlyphCache();

            if (!m_controller.IsOpen())
            {
                FinalizeAndCloseDialog();
            }
        }

        virtual void Draw() override
        {
            if (!IsShown())
                return;

            SyncWhiteFocusCellLabelColors();
            DrawKeyboardBackdropAndCells();
            const bool hideTextFooter = ShouldRenderFooterGlyphStrip();
            if (m_pFooter)
                m_pFooter->Show(!hideTextFooter);
            inherited::Draw();
            if (hideTextFooter)
                DrawFooterGlyphStrip();
        }

        virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override
        {
            if (!IR_process())
                return false;

            if (keyboard_action == WINDOW_KEY_PRESSED)
            {
                if (dik == DIK_ESCAPE)
                {
                    m_controller.CancelSession(0);
                    FinalizeAndCloseDialog();
                    return true;
                }
                if (dik == DIK_RETURN || dik == DIK_NUMPADENTER)
                {
                    m_controller.CommitSession(0);
                    FinalizeAndCloseDialog();
                    return true;
                }
                if (dik == DIK_BACK)
                {
                    m_controller.PressBackspace();
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_SPACE)
                {
                    m_controller.PressSpace();
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_UP)
                {
                    m_controller.MoveFocus(gpfocus::NavDir::Up);
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_DOWN)
                {
                    m_controller.MoveFocus(gpfocus::NavDir::Down);
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_LEFT)
                {
                    m_controller.MoveFocus(gpfocus::NavDir::Left);
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_RIGHT)
                {
                    m_controller.MoveFocus(gpfocus::NavDir::Right);
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_TAB)
                {
                    m_controller.ToggleLanguage();
                    RefreshLabels();
                    return true;
                }
                if (dik == DIK_CAPITAL || dik == DIK_LSHIFT || dik == DIK_RSHIFT)
                {
                    m_controller.ToggleCase();
                    RefreshLabels();
                    return true;
                }
            }
            return true;
        }

        virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action) override
        {
            if (!IR_process())
                return false;

            int32_t hitRow = -1, hitCol = -1;
            if (HitTestCell(x, y, hitRow, hitCol))
            {
                m_controller.SetFocusCell(hitRow, hitCol);
                if (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK)
                {
                    m_controller.PressFocusedCell(0);
                    RefreshLabels();
                    if (!m_controller.IsOpen())
                        FinalizeAndCloseDialog();
                }
                return true;
            }
            return inherited::OnMouseAction(x, y, mouse_action);
        }

        void ForceCommitAndClose(const char* reason)
        {
            (void)reason;
            if (m_controller.IsOpen())
                m_controller.CommitSession(BuildRawModalButtonsMask());
            FinalizeAndCloseDialog();
        }

        void ForceCancelAndClose(const char* reason)
        {
            (void)reason;
            if (m_controller.IsOpen())
                m_controller.CancelSession(BuildRawModalButtonsMask());
            FinalizeAndCloseDialog();
        }

        CUICustomEdit* GetTargetEdit() const noexcept { return m_pTargetEdit; }
        void ClearTargetEdit() noexcept { m_pTargetEdit = nullptr; }
        const gposk::OnScreenKeyboardController& GetController() const noexcept { return m_controller; }
        gposk::OnScreenKeyboardController& GetControllerMutable() noexcept { return m_controller; }

    private:
        CUITextWnd* CreateLabel(float x, float y, float w, float h, CGameFont* font, u32 color, CGameFont::EAligment align)
        {
            CUITextWnd* lbl = xr_new<CUITextWnd>();
            lbl->SetAutoDelete(true);
            lbl->SetWndPos(Fvector2().set(x, y));
            lbl->SetWndSize(Fvector2().set(w, h));
            lbl->SetFont(font);
            lbl->SetTextColor(color);
            lbl->SetTextAlignment(align);
            AttachChild(lbl);
            return lbl;
        }

        void ReleasePauseTokenIfHeld()
        {
            if (m_pauseTokenId != 0)
            {
                const u32 tok = m_pauseTokenId;
                m_pauseTokenId = 0;
                CPauseTokenManager::Instance().Release(tok);
            }
        }

        static Frect GetCellRect(int32_t row, int32_t col) noexcept
        {
            if (row == gposk::OnScreenKeyboardController::kTemplateRow)
            {
                const float chipGap = 6.0f;
                const float chipW = (GRID_W - chipGap * 5.0f) / 6.0f;
                const float x = GRID_X + col * (chipW + chipGap);
                return Frect().set(x, TMPL_Y, x + chipW, TMPL_Y + TMPL_H);
            }
            if (row >= gposk::OnScreenKeyboardController::kDigitRow &&
                row <= gposk::OnScreenKeyboardController::kLetterRowBottom)
            {
                const int32_t keyRowIdx = row - gposk::OnScreenKeyboardController::kDigitRow;
                const float keyW = (GRID_W - KEY_GAP * 11.0f) / 12.0f;
                const float x = GRID_X + col * (keyW + KEY_GAP);
                const float y = KEYS_Y + keyRowIdx * (KEY_H + KEY_GAP);
                return Frect().set(x, y, x + keyW, y + KEY_H);
            }
            if (row == gposk::OnScreenKeyboardController::kActionRow)
            {
                const float actGap = 6.0f;
                const float actW = (GRID_W - actGap * 5.0f) / 6.0f;
                const float x = GRID_X + col * (actW + actGap);
                return Frect().set(x, ACT_Y, x + actW, ACT_Y + ACT_H);
            }
            return Frect().set(0.0f, 0.0f, 0.0f, 0.0f);
        }

        bool HitTestCell(float x, float y, int32_t& outRow, int32_t& outCol) const noexcept
        {
            for (int32_t r = 0; r < gposk::OnScreenKeyboardController::kTotalRows; ++r)
            {
                const int32_t cols = m_controller.GetColsForRow(r);
                for (int32_t c = 0; c < cols; ++c)
                {
                    if (GetCellRect(r, c).in(x, y))
                    {
                        outRow = r;
                        outCol = c;
                        return true;
                    }
                }
            }
            return false;
        }

        uint32_t BuildRawModalButtonsMask(bool* outStart = nullptr,
                                          bool* outL3 = nullptr,
                                          bool* outR3 = nullptr,
                                          bool* outHasStickDir = nullptr,
                                          gpfocus::NavDir* outStickDir = nullptr) const
        {
            if (outStart)       *outStart       = false;
            if (outL3)          *outL3          = false;
            if (outR3)          *outR3          = false;
            if (outHasStickDir) *outHasStickDir = false;
            if (outStickDir)    *outStickDir    = gpfocus::NavDir::Up;

            if (!pInput)
                return 0;

            const auto& gp = pInput->GetGamepadState();
            if (!gp.connected || !gp.has_focus || gp.need_release_all)
                return 0;

            const u32 b = gp.buttons_mask;
            uint32_t mask = 0;

            if (b & (1u << CInput::GP_BTN_SOUTH))          mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::A);
            if (b & (1u << CInput::GP_BTN_EAST))           mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::B);
            if (b & (1u << CInput::GP_BTN_WEST))           mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::X);
            if (b & (1u << CInput::GP_BTN_NORTH))          mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::Y);
            if (b & (1u << CInput::GP_BTN_BACK))           mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::View);
            if (b & (1u << CInput::GP_BTN_LEFT_SHOULDER))  mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::LB);
            if (b & (1u << CInput::GP_BTN_RIGHT_SHOULDER)) mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::RB);
            if (b & (1u << CInput::GP_BTN_DPAD_UP))        mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::Up);
            if (b & (1u << CInput::GP_BTN_DPAD_DOWN))      mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::Down);
            if (b & (1u << CInput::GP_BTN_DPAD_LEFT))      mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::Left);
            if (b & (1u << CInput::GP_BTN_DPAD_RIGHT))     mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::Right);

            const gpmath::CalibrationProfile& prof = gpmath::GetCalibrationProfile();
            if (gpmath::NormalizeTriggerAxis(gp.axes[4]) >= prof.left_trigger.press)
                mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::LT);
            if (gpmath::NormalizeTriggerAxis(gp.axes[5]) >= prof.right_trigger.press)
                mask |= gpmodal::ModalButtonMask(gpmodal::ModalButton::RT);

            if (outStart) *outStart = (b & (1u << CInput::GP_BTN_START)) != 0;
            if (outL3)    *outL3    = (b & (1u << CInput::GP_BTN_LEFT_STICK)) != 0;
            if (outR3)    *outR3    = (b & (1u << CInput::GP_BTN_RIGHT_STICK)) != 0;

            if (outStickDir && outHasStickDir)
            {
                const float lx = static_cast<float>(gp.axes[0]) / 32767.0f;
                const float ly = static_cast<float>(gp.axes[1]) / 32767.0f;
                constexpr float kStickDead = 0.55f;
                if (_abs(lx) >= _abs(ly))
                {
                    if (lx <= -kStickDead)      { *outHasStickDir = true; *outStickDir = gpfocus::NavDir::Left; }
                    else if (lx >= kStickDead)  { *outHasStickDir = true; *outStickDir = gpfocus::NavDir::Right; }
                }
                else
                {
                    if (ly <= -kStickDead)      { *outHasStickDir = true; *outStickDir = gpfocus::NavDir::Up; }
                    else if (ly >= kStickDead)  { *outHasStickDir = true; *outStickDir = gpfocus::NavDir::Down; }
                }
            }

            return mask;
        }

        void FinalizeAndCloseDialog()
        {
            CUICustomEdit* edit = m_pTargetEdit;
            CUIDialogWnd* savedParent = m_pParentDialog;
            m_pTargetEdit = nullptr;
            m_pParentDialog = nullptr;

            // CR1-09: close OSK first if we are genuinely on top of a live holder stack; never dereference savedParent
            // directly because CDialogHolder::CleanInternals + ReloadUI may have freed it without calling Show(false).
            CDialogHolder* holder = GetActiveDialogHolderForOsk();
            const bool wasActiveTop = IsShown() && holder != nullptr &&
                                      GetHolder() == holder && holder->TopInputReceiver() == this;
            if (IsShown())
            {
                if (IsLiveOskDialogHolder(GetHolder()))
                {
                    HideDialog();
                }
                if (IsShown())
                {
                    SetHolder(nullptr);
                    Show(false);
                }
            }

            // Проверяем, что родительский диалог всё ещё открыт и активен, прежде чем обращаться к edit
            bool parentStillValid = false;
            if (edit && wasActiveTop && holder)
            {
                if (CUIDialogWnd* liveTop = holder->TopInputReceiver())
                {
                    if (!savedParent || liveTop == savedParent)
                    {
                        std::vector<CUICustomEdit*> liveEdits;
                        CGamepadOnScreenKeyboardUI::CollectVisibleEditBoxes(liveTop, liveEdits);
                        parentStillValid = (std::find(liveEdits.begin(), liveEdits.end(), edit) != liveEdits.end());
                    }
                }
            }

            if (edit && parentStillValid)
            {
                if (m_controller.WasCommitted())
                {
                    const std::string& committedCp1251 = m_controller.GetCommittedTextCp1251();
                    edit->SetText(committedCp1251.c_str());
                    edit->CaptureFocus(false);
                    if (CUIWindow* msgTarget = edit->GetMessageTarget())
                    {
                        msgTarget->SendMessage(edit, EDIT_TEXT_COMMIT, nullptr);
                    }
                    Msg("* [Gamepad] gp_osk commit: target=%s len=%u text_cp1251=\"%s\"",
                        gposk::GetTargetKindName(m_controller.GetTargetKind()),
                        static_cast<u32>(committedCp1251.size()),
                        committedCp1251.c_str());

                    if (ps_gp_telemetry && pInput)
                    {
                        string256 line;
                        xr_sprintf(line, sizeof(line), "gp_osk,%u,commit,kind=%s,len=%u",
                                   Device.dwTimeContinual,
                                   gposk::GetTargetKindName(m_controller.GetTargetKind()),
                                   static_cast<u32>(committedCp1251.size()));
                        pInput->WriteGamepadTelemetryLine(line);
                    }
                }
                else
                {
                    // Отмена по B: текст целевого поля НЕ меняется, снимаем только захват клавиатуры
                    edit->CaptureFocus(false);
                    Msg("* [Gamepad] gp_osk cancel (B): preserved original text (len=%u)",
                        static_cast<u32>(m_controller.GetOriginalTextCp1251().size()));

                    if (ps_gp_telemetry && pInput)
                    {
                        string256 line;
                        xr_sprintf(line, sizeof(line), "gp_osk,%u,cancel_B,kind=%s,preserved_len=%u",
                                   Device.dwTimeContinual,
                                   gposk::GetTargetKindName(m_controller.GetTargetKind()),
                                   static_cast<u32>(m_controller.GetOriginalTextCp1251().size()));
                        pInput->WriteGamepadTelemetryLine(line);
                    }
                }
            }

        }

        void RefreshLabels()
        {
            if (m_pTitle)
            {
                m_pTitle->SetText(gposk::TrOsk("ui_gp_eng_osk_title", "\xDD\xCA\xD0\xC0\xCD\xCD\xC0\xDF\x20\xCA\xCB\xC0\xC2\xC8\xC0\xD2\xD3\xD0\xC0\x20\xC3\xC5\xC9\xCC\xCF\xC0\xC4\xC0"));
            }
            if (m_pSubtitle)
            {
                string512 sub;
                xr_sprintf(sub, sizeof(sub), "%s  |  %s  |  %s",
                           GetTargetTitleCp1251(m_controller.GetTargetKind()),
                           (m_controller.GetLanguage() == gposk::OskLanguage::RU)
                               ? gposk::TrOsk("ui_gp_eng_osk_layout_ru", "RU (\xC9\xD6\xD3\xCA\xC5\xCD)")
                               : gposk::TrOsk("ui_gp_eng_osk_layout_en", "EN (QWERTY)"),
                           m_controller.IsUpperCase()
                               ? gposk::TrOsk("ui_gp_eng_osk_case_hdr_upper", "\xC0\xC1\xC2 / ABC")
                               : gposk::TrOsk("ui_gp_eng_osk_case_hdr_lower", "\xE0\xE1\xE2 / abc"));
                m_pSubtitle->SetText(sub);
            }
            if (m_pPreviewText)
            {
                std::string preview = m_controller.GetBufferCp1251();
                if (((Device.dwTimeContinual / 450) % 2) == 0)
                    preview.push_back('|');
                m_pPreviewText->SetText(preview.c_str());
            }
            if (m_pTemplateCaption)
            {
                m_pTemplateCaption->SetText(gposk::TrOsk("ui_gp_eng_osk_tmpl_caption", "\xD8\xE0\xE1\xEB\xEE\xED\xFB\x20\xE2\x20\x31\x20\xEA\xEB\xE8\xEA\x20\x28\xC2\xE2\xE5\xF0\xF5\x2F\xC2\xED\xE8\xE7\x20\x97\x20\xEF\xE5\xF0\xE5\xF5\xEE\xE4\x20\xEA\x20\xF1\xE5\xF2\xEA\xE5\x2C\x20\x41\x20\x97\x20\xE2\xF1\xF2\xE0\xE2\xE8\xF2\xFC\x29\x3A"));
            }

            const int32_t focusRow = m_controller.GetFocusRow();
            const int32_t focusCol = m_controller.GetFocusCol();

            for (int c = 0; c < gposk::OnScreenKeyboardController::kTemplateCols; ++c)
            {
                if (!m_pTemplateLabels[c])
                    continue;
                const auto info = m_controller.GetCellInfo(gposk::OnScreenKeyboardController::kTemplateRow, c);
                m_pTemplateLabels[c]->SetText(info.label_cp1251.c_str());
                const bool focused = (focusRow == gposk::OnScreenKeyboardController::kTemplateRow && focusCol == c);
                m_pTemplateLabels[c]->SetTextColor(focused ? color_argb(255, 255, 240, 150) : color_argb(255, 215, 225, 210));
            }

            for (int r = 0; r < 4; ++r)
            {
                const int32_t gridRow = gposk::OnScreenKeyboardController::kDigitRow + r;
                for (int c = 0; c < gposk::OnScreenKeyboardController::kKeyCols; ++c)
                {
                    if (!m_pKeyLabels[r][c])
                        continue;
                    const auto info = m_controller.GetCellInfo(gridRow, c);
                    m_pKeyLabels[r][c]->SetText(info.label_cp1251.c_str());
                    const bool focused = (focusRow == gridRow && focusCol == c);
                    m_pKeyLabels[r][c]->SetTextColor(focused ? color_argb(255, 255, 235, 120) : color_argb(255, 230, 235, 240));
                }
            }

            for (int c = 0; c < gposk::OnScreenKeyboardController::kActionCols; ++c)
            {
                if (!m_pActionLabels[c])
                    continue;
                const auto info = m_controller.GetCellInfo(gposk::OnScreenKeyboardController::kActionRow, c);
                m_pActionLabels[c]->SetText(info.label_cp1251.c_str());
                const bool focused = (focusRow == gposk::OnScreenKeyboardController::kActionRow && focusCol == c);
                m_pActionLabels[c]->SetTextColor(focused ? color_argb(255, 255, 240, 130) : color_argb(255, 225, 225, 205));
            }

            if (m_pFooter)
            {
                m_pFooter->SetText(gposk::TrOsk("ui_gp_eng_osk_footer", "\x5B\x41\x5D\x20\xC2\xE2\xEE\xE4\x20\xEA\xEB\xFE\xF7\xE0\x2F\xF8\xE0\xE1\xEB\xEE\xED\xE0\x20\x20\x5B\x58\x5D\x20\xC7\xE0\xE1\xEE\xE9\x20\x20\x5B\x59\x5D\x20\xCF\xF0\xEE\xE1\xE5\xEB\x20\x20\x5B\x4C\x33\x2F\x4C\x54\x5D\x20\xD0\xE5\xE3\xE8\xF1\xF2\xF0\x20\x20\x5B\x52\x33\x2F\x4C\x42\x2F\x52\x42\x5D\x20\x52\x55\x2F\x45\x4E\x20\x20\x5B\x53\x74\x61\x72\x74\x5D\x20\xCF\xF0\xE8\xEC\xE5\xED\xE8\xF2\xFC\x20\x20\x5B\x42\x5D\x20\xCE\xF2\xEC\xE5\xED\xE0"));
            }
        }

        void DrawFilledRect(float x, float y, float w, float h, u32 color)
        {
            UIRender->SetShader(*m_shaderWire);
            UIRender->StartPrimitive(4, IUIRender::ptTriStrip, UI().m_currentPointType);

            Fvector2 p0, p1;
            p0.set(x, y);
            p1.set(x + w, y + h);
            UI().ClientToScreenScaled(p0, p0.x, p0.y);
            UI().ClientToScreenScaled(p1, p1.x, p1.y);

            UIRender->PushPoint(p0.x, p0.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p1.x, p0.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p0.x, p1.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->FlushPrimitive();
        }

        void DrawBorderRect(float x, float y, float w, float h, u32 color)
        {
            UIRender->SetShader(*m_shaderWire);
            UIRender->StartPrimitive(8, IUIRender::ptLineList, UI().m_currentPointType);

            Fvector2 p0, p1;
            p0.set(x, y);
            p1.set(x + w, y + h);
            UI().ClientToScreenScaled(p0, p0.x, p0.y);
            UI().ClientToScreenScaled(p1, p1.x, p1.y);

            UIRender->PushPoint(p0.x, p0.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p1.x, p0.y, 0.0f, color, 0.0f, 0.0f);

            UIRender->PushPoint(p1.x, p0.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);

            UIRender->PushPoint(p1.x, p1.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p0.x, p1.y, 0.0f, color, 0.0f, 0.0f);

            UIRender->PushPoint(p0.x, p1.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->PushPoint(p0.x, p0.y, 0.0f, color, 0.0f, 0.0f);
            UIRender->FlushPrimitive();
        }

        LPCSTR ResolveOskFooterGlyph(LPCSTR slotName, LPCSTR fallbackXbox) const
        {
            if (g_ai_space)
            {
                try
                {
                    ::luabind::functor<LPCSTR> texFn;
                    if (ai().script_engine().functor("gp_icons.tex", texFn))
                    {
                        LPCSTR resolved = texFn(slotName, "round");
                        if (resolved && resolved[0])
                            return resolved;
                    }
                }
                catch (...)
                {
                }
            }
            auto& mgr = CGamepadGlyphManager::Instance();
            mgr.EnsureInitialized();
            switch (mgr.GetEffectiveGlyphSet())
            {
            case EGamepadGlyphSet::PlayStation:
                if (0 == xr_strcmp(slotName, "south")) return "ui_gp_white_ps_south";
                if (0 == xr_strcmp(slotName, "west"))  return "ui_gp_white_ps_west";
                if (0 == xr_strcmp(slotName, "east"))  return "ui_gp_white_ps_east";
                if (0 == xr_strcmp(slotName, "north")) return "ui_gp_white_ps_north";
                if (0 == xr_strcmp(slotName, "start")) return "ui_gp_white_ps_start";
                break;
            case EGamepadGlyphSet::Nintendo:
                if (0 == xr_strcmp(slotName, "south")) return "ui_gp_white_nin_south";
                if (0 == xr_strcmp(slotName, "west"))  return "ui_gp_white_nin_west";
                if (0 == xr_strcmp(slotName, "east"))  return "ui_gp_white_nin_east";
                if (0 == xr_strcmp(slotName, "north")) return "ui_gp_white_nin_north";
                if (0 == xr_strcmp(slotName, "start")) return "ui_gp_white_nin_start";
                break;
            default:
                break;
            }
            return fallbackXbox;
        }

        // FIX-LIVE6 (2026-11-16): Lua опрашивается только в Update (главный поток); Draw (поток рендера)
        // читает кэш — иначе гонка с CLevel::LuaGC (вылет lj_gc.c) и откат на старые значки.
        void RefreshGlyphCache()
        {
            m_cacheHintsVisible = QueryHintsVisibleLua();
            float iconScale = 1.0f;
            int rev = 0;
            if (g_ai_space)
            {
                try
                {
                    ::luabind::functor<float> scaleFn;
                    if (ai().script_engine().functor("gp_icons.scale_k", scaleFn))
                        iconScale = _min(1.25f, _max(0.8f, static_cast<float>(scaleFn())));
                    ::luabind::functor<int> revFn;
                    if (ai().script_engine().functor("gp_icons.revision", revFn))
                        rev = revFn();
                }
                catch (...)
                {
                }
            }
            m_cacheScale = iconScale;
            m_cacheRev = rev;
            static const LPCSTR slots[6][2] = { {"south","ui_gp_xbox_south"}, {"west","ui_gp_xbox_west"}, {"north","ui_gp_xbox_north"},
                {"lshoulder","ui_gp_xbox_lshoulder"}, {"start","ui_gp_xbox_start"}, {"east","ui_gp_xbox_east"} };
            for (int i = 0; i < 6; ++i)
                m_cacheTex[i] = ResolveOskFooterGlyph(slots[i][0], slots[i][1]);
            m_cacheValid = true;
        }

        bool ShouldRenderFooterGlyphStrip() const
        {
            if (!pInput || !pInput->IsGamepadActiveSource())
                return false;
            return m_cacheValid && m_cacheHintsVisible;
        }

        bool QueryHintsVisibleLua() const
        {
            if (g_ai_space)
            {
                try
                {
                    ::luabind::functor<bool> visFn;
                    if (ai().script_engine().functor("gp_icons.hints_visible", visFn))
                        return visFn("menu");
                }
                catch (...)
                {
                }
            }
            return true;
        }

        void SyncWhiteFocusCellLabelColors()
        {
            const int32_t focusRow = m_controller.GetFocusRow();
            const int32_t focusCol = m_controller.GetFocusCol();
            const u32 colFocused = color_argb(255, 255, 255, 255);
            const u32 colNormal  = color_argb(255, 218, 220, 224);

            for (int c = 0; c < gposk::OnScreenKeyboardController::kTemplateCols; ++c)
            {
                if (m_pTemplateLabels[c])
                {
                    const bool foc = (focusRow == gposk::OnScreenKeyboardController::kTemplateRow && focusCol == c);
                    m_pTemplateLabels[c]->SetTextColor(foc ? colFocused : colNormal);
                }
            }
            for (int r = 0; r < 4; ++r)
            {
                const int gridRow = gposk::OnScreenKeyboardController::kDigitRow + r;
                for (int c = 0; c < gposk::OnScreenKeyboardController::kKeyCols; ++c)
                {
                    if (m_pKeyLabels[r][c])
                    {
                        const bool foc = (focusRow == gridRow && focusCol == c);
                        m_pKeyLabels[r][c]->SetTextColor(foc ? colFocused : colNormal);
                    }
                }
            }
            for (int c = 0; c < gposk::OnScreenKeyboardController::kActionCols; ++c)
            {
                if (m_pActionLabels[c])
                {
                    const bool foc = (focusRow == gposk::OnScreenKeyboardController::kActionRow && focusCol == c);
                    m_pActionLabels[c]->SetTextColor(foc ? colFocused : colNormal);
                }
            }
        }

        void DrawFooterGlyphStrip()
        {
            CGameFont* font = UI().Font().pFontLetterica16Russian;
            const float scaleX = UI().ClientToScreenScaledX(1.0f);
            const float scaleY = UI().ClientToScreenScaledY(1.0f);
            if (!font || scaleX <= 0.0f || scaleY <= 0.0f)
                return;

            const float iconScale = m_cacheScale;
            const int rev = m_cacheRev;

            const bool numeric = (m_controller.GetTargetKind() == gposk::OskTargetKind::NumericArgb);
            struct FooterEntry
            {
                LPCSTR slot;
                LPCSTR fallback;
                LPCSTR label;
                bool wide;
            };
            const FooterEntry entries[6] = {
                { "south", "ui_gp_xbox_south", gposk::TrOsk("ui_gp_eng_osk_ftr_type", "\xC2\xE2\xEE\xE4"), false },
                { "west",  "ui_gp_xbox_west",  gposk::TrOsk("ui_gp_eng_osk_ftr_backspace", "\xD3\xE4\xE0\xEB\xE8\xF2\xFC"), false },
                { "north", "ui_gp_xbox_north", gposk::TrOsk("ui_gp_eng_osk_ftr_space", "\xCF\xF0\xEE\xE1\xE5\xEB"), false },
                { "lshoulder", "ui_gp_xbox_lshoulder", gposk::TrOsk("ui_gp_eng_osk_ftr_lang", "\xDF\xE7\xFB\xEA"), false },
                { "start", "ui_gp_xbox_start", gposk::TrOsk("ui_gp_eng_osk_ftr_done", "\xC3\xEE\xF2\xEE\xE2\xEE"), true },
                { "east",  "ui_gp_xbox_east",  gposk::TrOsk("ui_gp_eng_osk_ftr_cancel", "\xCE\xF2\xEC\xE5\xED\xE0"), false },
            };

            const float iconPxH = _max(12.0f, floorf(18.0f * iconScale * scaleY + 0.5f));
            const float iconH = iconPxH / scaleY;
            const float gapUi = 5.0f;
            const float sepUi = 14.0f;
            const float cy = PANEL_Y + PANEL_H - 25.0f;

            float totalW = 0.0f;
            int shownCount = 0;
            for (int i = 0; i < 6; ++i)
            {
                if (numeric && (i == 2 || i == 3))
                    continue;
                const float iconW = (entries[i].wide ? iconPxH * 1.45f : iconPxH) / scaleX;
                const float textW = font->SizeOf_(entries[i].label) / scaleX;
                if (shownCount > 0)
                    totalW += sepUi;
                totalW += iconW + gapUi + textW;
                ++shownCount;
            }

            float x = PANEL_X + (PANEL_W - totalW) * 0.5f;
            const float iconY = floorf((cy - iconH * 0.5f) * scaleY + 0.5f) / scaleY;
            font->SetAligment(CGameFont::alLeft);
            font->SetColor(color_argb(255, 225, 225, 225));
            const float textScreenY = UI().ClientToScreenScaledY(cy - 8.0f);

            int idx = 0;
            for (int i = 0; i < 6; ++i)
            {
                if (numeric && (i == 2 || i == 3))
                    continue;
                if (idx > 0)
                {
                    font->SetColor(color_argb(255, 125, 130, 127));
                    font->Out(UI().ClientToScreenScaledX(x + sepUi * 0.35f), textScreenY, "\xB7");
                    font->SetColor(color_argb(255, 225, 225, 225));
                    x += sepUi;
                }
                LPCSTR tex = m_cacheTex[i].c_str();
                if (!m_footerGlyphInited[i] || m_footerGlyphRev != rev || m_footerGlyphTex[i] != tex)
                {
                    CUITextureMaster::InitTexture(tex, &m_footerGlyphs[i]);
                    m_footerGlyphTex[i] = tex;
                    m_footerGlyphInited[i] = true;
                }
                const float iconW = (entries[i].wide ? iconPxH * 1.45f : iconPxH) / scaleX;
                m_footerGlyphs[i].SetSize(Fvector2().set(iconW, iconH));
                m_footerGlyphs[i].SetPos(x, iconY);
                m_footerGlyphs[i].SetColor(color_argb(255, 235, 235, 235));
                m_footerGlyphs[i].Render();

                x += iconW + gapUi;
                font->Out(UI().ClientToScreenScaledX(x), textScreenY, "%s", entries[i].label);
                x += font->SizeOf_(entries[i].label) / scaleX;
                ++idx;
            }
            m_footerGlyphRev = rev;
            font->OnRender();
        }

        void DrawWhiteFocusFrame2px(float x, float y, float w, float h, u32 color)
        {
            DrawBorderRect(x, y, w, h, color);
            const float dx = 1.0f / _max(0.5f, UI().ClientToScreenScaledX(1.0f));
            const float dy = 1.0f / _max(0.5f, UI().ClientToScreenScaledY(1.0f));
            if (w > dx * 4.0f && h > dy * 4.0f)
                DrawBorderRect(x + dx, y + dy, w - dx * 2.0f, h - dy * 2.0f, color);
        }

        void DrawKeyboardBackdropAndCells()
        {
            // Концепт §4.9, C1–C2: тёмная угольная подложка и спокойные нейтральные контуры без золота/зелени.
            DrawFilledRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, color_argb(238, 18, 20, 22));
            DrawBorderRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, color_argb(220, 82, 86, 92));

            // Поле предпросмотра вводимой строки
            DrawFilledRect(GRID_X, PANEL_Y + 62.0f, GRID_W, 42.0f, color_argb(245, 12, 14, 16));
            DrawBorderRect(GRID_X, PANEL_Y + 62.0f, GRID_W, 42.0f, color_argb(235, 122, 126, 132));

            const int32_t focusRow = m_controller.GetFocusRow();
            const int32_t focusCol = m_controller.GetFocusCol();
            const u32 focusFill   = color_argb(245, 72, 75, 80);   // ~15% белая подсветка внутри ячейки
            const u32 focusBorder = color_argb(255, 240, 240, 240); // единая белая рамка 2px (вместо золота 250,220,105)

            for (int32_t r = 0; r < gposk::OnScreenKeyboardController::kTotalRows; ++r)
            {
                const int32_t cols = m_controller.GetColsForRow(r);
                for (int32_t c = 0; c < cols; ++c)
                {
                    const Frect rect = GetCellRect(r, c);
                    const bool focused = (r == focusRow && c == focusCol);

                    u32 bgCol = color_argb(225, 32, 35, 38);
                    u32 brdCol = color_argb(200, 76, 80, 86);

                    if (r == gposk::OnScreenKeyboardController::kTemplateRow)
                    {
                        bgCol = focused ? focusFill : color_argb(225, 38, 41, 46);
                        brdCol = color_argb(210, 92, 96, 104);
                    }
                    else if (r == gposk::OnScreenKeyboardController::kActionRow)
                    {
                        if (c == 5) // Готово (Start)
                            bgCol = focused ? color_argb(245, 82, 86, 92) : color_argb(235, 48, 52, 58);
                        else
                            bgCol = focused ? focusFill : color_argb(225, 40, 43, 48);
                        brdCol = color_argb(210, 92, 96, 104);
                    }
                    else if (focused)
                    {
                        bgCol = focusFill;
                    }

                    DrawFilledRect(rect.left, rect.top, rect.width(), rect.height(), bgCol);
                    if (focused)
                        DrawWhiteFocusFrame2px(rect.left, rect.top, rect.width(), rect.height(), focusBorder);
                    else
                        DrawBorderRect(rect.left, rect.top, rect.width(), rect.height(), brdCol);
                }
            }
        }

        CUIStaticItem                       m_footerGlyphs[6];
        shared_str                          m_footerGlyphTex[6];
        bool                                m_footerGlyphInited[6] = {};
        int                                 m_footerGlyphRev = -1;

        u32                                 m_pauseTokenId  = 0;
        u32                                 m_lastUpdateFrame = 0;
        bool                                m_cacheValid = false;
        bool                                m_cacheHintsVisible = true;
        float                               m_cacheScale = 1.0f;
        int                                 m_cacheRev = 0;
        shared_str                          m_cacheTex[6];
        CUICustomEdit*                      m_pTargetEdit   = nullptr;
        CUIDialogWnd*                       m_pParentDialog = nullptr;
        gposk::OnScreenKeyboardController   m_controller;
        ui_shader                           m_shaderWire;

        CUITextWnd*                         m_pTitle        = nullptr;
        CUITextWnd*                         m_pSubtitle     = nullptr;
        CUITextWnd*                         m_pPreviewText  = nullptr;
        CUITextWnd*                         m_pTemplateCaption = nullptr;
        CUITextWnd*                         m_pTemplateLabels[gposk::OnScreenKeyboardController::kTemplateCols] = {};
        CUITextWnd*                         m_pKeyLabels[4][gposk::OnScreenKeyboardController::kKeyCols] = {};
        CUITextWnd*                         m_pActionLabels[gposk::OnScreenKeyboardController::kActionCols] = {};
        CUITextWnd*                         m_pFooter       = nullptr;
    };

    CUIGamepadOnScreenKeyboardWnd* g_pGamepadOskWnd = nullptr;

    CUIGamepadOnScreenKeyboardWnd* EnsureOskWindow()
    {
        if (!g_pGamepadOskWnd)
            g_pGamepadOskWnd = xr_new<CUIGamepadOnScreenKeyboardWnd>();
        return g_pGamepadOskWnd;
    }
} // namespace

int ps_gp_osk_enabled = 1; // FIX-LIVE8 I-06: 1 = показывать экранную клавиатуру, 0 = только фокус поля

CGamepadOnScreenKeyboardUI& CGamepadOnScreenKeyboardUI::Instance()
{
    static CGamepadOnScreenKeyboardUI s_instance;
    return s_instance;
}

void CGamepadOnScreenKeyboardUI::CollectVisibleEditBoxes(CUIWindow* root, std::vector<CUICustomEdit*>& outEdits)
{
    if (!root || !root->IsShown())
        return;

    if (root == g_pGamepadOskWnd)
        return;

    if (CUICustomEdit* edit = dynamic_cast<CUICustomEdit*>(root))
    {
        if (!edit->IsReadOnly() && edit->IsEnabled())
        {
            outEdits.push_back(edit);
        }
    }

    auto& children = root->GetChildWndList();
    for (CUIWindow* child : children)
    {
        CollectVisibleEditBoxes(child, outEdits);
    }
}

namespace
{
    struct CUIWindowOskCapturerProbe : public CUIWindow
    {
        static CUIWindow* GetCapturer(CUIWindow* w)
        {
            return w ? static_cast<CUIWindowOskCapturerProbe*>(w)->m_pKeyboardCapturer : nullptr;
        }
    };
}

CUICustomEdit* CGamepadOnScreenKeyboardUI::FindFocusedEditBoxInTree(CUIWindow* root)
{
    if (!root || !root->IsShown() || root == g_pGamepadOskWnd)
        return nullptr;

    if (CUICustomEdit* selfEdit = dynamic_cast<CUICustomEdit*>(root))
    {
        if (selfEdit->HasInputFocus() && !selfEdit->IsReadOnly())
            return selfEdit;
    }

    if (CUIWindow* cap = CUIWindowOskCapturerProbe::GetCapturer(root))
    {
        if (CUICustomEdit* capEdit = dynamic_cast<CUICustomEdit*>(cap))
        {
            if (capEdit->IsShown() && !capEdit->IsReadOnly())
                return capEdit;
        }
    }

    auto& children = root->GetChildWndList();
    for (CUIWindow* child : children)
    {
        if (CUICustomEdit* found = FindFocusedEditBoxInTree(child))
            return found;
    }
    return nullptr;
}

bool CGamepadOnScreenKeyboardUI::OpenForEditBox(CUICustomEdit* pEdit, const char* sourceTag)
{
    if (!pEdit || pEdit->IsReadOnly())
        return false;

    if (!ps_gp_osk_enabled)
    {
        static thread_local bool s_focusing = false;
        if (!s_focusing && !pEdit->HasInputFocus())
        {
            s_focusing = true;
            pEdit->CaptureFocus(true);
            s_focusing = false;
        }
        Msg("* [Gamepad] gp_osk disabled: focus edit (%s)", sourceTag ? sourceTag : "?");
        return false;
    }

    CDialogHolder* holder = GetActiveDialogHolderForOsk();
    if (!holder)
        return false;

    CUIDialogWnd* parentDlg = holder->TopInputReceiver();
    if (parentDlg == g_pGamepadOskWnd)
        return true;

    CUIGamepadOnScreenKeyboardWnd* wnd = EnsureOskWindow();
    return wnd->StartForEditBox(pEdit, parentDlg, sourceTag);
}

bool CGamepadOnScreenKeyboardUI::TryOpenForActiveDialog(const char* sourceTag)
{
    if (IsOpen())
        return true;

    CDialogHolder* holder = GetActiveDialogHolderForOsk();
    if (!holder)
        return false;

    CUIDialogWnd* top = holder->TopInputReceiver();
    if (!top || !top->IsShown() || top == g_pGamepadOskWnd)
        return false;

    if (CUICustomEdit* focused = FindFocusedEditBoxInTree(top))
    {
        return OpenForEditBox(focused, sourceTag);
    }

    std::vector<CUICustomEdit*> edits;
    CollectVisibleEditBoxes(top, edits);
    if (edits.empty())
        return false;

    return OpenForEditBox(edits.front(), sourceTag);
}

bool CGamepadOnScreenKeyboardUI::CycleSiblingFieldInActiveDialog()
{
    CDialogHolder* holder = GetActiveDialogHolderForOsk();
    if (!holder)
        return false;

    CUIDialogWnd* top = holder->TopInputReceiver();
    if (!top || !top->IsShown())
        return false;

    const bool oskWasTop = (top == g_pGamepadOskWnd);
    CUIDialogWnd* targetDlg = oskWasTop ? holder->PreviousInputReceiver(g_pGamepadOskWnd) : top;
    if (!targetDlg || !targetDlg->IsShown())
        return false;

    std::vector<CUICustomEdit*> edits;
    CollectVisibleEditBoxes(targetDlg, edits);
    if (edits.empty())
        return false;

    CUICustomEdit* cur = oskWasTop ? g_pGamepadOskWnd->GetTargetEdit() : nullptr;
    if (!cur)
        cur = FindFocusedEditBoxInTree(targetDlg);
    size_t nextIdx = 0;
    if (cur)
    {
        auto it = std::find(edits.begin(), edits.end(), cur);
        if (it != edits.end())
            nextIdx = (static_cast<size_t>(std::distance(edits.begin(), it)) + 1) % edits.size();
    }

    if (oskWasTop)
    {
        if (edits[nextIdx] == cur)
            return false;
        return EnsureOskWindow()->StartForEditBox(edits[nextIdx], targetDlg, "cycle_sibling_edit");
    }
    return OpenForEditBox(edits[nextIdx], "cycle_sibling_edit");
}

void CGamepadOnScreenKeyboardUI::OnTargetEditDestroyed(CUICustomEdit* pEdit)
{
    if (!pEdit || !g_pGamepadOskWnd || g_pGamepadOskWnd->GetTargetEdit() != pEdit)
        return;

    g_pGamepadOskWnd->ClearTargetEdit();
    if (g_pGamepadOskWnd->IsShown())
    {
        g_pGamepadOskWnd->ForceCancelAndClose("target_edit_destroyed");
    }
}

void CGamepadOnScreenKeyboardUI::CloseCancel(const char* reason)
{
    if (g_pGamepadOskWnd && g_pGamepadOskWnd->IsShown())
    {
        g_pGamepadOskWnd->ForceCancelAndClose(reason);
    }
}

void CGamepadOnScreenKeyboardUI::CloseCommit(const char* reason)
{
    if (g_pGamepadOskWnd && g_pGamepadOskWnd->IsShown())
    {
        g_pGamepadOskWnd->ForceCommitAndClose(reason);
    }
}

bool CGamepadOnScreenKeyboardUI::IsOpen() const
{
    if (!g_pGamepadOskWnd || !g_pGamepadOskWnd->IsShown())
        return false;

    CDialogHolder* holder = GetActiveDialogHolderForOsk();
    if (!holder || holder->TopInputReceiver() != g_pGamepadOskWnd)
    {
        // Если CDialogHolder::CleanInternals очистил стек диалогов (например, при выходе из MainMenu)
        return false;
    }
    return g_pGamepadOskWnd->GetController().IsOpen();
}

CUICustomEdit* CGamepadOnScreenKeyboardUI::GetActiveEditBox() const
{
    return (g_pGamepadOskWnd && IsOpen()) ? g_pGamepadOskWnd->GetTargetEdit() : nullptr;
}

gposk::OskTargetKind CGamepadOnScreenKeyboardUI::GetActiveTargetKind() const
{
    return g_pGamepadOskWnd ? g_pGamepadOskWnd->GetController().GetTargetKind() : gposk::OskTargetKind::GenericEdit;
}

const gposk::OnScreenKeyboardController& CGamepadOnScreenKeyboardUI::GetController() const
{
    return EnsureOskWindow()->GetController();
}

gposk::OnScreenKeyboardController& CGamepadOnScreenKeyboardUI::GetControllerMutable()
{
    return EnsureOskWindow()->GetControllerMutable();
}

void CGamepadOnScreenKeyboardUI::LogStatus() const
{
    const auto& ctrl = EnsureOskWindow()->GetController();
    Msg("* [Gamepad] gp_osk status: open=%d kind=%s lang=%s case=%s row=%d col=%d buf_len=%u orig_len=%u modal_depth=%u",
        IsOpen() ? 1 : 0,
        gposk::GetTargetKindName(ctrl.GetTargetKind()),
        (ctrl.GetLanguage() == gposk::OskLanguage::RU) ? "RU" : "EN",
        ctrl.IsUpperCase() ? "UPPER" : "lower",
        ctrl.GetFocusRow(),
        ctrl.GetFocusCol(),
        static_cast<u32>(ctrl.GetBufferCp1251().size()),
        static_cast<u32>(ctrl.GetOriginalTextCp1251().size()),
        static_cast<u32>(ctrl.GetModalStack().GetDepth()));
}

bool CGamepadOnScreenKeyboardUI::RunSelfTest()
{
    struct ScopedOskSelfTest
    {
        bool prev;
        ScopedOskSelfTest() : prev(gposk::g_inOskSelfTest) { gposk::g_inOskSelfTest = true; }
        ~ScopedOskSelfTest() { gposk::g_inOskSelfTest = prev; }
    } guard;

    std::string report;
    const bool ok66 = gposk::VerifyAll66RussianLettersRoundtrip(&report);
    Msg("* [Gamepad] gp_osk 66-letter roundtrip: %s", report.c_str());

    gposk::OnScreenKeyboardController ctrl;
    // Проверка B (отмена без ввода) и сохранения исходного текста
    ctrl.OpenSession(gposk::OskTargetKind::SaveDialog, " initial_save ", 0, 1000, "\xCA\xEE\xF0\xE4\xEE\xED", "07-11 14-30");
    ctrl.ApplyTemplateIndex(0);
    ctrl.CancelSession(gpmodal::ModalButtonMask(gpmodal::ModalButton::B));
    const bool okCancel = ctrl.WasCancelled() && !ctrl.WasCommitted() &&
                          ctrl.GetCommittedTextCp1251() == " initial_save " &&
                          ctrl.GetModalStack().IsButtonConsumedByBarrier(gpmodal::ModalButton::B);

    // Проверка шаблона сейва <Локация> — <Дата/Время> и очистки запрещённых символов
    ctrl.OpenSession(gposk::OskTargetKind::SaveDialog, "", 0, 2000, "\xCA\xEE\xF0\xE4\xEE\xED", "07.11 14:30");
    ctrl.ApplyTemplateIndex(0);
    ctrl.CommitSession(0);
    const bool okSaveTmpl = ctrl.WasCommitted() &&
                            gposk::IsValidSaveFileNameCp1251(ctrl.GetCommittedTextCp1251()) &&
                            !ctrl.GetCommittedTextCp1251().empty();

    const bool allOk = ok66 && okCancel && okSaveTmpl;
    Msg("* [Gamepad] gp_osk selftest: %s (ok66=%d okCancel=%d okSaveTmpl=%d)",
        allOk ? "PASS" : "FAIL", ok66 ? 1 : 0, okCancel ? 1 : 0, okSaveTmpl ? 1 : 0);
    return allOk;
}
#endif // !GPOSK_STANDALONE_TEST
