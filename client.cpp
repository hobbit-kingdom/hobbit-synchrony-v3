#define _CRT_SECURE_NO_WARNINGS
#include "shared.h"
#include "SecureConnection.h"
#include "Player.h"
#include "NPC.h"
#include "Marker.h"
#include "HobbitGameManager/HobbitGameManager.h"
#include "HobbitGameManager/HobbitProcessAnalyzer.h"

#undef SetPort
#undef SendMessage

#include <conio.h>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <atomic>
#include <vector>

#include "cutils.h"

#include "Hoistable.h"


#include <Windows.h>
#include <string>

#include "ChatOverlay.h"
#include "meridian.hpp"        // bilbo class + existing hooks (still needed by the rest of this file)
#include "minhook/MinHook.h"   // CreateStoneProjectile detour (MinHook is compiled into the project)

using namespace yojimbo;

// ===========================================================================
//  Globals
// ===========================================================================

HMODULE moduleInstance = nullptr;

static volatile int quit = 0;


static int isHost = 0;
static bool processedDataForThisLevel = false;

static void interruptHandler(int) { quit = 1; }


// --- Game process interface ---
static HobbitGameManager       gameManager;
static HobbitProcessAnalyzer* processAnalyzer = nullptr;

// --- Cached pointers into game memory ---
static uint32_t bilboPosBasePtr = 0;
static uint32_t bilboAnimPtr = 0;

// --- Local player state ---
static uint64_t myGuid = 0;
static Vector3  localPos;
static Vector3  localRot;
static uint32_t localAnimation = 0;
static float    localAnimFrame = 0.0f;
static float    localLastAnimFrame = 0.0f;

static uint32_t bilboWeapon;
static uint32_t nowLevel;
static bool levelIsRunning;

static uint64_t myNicknameGuid = 0;
static uint64_t myStatusGuid = 0;

static std::string myNickname = "Username";
static std::string myStatus = "Status";
static uint16_t myFakeBilboDamage = 10;

static constexpr uint32_t FakeBilboDamageAddress = 0x00572D8E;

// --- Remote players ---
CRITICAL_SECTION playersCriticalSection;
static std::vector<Player> activePlayers;

static std::vector<uint64_t> playerGuids;
static std::unordered_map<uint64_t, std::string> playerTextureNames;
static std::unordered_map<uint64_t, std::string> playerSkinFilePaths;

static SkinSync::LocalSkinDefinition localSkinDefinition;
static bool localSkinLoaded = false;
static bool localSkinUploadAttempted = false;
static std::string localSkinConfigPath;
static std::string localSkinConfigError;
static std::unordered_map<uint64_t, std::string> pendingNicknames;
static std::unordered_map<uint64_t, std::string> pendingStatuses;

// --- Animation data caches ---
std::unordered_map<uint32_t, uint32_t> animDataMap;
static std::unordered_map<uint32_t, float> animFrameRanges;

CRITICAL_SECTION enemiesCriticalSection;
std::unordered_map<uint64_t, Enemy> enemy_updates;
bool enemies_updated = false;

std::unordered_map<uint64_t, NPC*> enemies;
const uint32_t X_POSITION_PTR = 0x0075BA3C; // address of bilbo *g_pBilbo variable

static int g_enemies_ai_mode = 0;

struct HoistableUpdateStruct
{
	Hoistable* pObject;
	float x, y, z;
	float yaw;
	bool updated = false;
};

CRITICAL_SECTION hoistablesCriticalSection;
std::unordered_map<uint64_t, HoistableUpdateStruct> hoistables; // cache
static Hoistable* g_currentHoistable = nullptr;
static Hoistable* g_currentPushBlock = nullptr;

// --- Web walls sync (hook-based) ---
static CRITICAL_SECTION webWallBreak_CS;
static bool g_haveWebWallBreak = false;
static uint64_t g_lastWebWallGuid = 0;
static Vector3 g_lastWebWallBreakPoint{};
static std::unordered_set<uint64_t> g_sentWebWallBreaks;
static std::vector<uint32_t> allChests;

// --- Per-type object address caches (populated once per level in readGamePointers) ---
// Scanning the obj stack every frame for every type caused noticeable jitter; instead we
// snapshot addresses when the level loads and only re-scan on level change. The address
// itself stays valid for the whole level (the engine doesn't move live objects). Objects
// that get destroyed mid-level are filtered at use-time by validating the type tag byte,
// so a stale cache entry is harmless.
static std::vector<uint32_t> g_allPickupsAddrs;
static std::vector<uint32_t> g_allSwitchesAddrs;
static std::vector<uint32_t> g_allTriggersAddrs;

struct PendingWebWallBreak { uint64_t guid; Vector3 point; };
static CRITICAL_SECTION webWallBreakIncoming_CS;
static std::vector<PendingWebWallBreak> g_incomingWebWallBreaks;
static std::unordered_set<uint64_t> g_appliedWebWallBreaks;

// --- stone-throw networking (cross-thread: hook + OnAdvanceLogic = game thread, net loop = net thread) ---
CRITICAL_SECTION throwCriticalSection;

// outgoing: set by the CreateStoneProjectile hook when the LOCAL player throws
volatile bool    g_haveThrow = false;
Vector3          g_lastThrowFrom{};
Vector3          g_lastThrowTo{};
uint8_t          g_lastThrowType = 0x19;

// incoming: remote throws queued by the net thread, spawned on the game thread
struct PendingThrow { uint64_t guid; Vector3 from; Vector3 to; uint8_t type; };
static std::vector<PendingThrow> g_incomingThrows;

CRITICAL_SECTION chestCriticalSection;
static std::vector<uint64_t> g_outgoingChestOpens;
static std::vector<uint64_t> g_incomingChestOpens;
static bool g_suppressChestOpenHook = false;

// --- Pickup sync state ---
CRITICAL_SECTION pickupCriticalSection;
static std::unordered_map<uint64_t, uint16_t> pickupStatesCache;
static std::vector<uint64_t> g_outgoingPickupCollects;
static std::vector<uint64_t> g_incomingPickupCollects;

// --- Trigger sync state ---
CRITICAL_SECTION triggerCriticalSection;
static std::unordered_map<uint64_t, uint32_t> triggerStatesCache;
static std::unordered_map<uint64_t, int32_t> triggerSuppliedItemCountCache;
static std::vector<uint64_t> g_outgoingTriggerPressB;
static std::vector<uint64_t> g_incomingTriggerPressB;

struct PendingTriggerOnUse { uint64_t triggerGuid; int32_t itemCount; int32_t itemIds[4]; };
static std::vector<PendingTriggerOnUse> g_outgoingTriggerOnUse;
static std::vector<PendingTriggerOnUse> g_incomingTriggerOnUse;
// Triggers we applied remotely this frame — suppress re-detection to avoid feedback loops
static std::unordered_set<uint64_t> g_suppressTriggerDetection;

// --- Switch sync state ---
CRITICAL_SECTION switchCriticalSection;
static std::unordered_map<uint64_t, uint32_t> switchStatesCache;
static std::vector<std::pair<uint64_t, bool>> g_outgoingSwitchToggles;
static std::vector<std::pair<uint64_t, bool>> g_incomingSwitchToggles;

// Forward declarations for sync apply functions (defined later, called from OnAdvanceLogic)
static void applyQueuedChestOpens();
static void applyQueuedPickupCollects();
static void applyQueuedTriggerPressB();
static void applyQueuedTriggerOnUse();
static void applyQueuedSwitchToggles();
static void applyQueuedAnimSync();
static void detectLocalRingChange(void* bilboThis);
static void applyPlayerRingVisuals();
static void processLocalSpawnRequest();
static void applyQueuedSpawns();
static void applyQueuedFx();

// Projectile::CreateProjectile(eProjectileType, owner, vector3 from, vector3 to) @ 0x004B53A0
// type 0x19 = "Normal Rock" (Bilbo's native stone). Spawning from code avoids the
// audio-event path that crashes; from/to are 12-byte structs passed by value (__cdecl).
typedef uint64_t(__cdecl* CreateProjectile_t)(uint32_t type, uint64_t owner, Vector3 from, Vector3 to);
static const CreateProjectile_t game_CreateProjectile = reinterpret_cast<CreateProjectile_t>(0x004B53A0);

static double g_time = NetDefaults::INITIAL_TIME;

typedef void (bilbo::* pOnAdvanceLogic_t)(float fDeltaTime);
pOnAdvanceLogic_t pOnAdvanceLogic_orig;

class hook_bilbo
{
public:
	void OnAdvanceLogic(float fDeltaTime);
};

void hook_bilbo::OnAdvanceLogic(float fDeltaTime)
{
	bilbo* pBilbo = (bilbo*)this;
	(pBilbo->*pOnAdvanceLogic_orig)(fDeltaTime);

	// spawn any remote players' thrown stones (must run on the game thread)
	EnterCriticalSection(&throwCriticalSection);
	for (const PendingThrow& p : g_incomingThrows)
	{
		game_CreateProjectile(p.type, p.guid, p.from, p.to);
	}
	g_incomingThrows.clear();
	LeaveCriticalSection(&throwCriticalSection);

	applyQueuedChestOpens();
	applyQueuedPickupCollects();
	applyQueuedTriggerPressB();
	applyQueuedTriggerOnUse();
	applyQueuedSwitchToggles();
	applyQueuedAnimSync();

	// IMPORTANT: this is a vtable hook, so OnAdvanceLogic also fires for remote
	// players' fake-bilbo instances every frame. Run our per-frame sync logic ONLY
	// for the LOCAL player's bilbo — otherwise detectLocalRingChange would read a
	// different fake-bilbo's ring flag each call, thrash the state, and flood the
	// reliable channel with RING_SYNC (which desyncs it and disconnects clients).
	bilbo* localBilbo = *((bilbo**)X_POSITION_PTR);
	if (pBilbo == localBilbo)
	{
		// Ring stealth: detect our own ring toggle to broadcast, then re-apply the
		// stealth look to every remote player's fake-bilbo NPC.
		detectLocalRingChange(pBilbo);
		applyPlayerRingVisuals();

		// Object spawning: run our own /spawn request, then apply remote spawns.
		processLocalSpawnRequest();
		applyQueuedSpawns();

		// FX: play any queued local/remote fire-and-forget effects.
		applyQueuedFx();
	}

	// update hoistables and pushblock position
	EnterCriticalSection(&hoistablesCriticalSection);

	for (auto it = hoistables.begin(); it != hoistables.end(); it++) {
		HoistableUpdateStruct& upd = it->second;
		if (upd.updated) {
			if (upd.pObject->objectClass() == CLASS_PushBox) {
				upd.pObject->xSetPosition(upd.x, upd.y, upd.z);
				printf("UPDATE PUHSHBOX %.2f  %.2f  %.2f\n", upd.x, upd.y, upd.z);
			}
			else
				upd.pObject->setPosition(upd.x, upd.y, upd.z);
			upd.pObject->setRotationY(upd.yaw);
			upd.updated = false;
		}
	}

	LeaveCriticalSection(&hoistablesCriticalSection);

	/* update enemies */
	EnterCriticalSection(&enemiesCriticalSection);

	if (enemies_updated) {
		for (auto enemyUpdate : enemy_updates)
		{
			const auto enemyIt = enemies.find(enemyUpdate.first);
			if (enemyIt == enemies.end() || enemyIt->second == nullptr || !enemyIt->second->isValid())
				continue;

			NPC* badBoy = enemyIt->second;

			if (!badBoy->isActivated())
				continue;

			badBoy->setPosition(enemyUpdate.second.x, enemyUpdate.second.y, enemyUpdate.second.z);
			badBoy->setRotationY(enemyUpdate.second.rot);
			badBoy->setHealth(enemyUpdate.second.health);
			if (badBoy->getAnimation() != enemyUpdate.second.anim)
				badBoy->setNPCAnim(enemyUpdate.second.anim);

		}
		enemies_updated = false;
	}

	LeaveCriticalSection(&enemiesCriticalSection);

	// Interpolate all remote players
	EnterCriticalSection(&playersCriticalSection);

	for (auto& player : activePlayers)
	{
		// --- Animation ---
		if (player.animation == player.targetAnimation)
		{/*
			// Same animation — smoothly lerp frames
			float range = getClampedFrameRange(player.animation, msg->lastAnimFrame);

			player.prevAnimFrame = player.animFrame;

			if (range > 0.0f)
			{
				float prevWrapped = fmod(player.prevAnimFrame, range);
				float delta = msg->animFrame - prevWrapped;
				if (delta < -range / 2.0f)
					delta += range;
				player.targetAnimFrame = player.prevAnimFrame + delta;
			}
			else
			{
				player.targetAnimFrame = msg->animFrame;
			}

			player.prevLastAnimFrame = player.lastAnimFrame;
			player.targetLastAnimFrame = range;*/
		}
		else
		{/*
			// Different animation — snap immediately
			float range = getClampedFrameRange(player.animation, msg->lastAnimFrame);

			player.animation = msg->animation;
			player.animFrame = msg->animFrame;
			player.lastAnimFrame = range;
			player.prevAnimFrame = msg->animFrame;
			player.targetAnimFrame = msg->animFrame;
			player.prevLastAnimFrame = player.lastAnimFrame;
			player.targetLastAnimFrame = player.lastAnimFrame;
*/
// Apply new animation to NPC right away
			player.setPlayerAnim(player.targetAnimation);
		}

		double elapsed = g_time - player.lerpStartTime;
		float t = static_cast<float>(elapsed / NetDefaults::LERP_DURATION);
		if (t > 1.0f) t = 1.0f;
		player.tickLerp(t);
	}

	LeaveCriticalSection(&playersCriticalSection);

	// Apply incoming remote web wall breaks on the game thread
	{
		EnterCriticalSection(&webWallBreakIncoming_CS);
		std::vector<PendingWebWallBreak> pending = g_incomingWebWallBreaks;
		g_incomingWebWallBreaks.clear();
		LeaveCriticalSection(&webWallBreakIncoming_CS);

		for (const PendingWebWallBreak& wb : pending)
		{
			if (!processAnalyzer)
				continue;

			// Skip if already applied or already sent (prevents feedback loop:
			// applying calls StartBreakAtPoint which triggers our hook)
			if (g_appliedWebWallBreaks.count(wb.guid))
				continue;

			// Mark as sent BEFORE calling StartBreakAtPoint so our own hook
			// doesn't re-queue it for the network
			g_appliedWebWallBreaks.insert(wb.guid);

			uint32_t objAddr = processAnalyzer->findGameObjByGUID(wb.guid);
			if (objAddr == 0)
				continue;

			object* pObj = (object*)objAddr;
			if (pObj->_typeId() != CLASS_WebWall || !pObj->_isLoaded())
				continue;

			// Also mark as sent in the outgoing dedup set so the hook
			// won't try to re-broadcast this break
			EnterCriticalSection(&webWallBreak_CS);
			g_sentWebWallBreaks.insert(wb.guid);
			LeaveCriticalSection(&webWallBreak_CS);

			typedef void (object::* pweb_wall_StartBreakAtPoint)(const vector3& Point);
			pweb_wall_StartBreakAtPoint fn;
			unsigned addr = 0x004ef370;
			memcpy(&fn, &addr, 4);
			(pObj->*fn)(reinterpret_cast<const vector3&>(wb.point));

			printf("Applied remote web wall break: GUID %llu\n", wb.guid);
		}
	}
}

void InstallStoneHook();   // forward decl (defined further below)
void InstallChestHook();   // forward decl (defined further below)
void InstallWebWallHook(); // forward decl (defined further below)

