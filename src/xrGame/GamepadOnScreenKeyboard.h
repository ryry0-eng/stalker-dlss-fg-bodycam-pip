#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include "GamepadFocusGraph.h"
#include "GamepadModalStack.h"

namespace gposk
{
    // =========================================================================
    // 1. Однобайтовая кодировка Windows-1251 (CP1251) и проверка 66 русских букв
    //    (а..я = 0xE0..0xFF [32], А..Я = 0xC0..0xDF [32], ё = 0xB8 [1], Ё = 0xA8 [1])
    // =========================================================================

    constexpr uint8_t kCp1251CapitalYo = 0xA8; // 'Ё' (U+0401)
    constexpr uint8_t kCp1251SmallYo   = 0xB8; // 'ё' (U+0451)
    constexpr uint8_t kCp1251EmDash    = 0x97; // '—' (U+2014)
    constexpr uint8_t kCp1251EnDash    = 0x96; // '–' (U+2013)
    constexpr uint8_t kCp1251Numero    = 0xB9; // '№' (U+2116)

    constexpr bool IsRussianUpperLetterCp1251(uint8_t b) noexcept
    {
        return (b >= 0xC0 && b <= 0xDF) || (b == kCp1251CapitalYo);
    }

    constexpr bool IsRussianLowerLetterCp1251(uint8_t b) noexcept
    {
        return (b >= 0xE0 && b <= 0xFF) || (b == kCp1251SmallYo);
    }

    constexpr bool IsRussianLetterCp1251(uint8_t b) noexcept
    {
        return IsRussianUpperLetterCp1251(b) || IsRussianLowerLetterCp1251(b);
    }

    constexpr uint8_t ToUpperCp1251(uint8_t b) noexcept
    {
        if (b >= 0xE0 && b <= 0xFF)
            return static_cast<uint8_t>(b - 0x20);
        if (b == kCp1251SmallYo)
            return kCp1251CapitalYo;
        if (b >= 'a' && b <= 'z')
            return static_cast<uint8_t>(b - ('a' - 'A'));
        return b;
    }

    constexpr uint8_t ToLowerCp1251(uint8_t b) noexcept
    {
        if (b >= 0xC0 && b <= 0xDF)
            return static_cast<uint8_t>(b + 0x20);
        if (b == kCp1251CapitalYo)
            return kCp1251SmallYo;
        if (b >= 'A' && b <= 'Z')
            return static_cast<uint8_t>(b + ('a' - 'A'));
        return b;
    }

    uint32_t Cp1251ByteToCodepoint(uint8_t b) noexcept;
    bool CodepointToCp1251Byte(uint32_t codepoint, uint8_t& outByte) noexcept;

    std::string Utf8ToCp1251(std::string_view utf8, size_t* outUnmappedCount = nullptr);
    std::string Cp1251ToUtf8(std::string_view cp1251);

    // Очистка имени файла сохранения от символов, запрещённых в ui_save_dialog.script:176
    // (/ . \ % ? * : | " < >) и в valid_saved_game_name (console_commands.cpp:1032: ^ ( ) [ ])
    std::string SanitizeSaveFileNameCp1251(std::string_view cp1251);
    bool IsValidSaveFileNameCp1251(std::string_view cp1251) noexcept;

    // Модульная самопроверка всех 66 русских букв (а..я, А..Я, ё, Ё) туда и обратно без '?'
    bool VerifyAll66RussianLettersRoundtrip(std::string* outReport = nullptr);

    using FOskTranslateCallback = const char* (*)(const char* stringId, const char* fallbackCp1251);
    void SetOskTranslateCallback(FOskTranslateCallback cb);

    // =========================================================================
    // 2. Целевые окна ввода (FP.1 п. 7) и готовые шаблоны в 1 клик (D51)
    // =========================================================================

    enum class OskTargetKind : uint8_t
    {
        SaveDialog  = 0, // UISaveDialog (ui_save_dialog.script, edit_filename)
        StashDialog = 1, // UICreateStash (item_backpack.script, fld_input)
        WaypointPin = 2, // UIWPSettings / UIPinSettingsWnd (tasks_placeable_waypoints.script, fld_input_name/desc/input)
        NewGameName = 3, // UINewGame (ui_mm_faction_select.script, input_name)
        NumericArgb = 4, // Числовые поля ARGB (UIPinSettingsWnd fld_input_a/r/g/b, numbers_only)
        GenericEdit = 5  // Любой другой CUICustomEdit / CUIEditBox
    };

    const char* GetTargetKindName(OskTargetKind kind) noexcept;

    OskTargetKind ClassifyEditTarget(std::string_view editWindowName,
                                     std::string_view parentWindowName,
                                     std::string_view dialogWindowName,
                                     bool inMainMenu,
                                     bool numbersOnly = false) noexcept;

    struct TemplateItem
    {
        std::string label_cp1251; // Подпись на кнопке шаблона (Windows-1251)
        std::string value_cp1251; // Вставляемый текст (Windows-1251)
    };

