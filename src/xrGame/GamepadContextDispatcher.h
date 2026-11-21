#pragma once

#include "../xrEngine/xr_input.h"
#include "../xrEngine/GamepadMath.h"

// D3.1: Диспетчер контекстов ввода геймпада (R3 REC-1 / REC-2)
// - Каскад приоритетов из 12 контекстов (оценивается 1 раз за кадр)
// - Машина состояний кнопок: Pressed / Held / Released + автоповтор навигации на Device.dwTimeContinual
// - Барьер отпускания consumed_until_release при любой смене контекста (REC-2)
// - Подавление стиков, триггеров и кнопок при открытой консоли ~ (закрывает C4.3-D6)
// - Явная разметка и C++ сеттеры для Lua-зависимых подконтекстов, передаваемых из gp_bridge в D3.2

enum class EGamepadContext : u8
{
	Console = 0,                  // Приоритет 1: Console && Console->bVisible
	TextEntry = 1,                // Приоритет 2: CUICustomEdit в цепочке m_pKeyboardCapturer или Lua MCM k_binder (D3.2)
	MainMenu = 2,                 // Приоритет 3: MainMenu()->IsActive() или !g_pGameLevel
	Cutscene_InputBlock = 3,      // Приоритет 4: g_bDisableAllInput / m_FPCam / external_handler / lookat / dead / only_allow_movekeys / Lua FDDA (D3.2)
	Sleep = 4,                    // Приоритет 5: UISleep (помечено для D3.2: Lua _G._GUIs["UISleep"] / имя окна "UISleep")
	Dialog_NPC = 5,               // Приоритет 6: CUITalkWnd::IsShown() / Actor()->IsTalking()
	RadialWheel = 6,              // Приоритет 7: колесо быстрого доступа / патронов / компаньонов (Lua-тип из D3.2 + C++ эвристика !StopAnyMove)
	Placement_HideoutFurniture = 7, // Приоритет 8: размещение мебели Hideout Furniture (помечено для D3.2: Lua placeable_furniture state 2/3/4)
	World_3DPDA = 8,              // Приоритет 9: CUIPdaWnd::IsShown() (подрежимы: активный UI vs опущенный freelook)
	MenuUI_Modal = 9,             // Приоритет 10: любое модальное окно CUIDialogWnd на CurrentGameUI()->TopInputReceiver()
	World_VehicleOrHolder = 10,   // Приоритет 11: Actor()->Holder() != nullptr (транспорт / станковое оружие)
	World_Gameplay = 11,          // Приоритет 12: штатный геймплей в мире от первого лица

	Count = 12
};

class CActor;
class CObject;
class CUIWindow;
class CUIDialogWnd;
class CDialogHolder;
class CUIButton;
class CUIComboBox;
struct SMenuFocusTarget;
// E2.2e: "липкая" цель взгляда для геймпада (gp_x_target_angle); nullptr при мыши/клавиатуре. GamepadContextDispatcher.cpp
CObject* GamepadFindStickyUseTarget(CActor* pActor);

class CGamepadContextDispatcher
{
public:
	// Расширенные виртуальные индексы кнопок (15 физических кнопок CInput::EGamepadButtonBit 0..14 + 2 цифровых триггера LT/RT)
	enum EExtendedButtonBit : u32
	{
		VBTN_PHYS_COUNT = CInput::GP_BTN_DPAD_RIGHT + 1, // 15 физических кнопок (0..14)
		VBTN_LT = VBTN_PHYS_COUNT,                       // 15: левый триггер (axes[4]) как цифровая кнопка с гистерезисом
		VBTN_RT = VBTN_PHYS_COUNT + 1,                   // 16: правый триггер (axes[5]) как цифровая кнопка с гистерезисом
		VBTN_TOTAL_COUNT = VBTN_PHYS_COUNT + 2           // 17 бит
	};

	// Направления меню-навигации (D-Pad + левый стик с гистерезисом)
	enum class ENavDir : u8
	{
		None = 0,
		Up = 1,
		Down = 2,
		Left = 3,
		Right = 4
	};

	// Типы радиальных колёс (передаются из Lua в D3.2)
	enum class ERadialWheelType : u8
	{
		None = 0,
		QuickAction_Haru = 1, // UIHaruQAW (haruka_quick_action_wheel.script)
		Interactive_Pie = 2,  // UIRadialMenu (ui_radial_menu.script)
		Camo_3DSS = 3,        // UI3DSSCamoWheel (zas_3dss_camo_wheel.script)
		Ammo = 4,             // UIWheelAmmo (item_weapon.script)
		Companion_Flat = 5,   // UIWheelCompanion (idiots_ui.script — плоская панель с кнопками)
		CppHeuristic = 6      // Определено из C++ по TopInputReceiver() (!StopAnyMove && != PDA) без уточнения из Lua
	};

	// Состояния размещения мебели Hideout Furniture (placeable_furniture.script:28-34, передаются из Lua в D3.2)
	enum class EPlacementState : u8
	{
		None = 0,
		Holding = 2, // STATE.HOLDING = 2 (TopInputReceiver == nullptr, перенос предмета в мире)
		AdvCtrl = 3, // STATE.ADV_CTRL = 3 (открыто окно UIAdvancedControls)
		Preview = 4  // STATE.PREVIEW = 4 (TopInputReceiver == nullptr, предпросмотр)
	};