void SetupBilboHook(void)
{
	LPDWORD pAddressInVMT = LPDWORD(0x006e9828);

	LPVOID pOriginal = LPVOID(0x0041e360);
	void (hook_bilbo:: * pDetour_)(float fDeltaTime) = &hook_bilbo::OnAdvanceLogic;
	DWORD pDetour;

	memcpy(&pDetour, &pDetour_, 4);
	memcpy(&pOnAdvanceLogic_orig, &pOriginal, 4);

	DWORD oldProtect;
	VirtualProtect(pAddressInVMT, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
	*pAddressInVMT = pDetour;
	VirtualProtect(pAddressInVMT, 4, oldProtect, &oldProtect);

	InstallStoneHook();   // detour bilbo::CreateStoneProjectile to capture throw destination
	InstallChestHook();   // detour treasure_chest::Open to replicate opened chests
	InstallWebWallHook(); // detour web_wall::StartBreakAtPoint to capture web cuts
}

static std::string getModuleDirectory(HMODULE module)
{
	if (!module && module != nullptr)
		return {};

	char pathBuffer[MAX_PATH] = {};
	DWORD pathLength = GetModuleFileNameA(module, pathBuffer, MAX_PATH);
	if (pathLength == 0 || pathLength >= MAX_PATH)
		return {};

	return SkinSync::fs::path(pathBuffer).parent_path().string();
}

static void appendUniqueConfigCandidate(std::vector<std::string>& candidates, const SkinSync::fs::path& path)
{
	if (path.empty())
		return;

	std::error_code error;
	const std::string normalized = SkinSync::fs::absolute(path, error).string();
	const std::string value = normalized.empty() ? path.string() : normalized;

	for (const auto& existing : candidates)
	{
		if (existing == value)
			return;
	}

	candidates.push_back(value);
}

static bool loadLocalSkinDefinitionFromKnownPaths(SkinSync::LocalSkinDefinition& outSkin, std::string& loadedPath, std::string& errorMessage)
{
	std::vector<std::string> candidates;
	appendUniqueConfigCandidate(candidates, SkinSync::fs::path(SkinSync::LocalSkinConfigFile));

	const std::string exeDirectory = getModuleDirectory(nullptr);
	if (!exeDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(exeDirectory) / SkinSync::LocalSkinConfigFile);

	const std::string dllDirectory = getModuleDirectory(moduleInstance);
	if (!dllDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(dllDirectory) / SkinSync::LocalSkinConfigFile);

	std::string firstDetailedError;

	for (const auto& candidate : candidates)
	{
		std::string candidateError;
		if (SkinSync::loadLocalSkinDefinition(outSkin, candidate, &candidateError))
		{
			loadedPath = candidate;
			errorMessage.clear();
			return true;
		}

		std::error_code error;
		if (SkinSync::fs::exists(SkinSync::fs::path(candidate), error) && firstDetailedError.empty())
			firstDetailedError = candidateError;
	}

	loadedPath.clear();
	if (!firstDetailedError.empty())
	{
		errorMessage = firstDetailedError;
	}
	else
	{
		errorMessage = "skin_config.txt was not found in the current folder, game exe folder, or DLL folder";
	}

	return false;
}

static std::string resolveLocalProfileConfigPath()
{
	if (!localSkinConfigPath.empty())
		return localSkinConfigPath;

	std::vector<std::string> candidates;
	appendUniqueConfigCandidate(candidates, SkinSync::fs::path(SkinSync::LocalSkinConfigFile));

	const std::string exeDirectory = getModuleDirectory(nullptr);
	if (!exeDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(exeDirectory) / SkinSync::LocalSkinConfigFile);

	const std::string dllDirectory = getModuleDirectory(moduleInstance);
	if (!dllDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(dllDirectory) / SkinSync::LocalSkinConfigFile);

	for (const auto& candidate : candidates)
	{
		std::error_code error;
		if (SkinSync::fs::exists(SkinSync::fs::path(candidate), error))
			return candidate;
	}

	return candidates.empty() ? std::string(SkinSync::LocalSkinConfigFile) : candidates.front();
}

static std::string sanitizeIdentityValue(const std::string& value, size_t maxLength)
{
	std::string result;
	result.reserve(value.size());
	for (unsigned char ch : value)
	{
		if (ch >= 32 && ch <= 126)
			result.push_back(static_cast<char>(ch));
	}

	if (maxLength > 0 && result.size() >= maxLength)
		result.resize(maxLength - 1);

	return result;
}

static bool parseBoundedInt(const std::string& value, int minValue, int maxValue, int& result)
{
	std::string clean = SkinSync::trim(value);
	if (clean.empty())
		return false;

	char* end = nullptr;
	errno = 0;
	long parsed = std::strtol(clean.c_str(), &end, 10);
	if (errno == ERANGE || end == clean.c_str() || *end != '\0')
		return false;
	if (parsed < minValue || parsed > maxValue)
		return false;

	result = static_cast<int>(parsed);
	return true;
}

static uint16_t sanitizeDamageValue(int value)
{
	if (value < 0)
		return 0;
	if (value > 65535)
		return 65535;
	return static_cast<uint16_t>(value);
}

static uint32_t resolveLocalWeaponId(uint32_t weaponValue)
{
	if (NetworkClamp::sanitizeWeapon(weaponValue) == weaponValue)
		return weaponValue;

	struct WeaponMapping
	{
		uint32_t id;
		uint64_t guid;
	};

	static constexpr WeaponMapping mappings[] =
	{
		{ 2u, 0x0D8AD910E885100Dull },
		{ 0u, 0x0D8AD910E885100Bull },
		{ 1u, 0x0D8AD910E885100Aull },
		{ 3u, 0x0D8AD910E885100Cull }
	};

	if (!processAnalyzer)
		return NetworkClamp::WeaponDefault;

	for (const WeaponMapping& mapping : mappings)
	{
		uint32_t weaponObject = processAnalyzer->findGameObjByGUID(mapping.guid);
		if (weaponObject && processAnalyzer->readData<uint32_t>(weaponObject + 0x260) == weaponValue)
			return mapping.id;
	}

	return NetworkClamp::WeaponDefault;
}

static void parseLocalProfileConfigLine(const std::string& line, std::string& nickname, std::string& status, uint16_t& damage)
{
	const size_t separator = line.find('=');
	if (separator == std::string::npos)
		return;

	std::string key = SkinSync::trim(line.substr(0, separator));
	std::string value = SkinSync::trim(line.substr(separator + 1));

	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
		});

	if (key == "name" || key == "nickname")
		nickname = sanitizeIdentityValue(value, sizeof(NicknameUpdateMessage::new_name));
	else if (key == "status")
		status = sanitizeIdentityValue(value, sizeof(StatusUpdateMessage::new_status));
	else if (key == "damage" || key == "fake_bilbo_damage")
	{
		int parsedDamage = 0;
		if (parseBoundedInt(value, 0, 65535, parsedDamage))
			damage = sanitizeDamageValue(parsedDamage);
	}
}

static bool loadLocalPlayerProfile(std::string& nickname, std::string& status, uint16_t& damage, std::string* errorMessage = nullptr)
{
	nickname = "Username";
	status = "Status";
	damage = 10;

	const std::string configPath = resolveLocalProfileConfigPath();
	std::ifstream configFile(configPath);
	if (!configFile.is_open())
	{
		if (errorMessage)
			errorMessage->clear();
		return false;
	}

	std::string line;
	while (std::getline(configFile, line))
	{
		line = SkinSync::trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;

		parseLocalProfileConfigLine(line, nickname, status, damage);
	}

	if (errorMessage)
		errorMessage->clear();
	return true;
}

static bool saveLocalPlayerProfile(const std::string& nickname, const std::string& status, uint16_t damage, std::string* errorMessage = nullptr)
{
	const std::string configPath = resolveLocalProfileConfigPath();
	std::vector<std::string> lines;
	std::ifstream input(configPath);
	if (input.is_open())
	{
		std::string line;
		while (std::getline(input, line))
		{
			std::string trimmed = SkinSync::trim(line);
			if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed.find('=') == std::string::npos)
			{
				lines.push_back(line);
				continue;
			}

			std::string key = SkinSync::trim(trimmed.substr(0, trimmed.find('=')));
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
				});

			if (key == "name" || key == "nickname")
				continue;
			if (key == "status")
				continue;
			if (key == "damage" || key == "fake_bilbo_damage")
				continue;

			lines.push_back(line);
		}
	}

	lines.push_back("name=" + sanitizeIdentityValue(nickname, sizeof(NicknameUpdateMessage::new_name)));
	lines.push_back("status=" + sanitizeIdentityValue(status, sizeof(StatusUpdateMessage::new_status)));
	lines.push_back("damage=" + std::to_string(damage));

	std::ofstream output(configPath, std::ios::trunc);
	if (!output.is_open())
	{
		if (errorMessage)
			*errorMessage = "could not write profile config: " + configPath;
		return false;
	}

	for (const std::string& line : lines)
		output << line << "\n";

	if (errorMessage)
		errorMessage->clear();
	return output.good();
}

static void clearActivePlayers()
{
	EnterCriticalSection(&playersCriticalSection);
	for (auto& p : activePlayers)
	{
		if (p.npc)
		{
			delete p.npc;
			p.npc = nullptr;
		}
		if (p.nickname_marker)
		{
			delete p.nickname_marker;
			p.nickname_marker = nullptr;
		}
		if (p.status_marker)
		{
			delete p.status_marker;
			p.status_marker = nullptr;
		}
	}
	activePlayers.clear();
	LeaveCriticalSection(&playersCriticalSection);
}

static void clearEnemies()
{
	EnterCriticalSection(&enemiesCriticalSection);
	for (auto& pair : enemies)
	{
		if (pair.second)
		{
			delete pair.second;
		}
	}
	enemies.clear();
	LeaveCriticalSection(&enemiesCriticalSection);
}

static void resetClientSessionState()
{
	clearActivePlayers();
	clearEnemies();
	if (g_currentHoistable)
	{
		delete g_currentHoistable;
		g_currentHoistable = nullptr;
	}
	if (g_currentPushBlock)
	{
		delete g_currentPushBlock;
		g_currentPushBlock = nullptr;
	}

	EnterCriticalSection(&webWallBreak_CS);
	g_haveWebWallBreak = false;
	g_lastWebWallGuid = 0;
	g_lastWebWallBreakPoint = {};
	g_sentWebWallBreaks.clear();
	LeaveCriticalSection(&webWallBreak_CS);

	EnterCriticalSection(&webWallBreakIncoming_CS);
	g_incomingWebWallBreaks.clear();
	g_appliedWebWallBreaks.clear();
	LeaveCriticalSection(&webWallBreakIncoming_CS);

	EnterCriticalSection(&chestCriticalSection);
	allChests.clear();
	g_outgoingChestOpens.clear();
	g_incomingChestOpens.clear();
	LeaveCriticalSection(&chestCriticalSection);

	EnterCriticalSection(&pickupCriticalSection);
	pickupStatesCache.clear();
	g_outgoingPickupCollects.clear();
	g_incomingPickupCollects.clear();
	LeaveCriticalSection(&pickupCriticalSection);

	EnterCriticalSection(&triggerCriticalSection);
	triggerStatesCache.clear();
	triggerSuppliedItemCountCache.clear();
	g_outgoingTriggerPressB.clear();
	g_incomingTriggerPressB.clear();
	g_outgoingTriggerOnUse.clear();
	g_incomingTriggerOnUse.clear();
	g_suppressTriggerDetection.clear();
	LeaveCriticalSection(&triggerCriticalSection);

	EnterCriticalSection(&switchCriticalSection);
	switchStatesCache.clear();
	g_outgoingSwitchToggles.clear();
	g_incomingSwitchToggles.clear();
	LeaveCriticalSection(&switchCriticalSection);

	myGuid = 0;
	myNicknameGuid = 0;
	myStatusGuid = 0;
	localSkinUploadAttempted = false;
	processedDataForThisLevel = false;
	pendingNicknames.clear();
	pendingStatuses.clear();
}

static void applyFakeBilboDamage()
{
	if (!processAnalyzer)
		return;

	processAnalyzer->writeData<uint16_t>(FakeBilboDamageAddress, myFakeBilboDamage);
}

// ===========================================================================
//  Game Memory Reading
// ===========================================================================

static void readGamePointers()
{
	clearActivePlayers();
	nowLevel = processAnalyzer->readData<uint32_t>(0x00762B5C);


	bilboPosBasePtr = processAnalyzer->readData<uint32_t>(X_POSITION_PTR);
	bilboAnimPtr = 0x8 + processAnalyzer->readData<uint32_t>(
		0x560 + processAnalyzer->readData<uint32_t>(X_POSITION_PTR));

	//Enemies 
	clearEnemies();
	printf("List Enemies\n");

	//
	hoistables.clear();

	std::vector<uint32_t> allFriendAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000100000001, 0x184 + 0x8 * 0x4); //put the values that indicate that thing
	std::vector<uint32_t> allEnemieAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000200000002, 0x184 + 0x8 * 0x4); //put the values that indicate that thing
	std::vector<uint32_t> allRigidInstances = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x05, 0x7c); //put the values that indicate that thing
	size_t chestCount = 0;
	EnterCriticalSection(&chestCriticalSection);
	allChests = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x24, 0x7c); //put the values that indicate that thing
	chestCount = allChests.size();
	LeaveCriticalSection(&chestCriticalSection);
	std::vector<uint32_t> allPickups = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x22, 0x7c); //put the values that indicate that thing
	std::vector<uint32_t> allSwitches = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x33, 0x7c); //put the values that indicate that thing
	std::vector<uint32_t> allTriggers = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x35, 0x7c); //put the values that indicate that thing

	// Snapshot per-type address caches for this level. The per-frame detect* functions
	// iterate these cached lists instead of re-scanning the obj stack every frame,
	// which was causing noticeable jitter. Addresses are validated at use-time against
	// the type tag, so stale entries (destroyed mid-level objects) are skipped safely.
	g_allPickupsAddrs = allPickups;
	g_allSwitchesAddrs = allSwitches;
	g_allTriggersAddrs = allTriggers;


	std::cout << "SIZE OF ALL RIGID: " << allRigidInstances.size() << '\n';

	std::cout << "CHESTS AMOUNT: " << chestCount << "\n";

	for (uint32_t fr : allFriendAddrs) allEnemieAddrs.push_back(fr);

	for (uint32_t e : allEnemieAddrs)
	{
		uint32_t value = processAnalyzer->readData<uint32_t>(e + 0x10);
		if (0x04004232 != value && 0x004A14C0 != value)
			continue;

		uint64_t eGuid = processAnalyzer->readData<uint64_t>(e + 0x8);

		bool skip = false;
		for (uint64_t pGuid : playerGuids) if (pGuid == eGuid) skip = true;


		if (skip) continue;

		if (0xABCABCABCABCABC0 == eGuid)
		{
			printf("YOU ARE SETTING BILBO AS ENEMY NPC!!!");
			continue;
		}

		//hex
		std::cout << eGuid << " Address: " << e << " Health: " << processAnalyzer->readData<float>(e + 0x290) << '\n';

		NPC* enemy = new NPC(processAnalyzer);
		enemy->initializeByAddress(e);
		if (isHost == 0) enemy->setAIMode(0);

		enemies.emplace(enemy->getGUID(), enemy);
	}

	printf("Enemies Found: %d", enemies.size());
	applyFakeBilboDamage();
}

static Vector3 getBilboPos(void)
{
	Vector3 pos;
	pos.x = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C4);
	pos.y = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C8);
	pos.z = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7CC);
	return pos;
}


// --- capture the player's stone-throw destination (no SDK needed) -----------
// bilbo::CreateStoneProjectile(this, const vector3& from, const vector3& to) @ 0x00424C30
// __thiscall: this in ECX; from/to are vector3* on the stack. We hook it as
// __fastcall so ECX maps to `self`, the unused EDX is a filler, then from/to.
// The game's vector3 is just 3 contiguous floats, so our Vector3 maps 1:1.

// (g_haveThrow / g_lastThrowFrom / g_lastThrowTo are declared up top with the
//  rest of the throw-networking state.)

typedef void(__fastcall* CreateStoneProjectile_t)(
	void* self, void* edx, const Vector3* from, const Vector3* to);
static CreateStoneProjectile_t oCreateStoneProjectile = nullptr;

static void __fastcall hkCreateStoneProjectile(
	void* self, void* edx, const Vector3* from, const Vector3* to)
{
	// Determine stone type from the local player's globals at throw time
	float Fire_Stone = *reinterpret_cast<float*>(0x0075BE88);
	float Explosive_Stone = *reinterpret_cast<float*>(0x0075BE84);
	float Freeze_Stone = *reinterpret_cast<float*>(0x0075BE8C);

	uint8_t type = 0x19; // Normal Rock
	if (Fire_Stone > 0.0f)
		type = 0x1A;
	else if (Explosive_Stone > 0.0f)
		type = 0x1B;
	else if (Freeze_Stone > 0.0f)
		type = 0x1C;

	EnterCriticalSection(&throwCriticalSection);
	g_lastThrowFrom = *from;
	g_lastThrowTo = *to;
	g_lastThrowType = type;
	g_haveThrow = true;
	LeaveCriticalSection(&throwCriticalSection);

	printf("[stone] type 0x%02X from (%.1f, %.1f, %.1f)  ->  TO (%.1f, %.1f, %.1f)\n",
		type, from->x, from->y, from->z, to->x, to->y, to->z);
	oCreateStoneProjectile(self, edx, from, to);
}

void InstallStoneHook()
{
	MH_Initialize();                  // harmless if MinHook is already initialized
	void* target = reinterpret_cast<void*>(0x00424C30);   // raw address (ASLR off)
	MH_CreateHook(target, &hkCreateStoneProjectile,
		reinterpret_cast<void**>(&oCreateStoneProjectile));
	MH_EnableHook(target);
}

static bool isChestOpened(uint32_t chestAddress)
{
	return chestAddress != 0 && (processAnalyzer->readData<uint8_t>(chestAddress + 0x7B) & 0x08) != 0;
}

static uint32_t findChestAddressByGuid(uint64_t chestGuid)
{
	EnterCriticalSection(&chestCriticalSection);
	std::vector<uint32_t> chests = allChests;
	LeaveCriticalSection(&chestCriticalSection);

	for (uint32_t chestAddress : chests)
	{
		if (chestAddress != 0 && processAnalyzer->readData<uint64_t>(chestAddress + 0x8) == chestGuid)
			return chestAddress;
	}

	return processAnalyzer->findGameObjByGUID(chestGuid);
}

