// S3: Аварийный отчёт gamepad_last_error.log и защитная оболочка кадра геймпада ("Core Armor")
// Компилируется как внутри xrEngine (с отключённым PCH, как GamepadMath.cpp / GamepadBindings.cpp),
// так и в автономном юнит-тесте GamepadErrorGuard_test.exe.

#include "GamepadErrorGuard.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gperr
{
namespace
{

std::uint32_t g_totalProtectedErrors = 0;
std::string g_lastReportText;
std::string g_lastWrittenPath;
std::string g_lastCppExceptionWhat;
std::string g_primaryErrorLogPath = "appdata\\gamepad_last_error.log";
std::string g_secondaryErrorLogPath = "gamepad_last_error.log";
FEngineErrorRecoveryCallback g_recoveryCallback = nullptr;
void* g_recoveryUserData = nullptr;
FErrorTranslateUtf8Callback g_translateUtf8Cb = nullptr;
ESimulatedFaultMode g_simulatedFaultMode = ESimulatedFaultMode::None;

std::string TrErrUtf8(const char* stringId, const char* fallbackUtf8)
{
    if (g_translateUtf8Cb)
    {
        std::string res = g_translateUtf8Cb(stringId, fallbackUtf8);
        if (!res.empty())
            return res;
    }
    return fallbackUtf8 ? std::string(fallbackUtf8) : std::string();
}

std::string SafeStr(const char* s, const char* fallback = "")
{
    return (s && s[0]) ? std::string(s) : std::string(fallback ? fallback : "");
}

bool ContainsCaseInsensitive(const std::string& haystack, const char* needle)
{
    if (!needle || !needle[0])
        return false;
    std::string h = haystack;
    std::string n = needle;
    for (char& c : h)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    for (char& c : n)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return h.find(n) != std::string::npos;
}

std::string GetCurrentTimestampString()
{
    std::time_t now = std::time(nullptr);
    std::tm tmLocal = {};
#ifdef _WIN32
    localtime_s(&tmLocal, &now);
#else
    tmLocal = *std::localtime(&now);
#endif
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmLocal.tm_year + 1900,
                  tmLocal.tm_mon + 1,
                  tmLocal.tm_mday,
                  tmLocal.tm_hour,
                  tmLocal.tm_min,
                  tmLocal.tm_sec);
    return std::string(buf);
}

std::string FormatSehCodeName(std::uint32_t code)
{
    switch (code)
    {
    case 0xC0000005u: return TrErrUtf8("ui_gp_eng_err_seh_av", "EXCEPTION_ACCESS_VIOLATION (обращение к недоступной памяти / нулевому указателю)");
    case 0xC0000094u: return TrErrUtf8("ui_gp_eng_err_seh_div0", "EXCEPTION_INT_DIVIDE_BY_ZERO (деление целого числа на ноль)");
    case 0xC000008Cu: return TrErrUtf8("ui_gp_eng_err_seh_bounds", "EXCEPTION_ARRAY_BOUNDS_EXCEEDED (выход за границы массива)");
    case 0xC000001Du: return TrErrUtf8("ui_gp_eng_err_seh_ill", "EXCEPTION_ILLEGAL_INSTRUCTION (недопустимая инструкция процессора)");
    case 0xC00000FDu: return TrErrUtf8("ui_gp_eng_err_seh_stack", "EXCEPTION_STACK_OVERFLOW (переполнение стека)");
    case 0xE06D7363u: return TrErrUtf8("ui_gp_eng_err_seh_cpp", "CPP_STD_EXCEPTION (исключение C++)");
    default:
        break;
    }
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "SEH_EXCEPTION (0x%08X)", code);
    return std::string(buf);
}

