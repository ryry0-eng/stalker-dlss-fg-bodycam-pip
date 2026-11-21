// S3: Аварийный отчёт gamepad_last_error.log и защитная оболочка кадра геймпада ("Core Armor")
#pragma once

#include <cstdint>
#include <string>

#ifndef ENGINE_API
#define ENGINE_API
#endif

namespace gperr
{

// Информация о состоянии игры и геймпада в момент ошибки для формирования понятного человеку и технического отчёта
struct ErrorContextInfo
{
    const char* stage_tag = "unknown";        // точка сбоя: "CInput::GamepadUpdate", "CPauseTokenManager::OnFrame", "Lua:gp_bridge.on_actor_update" и т.д.
    const char* error_detail = "unknown";     // описание ошибки или код исключения SEH / C++ / Lua
    const char* context_name = "unknown";     // контекст геймпада: "World_Gameplay", "MenuUI_Modal", "RadialWheel", "World_3DPDA", "MainMenu" и т.д.
    const char* recovery_action = nullptr;    // если nullptr — подставляется стандартное описание действий защиты
    std::uint32_t seh_code = 0;               // 0 для логических/Lua ошибок, либо код SEH (например 0xC0000005)
    std::uint64_t seh_addr = 0;               // адрес инструкции при SEH-сбое
    std::uint32_t frame_id = 0;               // номер кадра Device.dwFrame
    std::uint32_t time_continual_ms = 0;      // время Device.dwTimeContinual (мс)
    std::uint32_t gamepad_instance_id = 0;    // ID активного геймпада SDL3
    std::uint32_t buttons_mask = 0;           // битовая маска кнопок до сброса
    std::int16_t axes[6] = { 0, 0, 0, 0, 0, 0 }; // оси LX, LY, RX, RY, LT, RT до сброса
    bool was_gamepad_active_source = false;   // был ли геймпад активным источником ввода до аварии
    bool was_device_paused = false;           // стояла ли игра на паузе
    std::uint32_t pause_tokens_count = 0;     // число активных токенов паузы
    bool is_lua_error = false;                // true, если ошибка пришла из Lua (gp_safe_guard / gp_failover / gp_error_log)
    std::uint32_t repeat_count = 1;           // сколько раз подряд или всего произошла эта ошибка
};

// Перевод технического названия контекста в понятное человеку описание момента игры (на русском языке, UTF-8)
ENGINE_API std::string DescribeGameMomentRussian(const char* contextName, const char* stageTag, bool wasPaused);

// Перевод технической ошибки в понятное человеку описание «Что произошло» (на русском языке, UTF-8)
ENGINE_API std::string DescribeWhatHappenedRussian(const ErrorContextInfo& info);

// Перевод действий защиты в понятное человеку описание «Что сделала защита» (на русском языке, UTF-8)
ENGINE_API std::string DescribeProtectionActionRussian(const ErrorContextInfo& info);

// Сформировать полный текст отчёта gamepad_last_error.log (сначала простым русским языком, ниже — технические детали)
ENGINE_API std::string FormatLastErrorReport(const ErrorContextInfo& info, const char* timestampStr = nullptr);

// Записать отчёт в указанный файл (атомарно через .tmp + замена, либо прямая запись)
ENGINE_API bool WriteLastErrorReportToPath(const char* filePath, const ErrorContextInfo& info, const char* timestampStr = nullptr);

// Записать отчёт в основные пути стенда ($app_data_root$\gamepad_last_error.log и корень стенда gamepad_last_error.log)
ENGINE_API bool RecordEngineGamepadError(const ErrorContextInfo& info);

// Упрощённая точка вызова из Lua (`game.gp_write_last_error_log(stage_tag, error_detail, context_name, recovery_action)`) и C++
ENGINE_API bool RecordSimpleGamepadError(const char* stageTag,
                                         const char* errorDetail,
                                         const char* contextName = nullptr,
                                         const char* recoveryAction = nullptr,
                                         bool isLuaError = false);

// Обработчик аварийного кадра геймпада (вызывается при срабатывании защитной оболочки в CInput / xrGame):
// сбрасывает геймпад в нейтраль, переключает ввод на клавиатуру и мышь и записывает gamepad_last_error.log.
using FEngineErrorRecoveryCallback = bool (*)(const char* stageTag,
                                              const char* errorDetail,
                                              std::uint32_t sehCode,
                                              std::uint64_t sehAddr,
                                              const char* contextOverride,
                                              const char* recoveryOverride,
                                              bool isLuaError,
                                              void* userData);

using FErrorTranslateUtf8Callback = std::string (*)(const char* stringId, const char* fallbackUtf8);
ENGINE_API void SetErrorTranslateUtf8Callback(FErrorTranslateUtf8Callback cb);

ENGINE_API void SetEngineErrorRecoveryCallback(FEngineErrorRecoveryCallback cb, void* userData = nullptr);
ENGINE_API void SetDefaultErrorLogPaths(const char* primaryAppDataLogPath, const char* secondaryRootLogPath = nullptr);
ENGINE_API const std::string& GetPrimaryErrorLogPath();
ENGINE_API const std::string& GetSecondaryErrorLogPath();

ENGINE_API bool HandleProtectedFrameFailure(const char* stageTag,
                                            const char* errorDetail,
                                            std::uint32_t sehCode = 0,
                                            std::uint64_t sehAddr = 0);

// Счётчики и состояние защиты ядра геймпада
ENGINE_API std::uint32_t GetTotalProtectedFrameErrors();
ENGINE_API const std::string& GetLastErrorReportText();
ENGINE_API const std::string& GetLastWrittenFilePath();
ENGINE_API const std::string& GetLastCaughtCppExceptionWhat();
ENGINE_API void ResetProtectedErrorStatsForTest();

// Управление искусственным сбоем для проверки и самотеста (`gp_error_log simulate_seh|simulate_cpp|selftest`)
enum class ESimulatedFaultMode : std::uint8_t
{
    None = 0,
    ThrowCppExceptionInPoll = 1,
    RaiseSehAccessViolationInPoll = 2,
    ThrowCppExceptionInOnFrame = 3
};

ENGINE_API void ArmSimulatedFaultNextFrame(ESimulatedFaultMode mode);
ENGINE_API ESimulatedFaultMode ConsumeSimulatedFaultMode();

// Универсальный SEH-исполнитель для одного шага кадра геймпада:
// перехватывает аппаратные/SEH исключения (например, обращение по нулевому указателю 0xC0000005),
// не давая игре закрыться, и возвращает код исключения в outSehCode.
using FProtectedStepFn = void (*)(void* userContext);
ENGINE_API bool ExecuteProtectedGamepadStepSEH(FProtectedStepFn fn,
                                               void* userContext,
                                               std::uint32_t& outSehCode,
                                               std::uint64_t& outSehAddr);

// Автономный самотест модуля (проверяет перехват ошибки, сброс в нейтраль, переключение на КМ и содержимое gamepad_last_error.log)
ENGINE_API bool RunSelfTest(const char* customOutputPath = nullptr);

} // namespace gperr
