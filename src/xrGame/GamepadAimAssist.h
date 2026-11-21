#pragma once

// E4.1 + E4.2: нативная помощь прицеливания для геймпада (решения D05, D17, D18, D19, D44).
// Четыре части: Slowdown (множитель правого стика в конусе цели), Rotational Tracking
// (компенсация собственного движения игрока левым стиком), сопровождение движущейся цели в ADS,
// мягкий довод при входе в ADS (~0.2 с). Точка наведения — центр масс (bip01_spine1, иначе центр
// габарита), никогда не голова. На оптике >4x остаётся только Slowdown, остальное плавно гаснет.
// Баллистику, отдачу и разброс модуль не трогает: он только меняет ввод камеры.
// E4.2: полная цепочка анти-радарных фильтров D19 (жив/враждебен, не компаньон axr_companions,
// не ранен/сдался xr_wounded, маскировка actor_<comm>/gameplay_disguise, кэшированный вердикт Lua
// on_enemy_eval/xr_combat_ignore/xr_bribe, накопление прозрачности кустов/препятствий
// fVisTransparencyFactor вдоль луча, освещённость цели ROS()->get_luminocity() с учётом активного
// фонаря CTorch/CFlashlight и ПНВ/тепловизора ps_r2_nightvision/ps_r2_heatvision, ослабление
// дальности в густом тумане fog_density/fog_distance). Любая ошибка или неизвестность — вклад 0
// (fail-closed, D19). Встроенная QPC-телеметрия бюджета кадра (p50/p95/p99/max в мкс).

class CActor;
class CCameraBase;
class CWeapon;

namespace gpaa
{
// gp_aa_level: 0 Выкл / 1 Плавно / 2 Средне / 3 Сильно / 4 Супер заметно / 5 Полное автонаведение (full_auto,
// специальные возможности, E4.1b). По умолчанию «Средне» (D44). Числа уровней — профили в GamepadAimAssist.cpp (D55).
// При 0 вызывающий код (ActorInput.cpp) не вызывает модуль вообще (D05).
extern u32 g_level;
extern int g_disableOnScope; // K6b: 1 — отключать помощь прицеливания на оптике (>4x) и в 3D-прицелах (3DSS), 0 — оставлять

struct FrameInput
{
	float dt = 0.f;
	float lookX = 0.f;   // правый стик после калибровки (ProcessStick), +x вправо
	float lookY = 0.f;   // +y вверх
	float moveMag = 0.f; // модуль левого стика после калибровки
	bool ads = false;    // IsGamepadAdsHeld() || IsZoomAimingMode()
	bool gamepadSource = false;
	CActor* actor = nullptr;
	CCameraBase* cam = nullptr;
	CWeapon* weapon = nullptr; // nullptr — в руках не оружие
	bool eligible = false;     // вид от первого лица, нет freelook/транспорта/подстройки HUD
};

struct FrameOutput
{
	float lookX = 0.f;
	float lookY = 0.f;
	float addYaw = 0.f;   // рад, >0 — поворот вправо (kRIGHT)
	float addPitch = 0.f; // рад, >0 — поднять взгляд (в CCameraFirstEye это kDOWN)
};

void Process(const FrameInput& in, FrameOutput& out);
void RegisterConsoleCommands();
} // namespace gpaa