	// D3.1 / D3.2: явный набор флагов для контекстов из R3, передаваемых из Lua (gp_bridge.script) при изменении (REC-4)
	struct SLuaBridgeHints
	{
		bool bMcmKeybindCapture = false;      // R3 #2: ui_mcm.GUI.k_binder ~= nil
		bool bFddaInputDisabled = false;      // R3 #4b: liz_fdda_input_manager.is_input_disabled
		bool bActorSleepingInfo = false;      // R3 #5b: has_alife_info("actor_is_sleeping")
		bool bSleepDialogShown = false;       // R3 #5a: _G._GUIs["UISleep"] открыт
		ERadialWheelType radialWheelType = ERadialWheelType::None; // R3 #7: активное колесо в Lua
		EPlacementState placementState = EPlacementState::None;   // R3 #8: placeable_furniture.get_state()
		shared_str modalMenuTag;              // R3 #10: уточняющий тег Lua-окна (например, "UIInventory:loot")
		int placementDikPlace = 0;            // R3 #8: DIK подтверждения установки мебели (0 = get_action_dik(kUSE))
		int placementDikCollision = 15;       // R3 #8: DIK переключения коллизии (DIK_TAB = 15)
		int placementDikAlign = 58;           // R3 #8: DIK переключения выравнивания (DIK_CAPITAL = 58)
		int placementDikControls = 199;       // R3 #8: DIK переключения окна ADV_CTRL/PREVIEW (DIK_HOME = 199)
		u32 luaHintSyncCount = 0;             // Число пушей изменённого состояния из gp_bridge в C++
	};

	// D3.2: статистика дискретной инъекции через CLevel::IR_OnKeyboardPress/Release/Hold (REC-3)
	struct SInjectionStats
	{
		u32 totalPressInjected = 0;
		u32 totalHoldInjected = 0;
		u32 totalReleaseInjected = 0;
		u32 blockedByContextCount = 0;
		u32 blockedByPlacementGuardCount = 0;
		int lastInjectedDik = 0;
		int lastInjectedAction = 0;
		EGamepadContext lastInjectedContext = EGamepadContext::MainMenu;
		LPCSTR lastInjectedPhase = "none";
	};

	// Детализация подрежима активного контекста за текущий кадр
	struct SContextSubFlags
	{
		bool bMainMenuHasModalSubDialog = false; // MainMenu()->TopInputReceiver() != nullptr
		bool bPartialMoveOnlyBlock = false;      // g_block_all_except_movement или liz_fdda (движение/камера разрешены)
		bool bCutsceneFullBlock = false;         // g_bDisableAllInput / m_FPCam / external_handler / lookat / dead
		bool b3dPdaInteractive = false;          // CUIPdaWnd::IsShown() && IsEnabled() (активный экран КПК vs freelook)
		bool bModalAllowsMovement = false;       // TopInputReceiver() != nullptr && !TopInputReceiver()->StopAnyMove()
		bool bDetectedViaLuaHint = false;        // Контекст или подрежим уточнён через SLuaBridgeHints (D3.2)
	};

	static constexpr u32 REPEAT_INITIAL_DELAY_MS = 320;
	static constexpr u32 REPEAT_STEP_MS = 90;
	// MENU-FOCUS-1: в меню (контекст MainMenu) крестовина и левый стик прыгают одинаково: 350 мс до автоповтора, далее 8 шагов/с, порог стика 50%
	static constexpr u32 MENU_FOCUS_REPEAT_INITIAL_DELAY_MS = 350;
	static constexpr u32 MENU_FOCUS_REPEAT_STEP_MS = 125;
	static constexpr float MENU_FOCUS_STICK_ENTER_THRESHOLD = 0.50f;
	static constexpr u32 GAMEPAD_CROUCH_HOLD_MS = 300;   // E1.1 (2, D16): удержание B >= 300 мс -> низкий присед (Box 2)
	static constexpr u32 GAMEPAD_FREELOOK_HOLD_MS = 200; // E1.1 (3): удержание R3 >= 200 мс вне ADS -> freelook
	static constexpr u32 GAMEPAD_DPAD_HOLD_MS = 300;     // E2.1 / E3.1 (D25): удержание крестовины >= 300 мс -> второе действие
	static constexpr u32 GAMEPAD_LB_HOLD_MS = 250;       // E3.1 / E5.1 (D13, kb/05 §3.1): короткое LB < 250 мс -> болт/быстрый слот; удержание >= 250 мс -> колесо
	// E2.1 (1): пороги LT/RT берутся из профиля калибровки (gpmath::TriggerConfig press/release, D2.2/D37);
	// константы ниже больше не используются для курков и оставлены только как справочные значения D3.2.
	static constexpr s16 TRIGGER_PRESS_THRESHOLD = 16384;   // ~0.50 (D3.2, устарело с E2.1)
	static constexpr s16 TRIGGER_RELEASE_THRESHOLD = 11468; // ~0.35 (D3.2, устарело с E2.1)

	static CGamepadContextDispatcher& Instance();

	// По-кадровое обновление (вызывается 1 раз за кадр из CPauseTokenManager::OnFrame)
	void OnFrame();

	// Текущий и предыдущий контекст
	EGamepadContext GetCurrentContext() const { return m_currentContext; }
	EGamepadContext GetPreviousContext() const { return m_prevContext; }
	bool DidContextChangeThisFrame() const { return m_bContextChangedThisFrame; }
	const SContextSubFlags& GetSubFlags() const { return m_subFlags; }
	LPCSTR GetLastTransitionReason() const { return m_lastTransitionReason; }