bool WriteTextFileAtomicOrDirect(const char* filePath, const std::string& content)
{
    if (!filePath || !filePath[0])
        return false;

#ifdef _CPPUNWIND
    try
#endif
    {
        std::filesystem::path targetPath(filePath);
        if (targetPath.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(targetPath.parent_path(), ec);
        }

        std::string tmpPath = std::string(filePath) + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (ofs)
            {
                ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
                ofs.flush();
                if (ofs.good())
                {
                    ofs.close();
#ifdef _WIN32
                    if (MoveFileExA(tmpPath.c_str(), filePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    {
                        return true;
                    }
                    DeleteFileA(tmpPath.c_str());
#else
                    std::error_code ecRename;
                    std::filesystem::rename(tmpPath, targetPath, ecRename);
                    if (!ecRename)
                        return true;
#endif
                }
            }
        }

        // Резервная прямая запись, если переименование временного файла не удалось
        std::ofstream directOfs(filePath, std::ios::binary | std::ios::trunc);
        if (!directOfs)
            return false;
        directOfs.write(content.data(), static_cast<std::streamsize>(content.size()));
        directOfs.flush();
        return directOfs.good();
    }
#ifdef _CPPUNWIND
    catch (...)
    {
        return false;
    }
#endif
}

} // namespace

ENGINE_API void SetErrorTranslateUtf8Callback(FErrorTranslateUtf8Callback cb)
{
    g_translateUtf8Cb = cb;
}