typedef void(__thiscall* ChestOpen_t)(void* self);
static ChestOpen_t oChestOpen = nullptr;

static void __fastcall hkChestOpen(void* self, void* edx)
{
	(void)edx;

	uint64_t chestGuid = 0;

	if (self)
	{
		uint32_t chestAddress = reinterpret_cast<uint32_t>(self);
		chestGuid = processAnalyzer ? processAnalyzer->readData<uint64_t>(chestAddress + 0x8) : 0;
	}

	oChestOpen(self);

	if (!g_suppressChestOpenHook && chestGuid != 0)
	{
		EnterCriticalSection(&chestCriticalSection);
		g_outgoingChestOpens.push_back(chestGuid);
		LeaveCriticalSection(&chestCriticalSection);
	}
}

void InstallChestHook()
{
	MH_Initialize();
	void* target = reinterpret_cast<void*>(0x004D5BD0);   // treasure_chest::Open
	MH_CreateHook(target, &hkChestOpen,
		reinterpret_cast<void**>(&oChestOpen));
	MH_EnableHook(target);
}

static void applyQueuedChestOpens()
{
	if (!oChestOpen || !processAnalyzer)
		return;

	EnterCriticalSection(&chestCriticalSection);
	std::vector<uint64_t> pending = g_incomingChestOpens;
	g_incomingChestOpens.clear();
	LeaveCriticalSection(&chestCriticalSection);

	for (uint64_t chestGuid : pending)
	{
		uint32_t chestAddress = findChestAddressByGuid(chestGuid);
		if (chestAddress == 0 || isChestOpened(chestAddress))
			continue;

		g_suppressChestOpenHook = true;
		oChestOpen(reinterpret_cast<void*>(chestAddress));
		g_suppressChestOpenHook = false;
	}
}

static void readLocalPlayerState()
{
	localPos = getBilboPos();
	localRot.y = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7AC);

	localAnimation = processAnalyzer->readData<uint32_t>(bilboAnimPtr);
	if (!(localAnimation >= 0 && localAnimation <= 200))
		localAnimation = 1;



	// Cache animation data pointer
	uint32_t animData = processAnalyzer->readData<uint32_t>(bilboAnimPtr + 0x4);
	animDataMap[localAnimation] = animData;

	// Read animation frame progress
	uint32_t baseAddr = processAnalyzer->readData<uint32_t>(0x0075BA3C);
	localAnimFrame = processAnalyzer->readData<float>(baseAddr + 0x530);
	localLastAnimFrame = processAnalyzer->readData<float>(baseAddr + 0x53C);

	// Read weapon and convert the live game pointer value back to the network weapon id.
	uint32_t rawBilboWeapon = processAnalyzer->readData<uint32_t>(0x0075C738);
	bilboWeapon = resolveLocalWeaponId(rawBilboWeapon);
	nowLevel = processAnalyzer->readData<uint32_t>(0x00762B5C);

	// Track max frame range per animation
	if (animFrameRanges.find(localAnimation) == animFrameRanges.end()
		|| localLastAnimFrame > animFrameRanges[localAnimation])
	{
		animFrameRanges[localAnimation] = localLastAnimFrame;
	}
}

std::unordered_map<uint64_t, Enemy> readEnemiesState()
{
	std::unordered_map<uint64_t, Enemy> temp;

	for (auto enemy : enemies)
	{

		Vector3 ePos = enemy.second->getPosition();
		float eRot = enemy.second->getRotationY();
		uint32_t eAnim = enemy.second->getAnimation();
		float eHealth = enemy.second->getHealth();

		temp[enemy.first] = NetworkClamp::sanitizeEnemy({ ePos.x, ePos.y, ePos.z, eRot, eAnim, eHealth });
	}

	return temp;
}

static void changeEnemiesAIMode(int mode)
{
	for (auto enemy : enemies)
	{
		enemy.second->setAIMode(mode);
	}
}

// ===========================================================================
//  Web Walls Synchronization (hook-based)
// ===========================================================================

// web_wall::StartBreakAtPoint(this, const vector3& Point) @ 0x004EF370
// Hooked to detect local web cuts and send them to the network.
// The this pointer is the web_wall object; its GUID lives at +0x08.

typedef void(__fastcall* StartBreakAtPoint_t)(void* self, void* edx, const vector3& point);
static StartBreakAtPoint_t oStartBreakAtPoint = nullptr;

static void __fastcall hkStartBreakAtPoint(void* self, void* edx, const vector3& point)
{
	uint64_t guid = *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(self) + 0x08);
	if (guid != 0)
	{
		EnterCriticalSection(&webWallBreak_CS);
		if (!g_sentWebWallBreaks.count(guid))
		{
			g_sentWebWallBreaks.insert(guid);
			g_lastWebWallGuid = guid;
			g_lastWebWallBreakPoint = { point.X, point.Y, point.Z };
			g_haveWebWallBreak = true;

			printf("Local web wall cut: GUID %llu at (%.1f, %.1f, %.1f)\n",
				guid, point.X, point.Y, point.Z);
		}
		LeaveCriticalSection(&webWallBreak_CS);
	}

	oStartBreakAtPoint(self, edx, point);
}

static void sendWebWallBreak(Client& client)
{
	if (!client.IsConnected() || myGuid == 0)
		return;

	EnterCriticalSection(&webWallBreak_CS);
	if (!g_haveWebWallBreak)
	{
		LeaveCriticalSection(&webWallBreak_CS);
		return;
	}

	uint64_t guid = g_lastWebWallGuid;
	Vector3 point = g_lastWebWallBreakPoint;
	g_haveWebWallBreak = false;
	LeaveCriticalSection(&webWallBreak_CS);

	auto* msg = static_cast<WebWallBreakMessage*>(client.CreateMessage(WEB_WALL_BREAK));
	if (!msg)
		return;

	msg->wallGuid = guid;
	msg->breakX = point.x;
	msg->breakY = point.y;
	msg->breakZ = point.z;
	msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
	client.SendMessage(channels::Gameplay, msg);
}

void InstallWebWallHook()
{
	MH_Initialize();
	void* target = reinterpret_cast<void*>(0x004EF370);
	MH_CreateHook(target, &hkStartBreakAtPoint,
		reinterpret_cast<void**>(&oStartBreakAtPoint));
	MH_EnableHook(target);
}

// ===========================================================================
//  Pickup Synchronization
// ===========================================================================

// Pickup memory layout (from chests_sdk.h):
//   +0x008  GUID (uint64_t)
//   +0x07C  object type tag (uint8_t, 0x22 = pickup)
//   +0x07F  objectFlags (uint8_t, bit0 = visible)
//   +0x126  disabledFlags (uint8_t, bit0 = not-getable)
//   +0x16E  stateFlags (uint16_t, PICKUP_PICKED_UP = 0x0004, PICKUP_CHASING = 0x0100)
static constexpr uint8_t  PICKUP_CLASS_TAG = 0x22;
static constexpr uint16_t PICKUP_PICKED_UP_FLAG = 0x0004;

// bilbo::Pickups_GetPickup(object*) @ 0x00446330 — directly grants the pickup to
// inventory (no chase animation). Same as the "Bilbo GetPickup" button in kingjoyer.
static constexpr uint32_t BILBO_PICKUPS_GETPICKUP_ADDR = 0x00446330;
typedef void(__thiscall* BilboPickupsGetPickup_t)(void* self, void* pickupObj);
static const BilboPickupsGetPickup_t game_BilboPickupsGetPickup = reinterpret_cast<BilboPickupsGetPickup_t>(BILBO_PICKUPS_GETPICKUP_ADDR);

static bool isPickupObject(uint32_t objAddr)
{
	return objAddr != 0
		&& processAnalyzer->readData<uint8_t>(objAddr + 0x7C) == PICKUP_CLASS_TAG;
}

static uint64_t getPickupGuid(uint32_t objAddr)
{
	return processAnalyzer->readData<uint64_t>(objAddr + 0x08);
}

static uint16_t getPickupStateFlags(uint32_t objAddr)
{
	return processAnalyzer->readData<uint16_t>(objAddr + 0x16E);
}

static bool isPickupCollected(uint32_t objAddr)
{
	return (getPickupStateFlags(objAddr) & PICKUP_PICKED_UP_FLAG) != 0;
}

// Apply the visual hide to a pickup in the local game (flags-only, no SDK call).
static void hidePickupLocally(uint32_t objAddr)
{
	if (objAddr == 0)
		return;

	uint8_t objFlags = processAnalyzer->readData<uint8_t>(objAddr + 0x7F);
	uint8_t disFlags = processAnalyzer->readData<uint8_t>(objAddr + 0x126);
	uint16_t stFlags = processAnalyzer->readData<uint16_t>(objAddr + 0x16E);

	objFlags &= ~0x01;       // clear visible
	disFlags |= 0x01;        // set not-getable
	stFlags |= PICKUP_PICKED_UP_FLAG;

	processAnalyzer->writeData<uint8_t>(objAddr + 0x7F, objFlags);
	processAnalyzer->writeData<uint8_t>(objAddr + 0x126, disFlags);
	processAnalyzer->writeData<uint16_t>(objAddr + 0x16E, stFlags);
}

// Scan all pickups for state changes (newly collected).
// Runs on the game thread. Must be called after readGamePointers().
static void detectPickupChanges()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&pickupCriticalSection);

	for (uint32_t addr : g_allPickupsAddrs)
	{
		// Validate the cached address is still a live pickup (the engine may have
		// destroyed it mid-level; the cache only refreshes on level change).
		if (processAnalyzer->readData<uint8_t>(addr + 0x7C) != PICKUP_CLASS_TAG)
			continue;

		uint64_t guid = getPickupGuid(addr);
		if (guid == 0)
			continue;

		uint16_t currentState = getPickupStateFlags(addr);

		auto it = pickupStatesCache.find(guid);
		if (it == pickupStatesCache.end())
		{
			// First time seeing this pickup — record its initial state
			pickupStatesCache[guid] = currentState;
		}
		else if (it->second != currentState)
		{
			// State changed — check if it became collected
			if ((currentState & PICKUP_PICKED_UP_FLAG) && !(it->second & PICKUP_PICKED_UP_FLAG))
			{
				g_outgoingPickupCollects.push_back(guid);
				printf("Pickup collected locally: GUID %llu\n", guid);
			}
			pickupStatesCache[guid] = currentState;
		}
	}

	LeaveCriticalSection(&pickupCriticalSection);
}

// Send outgoing pickup collect messages
static void sendPickupCollects(Client& client)
{
	if (!client.IsConnected() || myGuid == 0)
		return;

	EnterCriticalSection(&pickupCriticalSection);
	std::vector<uint64_t> pending = g_outgoingPickupCollects;
	g_outgoingPickupCollects.clear();
	LeaveCriticalSection(&pickupCriticalSection);

	for (uint64_t pickupGuid : pending)
	{
		auto* msg = static_cast<PickupCollectMessage*>(client.CreateMessage(PICKUP_COLLECT));
		if (!msg)
			continue;

		msg->pickupGuid = pickupGuid;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Process incoming pickup collect message
static void processPickupCollect(PickupCollectMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->pickupGuid == 0)
		return;

	EnterCriticalSection(&pickupCriticalSection);
	g_incomingPickupCollects.push_back(msg->pickupGuid);
	LeaveCriticalSection(&pickupCriticalSection);
}

// Apply queued incoming pickup collects on the game thread.
// Uses Bilbo_Pickups_GetPickup to directly grant the item to inventory
// (no chase animation — same as the "Bilbo GetPickup" button in kingjoyer).
static void applyQueuedPickupCollects()
{
	if (!processAnalyzer)
		return;

	// Get the Bilbo pointer for the grant call
	uint32_t bilboPtr = processAnalyzer->readData<uint32_t>(0x0075BA3C);
	if (bilboPtr == 0)
		return;

	EnterCriticalSection(&pickupCriticalSection);
	std::vector<uint64_t> pending = g_incomingPickupCollects;
	g_incomingPickupCollects.clear();
	LeaveCriticalSection(&pickupCriticalSection);

	for (uint64_t pickupGuid : pending)
	{
		uint32_t addr = processAnalyzer->findGameObjByGUID(pickupGuid);
		if (addr == 0 || !isPickupObject(addr) || isPickupCollected(addr))
			continue;

		// Directly grant the pickup to inventory (no chase)
		game_BilboPickupsGetPickup(reinterpret_cast<void*>(bilboPtr), reinterpret_cast<void*>(addr));
		printf("Applied remote pickup collect: GUID %llu\n", pickupGuid);
	}
}

// ===========================================================================
//  Trigger Synchronization
// ===========================================================================

// Trigger memory layout (from triggers_sdk.h):
//   +0x008  GUID (uint64_t)
//   +0x07C  object type tag (uint8_t, 0x35 = trigger)
//   +0x120  flags (uint32_t, TRIGGER_WAS_TRIGGERED = 0x10, TRIGGER_ENABLED = 0x08)
//   +0x138  requiredItemCount (int32)
//   +0x13c  requiredItemList (int32*)
//   +0x160  suppliedItemCount (int32) — items placed by player
//   +0x164  suppliedItemList (int32*) — player-supplied item ids
//   trigger::OnPressB   @ 0x004DD2D0 (game address, no ASLR)
//   trigger::OnUse(item) @ 0x004DCEF0 (game address, no ASLR)
static constexpr uint8_t   TRIGGER_CLASS_TAG = 0x35;
static constexpr uint32_t  TRIGGER_WAS_TRIGGERED_FLAG = 0x00000010;
static constexpr uint32_t  TRIGGER_ENABLED_FLAG = 0x00000008;
static constexpr uint32_t  TRIGGER_ONPRESSB_ADDR = 0x004DD2D0;
static constexpr uint32_t  TRIGGER_ONUSE_ADDR = 0x004DCEF0;
static constexpr uint32_t  TRIGGER_USEITEMS_ADDR = 0x004DD060;
static constexpr uint32_t  TRIGGER_SUPPLIEDITEMCOUNT_OFFSET = 0x160;
static constexpr uint32_t  TRIGGER_SUPPLIEDITEMLIST_OFFSET = 0x164;

typedef void(__thiscall* TriggerOnPressB_t)(void* self);
static const TriggerOnPressB_t game_TriggerOnPressB = reinterpret_cast<TriggerOnPressB_t>(TRIGGER_ONPRESSB_ADDR);

typedef void(__thiscall* TriggerOnUse_t)(void* self, int item);
static const TriggerOnUse_t game_TriggerOnUse = reinterpret_cast<TriggerOnUse_t>(TRIGGER_ONUSE_ADDR);

typedef void(__thiscall* TriggerUseItems_t)(void* self);
static const TriggerUseItems_t game_TriggerUseItems = reinterpret_cast<TriggerUseItems_t>(TRIGGER_USEITEMS_ADDR);

static bool isTriggerObject(uint32_t objAddr)
{
	return objAddr != 0
		&& processAnalyzer->readData<uint8_t>(objAddr + 0x7C) == TRIGGER_CLASS_TAG;
}

static uint32_t getTriggerFlags(uint32_t objAddr)
{
	return processAnalyzer->readData<uint32_t>(objAddr + 0x120);
}

static bool isTriggerFired(uint32_t objAddr)
{
	return (getTriggerFlags(objAddr) & TRIGGER_WAS_TRIGGERED_FLAG) != 0;
}

// Read supplied item IDs from a trigger and push an OnUse outgoing message.
static void readAndPushTriggerItems(uint32_t addr, uint64_t guid, int32_t itemCount)
{
	if (itemCount <= 0) return;
	if (itemCount > 4) itemCount = 4;

	uint32_t listAddr = processAnalyzer->readData<uint32_t>(addr + TRIGGER_SUPPLIEDITEMLIST_OFFSET);
	if (listAddr == 0) return;

	PendingTriggerOnUse pending{};
	pending.triggerGuid = guid;
	pending.itemCount = itemCount;
	for (int i = 0; i < itemCount; i++)
		pending.itemIds[i] = processAnalyzer->readData<int32_t>(listAddr + i * 4);

	g_outgoingTriggerOnUse.push_back(pending);
	printf("Trigger OnUse detected: GUID %llu, %d items\n", guid, itemCount);
}

// Scan all triggers for state changes (newly fired or items placed).
static void detectTriggerChanges()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&triggerCriticalSection);

	for (uint32_t addr : g_allTriggersAddrs)
	{
		if (processAnalyzer->readData<uint8_t>(addr + 0x7C) != TRIGGER_CLASS_TAG)
			continue;

		uint64_t guid = processAnalyzer->readData<uint64_t>(addr + 0x08);
		if (guid == 0)
			continue;

		// Skip triggers that were applied remotely this cycle
		if (g_suppressTriggerDetection.count(guid))
			continue;

		uint32_t currentFlags = getTriggerFlags(addr);
		int32_t currentItemCount = processAnalyzer->readData<int32_t>(addr + TRIGGER_SUPPLIEDITEMCOUNT_OFFSET);

		auto it = triggerStatesCache.find(guid);
		bool isNewTrigger = (it == triggerStatesCache.end());
		bool flagChanged = !isNewTrigger && (it->second != currentFlags);
		bool justFired = flagChanged
			&& (currentFlags & TRIGGER_WAS_TRIGGERED_FLAG)
			&& !(it->second & TRIGGER_WAS_TRIGGERED_FLAG);

		// --- When trigger just fired with items: this is an OnUse activation ---
		if (justFired && currentItemCount > 0)
		{
			readAndPushTriggerItems(addr, guid, currentItemCount);
			// Update both caches so we don't double-detect
			triggerStatesCache[guid] = currentFlags;
			triggerSuppliedItemCountCache[guid] = currentItemCount;
			continue;
		}

		// --- When trigger just fired without items: OnPressB ---
		if (justFired && currentItemCount == 0)
		{
			g_outgoingTriggerPressB.push_back(guid);
			printf("Trigger fired locally (OnPressB): GUID %llu\n", guid);
			triggerStatesCache[guid] = currentFlags;
			continue;
		}

		// --- Detect item placement without trigger fire (multi-item triggers) ---
		auto icIt = triggerSuppliedItemCountCache.find(guid);
		if (icIt == triggerSuppliedItemCountCache.end())
		{
			// First time seeing this trigger — just initialize caches
			triggerStatesCache[guid] = currentFlags;
			triggerSuppliedItemCountCache[guid] = currentItemCount;
		}
		else if (currentItemCount > icIt->second)
		{
			// Items placed but trigger hasn't fired yet (multi-item trigger)
			readAndPushTriggerItems(addr, guid, currentItemCount - icIt->second);
			triggerStatesCache[guid] = currentFlags;
			triggerSuppliedItemCountCache[guid] = currentItemCount;
		}
		else
		{
			// No change or items ejected — just update caches
			triggerStatesCache[guid] = currentFlags;
			triggerSuppliedItemCountCache[guid] = currentItemCount;
		}
	}

	LeaveCriticalSection(&triggerCriticalSection);
}

