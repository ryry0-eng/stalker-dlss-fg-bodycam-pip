#include "stdafx.h"
#include "GamepadAimAssist.h"
#include "../xrEngine/device.h"
#include "../xrEngine/XR_IOConsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/CameraBase.h"
#include "../xrEngine/Environment.h"
#include "../xrEngine/GameMtlLib.h"
#include "../xrEngine/Render.h"
#include "../Include/xrRender/Kinematics.h"
#include "Level.h"
#include "GamePersistent.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "ActorNightVision.h"
#include "Weapon.h"
#include "WeaponBinoculars.h"
#include "WeaponKnife.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "character_info.h"
#include "relation_registry.h"
#include "relation_registry_inline.h"
#include "Torch.h"
#include "Flashlight.h"
#include "entity_alive.h"
#include "CustomMonster.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "ai_space.h"
#include "Script_Game_Object.h"
#include "../xrServerEntities/script_engine.h"
#include "PauseTokenManager.h"
#include "ai/monsters/bloodsucker/bloodsucker.h"
#include <algorithm>

// E4.1 + E4.2 — см. GamepadAimAssist.h. Все углы в радианах.
// ПНВ и тепловизор из рендера (монолитная линковка exe, аналогично bloodsucker.cpp:884).
extern int ps_r2_nightvision;
extern int ps_r2_heatvision;

namespace gpaa
{
u32 g_level = 2; // «Средне» по умолчанию (D44)
// E4.1b: растёт при каждой фактической смене gp_aa_level из консоли/user.ltx. Process сравнивает
// со своим S.lastEpochSeen и при расхождении сбрасывает переходное состояние (цель, engage, довод).
static u32 g_levelEpoch = 0;
static u32 g_levelPrev = 2;
static bool g_levelLogQuiet = false; // selftest гоняет команду сам — без строк смены в логе

// Тонкие ползунки частей 0..1 («Дополнительно», D44). Сохраняются в user.ltx как обычные консольные переменные.
static float g_partSlowdown = 1.f;
static float g_partRotational = 1.f;
static float g_partTracking = 1.f;
static float g_partAdsPull = 1.f;
static float g_partAuto = 1.f; // E4.1b: сила автонаведения уровня full_auto (0..1)
int g_disableOnScope = 1;      // K6b: 1 — отключать всю помощь прицеливания (включая Slowdown) на оптике >4x и в 3DSS; 0 — не ослаблять
static int g_debugLog = 0;

// E4.2: пороги анти-радарных фильтров D19 (доступны для калибровки в пакете E4.3/T11).
static float g_visThreshold = 0.25f;    // минимальный накопленный fVisTransparencyFactor сквозь кусты
static float g_minLuminosity = 0.04f;   // минимальная освещённость ROS()->get_luminocity() без фонаря/ПНВ
static float g_torchRange = 35.f;       // эффективная дальность подсветки цели фонарём игрока (м)
static float g_fogDensityCutoff = 0.55f;// плотность тумана, при которой дальность AA жестко режется

namespace
{
// --- Константы модели (стартовые ориентиры; калибровка — пакет E4.3/T11) ---
constexpr float kMaxRange = 160.f;            // м, дальше помощь не работает (FIX-LIVE2 п.3: 160 м в ADS)
constexpr float kHipRange = 30.f;             // м, FIX-LIVE2 п.3: от бедра помощь только до 30 м
constexpr float kStrongLeadSec = 0.04f;       // с, FIX-LIVE2 п.2: «Сильная» упреждает цель на 40 мс
constexpr float kScanPeriodMs = 100.f;        // поиск кандидатов 10 раз в секунду
constexpr float kConeMinRad = 1.5f * PI / 180.f; // нижний предел конуса (дальние цели), общий для уровней
constexpr float kKeepConeFactor = 1.3f;       // гистерезис удержания текущей цели
constexpr float kPullConeMinRad = 1.f * PI / 180.f;
constexpr float kPullTau = 0.08f;             // постоянная времени экспоненциального довода

// E4.1b (решение Миши 2026-11-06, вариант A): у каждого уровня свой профиль вместо одного множителя силы.
// Значения итоговые (уже с учётом уровня); ползунки gp_aa_* (0..1) их только ослабляют.
// smooth и medium совпадают с E4.1 (0.35/0.6 от прежних констант 0.6/0.8/0.7/0.7 и 90/60/45 °/с);
// strong слегка поднят, super заметно сильнее (конус шире, стик в центре x0.25, полная компенсация шага).
struct LevelProfile
{
	float slowdown;        // доля снижения скорости правого стика в центре конуса (стик x(1-slowdown))
	float coneRadiusM;     // «радиус» конуса захвата вокруг центра масс, м
	float coneMaxRad;      // верхний предел конуса (близкие цели)
	float rotGain;         // доля компенсации собственного смещения (Rotational Tracking)
	float rotMaxRate;      // рад/с
	float trkGain;         // доля сопровождения движения цели (только ADS)
	float trkMaxRate;      // рад/с
	float pullGain;        // мягкий довод при входе в ADS
	float pullMaxRate;     // рад/с
	float pullDuration;    // с (D44: ~0.2 с, не рывок)
	float pullConeRadiusM; // малый конус довода, м
	float pullConeMaxRad;
	// E4.1b: верхний уровень full_auto («Полное автонаведение», специальные возможности, решение Миши
	// 2026-11-06 п.3). У остальных уровней autoGain = 0 — автонаведения нет.
	float coneMinRad;      // нижний предел конуса (дальние цели)
	float autoGain;        // доля непрерывного довода к центру масс за постоянную времени autoTau
	float autoTau;         // с
	float autoMaxRate;     // рад/с при кратности 1 (на оптике делится на кратность)
};
constexpr float kDeg = PI / 180.f;
constexpr u32 kLevelFullAuto = 5;
constexpr u32 kLevelCount = 6;
constexpr LevelProfile kProfiles[kLevelCount] = {
	// slow   cone м cone max     rot    rot max        trk     trk max      pull    pull max        dur    pcone  pcone max    cone min      auto  tau    auto max
	{0.f,    0.f,   0.f,          0.f,   0.f,           0.f,    0.f,         0.f,    0.f,            0.f,   0.f,   0.f,         0.f,          0.f,  0.f,   0.f},           // off
	// FIX-LIVE2 п.2 (решение Миши 2026-11-16): общее замедление 25% на всех пресетах (свой ползунок),
	// rot/trk 15/20, 35/50, 65/75 %, довод 0 / 20 / 85 %; «Сильная» тянет сама только при зажатом LT.
	{0.25f,  1.2f,  8.f * kDeg,   0.15f, 31.5f * kDeg,  0.20f,  21.f * kDeg, 0.f,    15.75f * kDeg,  0.2f,  0.8f,  5.f * kDeg,  1.5f * kDeg,  0.f,  0.f,   0.f},           // smooth (Слабая)
	{0.25f,  1.2f,  8.f * kDeg,   0.35f, 54.f * kDeg,   0.50f,  36.f * kDeg, 0.20f,  27.f * kDeg,    0.2f,  0.8f,  5.f * kDeg,  1.5f * kDeg,  0.f,  0.f,   0.f},           // medium (Средняя)
	{0.25f,  2.0f,  14.f * kDeg,  0.65f, 110.f * kDeg,  0.75f,  90.f * kDeg, 0.85f,  75.f * kDeg,    0.32f, 1.6f,  10.f * kDeg, 1.5f * kDeg,  0.f,  0.f,   0.f},           // strong (Сильная)
	{0.75f,  2.4f,  16.f * kDeg,  1.0f,  150.f * kDeg,  0.96f,  125.f * kDeg, 1.0f,  105.f * kDeg,   0.40f, 2.0f,  12.f * kDeg, 1.5f * kDeg,  0.f,  0.f,   0.f},           // super
	// full_auto: конус 20°–30° (цель не надо ловить прицелом), камера сама ведёт центр масс (tau 0.1 с,
	// не быстрее 180 °/с — плавно, без рывка), ведение цели и компенсация шага полные, всегда (не только ADS).
	{0.4f,   50.f,  30.f * kDeg,  1.0f,  180.f * kDeg,  1.0f,  180.f * kDeg, 0.f,    0.f,            0.f,   0.f,   0.f,         20.f * kDeg,  1.f,  0.1f,  180.f * kDeg},  // full_auto
};
// FIX-LIVE3 п.6 (решение Миши 2026-11-16) — новая схема для Слабая/Средняя/Сильная (уровни 1..3):
//  * ADS: непрерывный довод к цели внутри конуса захвата; конус по уровню — Слабая почти впритык,
//    Сильная почти весь экран; торможение у цели (малый конус kNear*), гаснет при резком движении стика;
//  * от бедра: только торможение, сила 70% от ADS; без rot/trk/pull.
// Таблица kProfiles для уровней 1..3 остаётся (rot/trk в ADS, slowdown 25%, selftests FIX-LIVE2), ADS-захват и
// довод берутся отсюда. Super/full_auto работают по-прежнему.
struct AdsCapture
{
	float radiusM;    // «радиус» конуса захвата вокруг точки прицеливания, м
	float minRad;     // нижний предел (дальние цели)
	float maxRad;     // верхний предел (близкие цели)
	float pullGain;   // доля довода за постоянную времени kAdsPullTau
	float pullMaxRate;// рад/с при кратности 1 (на оптике делится на кратность)
};
constexpr AdsCapture kAdsCapture[4] = {
	{0.f,   0.f,          0.f,          0.f,   0.f},           // off
	{0.45f, 1.0f * kDeg,  3.f * kDeg,   0.30f, 20.f * kDeg},   // Слабая: почти впритык
	{2.5f,  4.f * kDeg,   12.f * kDeg,  0.50f, 40.f * kDeg},   // Средняя
	{60.f,  28.f * kDeg,  35.f * kDeg,  0.85f, 75.f * kDeg},   // Сильная: почти весь экран прицеливания
};
constexpr float kAdsPullTau = 0.12f;          // с, постоянная времени довода
constexpr float kNearRadiusM = 1.2f;          // конус «у цели» (торможение, rot/trk): как прежний конус уровней 1..3
constexpr float kNearMaxRad = 8.f * kDeg;
constexpr float kHipSlowShare = 0.70f;        // торможение от бедра = 70% от ADS
constexpr float kSharpStickMag = 0.80f;       // |RS| выше — резкий поворот
constexpr float kSharpStickRate = 6.f;        // 1/с: скорость изменения RS выше — рывок стиком
constexpr float kSharpHoldSec = 0.25f;        // с: после рывка торможение и довод ждут

bool IsNewScheme(u32 level, bool autoAim) { return !autoAim && level >= 1 && level <= 3; }
bool IsSharpStick(float lookMag, float lookRate) { return lookMag > kSharpStickMag || lookRate > kSharpStickRate; }
float NearCone(float dist);
float AdsCaptureCone(u32 level, float dist);

constexpr float kAutoStickOverride = 0.5f;   // правый стик сильнее — игрок ведёт сам (смена цели)
constexpr float kAutoResumeDelay = 0.35f;    // с после отпускания стика до возврата автонаведения

constexpr float kMoveStickActive = 0.10f;     // левый стик «движется» (снижен порог для отзывчивого Rotational Tracking)
constexpr float kScopeFadeFrom = 4.f;         // D18: >4x — только Slowdown
constexpr float kScopeFadeTo = 5.f;
constexpr float kScopeFadeRate = 4.f;         // 1/с, ~0.25 с на полное угасание
constexpr float kMaxFrameDt = 0.1f;
constexpr float kEngageRate = 8.f;            // 1/с: включение/выключение за ~0.12 с, без ступенек
constexpr float kTrackFilterTau = 0.06f;      // с: НЧ-фильтр скорости цели (гасит покачивание корпуса)
constexpr float kTorchConeHalfRad = 32.f * PI / 180.f; // полуугол конуса подсветки фонарём
constexpr u32 kLuaCacheSlots = 64;            // размер кэша вердиктов Lua (степень двойки)
constexpr u32 kLuaCacheTtlMs = 250;           // базовый TTL вердикта Lua (мс)
constexpr u32 kDisguiseCacheTtlMs = 250;      // TTL опроса gameplay_disguise.is_actor_disguised
constexpr u32 kMaxLuaEvalsPerFrame = 2;       // защита бюджета кадра от всплеска промахов кэша
constexpr u32 kLockedLosPeriodMs = 25;        // период перепроверки RayQuery по захваченной цели (мс)
constexpr size_t kTimingWindow = 512;         // кольцевое окно QPC-замеров (мкс)

enum EReason : u8
{
	rOk = 0,
	rNotGamepad,
	rNotEligible,
	rNoWeapon,
	rNoCandidates,
	rOutOfCone,
	rOccluded,
	rOccludedBush,
	rDarkness,
	rFog,
	rDisguise,
	rWounded,
	rCompanion,
	rEnemyEval,
	rTargetLost,
	rBadFrame,
};

LPCSTR ReasonName(u8 r)
{
	switch (r)
	{
	case rOk: return "ok";
	case rNotGamepad: return "mouse_or_kbm_source";
	case rNotEligible: return "not_eligible(camera/freelook/holder/hud_adj)";
	case rNoWeapon: return "no_weapon";
	case rNoCandidates: return "no_enemy_candidates";
	case rOutOfCone: return "target_out_of_cone";
	case rOccluded: return "occluded_static";
	case rOccludedBush: return "occluded_bush";
	case rDarkness: return "darkness";
	case rFog: return "fog";
	case rDisguise: return "disguise";
	case rWounded: return "wounded";
	case rCompanion: return "companion";
	case rEnemyEval: return "enemy_eval";
	case rTargetLost: return "target_lost";
	case rBadFrame: return "bad_frame";
	}
	return "?";
}

struct LuaVerdictEntry
{
	u16 id = u16(-1);
	u64 instanceGen = 0;
	const CObject* obj = nullptr;
	u32 expireMs = 0;
	u8 reason = rEnemyEval; // fail-closed по умолчанию
};

struct FilterCounters
{
	u32 companion = 0;
	u32 wounded = 0;
	u32 disguise = 0;
	u32 enemyEval = 0;
	u32 fog = 0;
	u32 darkness = 0;
	u32 occludedWall = 0;
	u32 occludedBush = 0;
	u32 luaCalls = 0;
	u32 luaCacheHits = 0;
	u32 luaErrors = 0;
};

struct State
{
	u16 targetId = u16(-1);
	const CObject* targetObj = nullptr; // только для сравнения: ID в движке переиспользуются
	u64 targetInstanceGen = 0;
	const IRenderVisual* targetVisual = nullptr;
	u32 sessionGen = 0;
	u16 targetBone = u16(-1);
	bool boneResolved = false;
	const void* level = nullptr;        // смена уровня — полный сброс
	float engage = 0.f;                 // сглаженная во времени степень включения (без ступенек)
	float trkYawRate = 0.f, trkPitchRate = 0.f; // НЧ-фильтр скорости цели (рад/с)
	u32 lastScanMs = 0;
	u32 lastLockedLosMs = 0;
	u32 lastFrame = 0;
	u32 luaFrameStamp = 0;
	u32 luaEvalsThisFrame = 0;
	bool hasHistory = false;
	Fvector prevEye = {0, 0, 0};
	Fvector prevAim = {0, 0, 0};
	bool prevAds = false;
	float pullLeft = 0.f;
	float scopeK = 1.f; // 1 — все части; 0 — только Slowdown
	u8 reason = rNoCandidates;
	u16 lastRejectId = u16(-1);
	// последние диагностические метрики (для gp_aa_debug)
	float lastDist = 0.f, lastErr = 0.f, lastCone = 0.f, lastMag = 1.f;
	float lastVis = 1.f, lastLum = 0.f, lastFogDensity = 0.f, lastFogMaxRange = kMaxRange;
	float curRange = kHipRange; // FIX-LIVE2 п.3: 30 м от бедра, 160 м в ADS
	float lastSlowMul = 1.f, lastRotYaw = 0.f, lastRotPitch = 0.f, lastTrkYaw = 0.f, lastTrkPitch = 0.f;
	float lastPullYaw = 0.f, lastPullPitch = 0.f;
	u32 lastLogMs = 0;
	u32 callCount = 0;
	// E4.1b: номер смены gp_aa_level, который модуль уже применил (u32(-1) — ещё не вызывался),
	// счётчик применённых смен, кадр прошлого вызова Process (для разрыва кадров: off, меню, пауза).
	u32 lastEpochSeen = u32(-1);
	u32 levelResets = 0;
	u32 lastProcessFrame = 0;
	float autoHold = 0.f;  // E4.1b: оставшееся время паузы автонаведения после ручного ввода стиком
	bool curAds = false;   // FIX-LIVE3 п.6: Scan выбирает конус захвата по режиму (ADS/от бедра)
	float sharpHold = 0.f; // FIX-LIVE3 п.6: пауза торможения/довода после резкого движения RS
	float prevLookX = 0.f, prevLookY = 0.f;
	float lastAutoYaw = 0.f, lastAutoPitch = 0.f;