ENGINE_API std::string DescribeGameMomentRussian(const char* contextName, const char* stageTag, bool wasPaused)
{
    const std::string ctx = SafeStr(contextName, "unknown");
    const std::string stage = SafeStr(stageTag, "unknown");

    std::string moment;
    if (ContainsCaseInsensitive(ctx, "MainMenu") || ContainsCaseInsensitive(stage, "main_menu"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_main_menu", "В главном меню или в окне настроек игры.");
    }
    else if (ContainsCaseInsensitive(ctx, "RadialWheel") || ContainsCaseInsensitive(stage, "gp_wheel"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_wheel", "Во время открытия или выбора предмета в колесе быстрого доступа.");
    }
    else if (ContainsCaseInsensitive(ctx, "World_3DPDA") || ContainsCaseInsensitive(stage, "gp_pda") || ContainsCaseInsensitive(stage, "Pda"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_pda", "Во время работы с КПК (карта Зоны, задания или вкладки КПК).");
    }
    else if (ContainsCaseInsensitive(ctx, "MenuUI") || ContainsCaseInsensitive(stage, "inv") || ContainsCaseInsensitive(stage, "gp_bridge") || ContainsCaseInsensitive(stage, "gp_screens"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_menu_ui", "Во время работы с открытым окном (инвентарь, обыск, торговля, ремонт или окно действия).");
    }
    else if (ContainsCaseInsensitive(ctx, "Dialog_NPC") || ContainsCaseInsensitive(stage, "dialog"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_dialog", "Во время разговора с персонажем (диалог).");
    }
    else if (ContainsCaseInsensitive(ctx, "TextEntry") || ContainsCaseInsensitive(stage, "osk") || ContainsCaseInsensitive(stage, "Keyboard"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_osk", "Во время набора текста на экранной клавиатуре.");
    }
    else if (ContainsCaseInsensitive(ctx, "Sleep") || ContainsCaseInsensitive(stage, "sleep"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_sleep", "В окне выбора времени сна.");
    }
    else if (ContainsCaseInsensitive(ctx, "Placement") || ContainsCaseInsensitive(stage, "furniture"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_placement", "В режиме расстановки мебели или предметов в убежище.");
    }
    else if (ContainsCaseInsensitive(ctx, "Console"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_console", "При открытой командной строке игры (~).");
    }
    else if (ContainsCaseInsensitive(ctx, "World_Gameplay") || ContainsCaseInsensitive(stage, "GamepadUpdate") || ContainsCaseInsensitive(stage, "LookInput") || ContainsCaseInsensitive(stage, "MoveInput"))
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_world", "Во время обычной игры в мире (ходьба, осмотр по сторонам или бой).");
    }
    else
    {
        moment = TrErrUtf8("ui_gp_eng_err_moment_generic", "Во время очередного кадра обработки управления геймпадом.");
    }

    if (wasPaused)
    {
        moment += TrErrUtf8("ui_gp_eng_err_moment_paused_suffix", " Игра в этот момент стояла на паузе.");
    }
    return moment;
}

ENGINE_API std::string DescribeWhatHappenedRussian(const ErrorContextInfo& info)
{
    const std::string unknownStage = TrErrUtf8("ui_gp_eng_err_unknown_stage", "неизвестный участок");
    const std::string unexpectedFault = TrErrUtf8("ui_gp_eng_err_unexpected_fault", "непредвиденный сбой");
    const std::string stage = SafeStr(info.stage_tag, unknownStage.c_str());
    const std::string detail = SafeStr(info.error_detail, unexpectedFault.c_str());

    if (info.is_lua_error || ContainsCaseInsensitive(stage, "gp_") || ContainsCaseInsensitive(stage, "lua"))
    {
        if (ContainsCaseInsensitive(stage, "wheel"))
        {
            return TrErrUtf8("ui_gp_eng_err_what_lua_wheel", "Произошла непредвиденная ошибка при обработке быстрого колеса выбора для геймпада.");
        }
        if (ContainsCaseInsensitive(stage, "pda"))
        {
            return TrErrUtf8("ui_gp_eng_err_what_lua_pda", "Произошла непредвиденная ошибка при управлении КПК с геймпада.");
        }
        if (ContainsCaseInsensitive(stage, "bridge") || ContainsCaseInsensitive(stage, "inv") || ContainsCaseInsensitive(stage, "screens"))
        {
            return TrErrUtf8("ui_gp_eng_err_what_lua_ui", "Произошла непредвиденная ошибка при обработке кнопки или курсора геймпада в окне инвентаря / меню.");
        }
        if (ContainsCaseInsensitive(stage, "weapon") || ContainsCaseInsensitive(stage, "field"))
        {
            return TrErrUtf8("ui_gp_eng_err_what_lua_field", "Произошла непредвиденная ошибка при выполнении полевого или оружейного действия с геймпада.");
        }
        return TrErrUtf8("ui_gp_eng_err_what_lua_generic", "В одном из вспомогательных обработчиков геймпада возникла непредвиденная ошибка.");
    }

    if (info.seh_code != 0)
    {
        return TrErrUtf8("ui_gp_eng_err_what_seh", "При опросе геймпада внутри ядра игры возникла критическая ошибка памяти или вычислений, которую перехватила встроенная броня.");
    }

    if (ContainsCaseInsensitive(stage, "GamepadUpdate"))
    {
        return TrErrUtf8("ui_gp_eng_err_what_update", "Во время покадрового опроса кнопок и стиков геймпада произошёл непредвиденный сбой.");
    }
    if (ContainsCaseInsensitive(stage, "LookInput"))
    {
        return TrErrUtf8("ui_gp_eng_err_what_look", "Во время расчёта поворота камеры правым стиком геймпада произошёл непредвиденный сбой.");
    }
    if (ContainsCaseInsensitive(stage, "MoveInput"))
    {
        return TrErrUtf8("ui_gp_eng_err_what_move", "Во время расчёта движения персонажа левым стиком геймпада произошёл непредвиденный сбой.");
    }

    const std::string fmt = TrErrUtf8("ui_gp_eng_err_what_frame_fmt", "При обработке кадра управления геймпадом произошла непредвиденная ошибка (%s).");
    char msgBuf[1024] = {};
    std::snprintf(msgBuf, sizeof(msgBuf), fmt.c_str(), detail.c_str());
    return std::string(msgBuf);
}

ENGINE_API std::string DescribeProtectionActionRussian(const ErrorContextInfo& info)
{
    if (info.recovery_action && info.recovery_action[0])
    {
        return std::string(info.recovery_action);
    }

    return TrErrUtf8("ui_gp_eng_err_recovery_default", "Проблемный кадр геймпада безопасно пропущен. Все кнопки, курки, стики и вибрация геймпада мгновенно сброшены в нейтральное положение (персонаж не бежит и не стреляет сам по себе), управление автоматически переключено на клавиатуру и мышь, а игра продолжила работу без вылета и без закрытия окна.");
}

ENGINE_API std::string FormatLastErrorReport(const ErrorContextInfo& info, const char* timestampStr)
{
    const std::string ts = (timestampStr && timestampStr[0]) ? std::string(timestampStr) : GetCurrentTimestampString();
    const std::string whatHappened = DescribeWhatHappenedRussian(info);
    const std::string gameMoment = DescribeGameMomentRussian(info.context_name, info.stage_tag, info.was_device_paused);
    const std::string protectionAction = DescribeProtectionActionRussian(info);

    std::ostringstream oss;
    oss << "================================================================================\r\n";
    oss << TrErrUtf8("ui_gp_eng_err_hdr_title", "АВАРИЙНЫЙ ОТЧЁТ ЗАЩИТЫ ГЕЙМПАДА (gamepad_last_error.log)") << "\r\n";
    oss << TrErrUtf8("ui_gp_eng_err_hdr_time", "Время записи: ") << ts << "\r\n";
    oss << "================================================================================\r\n\r\n";

    oss << TrErrUtf8("ui_gp_eng_err_sec_what", "1. ЧТО ПРОИЗОШЛО:") << "\r\n";
    oss << "   " << whatHappened << "\r\n\r\n";

    oss << TrErrUtf8("ui_gp_eng_err_sec_when", "2. В КАКОЙ МОМЕНТ ИГРЫ:") << "\r\n";
    oss << "   " << gameMoment << "\r\n\r\n";

    oss << TrErrUtf8("ui_gp_eng_err_sec_recovery", "3. ЧТО СДЕЛАЛА ЗАЩИТА:") << "\r\n";
    oss << "   " << protectionAction << "\r\n\r\n";

    oss << "--------------------------------------------------------------------------------\r\n";
    oss << TrErrUtf8("ui_gp_eng_err_sec_tech", "ТЕХНИЧЕСКИЕ ДЕТАЛИ ДЛЯ БЫСТРОЙ ПОЧИНКИ:") << "\r\n";
    oss << "--------------------------------------------------------------------------------\r\n";
    oss << "stage_tag            : " << SafeStr(info.stage_tag, "unknown") << "\r\n";
    oss << "origin_layer         : " << (info.is_lua_error ? "Lua (gp_safe_guard / gp_failover)" : "C++ Engine Core (Xr_input / xrGame)") << "\r\n";
    oss << "error_detail         : " << SafeStr(info.error_detail, "none") << "\r\n";
    if (info.seh_code != 0)
    {
        char hexBuf[64] = {};
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", info.seh_code);
        oss << "seh_exception_code   : " << hexBuf << " (" << FormatSehCodeName(info.seh_code) << ")\r\n";
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%016llX", static_cast<unsigned long long>(info.seh_addr));
        oss << "seh_exception_addr   : " << hexBuf << "\r\n";
    }
    oss << "gamepad_context      : " << SafeStr(info.context_name, "unknown") << "\r\n";
    oss << "frame_id             : " << info.frame_id << "\r\n";
    oss << "time_continual_ms    : " << info.time_continual_ms << "\r\n";
    oss << "repeat_count         : " << (info.repeat_count > 0 ? info.repeat_count : 1u) << "\r\n";
    oss << "total_guarded_errors : " << (g_totalProtectedErrors > 0 ? g_totalProtectedErrors : 1u) << "\r\n";
    oss << "gamepad_instance_id  : " << info.gamepad_instance_id << "\r\n";
    oss << "was_gamepad_source   : " << (info.was_gamepad_active_source ? "1 (gamepad -> switched to kbm)" : "0 (kbm)") << "\r\n";
    oss << "was_device_paused    : " << (info.was_device_paused ? "1" : "0") << " (pause_tokens=" << info.pause_tokens_count << ")\r\n";

    char stateBuf[160] = {};
    std::snprintf(stateBuf, sizeof(stateBuf),
                  "buttons=0x%08X axes=[LX:%d, LY:%d, RX:%d, RY:%d, LT:%d, RT:%d] -> RESET_TO_NEUTRAL",
                  info.buttons_mask,
                  static_cast<int>(info.axes[0]),
                  static_cast<int>(info.axes[1]),
                  static_cast<int>(info.axes[2]),
                  static_cast<int>(info.axes[3]),
                  static_cast<int>(info.axes[4]),
                  static_cast<int>(info.axes[5]));
    oss << "gamepad_state_before : " << stateBuf << "\r\n";
    oss << "================================================================================\r\n";

    return oss.str();
}

ENGINE_API bool WriteLastErrorReportToPath(const char* filePath, const ErrorContextInfo& info, const char* timestampStr)
{
    if (!filePath || !filePath[0])
        return false;

    ++g_totalProtectedErrors;
    const std::string report = FormatLastErrorReport(info, timestampStr);
    g_lastReportText = report;

    const bool ok = WriteTextFileAtomicOrDirect(filePath, report);
    if (ok)
    {
        g_lastWrittenPath = filePath;
    }
    return ok;
}

ENGINE_API void SetEngineErrorRecoveryCallback(FEngineErrorRecoveryCallback cb, void* userData)
{
    g_recoveryCallback = cb;
    g_recoveryUserData = userData;
}

ENGINE_API void SetDefaultErrorLogPaths(const char* primaryAppDataLogPath, const char* secondaryRootLogPath)
{
    if (primaryAppDataLogPath && primaryAppDataLogPath[0])
    {
        g_primaryErrorLogPath = primaryAppDataLogPath;
    }
    if (secondaryRootLogPath)
    {
        g_secondaryErrorLogPath = secondaryRootLogPath;
    }
}

ENGINE_API const std::string& GetPrimaryErrorLogPath()
{
    return g_primaryErrorLogPath;
}

ENGINE_API const std::string& GetSecondaryErrorLogPath()
{
    return g_secondaryErrorLogPath;
}

ENGINE_API bool RecordEngineGamepadError(const ErrorContextInfo& info)
{
    ++g_totalProtectedErrors;
    const std::string report = FormatLastErrorReport(info, nullptr);
    g_lastReportText = report;

    bool anyOk = false;
    if (!g_primaryErrorLogPath.empty())
    {
        if (WriteTextFileAtomicOrDirect(g_primaryErrorLogPath.c_str(), report))
        {
            g_lastWrittenPath = g_primaryErrorLogPath;
            anyOk = true;
        }
    }
    if (!g_secondaryErrorLogPath.empty() && g_secondaryErrorLogPath != g_primaryErrorLogPath)
    {
        if (WriteTextFileAtomicOrDirect(g_secondaryErrorLogPath.c_str(), report))
        {
            if (!anyOk)
            {
                g_lastWrittenPath = g_secondaryErrorLogPath;
            }
            anyOk = true;
        }
    }
    return anyOk;
}

ENGINE_API bool RecordSimpleGamepadError(const char* stageTag,
                                         const char* errorDetail,
                                         const char* contextName,
                                         const char* recoveryAction,
                                         bool isLuaError)
{
    if (g_recoveryCallback)
    {
        return g_recoveryCallback(stageTag, errorDetail, 0, 0, contextName, recoveryAction, isLuaError, g_recoveryUserData);
    }

    ErrorContextInfo info = {};
    info.stage_tag = (stageTag && stageTag[0]) ? stageTag : "unknown";
    info.error_detail = (errorDetail && errorDetail[0]) ? errorDetail : "unknown";
    info.context_name = (contextName && contextName[0]) ? contextName : "unknown";
    info.recovery_action = (recoveryAction && recoveryAction[0]) ? recoveryAction : nullptr;
    info.is_lua_error = isLuaError;
    return RecordEngineGamepadError(info);
}

ENGINE_API bool HandleProtectedFrameFailure(const char* stageTag,
                                            const char* errorDetail,
                                            std::uint32_t sehCode,
                                            std::uint64_t sehAddr)
{
    const char* effectiveDetail = errorDetail;
    std::string detailBuf;
    if ((!effectiveDetail || !effectiveDetail[0]) && !g_lastCppExceptionWhat.empty())
    {
        effectiveDetail = g_lastCppExceptionWhat.c_str();
    }
    if (!effectiveDetail || !effectiveDetail[0])
    {
        detailBuf = (sehCode != 0) ? FormatSehCodeName(sehCode) : std::string("Protected gamepad frame error");
        effectiveDetail = detailBuf.c_str();
    }

    if (g_recoveryCallback)
    {
        return g_recoveryCallback(stageTag, effectiveDetail, sehCode, sehAddr, nullptr, nullptr, false, g_recoveryUserData);
    }

    ErrorContextInfo info = {};
    info.stage_tag = (stageTag && stageTag[0]) ? stageTag : "GamepadFrameStep";
    info.error_detail = effectiveDetail;
    info.context_name = "World_Gameplay";
    info.seh_code = sehCode;
    info.seh_addr = sehAddr;
    info.is_lua_error = false;
    return RecordEngineGamepadError(info);
}

ENGINE_API std::uint32_t GetTotalProtectedFrameErrors()
{
    return g_totalProtectedErrors;
}

ENGINE_API const std::string& GetLastErrorReportText()
{
    return g_lastReportText;
}

ENGINE_API const std::string& GetLastWrittenFilePath()
{
    return g_lastWrittenPath;
}

ENGINE_API const std::string& GetLastCaughtCppExceptionWhat()
{
    return g_lastCppExceptionWhat;
}

ENGINE_API void ResetProtectedErrorStatsForTest()
{
    g_totalProtectedErrors = 0;
    g_lastReportText.clear();
    g_lastWrittenPath.clear();
    g_lastCppExceptionWhat.clear();
    g_simulatedFaultMode = ESimulatedFaultMode::None;
}

ENGINE_API void ArmSimulatedFaultNextFrame(ESimulatedFaultMode mode)
{
    g_simulatedFaultMode = mode;
}

ENGINE_API ESimulatedFaultMode ConsumeSimulatedFaultMode()
{
    const ESimulatedFaultMode cur = g_simulatedFaultMode;
    g_simulatedFaultMode = ESimulatedFaultMode::None;
    return cur;
}

#ifdef _WIN32
namespace
{
int FilterGamepadStepSEH(unsigned int code, _EXCEPTION_POINTERS* ep, std::uint32_t* outCode, std::uint64_t* outAddr)
{
    if (outCode)
        *outCode = code;
    if (outAddr)
    {
        *outAddr = (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionAddress)
                       ? reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress)
                       : 0ULL;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool InvokeStepWithOptionalCppCatch(FProtectedStepFn fn, void* userContext, std::uint32_t& outCode)
{
#ifdef _CPPUNWIND
    try
    {
        fn(userContext);
        return true;
    }
    catch (const std::exception& e)
    {
        g_lastCppExceptionWhat = e.what() ? e.what() : "std::exception";
        outCode = 0xE06D7363u;
        return false;
    }
    catch (...)
    {
        g_lastCppExceptionWhat = "unknown C++ exception";
        outCode = 0xE06D7363u;
        return false;
    }
#else
    (void)outCode;
    fn(userContext);
    return true;
#endif
}
} // namespace

ENGINE_API bool ExecuteProtectedGamepadStepSEH(FProtectedStepFn fn,
                                               void* userContext,
                                               std::uint32_t& outSehCode,
                                               std::uint64_t& outSehAddr)
{
    outSehCode = 0;
    outSehAddr = 0;
    if (!fn)
        return true;

    __try
    {
        return InvokeStepWithOptionalCppCatch(fn, userContext, outSehCode);
    }
    __except (FilterGamepadStepSEH(GetExceptionCode(), GetExceptionInformation(), &outSehCode, &outSehAddr))
    {
        return false;
    }
}
#else
ENGINE_API bool ExecuteProtectedGamepadStepSEH(FProtectedStepFn fn,
                                               void* userContext,
                                               std::uint32_t& outSehCode,
                                               std::uint64_t& outSehAddr)
{
    outSehCode = 0;
    outSehAddr = 0;
    if (!fn)
        return true;
    try
    {
        fn(userContext);
        return true;
    }
    catch (...)
    {
        outSehCode = 0xE06D7363u;
        return false;
    }
}
#endif

namespace
{
struct STestFaultCtx
{
    int mode = 0;
    int healthyTicks = 0;
};

void TestStepFunction(void* rawCtx)
{
    STestFaultCtx* ctx = static_cast<STestFaultCtx*>(rawCtx);
    if (!ctx)
        return;
    if (ctx->mode == 1)
    {
#ifdef _WIN32
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    }
    ctx->healthyTicks++;
}
} // namespace

ENGINE_API bool RunSelfTest(const char* customOutputPath)
{
    const std::uint32_t savedErrors = g_totalProtectedErrors;
    const std::string savedReport = g_lastReportText;
    const std::string savedPath = g_lastWrittenPath;

    ResetProtectedErrorStatsForTest();

    std::string testLogPath;
    if (customOutputPath && customOutputPath[0])
    {
        testLogPath = customOutputPath;
    }
    else
    {
        std::error_code ec;
        std::filesystem::path tmpDir = std::filesystem::temp_directory_path(ec);
        if (ec || tmpDir.empty())
            tmpDir = ".";
        testLogPath = (tmpDir / "gamepad_last_error.log").string();
    }

    // 1. Проверка SEH-оболочки на искусственном нарушении доступа (EXCEPTION_ACCESS_VIOLATION)
    STestFaultCtx ctx = {};
    ctx.mode = 1;
    std::uint32_t sehCode = 0;
    std::uint64_t sehAddr = 0;
    const bool step1Ok = ExecuteProtectedGamepadStepSEH(&TestStepFunction, &ctx, sehCode, sehAddr);
    if (step1Ok || sehCode != 0xC0000005u || ctx.healthyTicks != 0)
    {
        return false;
    }

    // 2. Следующий кадр без ошибки должен выполниться штатно
    ctx.mode = 0;
    const bool step2Ok = ExecuteProtectedGamepadStepSEH(&TestStepFunction, &ctx, sehCode, sehAddr);
    if (!step2Ok || sehCode != 0 || ctx.healthyTicks != 1)
    {
        return false;
    }

    // 3. Проверка записи и содержимого файла gamepad_last_error.log (в эталонном русском режиме)
    struct ScopedDisableTranslate
    {
        FErrorTranslateUtf8Callback prev;
        ScopedDisableTranslate() : prev(g_translateUtf8Cb) { g_translateUtf8Cb = nullptr; }
        ~ScopedDisableTranslate() { g_translateUtf8Cb = prev; }
    } disableTranslateGuard;

    ErrorContextInfo info = {};
    info.stage_tag = "CInput::GamepadUpdate";
    info.error_detail = "Artificial SEH access violation during gamepad frame poll";
    info.context_name = "MenuUI_Modal";
    info.seh_code = 0xC0000005u;
    info.seh_addr = 0x0000000140001234ULL;
    info.frame_id = 4242;
    info.time_continual_ms = 128500;
    info.gamepad_instance_id = 7;
    info.buttons_mask = 0x00000001u;
    info.axes[0] = 16000;
    info.axes[1] = -12000;
    info.was_gamepad_active_source = true;
    info.was_device_paused = true;
    info.pause_tokens_count = 1;
    info.is_lua_error = false;
    info.repeat_count = 1;

    if (!WriteLastErrorReportToPath(testLogPath.c_str(), info, "2026-11-07 12:55:00"))
    {
        return false;
    }

    std::ifstream ifs(testLogPath, std::ios::binary);
    if (!ifs)
    {
        return false;
    }
    std::ostringstream readBuf;
    readBuf << ifs.rdbuf();
    const std::string diskText = readBuf.str();
    ifs.close();

    const bool hasRussianHeader1 = (diskText.find("1. ЧТО ПРОИЗОШЛО:") != std::string::npos);
    const bool hasRussianHeader2 = (diskText.find("2. В КАКОЙ МОМЕНТ ИГРЫ:") != std::string::npos);
    const bool hasRussianHeader3 = (diskText.find("3. ЧТО СДЕЛАЛА ЗАЩИТА:") != std::string::npos);
    const bool hasTechSection = (diskText.find("ТЕХНИЧЕСКИЕ ДЕТАЛИ ДЛЯ БЫСТРОЙ ПОЧИНКИ:") != std::string::npos);
    const bool hasStageTag = (diskText.find("CInput::GamepadUpdate") != std::string::npos);
    const bool hasSehCode = (diskText.find("0xC0000005") != std::string::npos);
    const bool hasNeutralReset = (diskText.find("RESET_TO_NEUTRAL") != std::string::npos);

    g_totalProtectedErrors = savedErrors;
    g_lastReportText = savedReport;
    g_lastWrittenPath = savedPath;

    return hasRussianHeader1 && hasRussianHeader2 && hasRussianHeader3 &&
           hasTechSection && hasStageTag && hasSehCode && hasNeutralReset;
}

} // namespace gperr