// Send outgoing trigger OnPressB messages
static void sendTriggerPressB(Client& client)
{
	if (!client.IsConnected() || myGuid == 0)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	std::vector<uint64_t> pending = g_outgoingTriggerPressB;
	g_outgoingTriggerPressB.clear();
	LeaveCriticalSection(&triggerCriticalSection);

	for (uint64_t triggerGuid : pending)
	{
		auto* msg = static_cast<TriggerOnPressBMessage*>(client.CreateMessage(TRIGGER_ONPRESSB));
		if (!msg)
			continue;

		msg->triggerGuid = triggerGuid;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Process incoming trigger OnPressB message
static void processTriggerOnPressB(TriggerOnPressBMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->triggerGuid == 0)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	g_incomingTriggerPressB.push_back(msg->triggerGuid);
	LeaveCriticalSection(&triggerCriticalSection);
}

// Apply queued incoming trigger OnPressB on the game thread
static void applyQueuedTriggerPressB()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	std::vector<uint64_t> pending = g_incomingTriggerPressB;
	g_incomingTriggerPressB.clear();
	LeaveCriticalSection(&triggerCriticalSection);

	for (uint64_t triggerGuid : pending)
	{
		uint32_t addr = processAnalyzer->findGameObjByGUID(triggerGuid);
		if (addr == 0 || !isTriggerObject(addr))
			continue;

		// Enable the trigger and fire OnPressB
		uint32_t flags = getTriggerFlags(addr);
		if (!(flags & TRIGGER_ENABLED_FLAG))
		{
			flags |= TRIGGER_ENABLED_FLAG;
			processAnalyzer->writeData<uint32_t>(addr + 0x120, flags);
		}

		game_TriggerOnPressB(reinterpret_cast<void*>(addr));
		printf("Applied remote trigger OnPressB: GUID %llu\n", triggerGuid);
	}
}

// Send outgoing trigger OnUse messages (items placed into triggers)
static void sendTriggerOnUse(Client& client)
{
	if (!client.IsConnected() || myGuid == 0)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	std::vector<PendingTriggerOnUse> pending = g_outgoingTriggerOnUse;
	g_outgoingTriggerOnUse.clear();
	LeaveCriticalSection(&triggerCriticalSection);

	for (const PendingTriggerOnUse& use : pending)
	{
		auto* msg = static_cast<TriggerOnUseMessage*>(client.CreateMessage(TRIGGER_ONUSE));
		if (!msg)
			continue;

		msg->triggerGuid = use.triggerGuid;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		msg->itemCount = use.itemCount;
		for (int i = 0; i < 4; i++)
			msg->itemIds[i] = use.itemIds[i];

		client.SendMessage(channels::Gameplay, msg);
	}
}

// Process incoming trigger OnUse message
static void processTriggerOnUse(TriggerOnUseMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->triggerGuid == 0)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	g_incomingTriggerOnUse.push_back({ msg->triggerGuid, msg->itemCount, { msg->itemIds[0], msg->itemIds[1], msg->itemIds[2], msg->itemIds[3] } });
	LeaveCriticalSection(&triggerCriticalSection);
}

// Apply queued incoming trigger OnUse on the game thread
// Directly places items into the trigger's supplied list, then calls UseItems
// to check the set-match and fire OnActivate if all required items are present.
static void applyQueuedTriggerOnUse()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&triggerCriticalSection);
	std::vector<PendingTriggerOnUse> pending = g_incomingTriggerOnUse;
	g_incomingTriggerOnUse.clear();
	LeaveCriticalSection(&triggerCriticalSection);

	for (const PendingTriggerOnUse& use : pending)
	{
		uint32_t addr = processAnalyzer->findGameObjByGUID(use.triggerGuid);
		if (addr == 0 || !isTriggerObject(addr))
			continue;

		// Suppress detection for this trigger so we don't echo it back
		EnterCriticalSection(&triggerCriticalSection);
		g_suppressTriggerDetection.insert(use.triggerGuid);
		LeaveCriticalSection(&triggerCriticalSection);

		// Enable the trigger if not already enabled
		uint32_t flags = getTriggerFlags(addr);
		if (!(flags & TRIGGER_ENABLED_FLAG))
		{
			flags |= TRIGGER_ENABLED_FLAG;
			processAnalyzer->writeData<uint32_t>(addr + 0x120, flags);
		}

		// Call Trigger_OnUse for each item — this runs the engine's full
		// item-placement code path including script callbacks and visuals
		for (int i = 0; i < use.itemCount; i++)
		{
			if (use.itemIds[i] < 0)
				continue;
			game_TriggerOnUse(reinterpret_cast<void*>(addr), use.itemIds[i]);
		}

		// Snapshot post-apply state so detection won't echo this back
		int32_t newCount = processAnalyzer->readData<int32_t>(addr + TRIGGER_SUPPLIEDITEMCOUNT_OFFSET);
		EnterCriticalSection(&triggerCriticalSection);
		triggerStatesCache[use.triggerGuid] = getTriggerFlags(addr);
		triggerSuppliedItemCountCache[use.triggerGuid] = newCount;
		LeaveCriticalSection(&triggerCriticalSection);

		printf("Applied remote trigger OnUse: GUID %llu, %d items (total %d)\n", use.triggerGuid, use.itemCount, newCount);
	}
}

// ===========================================================================
//  Switch Synchronization
// ===========================================================================

// Switch memory layout (from triggers_sdk.h):
//   +0x008  GUID (uint64_t)
//   +0x07C  object type tag (uint8_t, 0x33 = switch)
//   +0x160  stateFlags (uint32_t, SWITCH_ON = 0x00000002, bit1)
//   Switch_Toggle @ 0x004d2f50, Switch_SetOn @ 0x004d2f20 (game addresses, no ASLR)
static constexpr uint8_t   SWITCH_CLASS_TAG = 0x33;
static constexpr uint32_t  SWITCH_ON_FLAG = 0x00000002;
static constexpr uint32_t  SWITCH_TOGGLE_ADDR = 0x004d2f50;
static constexpr uint32_t  SWITCH_SETON_ADDR = 0x004d2f20;

typedef void(__thiscall* SwitchToggle_t)(void* self);
static const SwitchToggle_t game_SwitchToggle = reinterpret_cast<SwitchToggle_t>(SWITCH_TOGGLE_ADDR);

typedef void(__thiscall* SwitchSetOn_t)(void* self, int on);
static const SwitchSetOn_t game_SwitchSetOn = reinterpret_cast<SwitchSetOn_t>(SWITCH_SETON_ADDR);

static bool isSwitchObject(uint32_t objAddr)
{
	return objAddr != 0
		&& processAnalyzer->readData<uint8_t>(objAddr + 0x7C) == SWITCH_CLASS_TAG;
}

static uint32_t getSwitchStateFlags(uint32_t objAddr)
{
	return processAnalyzer->readData<uint32_t>(objAddr + 0x160);
}

static bool isSwitchOn(uint32_t objAddr)
{
	return (getSwitchStateFlags(objAddr) & SWITCH_ON_FLAG) != 0;
}

// Scan all switches for state changes.
static void detectSwitchChanges()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&switchCriticalSection);

	for (uint32_t addr : g_allSwitchesAddrs)
	{
		if (processAnalyzer->readData<uint8_t>(addr + 0x7C) != SWITCH_CLASS_TAG)
			continue;

		uint64_t guid = processAnalyzer->readData<uint64_t>(addr + 0x08);
		if (guid == 0)
			continue;

		uint32_t currentFlags = getSwitchStateFlags(addr);

		auto it = switchStatesCache.find(guid);
		if (it == switchStatesCache.end())
		{
			switchStatesCache[guid] = currentFlags;
		}
		else if (it->second != currentFlags)
		{
			// Switch state changed — record the new on/off state
			bool nowOn = (currentFlags & SWITCH_ON_FLAG) != 0;
			g_outgoingSwitchToggles.push_back({ guid, nowOn });
			printf("Switch toggled locally: GUID %llu -> %s\n", guid, nowOn ? "ON" : "OFF");
			switchStatesCache[guid] = currentFlags;
		}
	}

	LeaveCriticalSection(&switchCriticalSection);
}

// Send outgoing switch toggle messages
static void sendSwitchToggles(Client& client)
{
	if (!client.IsConnected() || myGuid == 0)
		return;

	EnterCriticalSection(&switchCriticalSection);
	std::vector<std::pair<uint64_t, bool>> pending = g_outgoingSwitchToggles;
	g_outgoingSwitchToggles.clear();
	LeaveCriticalSection(&switchCriticalSection);

	for (const auto& entry : pending)
	{
		auto* msg = static_cast<SwitchToggleMessage*>(client.CreateMessage(SWITCH_TOGGLE));
		if (!msg)
			continue;

		msg->switchGuid = entry.first;
		msg->switchedOn = entry.second ? 1 : 0;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Process incoming switch toggle message
static void processSwitchToggle(SwitchToggleMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->switchGuid == 0)
		return;

	EnterCriticalSection(&switchCriticalSection);
	g_incomingSwitchToggles.push_back({ msg->switchGuid, msg->switchedOn != 0 });
	LeaveCriticalSection(&switchCriticalSection);
}

// Apply queued incoming switch toggles on the game thread
static void applyQueuedSwitchToggles()
{
	if (!processAnalyzer)
		return;

	EnterCriticalSection(&switchCriticalSection);
	std::vector<std::pair<uint64_t, bool>> pending = g_incomingSwitchToggles;
	g_incomingSwitchToggles.clear();
	LeaveCriticalSection(&switchCriticalSection);

	for (const auto& entry : pending)
	{
		uint32_t addr = processAnalyzer->findGameObjByGUID(entry.first);
		if (addr == 0 || !isSwitchObject(addr))
			continue;

		bool currentOn = isSwitchOn(addr);
		if (currentOn == entry.second)
			continue; // already in the desired state

		game_SwitchSetOn(reinterpret_cast<void*>(addr), entry.second ? 1 : 0);
		printf("Applied remote switch toggle: GUID %llu -> %s\n", entry.first, entry.second ? "ON" : "OFF");
	}
}

// ===========================================================================
//  Animation Frame Synchronization (rigid_instance)
// ===========================================================================
//
// Snaps every animated rigid_instance on the level to a common animation frame
// once. The engine loops rigid_instance animations at a fixed rate, so a single
// alignment keeps them in phase from that point on. Field offsets recovered from
// the Reverse SDK (rigid_instance::SetAnimFrame @0x004C7580, GetAnimPlayer
// @0x004C74B0, AnimDataAvailable @0x004C71A0):
//   object        +0x008  GUID (uint64_t)
//   object        +0x07C  type tag (uint8_t, 0x05 = rigid_instance)
//   rigid_instance+0x140  simple_anim_player*   (0 when the object has no anim)
//   anim_player   +0x010  current frame (float) <- the value we sync
//   anim_player   +0x088  anim-data-valid flag  (0 => not animated)
static constexpr uint8_t  RIGID_INSTANCE_CLASS_TAG = 0x05;
static constexpr uint32_t RI_ANIMPLAYER_OFF = 0x140;
static constexpr uint32_t AP_FRAME_OFF = 0x10;
static constexpr uint32_t AP_VALID_OFF = 0x88;
static constexpr uint32_t RI_SETANIMFRAME_ADDR = 0x004C7580;

// public: void __thiscall rigid_instance::SetAnimFrame(float)
// Sets the current frame and resets the sub-frame + event accumulators. Internally
// guards on AnimDataAvailable(), so it is safe to call on any rigid_instance.
typedef void(__thiscall* SetAnimFrame_t)(void* self, float frame);
static const SetAnimFrame_t game_SetAnimFrame = reinterpret_cast<SetAnimFrame_t>(RI_SETANIMFRAME_ADDR);

CRITICAL_SECTION animSyncCriticalSection;
static std::atomic<bool> g_animSyncRequested{ false };
static std::unordered_map<uint64_t, float> g_incomingAnimFrames;

// Return the simple_anim_player pointer for an animated rigid_instance, or 0 if
// the object is not a rigid_instance, has no anim player, or has no anim data.
static uint32_t getRigidAnimPlayer(uint32_t objAddr)
{
	if (objAddr == 0 || !processAnalyzer)
		return 0;
	if (processAnalyzer->readData<uint8_t>(objAddr + 0x7C) != RIGID_INSTANCE_CLASS_TAG)
		return 0;
	uint32_t player = processAnalyzer->readData<uint32_t>(objAddr + RI_ANIMPLAYER_OFF);
	if (player == 0)
		return 0;
	if (processAnalyzer->readData<uint32_t>(player + AP_VALID_OFF) == 0)
		return 0; // AnimDataAvailable == false
	return player;
}

// Capture the current anim frame of every animated rigid_instance and broadcast
// it once. Runs on the network thread (like the other send* helpers); reading the
// game's memory from here is safe (it is our own process). Triggered by /syncanim.
static void sendAnimSync(Client& client)
{
	if (!g_animSyncRequested.exchange(false))
		return;
	if (!client.IsConnected() || !processAnalyzer || !gameManager.isOnLevel())
		return;

	std::vector<uint32_t> rigids =
		processAnalyzer->findAllGameObjByPattern<uint8_t>(RIGID_INSTANCE_CLASS_TAG, 0x7C);

	std::unordered_map<uint64_t, float> snapshot;
	for (uint32_t addr : rigids)
	{
		if (snapshot.size() >= MaxAnimSyncPerMessage)
			break;
		uint32_t player = getRigidAnimPlayer(addr);
		if (player == 0)
			continue;
		uint64_t guid = processAnalyzer->readData<uint64_t>(addr + 0x8);
		if (guid == 0)
			continue;
		snapshot[guid] = processAnalyzer->readData<float>(player + AP_FRAME_OFF);
	}

	if (snapshot.empty())
	{
		g_ChatOverlay.AddSystemMessage("[System] No animated objects found to sync.");
		return;
	}

	auto* msg = static_cast<AnimSyncMessage*>(client.CreateMessage(ANIM_SYNC));
	if (!msg)
		return;
	msg->frames = std::move(snapshot);
	msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
	size_t sent = msg->frames.size();
	client.SendMessage(channels::Gameplay, msg);

	g_ChatOverlay.AddSystemMessage("[System] Synced animation frame of " +
		std::to_string(sent) + " object(s) to all players.");
	printf("Broadcast anim sync for %zu rigid_instances\n", sent);
}

// Queue an incoming anim-sync snapshot (network thread).
static void processAnimSync(AnimSyncMessage* msg)
{
	if (msg->nowLevel != nowLevel)
		return;

	EnterCriticalSection(&animSyncCriticalSection);
	for (const auto& kv : msg->frames)
		g_incomingAnimFrames[kv.first] = kv.second; // latest snapshot wins
	LeaveCriticalSection(&animSyncCriticalSection);
}

// Apply queued anim-sync frames on the game thread (called from OnAdvanceLogic).
static void applyQueuedAnimSync()
{
	if (!processAnalyzer)
		return;

	std::unordered_map<uint64_t, float> pending;
	EnterCriticalSection(&animSyncCriticalSection);
	pending.swap(g_incomingAnimFrames);
	LeaveCriticalSection(&animSyncCriticalSection);

	if (pending.empty())
		return;

	for (const auto& kv : pending)
	{
		uint32_t addr = processAnalyzer->findGameObjByGUID(kv.first);
		if (addr == 0 || getRigidAnimPlayer(addr) == 0)
			continue; // object gone, or no longer an animated rigid_instance
		game_SetAnimFrame(reinterpret_cast<void*>(addr), kv.second);
	}
}

// ===========================================================================
//  Ring (One Ring) Stealth Synchronization
// ===========================================================================
//
// When a player equips the One Ring, the base game turns their own bilbo mesh
// half-transparent. We broadcast that equipped state so every peer applies the
// same stealth look to that player's fake-bilbo NPC, tiered by the NPC's team:
//   team 2         -> invisible / barely visible
//   any other team -> half-transparent (matches bilbo's own ring)
//
// Field offsets (from the Reverse SDK):
//   bilbo::SetRingEquipped @0x00423C90 writes bilbo+0x420 (1 = ring equipped)
//   NPC::setTeam writes the team byte at NPCObject+0x1a4
//   NPCObject::MakeTransparent @0x004A99A0 toggles bit 0x40 of the render-flags
//     dword at (NPCObject+0x310)+0xe0 (verified: NPCObject::RenderGeometry gates
//     the translucent pass on that same bit).
static constexpr uint32_t BILBO_RING_EQUIPPED_OFF = 0x420; // bilbo+0x420: 1 = ring on
static constexpr uint32_t NPC_TEAM_OFF = 0x1A4;            // NPCObject+0x1a4: team id (byte)
static constexpr uint32_t NPC_RENDER_COMPONENT_OFF = 0x310; // NPCObject+0x310: render instance
static constexpr uint32_t RENDER_FLAGS_OFF = 0xE0;        // component+0xe0: render flags dword
static constexpr uint32_t RENDER_TRANSPARENT_BIT = 0x40;   // bit 0x40: half-transparent pass
static constexpr uint32_t NPC_MAKETRANSPARENT_ADDR = 0x004A99A0;
static constexpr uint8_t  RING_TEAM_INVISIBLE = 2;         // this team goes (near-)invisible

// Extra render-flag bit OR'd on top of the half-transparent bit for team 2 to push
// it toward invisible. 0x40 alone = 50%. Adjust this once confirmed in-game if a
// stronger engine hide bit is identified; writing a render-flag bit is side-effect
// -free (it can't crash), so experimenting here is safe.
static constexpr uint32_t RING_TEAM2_EXTRA_HIDE_BITS = 0x40;

// public: void __thiscall NPCObject::MakeTransparent(int on)
typedef void(__thiscall* NPCMakeTransparent_t)(void* self, int on);
static const NPCMakeTransparent_t game_NPCMakeTransparent =
	reinterpret_cast<NPCMakeTransparent_t>(NPC_MAKETRANSPARENT_ADDR);

CRITICAL_SECTION ringCriticalSection;
static std::unordered_map<uint64_t, uint8_t> g_playerRingState; // npcGuid -> ring equipped (0/1)
static std::atomic<int>     g_lastLocalRing{ -1 };  // last broadcast local ring state (-1 = unknown)
static std::atomic<bool>    g_localRingDirty{ false };
static std::atomic<uint8_t> g_localRingValue{ 0 };

static void applyPlayerRingVisuals();

// Read this=local bilbo's ring-equipped flag (game thread) and flag a broadcast on change.
static void detectLocalRingChange(void* bilboThis)
{
	if (!bilboThis)
		return;
	int cur = (*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(bilboThis) + BILBO_RING_EQUIPPED_OFF)) ? 1 : 0;
	if (cur != g_lastLocalRing.load())
	{
		g_lastLocalRing.store(cur);
		g_localRingValue.store(static_cast<uint8_t>(cur));
		g_localRingDirty.store(true);
	}
}