	// Разрешение мировых осей стиков (закрывает C4.3-D6: в Console и модальных меню возвращает false)
	bool AllowsWorldMove() const;
	bool AllowsWorldLook() const;
	bool AllowsGamepadButtons() const;

	// Состояния кнопок с учётом барьера consumed_until_release (REC-2)
	bool IsButtonPressed(u32 btnBit) const;
	bool IsButtonHeld(u32 btnBit) const;
	bool IsButtonReleased(u32 btnBit) const;
	bool WasButtonReleasedOnContextExit(u32 btnBit) const;
	bool IsButtonConsumedUntilRelease(u32 btnBit) const;

	u32 GetRawDownMask() const { return m_rawDownMask; }
	u32 GetConsumedUntilReleaseMask() const { return m_consumedUntilReleaseMask; }
	u32 GetEffectiveDownMask() const { return m_effectiveDownMask; }
	u32 GetPressedMask() const { return m_pressedMask; }
	u32 GetHeldMask() const { return m_heldMask; }
	u32 GetReleasedMask() const { return m_releasedMask; }
	u32 GetReleasedOnContextExitMask() const { return m_releasedOnContextExitMask; }

	// Автоповтор навигации (D-Pad / левый стик) на Device.dwTimeContinual с учётом барьера
	ENavDir GetActiveNavDir() const { return m_activeNavDir; }
	bool IsNavDirTriggeredThisFrame(ENavDir* outDir = nullptr, bool* outIsRepeat = nullptr, LPCSTR* outSource = nullptr) const;
	bool IsStickNavConsumedUntilNeutral() const { return m_bStickNavConsumedUntilNeutral; }
	bool IsStickMoveConsumedUntilNeutral() const { return m_bStickMoveConsumedUntilNeutral; }

	// Принудительно пометить конкретную кнопку или все зажатые кнопки как consumed_until_release
	void ConsumeButtonUntilRelease(u32 btnBit);
	void ConsumeAllCurrentlyHeldButtons();

	// C++ API для Lua-моста D3.2 (контексты, которые не определяются из C++ без Lua)
	void SetLuaMcmKeybindCapture(bool active);
	void SetLuaFddaInputDisabled(bool active);
	void SetLuaActorSleepingInfo(bool active);
	void SetLuaSleepDialogShown(bool active);
	void SetLuaRadialWheelType(ERadialWheelType wheelType);
	void SetLuaPlacementState(EPlacementState state);
	void SetLuaModalMenuTag(LPCSTR tag);
	void NotifyModalLayerTransition(LPCSTR newModalTag, LPCSTR reason = "modal_layer_transition");
	LPCSTR GetActiveModalLayerTag() const { return m_activeModalLayerTag.size() ? m_activeModalLayerTag.c_str() : ""; }
	void SetLuaBridgeHintsBatch(bool mcmBind, bool fddaDis, bool sleepInfo, bool sleepDlg, int wheelType, int placementState, LPCSTR modalTag);
	void SetLuaPlacementKeys(int dikPlace, int dikCollision, int dikAlign, int dikControls);
	void PollLuaBridgeHints();
	void ResetLuaHints(LPCSTR reason = "reset");
	const SLuaBridgeHints& GetLuaHints() const { return m_luaHints; }

	// D3.2: инъекция дискретных действий геймпада через CLevel::IR_OnKeyboardPress/Release/Hold (REC-3)
	void DispatchDiscreteActions();
	void StepWorldMovementButtons(u32 nowContinual, bool& outHandledL3, bool& outHandledB, bool& outHandledR3);
	// E2.1: оружейные кнопки World_Gameplay (LT Hold ADS, наклоны LB/RB в ADS, 3DSS крестовиной, R3 смена прицела,
	// крестовина Влево/Вправо коротко/удержание). Возвращает маску кнопок, обработанных здесь (generic-цикл их пропускает).
	u32 StepWorldWeaponButtons(u32 nowContinual, bool handledR3);
	bool IsGamepadAdsHeld() const { return m_bLtAdsActive; }
	void LogWeaponState() const; // консоль gp_weapon_state
	bool IsGamepadFreelookHeld() const { return m_bR3FreelookActive; }
	void SetGamepadFreelookHeldForTest(bool active) { m_bR3FreelookActive = active; }
	int ResolveActionDik(int actionId, bool* outUsedSynthetic = nullptr) const;
	bool ResolveContextualButtonInjection(u32 btnBit,
	                                      EGamepadContext ctx,
	                                      const SContextSubFlags& subFlags,
	                                      int& outActionId,
	                                      int& outExplicitDik,
	                                      LPCSTR& outReason) const;
	bool InjectKeyboardEvent(int dik, int actionId, LPCSTR phase, EGamepadContext ctx, bool emitLog = false);
	const SInjectionStats& GetInjectionStats() const { return m_injectionStats; }

	// Диагностика и самопроверка (gp_ctx / gp_ctx selftest)
	void LogState() const;
	bool RunSelfTest();