	// Кэш состояния маскировки актора (gameplay_disguise.is_actor_disguised)
	u32 disguiseExpireMs = 0;
	bool actorDisguised = false;
	bool disguiseQueryOk = true;

	// Кэш вердиктов Lua по целям (on_enemy_eval / xr_combat_ignore / xr_wounded / xr_bribe)
	LuaVerdictEntry luaCache[kLuaCacheSlots] = {};
	FilterCounters counts = {};
};
State S;

// Кольцевой буфер времени исполнения Process() в микросекундах (не сбрасывается при смене уровня).
struct TimingStats
{
	float samplesUs[kTimingWindow] = {};
	size_t writeIdx = 0;
	size_t count = 0;
	float lastUs = 0.f;
	float maxEverUs = 0.f;

	void Record(float us)
	{
		if (!_valid(us) || us < 0.f)
			return;
		lastUs = us;
		if (us > maxEverUs)
			maxEverUs = us;
		samplesUs[writeIdx] = us;
		writeIdx = (writeIdx + 1) % kTimingWindow;
		if (count < kTimingWindow)
			++count;
	}

	void Compute(float& outAvg, float& outP50, float& outP95, float& outP99, float& outMax) const
	{
		if (count == 0)
		{
			outAvg = outP50 = outP95 = outP99 = outMax = 0.f;
			return;
		}
		float sorted[kTimingWindow];
		double sum = 0.0;
		for (size_t i = 0; i < count; ++i)
		{
			sorted[i] = samplesUs[i];
			sum += samplesUs[i];
		}
		std::sort(sorted, sorted + count);
		outAvg = static_cast<float>(sum / double(count));
		outP50 = sorted[(count - 1) * 50 / 100];
		outP95 = sorted[(count - 1) * 95 / 100];
		outP99 = sorted[(count - 1) * 99 / 100];
		outMax = sorted[count - 1];
	}
};
TimingStats g_timing;

struct ScopedQpcTimer
{
	u64 startTicks;
	ScopedQpcTimer() : startTicks(CPU::QPC()) {}
	~ScopedQpcTimer()
	{
		const u64 endTicks = CPU::QPC();
		if (CPU::qpc_freq > 0 && endTicks >= startTicks)
		{
			const double us = double(endTicks - startTicks) * 1000000.0 / double(CPU::qpc_freq);
			g_timing.Record(static_cast<float>(us));
		}
	}
};

xr_vector<CObject*> s_nearest;
collide::rq_results s_rayResults;

void BumpReasonCounter(u8 reason)
{
	switch (reason)
	{
	case rCompanion: ++S.counts.companion; break;
	case rWounded: ++S.counts.wounded; break;
	case rDisguise: ++S.counts.disguise; break;
	case rEnemyEval: ++S.counts.enemyEval; break;
	case rFog: ++S.counts.fog; break;
	case rDarkness: ++S.counts.darkness; break;
	case rOccluded: ++S.counts.occludedWall; break;
	case rOccludedBush: ++S.counts.occludedBush; break;
	default: break;
	}
}

const LevelProfile& Profile(u32 level)
{
	return kProfiles[level < kLevelCount ? level : 0];
}

bool IsAutoAim(const LevelProfile& P)
{
	return P.autoGain > 0.f;
}

// E4.1b: правый стик за порогом — игрок перехватывает наведение (full_auto не тянет камеру).
bool AutoAimStickOverride(float lookX, float lookY)
{
	return lookX * lookX + lookY * lookY > kAutoStickOverride * kAutoStickOverride;
}

// Сводная «сила» уровня только для логов и шкалы (E4.1: 0/0.35/0.6/0.8/1.0). Расчёт идёт по Profile().
float LevelStrength(u32 level)
{
	switch (level)
	{
	case 1: return 0.35f;
	case 2: return 0.6f;
	case 3: return 0.8f;
	case 4: return 1.f;
	case 5: return 1.f; // full_auto: шкала силы не применяется, работает автонаведение
	}
	return 0.f;
}

const char* LevelName(u32 l)
{
	switch (l)
	{
	case 0: return "off";
	case 1: return "smooth";
	case 2: return "medium";
	case 3: return "strong";
	case 4: return "super";
	case 5: return "full_auto";
	}
	return "?";
}

float WrapPi(float a)
{
	while (a > PI) a -= PI_MUL_2;
	while (a < -PI) a += PI_MUL_2;
	return a;
}

// Угловая ошибка точки p относительно камеры: x>0 — правее, y>0 — выше.
void AngleError(const Fvector& eye, const Fvector& dir, const Fvector& up, const Fvector& right, const Fvector& p,
                float& ex, float& ey)
{
	Fvector d;
	d.sub(p, eye);
	const float fwd = d.dotproduct(dir);
	ex = atan2f(d.dotproduct(right), fwd);
	ey = atan2f(d.dotproduct(up), _sqrt(fwd * fwd + d.dotproduct(right) * d.dotproduct(right)));
}

float ConeFor(float dist, float radiusM, float minRad, float maxRad)
{
	const float c = atan2f(radiusM, dist > 0.1f ? dist : 0.1f);
	return c < minRad ? minRad : (c > maxRad ? maxRad : c);
}

// Плавное снижение: 1 в центре, 0 на краю конуса (квадратичный спад).
// FIX-LIVE3 п.6: конусы новой схемы (см. kAdsCapture)
float NearCone(float dist)
{
	return ConeFor(dist, kNearRadiusM, kConeMinRad, kNearMaxRad);
}

float AdsCaptureCone(u32 level, float dist)
{
	const AdsCapture& C = kAdsCapture[level <= 3 ? level : 0];
	return ConeFor(dist, C.radiusM, C.minRad, C.maxRad);
}

float ConeFalloff(float err, float cone)
{
	if (cone <= 0.f || err >= cone)
		return 0.f;
	const float t = err / cone;
	return 1.f - t * t;
}

float ScopeTarget(float mag, bool sdsActive, bool disableOnScope)
{
	if (!disableOnScope)
		return 1.f;
	if (sdsActive)
		return 0.f;
	if (mag <= kScopeFadeFrom) return 1.f;
	if (mag >= kScopeFadeTo) return 0.f;
	return (kScopeFadeTo - mag) / (kScopeFadeTo - kScopeFadeFrom);
}

void CapRate(float& yaw, float& pitch, float maxRate, float dt)
{
	const float lim = maxRate * dt;
	const float m = _sqrt(yaw * yaw + pitch * pitch);
	if (m > lim && m > 0.f)
	{
		yaw *= lim / m;
		pitch *= lim / m;
	}
}

float Magnification(const CWeapon* w, const CCameraBase* cam)
{
	float fov = cam ? cam->f_fov : g_fov;
	if (w && Device.m_SecondViewport.IsSVPActive() && w->IsZoomed())
		fov = w->GetSecondVPFov();
	if (fov < 0.5f || fov > 179.f)
		return 1.f;
	const float t0 = tanf(deg2rad(g_fov) * 0.5f);
	const float t1 = tanf(deg2rad(fov) * 0.5f);
	return (t1 > 0.f) ? t0 / t1 : 1.f;
}

// E4.2: отсечение бинокля и ножа (наследуются от CWeapon, но не стрелковое оружие).
bool IsFirearmWeapon(CWeapon* w)
{
	if (!w)
		return false;
	if (smart_cast<CWeaponBinoculars*>(w) != nullptr)
		return false;
	if (smart_cast<CWeaponKnife*>(w) != nullptr)
		return false;
	return true;
}

// Чистая функция накопления прозрачности препятствий вдоль луча (для луча и для selftest).
// Если на пути встретилась глухая преграда (<=0.001) — rOccluded; если кусты снизили прозрачность
// до <=threshold — rOccludedBush; при некорректных данных — fail-closed (rOccluded).
u8 EvaluateVisTransparency(const float* factors, size_t count, float threshold, float& outVis)
{
	if (!factors && count > 0)
	{
		outVis = 0.f;
		return rOccluded;
	}
	if (!_valid(threshold) || threshold < 0.f || threshold >= 1.f)
		threshold = 0.25f;

	float vis = 1.f;
	bool sawTranslucent = false;
	for (size_t i = 0; i < count; ++i)
	{
		const float f = factors[i];
		if (!_valid(f) || f < 0.f)
		{
			outVis = 0.f;
			return rOccluded;
		}
		const float clamped = clampr(f, 0.f, 1.f);
		if (clamped <= 0.001f)
		{
			outVis = 0.f;
			return rOccluded;
		}
		if (clamped < 0.999f)
			sawTranslucent = true;
		vis *= clamped;
		if (vis <= threshold)
		{
			outVis = vis;
			return sawTranslucent ? rOccludedBush : rOccluded;
		}
	}
	outVis = vis;
	return rOk;
}

// Чистая функция ослабления/отсечения дальности AA в тумане (D19 п.5).
// При включённом тепловизоре (thermalActive) туман не блокирует цель.
bool EvaluateFogRange(float dist, float baseMaxRange, float fogDensity, float fogDistance, bool thermalActive,
                      float densityCutoff, float& outEffectiveRange)
{
	if (!_valid(dist) || !_valid(baseMaxRange) || dist < 0.f || baseMaxRange <= 0.f)
	{
		outEffectiveRange = 0.f;
		return false;
	}
	if (thermalActive)
	{
		outEffectiveRange = baseMaxRange;
		return dist <= baseMaxRange;
	}
	if (!_valid(fogDensity) || !_valid(fogDistance) || fogDensity < 0.f || fogDistance <= 0.f)
	{
		outEffectiveRange = 0.f;
		return false; // fail-closed при битых параметрах погоды
	}

	const float dens = clampr(fogDensity, 0.f, 1.f);
	// На основе формулы CEnvDescriptorMixer::lerp: fog_near = (1 - fog_density) * 0.85 * fog_distance.
	float fogCap = fogDistance * (1.f - 0.65f * dens);
	float range = baseMaxRange;
	if (dens >= densityCutoff)
	{
		const float heavyAtten = 1.f - clampr((dens - 0.2f) * 0.85f, 0.f, 0.8f);
		range = _min(range * heavyAtten, fogCap);
	}
	else if (dens > 0.25f)
	{
		const float mildAtten = 1.f - (dens - 0.25f) * 0.6f;
		range = _min(range * mildAtten, fogCap);
	}
	else
	{
		range = _min(range, fogDistance * 0.9f);
	}
	if (range < 4.f)
		range = 4.f;
	outEffectiveRange = range;
	return dist <= range;
}

// Чистая функция проверки освещённости цели (D19 п.4).
// Цель видима, если: (а) активен ПНВ/тепловизор, ИЛИ (б) активен фонарь игрока и цель в его конусе/дальности,
// ИЛИ (в) собственная освещённость цели ROS()->get_luminocity() >= minLum.
bool EvaluateIllumination(float rosLum, float dist, float coneErrRad, bool playerLightActive, bool nvgOrThermalActive,
                          float minLum, float torchMaxRange)
{
	if (nvgOrThermalActive)
		return true;
	if (!_valid(dist) || !_valid(coneErrRad) || dist < 0.f)
		return false;
	if (playerLightActive && _valid(torchMaxRange) && dist <= torchMaxRange && coneErrRad <= kTorchConeHalfRad)
		return true;
	if (!_valid(rosLum) || !_valid(minLum) || rosLum < 0.f)
		return false; // fail-closed при отсутствии/ошибке ROS
	return rosLum >= minLum;
}

CHARACTER_GOODWILL GetAttitudeNeutralThreshold()
{
	static bool s_loaded = false;
	static CHARACTER_GOODWILL s_threshold = -400;
	if (!s_loaded && pSettings && pSettings->section_exist(GAME_RELATIONS_SECT) &&
	    pSettings->line_exist(GAME_RELATIONS_SECT, "attitude_neutal_threshold"))
	{
		s_threshold = pSettings->r_s16(GAME_RELATIONS_SECT, "attitude_neutal_threshold");
		s_loaded = true;
	}
	return s_threshold;
}

bool HasActorDisguisePrefix(const CInventoryOwner* actorIo)
{
	if (!actorIo)
		return false;
	LPCSTR comm = actorIo->CharacterInfo().Community().id().c_str();
	return comm && strncmp(comm, "actor_", 6) == 0;
}

// Кэшированный опрос gameplay_disguise.is_actor_disguised() (не чаще 4 раз в секунду).
bool IsActorDisguisedCached(CActor* actor, bool& outQueryOk)
{
	const u32 now = Device.dwTimeGlobal;
	if (now < S.disguiseExpireMs && S.disguiseExpireMs - now <= kDisguiseCacheTtlMs * 2)
	{
		outQueryOk = S.disguiseQueryOk;
		return S.actorDisguised;
	}

	S.disguiseExpireMs = now + kDisguiseCacheTtlMs;
	S.actorDisguised = false;
	S.disguiseQueryOk = true;

	if (!actor || !ai().get_alife() || !ai().script_engine().lua())
	{
		outQueryOk = false;
		S.disguiseQueryOk = false;
		return false;
	}

	try
	{
		::luabind::functor<bool> fnDisguised;
		if (ai().script_engine().functor("gameplay_disguise.is_actor_disguised", fnDisguised))
		{
			S.actorDisguised = fnDisguised();
			++S.counts.luaCalls;
		}
	}
	catch (...)
	{
		S.actorDisguised = false;
		S.disguiseQueryOk = false;
		++S.counts.luaErrors;
	}

	outQueryOk = S.disguiseQueryOk;
	return S.actorDisguised;
}

// Быстрые C++-фильтры D19 (без вызова Lua на цель):
// 1) Живой CCustomMonster (сталкер/мутант, не ворона и не труп)
// 2) Не компаньон (HasInfo("npcx_is_companion"))
// 3) Не ранен/сдался (CAI_Stalker::wounded / critically_wounded)
// 4) Не нейтрален по маскировке (actor_<comm>, gameplay_disguise)
// 5) FIX-LIVE2 п.2б: враждебность не требуется (нейтралы и друзья вне отряда — тоже цели)
u8 CheckEntityFastD19(CObject* o, CActor* actor, bool& outIsAliveCreature)
{
	outIsAliveCreature = false;
	if (!o || !actor || o == actor || o->getDestroy())
		return rNoCandidates;

	CEntityAlive* ea = smart_cast<CEntityAlive*>(o);
	if (!ea || !ea->g_Alive() || ea->AlreadyDie())
		return rNoCandidates;

	CCustomMonster* cm = smart_cast<CCustomMonster*>(ea);
	if (!cm)
		return rNoCandidates; // отсекает ворон и не-ИИ объекты

	// FIX-LIVE4 (Миша 2026-11-16): полностью невидимый кровосос — не цель; в промежуточном
	// состоянии (partial_visibility, мерцает) аим работает.
	if (const CAI_Bloodsucker* bs = smart_cast<const CAI_Bloodsucker*>(cm))
	{
		if (bs->get_visibility_state() == CAI_Bloodsucker::no_visibility)
			return rNoCandidates;
	}

	outIsAliveCreature = true;

	const CInventoryOwner* targetIo = smart_cast<const CInventoryOwner*>(o);
	const CInventoryOwner* actorIo = smart_cast<const CInventoryOwner*>(actor);

	// 1) Компаньон (axr_companions ставит инфопоршень npcx_is_companion)
	if (targetIo && targetIo->HasInfo("npcx_is_companion"))
		return rCompanion;

	// 2) Раненый / сдавшийся сталкер (C++ флаги CAI_Stalker)
	CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(o);
	if (stalker && (stalker->wounded() || stalker->critically_wounded()))
		return rWounded;

	// 3) Маскировка G.A.M.M.A. (gameplay_disguise, actor_<comm>):
	// пока маскировка не раскрыта и NPC не ведёт активный бой с актором, цели, чья группировка
	// нейтральна/дружественна к текущей маскировке, отбрасываются с причиной rDisguise (D19).
	if (targetIo && actorIo && HasActorDisguisePrefix(actorIo))
	{
		bool disguiseOk = true;
		const bool disguised = IsActorDisguisedCached(actor, disguiseOk);
		const bool fightingActor = (cm->memory().enemy().selected() == actor);
		const CHARACTER_GOODWILL commRel =
			RELATION_REGISTRY().GetCommunityRelation(targetIo->Community(), actorIo->Community());
		const bool commNeutralOrFriend = (commRel >= GetAttitudeNeutralThreshold());

		if (!disguiseOk)
		{
			// Fail-closed (D19): при сбое чтения статуса маскировки не притягиваем к нейтральной фракции
			if (commNeutralOrFriend && !fightingActor)
				return rDisguise;
		}
		else if (disguised && !fightingActor)
		{
			if (commNeutralOrFriend || !ea->is_relation_enemy(actor))
				return rDisguise;
		}
	}

	// 4) FIX-LIVE2 п.2б (решение Миши 2026-11-16): нейтральные и дружественные сталкеры вне отряда
	// получают ту же помощь, что и враги; компаньоны, раненые и маскировка отсечены выше.

	return rOk;
}

// Прямой вызов Lua-проверок D19 для одной цели (вызывается только при промахе кэша для цели в конусе):
// - xr_wounded.is_wounded(npc)
// - SendScriptCallback("on_enemy_eval", npc, actor, flags)
// - xr_combat_ignore.ignore_enemy_by_overrides(npc, actor, false)
// - xr_combat_ignore.npc_in_safe_zone(npc)
// - xr_bribe.at_peace(npc_comm, actor_comm, dist_sqr)
// Любое исключение или невалидное состояние трактуется как отказ (fail-closed -> rEnemyEval).
u8 QueryTargetLuaD19(CObject* o, CActor* actor)
{
	if (!o || !actor || !ai().get_alife())
		return rEnemyEval;

	lua_State* L = ai().script_engine().lua();
	if (!L)
		return rEnemyEval;

	CGameObject* goTarget = smart_cast<CGameObject*>(o);
	CGameObject* goActor = smart_cast<CGameObject*>(actor);
	if (!goTarget || !goActor)
		return rEnemyEval;

	CScriptGameObject* luaTarget = goTarget->lua_game_object();
	CScriptGameObject* luaActor = goActor->lua_game_object();
	if (!luaTarget || !luaActor)
		return rEnemyEval;

	++S.counts.luaCalls;

	try
	{
		// 1) Скриптовое состояние ранения/сдачи (db.storage[id].wounded.wound_manager)
		::luabind::functor<bool> fnWounded;
		if (ai().script_engine().functor("xr_wounded.is_wounded", fnWounded))
		{
			if (fnWounded(luaTarget))
				return rWounded;
		}

		// 2) Единый колбэк G.A.M.M.A. on_enemy_eval (axr_companions, xr_wounded, gamemode_agony и др.)
		::luabind::functor<void> fnSendCb;
		if (!ai().script_engine().functor("_G.SendScriptCallback", fnSendCb))
			return rEnemyEval; // fail-closed: без SendScriptCallback нельзя гарантировать честность

		::luabind::object flags = ::luabind::newtable(L);
		flags["override"] = false;
		flags["result"] = true;
		fnSendCb("on_enemy_eval", luaTarget, luaActor, flags);
		if (flags["override"] && ::luabind::object_cast<bool>(flags["override"]))
		{
			const bool allowed = flags["result"] && ::luabind::object_cast<bool>(flags["result"]);
			if (!allowed)
			{
				const CInventoryOwner* targetIo = smart_cast<const CInventoryOwner*>(o);
				if (targetIo && targetIo->HasInfo("npcx_is_companion"))
					return rCompanion;
				return rEnemyEval;
			}
		}

		// 3) Оверрайды логики combat_ignore_cond / enemy_ignore_cond / no_combat_job и безопасные зоны
		const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(o);
		if (stalker)
		{
			::luabind::functor<bool> fnIgnoreOverrides;
			if (ai().script_engine().functor("xr_combat_ignore.ignore_enemy_by_overrides", fnIgnoreOverrides))
			{
				if (fnIgnoreOverrides(luaTarget, luaActor, false))
					return rEnemyEval;
			}

			::luabind::functor<bool> fnSafeZone;
			if (ai().script_engine().functor("xr_combat_ignore.npc_in_safe_zone", fnSafeZone))
			{
				if (fnSafeZone(luaTarget))
					return rEnemyEval;
			}

			// 4) Временное перемирие / взятка группировке (xr_bribe.at_peace)
			const CInventoryOwner* targetIo = smart_cast<const CInventoryOwner*>(o);
			const CInventoryOwner* actorIo = smart_cast<const CInventoryOwner*>(actor);
			if (targetIo && actorIo)
			{
				::luabind::functor<bool> fnBribe;
				if (ai().script_engine().functor("xr_bribe.at_peace", fnBribe))
				{
					const float distSqr = o->Position().distance_to_sqr(actor->Position());
					LPCSTR targetComm = targetIo->CharacterInfo().Community().id().c_str();
					LPCSTR actorComm = actorIo->CharacterInfo().Community().id().c_str();
					if (targetComm && actorComm && fnBribe(targetComm, actorComm, distSqr))
						return rEnemyEval;
				}
			}
		}

		return rOk;
	}
	catch (...)
	{
		++S.counts.luaErrors;
		return rEnemyEval; // fail-closed (D19)
	}
}

// Кэшированная обёртка вокруг QueryTargetLuaD19 с пообъектным TTL и джиттером.
u8 CheckLuaEnemyEvalCached(CObject* o, CActor* actor)
{
	if (!o || !actor)
		return rEnemyEval;

	const u16 id = o->ID();
	const u64 instGen = CPauseTokenManager::Instance().GetObjectInstanceGen(id);
	const u32 now = Device.dwTimeGlobal;
	LuaVerdictEntry& slot = S.luaCache[id & (kLuaCacheSlots - 1)];

	if (slot.id == id && slot.instanceGen == instGen && slot.obj == o && now < slot.expireMs &&
	    (slot.expireMs - now) <= (kLuaCacheTtlMs + 64))
	{
		++S.counts.luaCacheHits;
		return slot.reason;
	}

	if (S.luaFrameStamp != Device.dwFrame)
	{
		S.luaFrameStamp = Device.dwFrame;
		S.luaEvalsThisFrame = 0;
	}

	// Защита бюджета кадра: не более kMaxLuaEvalsPerFrame вызовов Lua за один кадр.
	if (S.luaEvalsThisFrame >= kMaxLuaEvalsPerFrame)
	{
		// Если есть недавний вердикт для того же указателя — используем его, иначе fail-closed (rEnemyEval).
		if (slot.id == id && slot.instanceGen == instGen && slot.obj == o &&
		    now < slot.expireMs && (slot.expireMs - now) <= (kLuaCacheTtlMs + 64))
			return slot.reason;
		return rEnemyEval;
	}

	++S.luaEvalsThisFrame;
	const u8 reason = QueryTargetLuaD19(o, actor);
	const u32 jitter = (u32(id) % 53u);
	slot.id = id;
	slot.instanceGen = instGen;
	slot.obj = o;
	slot.expireMs = now + kLuaCacheTtlMs + jitter;
	slot.reason = reason;
	return reason;
}

bool IsPlayerLightActive(CActor* actor)
{
	if (!actor)
		return false;
	if (CTorch* torch = smart_cast<CTorch*>(actor->inventory().ItemFromSlot(TORCH_SLOT)))
	{
		if (torch->torch_active())
			return true;
	}
	if (CFlashlight* flashlight = smart_cast<CFlashlight*>(actor->inventory().ItemFromSlot(DETECTOR_SLOT)))
	{
		if (flashlight->torch_active())
			return true;
	}
	return false;
}

bool IsPlayerNvgOrThermalActive(CActor* actor)
{
	if (ps_r2_nightvision > 0 || ps_r2_heatvision > 0)
		return true;
	if (!actor)
		return false;
	if (actor->Cameras().GetPPEffector(static_cast<EEffectorPPType>(effNightvision)) != nullptr)
		return true;
	return false;
}

bool CheckFogFilterD19(float dist)
{
	if (!g_pGamePersistent)
		return false;
	CEnvDescriptorMixer* env = g_pGamePersistent->Environment().CurrentEnv;
	if (!env)
		return false;

	S.lastFogDensity = env->fog_density;
	float effRange = S.curRange;
	const bool ok = EvaluateFogRange(dist, S.curRange, env->fog_density, env->fog_distance,
	                                 ps_r2_heatvision > 0, g_fogDensityCutoff, effRange);
	S.lastFogMaxRange = effRange;
	return ok;
}

bool CheckIlluminationFilterD19(CObject* o, CActor* actor, float dist, float coneErrRad)
{
	if (!o || !actor)
		return false;
	IRender_ObjectSpecific* ros = o->ROS();
	const float lum = ros ? ros->get_luminocity() : -1.f;
	S.lastLum = _valid(lum) ? lum : -1.f;
	const bool lightOn = IsPlayerLightActive(actor);
	const bool nvgOrThermal = IsPlayerNvgOrThermalActive(actor);
	return EvaluateIllumination(lum, dist, coneErrRad, lightOn, nvgOrThermal, g_minLuminosity, g_torchRange);
}

// Контекст трассировки луча видимости сквозь статику и динамические объекты (кусты/решётки/двери).
struct VisRayCtx
{
	const CObject* actor = nullptr;
	const CObject* target = nullptr;
	float threshold = 0.25f;
	float vis = 1.f;
	bool hitOpaque = false;
	bool hitTranslucent = false;
};

BOOL VisRayQueryCallback(collide::rq_result& result, LPVOID params)
{
	VisRayCtx* ctx = static_cast<VisRayCtx*>(params);
	if (!ctx)
		return FALSE;

	float mtlTransp = 0.f; // fail-closed при неизвестном материале
	if (result.O != nullptr)
	{
		const CObject* root = result.O->H_Root();
		if (result.O == ctx->actor || result.O == ctx->target || root == ctx->actor || root == ctx->target)
			return TRUE; // игнорируем самого игрока, саму цель и их экипировку

		// Живые существа на линии огня не считаем «кустом», сквозь них луч проходит к проверке геометрии
		if (smart_cast<CEntityAlive*>(result.O) != nullptr)
			return TRUE;

		if (IKinematics* V = smart_cast<IKinematics*>(result.O->Visual()))
		{
			const u16 boneCount = V->LL_BoneCount();
			if (result.element >= 0 && result.element < int(boneCount))
			{
				const CBoneData& B = V->LL_GetData(static_cast<u16>(result.element));
				if (SGameMtl* mtl = GMLib.GetMaterialByIdx(B.game_mtl_idx))
					mtlTransp = mtl->fVisTransparencyFactor;
			}
		}
	}
	else
	{
		const CDB::MODEL* staticModel = Level().ObjectSpace.GetStaticModel();
		CDB::TRI* tris = Level().ObjectSpace.GetStaticTris();
		if (tris && staticModel && result.element >= 0 && result.element < staticModel->get_tris_count())
		{
			const CDB::TRI& T = tris[result.element];
			if (SGameMtl* mtl = GMLib.GetMaterialByIdx(T.material))
				mtlTransp = mtl->fVisTransparencyFactor;
		}
	}

	if (!_valid(mtlTransp) || mtlTransp <= 0.001f)
	{
		ctx->vis = 0.f;
		ctx->hitOpaque = true;
		return FALSE; // глухая стена/преграда — немедленный выход
	}

	const float clamped = clampr(mtlTransp, 0.f, 1.f);
	if (clamped < 0.999f)
		ctx->hitTranslucent = true;
	ctx->vis *= clamped;

	if (ctx->vis <= ctx->threshold)
		return FALSE; // накопленная плотность кустов превысила порог
	return TRUE;
}

// Трассировка линии видимости от камеры к точке наведения с учётом fVisTransparencyFactor (D19 п.3).
u8 CheckLineOfSightD19(const Fvector& eye, const Fvector& p, CActor* actor, const CObject* target, float& outVis)
{
	if (!_valid(eye) || !_valid(p) || !actor || !target)
	{
		outVis = 0.f;
		return rOccluded;
	}

	Fvector d;
	d.sub(p, eye);
	const float dist = d.magnitude();
	if (dist < 0.5f)
	{
		outVis = 1.f;
		return rOk;
	}
	d.mul(1.f / dist);

	VisRayCtx ctx;
	ctx.actor = actor;
	ctx.target = target;
	ctx.threshold = clampr(g_visThreshold, 0.01f, 0.95f);
	ctx.vis = 1.f;

	s_rayResults.r_clear();
	collide::ray_defs rd(eye, d, dist - 0.25f, CDB::OPT_CULL, collide::rqtBoth);
	Level().ObjectSpace.RayQuery(s_rayResults, rd, VisRayQueryCallback, &ctx, nullptr, actor);

	outVis = ctx.vis;
	if (ctx.hitOpaque || ctx.vis <= 0.001f)
		return rOccluded;
	if (ctx.vis <= ctx.threshold)
		return ctx.hitTranslucent ? rOccludedBush : rOccluded;
	return rOk;
}

// FIX-LIVE2 п.5: верх груди — bip01_spine2, иначе bip01_spine1, иначе центр габарита. Никогда не голова.
u16 ChestBone(IKinematics* K)
{
	u16 id = K->LL_BoneID("bip01_spine2");
	if (id == BI_NONE || id >= K->LL_BoneCount())
		id = K->LL_BoneID("bip01_spine1");
	return id;
}

// Центр масс для произвольного кандидата (при сканировании) без перезаписи кэша кости текущей цели.
bool CandidateAimPoint(CObject* o, Fvector& p)
{
	if (!o)
		return false;
	if (o == S.targetObj && S.boneResolved && S.targetVisual == o->Visual())
	{
		if (IKinematics* K = o->Visual() ? smart_cast<IKinematics*>(o->Visual()) : nullptr)
		{
			if (S.targetBone != BI_NONE && S.targetBone < K->LL_BoneCount())
			{
				Fmatrix m;
				m.mul_43(o->XFORM(), K->LL_GetTransform(S.targetBone));
				p = m.c;
				return _valid(p);
			}
		}
	}
	else if (IKinematics* K = o->Visual() ? smart_cast<IKinematics*>(o->Visual()) : nullptr)
	{
		const u16 boneId = ChestBone(K);
		if (boneId != BI_NONE && boneId < K->LL_BoneCount())
		{
			Fmatrix m;
			m.mul_43(o->XFORM(), K->LL_GetTransform(boneId));
			p = m.c;
			return _valid(p);
		}
	}
	o->Center(p);
	return _valid(p);
}

// Точка захваченной цели: верх груди (ChestBone), иначе центр габарита. Никогда не голова (D18).
bool AimPoint(CObject* o, Fvector& p)
{
	IKinematics* K = o->Visual() ? smart_cast<IKinematics*>(o->Visual()) : nullptr;
	if (K)
	{
		if (!S.boneResolved || S.targetVisual != o->Visual())
		{
			S.targetBone = ChestBone(K);
			S.targetVisual = o->Visual();
			S.boneResolved = true;
		}
		if (S.targetBone != BI_NONE && S.targetBone < K->LL_BoneCount())
		{
			Fmatrix m;
			m.mul_43(o->XFORM(), K->LL_GetTransform(S.targetBone));
			p = m.c;
			return _valid(p);
		}
	}
	o->Center(p);
	return _valid(p);
}

void ClearContrib()
{
	S.lastSlowMul = 1.f;
	S.lastRotYaw = S.lastRotPitch = S.lastTrkYaw = S.lastTrkPitch = S.lastPullYaw = S.lastPullPitch = 0.f;
	S.lastAutoYaw = S.lastAutoPitch = 0.f;
}

void DropTarget(u8 reason, u16 rejectedId = u16(-1))
{
	S.targetId = u16(-1);
	S.targetInstanceGen = 0;
	S.targetObj = nullptr;
	S.targetVisual = nullptr;
	S.boneResolved = false;
	S.hasHistory = false;
	S.trkYawRate = S.trkPitchRate = 0.f;
	S.reason = reason;
	S.lastRejectId = rejectedId;
}

void StepToward(float& v, float target, float step)
{
	if (v < target) v = (v + step > target) ? target : v + step;
	else if (v > target) v = (v - step < target) ? target : v - step;
}

// E4.1b: при gp_aa_level=off модуль не вызывается (D05), поэтому состояние от прошлого включения
// (захваченная цель, engage, окно довода, prevAds, отметки скана) остаётся «замороженным».
// При смене уровня (epoch) или разрыве кадров переходное состояние сбрасывается, следующий скан
// делается в этом же кадре. Счётчики D19 и QPC-окно не трогаются. Возвращает true при смене уровня.
bool SyncLevelAndContinuity(State& st, u32 epoch, u32 frame)
{
	const bool epochChanged = st.lastEpochSeen != epoch;
	// Повторный вызов в том же кадре — не разрыв (на случай двойного UpdateCL).
	const bool frameGap = st.lastProcessFrame != 0 && frame != st.lastProcessFrame && st.lastProcessFrame + 1 != frame;
	st.lastProcessFrame = frame;
	if (!epochChanged && !frameGap)
		return false;

	st.targetId = u16(-1);
	st.targetObj = nullptr;
	st.boneResolved = false;
	st.hasHistory = false;
	st.trkYawRate = st.trkPitchRate = 0.f;
	st.engage = 0.f;
	st.pullLeft = 0.f;
	st.prevAds = false;
	st.lastScanMs = 0;
	st.lastLockedLosMs = 0;
	st.autoHold = 0.f;
	if (!epochChanged)
		return false;
	st.lastEpochSeen = epoch;
	++st.levelResets;
	return true;
}

void Scan(const Fvector& eye, const Fvector& dir, const Fvector& up, const Fvector& right, CActor* actor)
{
	s_nearest.clear();
	Level().ObjectSpace.GetNearest(s_nearest, eye, kMaxRange, actor);

	u16 bestId = u16(-1);
	const CObject* bestObj = nullptr;
	float bestScore = flt_max;
	float bestVis = 1.f;
	const u16 curId = S.targetId;

	u32 potentialEnemiesOutOfCone = 0;
	u8 bestConeRejectReason = rNoCandidates;
	u16 bestConeRejectId = u16(-1);
	float bestConeRejectScore = flt_max;

	auto recordConeReject = [&](u8 reason, u16 id, float normScore) {
		BumpReasonCounter(reason);
		if (normScore < bestConeRejectScore)
		{
			bestConeRejectScore = normScore;
			bestConeRejectReason = reason;
			bestConeRejectId = id;
		}
	};

	for (CObject* o : s_nearest)
	{
		bool isAliveCreature = false;
		const u8 fastReason = CheckEntityFastD19(o, actor, isAliveCreature);
		if (!isAliveCreature)
			continue;

		Fvector p;
		if (!CandidateAimPoint(o, p))
			continue;
		const float dist = eye.distance_to(p);
		if (dist > S.curRange || dist < 0.3f)
			continue;

		float ex, ey;
		AngleError(eye, dir, up, right, p, ex, ey);
		const float err = _sqrt(ex * ex + ey * ey);
		const LevelProfile& P = Profile(g_level);
		float cone = ConeFor(dist, P.coneRadiusM, P.coneMinRad, P.coneMaxRad);
		if (IsNewScheme(g_level, IsAutoAim(P)))
			cone = S.curAds ? AdsCaptureCone(g_level, dist) : NearCone(dist); // FIX-LIVE3 п.6
		if (o->ID() == curId)
			cone *= kKeepConeFactor;

		const float normScore = (cone > 0.f) ? (err / cone) : flt_max;
		if (err > cone)
		{
			if (fastReason == rOk)
				++potentialEnemiesOutOfCone;
			continue;
		}

		// Цель внутри конуса прицеливания: проверяем полный каскад анти-радарных фильтров D19.
		if (fastReason != rOk)
		{
			if (fastReason != rNoCandidates)
				recordConeReject(fastReason, o->ID(), normScore);
			continue;
		}

		const u8 luaReason = CheckLuaEnemyEvalCached(o, actor);
		if (luaReason != rOk)
		{
			recordConeReject(luaReason, o->ID(), normScore);
			continue;
		}

		if (!CheckFogFilterD19(dist))
		{
			recordConeReject(rFog, o->ID(), normScore);
			continue;
		}

		if (!CheckIlluminationFilterD19(o, actor, dist, err))
		{
			recordConeReject(rDarkness, o->ID(), normScore);
			continue;
		}

		float score = normScore;
		if (o->ID() == curId)
			score *= 0.8f; // предпочтение текущей цели, без «прыжков» между соседями
		if (score >= bestScore)
			continue;

		float candVis = 1.f;
		const u8 losReason = CheckLineOfSightD19(eye, p, actor, o, candVis);
		S.lastVis = candVis;
		if (losReason != rOk)
		{
			recordConeReject(losReason, o->ID(), normScore);
			continue;
		}

		bestScore = score;
		bestId = o->ID();
		bestObj = o;
		bestVis = candVis;
	}

	if (bestId == u16(-1))
	{
		if (bestConeRejectReason != rNoCandidates)
			DropTarget(bestConeRejectReason, bestConeRejectId);
		else
			DropTarget(potentialEnemiesOutOfCone ? rOutOfCone : rNoCandidates);
		return;
	}

	const u64 bestInstGen = CPauseTokenManager::Instance().GetObjectInstanceGen(bestId);
	if (bestId != curId || bestObj != S.targetObj || bestInstGen != S.targetInstanceGen)
	{
		DropTarget(rOk);
		S.targetId = bestId;
		S.targetInstanceGen = bestInstGen;
		S.targetObj = bestObj;
	}
	S.lastVis = bestVis;
	S.lastLockedLosMs = Device.dwTimeGlobal;
	S.reason = rOk;
	S.lastRejectId = u16(-1);
}

void MaybeLogPeriodicDebug(bool ads)
{
	if (!g_debugLog || Device.dwTimeGlobal - S.lastLogMs < 500)
		return;
	S.lastLogMs = Device.dwTimeGlobal;

	float avgUs = 0.f, p50Us = 0.f, p95Us = 0.f, p99Us = 0.f, maxUs = 0.f;
	g_timing.Compute(avgUs, p50Us, p95Us, p99Us, maxUs);

	if (S.targetId != u16(-1) && S.reason == rOk)
	{
		CObject* o = g_pGameLevel ? Level().Objects.net_Find(S.targetId) : nullptr;
		Msg("* [Gamepad] gp_aa: lvl=%s reason=ok target=%u(%s) dist=%.1f err=%.2fdeg cone=%.2fdeg vis=%.2f lum=%.3f fog=%.2f(max=%.1fm) mag=%.2f scopeK=%.2f slow=%.2f rot=(%.3f,%.3f)deg trk=(%.3f,%.3f)deg pull=(%.3f,%.3f)deg auto=(%.3f,%.3f)deg hold=%.2fs ads=%d p99=%.1fus",
		    LevelName(g_level), S.targetId, o ? o->cName().c_str() : "-", S.lastDist, rad2deg(S.lastErr), rad2deg(S.lastCone),
		    S.lastVis, S.lastLum, S.lastFogDensity, S.lastFogMaxRange, S.lastMag, S.scopeK, S.lastSlowMul,
		    rad2deg(S.lastRotYaw), rad2deg(S.lastRotPitch), rad2deg(S.lastTrkYaw), rad2deg(S.lastTrkPitch),
		    rad2deg(S.lastPullYaw), rad2deg(S.lastPullPitch), rad2deg(S.lastAutoYaw), rad2deg(S.lastAutoPitch), S.autoHold,
		    ads ? 1 : 0, p99Us);
	}
	else
	{
		CObject* rejObj = (g_pGameLevel && S.lastRejectId != u16(-1)) ? Level().Objects.net_Find(S.lastRejectId) : nullptr;
		Msg("* [Gamepad] gp_aa: lvl=%s reason=%s rejected_id=%d(%s) vis=%.2f lum=%.3f fog=%.2f(max=%.1fm) p99=%.1fus",
		    LevelName(g_level), ReasonName(S.reason), S.lastRejectId == u16(-1) ? -1 : int(S.lastRejectId),
		    rejObj ? rejObj->cName().c_str() : "-", S.lastVis, S.lastLum, S.lastFogDensity, S.lastFogMaxRange, p99Us);
	}
}
} // namespace

void Process(const FrameInput& in, FrameOutput& out)
{
	ScopedQpcTimer qpcTimer;

	out.lookX = in.lookX;
	out.lookY = in.lookY;
	out.addYaw = 0.f;
	out.addPitch = 0.f;
	++S.callCount;
	ClearContrib();

	// Смена уровня/загрузка: состояние от старого уровня недействительно (ID объектов переиспользуются).
	const u32 curSessionGen = CPauseTokenManager::Instance().GetSessionGen();
	if (S.level != (const void*)g_pGameLevel || S.sessionGen != curSessionGen)
	{
		const u32 calls = S.callCount;
		const u32 epochSeen = S.lastEpochSeen;
		const u32 resets = S.levelResets;
		S = State();
		S.callCount = calls;
		S.lastEpochSeen = epochSeen;
		S.levelResets = resets;
		S.level = (const void*)g_pGameLevel;
		S.sessionGen = curSessionGen;
	}

	// E4.1b: возврат после off / смена уровня / разрыв кадров — переходное состояние сбрасывается,
	// профиль нового уровня действует с этого же кадра (Profile(g_level) берётся каждый кадр).
	if (SyncLevelAndContinuity(S, g_levelEpoch, Device.dwFrame))
	{
		const LevelProfile& AP = Profile(g_level);
		Msg("* [Gamepad] gp_aa: applied level=%u(%s) stick_in_cone=x%.2f cone=%.1fm rot=%.2f trk=%.2f pull=%.2f/%.2fs auto=%s epoch=%u source=%s (transient state reset)",
		    g_level, LevelName(g_level), 1.f - AP.slowdown, AP.coneRadiusM, AP.rotGain, AP.trkGain, AP.pullGain,
		    AP.pullDuration, IsAutoAim(AP) ? "on" : "off", g_levelEpoch, in.gamepadSource ? "gamepad" : "kbm");
	}

	// Мышь/клавиатура — выход ровно 0 (D05/kb05 §6).
	if (!in.gamepadSource)
	{
		DropTarget(rNotGamepad);
		S.engage = 0.f;
		S.prevAds = in.ads;
		S.pullLeft = 0.f;
		MaybeLogPeriodicDebug(in.ads);
		return;
	}
	if (!in.eligible || !in.actor || !in.cam || !g_pGameLevel)
	{
		DropTarget(rNotEligible);
		S.engage = 0.f;
		S.prevAds = in.ads;
		S.pullLeft = 0.f;
		MaybeLogPeriodicDebug(in.ads);
		return;
	}
	if (!IsFirearmWeapon(in.weapon))
	{
		DropTarget(rNoWeapon);
		S.engage = 0.f;
		S.prevAds = in.ads;
		S.pullLeft = 0.f;
		MaybeLogPeriodicDebug(in.ads);
		return;
	}
	if (!_valid(in.dt) || in.dt <= 0.f || in.dt > kMaxFrameDt || !_valid(in.lookX) || !_valid(in.lookY))
	{
		DropTarget(rBadFrame);
		S.engage = 0.f;
		S.pullLeft = 0.f;
		S.hasHistory = false;
		S.prevAds = in.ads;
		return;
	}

	// Разрыв кадров (меню, пауза, консоль, Off) — история скоростей недействительна.
	if (S.lastFrame + 1 != Device.dwFrame)
		S.hasHistory = false;
	S.lastFrame = Device.dwFrame;

	const LevelProfile& P = Profile(g_level);
	const Fvector eye = in.cam->Position();
	const Fvector dir = in.cam->Direction();
	const Fvector up = in.cam->Up();
	const Fvector right = in.cam->Right();

	// Оптика >4x и 3D-прицелы (3DSS): при включённом disableOnScope (K6b, по умолчанию 1) помощь плавно гаснет до 0
	// (включая Slowdown); при disableOnScope == 0 помощь на оптике не ослабляется (scopeTarget = 1.f).
	const bool sdsActive = in.weapon && in.weapon->IsZoomed() && Device.m_SecondViewport.IsSVPActive();
	S.lastMag = Magnification(in.weapon, in.cam);
	const bool disableOnScope = (g_disableOnScope != 0);
	const float scopeTarget = IsAutoAim(P) ? 1.f : ScopeTarget(S.lastMag, sdsActive, disableOnScope); // full_auto: D18-гашение не для спецвозможностей
	const float step = kScopeFadeRate * in.dt;
	if (S.scopeK < scopeTarget) S.scopeK = (S.scopeK + step > scopeTarget) ? scopeTarget : S.scopeK + step;
	else if (S.scopeK > scopeTarget) S.scopeK = (S.scopeK - step < scopeTarget) ? scopeTarget : S.scopeK - step;

	S.curRange = (in.ads || IsAutoAim(P)) ? kMaxRange : kHipRange;
	S.curAds = in.ads;
	const bool newScheme = IsNewScheme(g_level, IsAutoAim(P));

	// FIX-LIVE3 п.6: резкое движение правого стика (сильный наклон или рывок) — торможение и довод
	// гаснут и ждут kSharpHoldSec после успокоения стика (смена цели не борется с помощью).
	{
		const float lookMagNow = _sqrt(in.lookX * in.lookX + in.lookY * in.lookY);
		const float dlx = in.lookX - S.prevLookX, dly = in.lookY - S.prevLookY;
		const float lookRate = S.hasHistory ? _sqrt(dlx * dlx + dly * dly) / in.dt : 0.f;
		S.prevLookX = in.lookX;
		S.prevLookY = in.lookY;
		if (IsSharpStick(lookMagNow, lookRate))
			S.sharpHold = kSharpHoldSec;
		else if (S.sharpHold > 0.f)
			S.sharpHold = (S.sharpHold > in.dt) ? S.sharpHold - in.dt : 0.f;
	}
	const bool sharp = newScheme && S.sharpHold > 0.f;

	// Вход в ADS — окно мягкого довода.
	if (in.ads && !S.prevAds)
		S.pullLeft = P.pullDuration;
	else if (!in.ads)
		S.pullLeft = 0.f;
	S.prevAds = in.ads;

	if (Device.dwTimeGlobal - S.lastScanMs >= u32(kScanPeriodMs) || S.lastScanMs > Device.dwTimeGlobal ||
	    (in.ads && S.pullLeft == P.pullDuration))
	{
		S.lastScanMs = Device.dwTimeGlobal;
		Scan(eye, dir, up, right, in.actor);
	}

	// E4.1b full_auto: ручной ввод правым стиком приостанавливает автонаведение (смена цели, осмотр).
	const bool autoAim = IsAutoAim(P);
	if (autoAim && AutoAimStickOverride(in.lookX, in.lookY))
		S.autoHold = kAutoResumeDelay;
	else if (S.autoHold > 0.f)
		S.autoHold = (S.autoHold > in.dt) ? S.autoHold - in.dt : 0.f;

	// Slowdown применяется и при потере цели: степень включения гаснет по времени, без ступенек.
	// K6b: при disableOnScope != 0 на оптике (>4x) и в 3D-прицелах (3DSS) Slowdown также масштабируется через S.scopeK.
	auto applySlowdown = [&](float targetEngage) {
		StepToward(S.engage, targetEngage, kEngageRate * in.dt);
		const float slowScopeK = (disableOnScope && !autoAim) ? S.scopeK : 1.f;
		float baseSlow = P.slowdown;
		if (in.ads && g_level >= 4 && baseSlow > 0.f) // FIX-LIVE2: Слабая/Средняя/Сильная — одинаковые 25%
			baseSlow = (baseSlow * 1.12f > 0.84f) ? 0.84f : (baseSlow * 1.12f);
		if (newScheme && !in.ads)
			baseSlow *= kHipSlowShare; // FIX-LIVE3 п.6: от бедра — 70% от ADS
		const float k = 1.f - baseSlow * g_partSlowdown * S.engage * slowScopeK;
		out.lookX = in.lookX * k;
		out.lookY = in.lookY * k;
		S.lastSlowMul = k;
	};

	if (S.targetId == u16(-1))
	{
		S.pullLeft = (S.pullLeft > in.dt) ? S.pullLeft - in.dt : 0.f;
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	const u16 lockedId = S.targetId;
	const u64 curInstGen = CPauseTokenManager::Instance().GetObjectInstanceGen(lockedId);
	CObject* o = Level().Objects.net_Find(lockedId);
	if (!o || o != S.targetObj || curInstGen != S.targetInstanceGen)
	{
		DropTarget(rTargetLost, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	// По-кадровая проверка D19 для захваченной цели (мгновенный сброс при ранении, смене маскировки и т.д.).
	bool isAliveCreature = false;
	const u8 fastReason = CheckEntityFastD19(o, in.actor, isAliveCreature);
	if (!isAliveCreature || fastReason != rOk)
	{
		const u8 dropReason = (fastReason != rOk && fastReason != rNoCandidates) ? fastReason : rTargetLost;
		BumpReasonCounter(dropReason);
		DropTarget(dropReason, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	const u8 luaReason = CheckLuaEnemyEvalCached(o, in.actor);
	if (luaReason != rOk)
	{
		BumpReasonCounter(luaReason);
		DropTarget(luaReason, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	Fvector aim;
	if (!AimPoint(o, aim))
	{
		DropTarget(rTargetLost, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	const float dist = eye.distance_to(aim);
	float ex, ey;
	AngleError(eye, dir, up, right, aim, ex, ey);
	const float err = _sqrt(ex * ex + ey * ey);
	const float cone = (newScheme ? (in.ads ? AdsCaptureCone(g_level, dist) : NearCone(dist))
	                              : ConeFor(dist, P.coneRadiusM, P.coneMinRad, P.coneMaxRad)) * kKeepConeFactor;
	S.lastDist = dist;
	S.lastErr = err;
	S.lastCone = cone;
	if (dist > S.curRange || err > cone)
	{
		DropTarget(rOutOfCone, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	if (!CheckFogFilterD19(dist))
	{
		BumpReasonCounter(rFog);
		DropTarget(rFog, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	if (!CheckIlluminationFilterD19(o, in.actor, dist, err))
	{
		BumpReasonCounter(rDarkness);
		DropTarget(rDarkness, lockedId);
		applySlowdown(0.f);
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	if (Device.dwTimeGlobal - S.lastLockedLosMs >= kLockedLosPeriodMs || S.lastLockedLosMs > Device.dwTimeGlobal)
	{
		S.lastLockedLosMs = Device.dwTimeGlobal;
		float vis = 1.f;
		const u8 losReason = CheckLineOfSightD19(eye, aim, in.actor, o, vis);
		S.lastVis = vis;
		if (losReason != rOk)
		{
			BumpReasonCounter(losReason);
			DropTarget(losReason, lockedId);
			applySlowdown(0.f);
			MaybeLogPeriodicDebug(in.ads);
			return;
		}
	}

	// FIX-LIVE3 п.6: новая схема. Торможение — только «у цели» (малый конус), гаснет при рывке стика.
	// От бедра на этом всё: rot/trk/pull нет. В ADS — rot/trk у цели (как раньше) + непрерывный довод
	// внутри конуса захвата уровня.
	if (newScheme)
	{
		const float nearC = NearCone(dist) * kKeepConeFactor;
		const float slowCone = (cone < nearC) ? cone : nearC;
		const float nearK = ConeFalloff(err, slowCone);
		applySlowdown(sharp ? 0.f : nearK);
		if (in.ads && S.hasHistory)
		{
			float exPrevPrev, eyPrevPrev, exPrevNow, eyPrevNow;
			AngleError(S.prevEye, dir, up, right, S.prevAim, exPrevPrev, eyPrevPrev);
			AngleError(eye, dir, up, right, S.prevAim, exPrevNow, eyPrevNow);
			const float selfYaw = WrapPi(exPrevNow - exPrevPrev);
			const float selfPitch = eyPrevNow - eyPrevPrev;
			const float a = 1.f - expf(-in.dt / kTrackFilterTau);
			S.trkYawRate += (WrapPi(ex - exPrevNow) / in.dt - S.trkYawRate) * a;
			S.trkPitchRate += ((ey - eyPrevNow) / in.dt - S.trkPitchRate) * a;
			const float inNear = (nearK < S.engage) ? nearK : S.engage;
			if (in.moveMag > kMoveStickActive)
			{
				const float s = g_partRotational * S.scopeK * inNear;
				float y = selfYaw * P.rotGain * s, p = selfPitch * P.rotGain * s;
				CapRate(y, p, P.rotMaxRate, in.dt);
				out.addYaw += y;
				out.addPitch += p;
				S.lastRotYaw = y;
				S.lastRotPitch = p;
			}
			{
				const float s = g_partTracking * S.scopeK * inNear;
				float y = S.trkYawRate * in.dt * P.trkGain * s, p = S.trkPitchRate * in.dt * P.trkGain * s;
				CapRate(y, p, P.trkMaxRate, in.dt);
				out.addYaw += y;
				out.addPitch += p;
				S.lastTrkYaw = y;
				S.lastTrkPitch = p;
			}
		}
		if (in.ads && !sharp)
		{
			// Довод: сильнее в центре конуса, у края — половина (без ступеньки при входе цели в конус).
			const AdsCapture& C = kAdsCapture[g_level];
			const float edge = (cone > 0.f) ? clampr(err / cone, 0.f, 1.f) : 1.f;
			const float s = g_partAdsPull * S.scopeK * (1.f - 0.5f * edge);
			const float frac = (1.f - expf(-in.dt / kAdsPullTau)) * C.pullGain * s;
			const float lead = (g_level == 3) ? kStrongLeadSec : 0.f;
			float y = (ex + S.trkYawRate * lead) * frac;
			float p = (ey + S.trkPitchRate * lead) * frac;
			CapRate(y, p, C.pullMaxRate / (S.lastMag > 1.f ? S.lastMag : 1.f), in.dt);
			out.addYaw += y;
			out.addPitch += p;
			S.lastPullYaw = y;
			S.lastPullPitch = p;
		}
		S.prevEye = eye;
		S.prevAim = aim;
		S.hasHistory = true;
		S.reason = rOk;
		MaybeLogPeriodicDebug(in.ads);
		return;
	}

	// 1) Slowdown: множитель правого стика внутри конуса (сглажен по времени).
	applySlowdown(ConeFalloff(err, cone));
	// Остальные части не сильнее текущей сглаженной степени включения.
	const float inCone = (ConeFalloff(err, cone) < S.engage) ? ConeFalloff(err, cone) : S.engage;
	const float lookMag = _sqrt(in.lookX * in.lookX + in.lookY * in.lookY);
	const bool lookActive = (lookMag > 0.03f && lookMag < 0.85f);

	if (S.hasHistory)
	{
		// Разложение видимого смещения цели на «двигался я» и «двигалась цель» в осях текущей камеры.
		float exPrevPrev, eyPrevPrev, exPrevNow, eyPrevNow;
		AngleError(S.prevEye, dir, up, right, S.prevAim, exPrevPrev, eyPrevPrev);
		AngleError(eye, dir, up, right, S.prevAim, exPrevNow, eyPrevNow);
		const float selfYaw = WrapPi(exPrevNow - exPrevPrev);
		const float selfPitch = eyPrevNow - eyPrevPrev;
		// НЧ-фильтр скорости цели: покачивание корпуса в анимациях и редкий пересчёт костей не дают тряски.
		const float a = 1.f - expf(-in.dt / kTrackFilterTau);
		S.trkYawRate += (WrapPi(ex - exPrevNow) / in.dt - S.trkYawRate) * a;
		S.trkPitchRate += ((ey - eyPrevNow) / in.dt - S.trkPitchRate) * a;
		const float tgtYaw = S.trkYawRate * in.dt;
		const float tgtPitch = S.trkPitchRate * in.dt;

		// 2) Rotational Tracking: при движении левым стиком (или в ADS при активном ведении).
		if (in.moveMag > kMoveStickActive || (in.ads && lookActive))
		{
			const float moveFactor = (in.moveMag > kMoveStickActive) ? 1.f : 0.65f;
			const float s = g_partRotational * S.scopeK * inCone * moveFactor;
			float y = selfYaw * P.rotGain * s;
			float p = selfPitch * P.rotGain * s;
			CapRate(y, p, P.rotMaxRate, in.dt);
			out.addYaw += y;
			out.addPitch += p;
			S.lastRotYaw = y;
			S.lastRotPitch = p;
		}

		// 3) Сопровождение движущейся цели — в ADS и full_auto в полную силу, от бедра при активных стиках — 0.75.
		if (in.ads || autoAim || in.moveMag > kMoveStickActive || lookActive)
		{
			const float hipScale = (in.ads || autoAim) ? 1.f : 0.75f;
			const float s = g_partTracking * S.scopeK * inCone * hipScale;
			float y = tgtYaw * P.trkGain * s;
			float p = tgtPitch * P.trkGain * s;
			CapRate(y, p, P.trkMaxRate, in.dt);
			out.addYaw += y;
			out.addPitch += p;
			S.lastTrkYaw = y;
			S.lastTrkPitch = p;
		}
	}

	// 4) Мягкий довод при входе в ADS + плавное магнитное притяжение к центру масс при ведении стиком / в ADS.
	if (in.ads && S.pullLeft > 0.f)
	{
		const float pullCone = ConeFor(dist, P.pullConeRadiusM, kPullConeMinRad, P.pullConeMaxRad);
		if (err <= pullCone)
		{
			const float s = g_partAdsPull * S.scopeK;
			const float frac = (1.f - expf(-in.dt / kPullTau)) * P.pullGain * s;
			float y = ex * frac;
			float p = ey * frac;
			CapRate(y, p, P.pullMaxRate, in.dt);
			out.addYaw += y;
			out.addPitch += p;
			S.lastPullYaw = y;
			S.lastPullPitch = p;
		}
		S.pullLeft = (S.pullLeft > in.dt) ? S.pullLeft - in.dt : 0.f;
	}
	else if (!autoAim && g_level == 3 && in.ads && P.pullGain > 0.f)
	{
		// FIX-LIVE2 п.2: «Сильная» — непрерывный довод к верху груди, пока зажат LT, с упреждением 40 мс;
		// на оптике гаснет вместе с S.scopeK (D18), ручной ввод стиком не блокирует.
		const float s = g_partAdsPull * S.scopeK * inCone;
		const float frac = (1.f - expf(-in.dt / 0.10f)) * P.pullGain * s;
		float y = (ex + S.trkYawRate * kStrongLeadSec) * frac;
		float p = (ey + S.trkPitchRate * kStrongLeadSec) * frac;
		CapRate(y, p, P.pullMaxRate / (S.lastMag > 1.f ? S.lastMag : 1.f), in.dt);
		out.addYaw += y;
		out.addPitch += p;
		S.lastPullYaw = y;
		S.lastPullPitch = p;
	}
	else if (!autoAim && P.pullGain > 0.f && (in.ads || lookActive || (g_level >= 4 && in.moveMag > kMoveStickActive)))
	{
		// Плавная магнитная доводка внутри конуса захвата после завершения стартового окна ADS (и от бедра при ведении стиком).
		const float modeScale = in.ads ? 0.55f : (lookActive ? 0.40f : 0.28f);
		const float s = g_partAdsPull * S.scopeK * inCone * modeScale;
		const float frac = (1.f - expf(-in.dt / 0.12f)) * P.pullGain * s;
		float y = ex * frac;
		float p = ey * frac;
		CapRate(y, p, P.pullMaxRate * (in.ads ? 0.65f : 0.45f), in.dt);
		out.addYaw += y;
		out.addPitch += p;
		S.lastPullYaw = y;
		S.lastPullPitch = p;
	}

	// 5) E4.1b full_auto: непрерывный довод к центру масс захваченной видимой цели. Плавно (экспонента
	// tau, ограничение скорости, на оптике делится на кратность), набирается вместе с engage, пауза при
	// ручном вводе стиком. Голова не выбирается (D18), фильтры анти-радара D19 действуют как на всех уровнях.
	if (autoAim && S.autoHold <= 0.f)
	{
		const float frac = (1.f - expf(-in.dt / P.autoTau)) * P.autoGain * g_partAuto * S.engage;
		float y = ex * frac;
		float p = ey * frac;
		CapRate(y, p, P.autoMaxRate / (S.lastMag > 1.f ? S.lastMag : 1.f), in.dt);
		out.addYaw += y;
		out.addPitch += p;
		S.lastAutoYaw = y;
		S.lastAutoPitch = p;
	}

	S.prevEye = eye;
	S.prevAim = aim;
	S.hasHistory = true;
	S.reason = rOk;

	MaybeLogPeriodicDebug(in.ads);
}

// ---------------- Консоль ----------------
namespace
{
xr_token gp_aa_level_tokens[] = {
	{"off", 0}, {"smooth", 1}, {"medium", 2}, {"strong", 3}, {"super", 4}, {"full_auto", 5}, {nullptr, 0}};

// Офлайн-проверка математики E4.1 и анти-радарных фильтров E4.2 (D19) + бенчмарк p99.
bool SelfTest()
{
	u32 passed = 0, failed = 0;
	auto check = [&](bool ok, LPCSTR what) {
		if (ok) ++passed;
		else { ++failed; Msg("! [Gamepad] gp_aa selftest FAIL: %s", what); }
	};
	const Fvector eye = {0, 0, 0}, dir = {0, 0, 1}, up = {0, 1, 0};
	const Fvector right = Fvector().crossproduct(up, dir);
	float ex, ey;
	AngleError(eye, dir, up, right, Fvector().set(1, 0, 10), ex, ey);
	check(ex > 0.f && _abs(ey) < 1e-5f, "target right -> ex>0");
	AngleError(eye, dir, up, right, Fvector().set(0, 1, 10), ex, ey);
	check(ey > 0.f && _abs(ex) < 1e-5f, "target above -> ey>0");
	check(_abs(ex) < 1e-5f && _abs(ey - atan2f(1.f, 10.f)) < 1e-4f, "pitch magnitude");
	check(_abs(WrapPi(PI_MUL_2 + 0.1f) - 0.1f) < 1e-5f && _abs(WrapPi(-PI - 0.1f) - (PI - 0.1f)) < 1e-4f, "WrapPi");
	check(LevelStrength(0) == 0.f && LevelStrength(2) > LevelStrength(1) && LevelStrength(4) == 1.f, "level strengths monotonic");
	check(ConeFalloff(0.f, 0.1f) == 1.f && ConeFalloff(0.1f, 0.1f) == 0.f && ConeFalloff(0.05f, 0.1f) > 0.5f, "cone falloff");
	check(ConeFor(1.f, Profile(2).coneRadiusM, Profile(2).coneMinRad, Profile(2).coneMaxRad) == Profile(2).coneMaxRad &&
	      ConeFor(1000.f, Profile(2).coneRadiusM, Profile(2).coneMinRad, Profile(2).coneMaxRad) == Profile(2).coneMinRad &&
	      Profile(2).coneMinRad == kConeMinRad, "cone clamps");
	check(ScopeTarget(1.f, false, true) == 1.f && ScopeTarget(4.f, false, true) == 1.f && ScopeTarget(6.f, false, true) == 0.f &&
	      ScopeTarget(4.5f, false, true) > 0.f && ScopeTarget(4.5f, false, true) < 1.f &&
	      ScopeTarget(2.f, true, true) == 0.f && ScopeTarget(6.f, true, false) == 1.f,
	      "scope fade >4x and K6b disable_on_scope/3DSS");
	float y = 1.f, p = 1.f;
	CapRate(y, p, 1.f, 0.01f);
	check(_abs(_sqrt(y * y + p * p) - 0.01f) < 1e-5f, "rate cap");
	// Довод на каждом уровне: за окно уровня закрывается не вся ошибка (не рывок), чем выше уровень —
	// тем больше; на medium — больше половины (как в E4.1).
	{
		float prevResidual = deg2rad(3.f) + 1e-6f;
		bool softAll = true, monotonic = true;
		float mediumResidual = 0.f;
		for (u32 lvl = 1; lvl <= 4; ++lvl)
		{
			const LevelProfile& P = Profile(lvl);
			float e = deg2rad(3.f), t = 0.f;
			const float dt = 1.f / 60.f;
			while (t < P.pullDuration)
			{
				float yy = e * (1.f - expf(-dt / kPullTau)) * P.pullGain, pp = 0.f;
				CapRate(yy, pp, P.pullMaxRate, dt);
				e -= yy;
				t += dt;
			}
			softAll = softAll && e > 0.f;
			monotonic = monotonic && (lvl == 1 ? e <= prevResidual : e < prevResidual);
			prevResidual = e;
			if (lvl == 2)
				mediumResidual = e;
		}
		check(softAll && monotonic && mediumResidual < deg2rad(3.f), "ads pull soft on all levels, stronger with level (smooth=0)");
	}
	// E4.1b: профили — каждая часть растёт с уровнем, off = 0, доли не больше 1 (без перекомпенсации),
	// smooth/medium совпадают с E4.1, super: стик в центре x0.25, конус на 20 м заметно шире medium.
	{
		bool mono = true, capped = true;
		for (u32 lvl = 1; lvl <= 4; ++lvl)
		{
			const LevelProfile& a = Profile(lvl - 1);
			const LevelProfile& b = Profile(lvl);
			mono = mono && b.slowdown >= a.slowdown && b.coneRadiusM >= a.coneRadiusM && b.rotGain > a.rotGain &&
			       b.trkGain > a.trkGain && b.pullGain >= a.pullGain && b.rotMaxRate > a.rotMaxRate &&
			       b.trkMaxRate > a.trkMaxRate && b.pullMaxRate > a.pullMaxRate && b.pullDuration >= a.pullDuration;
			capped = capped && b.slowdown < 1.f && b.rotGain <= 1.f && b.trkGain <= 1.f && b.pullGain <= 1.f;
		}
		check(mono && capped && Profile(0).slowdown == 0.f && Profile(kLevelCount + 1).slowdown == 0.f,
		      "E4.1b level profiles monotonic, capped, off=0");
		check(Profile(1).slowdown == 0.25f && Profile(2).slowdown == 0.25f && Profile(3).slowdown == 0.25f &&
		      Profile(1).rotGain == 0.15f && Profile(2).rotGain == 0.35f && Profile(3).rotGain == 0.65f &&
		      Profile(1).trkGain == 0.20f && Profile(2).trkGain == 0.50f && Profile(3).trkGain == 0.75f &&
		      Profile(1).pullGain == 0.f && Profile(2).pullGain == 0.20f && Profile(3).pullGain == 0.85f &&
		      kHipRange == 30.f && kMaxRange == 160.f && kTrackFilterTau == 0.06f && kStrongLeadSec == 0.04f,
		      "FIX-LIVE2 presets weak/medium/strong, range 30/160, filter 60ms, lead 40ms");
		check(_abs((1.f - Profile(4).slowdown) - 0.25f) < 1e-6f &&
		      ConeFor(20.f, Profile(4).coneRadiusM, kConeMinRad, Profile(4).coneMaxRad) >
		          1.4f * ConeFor(20.f, Profile(2).coneRadiusM, kConeMinRad, Profile(2).coneMaxRad),
		      "E4.1b super: stick x0.25 in center, cone at 20 m >1.4x medium");
	}
	// FIX-LIVE3 п.6: новая схема — конус захвата в ADS растёт с уровнем (Слабая впритык, Сильная ~весь экран),
	// довод растёт с уровнем, от бедра торможение 70%, резкий стик гасит помощь.
	check(AdsCaptureCone(1, 20.f) <= 3.f * kDeg && AdsCaptureCone(1, 20.f) < AdsCaptureCone(2, 20.f) &&
	      AdsCaptureCone(2, 20.f) < AdsCaptureCone(3, 20.f) && AdsCaptureCone(3, 100.f) >= 28.f * kDeg &&
	      kAdsCapture[1].pullGain < kAdsCapture[2].pullGain && kAdsCapture[2].pullGain < kAdsCapture[3].pullGain &&
	      kHipSlowShare == 0.70f && NearCone(1.f) == kNearMaxRad && IsNewScheme(2, false) && !IsNewScheme(4, false) &&
	      !IsNewScheme(2, true) && IsSharpStick(0.9f, 0.f) && IsSharpStick(0.3f, 7.f) && !IsSharpStick(0.3f, 1.f),
	      "FIX-LIVE3 ads capture cones, pull by level, hip slowdown 70%, sharp stick");
	// Slowdown на краю конуса — 1 (нет «ступеньки»), в центре при «Супер» — 1-slowdown профиля.
	check(_abs((1.f - Profile(4).slowdown * ConeFalloff(0.f, 0.1f)) - (1.f - Profile(4).slowdown)) < 1e-6f &&
	      (1.f - Profile(4).slowdown * ConeFalloff(0.1f, 0.1f)) == 1.f, "slowdown range");

	// --- E4.2: проверки цепочки D19 ---
	// 1) Прозрачность кустов и стен (fVisTransparencyFactor)
	{
		float vis = 0.f;
		const float oneThinBush[] = {0.65f};
		check(EvaluateVisTransparency(oneThinBush, 1, 0.25f, vis) == rOk && _abs(vis - 0.65f) < 1e-5f,
		      "D19 single sparse bush passes");
		const float threeBushes[] = {0.6f, 0.6f, 0.6f}; // 0.216 <= 0.25 -> rOccludedBush
		check(EvaluateVisTransparency(threeBushes, 3, 0.25f, vis) == rOccludedBush && vis <= 0.25f,
		      "D19 multi-bush rejects with occluded_bush");
		const float solidWall[] = {0.7f, 0.0f};
		check(EvaluateVisTransparency(solidWall, 2, 0.25f, vis) == rOccluded && vis == 0.f,
		      "D19 opaque wall rejects with occluded_static");
		const float badFactor[] = {-0.5f};
		check(EvaluateVisTransparency(badFactor, 1, 0.25f, vis) == rOccluded,
		      "D19 invalid material factor fails closed");
	}
	// 2) Освещённость, фонарь и ПНВ/тепловизор
	{
		check(!EvaluateIllumination(0.01f, 20.f, deg2rad(2.f), false, false, 0.04f, 35.f),
		      "D19 darkness without light/NVG rejects");
		check(EvaluateIllumination(0.15f, 20.f, deg2rad(2.f), false, false, 0.04f, 35.f),
		      "D19 lit target without light/NVG passes");
		check(EvaluateIllumination(0.005f, 25.f, deg2rad(4.f), true, false, 0.04f, 35.f),
		      "D19 dark target inside active torch cone+range passes");
		check(!EvaluateIllumination(0.005f, 55.f, deg2rad(4.f), true, false, 0.04f, 35.f),
		      "D19 dark target beyond torch range rejects");
		check(EvaluateIllumination(0.0f, 80.f, deg2rad(4.f), false, true, 0.04f, 35.f),
		      "D19 dark target with active NVG/thermal passes");
		check(!EvaluateIllumination(-1.f, 15.f, deg2rad(2.f), false, false, 0.04f, 35.f),
		      "D19 missing ROS luminosity fails closed");
	}
	// 3) Туман (fog_density / fog_distance)
	{
		float effRange = 0.f;
		check(EvaluateFogRange(70.f, 120.f, 0.1f, 300.f, false, 0.55f, effRange) && effRange >= 100.f,
		      "D19 clear weather preserves range");
		check(!EvaluateFogRange(45.f, 120.f, 0.75f, 60.f, false, 0.55f, effRange) && effRange < 35.f,
		      "D19 thick fog attenuates and rejects far target");
		check(EvaluateFogRange(45.f, 120.f, 0.75f, 60.f, true, 0.55f, effRange),
		      "D19 thermal vision bypasses fog attenuation");
		check(!EvaluateFogRange(10.f, 120.f, -1.f, 0.f, false, 0.55f, effRange),
		      "D19 invalid fog params fail closed");
	}
	// 4) Строковые коды причин отсева для gp_aa_debug 1
	{
		check(!xr_strcmp(ReasonName(rOccludedBush), "occluded_bush") &&
		      !xr_strcmp(ReasonName(rDarkness), "darkness") &&
		      !xr_strcmp(ReasonName(rFog), "fog") &&
		      !xr_strcmp(ReasonName(rDisguise), "disguise") &&
		      !xr_strcmp(ReasonName(rWounded), "wounded") &&
		      !xr_strcmp(ReasonName(rCompanion), "companion") &&
		      !xr_strcmp(ReasonName(rEnemyEval), "enemy_eval"),
		      "D19 reason names match specification");
	}
	// 5) Бенчмарк бюджета p99 (512 прогонов полного каскада чистых фильтров + математики)
	{
		const float bushes[] = {0.85f, 0.75f, 0.8f};
		for (int i = 0; i < 512; ++i)
		{
			ScopedQpcTimer timer;
			float vis = 1.f, effRange = 120.f;
			(void)EvaluateVisTransparency(bushes, 3, 0.25f, vis);
			(void)EvaluateFogRange(25.f, 120.f, 0.35f, 120.f, false, 0.55f, effRange);
			(void)EvaluateIllumination(0.08f, 25.f, deg2rad(2.5f), false, false, 0.04f, 35.f);
			AngleError(eye, dir, up, right, Fvector().set(0.3f, 0.2f, 25.f), ex, ey);
		}
		float avgUs = 0.f, p50Us = 0.f, p95Us = 0.f, p99Us = 0.f, maxUs = 0.f;
		g_timing.Compute(avgUs, p50Us, p95Us, p99Us, maxUs);
		check(p99Us > 0.f && p99Us < 200.f, "D19 p99 frame budget < 200 us");
		Msg("* [Gamepad] gp_aa selftest timing: avg=%.2fus p50=%.2fus p95=%.2fus p99=%.2fus max=%.2fus",
		    avgUs, p50Us, p95Us, p99Us, maxUs);
	}

	// --- E4.1b: верхний уровень full_auto (полное автонаведение, специальные возможности) ---
	{
		const LevelProfile& A = Profile(kLevelFullAuto);
		bool classicNoAuto = true;
		for (u32 lvl = 0; lvl < kLevelFullAuto; ++lvl)
			classicNoAuto = classicNoAuto && !IsAutoAim(Profile(lvl));
		check(classicNoAuto && IsAutoAim(A) && kLevelFullAuto == kLevelCount - 1 && !xr_strcmp(LevelName(kLevelFullAuto), "full_auto"),
		      "full_auto: only the top, explicitly named level auto-aims");
		check(A.rotGain == 1.f && A.trkGain == 1.f && A.autoGain <= 1.f && A.autoMaxRate > Profile(4).rotMaxRate &&
		      ConeFor(50.f, A.coneRadiusM, A.coneMinRad, A.coneMaxRad) >= deg2rad(29.9f) &&
		      ConeFor(120.f, A.coneRadiusM, A.coneMinRad, A.coneMaxRad) >= deg2rad(20.f) &&
		      ConeFor(20.f, A.coneRadiusM, A.coneMinRad, A.coneMaxRad) > 4.f * ConeFor(20.f, Profile(4).coneRadiusM, Profile(4).coneMinRad, Profile(4).coneMaxRad),
		      "full_auto: wide cone 20-30 deg, full tracking/rotational, no overcompensation");
		check(AutoAimStickOverride(0.6f, 0.f) && AutoAimStickOverride(0.4f, 0.4f) && !AutoAimStickOverride(0.3f, 0.2f) && !AutoAimStickOverride(0.f, 0.f),
		      "full_auto: right stick over 0.5 overrides auto-aim");
		// Неподвижная цель в 15° справа, 60 к/с, engage набирается как в Process: камера доходит плавно,
		// без перелёта и без шага быстрее предела, за 0.5 с остаток < 0.5°, за 1 с < 0.05°.
		{
			const float dt = 1.f / 60.f;
			float e = deg2rad(15.f), engage = 0.f, maxStep = 0.f, residualHalf = 0.f;
			bool noOvershoot = true;
			for (int i = 0; i < 60; ++i)
			{
				StepToward(engage, 1.f, kEngageRate * dt);
				float y = e * (1.f - expf(-dt / A.autoTau)) * A.autoGain * engage, pp = 0.f;
				CapRate(y, pp, A.autoMaxRate, dt);
				maxStep = (y > maxStep) ? y : maxStep;
				e -= y;
				noOvershoot = noOvershoot && e >= 0.f;
				if (i == 29)
					residualHalf = e;
			}
			check(noOvershoot && maxStep <= A.autoMaxRate * dt + 1e-6f && residualHalf < deg2rad(0.5f) && e < deg2rad(0.05f),
			      "full_auto: converges to center of mass smoothly (no snap, no overshoot)");
			// Тот же расчёт на оптике 8x: предел скорости делится на кратность — медленнее, но сходится.
			float e8 = deg2rad(3.f);
			for (int i = 0; i < 120; ++i)
			{
				float y = e8 * (1.f - expf(-dt / A.autoTau)) * A.autoGain, pp = 0.f;
				CapRate(y, pp, A.autoMaxRate / 8.f, dt);
				e8 -= y;
			}
			check(e8 >= 0.f && e8 < deg2rad(0.1f), "full_auto: works through 8x scope with rate / magnification");
		}
		// Через настоящую команду: full_auto ставится, переход super -> full_auto -> super меняет epoch.
		IConsole_Command* cmdA = Console ? Console->GetCommand("gp_aa_level") : nullptr;
		if (cmdA)
		{
			const u32 savedLevel = g_level, savedEpoch = g_levelEpoch, savedPrev = g_levelPrev;
			g_levelLogQuiet = true;
			cmdA->Execute("super");
			const u32 e0 = g_levelEpoch;
			cmdA->Execute("full_auto");
			const bool toAuto = g_level == kLevelFullAuto && IsAutoAim(Profile(g_level)) && g_levelEpoch != e0;
			cmdA->Execute("super");
			check(toAuto && g_level == 4 && !IsAutoAim(Profile(g_level)), "full_auto: console token super -> full_auto -> super");
			g_levelLogQuiet = false;
			g_level = savedLevel;
			g_levelEpoch = savedEpoch;
			g_levelPrev = savedPrev;
		}
	}

	// --- E4.1b: цепочка medium -> off -> medium через настоящую консольную команду gp_aa_level ---
	{
		const u32 savedLevel = g_level, savedEpoch = g_levelEpoch, savedPrev = g_levelPrev;
		IConsole_Command* cmd = Console ? Console->GetCommand("gp_aa_level") : nullptr;
		check(cmd != nullptr, "E4.1b gp_aa_level command registered");
		if (cmd)
		{
			g_levelLogQuiet = true;
			State st;
			const float dt = 1.f / 60.f;

			cmd->Execute("medium");
			check(g_level == 2 && LevelStrength(g_level) == 0.6f, "E4.1b medium -> level 2, strength 0.60");
			SyncLevelAndContinuity(st, g_levelEpoch, 100);
			st.targetId = 7; st.engage = 1.f; st.pullLeft = 0.1f; st.prevAds = true; st.lastScanMs = 12345;
			check(!SyncLevelAndContinuity(st, g_levelEpoch, 101) && st.targetId == 7 && st.engage == 1.f,
			      "E4.1b medium: continuous frames keep target and engage");

			const u32 epochBeforeOff = g_levelEpoch;
			cmd->Execute("off");
			check(g_level == 0 && LevelStrength(g_level) == 0.f && g_levelEpoch == epochBeforeOff + 1,
			      "E4.1b off -> level 0, strength 0, epoch+1 (ActorInput gate g_level!=0 closed)");
			// Пока off, Process не вызывается (D05): кадры 102..200 модуль не видит, st заморожен.

			cmd->Execute("medium");
			check(g_level == 2 && LevelStrength(g_level) > 0.f && g_levelEpoch == epochBeforeOff + 2,
			      "E4.1b back to medium -> level 2, strength>0, epoch+1");
			const bool applied = SyncLevelAndContinuity(st, g_levelEpoch, 201);
			check(applied && st.lastEpochSeen == g_levelEpoch && st.levelResets >= 2,
			      "E4.1b first frame after re-enable applies new level");
			check(st.targetId == u16(-1) && st.engage == 0.f && st.pullLeft == 0.f && !st.prevAds &&
			      st.lastScanMs == 0 && !st.hasHistory,
			      "E4.1b frozen state from before off is reset, scan runs this frame");
			check(!SyncLevelAndContinuity(st, g_levelEpoch, 202), "E4.1b no repeated reset on next frame");
			st.targetId = 5;
			check(!SyncLevelAndContinuity(st, g_levelEpoch, 202) && st.targetId == 5, "E4.1b same-frame call is not a gap");

			// Slowdown после возврата набирает силу как до off: за 0.2 с engage 0 -> 1, множитель 1-0.6*0.6.
			for (int i = 0; i < 12; ++i)
				StepToward(st.engage, 1.f, kEngageRate * dt);
			const float k = 1.f - Profile(g_level).slowdown * g_partSlowdown * st.engage;
			check(st.engage == 1.f && _abs(k - (1.f - Profile(2).slowdown * g_partSlowdown)) < 1e-6f && (g_partSlowdown == 0.f || k < 1.f),
			      "E4.1b slowdown after re-enable equals medium before off");

			// Разрыв кадров без смены уровня (меню/пауза): цель сбрасывается, уровень не «применяется» заново.
			st.targetId = 9;
			check(!SyncLevelAndContinuity(st, g_levelEpoch, 300) && st.targetId == u16(-1),
			      "E4.1b frame gap resets target without level change");

			g_levelLogQuiet = false;
		}
		g_level = savedLevel;
		g_levelEpoch = savedEpoch;
		g_levelPrev = savedPrev;
	}

	Msg("* [Gamepad] gp_aa selftest: %s (passed=%u failed=%u)", failed == 0 ? "PASS" : "FAIL", passed, failed);
	return failed == 0;
}

// E4.1b: gp_aa_level — тот же CCC_Token, плюс фиксация фактической смены (epoch) и строка в лог,
// чтобы по xray-логу было видно, какой уровень реально установлен и какой источник ввода активен.
class CCC_GamepadAimAssistLevel : public CCC_Token
{
public:
	CCC_GamepadAimAssistLevel(LPCSTR N, u32* V, xr_token* T) : CCC_Token(N, V, T) {}
	virtual void Execute(LPCSTR args)
	{
		const u32 before = *value;
		CCC_Token::Execute(args);
		if (*value == before)
			return;
		g_levelPrev = before;
		++g_levelEpoch;
		if (g_levelLogQuiet)
			return;
		const bool gpSource = pInput && pInput->IsGamepadActiveSource();
		Msg("* [Gamepad] gp_aa_level: %s -> %s (stick_in_cone x%.2f -> x%.2f, cone %.1fm -> %.1fm, epoch=%u, source=%s)%s",
		    LevelName(before), LevelName(*value), 1.f - Profile(before).slowdown, 1.f - Profile(*value).slowdown,
		    Profile(before).coneRadiusM, Profile(*value).coneRadiusM, g_levelEpoch,
		    gpSource ? "gamepad" : "kbm",
		    (*value != 0 && !gpSource) ? " - assist resumes on next gamepad stick/button input" : "");
	}
};

class CCC_GamepadAimAssistDebug : public IConsole_Command
{
public:
	CCC_GamepadAimAssistDebug(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	virtual void Execute(LPCSTR args)
	{
		string256 a;
		a[0] = 0;
		if (args)
			sscanf(args, "%255s", a);
		if (!xr_strcmp(a, "selftest"))
		{
			SelfTest();
			return;
		}
		if (!xr_strcmp(a, "1") || !xr_strcmp(a, "on"))
			g_debugLog = 1;
		else if (!xr_strcmp(a, "0") || !xr_strcmp(a, "off"))
			g_debugLog = 0;

		float avgUs = 0.f, p50Us = 0.f, p95Us = 0.f, p99Us = 0.f, maxUs = 0.f;
		g_timing.Compute(avgUs, p50Us, p95Us, p99Us, maxUs);

		Msg("* [Gamepad] gp_aa_debug: level=%u(%s) parts slow=%.2f rot=%.2f trk=%.2f pull=%.2f disable_on_scope=%d log=%d calls=%u",
			g_level, LevelName(g_level), g_partSlowdown, g_partRotational, g_partTracking, g_partAdsPull, g_disableOnScope, g_debugLog, S.callCount);
		{
			// E4.1b: что реально действует сейчас. effective=active — модуль вызывается с геймпада и
			// применил текущий уровень; idle_kbm — активна мышь/клавиатура (вклад 0 до ввода с геймпада);
			// stale — модуль давно не вызывался (off, меню, пауза, консоль); pending — смена уровня ещё
			// не применена ни одним кадром.
			const bool gpSource = pInput && pInput->IsGamepadActiveSource();
			const u32 framesAgo = S.lastProcessFrame ? Device.dwFrame - S.lastProcessFrame : u32(-1);
			LPCSTR effective = (g_level == 0) ? "off"
			                 : !gpSource ? "idle_kbm"
			                 : (S.lastEpochSeen != g_levelEpoch) ? "pending"
			                 : (framesAgo > 2) ? "stale"
			                 : "active";
			const LevelProfile& DP = Profile(g_level);
			Msg("* [Gamepad] gp_aa_debug: profile stick_in_cone=x%.2f cone=%.1fm(%.1f-%.0fdeg) rot=%.2f/%.0fdps trk=%.2f/%.0fdps pull=%.2f/%.0fdps %.2fs pull_cone=%.1fm(max %.0fdeg) auto=%s gain=%.2f tau=%.2fs max=%.0fdps slider=%.2f hold=%.2fs",
				1.f - DP.slowdown, DP.coneRadiusM, rad2deg(DP.coneMinRad), rad2deg(DP.coneMaxRad), DP.rotGain, rad2deg(DP.rotMaxRate), DP.trkGain,
				rad2deg(DP.trkMaxRate), DP.pullGain, rad2deg(DP.pullMaxRate), DP.pullDuration, DP.pullConeRadiusM, rad2deg(DP.pullConeMaxRad),
				IsAutoAim(DP) ? "on" : "off", DP.autoGain, DP.autoTau, rad2deg(DP.autoMaxRate), g_partAuto, S.autoHold);
			Msg("* [Gamepad] gp_aa_debug: effective=%s source=%s epoch=%u applied=%d prev=%s resets=%u last_process_frames_ago=%d engage=%.2f",
				effective, gpSource ? "gamepad" : "kbm", g_levelEpoch,
				S.lastEpochSeen == u32(-1) ? -1 : int(S.lastEpochSeen), LevelName(g_levelPrev), S.levelResets,
				framesAgo == u32(-1) ? -1 : int(framesAgo), S.engage);
		}
		Msg("* [Gamepad] gp_aa_debug: budget_us last=%.2f avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f (window=%zu)",
			g_timing.lastUs, avgUs, p50Us, p95Us, p99Us, maxUs, g_timing.count);
		Msg("* [Gamepad] gp_aa_debug: d19_rejects companion=%u wounded=%u disguise=%u enemy_eval=%u fog=%u darkness=%u occluded_wall=%u occluded_bush=%u (lua_calls=%u cache_hits=%u errors=%u)",
			S.counts.companion, S.counts.wounded, S.counts.disguise, S.counts.enemyEval,
			S.counts.fog, S.counts.darkness, S.counts.occludedWall, S.counts.occludedBush,
			S.counts.luaCalls, S.counts.luaCacheHits, S.counts.luaErrors);
		if (g_level == 0)
		{
			Msg("* [Gamepad] gp_aa_debug: off - module is not called (D05)");
			return;
		}
		CObject* o = (g_pGameLevel && S.targetId != u16(-1)) ? Level().Objects.net_Find(S.targetId) : nullptr;
		Msg("* [Gamepad] gp_aa_debug: target=%d(%s) reason=%s rejected_id=%d dist=%.1f err=%.2fdeg cone=%.2fdeg vis=%.2f lum=%.3f fog=%.2f(max=%.1fm) mag=%.2f scopeK=%.2f",
			S.targetId == u16(-1) ? -1 : int(S.targetId), o ? o->cName().c_str() : "-", ReasonName(S.reason),
			S.lastRejectId == u16(-1) ? -1 : int(S.lastRejectId),
			S.lastDist, rad2deg(S.lastErr), rad2deg(S.lastCone), S.lastVis, S.lastLum, S.lastFogDensity, S.lastFogMaxRange, S.lastMag, S.scopeK);
		Msg("* [Gamepad] gp_aa_debug: contrib slow_mul=%.3f rot=(%.3f,%.3f)deg trk=(%.3f,%.3f)deg pull=(%.3f,%.3f)deg auto=(%.3f,%.3f)deg pull_left=%.3fs",
			S.lastSlowMul, rad2deg(S.lastRotYaw), rad2deg(S.lastRotPitch), rad2deg(S.lastTrkYaw), rad2deg(S.lastTrkPitch),
			rad2deg(S.lastPullYaw), rad2deg(S.lastPullPitch), rad2deg(S.lastAutoYaw), rad2deg(S.lastAutoPitch), S.pullLeft);
	}
	virtual void Info(TInfo& I) { xr_strcpy(I, "[|0|1|selftest] aim assist state / D19 filter stats / QPC p99 budget / offline test"); }
};
class CCC_GamepadAimAssistMagnet : public IConsole_Command
{
public:
	CCC_GamepadAimAssistMagnet(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	virtual void Execute(LPCSTR args)
	{
		if (!args || !args[0])
		{
			Msg("* [Gamepad] gp_aa_magnet = %.2f (rot=%.2f trk=%.2f pull=%.2f auto=%.2f)",
			    g_partAdsPull, g_partRotational, g_partTracking, g_partAdsPull, g_partAuto);
			return;
		}
		float v = (float)atof(args);
		if (v < 0.f) v = 0.f;
		if (v > 1.f) v = 1.f;
		g_partRotational = v;
		g_partTracking = v;
		g_partAdsPull = v;
		g_partAuto = v;
	}
	virtual void Status(TStatus& S)
	{
		xr_sprintf(S, sizeof(S), "%.3f", g_partAdsPull);
	}
	virtual void Info(TInfo& I) { xr_strcpy(I, "[0.0 .. 1.0] unified magnetic assist multiplier (rotational + tracking + ads_pull + auto)"); }
};
} // namespace

void RegisterConsoleCommands()
{
	CMD3(CCC_GamepadAimAssistLevel, "gp_aa_level", &g_level, gp_aa_level_tokens);
	CMD4(CCC_Float, "gp_aa_slowdown", &g_partSlowdown, 0.f, 1.f);
	CMD1(CCC_GamepadAimAssistMagnet, "gp_aa_magnet");
	CMD4(CCC_Float, "gp_aa_rotational", &g_partRotational, 0.f, 1.f);
	CMD4(CCC_Float, "gp_aa_tracking", &g_partTracking, 0.f, 1.f);
	CMD4(CCC_Float, "gp_aa_ads_pull", &g_partAdsPull, 0.f, 1.f);
	CMD4(CCC_Float, "gp_aa_auto", &g_partAuto, 0.f, 1.f);
	CMD4(CCC_Integer, "gp_aa_disable_on_scope", &g_disableOnScope, 0, 1); // K6b
	CMD4(CCC_Float, "gp_aa_vis_threshold", &g_visThreshold, 0.05f, 0.9f);
	CMD4(CCC_Float, "gp_aa_min_lum", &g_minLuminosity, 0.005f, 0.5f);
	CMD4(CCC_Float, "gp_aa_torch_range", &g_torchRange, 5.f, 80.f);
	CMD4(CCC_Float, "gp_aa_fog_cutoff", &g_fogDensityCutoff, 0.1f, 0.95f);
	CMD1(CCC_GamepadAimAssistDebug, "gp_aa_debug");
}
} // namespace gpaa