// Send the local ring state if it changed (network thread, from the client loop).
static void sendRingSync(Client& client)
{
	if (!g_localRingDirty.exchange(false))
		return;
	if (!client.IsConnected() || myGuid == 0)
		return;

	auto* msg = static_cast<RingSyncMessage*>(client.CreateMessage(RING_SYNC));
	if (!msg)
		return;
	msg->playerGuid = myGuid;
	msg->ringEquipped = g_localRingValue.load();
	msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
	client.SendMessage(channels::Gameplay, msg);

	printf("Broadcast ring state: %s\n", msg->ringEquipped ? "ON" : "OFF");
}

// Store an incoming ring state (network thread). Persistent per player, so not
// level-gated — the stealth look is a player attribute, re-applied every frame.
static void processRingSync(RingSyncMessage* msg)
{
	if (msg->playerGuid == 0)
		return;
	EnterCriticalSection(&ringCriticalSection);
	g_playerRingState[msg->playerGuid] = msg->ringEquipped ? 1 : 0;
	LeaveCriticalSection(&ringCriticalSection);
}

// What same-team players look like while the ring is on. bit 0x40 (via
// MakeTransparent) is a BINARY hide, not a 50% alpha, so a true "half" needs the
// character material alpha (not yet pinned). Pick the reliable behaviour here:
//   0 = fully visible (ring has no visible effect on same-team players)
//   1 = invisible     (same as the enemy-team look)
static constexpr int RING_SAMETEAM_MODE = 0;

// Apply the ring stealth look to one NPC (game thread). Idempotent.
// NOTE (verified in-game): NPCObject::MakeTransparent toggles bit 0x40 of the
// CharacterObject render field at (npc+0x310)+0xe0, and that bit fully HIDES the
// character — it is a visible/invisible switch, not a translucency level.
static void applyRingVisual(uint32_t npcAddr, bool ringOn)
{
	if (!processAnalyzer || npcAddr == 0)
		return;

	if (!ringOn)
	{
		game_NPCMakeTransparent(reinterpret_cast<void*>(npcAddr), 0); // fully visible
		return;
	}

	uint8_t team = processAnalyzer->readData<uint8_t>(npcAddr + NPC_TEAM_OFF);
	bool hide = (team == RING_TEAM_INVISIBLE) || (RING_SAMETEAM_MODE == 1);
	game_NPCMakeTransparent(reinterpret_cast<void*>(npcAddr), hide ? 1 : 0);
}

// Re-apply the ring stealth look to every remote player each frame (game thread).
// Idempotent + snapshot-based, so respawns and late joiners self-correct.
static void applyPlayerRingVisuals()
{
	if (!processAnalyzer)
		return;

	std::unordered_map<uint64_t, uint8_t> ringSnapshot;
	EnterCriticalSection(&ringCriticalSection);
	ringSnapshot = g_playerRingState;
	LeaveCriticalSection(&ringCriticalSection);

	EnterCriticalSection(&playersCriticalSection);
	for (auto& p : activePlayers)
	{
		if (!p.npc)
			continue;
		uint32_t addr = p.npc->getObjectPtr();
		if (addr == 0)
			continue;
		auto it = ringSnapshot.find(p.npcGuid);
		bool ringOn = (it != ringSnapshot.end()) && it->second != 0;
		applyRingVisual(addr, ringOn);
	}
	LeaveCriticalSection(&playersCriticalSection);
}

// ===========================================================================
//  Rigid-Instance Spawning (synced)
// ===========================================================================
//
// Recipe (from the Kingjoyer project): obj_mgr::CreateObject("RigidInstance", guid)
// creates the object with the given GUID (pass GUID 0 to let the engine assign one
// and read it back from the return value). Then object::OnImport(bin_in) configures
// it from a ./Templates/<name>.export file (defines mesh/props), and object::Move
// positions it. To sync, the spawner creates with an engine-assigned GUID and
// broadcasts { GUID, world position, template }; every peer re-creates the object
// with that SAME GUID and template so it stays cross-referenceable and identical.
//
// NOTE: the template file must exist in ./Templates/ on each client (like skins).
// Without a template the object still spawns (bare) but has no mesh to render.
static const char* RIGID_INSTANCE_TYPE = "RigidInstance";

struct PendingSpawn
{
	uint64_t guid = 0;
	float    x = 0.0f, y = 0.0f, z = 0.0f;
	char     tmpl[64] = {};
};

CRITICAL_SECTION spawnCriticalSection;
static std::vector<PendingSpawn> g_incomingSpawns;
static std::vector<PendingSpawn> g_outgoingSpawns;
static std::atomic<bool> g_localSpawnRequested{ false };
static char g_localSpawnTemplate[64] = {};

static void applyQueuedSpawns();
static void processLocalSpawnRequest();

// Configure a freshly created object: import the template (mesh/props) if present,
// then move it to the target world position. Game thread only.
static void configureSpawnedObject(uint64_t objectGuid, const char* tmpl, float x, float y, float z)
{
	object* pObj = reinterpret_cast<object*>(
		static_cast<uintptr_t>(processAnalyzer->findGameObjByGUID(objectGuid)));
	if (!pObj)
		return;

	// Load the template only if the name is a plain file name — reject path
	// separators / traversal so a peer's spawn can't make us read arbitrary files.
	if (tmpl && tmpl[0]
		&& !strchr(tmpl, '/') && !strchr(tmpl, '\\') && !strchr(tmpl, ':')
		&& !strstr(tmpl, ".."))
	{
		char path[300];
		sprintf_s(path, sizeof(path), "./Templates/%s", tmpl);
		bin_in BinIn{};
		if (BinIn.OpenFile(path) && BinIn.ReadHeader() && BinIn.ReadFields())
			pObj->OnImport(BinIn);
	}

	vector3 P{ x, y, z };
	pObj->Move(P, 1);
}

// Create a RigidInstance with an explicit (shared) GUID for an incoming spawn.
// Idempotent: skips if an object with that GUID already exists locally.
static void spawnRemoteRigidInstance(uint64_t objectGuid, const char* tmpl, float x, float y, float z)
{
	if (!processAnalyzer || objectGuid == 0)
		return;
	if (processAnalyzer->findGameObjByGUID(objectGuid) != 0)
		return; // already spawned

	guid gid;
	gid.Guid = objectGuid;
	g_ObjMgr.CreateObject(RIGID_INSTANCE_TYPE, gid); // force the shared GUID
	configureSpawnedObject(objectGuid, tmpl, x, y, z);
}

// Handle the local /spawn request on the game thread: create locally (engine
// assigns a GUID), configure it, then queue a broadcast so peers spawn the same.
static void processLocalSpawnRequest()
{
	if (!g_localSpawnRequested.exchange(false))
		return;
	if (!processAnalyzer || !gameManager.isOnLevel() || myGuid == 0)
		return;

	char tmpl[64];
	EnterCriticalSection(&spawnCriticalSection);
	memcpy(tmpl, g_localSpawnTemplate, sizeof(tmpl));
	LeaveCriticalSection(&spawnCriticalSection);
	tmpl[sizeof(tmpl) - 1] = 0;

	guid zero;
	zero.Guid = 0;
	guid created = g_ObjMgr.CreateObject(RIGID_INSTANCE_TYPE, zero); // engine assigns GUID
	if (created.Guid == 0)
	{
		g_ChatOverlay.AddSystemMessage("[System] Spawn failed (CreateObject returned 0).");
		return;
	}

	// Spawn just in front of / beside the player (matches Kingjoyer's offset).
	Vector3 bp = getBilboPos();
	float x = bp.x + 50.0f;
	float y = bp.y;
	float z = bp.z + 50.0f;

	configureSpawnedObject(created.Guid, tmpl, x, y, z);

	PendingSpawn ps;
	ps.guid = created.Guid;
	ps.x = x; ps.y = y; ps.z = z;
	memcpy(ps.tmpl, tmpl, sizeof(ps.tmpl));

	EnterCriticalSection(&spawnCriticalSection);
	g_outgoingSpawns.push_back(ps);
	LeaveCriticalSection(&spawnCriticalSection);

	g_ChatOverlay.AddSystemMessage("[System] Spawned rigid instance (synced to all players).");
	printf("Spawned RigidInstance GUID %llu, broadcasting\n", created.Guid);
}