	// E5.1 (D13, D14, D43, D45): быстрое колесо (gp_wheel.script), выбор сектора с гистерезисом и барьер закрытия
	void ResetWheelSectorState();
	gpmath::RadialSectorResult UpdateWheelSectorFromRightStick(int sectorCount, int currentSector);
	bool IsWheelStickActive() const { return m_bWheelStickActive; }
	LPCSTR GetWheelTimeMode() const { return m_wheelTimeMode.size() ? m_wheelTimeMode.c_str() : "slowmo"; }
	void SetWheelTimeMode(LPCSTR mode);
	void LogWheelState() const;
	bool RunWheelSelfTest();

	// E5.2b + FG.1 (D59): виртуальный курсор геймпада:
	// - в RadialWheel (Interactive_Pie, Camo_3DSS, Ammo, Companion_Flat): правый стик двигает курсор;
	// - в MenuUI_Modal (UIInventory и другие курсорные окна): левый стик двигает курсор с автозадержкой (55% торможение)
	//   и мягким притяжением к центру ячейки/кнопки, правый стик (вертикаль) крутит списки по Device.dwTimeContinual,
	//   а крестовина (D-Pad Up/Down/Left/Right) прыгает по ячейкам и строкам контекстного меню с автопрокруткой.
	struct SCursorSnapTarget
	{
		float cx = 0.0f;
		float cy = 0.0f;
		float w = 41.0f;
		float h = 41.0f;
		int containerId = 0;
		LPCSTR kind = "cell";
	};

	static constexpr float VIRTUAL_CURSOR_SPEED_UI_PX_PER_S = 700.0f;      // база UI 1024x768
	static constexpr float VIRTUAL_CURSOR_FRICTION_MULT = 0.45f;           // FG.1: ~55% снижение скорости над целью
	static constexpr float VIRTUAL_CURSOR_SOFT_SNAP_PULL_PER_S = 6.5f;     // FG.1: мягкая доводка к центру ячейки
	static constexpr float VIRTUAL_CURSOR_SOFT_SNAP_STOP_RADIUS_PX = 1.2f; // FG.1: остановка доводки вблизи центра
	static constexpr u32 VIRTUAL_CURSOR_SOFT_SNAP_WINDOW_MS = 220;         // FG.1: окно мягкой доводки после движения стиком
	static constexpr float VIRTUAL_CURSOR_SCROLL_STEPS_PER_S = 7.0f;       // FG.1: скорость прокрутки правым стиком (шагов/с)
	static constexpr u32 VIRTUAL_CURSOR_SCROLL_MIN_INTERVAL_MS = 30;       // FG.1: анти-троттлинг (>25 мс гейта UICellContainer:On_Scroll)

	void StepVirtualCursor(u32 nowContinual);
	static bool IsVirtualCursorWheelType(ERadialWheelType t);
	bool IsVirtualCursorActive() const { return m_bVcActive; }
	// CURSOR1: режим виртуальной мыши для неподдерживаемых модовых окон (ActionId::MouseCursor, по умолчанию R3 в MenuUI)
	bool IsGamepadMouseMode() const { return m_bGamepadMouseMode; }
	bool SetGamepadMouseMode(bool enable, LPCSTR reason = "api", CUIDialogWnd* explicitOwnerTir = nullptr);
	LPCSTR GetMouseModeHintBarText() const;
	// INV-REWORK-16 (п.6): Lua gp_bridge.cursor_locked (окно «Подробно» поверх инвентаря) — курсор стоит и не рисуется
	bool IsVcLocked() const { return m_bVcLocked; }
	bool IsVcHidden() const { return m_bVcHidden; } // FIX-LIVE2 p.20: tutorial cards, no gamepad cursor
	// INV-REWORK-21: диалог с НПС с геймпада — выбор реплик без курсора (рендер ничего не рисует)
	bool IsDialogSelectMode() const;
	void StepDialogSelect(CUIDialogWnd* tir, u32 dtMs, u32 nowContinual);
	u32 m_dlgLastMoveMs = 0;
	bool m_bVcLocked = false;
	bool m_bVcHidden = false;
	bool IsCursorOverInteractiveTarget() const { return m_bVcActive && m_bVcTargetHit; }
	Fvector2 GetCursorTargetCenter() const { return Fvector2().set(m_vcTargetCx, m_vcTargetCy); }
	Fvector2 GetCursorTargetSize() const { return Fvector2().set(m_vcTargetW, m_vcTargetH); }
	struct SMenuFocusDraw
	{
		enum class Shape { Rect, Ellipse, Caret };
		Shape shape = Shape::Rect;
		Frect frame;
		// P14 Caret: frame.x1/y1 = anchor (UI: left edge of caption text or radio ring, its center y);
		// caretGapPx = gap on the 2559x1079 reference frame; caretKey changes when the focused row changes.
		float caretGapPx = 16.0f;
		const void* caretKey = nullptr;
		const void* transitionRoot = nullptr;
		const void* transitionPanel = nullptr;
	};
	bool GetMenuFocusDraw(SMenuFocusDraw& out) const;
	bool HasMenuFocus(const CUIWindow* widget) const;
	int GetMenuDefaultHoldFrame() const;
	int GetMenuBackHoldFrame(const CUIButton* button) const;
	LPCSTR GetCursorTargetKind() const { return m_vcTargetKind.size() ? m_vcTargetKind.c_str() : "none"; }
	// MENU-FOCUS-1: режим «выделение вместо курсора» (главное меню, Настройки, MCM, загрузка/сохранение при активном геймпаде)
	bool IsMenuFocusMode() const;
	// INV-REWORK-1: инвентарь (все режимы) при геймпаде — белая рамка вокруг цели вместо курсора, левый стик прыгает по целям
	bool IsInventoryFocusFrameMode() const;
	// INV-REWORK-2: инвентарь открыт при геймпаде, но диспетчер ещё не вошёл в режим рамки (первые кадры
	// после открытия) — спросить Lua напрямую, чтобы не мелькал золотой курсор (кэш на кадр)
	bool IsInventoryFocusFramePending() const; // только чтение флага (можно из рендера)
	void UpdateInventoryFocusFramePending();   // опрос Lua — только игровой поток
	// MENU-FOCUS-1: цель со своей ванильной подсветкой (пункты главного меню, «лупа» CUIMMShniaga) — белую рамку не рисуем
	bool IsMenuFocusNativeHighlightTarget() const;
	int GetMenuShortcutIndex(CUIButton* button) const;
	// MENU-FOCUS-4 P13: footer row spans the strip/list above it, buttons share it evenly.
	void ArrangeMenuShortcutRow(CUIButton* button);
	// MENU-FOCUS-5 P14: -dbg console gp_menu_layout_scan walks every Settings/MCM page and logs caption/control misalignment.
	void StartMenuLayoutScan();
	void TickMenuLayoutScan();
	u32 GetVirtualCursorMoveCount() const { return m_vcMoveCount; }
	u32 GetVirtualCursorScrollCount() const { return m_vcScrollCount; }
	u32 GetVirtualCursorDpadSnapCount() const { return m_vcDpadSnapCount; }
	bool IsInventoryModalActive() const;
	bool IsWorkshopModalActive() const; // WS1: top Lua window is UIWorkshop or UIFurnitureWorkshop
	bool IsInventoryLbModifierActive() const { return m_bInvLbModifierActive; }
	u32 GetInventoryFastButtonCount() const { return m_invFastButtonCount; }
	LPCSTR GetLastInventoryFastButton() const { return m_invLastFastButton ? m_invLastFastButton : "none"; }