    std::string BuildSaveLocationDateTimeCp1251(std::string_view levelNameCp1251,
                                                std::string_view dateTimeCp1251);

    std::vector<TemplateItem> BuildTemplatesForTarget(OskTargetKind kind,
                                                      std::string_view dynamicLevelNameCp1251 = {},
                                                      std::string_view dynamicDateTimeCp1251 = {});

    // =========================================================================
    // 3. Раскладки клавиатуры: RU (ЙЦУКЕН) и EN (QWERTY) + цифровой ряд (D51)
    // =========================================================================

    enum class OskLanguage : uint8_t
    {
        RU = 0, // ЙЦУКЕН (все 33 буквы: а..я + ё)
        EN = 1  // QWERTY (a..z)
    };

    enum class OskActionKind : uint8_t
    {
        None         = 0,
        ToggleLang   = 1, // [RU/EN (R3)]
        ToggleCase   = 2, // [абв/АБВ (L3)]
        Space        = 3, // [Пробел (Y)]
        Backspace    = 4, // [<- Забой (X)]
        Clear        = 5, // [Очистить]
        Commit       = 6, // [Готово (Start)]
        ApplyTemplate = 7 // Шаблон в 1 клик (ряд 0)
    };

    struct OskGridCellInfo
    {
        int32_t       row         = 0;
        int32_t       col         = 0;
        OskActionKind action      = OskActionKind::None;
        uint8_t       char_cp1251 = 0;    // Для символьных клавиш (ряды 1..4)
        std::string   label_cp1251;       // Отображаемая подпись ячейки в Windows-1251
    };

    // Сетка клавиатуры:
    //   row = 0: полоса шаблонов в 1 клик (6 кнопок, col = 0..5)
    //   row = 1: цифровой ряд (12 клавиш, col = 0..11)
    //   row = 2: верхний буквенный ряд ЙЦУКЕН / QWERTY (12 клавиш, col = 0..11)
    //   row = 3: средний буквенный ряд ФЫВА / ASDF + ё/Ё (12 клавиш, col = 0..11)
    //   row = 4: нижний буквенный ряд ЯЧСМ / ZXCV (12 клавиш, col = 0..11)
    //   row = 5: нижний ряд действий (6 кнопок, col = 0..5: Язык, Регистр, Пробел, Забой, Очистить, Готово)
    class OnScreenKeyboardController
    {
    public:
        static constexpr int32_t  kTemplateRow      = 0;
        static constexpr int32_t  kDigitRow         = 1;
        static constexpr int32_t  kLetterRowTop     = 2;
        static constexpr int32_t  kLetterRowHome    = 3;
        static constexpr int32_t  kLetterRowBottom  = 4;
        static constexpr int32_t  kActionRow        = 5;
        static constexpr int32_t  kTotalRows        = 6;

        static constexpr int32_t  kTemplateCols     = 6;
        static constexpr int32_t  kKeyCols          = 12;
        static constexpr int32_t  kActionCols       = 6;
        static constexpr size_t   kDefaultMaxLen    = 128;

        void OpenSession(OskTargetKind kind,
                         std::string_view initialTextCp1251,
                         uint32_t heldButtonsMask,
                         uint32_t nowContinualMs,
                         std::string_view dynamicLevelNameCp1251 = {},
                         std::string_view dynamicDateTimeCp1251 = {},
                         size_t maxLen = kDefaultMaxLen);
        void SeedHeldExtraButtons(bool startDown, bool l3Down, bool r3Down) noexcept;

        // Навигация по сетке (D-Pad / левый стик)
        bool MoveFocus(gpfocus::NavDir dir);
        bool SetFocusCell(int32_t row, int32_t col);

        int32_t GetFocusRow() const noexcept { return m_focusRow; }
        int32_t GetFocusCol() const noexcept { return m_focusCol; }
        int32_t GetColsForRow(int32_t row) const noexcept;
        OskGridCellInfo GetCellInfo(int32_t row, int32_t col) const;

        // Действия кнопок геймпада по D51 / F5.1:
        //   A     — нажать выбранную ячейку (буква/цифра, шаблон или кнопка действия, включая «Готово»)
        //   X     — удалить символ (Backspace)
        //   Y     — пробел
        //   L3/LT — переключить регистр (строчные <-> ЗАГЛАВНЫЕ)
        //   R3    — переключить язык (RU ЙЦУКЕН <-> EN QWERTY)
        //   Start — сразу подтвердить ввод и закрыть клавиатуру
        //   B     — закрыть клавиатуру без ввода (с барьером consumed_until_release и сохранением исходного текста)
        bool PressFocusedCell(uint32_t heldButtonsMask = 0);
        bool PressBackspace();
        bool PressSpace();
        void ToggleLanguage();
        void SetLanguage(OskLanguage lang) noexcept { m_language = lang; }
        void ToggleCase() noexcept { m_upperCase = !m_upperCase; }
        void SetUpperCase(bool upper) noexcept { m_upperCase = upper; }
        void ClearBuffer() noexcept { m_bufferCp1251.clear(); }
        bool ApplyTemplateIndex(size_t idx);
        bool AppendCharCp1251(uint8_t ch);
        bool SetBufferCp1251(std::string_view textCp1251);