// Send any queued local spawns to peers (network thread, from the client loop).
static void sendSpawns(Client& client)
{
	std::vector<PendingSpawn> out;
	EnterCriticalSection(&spawnCriticalSection);
	out.swap(g_outgoingSpawns);
	LeaveCriticalSection(&spawnCriticalSection);

	for (const auto& s : out)
	{
		if (!client.IsConnected())
			break;
		auto* msg = static_cast<SpawnObjectMessage*>(client.CreateMessage(SPAWN_OBJECT));
		if (!msg)
			continue;
		msg->objectGuid = s.guid;
		msg->x = s.x; msg->y = s.y; msg->z = s.z;
		strlcpy(msg->templateName, s.tmpl);
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Queue an incoming spawn (network thread).
static void processSpawnObject(SpawnObjectMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->objectGuid == 0)
		return;

	PendingSpawn ps;
	ps.guid = msg->objectGuid;
	ps.x = msg->x; ps.y = msg->y; ps.z = msg->z;
	strncpy(ps.tmpl, msg->templateName, sizeof(ps.tmpl));
	ps.tmpl[sizeof(ps.tmpl) - 1] = 0;

	EnterCriticalSection(&spawnCriticalSection);
	g_incomingSpawns.push_back(ps);
	LeaveCriticalSection(&spawnCriticalSection);
}

// Apply queued remote spawns on the game thread.
static void applyQueuedSpawns()
{
	if (!processAnalyzer)
		return;

	std::vector<PendingSpawn> pending;
	EnterCriticalSection(&spawnCriticalSection);
	pending.swap(g_incomingSpawns);
	LeaveCriticalSection(&spawnCriticalSection);

	for (const auto& s : pending)
		spawnRemoteRigidInstance(s.guid, s.tmpl, s.x, s.y, s.z);
}

// ===========================================================================
//  FX Spawning (fire-and-forget, synced)
// ===========================================================================
//
// fx_object::FireAndForget(name, pos, rot, scale) @ 0x00488D30 plays a one-shot
// effect that auto-destroys — so no GUID/lifetime sync is needed, each client
// simply plays it. SpawnFxSynced() plays the effect locally AND broadcasts it so
// every peer plays the same effect; call it from a future trigger.
static constexpr uint32_t FX_FIREANDFORGET_ADDR = 0x00488D30;

// static u64 __cdecl fx_object::FireAndForget(const char*, vector3&, radian3&, vector3&)
// References are passed as pointers by the ABI.
typedef uint64_t(__cdecl* FxFireAndForget_t)(const char* name, const vector3* pos,
	const radian3* rot, const vector3* scale);
static const FxFireAndForget_t game_fxFireAndForget =
	reinterpret_cast<FxFireAndForget_t>(FX_FIREANDFORGET_ADDR);

struct PendingFx
{
	char  name[64] = {};
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
	float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
};

CRITICAL_SECTION fxCriticalSection;
static std::vector<PendingFx> g_incomingFx;   // to play locally (remote + own)
static std::vector<PendingFx> g_outgoingFx;   // to broadcast to peers

static void applyQueuedFx();

// Play one FX now (game thread only — calls the engine directly).
static void playFxLocal(const PendingFx& fx)
{
	if (fx.name[0] == 0)
		return;
	vector3 pos{ fx.x, fx.y, fx.z };
	radian3 rot{ fx.pitch, fx.yaw, fx.roll };
	vector3 scale{ fx.scaleX, fx.scaleY, fx.scaleZ };
	game_fxFireAndForget(fx.name, &pos, &rot, &scale);
}

// PUBLIC: play an FX locally and broadcast it to all peers. Safe to call from any
// thread — the local play is deferred to the game thread. This is the entry point
// a future trigger should call.
static void SpawnFxSynced(const char* name, float x, float y, float z,
	float pitch = 0.0f, float yaw = 0.0f, float roll = 0.0f,
	float scaleX = 1.0f, float scaleY = 1.0f, float scaleZ = 1.0f)
{
	if (!name || name[0] == 0)
		return;

	PendingFx fx;
	strncpy(fx.name, name, sizeof(fx.name));
	fx.name[sizeof(fx.name) - 1] = 0;
	fx.x = x; fx.y = y; fx.z = z;
	fx.pitch = pitch; fx.yaw = yaw; fx.roll = roll;
	fx.scaleX = scaleX; fx.scaleY = scaleY; fx.scaleZ = scaleZ;

	EnterCriticalSection(&fxCriticalSection);
	g_incomingFx.push_back(fx);   // play on our own screen
	g_outgoingFx.push_back(fx);   // and send to everyone else
	LeaveCriticalSection(&fxCriticalSection);
}

// Send any queued FX spawns to peers (network thread, from the client loop).
static void sendFxSpawns(Client& client)
{
	std::vector<PendingFx> out;
	EnterCriticalSection(&fxCriticalSection);
	out.swap(g_outgoingFx);
	LeaveCriticalSection(&fxCriticalSection);

	for (const auto& fx : out)
	{
		if (!client.IsConnected())
			break;
		auto* msg = static_cast<SpawnFxMessage*>(client.CreateMessage(SPAWN_FX));
		if (!msg)
			continue;
		strlcpy(msg->fxName, fx.name);
		msg->x = fx.x; msg->y = fx.y; msg->z = fx.z;
		msg->pitch = fx.pitch; msg->yaw = fx.yaw; msg->roll = fx.roll;
		msg->scaleX = fx.scaleX; msg->scaleY = fx.scaleY; msg->scaleZ = fx.scaleZ;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Queue an incoming FX spawn (network thread).
static void processSpawnFx(SpawnFxMessage* msg)
{
	if (msg->nowLevel != nowLevel)
		return;

	PendingFx fx;
	strncpy(fx.name, msg->fxName, sizeof(fx.name));
	fx.name[sizeof(fx.name) - 1] = 0;
	fx.x = msg->x; fx.y = msg->y; fx.z = msg->z;
	fx.pitch = msg->pitch; fx.yaw = msg->yaw; fx.roll = msg->roll;
	fx.scaleX = msg->scaleX; fx.scaleY = msg->scaleY; fx.scaleZ = msg->scaleZ;

	EnterCriticalSection(&fxCriticalSection);
	g_incomingFx.push_back(fx);
	LeaveCriticalSection(&fxCriticalSection);
}

// Play all queued FX on the game thread.
static void applyQueuedFx()
{
	std::vector<PendingFx> pending;
	EnterCriticalSection(&fxCriticalSection);
	pending.swap(g_incomingFx);
	LeaveCriticalSection(&fxCriticalSection);

	if (pending.empty() || !gameManager.isOnLevel())
		return;

	for (const auto& fx : pending)
		playFxLocal(fx);
}

// ===========================================================================
//  Server IP Config
// ===========================================================================

[[maybe_unused]] static std::string readServerIP()
{
	std::string ip = NetDefaults::DEFAULT_IP;
	std::ifstream configFile(NetDefaults::CONFIG_FILE);
	if (configFile.is_open())
	{
		std::getline(configFile, ip);
		printf("Server IP from config.txt: %s\n", ip.c_str());
	}
	else
	{
		printf("config.txt not found — using default: %s\n", ip.c_str());
	}
	return ip;
}

static std::string readSecureServerIP()
{
	const std::string ip = SecureConnect::getClientServerIp();
	printf("Server IP for secure token request: %s\n", ip.c_str());
	return ip;
}

// ===========================================================================
//  Remote Player Update
// ===========================================================================

/// Helper: get the clamped animation frame range for an animation ID.
static float getClampedFrameRange(uint32_t animId, float fallback)
{
	if (animFrameRanges.count(animId))
	{
		float range = animFrameRanges[animId] - 0.5f;
		return (range < 0.0f) ? 0.0f : range;
	}
	return fallback;
}

static Player* findPlayerByGuid(uint64_t guid)
{
	for (auto& p : activePlayers)
		if (p.npcGuid == guid)
			return &p;
	return nullptr;
}

static std::string getCanonicalSkinFileNameForGuid(uint64_t guid)
{
	return SkinSync::getGuidBoundSkinFileName(playerGuids, guid);
}

static std::string getTextureNameForGuid(uint64_t guid)
{
	const auto it = playerTextureNames.find(guid);
	if (it != playerTextureNames.end() && !it->second.empty())
		return it->second;

	return getCanonicalSkinFileNameForGuid(guid);
}

static std::string getSkinFilePathForGuid(uint64_t guid)
{
	const auto it = playerSkinFilePaths.find(guid);
	return (it != playerSkinFilePaths.end()) ? it->second : std::string();
}

static bool installSkinBytesForGuid(uint64_t guid, const std::string& announcedFileName, const uint8_t* data, size_t bytes, std::string& savedPath)
{
	std::string canonicalFileName = SkinSync::sanitizeFileName(announcedFileName);
	if (canonicalFileName.empty())
		canonicalFileName = getCanonicalSkinFileNameForGuid(guid);
	if (canonicalFileName.empty())
		return false;

	return SkinSync::saveInstalledSkinFile(canonicalFileName, data, bytes, savedPath);
}

static void applySkinMetadataToPlayer(Player& player)
{
	player.textureName = getTextureNameForGuid(player.npcGuid);
	player.textureFilePath = getSkinFilePathForGuid(player.npcGuid);
}

static void applyNameStatusMetadataToPlayer(Player& player)
{
	const auto nicknameIt = pendingNicknames.find(player.npcGuid);
	if (nicknameIt != pendingNicknames.end())
		player.nickname = nicknameIt->second;

	const auto statusIt = pendingStatuses.find(player.npcGuid);
	if (statusIt != pendingStatuses.end())
		player.status = statusIt->second;

	if (player.nickname_marker)
		player.nickname_marker->setText(player.nickname.c_str());
	if (player.status_marker)
		player.status_marker->setText(player.status.c_str());
}

static void updateExistingPlayer(Player& player, PositionMessage* msg, double currentTime)
{
	// --- Snapshot previous position ---
	player.prevX = player.x;
	player.prevY = player.y;
	player.prevZ = player.z;
	player.targetX = msg->x;
	player.targetY = msg->y;
	player.targetZ = msg->z;

	if (player.bilboWeapon != msg->bilboWeapon)
		player.setWeapon(msg->bilboWeapon);
	player.nowLevel = msg->nowLevel;

	// Don't render NPC for the local player
	if (player.npcGuid == myGuid)
		player.npc = nullptr;

	// --- Rotation ---
	player.targetRotationY = msg->rotationY;

	if (msg->animation >= 0 && msg->animation <= 200)
		player.targetAnimation = msg->animation;
	player.lerpStartTime = currentTime;
	applySkinMetadataToPlayer(player);
	applyNameStatusMetadataToPlayer(player);
}

static void addNewPlayer(PositionMessage* msg, double currentTime)
{
	Player newPlayer;
	newPlayer.setNpcGuid(msg->playerGuid);
	newPlayer.setPosition(msg->x, msg->y, msg->z);
	newPlayer.setRotationY(msg->rotationY);
	newPlayer.setAnimation(msg->animation, msg->animFrame, msg->lastAnimFrame);
	newPlayer.bilboWeapon = msg->bilboWeapon;
	newPlayer.nowLevel = msg->nowLevel;
	applySkinMetadataToPlayer(newPlayer);
	// Override lastAnimFrame with known range if available
	if (animFrameRanges.count(msg->animation))
		newPlayer.lastAnimFrame = animFrameRanges[msg->animation];

	newPlayer.initializeLerp(currentTime);

	// Create NPC for remote players only
	if (newPlayer.npcGuid == myGuid)
	{
		newPlayer.npc = nullptr;
	}
	else
	{
		newPlayer.npc = new NPC(processAnalyzer);
		newPlayer.npc->initializeByGuid(newPlayer.npcGuid);

		uint64_t nicknameGuid = (newPlayer.npcGuid & 0xFFFFFFFF) | 0x0D8AD91100000000ull;

		newPlayer.nickname_marker = new Marker(processAnalyzer);
		newPlayer.nickname_marker->initializeByGuid(nicknameGuid);

		uint64_t statusGuid = (newPlayer.npcGuid & 0xFFFFFFFF) | 0x0D8AD91200000000ull;

		newPlayer.status_marker = new Marker(processAnalyzer);
		newPlayer.status_marker->initializeByGuid(statusGuid);
	}

	applyNameStatusMetadataToPlayer(newPlayer);

	activePlayers.push_back(newPlayer);
}

static void processSkinAnnouncement(SkinAnnouncementMessage* msg)
{
	std::string textureName = SkinSync::sanitizeFileName(msg->textureName);
	if (textureName.empty())
		textureName = getCanonicalSkinFileNameForGuid(msg->playerGuid);
	if (textureName.empty())
		return;

	playerTextureNames[msg->playerGuid] = textureName;

	if (Player* player = findPlayerByGuid(msg->playerGuid))
		applySkinMetadataToPlayer(*player);

	printf("Skin mapped: GUID %llu -> texture '%s'\n", msg->playerGuid, textureName.c_str());
}

static void processSkinFileTransfer(SkinFileTransferMessage* msg)
{
	const int blockSize = msg->GetBlockSize();
	if (blockSize <= 0 || blockSize > SkinSync::MaxSkinFileBytes || !msg->GetBlockData())
		return;

	std::string textureName = SkinSync::sanitizeFileName(msg->textureName);
	std::string fileName = SkinSync::sanitizeFileName(msg->fileName);
	if (fileName.empty())
		fileName = getCanonicalSkinFileNameForGuid(msg->playerGuid);
	if (textureName.empty())
		textureName = fileName;
	if (!textureName.empty())
		playerTextureNames[msg->playerGuid] = textureName;

	std::string savedPath;
	if (installSkinBytesForGuid(msg->playerGuid, fileName,
		msg->GetBlockData(), static_cast<size_t>(blockSize), savedPath))
	{
		playerSkinFilePaths[msg->playerGuid] = savedPath;

		if (Player* player = findPlayerByGuid(msg->playerGuid))
			applySkinMetadataToPlayer(*player);

		printf("Saved skin file for GUID %llu to %s\n", msg->playerGuid, savedPath.c_str());
	}
}

static void processSkinClear(SkinClearMessage* msg)
{
	const auto pathIt = playerSkinFilePaths.find(msg->playerGuid);
	if (pathIt != playerSkinFilePaths.end())
		SkinSync::removeInstalledSkinFileByPath(pathIt->second);

	playerTextureNames.erase(msg->playerGuid);
	playerSkinFilePaths.erase(msg->playerGuid);

	if (Player* player = findPlayerByGuid(msg->playerGuid))
	{
		player->textureName.clear();
		player->textureFilePath.clear();
	}

	printf("Cleared skin mapping for GUID %llu\n", msg->playerGuid);
}

static void sendLocalSkinData(Client& client)
{
	if (localSkinUploadAttempted || !client.IsConnected() || myGuid == 0)
		return;

	localSkinUploadAttempted = true;

	if (!localSkinLoaded || !localSkinDefinition.enabled)
		return;

	const std::string canonicalFileName = getCanonicalSkinFileNameForGuid(myGuid);
	if (canonicalFileName.empty())
		return;

	std::string savedPath;
	if (installSkinBytesForGuid(myGuid, canonicalFileName,
		localSkinDefinition.fileBytes.data(), localSkinDefinition.fileBytes.size(), savedPath))
	{
		playerTextureNames[myGuid] = canonicalFileName;
		playerSkinFilePaths[myGuid] = savedPath;
	}

	auto* announceMsg = static_cast<SkinAnnouncementMessage*>(client.CreateMessage(SKIN_ANNOUNCE));
	if (announceMsg)
	{
		announceMsg->playerGuid = myGuid;
		SkinSync::copyStringToBuffer(canonicalFileName, announceMsg->textureName, sizeof(announceMsg->textureName));
		client.SendMessage(channels::Skin, announceMsg);
	}

	auto* fileMsg = static_cast<SkinFileTransferMessage*>(client.CreateMessage(SKIN_FILE_TRANSFER));
	if (!fileMsg)
		return;

	fileMsg->playerGuid = myGuid;
	SkinSync::copyStringToBuffer(canonicalFileName, fileMsg->textureName, sizeof(fileMsg->textureName));
	SkinSync::copyStringToBuffer(canonicalFileName, fileMsg->fileName, sizeof(fileMsg->fileName));

	uint8_t* block = client.AllocateBlock(static_cast<int>(localSkinDefinition.fileBytes.size()));
	if (!block)
	{
		client.ReleaseMessage(fileMsg);
		return;
	}

	std::memcpy(block, localSkinDefinition.fileBytes.data(), localSkinDefinition.fileBytes.size());
	client.AttachBlockToMessage(fileMsg, block, static_cast<int>(localSkinDefinition.fileBytes.size()));
	client.SendMessage(channels::Skin, fileMsg);

	printf("Uploaded local skin '%s' (%zu bytes) for GUID %llu\n",
		canonicalFileName.c_str(),
		localSkinDefinition.fileBytes.size(),
		myGuid);
}

// ===========================================================================
//  Message Dispatch
// ===========================================================================

static void processPositionUpdate(PositionMessage* msg, double currentTime)
{
	if (msg->nowLevel != nowLevel)
		return;

	EnterCriticalSection(&playersCriticalSection);

	Player* existing = findPlayerByGuid(msg->playerGuid);
	if (existing)
		updateExistingPlayer(*existing, msg, currentTime);
	else
		addNewPlayer(msg, currentTime);

	LeaveCriticalSection(&playersCriticalSection);
	/*
	std::cout << "Player " << std::hex << msg->playerGuid << std::dec
		<< ": pos(" << msg->x << ", "
		<< msg->y << ", "
		<< msg->z << ") rot("
		<< msg->rotationY << ") anim("
		<< msg->animation << ") frame("
		<< msg->animFrame << ") weapon("
		<< msg->bilboWeapon << ") level("
		<< msg->nowLevel << ")"
		<< "\n";
	*/
}

static void processHoistableAcquire(HoistableAcquireReleaseMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	// do we need it here ?
	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	Hoistable* pHoistable;

	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;
		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		hoistables.emplace(msg->hoistableGuid, upd);

		pHoistable = upd.pObject;
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;
		pHoistable = upd.pObject;
	}

	pHoistable->MakeHoistable(false);

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processHoistableRelease(HoistableAcquireReleaseMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	// do we need it here ?
	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	Hoistable* pHoistable;

	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;
		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		hoistables.emplace(msg->hoistableGuid, upd);

		pHoistable = upd.pObject;
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;
		pHoistable = upd.pObject;
	}

	pHoistable->MakeHoistable(true);

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processHoistableUpdate(HoistableStateMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;

		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		upd.x = msg->x;
		upd.y = msg->y;
		upd.z = msg->z;
		upd.yaw = msg->rotationY;
		upd.updated = true;

		hoistables.emplace(msg->hoistableGuid, upd);
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;

		upd.x = msg->x;
		upd.y = msg->y;
		upd.z = msg->z;
		upd.yaw = msg->rotationY;
		upd.updated = true;
	}

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processPushBlockAcquire(HoistableAcquireReleaseMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	// do we need it here ?
	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	Hoistable* pHoistable;

	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;
		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		hoistables.emplace(msg->hoistableGuid, upd);

		pHoistable = upd.pObject;
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;
		pHoistable = upd.pObject;
	}

	pHoistable->EnablePushBlock(false);

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processPushBlockRelease(HoistableAcquireReleaseMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	// do we need it here ?
	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	Hoistable* pHoistable;

	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;
		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		hoistables.emplace(msg->hoistableGuid, upd);

		pHoistable = upd.pObject;
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;
		pHoistable = upd.pObject;
	}

	pHoistable->EnablePushBlock(true);

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processPushBlockUpdate(HoistableStateMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
		return;

	EnterCriticalSection(&hoistablesCriticalSection);

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	if (hoistableIt == hoistables.end()) {
		HoistableUpdateStruct upd;

		upd.pObject = new Hoistable(processAnalyzer);
		upd.pObject->initializeByGuid(msg->hoistableGuid);
		upd.x = msg->x;
		upd.y = msg->y;
		upd.z = msg->z;
		upd.yaw = msg->rotationY;
		upd.updated = true;

		hoistables.emplace(msg->hoistableGuid, upd);
	}
	else {
		HoistableUpdateStruct& upd = hoistableIt->second;

		upd.x = msg->x;
		upd.y = msg->y;
		upd.z = msg->z;
		upd.yaw = msg->rotationY;
		upd.updated = true;
	}

	LeaveCriticalSection(&hoistablesCriticalSection);
}

static void processWebWallBreakIncoming(WebWallBreakMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->wallGuid == 0)
		return;

	EnterCriticalSection(&webWallBreakIncoming_CS);
	g_incomingWebWallBreaks.push_back({ msg->wallGuid, { msg->breakX, msg->breakY, msg->breakZ } });
	LeaveCriticalSection(&webWallBreakIncoming_CS);
}

static void processEnemiesUpdate(EnemiesStateMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel) return;

	std::unordered_map<uint64_t, Enemy> updates = msg->enemies;

	EnterCriticalSection(&enemiesCriticalSection);
	enemy_updates = updates;
	enemies_updated = true;
	LeaveCriticalSection(&enemiesCriticalSection);

	/*
	for (auto enemyUpdate : updates)
	{
		const auto enemyIt = enemies.find(enemyUpdate.first);
		if (enemyIt == enemies.end() || enemyIt->second == nullptr || !enemyIt->second->isValid())
			continue;

		NPC* badBoy = enemyIt->second;
		badBoy->setPosition(enemyUpdate.second.x, enemyUpdate.second.y, enemyUpdate.second.z);
		badBoy->setRotationY(enemyUpdate.second.rot);
		badBoy->setHealth(enemyUpdate.second.health);
		if (badBoy->getAnimation() != enemyUpdate.second.anim)
			badBoy->setNPCAnim(enemyUpdate.second.anim);

	}
	*/
}

static void processNicknameUpdate(NicknameUpdateMessage* msg)
{
	std::string newName = sanitizeIdentityValue(msg->new_name, sizeof(NicknameUpdateMessage::new_name));
	std::cout << "processNicknameUpdate\r\n";
	std::cout << "player GUID = " << msg->player_guid << "\r\n";
	std::cout << "new name = " << newName << "\r\n";

	pendingNicknames[msg->player_guid] = newName;

	Player* pl = findPlayerByGuid(msg->player_guid);
	if (pl) {
		std::cout << "playerFound\r\n";
		pl->nickname = newName;
		if (pl->nickname_marker) {
			std::cout << "markerFound\r\n";
			pl->nickname_marker->setText(newName.c_str());
		}
	}
}

static void processStatusUpdate(StatusUpdateMessage* msg)
{
	std::string newStatus = sanitizeIdentityValue(msg->new_status, sizeof(StatusUpdateMessage::new_status));
	std::cout << "processStatusUpdate\r\n";
	std::cout << "player GUID = " << msg->player_guid << "\r\n";
	std::cout << "new status = " << newStatus << "\r\n";

	pendingStatuses[msg->player_guid] = newStatus;

	Player* pl = findPlayerByGuid(msg->player_guid);
	if (pl) {
		std::cout << "playerFound\r\n";
		pl->status = newStatus;
		if (pl->status_marker) {
			std::cout << "markerFound\r\n";
			pl->status_marker->setText(newStatus.c_str());
		}
	}
}

static void processChatMessage(ChatMsgMessage* msg)
{
	std::string chatText = sanitizeIdentityValue(msg->msg, sizeof(ChatMsgMessage::msg));
	std::cout << "processChatMessage\r\n";
	std::cout << "player GUID = " << msg->player_guid << "\r\n";
	std::cout << "msg = " << chatText << "\r\n";

	std::string sender_name;

	if (msg->player_guid == myGuid) {
		sender_name = myNickname;
	}
	else {
		Player* pl = findPlayerByGuid(msg->player_guid);
		if (pl) {
			sender_name = pl->nickname;
		}
		else {
			sender_name = "Unknown";
		}
	}

	g_ChatOverlay.AddSystemMessage("[" + sender_name + "] " + chatText);
}

static void processStoneThrow(StoneThrowMessage* msg)
{
	if (msg->playerGuid == myGuid)
		return;

	PendingThrow p;
	p.guid = msg->playerGuid;
	p.from = { msg->fromX, msg->fromY, msg->fromZ };
	p.to = { msg->toX,   msg->toY,   msg->toZ };
	p.type = msg->stoneType;

	EnterCriticalSection(&throwCriticalSection);
	g_incomingThrows.push_back(p);
	LeaveCriticalSection(&throwCriticalSection);
}

static void processChestOpen(ChestOpenMessage* msg)
{
	if (msg->nowLevel != nowLevel || msg->chestGuid == 0)
		return;

	EnterCriticalSection(&chestCriticalSection);
	g_incomingChestOpens.push_back(msg->chestGuid);
	LeaveCriticalSection(&chestCriticalSection);
}

static void applyLocalIdentityMarkers()
{
	if (myGuid == 0)
		return;

	Marker nicknameMarker(processAnalyzer);
	nicknameMarker.initializeByGuid(myNicknameGuid);
	nicknameMarker.setText(myNickname.c_str());

	Marker statusMarker(processAnalyzer);
	statusMarker.initializeByGuid(myStatusGuid);
	statusMarker.setText(myStatus.c_str());
}

static void sendLocalNicknameUpdate(Client& client)
{
	if (myGuid == 0)
		return;

	auto* msg = static_cast<NicknameUpdateMessage*>(client.CreateMessage(NICKNAME_UPDATE));
	msg->player_guid = myGuid;
	strlcpy(msg->new_name, myNickname.c_str());
	client.SendMessage(channels::Gameplay, msg);
}

static void sendLocalStatusUpdate(Client& client)
{
	if (myGuid == 0)
		return;

	auto* msg = static_cast<StatusUpdateMessage*>(client.CreateMessage(STATUS_UPDATE));
	msg->player_guid = myGuid;
	strlcpy(msg->new_status, myStatus.c_str());
	client.SendMessage(channels::Gameplay, msg);
}

static void sendLocalIdentityToServer(Client& client)
{
	sendLocalNicknameUpdate(client);
	sendLocalStatusUpdate(client);
}

static void processMessage(Client& client, Message* message, double time)
{
	switch (message->GetType())
	{
	case POSITION_UPDATE:
		if (gameManager.isOnLevel()) processPositionUpdate(static_cast<PositionMessage*>(message), time);
		break;
	case ENEMIES_UPDATE:
		if (gameManager.isOnLevel()) processEnemiesUpdate(static_cast<EnemiesStateMessage*>(message), time);
		break;

	case HOISTABLE_ACQUIRE:
		if (gameManager.isOnLevel()) processHoistableAcquire(static_cast<HoistableAcquireReleaseMessage*>(message), time);
		break;
	case HOISTABLE_RELEASE:
		if (gameManager.isOnLevel()) processHoistableRelease(static_cast<HoistableAcquireReleaseMessage*>(message), time);
		break;
	case HOISTABLE_UPDATE:
		if (gameManager.isOnLevel()) processHoistableUpdate(static_cast<HoistableStateMessage*>(message), time);
		break;

	case PUSHBLOCK_ACQUIRE:
		if (gameManager.isOnLevel()) processPushBlockAcquire(static_cast<HoistableAcquireReleaseMessage*>(message), time);
		break;
	case PUSHBLOCK_RELEASE:
		if (gameManager.isOnLevel()) processPushBlockRelease(static_cast<HoistableAcquireReleaseMessage*>(message), time);
		break;
	case PUSHBLOCK_UPDATE:
		if (gameManager.isOnLevel()) processPushBlockUpdate(static_cast<HoistableStateMessage*>(message), time);
		break;

	case WEB_WALL_BREAK:
		if (gameManager.isOnLevel()) processWebWallBreakIncoming(static_cast<WebWallBreakMessage*>(message));
		break;

	case GUID_ASSIGN:
		myGuid = static_cast<GuidAssignMessage*>(message)->guid;
		localSkinUploadAttempted = false;
		printf("Assigned my GUID: %llu\n", myGuid);

		myNicknameGuid = (myGuid & 0xFFFFFFFF) | 0x0D8AD91100000000ull;
		myStatusGuid = (myGuid & 0xFFFFFFFF) | 0x0D8AD91200000000ull;
		applyLocalIdentityMarkers();
		sendLocalIdentityToServer(client);

		break;
	case SKIN_ANNOUNCE:
		processSkinAnnouncement(static_cast<SkinAnnouncementMessage*>(message));
		break;
	case SKIN_FILE_TRANSFER:
		processSkinFileTransfer(static_cast<SkinFileTransferMessage*>(message));
		break;
	case SKIN_CLEAR:
		processSkinClear(static_cast<SkinClearMessage*>(message));
		break;

	case NICKNAME_UPDATE:
		processNicknameUpdate(static_cast<NicknameUpdateMessage*>(message));
		break;
	case STATUS_UPDATE:
		processStatusUpdate(static_cast<StatusUpdateMessage*>(message));
		break;
	case CHAT_MESSAGE:
		processChatMessage(static_cast<ChatMsgMessage*>(message));
		break;
	case STONE_THROW:
		if (gameManager.isOnLevel()) processStoneThrow(static_cast<StoneThrowMessage*>(message));
		break;
	case CHEST_OPEN:
		if (gameManager.isOnLevel()) processChestOpen(static_cast<ChestOpenMessage*>(message));
		break;
	case PICKUP_COLLECT:
		if (gameManager.isOnLevel()) processPickupCollect(static_cast<PickupCollectMessage*>(message));
		break;
	case TRIGGER_ONPRESSB:
		if (gameManager.isOnLevel()) processTriggerOnPressB(static_cast<TriggerOnPressBMessage*>(message));
		break;
	case TRIGGER_ONUSE:
		if (gameManager.isOnLevel()) processTriggerOnUse(static_cast<TriggerOnUseMessage*>(message));
		break;
	case SWITCH_TOGGLE:
		if (gameManager.isOnLevel()) processSwitchToggle(static_cast<SwitchToggleMessage*>(message));
		break;
	case ANIM_SYNC:
		if (gameManager.isOnLevel()) processAnimSync(static_cast<AnimSyncMessage*>(message));
		break;
	case RING_SYNC:
		processRingSync(static_cast<RingSyncMessage*>(message));
		break;
	case SPAWN_OBJECT:
		if (gameManager.isOnLevel()) processSpawnObject(static_cast<SpawnObjectMessage*>(message));
		break;
	case SPAWN_FX:
		if (gameManager.isOnLevel()) processSpawnFx(static_cast<SpawnFxMessage*>(message));
		break;

	default:
		break;
	}
}

// chat commands
static yojimbo::Client* g_Client = nullptr;

static void ChatMessage(const std::string& message)
{
	if (!g_Client || myGuid == 0)
		return;

	std::string cleanMessage = sanitizeIdentityValue(message, sizeof(ChatMsgMessage::msg));
	if (cleanMessage.empty())
		return;

	// Send network message
	auto* xmsg = static_cast<ChatMsgMessage*>(g_Client->CreateMessage(CHAT_MESSAGE));
	xmsg->player_guid = myGuid;
	strlcpy(xmsg->msg, cleanMessage.c_str());
	g_Client->SendMessage(channels::Gameplay, xmsg);

	//	std::cout << "sent chat message\r\n";
	//	std::cout << "player GUID = " << myGuid << "\r\n";
	//	std::cout << "message = " << xmsg->msg << "\r\n";
}

static void ChatCommandNickname(const std::string& name)
{
	if (!g_Client || myGuid == 0)
		return;

	myNickname = sanitizeIdentityValue(name, sizeof(NicknameUpdateMessage::new_name));
	if (myNickname.empty())
	{
		g_ChatOverlay.AddSystemMessage("[System] Invalid nickname.");
		return;
	}

	applyLocalIdentityMarkers();
	sendLocalNicknameUpdate(*g_Client);
	if (!saveLocalPlayerProfile(myNickname, myStatus, myFakeBilboDamage))
		g_ChatOverlay.AddSystemMessage("[System] Failed to save name/status config.");
	g_ChatOverlay.AddSystemMessage("[System] Name changed to " + myNickname + ".");

	//	std::cout << "sent nickname update\r\n";
	//	std::cout << "player GUID = " << myGuid << "\r\n";
	//	std::cout << "new name = " << myNickname << "\r\n";
}

static void ChatCommandStatus(const std::string& status)
{
	if (!g_Client || myGuid == 0)
		return;

	myStatus = sanitizeIdentityValue(status, sizeof(StatusUpdateMessage::new_status));
	if (myStatus.empty())
	{
		g_ChatOverlay.AddSystemMessage("[System] Invalid status.");
		return;
	}

	applyLocalIdentityMarkers();
	sendLocalStatusUpdate(*g_Client);
	if (!saveLocalPlayerProfile(myNickname, myStatus, myFakeBilboDamage))
		g_ChatOverlay.AddSystemMessage("[System] Failed to save name/status config.");
	g_ChatOverlay.AddSystemMessage("[System] Status changed to " + myStatus + ".");
}

static void ChatCommandChangeAIMode(const std::string& aiMode)
{
	if (!g_Client || myGuid == 0)
		return;

	int parsedMode = 0;
	if (!parseBoundedInt(aiMode, 0, 3, parsedMode))
	{
		g_ChatOverlay.AddSystemMessage("[System] Invalid AI mode. Use 0, 1, 2, or 3.");
		return;
	}

	g_enemies_ai_mode = parsedMode;
	changeEnemiesAIMode(g_enemies_ai_mode);
	g_ChatOverlay.AddSystemMessage("[System] AI mode set to " + std::to_string(parsedMode) + ".");
}

static void ChatCommandDamage(const std::string& damage)
{
	if (!processAnalyzer)
		return;

	int parsedDamage = 0;
	if (parseBoundedInt(damage, 0, 65535, parsedDamage))
	{
		myFakeBilboDamage = sanitizeDamageValue(parsedDamage);
		applyFakeBilboDamage();
		if (!saveLocalPlayerProfile(myNickname, myStatus, myFakeBilboDamage))
			g_ChatOverlay.AddSystemMessage("[System] Failed to save damage config.");
		g_ChatOverlay.AddSystemMessage("[System] Fake Bilbo damage set to " + std::to_string(myFakeBilboDamage) + ".");
	}
	else
	{
		g_ChatOverlay.AddSystemMessage("[System] Invalid damage value. Use 0 through 65535.");
	}
}

static std::atomic<bool> g_reconnectRequested(false);

static void ChatCommandReconnect(const std::string&)
{
	g_ChatOverlay.AddSystemMessage("[System] Reconnect requested...");
	g_reconnectRequested.store(true);
}

static void ChatCommandSetTeam(const std::string& team)
{
	int teamId = 0;
	if (!parseBoundedInt(team, 0, 2, teamId))
	{
		g_ChatOverlay.AddSystemMessage("[System] Invalid team. Use 0, 1, or 2.");
		return;
	}

	for (auto player : activePlayers)
	{
		player.setTeam(teamId);
	}
	g_ChatOverlay.AddSystemMessage("[System] Team set to " + std::to_string(teamId) + ".");
}

// Broadcast this peer's current animation frames for every animated object on the
// level. Everyone else snaps to them once; looping keeps them in sync afterward.
static void ChatCommandSyncAnim(const std::string&)
{
	if (!g_Client || myGuid == 0)
	{
		g_ChatOverlay.AddSystemMessage("[System] Not connected.");
		return;
	}
	if (!gameManager.isOnLevel())
	{
		g_ChatOverlay.AddSystemMessage("[System] Not on a level.");
		return;
	}

	// The actual capture + send runs on the network thread in the client loop.
	g_animSyncRequested.store(true);
	g_ChatOverlay.AddSystemMessage("[System] Syncing animation frames...");
}

// Spawn a RigidInstance (synced to all players). Optional arg = template file name
// in ./Templates/ (e.g. "/spawn barrel.export"); with no arg a bare object spawns.
static void ChatCommandSpawn(const std::string& templateArg)
{
	if (!g_Client || myGuid == 0)
	{
		g_ChatOverlay.AddSystemMessage("[System] Not connected.");
		return;
	}
	if (!gameManager.isOnLevel())
	{
		g_ChatOverlay.AddSystemMessage("[System] Not on a level.");
		return;
	}

	std::string t = sanitizeIdentityValue(templateArg, sizeof(g_localSpawnTemplate));

	EnterCriticalSection(&spawnCriticalSection);
	strlcpy(g_localSpawnTemplate, t.c_str());
	LeaveCriticalSection(&spawnCriticalSection);

	// The actual spawn + broadcast runs on the game thread (calls engine functions).
	g_localSpawnRequested.store(true);
	g_ChatOverlay.AddSystemMessage(t.empty()
		? "[System] Spawning rigid instance..."
		: ("[System] Spawning rigid instance from " + t + "..."));
}

// Test trigger for the FX packet: play an FX (default "fx_fire") near the player
// and broadcast it to everyone. The "real" trigger will call SpawnFxSynced directly.
static void ChatCommandSpawnFx(const std::string& fxArg)
{
	if (!g_Client || myGuid == 0 || !gameManager.isOnLevel())
	{
		g_ChatOverlay.AddSystemMessage("[System] Not connected / not on a level.");
		return;
	}

	std::string name = sanitizeIdentityValue(fxArg, sizeof(SpawnFxMessage::fxName));
	if (name.empty())
		name = "fx_fire";

	Vector3 bp = getBilboPos();
	SpawnFxSynced(name.c_str(), bp.x + 100.0f, bp.y, bp.z);
	g_ChatOverlay.AddSystemMessage("[System] Spawned FX '" + name + "' (synced).");
}

// ===========================================================================
//  Client Main Loop
// ===========================================================================

static int clientMain()
{
	printf("\nConnecting client with a server-issued token...\n");

	double lastSend = g_time;

	// Read server address
	std::string serverIP = readSecureServerIP();

	// Connect
	ClientServerConfig config;
	ConfigureGameNetworking(config);
	Client client(GetDefaultAllocator(), Address("0.0.0.0"), config, gameAdapter, g_time);

	// add chat commands
	g_Client = &client;
	g_ChatOverlay.SetMsgCallback(ChatMessage);
	g_ChatOverlay.AddCommand("/name", "<nickname> - Change your nickname", ChatCommandNickname);
	g_ChatOverlay.AddCommand("/status", "<status> - Change your status", ChatCommandStatus);
	g_ChatOverlay.AddCommand("/ai", "<mode> - Change AI mode", ChatCommandChangeAIMode);
	g_ChatOverlay.AddCommand("/damage", "<value> - Set damage of fake Bilbo", ChatCommandDamage);
	g_ChatOverlay.AddCommand("/reconnect", "- Try to reconnect to the server", ChatCommandReconnect);
	g_ChatOverlay.AddCommand("/setTeam", "<0,1,2> - Set fake Bilbo's team", ChatCommandSetTeam);
	g_ChatOverlay.AddCommand("/syncanim", "- Sync animation frame of all animated objects on the level", ChatCommandSyncAnim);
	g_ChatOverlay.AddCommand("/spawn", "[template] - Spawn a rigid instance (synced to all players)", ChatCommandSpawn);
	g_ChatOverlay.AddCommand("/spawnfx", "[fxName] - Play an FX effect (synced to all players)", ChatCommandSpawnFx);

	// try to hook bilbo's OnAdvanceLogic
	InitializeCriticalSection(&playersCriticalSection);
	InitializeCriticalSection(&hoistablesCriticalSection);
	InitializeCriticalSection(&enemiesCriticalSection);
	InitializeCriticalSection(&throwCriticalSection);
	InitializeCriticalSection(&webWallBreak_CS);
	InitializeCriticalSection(&webWallBreakIncoming_CS);
	InitializeCriticalSection(&chestCriticalSection);
	InitializeCriticalSection(&pickupCriticalSection);
	InitializeCriticalSection(&triggerCriticalSection);
	InitializeCriticalSection(&switchCriticalSection);
	InitializeCriticalSection(&animSyncCriticalSection);
	InitializeCriticalSection(&ringCriticalSection);
	InitializeCriticalSection(&spawnCriticalSection);
	InitializeCriticalSection(&fxCriticalSection);
	SetupBilboHook();

	signal(SIGINT, interruptHandler);

	bool firstConnectAttempt = true;
	bool connectionMessageShown = false;

	// --- Game loop ---
	while (!quit)
	{
		if (firstConnectAttempt)
		{
			firstConnectAttempt = false;
			uint64_t clientId = 0;
			uint8_t connectToken[ConnectTokenBytes] = {};
			if (!SecureConnect::requestConnectTokenFromServer(serverIP, clientId, connectToken))
			{
				printf("Failed to request connect token from %s:%d\n", serverIP.c_str(), SecureConnect::ConnectTokenPort);
				g_ChatOverlay.AddSystemMessage("[System] Failed to request connect token from server.");
			}
			else
			{
				printf("Client ID from server token: %.16" PRIx64 "\n", clientId);
				client.Connect(clientId, connectToken);

				char addrStr[256];
				client.GetAddress().ToString(addrStr, sizeof(addrStr));
				printf("Client address: %s\n", addrStr);
			}
		}

		if (g_reconnectRequested.exchange(false))
		{
			connectionMessageShown = false;
			g_ChatOverlay.AddSystemMessage("[System] Attempting to reconnect...");

			client.Disconnect();
			resetClientSessionState();

			uint64_t clientId = 0;
			uint8_t connectToken[ConnectTokenBytes] = {};
			if (!SecureConnect::requestConnectTokenFromServer(serverIP, clientId, connectToken))
			{
				printf("Failed to request connect token from %s:%d for reconnect\n", serverIP.c_str(), SecureConnect::ConnectTokenPort);
				g_ChatOverlay.AddSystemMessage("[System] Reconnect failed: couldn't get token from server.");
			}
			else
			{
				printf("Reconnecting client with Client ID: %.16" PRIx64 "\n", clientId);
				client.Connect(clientId, connectToken);
				g_ChatOverlay.AddSystemMessage("[System] Reconnecting to server...");
			}
		}

		if (client.IsConnected() || client.IsConnecting())
		{
			client.SendPackets();
			client.ReceivePackets();

			if (gameManager.isOnLevel() && !processedDataForThisLevel)
			{
				readGamePointers();
				processedDataForThisLevel = true;
			}
			if (!gameManager.isOnLevel())
			{
				processedDataForThisLevel = false;
				clearActivePlayers();
				clearEnemies();

				EnterCriticalSection(&chestCriticalSection);
				allChests.clear();
				g_outgoingChestOpens.clear();
				g_incomingChestOpens.clear();
				LeaveCriticalSection(&chestCriticalSection);

				EnterCriticalSection(&pickupCriticalSection);
				pickupStatesCache.clear();
				g_outgoingPickupCollects.clear();
				g_incomingPickupCollects.clear();
				LeaveCriticalSection(&pickupCriticalSection);

				EnterCriticalSection(&triggerCriticalSection);
				triggerStatesCache.clear();
				triggerSuppliedItemCountCache.clear();
				g_outgoingTriggerPressB.clear();
				g_incomingTriggerPressB.clear();
				g_outgoingTriggerOnUse.clear();
				g_incomingTriggerOnUse.clear();
				g_suppressTriggerDetection.clear();
				LeaveCriticalSection(&triggerCriticalSection);

				EnterCriticalSection(&switchCriticalSection);
				switchStatesCache.clear();
				g_outgoingSwitchToggles.clear();
				g_incomingSwitchToggles.clear();
				LeaveCriticalSection(&switchCriticalSection);

				EnterCriticalSection(&webWallBreak_CS);
				g_haveWebWallBreak = false;
				g_lastWebWallGuid = 0;
				g_lastWebWallBreakPoint = {};
				g_sentWebWallBreaks.clear();
				LeaveCriticalSection(&webWallBreak_CS);

				EnterCriticalSection(&webWallBreakIncoming_CS);
				g_incomingWebWallBreaks.clear();
				g_appliedWebWallBreaks.clear();
				LeaveCriticalSection(&webWallBreakIncoming_CS);
			}

			// Process incoming messages on all channels
			for (int ch = 0; ch < config.numChannels; ch++)
			{
				Message* msg = client.ReceiveMessage(ch);
				while (msg)
				{
					processMessage(client, msg, g_time);

					client.ReleaseMessage(msg);
					msg = client.ReceiveMessage(ch);
				}
			}

			sendLocalSkinData(client);

			// Send local player state at the configured tick rate
			if (client.IsConnected() && myGuid != 0)
			{
				// Event-driven: if the local player threw a stone, send it immediately
				if (g_haveThrow)
				{
					EnterCriticalSection(&throwCriticalSection);
					Vector3 from = g_lastThrowFrom;
					Vector3 to = g_lastThrowTo;
					uint8_t type = g_lastThrowType;
					g_haveThrow = false;
					LeaveCriticalSection(&throwCriticalSection);

					auto* tmsg = static_cast<StoneThrowMessage*>(client.CreateMessage(STONE_THROW));
					tmsg->playerGuid = myGuid;
					tmsg->fromX = NetworkClamp::sanitizePosition(from.x);
					tmsg->fromY = NetworkClamp::sanitizePosition(from.y);
					tmsg->fromZ = NetworkClamp::sanitizePosition(from.z);
					tmsg->toX = NetworkClamp::sanitizePosition(to.x);
					tmsg->toY = NetworkClamp::sanitizePosition(to.y);
					tmsg->toZ = NetworkClamp::sanitizePosition(to.z);
					tmsg->stoneType = type;
					tmsg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
					client.SendMessage(channels::Gameplay, tmsg);
				}

				if (gameManager.isOnLevel())
				{
					EnterCriticalSection(&chestCriticalSection);
					std::vector<uint64_t> chestOpens = g_outgoingChestOpens;
					g_outgoingChestOpens.clear();
					LeaveCriticalSection(&chestCriticalSection);

					for (uint64_t chestGuid : chestOpens)
					{
						auto* cmsg = static_cast<ChestOpenMessage*>(client.CreateMessage(CHEST_OPEN));
						if (!cmsg)
							continue;

						cmsg->chestGuid = chestGuid;
						cmsg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
						client.SendMessage(channels::Gameplay, cmsg);
					}

					// Detect and send pickup collection events
					detectPickupChanges();
					sendPickupCollects(client);

					// Detect and send trigger activation events
					detectTriggerChanges();
					sendTriggerPressB(client);
					sendTriggerOnUse(client);

					// Detect and send switch toggle events
					detectSwitchChanges();
					sendSwitchToggles(client);

					// Send a one-shot animation-frame sync if /syncanim was used
					sendAnimSync(client);

					// Broadcast our ring state when it changes
					sendRingSync(client);

					// Broadcast any objects we spawned via /spawn
					sendSpawns(client);

					// Broadcast any FX effects triggered locally
					sendFxSpawns(client);
				}

				if (g_time - lastSend > NetDefaults::SEND_INTERVAL && gameManager.isOnLevel())
				{
					readLocalPlayerState();

					auto* msg = static_cast<PositionMessage*>(client.CreateMessage(POSITION_UPDATE));
					msg->x = NetworkClamp::sanitizePosition(localPos.x);
					msg->y = NetworkClamp::sanitizePosition(localPos.y);
					msg->z = NetworkClamp::sanitizePosition(localPos.z);
					msg->rotationY = NetworkClamp::sanitizeRotationRadians(localRot.y);
					msg->animation = NetworkClamp::sanitizeAnimation(localAnimation);
					msg->animFrame = NetworkClamp::sanitizeAnimationFrame(localAnimFrame);
					msg->lastAnimFrame = NetworkClamp::sanitizeAnimationFrame(localLastAnimFrame);
					msg->playerGuid = myGuid;

					msg->bilboWeapon = NetworkClamp::sanitizeWeapon(bilboWeapon);
					msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

					client.SendMessage(channels::Gameplay, msg);

					if (isHost == 1)
					{
						//ENEMIES
						std::unordered_map<uint64_t, Enemy> enemiesPosSnap = readEnemiesState();
						auto* msgEnemy = static_cast<EnemiesStateMessage*>(client.CreateMessage(ENEMIES_UPDATE));
						msgEnemy->enemies = enemiesPosSnap;
						msgEnemy->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
						client.SendMessage(channels::Gameplay, msgEnemy);
					}

					if (isHost == 0)
						changeEnemiesAIMode(g_enemies_ai_mode);

					lastSend = g_time;
				}

				// Send web wall break events
				if (gameManager.isOnLevel())
				{
					sendWebWallBreak(client);
				}
			}

			// update hoistable
			{
				bilbo* pBilbo = *((bilbo**)X_POSITION_PTR);
				if (pBilbo) {
					guid hoist_guid = pBilbo->_get_nearest_hoistable();

					if (!g_currentHoistable && hoist_guid.Guid != 0 && pBilbo->_get_state() == BS_HOISTING) {
						g_currentHoistable = new Hoistable(processAnalyzer);
						g_currentHoistable->initializeByGuid(hoist_guid.Guid);

						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(HOISTABLE_ACQUIRE));
						msg->hoistableGuid = g_currentHoistable->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);
						std::cout << "hoistable acquire\r\n";
					}

					if (g_currentHoistable && (hoist_guid.Guid == 0 || pBilbo->_get_state() != BS_HOISTING)) {
						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(HOISTABLE_RELEASE));
						msg->hoistableGuid = g_currentHoistable->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);

						delete g_currentHoistable;
						g_currentHoistable = nullptr;
						std::cout << "hoistable release\r\n";
					}

					if (g_currentHoistable) {
						auto* msgHoistable = static_cast<HoistableStateMessage*>(client.CreateMessage(HOISTABLE_UPDATE));
						Vector3 v = g_currentHoistable->getPosition();
						msgHoistable->x = NetworkClamp::sanitizePosition(v.x);
						msgHoistable->y = NetworkClamp::sanitizePosition(v.y);
						msgHoistable->z = NetworkClamp::sanitizePosition(v.z);
						msgHoistable->rotationY = NetworkClamp::sanitizeRotationRadians(g_currentHoistable->getRotationY());
						msgHoistable->hoistableGuid = g_currentHoistable->getGUID();
						msgHoistable->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msgHoistable);
					}
				}
			}

			// update pushblock
			{
				bilbo* pBilbo = *((bilbo**)X_POSITION_PTR);
				if (pBilbo) {
					guid pb_guid = pBilbo->_get_nearest_pushblock();

					/* is acquiring reliable ? */
					if (!g_currentPushBlock && pb_guid.Guid != 0 && pBilbo->_get_state() == BS_PUSH_BLOCK) {
						g_currentPushBlock = new Hoistable(processAnalyzer);
						g_currentPushBlock->initializeByGuid(pb_guid.Guid);

						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(PUSHBLOCK_ACQUIRE));
						msg->hoistableGuid = g_currentPushBlock->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);
						std::cout << "pushblock acquire\r\n";
					}

					if (g_currentPushBlock && pBilbo->_get_state() != BS_PUSH_BLOCK) {
						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(PUSHBLOCK_RELEASE));
						msg->hoistableGuid = g_currentPushBlock->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);

						delete g_currentPushBlock;
						g_currentPushBlock = nullptr;
						std::cout << "pushblock release\r\n";
					}

					if (g_currentPushBlock) {
						auto* msgUpdate = static_cast<HoistableStateMessage*>(client.CreateMessage(PUSHBLOCK_UPDATE));
						Vector3 v = g_currentPushBlock->xGetPosition();
						msgUpdate->x = NetworkClamp::sanitizePosition(v.x);
						msgUpdate->y = NetworkClamp::sanitizePosition(v.y);
						msgUpdate->z = NetworkClamp::sanitizePosition(v.z);
						msgUpdate->rotationY = NetworkClamp::sanitizeRotationRadians(g_currentPushBlock->getRotationY());
						msgUpdate->hoistableGuid = g_currentPushBlock->getGUID();
						msgUpdate->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						printf("SENT UPDATE PUSHBOX %.2f %.2f %.2f\n", msgUpdate->x, msgUpdate->y, msgUpdate->z);

						client.SendMessage(channels::Gameplay, msgUpdate);
					}
				}
			}
		}
		else
		{
			if (!connectionMessageShown)
			{
				connectionMessageShown = true;
				if (client.ConnectionFailed())
				{
					g_ChatOverlay.AddSystemMessage("[System] Connection failed! Type /reconnect to try again.");
				}
				else
				{
					g_ChatOverlay.AddSystemMessage("[System] Disconnected from server. Type /reconnect to try again.");
				}
				resetClientSessionState();
			}
		}

		g_time += NetDefaults::DELTA_TIME;
		client.AdvanceTime(g_time);

		yojimbo_sleep(NetDefaults::DELTA_TIME);
	}

	client.Disconnect();
	resetClientSessionState();
	g_Client = nullptr;
	return 0;
}