	bool IsPdaInteractiveActive() const;
	bool IsPdaMapEyeActive() const;
	void SetPdaMapFiltersMode(bool enabled) { m_bPdaMapFiltersMode = enabled; }
	bool IsPdaMapFiltersMode() const { return m_bPdaMapFiltersMode; }
	bool IsPdaEyeSpotHit() const { return m_bPdaEyeSpotHit; }
	Fvector2 GetPdaEyeSpotCenter() const { return Fvector2().set(m_pdaEyeSpotCx, m_pdaEyeSpotCy); }
	Fvector2 GetPdaEyeSpotSize() const { return Fvector2().set(m_pdaEyeSpotW, m_pdaEyeSpotH); }
	void SnapPdaCursor(const Fvector2& targetPos);
	u32 GetPdaFastButtonCount() const { return m_pdaFastButtonCount; }
	LPCSTR GetLastPdaFastButton() const { return m_pdaLastFastButton ? m_pdaLastFastButton : "none"; }

	static Fvector2 StepCursorKinematics(Fvector2 curPos,
	                                     gpmath::Vec2 moveStick,
	                                     u32 dtMs,
	                                     bool targetHit,
	                                     Fvector2 targetCenter,
	                                     u32 timeSinceStickActiveMs,
	                                     float& ioRemX,
	                                     float& ioRemY,
	                                     bool* outMoved = nullptr,
	                                     bool* outSoftSnapped = nullptr,
	                                     bool smoothAccelCurve = false);
	static int StepRightStickScrollAccumulator(float stickY,
	                                           u32 dtMs,
	                                           u32 nowContinual,
	                                           float& ioScrollAccum,
	                                           u32& ioLastScrollStepMs);
	static int SelectBestDpadSnapTarget(Fvector2 curPos,
	                                    int curContainerId,
	                                    ENavDir dir,
	                                    const SCursorSnapTarget* targets,
	                                    int targetCount);
	void LogCursorState() const;
	bool RunCursorSelfTest();

	static LPCSTR GetContextName(EGamepadContext ctx);
	static u32 GetContextPriorityNumber(EGamepadContext ctx); // 1..12
	static bool IsContextLuaAssistedByDesign(EGamepadContext ctx);
	static LPCSTR GetNavDirName(ENavDir dir);
	static LPCSTR GetRadialWheelTypeName(ERadialWheelType type);
	static LPCSTR GetPlacementStateName(EPlacementState state);

private:
	CGamepadContextDispatcher() = default;

	EGamepadContext EvaluateContextWaterfall(SContextSubFlags& outSubFlags, LPCSTR& outReason) const;
	u32 SampleRawButtonsAndTriggersMask();
	ENavDir SampleStickCardinalDirection(s16 lxRaw, s16 lyRaw);
	void StepStateMachine(EGamepadContext evaluatedCtx,
	                      const SContextSubFlags& evaluatedSubFlags,
	                      LPCSTR evalReason,
	                      u32 rawButtonsMask,
	                      s16 lxRaw,
	                      s16 lyRaw,
	                      u32 nowContinual,
	                      bool emitTelemetry);

	u32 m_lastUpdateFrame = 0;
	u32 m_lastDispatchFrame = 0;
	bool m_bInitialized = false;
	bool m_bInSelfTest = false; // D3.3b: в RunSelfTest() изолирует AllowsWorldMove/Look/Buttons от глобального Console->bVisible