        bool CommitSession(uint32_t heldButtonsMask = 0);
        bool CancelSession(uint32_t heldButtonsMask = 0);

        // Покадровый шаг (с учётом барьера F1.2 и автоповтора 320/90 мс по Device.dwTimeContinual)
        void StepController(uint32_t rawModalButtonsMask,
                            bool pressStart,
                            bool pressL3,
                            bool pressR3,
                            bool hasStickDir,
                            gpfocus::NavDir stickDir,
                            uint32_t nowContinualMs);

        // Найти координаты символа CP1251 на текущей раскладке (для модульных тестов всех 66 букв)
        bool FindCharOnGrid(OskLanguage lang, bool upperCase, uint8_t targetCp1251, int32_t& outRow, int32_t& outCol) const noexcept;

        bool IsOpen() const noexcept { return m_isOpen; }
        bool WasCommitted() const noexcept { return m_wasCommitted; }
        bool WasCancelled() const noexcept { return m_wasCancelled; }

        OskTargetKind GetTargetKind() const noexcept { return m_targetKind; }
        OskLanguage GetLanguage() const noexcept { return m_language; }
        bool IsUpperCase() const noexcept { return m_upperCase; }
        size_t GetMaxLength() const noexcept { return m_maxLength; }

        const std::string& GetBufferCp1251() const noexcept { return m_bufferCp1251; }
        const std::string& GetOriginalTextCp1251() const noexcept { return m_originalTextCp1251; }
        const std::string& GetCommittedTextCp1251() const noexcept { return m_committedTextCp1251; }
        const std::vector<TemplateItem>& GetTemplates() const noexcept { return m_templates; }

        const gpmodal::ModalStack& GetModalStack() const noexcept { return m_modalStack; }
        gpmodal::ModalStack& GetModalStackMutable() noexcept { return m_modalStack; }

    private:
        bool                      m_isOpen             = false;
        bool                      m_wasCommitted       = false;
        bool                      m_wasCancelled       = false;
        OskTargetKind             m_targetKind         = OskTargetKind::GenericEdit;
        OskLanguage               m_language           = OskLanguage::RU;
        bool                      m_upperCase          = false;
        size_t                    m_maxLength          = kDefaultMaxLen;

        int32_t                   m_focusRow           = kLetterRowTop;
        int32_t                   m_focusCol           = 0;
        int32_t                   m_lastKeyCol12       = 0;

        std::string               m_originalTextCp1251;
        std::string               m_bufferCp1251;
        std::string               m_committedTextCp1251;
        std::vector<TemplateItem> m_templates;

        gpfocus::FocusGraph       m_callerFocusGraph;
        gpmodal::ModalStack       m_modalStack;
        gpmodal::ContinualRepeatTimer m_navRepeatTimer;
        gpmodal::ContinualRepeatTimer m_backspaceRepeatTimer;
        uint32_t                  m_stepFrameId        = 1;

        bool                      m_prevStartDown      = false;
        bool                      m_prevL3Down         = false;
        bool                      m_prevR3Down         = false;
    };
} // namespace gposk

#ifndef GPOSK_STANDALONE_TEST
class CUICustomEdit;
class CUIWindow;
class CUIDialogWnd;

class CGamepadOnScreenKeyboardUI
{
public:
    static CGamepadOnScreenKeyboardUI& Instance();

    bool OpenForEditBox(CUICustomEdit* pEdit, const char* sourceTag = "edit_focus");
    bool TryOpenForActiveDialog(const char* sourceTag = "dialog_shortcut");
    bool CycleSiblingFieldInActiveDialog();
    void OnTargetEditDestroyed(CUICustomEdit* pEdit);
    void CloseCancel(const char* reason = "cancel");
    void CloseCommit(const char* reason = "commit");

    bool IsOpen() const;
    CUICustomEdit* GetActiveEditBox() const;
    gposk::OskTargetKind GetActiveTargetKind() const;
    const gposk::OnScreenKeyboardController& GetController() const;
    gposk::OnScreenKeyboardController& GetControllerMutable();

    // Поиск редактируемых полей CUICustomEdit и интерактивных кнопок в активном диалоге
    static void CollectVisibleEditBoxes(CUIWindow* root, std::vector<CUICustomEdit*>& outEdits);
    static CUICustomEdit* FindFocusedEditBoxInTree(CUIWindow* root);

    void LogStatus() const;
    bool RunSelfTest();

private:
    CGamepadOnScreenKeyboardUI() = default;
};
#endif // !GPOSK_STANDALONE_TEST