// ===========================================================================
//  Entry Point
// ===========================================================================

DWORD WINAPI mainThread(LPVOID)
{
	//-----------------------------------------------------
	// DEBUG CONSOLE
	//-----------------------------------------------------

	AllocConsole();

	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);

	std::cout << "Injected.\n";

	//-----------------------------------------------------
	// START GAME MANAGER
	//-----------------------------------------------------

	gameManager.start();

	processAnalyzer =
		gameManager.getHobbitProcessAnalyzer();

	//-----------------------------------------------------
	// FIND GAME WINDOW
	//-----------------------------------------------------

	g_ChatOverlay.Init();

	//-----------------------------------------------------
	// LOAD GUIDS
	//-----------------------------------------------------

	playerGuids =
		loadGuidsFromFile(
			"FAKE_BILBO_GUID.txt");

	std::cout << "GUID count: "
		<< playerGuids.size()
		<< "\n";

	//-----------------------------------------------------
	// LOAD SKIN CONFIG
	//-----------------------------------------------------

	localSkinLoaded =
		loadLocalSkinDefinitionFromKnownPaths(
			localSkinDefinition,
			localSkinConfigPath,
			localSkinConfigError);

	if (localSkinLoaded)
	{
		std::cout
			<< "Loaded skin config from: "
			<< localSkinConfigPath
			<< "\n";
	}
	else
	{
		std::cout
			<< "Skin sync disabled: "
			<< localSkinConfigError
			<< "\n";
	}

	std::string profileError;
	loadLocalPlayerProfile(myNickname, myStatus, myFakeBilboDamage, &profileError);
	if (!profileError.empty())
	{
		std::cout
			<< "Profile config disabled: "
			<< profileError
			<< "\n";
	}
	else
	{
		std::cout
			<< "Default name/status/damage: "
			<< myNickname
			<< " / "
			<< myStatus
			<< " / "
			<< myFakeBilboDamage
			<< "\n";
	}

	//-----------------------------------------------------
	// HOST INPUT
	//-----------------------------------------------------

	std::cout
		<< "Are you the host? "
		<< "(yes = 1 / no = 0)\n";

	std::cin >> isHost;

	//-----------------------------------------------------
	// INIT YOJIMBO
	//-----------------------------------------------------

	if (!InitializeYojimbo())
	{
		std::cout
			<< "Failed to initialize Yojimbo.\n";

		return 1;
	}

	yojimbo_log_level(
		YOJIMBO_LOG_LEVEL_INFO);

	srand(
		static_cast<unsigned int>(
			::time(NULL)));

	//-----------------------------------------------------
	// START NETWORKING
	//-----------------------------------------------------

	int result = clientMain();

	//-----------------------------------------------------
	// SHUTDOWN
	//-----------------------------------------------------

	ShutdownYojimbo();

	//-----------------------------------------------------
	// SHUTDOWN CHAT OVERLAY
	//-----------------------------------------------------

	g_ChatOverlay.Shutdown();

	//-----------------------------------------------------
	// CLEANUP
	//-----------------------------------------------------

	FreeConsole();

	FreeLibraryAndExitThread(
		moduleInstance,
		0);

	return result;
}


BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD fdwReason, LPVOID)
{
	DisableThreadLibraryCalls(hInstance);

	if (fdwReason == DLL_PROCESS_ATTACH) {
		moduleInstance = hInstance;
		CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)mainThread, NULL, 0, NULL));
	}

	return TRUE;
}