	EGamepadContext m_currentContext = EGamepadContext::MainMenu;
	EGamepadContext m_prevContext = EGamepadContext::MainMenu;
	bool m_bContextChangedThisFrame = false;
	SContextSubFlags m_subFlags = {};
	LPCSTR m_lastTransitionReason = "init";
	u32 m_lastTransitionTimeContinual = 0;
	u32 m_transitionCount = 0;

	// Маски кнопок (биты 0..14 = CInput::EGamepadButtonBit, бит 15 = VBTN_LT, бит 16 = VBTN_RT)
	u32 m_rawDownMask = 0;
	u32 m_consumedUntilReleaseMask = 0;
	u32 m_effectiveDownMask = 0;
	u32 m_prevEffectiveDownMask = 0;
	u32 m_pressedMask = 0;
	u32 m_heldMask = 0;
	u32 m_releasedMask = 0;
	u32 m_releasedOnContextExitMask = 0;

	bool m_bLtDigitalDown = false;
	bool m_bRtDigitalDown = false;

	// Состояние навигации и автоповтора на Device.dwTimeContinual
	ENavDir m_stickCardinalDir = ENavDir::None;
	ENavDir m_activeNavDir = ENavDir::None;
	LPCSTR m_activeNavSource = "none";
	bool m_bNavTriggeredThisFrame = false;
	bool m_bNavTriggeredIsRepeat = false;
	bool m_menuVerticalBlocked = false;
	bool m_bStickNavConsumedUntilNeutral = false;
	bool m_bStickMoveConsumedUntilNeutral = false; // FG.6h: барьер аналогового движения левым стиком после закрытия UI до нейтрали
	u32 m_navPressTimeContinual = 0;
	u32 m_navNextRepeatTimeContinual = 0;

	SLuaBridgeHints m_luaHints = {};
	shared_str m_activeModalLayerTag; // F1.2: отслеживание активного слоя стека модалок внутри MenuUI_Modal
	int m_activeInjectedDik[VBTN_TOTAL_COUNT] = {};
	int m_activeInjectedAction[VBTN_TOTAL_COUNT] = {};
	EGamepadContext m_activeInjectedCtx[VBTN_TOTAL_COUNT] = {};
	SInjectionStats m_injectionStats = {};

	// E1.1: двухрежимные кнопки перемещения в World_Gameplay (B: Toggle присед / Hold низкий присед; R3: Tap удар / Hold freelook)
	bool m_bEastDualModePending = false;
	bool m_bEastHoldCommitted = false;
	u32 m_eastPressTimeContinual = 0;
	bool m_bR3DualModePending = false;
	bool m_bR3FreelookActive = false;
	u32 m_r3PressTimeContinual = 0;
	int m_r3FreelookDik = 0;

	// E2.1: состояние оружейных кнопок
	void InjectWeaponTap(int actionId, LPCSTR tag);
	int ResolveNextWeaponSlotAction();
	void SetAdsLean(int side, LPCSTR tag); // side: -1 влево, +1 вправо, 0 — сброс
	u32 m_nextWeaponCycleIdx = 0;
	u32 m_fddaDisabledSinceMs = 0;
	bool m_bLtAdsActive = false;         // LT удерживается и мы послали kWPN_ZOOM press
	bool m_bLtAdsAdopted = false;        // LT нажат при уже включённом (AF_AIM_TOGGLE) зуме — его не выключаем
	int m_adsLeanSide = 0;               // -1/0/+1 — наклон, включённый геймпадом в ADS (Toggle по D16)
	bool m_bDpadLeftPending = false;
	bool m_bDpadLeftHoldCommitted = false;
	u32 m_dpadLeftPressTime = 0;
	bool m_bDpadRightPending = false;
	bool m_bDpadRightHoldCommitted = false;
	u32 m_dpadRightPressTime = 0;
	LPCSTR m_lastWeaponAction = "none";
	u32 m_lastWeaponActionTime = 0;
	u32 m_weaponActionCount = 0;
	u32 m_tutorialRoutedPressCount = 0; // E2.2c: нажатия, доставленные через активный туториал (костёр)

	// E2.2 (D15): контекстная X — прямые адаптеры gp_weapon.script, без эмуляции клавиш
	static constexpr u32 GAMEPAD_X_HOLD_MS = 350;   // удержание X >= 350 мс -> AmmoCheck (kb/05 §3.1 T_hold)
	int m_xUseHoldDik = 0;                 // INV-REWORK-21: kUSE зажат удержанием X на цели
	u32 m_xUseHoldBtn = 0xFFFFFFFFu;
	// GAYS v0.2 (2026-11-13): LB + B freelook toggle, LB + View tutorial prompt
	bool m_bChordFreelookOn = false;
	int m_chordFreelookDik = 0;
	u32 m_tutorialChordBtn = 0xFFFFFFFFu;
	u32 m_tutorialChordStart = 0;
	bool m_tutorialChordHoldSent = false;
	static constexpr u32 GAMEPAD_TUTORIAL_HOLD_MS = 500;
	static constexpr u32 GAMEPAD_X_REPEAT_MS = 250; // повторное X < 250 мс после старта перезарядки -> быстрый сброс
	void RunXAdapter(LPCSTR luaFn, LPCSTR phase, LPCSTR why);
	void TapUseFromX(LPCSTR targetWhy); // E2.2d: причина цели (hud_target / tutorial_kUSE / script:<id>)
	bool BeginUseHoldFromX(u32 btn);
	void EndUseHoldFromX(LPCSTR why);
	void NoteXNearTargetNoReload(LPCSTR detail); // E2.2d
	bool m_bXPending = false;        // X нажата в мире, ждём отпускания (< 350 мс) или порога удержания
	bool m_bXHoldCommitted = false;  // порог удержания пройден, AmmoCheck вызван
	u32 m_xPressTime = 0;
	u32 m_xLastReloadStartTime = 0;  // время старта перезарядки коротким X (0 — нет окна повтора)
	u32 m_xActionCount = 0;
	shared_str m_xLastChoice;        // "<фаза>/<результат адаптера>"
	u32 m_xLastChoiceTime = 0;

	// E3.1 (D12, D13, D16, D25, D46): быстрые устройства и полевые действия вне ADS (gp_field.script)
	void RunFieldAdapter(LPCSTR luaFn, LPCSTR phase, LPCSTR why, string256* outResult = nullptr);
	bool m_bDpadUpPending = false;
	bool m_bDpadUpHoldCommitted = false;
	u32 m_dpadUpPressTime = 0;
	bool m_bDpadDownPending = false;
	bool m_bDpadDownHoldCommitted = false;
	u32 m_dpadDownPressTime = 0;
	bool m_bLbPending = false;
	bool m_bLbHoldCommitted = false;
	u32 m_lbPressTime = 0;
	u32 m_fieldActionCount = 0;
	shared_str m_fieldLastChoice;
	u32 m_fieldLastChoiceTime = 0;

	// E5.1: состояние быстрого колеса в RadialWheel (LT/RT переключение вкладок, отпускание кнопки колеса из профиля K2, правый стик с гистерезисом)
	u32 StepRadialWheelButtons(u32 nowContinual);
	u32 GetWheelHoldButtonBit() const { return m_wheelHoldBtnBit; }
	bool m_bWheelLbWasPhysicallyDown = false;
	bool m_weaponSelectorOpen = false;
	bool m_weaponSelectorLookGate = false;
	u32 m_wheelHoldBtnBit = CInput::GP_BTN_LEFT_SHOULDER;
	u32 m_freelookBtnBit = CInput::GP_BTN_RIGHT_STICK;
	u32 m_lastProfileDiskCheckTime = 0;
	void ExecuteWorldGameplayAction(int gpbindActionId, u32 btnBit, int triggerType, u32 nowContinual);
	bool m_worldDualPending[VBTN_TOTAL_COUNT] = {};
	bool m_worldDualHoldCommitted[VBTN_TOTAL_COUNT] = {};
	u32 m_worldDualPressTime[VBTN_TOTAL_COUNT] = {};
	bool m_bWheelStickActive = false;
	int m_wheelSelectedSector = -1;
	float m_wheelLastAngleRad = 0.0f;
	float m_wheelLastMagnitude = 0.0f;
	shared_str m_wheelTimeMode;

	// E5.2b + FG.1 + FG.2 + FG.4: состояние виртуального курсора, автозадержки (магнита), прокрутки правым стиком, прыжков крестовиной и быстрых кнопок UIInventory / CUIPdaWnd
	enum class EMenuFocusAction { Navigate, Enter, Back, View };
	bool StepMenuFocus(CUIDialogWnd* root, ENavDir dir, int panelStep, int wheel, u32 now,
	                   EMenuFocusAction action = EMenuFocusAction::Navigate);
	bool m_menuFocusOnTrackbar = false;
	void ApplyMenuFocus(CUIDialogWnd* root, const SMenuFocusTarget& target, CUIComboBox* combo, bool reveal, u32 now, int scrollDirection = 0);
	bool HandleMenuShortcuts(u32 now);
	bool HandleConfirmationShortcuts();
	bool HandleMenuCaptureInput(u32 now);
	bool HandleMenuPageInput(u32 now);
	CUIDialogWnd* m_menuFocusRoot = nullptr;
	CUIDialogWnd* m_menuMemoryRoot = nullptr;
	CUIWindow* m_menuTarget = nullptr;
	CUIWindow* m_menuPanel = nullptr;
	bool m_menuCaretValid = false;
	const CUIWindow* m_menuTransitionPanel = nullptr;
	const CUIWindow* m_menuRadioGroup = nullptr;
	u32 m_menuRadioEntered = 0;
	bool m_menuRadioAtRing = false;
	Fvector2 m_menuCaretAnchor{};
	float m_menuCaretGapPx = 16.0f;
	struct SMenuLayoutScan
	{
		bool active = false;
		CUIDialogWnd* root = nullptr;
		xr_vector<int> path;
		u32 waitUntil = 0;
		int pages = 0;
		int issues = 0;
		int rows = 0;
		xr_string log;
	} m_menuScan;
	CUIComboBox* m_menuOpenCombo = nullptr;
	struct SMenuPosition
	{
		CUIWindow* widget = nullptr;
		int index = -1;
	};
	xr_map<xr_string, SMenuPosition> m_menuPanelMemory;
	CUIButton* m_menuShortcutButtons[4] = {};
	u32 m_menuFooterLayoutFrame = u32(-1);
	CUIDialogWnd* m_menuHoldRoot = nullptr;
	CUIButton* m_menuHoldDefault = nullptr;
	xr_string m_menuHoldPage;
	u32 m_menuHoldStart = 0;
	bool m_menuHoldFired = false;
	CUIDialogWnd* m_menuBackRoot = nullptr;
	CUIButton* m_menuBackButton = nullptr;
	u32 m_menuBackStarted = 0;
	bool m_menuBackClosedCombo = false;
	u32 m_menuNativeCaptureButtons = 0;
	CUIDialogWnd* m_menuLoadRoot = nullptr;
	shared_str m_menuLoadArmed;
	shared_str m_menuLastSaveRow[2]; // MENU-FOCUS P16: last chosen row of the load [0] / save [1] window (whole session)
	int m_menuLoadFooterIndex = 0;
	int m_menuDeleteIndex = -1;
	u32 m_menuDeleteCount = 0;
	bool m_menuDeleteModalSeen = false;
	void RefreshCursorTargetFromLua(Fvector2 pos, u32 nowContinual);
	bool TryDpadSnapInModalUI(ENavDir dir, u32 nowContinual);
	u32 StepInventoryButtons(u32 nowContinual);
	u32 StepPdaButtons(u32 nowContinual);
	u32 StepItemSheetButtons(u32 nowContinual); // INV-REWORK-17: LB/RB/Y/A/B in ui_itm_details
	u32 StepWorkshopButtons(u32 nowContinual); // WS1: UIWorkshop / UIFurnitureWorkshop buttons -> gp_bridge.ws_button
	u32 StepHoldButtonProtocol(u32 nowContinual,
	                           int ownerId,
	                           bool active,
	                           LPCSTR functorName,
	                           const void* buttonsPtr,
	                           size_t buttonCount,
	                           bool allowMutantLootCancel,
	                           u32* ioFastCount,
	                           LPCSTR* ioLastFast,
	                           LPCSTR logPrefix);
	bool m_bPdaMapFiltersMode = false;
	bool m_bPdaEyeSpotHit = false;
	bool m_bPdaEyeOnButton = false; // FIX-LIVE2 п.7: LS увёл курсор с карты на кнопку, око не рисуется до RS
	bool m_pdaTaskSnapPending = false; // FIX-LIVE3 п.21: снап к строке задания на кадр после открытия списка
	Fvector2 m_pdaReticleOff = Fvector2().set(0.0f, 0.0f); // FIX-LIVE4: смещение метки карты от центра, когда карта упёрлась в край
	float m_pdaEyeSpotCx = 0.0f;
	float m_pdaEyeSpotCy = 0.0f;
	float m_pdaEyeSpotW = 0.0f;
	float m_pdaEyeSpotH = 0.0f;
	bool m_bVcActive = false;
	bool m_bVcInModalMenu = false;
	bool m_bInvFocusFrame = false; // INV-REWORK-1: ответ gp_bridge.inv_focus_frame_active() за текущий кадр
	u32 m_invPendingFrame = u32(-1);
	bool m_bInvPending = false;
	CUIDialogWnd* m_pVcLastMainMenuTir = nullptr;
	bool m_bInvLbModifierActive = false;
	bool m_bInvR3FullStats = false; // INV-REWORK-20: RS нажимали в инвентаре -> при выходе сбросить show_full_stats
	u32 m_invFastButtonCount = 0;
	LPCSTR m_invLastFastButton = "none";
	u32 m_pdaFastButtonCount = 0;
	LPCSTR m_pdaLastFastButton = "none";
	u32 m_vcLastTime = 0;
	u32 m_vcLastStickMoveTime = 0;
	u32 m_vcLastTargetQueryTime = 0;
	u32 m_vcLastScrollStepTime = 0;
	float m_vcRemX = 0.0f;
	float m_vcRemY = 0.0f;
	float m_vcScrollAccum = 0.0f;
	bool m_bVcTargetHit = false;
	float m_vcTargetCx = 0.0f;
	float m_vcTargetCy = 0.0f;
	float m_vcTargetW = 0.0f;
	float m_vcTargetH = 0.0f;
	shared_str m_vcTargetKind;
	u32 m_vcMoveCount = 0;
	u32 m_vcSoftSnapCount = 0;
	u32 m_vcScrollCount = 0;
	u32 m_vcDpadSnapCount = 0;
	ENavDir m_vcLastDpadSnapDir = ENavDir::None;

	// CURSOR1: состояние режима виртуальной мыши (m_pMouseModeOwnerTir используется ТОЛЬКО для сравнения указателя tir == m_pMouseModeOwnerTir!)
	CUIDialogWnd* GetActiveModalDialog(CDialogHolder** outHolder = nullptr) const;
	void ValidateGamepadMouseModeAutoReset();
	bool CanEnableGamepadMouseMode(CUIDialogWnd* tir) const;
	bool StepGamepadMouseModeToggle(u32 nowContinual);
	void UpdateMouseModeHintBarCache(u32 nowContinual, bool force = false);
	void ReleaseActiveMouseModeInjections();
	bool m_bGamepadMouseMode = false;
	CUIDialogWnd* m_pMouseModeOwnerTir = nullptr;
	u32 m_mouseModeHoldStartMs = 0;
	bool m_bMouseModeHoldFired = false;
	u32 m_mouseModeLastHintPollMs = 0;
	volatile LONG m_mouseModeHintActiveIdx = 0;
	char m_mouseModeHintBuf[2][512] = {};
};
