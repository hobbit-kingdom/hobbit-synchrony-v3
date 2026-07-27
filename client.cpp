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
#include "DebugLog.h"

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
static std::string localConfigPath;
static std::string localSkinError;
static std::unordered_map<uint64_t, std::string> pendingNicknames;
static std::unordered_map<uint64_t, std::string> pendingStatuses;

// --- Animation data caches ---
std::unordered_map<uint32_t, uint32_t> animDataMap;
static std::unordered_map<uint32_t, float> animFrameRanges;

CRITICAL_SECTION enemiesCriticalSection;
std::unordered_map<uint64_t, Enemy> enemy_updates;
bool enemies_updated = false;

// Every NPC on the level, keyed by GUID — including neutral ones. Which of them are
// actually synced is decided per tick from their current team, not at discovery time.
std::unordered_map<uint64_t, NPC*> enemies;
const uint32_t X_POSITION_PTR = 0x0075BA3C; // address of bilbo *g_pBilbo variable

// Class signature the engine stamps into object+0x10 for NPC objects (from the
// obj_mgr class registry entry for type tag 0x1C).
static constexpr uint32_t NPC_CLASS_SIGNATURE = 0x04004232;

static int g_enemies_ai_mode = 0;

// NPCs whose local AI we switched off because they were on a synced team. Kept so
// that if a scripted event drops one back to neutral we can hand it back to its own
// AI, and so we never re-enable AI on an NPC we never touched.
static std::unordered_set<uint64_t> g_aiSuppressed;

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
static void applyQueuedCinemaStarts();
static void detectLocalRingChange(void* bilboThis);
static void applyPlayerRingVisuals();
static void refreshRemoteSenseSnapshot();
static void refreshRemotePainSnapshot();
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
	applyQueuedCinemaStarts();

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

		// NPC sight: refresh what the sense hooks are allowed to see this frame.
		refreshRemoteSenseSnapshot();

		// Damage types: refresh who the fake bilbos are and which melee weapon
		// their pain events should be attributed to this frame.
		refreshRemotePainSnapshot();

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
				dprintf("UPDATE PUHSHBOX %.2f  %.2f  %.2f\n", upd.x, upd.y, upd.z);
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

			// Shield break. Only the machine running this NPC's AI shatters it, so
			// the host's answer is the authority. The hasShield() guard makes this
			// fire exactly once per break (ShatterShield clears the GUID) and lets
			// the state repair itself for anyone who joined or reloaded after it.
			if (!enemyUpdate.second.shieldIntact && badBoy->hasShield())
				badBoy->shatterShield();

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

			dprintf("Applied remote web wall break: GUID %llu\n", wb.guid);
		}
	}
}

void InstallStoneHook();   // forward decl (defined further below)
void InstallChestHook();   // forward decl (defined further below)
void InstallFxHook();      // forward decl (defined further below)
void InstallWebWallHook(); // forward decl (defined further below)
void InstallSenseHooks();  // forward decl (defined further below)
void InstallPainHooks();   // forward decl (defined further below)
void InstallCinemaHook();  // forward decl (defined further below)
void InstallLayerHooks();  // forward decl (defined further below)

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
	InstallFxHook();      // detour fx_object::FireAndForget to sync lockpick-failure explosions
	InstallWebWallHook(); // detour web_wall::StartBreakAtPoint to capture web cuts
	InstallSenseHooks();  // detour SenseController::CanSee + UpdateSensedArray so that
	                      // enemies need line of sight (and lose the ring) on remote players
	InstallPainHooks();   // detour event_mgr::ApplyPainFromTable + GeneratePain so remote
	                      // players' attacks deal their real sting/stick/fire/freeze damage
	InstallCinemaHook();  // detour cinema::Start to share whitelisted cutscenes
	InstallLayerHooks();  // detour load_trigger::Execute so the host re-broadcasts
	                      // anim frames when a layer streams in
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

// Where the client's settings file may live: current folder, game exe folder,
// DLL folder - the current name in all three first, then the pre-rename name, so
// a new config always wins over a leftover skin_config.txt.
static std::vector<std::string> buildLocalConfigCandidates()
{
	std::vector<std::string> candidates;

	const std::string exeDirectory = getModuleDirectory(nullptr);
	const std::string dllDirectory = getModuleDirectory(moduleInstance);

	for (const char* fileName : { SkinSync::LocalConfigFile, SkinSync::LegacyLocalConfigFile })
	{
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(fileName));

		if (!exeDirectory.empty())
			appendUniqueConfigCandidate(candidates, SkinSync::fs::path(exeDirectory) / fileName);

		if (!dllDirectory.empty())
			appendUniqueConfigCandidate(candidates, SkinSync::fs::path(dllDirectory) / fileName);
	}

	return candidates;
}

static bool loadLocalSkinDefinitionFromKnownPaths(SkinSync::LocalSkinDefinition& outSkin, std::string& loadedPath, std::string& errorMessage)
{
	std::vector<std::string> candidates = buildLocalConfigCandidates();

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
		errorMessage = std::string(SkinSync::LocalConfigFile)
			+ " was not found in the current folder, game exe folder, or DLL folder";
	}

	return false;
}

// The settings file every non-skin reader/writer uses. Prefers the one the skin
// loader already opened, then the first candidate that exists; if none does, the
// current name in the current folder, so a first save creates the right file.
static std::string resolveLocalProfileConfigPath()
{
	if (!localConfigPath.empty())
		return localConfigPath;

	std::vector<std::string> candidates = buildLocalConfigCandidates();

	for (const auto& candidate : candidates)
	{
		std::error_code error;
		if (SkinSync::fs::exists(SkinSync::fs::path(candidate), error))
			return candidate;
	}

	return candidates.empty() ? std::string(SkinSync::LocalConfigFile) : candidates.front();
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

// Read one setting out of the config file, accepting any of the given key
// spellings. Returns an empty string when the key is absent; the last occurrence
// wins, matching how the profile loader treats duplicates.
//
// Used for the settings that are needed on their own, before or without the
// skin/profile loaders: they still work when the file has neither skin nor
// profile keys in it.
static std::string readLocalConfigValue(std::initializer_list<const char*> keys)
{
	std::ifstream configFile(resolveLocalProfileConfigPath());
	if (!configFile.is_open())
		return {};

	std::string result;
	std::string line;
	while (std::getline(configFile, line))
	{
		line = SkinSync::trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;

		const size_t separator = line.find('=');
		if (separator == std::string::npos)
			continue;

		std::string key = SkinSync::trim(line.substr(0, separator));
		std::transform(key.begin(), key.end(), key.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

		for (const char* wanted : keys)
		{
			if (key == wanted)
				result = SkinSync::stripQuotes(SkinSync::trim(line.substr(separator + 1)));
		}
	}

	return result;
}

/// Address of the server to join, or empty when the config does not set one.
static std::string readConfiguredServerIp()
{
	return readLocalConfigValue({ "server_ip", "server", "ip" });
}

// Console logging is off by default so a normal session stays quiet; "debug=1"
// turns the development output back on. Loaded before anything else, because
// almost everything that logs runs before the profile is read.
//
// saveLocalPlayerProfile only rewrites the name/status/damage lines and copies
// everything else through, so /name and /damage cannot clobber this setting.
static void loadDebugLoggingFlag()
{
	std::string value = readLocalConfigValue({ "debug", "debug_log", "logging" });
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

	g_debugLogging = (value == "1" || value == "true" || value == "yes" || value == "on");
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
	g_aiSuppressed.clear();
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
	dprintf("List Enemies\n");

	//
	hoistables.clear();

	// Every NPC on the level, found by object type tag the same way the other object
	// kinds are found — NOT by team. Teams are not fixed: level scripts flip NPCs
	// between neutral and a fighting side while the level runs, and a team-based scan
	// would permanently miss anyone who was neutral at load time. We take the whole
	// population once, then decide per frame who is worth syncing (see readEnemiesState).
	std::vector<uint32_t> allNpcAddrs = processAnalyzer->findAllGameObjByPattern<uint8_t>(OBJ_TYPE_NPC, OBJ_TYPE_OFFSET);
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


	dcout() << "SIZE OF ALL RIGID: " << allRigidInstances.size() << '\n';

	dcout() << "CHESTS AMOUNT: " << chestCount << "\n";

	for (uint32_t e : allNpcAddrs)
	{
		// Per-instance class signature written by the NPC descriptor. Cheap double
		// check that the type tag wasn't a coincidence on a recycled object slot.
		if (processAnalyzer->readData<uint32_t>(e + 0x10) != NPC_CLASS_SIGNATURE)
			continue;

		uint64_t eGuid = processAnalyzer->readData<uint64_t>(e + 0x8);

		// Remote players' fake bilbos are NPC objects too, so they land in this scan.
		// They are driven by the player sync path and must never be treated as AI.
		bool skip = false;
		for (uint64_t pGuid : playerGuids) if (pGuid == eGuid) skip = true;


		if (skip) continue;

		if (0xABCABCABCABCABC0 == eGuid)
		{
			dprintf("YOU ARE SETTING BILBO AS ENEMY NPC!!!");
			continue;
		}

		//hex
		dcout() << eGuid << " Address: " << e
			<< " Team: " << processAnalyzer->readData<uint32_t>(e + OBJ_TEAM_OFFSET)
			<< " Health: " << processAnalyzer->readData<float>(e + 0x290) << '\n';

		NPC* enemy = new NPC(processAnalyzer);
		enemy->initializeByAddress(e);
		// AI is deliberately NOT touched here. Whether this NPC is host-driven depends
		// on its team right now, which can change later, so that call lives in the
		// per-tick changeEnemiesAIMode() instead.

		enemies.emplace(enemy->getGUID(), enemy);
	}

	dprintf("NPCs tracked: %d", enemies.size());
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

	dprintf("[stone] type 0x%02X from (%.1f, %.1f, %.1f)  ->  TO (%.1f, %.1f, %.1f)\n",
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
		// `enemies` holds every NPC on the level. Only the ones currently on a
		// fighting side are host-driven; neutral NPCs run their own AI on each
		// client and are left out of the packet entirely. Re-read every tick so an
		// NPC that a script just turned hostile starts being sent immediately.
		if (!enemy.second->isNpcType())
			continue;
		if (!isSyncedTeam(enemy.second->getTeam()))
			continue;

		Vector3 ePos = enemy.second->getPosition();
		float eRot = enemy.second->getRotationY();
		uint32_t eAnim = enemy.second->getAnimation();
		float eHealth = enemy.second->getHealth();
		bool eShield = enemy.second->hasShield();

		temp[enemy.first] = NetworkClamp::sanitizeEnemy(
			{ ePos.x, ePos.y, ePos.z, eRot, eAnim, eHealth, eShield });
	}

	return temp;
}

// Clients run this every send tick, which is also what makes team changes take
// effect: an NPC only has its AI suppressed while it sits on a synced team, and gets
// its own AI back the moment a script returns it to neutral.
static void changeEnemiesAIMode(int mode)
{
	// Levels that must keep their own AI untouched: 1, and 9 (Smaug).
	if (nowLevel == 1 || nowLevel == 9)
		return;

	for (auto enemy : enemies)
	{
		NPC* npc = enemy.second;
		if (npc == nullptr || !npc->isValid() || !npc->isNpcType())
			continue;

		if (isSyncedTeam(npc->getTeam()))
		{
			npc->setAIMode(mode);
			g_aiSuppressed.insert(enemy.first);
		}
		else if (g_aiSuppressed.erase(enemy.first) != 0)
		{
			// Was host-driven, now neutral — give it back the AI flags it had when
			// we found it. NPCs we never suppressed are left completely alone.
			npc->restoreAIState();
		}
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

			dprintf("Local web wall cut: GUID %llu at (%.1f, %.1f, %.1f)\n",
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
				dprintf("Pickup collected locally: GUID %llu\n", guid);
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
		dprintf("Applied remote pickup collect: GUID %llu\n", pickupGuid);
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

// Triggers that must stay purely local. Each client fires these on its own; syncing
// them causes a double activation.
static bool isSyncExcludedTrigger(uint64_t guid)
{
	switch (guid)
	{
	case 0xCA3DDF12B1857C01ULL:   // CA3DDF12_B1857C01
	case 0xCA3DDC4FE3C7A400ULL:   // CA3DDC4F_E3C7A400
		return true;
	default:
		return false;
	}
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
	dprintf("Trigger OnUse detected: GUID %llu, %d items\n", guid, itemCount);
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

		// Never broadcast this one - it is local-only on every client
		if (isSyncExcludedTrigger(guid))
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
			dprintf("Trigger fired locally (OnPressB): GUID %llu\n", guid);
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

	// The suppression is meant to cover exactly one detection pass after a remote
	// apply, and this is the end of that pass. It used to be inserted and never
	// erased (only cleared on level change), which silently made a trigger
	// one-directional forever: once a peer's activation had been applied here, our
	// own later activations of it were skipped and never broadcast.
	g_suppressTriggerDetection.clear();

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
		if (isSyncExcludedTrigger(triggerGuid))
			continue;   // local-only: ignore it even if a peer sends it

		uint32_t addr = processAnalyzer->findGameObjByGUID(triggerGuid);
		if (addr == 0 || !isTriggerObject(addr))
			continue;

		// Suppress detection for this trigger so we don't echo it back. Without
		// this, applying a remote press leaves triggerStatesCache holding the
		// pre-fire flags, so the next detectTriggerChanges() sees a fresh rising
		// edge on WAS_TRIGGERED and broadcasts it straight back at the sender.
		// One-shot triggers latch and the ping-pong dies out; a RE-TRIGGERABLE one
		// re-arms after every fire, so each bounce produces another edge and the
		// two clients bat it back and forth forever. (Same guard OnUse already had.)
		EnterCriticalSection(&triggerCriticalSection);
		g_suppressTriggerDetection.insert(triggerGuid);
		LeaveCriticalSection(&triggerCriticalSection);

		// Enable the trigger and fire OnPressB
		uint32_t flags = getTriggerFlags(addr);
		if (!(flags & TRIGGER_ENABLED_FLAG))
		{
			flags |= TRIGGER_ENABLED_FLAG;
			processAnalyzer->writeData<uint32_t>(addr + 0x120, flags);
		}

		game_TriggerOnPressB(reinterpret_cast<void*>(addr));

		// Snapshot post-apply state so detection won't echo this back
		EnterCriticalSection(&triggerCriticalSection);
		triggerStatesCache[triggerGuid] = getTriggerFlags(addr);
		LeaveCriticalSection(&triggerCriticalSection);

		dprintf("Applied remote trigger OnPressB: GUID %llu\n", triggerGuid);
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
		if (isSyncExcludedTrigger(use.triggerGuid))
			continue;   // local-only: ignore it even if a peer sends it

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

		dprintf("Applied remote trigger OnUse: GUID %llu, %d items (total %d)\n", use.triggerGuid, use.itemCount, newCount);
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
			dprintf("Switch toggled locally: GUID %llu -> %s\n", guid, nowOn ? "ON" : "OFF");
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
		dprintf("Applied remote switch toggle: GUID %llu -> %s\n", entry.first, entry.second ? "ON" : "OFF");
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

// Engine object list (same globals HobbitProcessAnalyzer uses).
static constexpr uint32_t OBJ_LIST_PTR_ADDR   = 0x0076F648; // -> object records
static constexpr uint32_t OBJ_LIST_COUNT_ADDR = 0x0076F660; // record count
static constexpr uint32_t OBJ_RECORD_SIZE     = 0x14;       // stride per record
static constexpr uint32_t MAX_SCAN_OBJECTS    = 65536;      // sanity bound
static constexpr uint32_t MAX_ANIM_ENTRIES    = 4096;       // scratch capacity

struct AnimObjEntry
{
	uint32_t addr;   // rigid_instance*
	uint64_t guid;   // object GUID
	float    frame;  // current anim frame
};

// One direct pass over the engine's object list collecting every ANIMATED
// rigid_instance. Used by both the sender (needs guid+frame) and the receiver
// (needs addr+guid), so the whole feature costs a single O(N) walk per side.
//
// Reads are plain pointer dereferences: this DLL is injected INTO the game, so
// ReadProcessMemory would be a kernel transition for memory we already own — it was
// costing two syscalls per object, per lookup.
//
// POD-only and SEH-guarded on purpose: every game-memory dereference lives in here,
// and __try cannot be used in a function that requires C++ object unwinding.
static uint32_t snapshotAnimatedRigids(AnimObjEntry* out, uint32_t maxOut)
{
	uint32_t n = 0;
	__try
	{
		uint32_t listAddr = *reinterpret_cast<uint32_t*>(OBJ_LIST_PTR_ADDR);
		uint32_t count    = *reinterpret_cast<uint32_t*>(OBJ_LIST_COUNT_ADDR);
		if (listAddr == 0 || count == 0 || count > MAX_SCAN_OBJECTS)
			return 0;

		for (uint32_t i = 0; i < count && n < maxOut; ++i)
		{
			uint32_t objAddr = *reinterpret_cast<uint32_t*>(listAddr + i * OBJ_RECORD_SIZE);
			if (objAddr < 0x400000)                                    // unset / bogus slot
				continue;
			if (*reinterpret_cast<uint8_t*>(objAddr + 0x7C) != RIGID_INSTANCE_CLASS_TAG)
				continue;                                              // not a rigid_instance
			uint32_t player = *reinterpret_cast<uint32_t*>(objAddr + RI_ANIMPLAYER_OFF);
			if (player == 0)
				continue;                                              // no anim player
			if (*reinterpret_cast<uint32_t*>(player + AP_VALID_OFF) == 0)
				continue;                                              // AnimDataAvailable == false

			out[n].addr  = objAddr;
			out[n].guid  = *reinterpret_cast<uint64_t*>(objAddr + 0x8);
			out[n].frame = *reinterpret_cast<float*>(player + AP_FRAME_OFF);
			++n;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		dprintf("anim scan raised an exception - suppressed (%u collected)\n", n);
	}
	return n;
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

	static std::vector<AnimObjEntry> scan(MAX_ANIM_ENTRIES);
	uint32_t found = snapshotAnimatedRigids(scan.data(), MAX_ANIM_ENTRIES);

	std::unordered_map<uint64_t, float> snapshot;
	for (uint32_t i = 0; i < found; ++i)
	{
		if (snapshot.size() >= MaxAnimSyncPerMessage)
			break;
		if (scan[i].guid == 0)
			continue;
		snapshot[scan[i].guid] = scan[i].frame;
	}
	if (found > MaxAnimSyncPerMessage)
	{
		dprintf("anim sync: %u animated objects found, capped at %zu\n",
			found, MaxAnimSyncPerMessage);
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
	dprintf("Broadcast anim sync for %zu rigid_instances\n", sent);
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
//
// Walks the object list ONCE and looks each object's GUID up in the received map
// (O(1) per object). The previous version searched the entire object list for every
// synced object — O(M*N) with two syscalls per step, which stalled the frame for
// ~half a second on a busy level. This is O(N) with no syscalls at all.
static void applyQueuedAnimSync()
{
	std::unordered_map<uint64_t, float> pending;
	EnterCriticalSection(&animSyncCriticalSection);
	pending.swap(g_incomingAnimFrames);
	LeaveCriticalSection(&animSyncCriticalSection);

	if (pending.empty())
		return;

	static std::vector<AnimObjEntry> scan(MAX_ANIM_ENTRIES);
	uint32_t found = snapshotAnimatedRigids(scan.data(), MAX_ANIM_ENTRIES);

	uint32_t applied = 0;
	for (uint32_t i = 0; i < found; ++i)
	{
		auto it = pending.find(scan[i].guid);
		if (it == pending.end())
			continue;   // not part of this snapshot
		game_SetAnimFrame(reinterpret_cast<void*>(scan[i].addr), it->second);
		++applied;
	}

	dprintf("anim sync applied to %u/%zu object(s)\n", applied, pending.size());
}

// ===========================================================================
//  Ring (One Ring) Stealth Synchronization
// ===========================================================================
//
// When a player equips the One Ring, the base game turns their own bilbo mesh
// half-transparent. We broadcast that equipped state so every peer applies the
// same stealth look to that player's fake-bilbo NPC, tiered by the NPC's team:
//   any team -> character mesh is UNRENDERED
//   team 2   -> also loses its floating nickname + status labels
//
// Field offsets (from the Reverse SDK):
//   bilbo::SetRingEquipped @0x00423C90 writes bilbo+0x420 (1 = ring equipped)
//   NPC::setTeam writes the team byte at NPCObject+0x1a4
//   NPCObject::MakeTransparent @0x004A99A0 toggles bit 0x40 of the render field at
//     (NPCObject+0x310)+0xe0. That component is a CharacterObject, and the bit is a
//     BINARY visible/invisible switch — confirmed in-game, it is not an alpha level.
static constexpr uint32_t BILBO_RING_EQUIPPED_OFF = 0x420; // bilbo+0x420: 1 = ring on
static constexpr uint32_t NPC_TEAM_OFF = 0x1A4;            // NPCObject+0x1a4: team id (byte)
static constexpr uint32_t NPC_MAKETRANSPARENT_ADDR = 0x004A99A0;
static constexpr uint8_t  RING_TEAM_HIDE_LABELS = 2;       // this team also loses its labels

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

	dprintf("Broadcast ring state: %s\n", msg->ringEquipped ? "ON" : "OFF");
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

// Ring stealth look, per team:
//   any team -> character is UNRENDERED (NPCObject::MakeTransparent bit 0x40 is a
//               binary visible/invisible switch, verified in-game — not an alpha)
//   team 2   -> additionally hide the floating nickname + status labels, so the
//               player leaves no trace at all
//
// Applied every frame and fully idempotent, so it self-corrects after respawns,
// late joins, or a nickname/status update writing the labels back.
static void applyRingVisual(Player& p, bool ringOn)
{
	if (!processAnalyzer || !p.npc)
		return;
	uint32_t npcAddr = p.npc->getObjectPtr();
	if (npcAddr == 0)
		return;

	// Character mesh: hidden whenever the ring is on, for every team.
	game_NPCMakeTransparent(reinterpret_cast<void*>(npcAddr), ringOn ? 1 : 0);

	// Labels: only team 2 loses them, and only while the ring is on.
	bool hideLabels = false;
	if (ringOn)
	{
		uint8_t team = processAnalyzer->readData<uint8_t>(npcAddr + NPC_TEAM_OFF);
		hideLabels = (team == RING_TEAM_HIDE_LABELS);
	}

	if (p.nickname_marker)
		p.nickname_marker->setText(hideLabels ? "" : p.nickname.c_str());
	if (p.status_marker)
		p.status_marker->setText(hideLabels ? "" : p.status.c_str());
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
		auto it = ringSnapshot.find(p.npcGuid);
		bool ringOn = (it != ringSnapshot.end()) && it->second != 0;
		applyRingVisual(p, ringOn);
	}
	LeaveCriticalSection(&playersCriticalSection);
}

// ===========================================================================
//  NPC Sight — honest detection of remote players
// ===========================================================================
//
// Remote players are NPC objects, and the engine's sight code takes two shortcuts
// that only ever made sense for NPC-vs-NPC awareness. Both make enemies spot a
// fake bilbo through walls, floors and from behind:
//
//   1. SenseController::CanSee @0x005AB800 loads the target's class signature from
//      object+0x10 and, right after the range test, does
//          005AB9BF  test bl, 0x10      ; signature bit 0x10 == "is an NPC"
//          005AB9C2  jne  0x005ABC90    ; -> return 1 (SEEN)
//      which jumps over the FOV cone, the LOS raycast AND the stealth gate. Bit
//      0x10 is set on exactly one of the 48 registered classes, NPC, so any NPC
//      target degrades to a plain "is it inside SightRange" sphere test.
//
//   2. SenseController::UpdateSensedArray @0x005AAAD0 is what actually acquires
//      targets. Its Bilbo half calls CanSee (full FOV + LOS + stealth), but when
//      Bilbo is further than 500 units it falls through to a scan over every NPC
//      object that tests only range/enemy/alive - it never calls CanSee at all,
//      so there is no raycast in it to switch on.
//
// On top of that, stealth is not a character property: the grant at the end of
// CanSee is guarded by `GetBilboGuid() == targetGuid`, so a remote player can
// never receive it no matter what state we put the object in.
//
// The fix for (1) is to stop lying about what the target is for the duration of a
// single call: swap signature bit 0x10 (NPC) for bit 0x40 (Bilbo). CanSee reads
// the signature once and uses it for the fast path plus two `& 0x50` "is this a
// character" gates, so the swap skips the shortcut while keeping the IsEnemy and
// IsAlive checks intact - and the engine then runs its own FOV cone and its own
// raycast, with the correct bbox-centre endpoints and self-ignore GUIDs. The
// value is restored immediately, so nothing else (projectile hits, hearing,
// targeting) ever observes it. The fix for (2) is to let the scan run and then
// re-validate whatever it latched through the same path.
static constexpr uint32_t SENSE_CANSEE_ADDR = 0x005AB800;
static constexpr uint32_t SENSE_UPDATESENSEDARRAY_ADDR = 0x005AAAD0;
static constexpr uint32_t SENSE_GETOWNER_ADDR = 0x005AA750;
static constexpr uint32_t SENSE_CANSMELL_ADDR = 0x005AB3F0;

static constexpr uint32_t OBJ_SIGNATURE_OFF = 0x10;   // object+0x10: class signature
static constexpr uint32_t OBJ_SIG_NPC_BIT = 0x10;     //   bit 0x10: "is an NPC"
static constexpr uint32_t OBJ_SIG_BILBO_BIT = 0x40;   //   bit 0x40: "is bilbo"

static constexpr uint32_t SENSE_FLAGS_OFF = 0x18;     // SenseController+0x18: flags
static constexpr uint32_t SENSE_FLAG_SENSING_OTHER = 0x02; //   bit 2: sensing a non-Bilbo
static constexpr uint32_t SENSE_FLAG_DOLOS = 0x04;    //   bit 4: run the LOS raycast
static constexpr uint32_t SENSE_TARGET_GUID_OFF = 0x38; // SenseController+0x38: sensed GUID
static constexpr uint32_t NPC_IGNORES_STEALTH_OFF = 0x23C; // NPCObject+0x23c: sees through hiding

// public: int  __thiscall SenseController::CanSee(unsigned __int64, int, int)   [ret 0x10]
// public: void __thiscall SenseController::UpdateSensedArray(int, float)        [ret 8]
// public: int  __thiscall SenseController::CanSmell(unsigned __int64, int)      [ret 0xC]
// The SDK labels 0x005AA750 SetBlind; it is really the owner accessor - it resolves
// the owning NPCObject from the GUID at +0x40 and caches it at +0x48.
typedef int(__fastcall* SenseCanSee_t)(void* self, void* edx, uint64_t targetGuid, int checkEnemy, int doLOS);
typedef void(__fastcall* SenseUpdateSensedArray_t)(void* self, void* edx, int arg, float scanOthers);
typedef void* (__fastcall* SenseGetOwner_t)(void* self, void* edx);
typedef int(__fastcall* SenseCanSmell_t)(void* self, void* edx, uint64_t targetGuid, int checkEnemy);

static SenseCanSee_t            oSenseCanSee = nullptr;
static SenseUpdateSensedArray_t oSenseUpdateSensedArray = nullptr;
static const SenseGetOwner_t    game_SenseGetOwner =
	reinterpret_cast<SenseGetOwner_t>(SENSE_GETOWNER_ADDR);
// Not hooked - CanSmell has no NPC fast path and no stealth gate, so it already
// treats a remote player exactly like it treats Bilbo. We only need to call it.
static const SenseCanSmell_t    game_SenseCanSmell =
	reinterpret_cast<SenseCanSmell_t>(SENSE_CANSMELL_ADDR);

// Per-frame snapshot of the remote players an NPC could sense. Built on the game
// thread from activePlayers + the ring state, and read on the game thread by the
// hooks below, so it needs no locking of its own. FAKE_BILBO_GUID.txt caps the
// session at 8 players; the headroom is free.
struct RemotePlayerSense
{
	uint64_t guid = 0;
	uint32_t npcAddr = 0;
	bool     hidden = false;
};

static constexpr int MAX_SENSE_PLAYERS = 16;
static RemotePlayerSense g_senseSnapshot[MAX_SENSE_PLAYERS];
static int               g_senseSnapshotCount = 0;

// Rebuild the snapshot (game thread, once per frame alongside the ring visuals).
static void refreshRemoteSenseSnapshot()
{
	int count = 0;

	if (processAnalyzer)
	{
		std::unordered_map<uint64_t, uint8_t> ringSnapshot;
		EnterCriticalSection(&ringCriticalSection);
		ringSnapshot = g_playerRingState;
		LeaveCriticalSection(&ringCriticalSection);

		EnterCriticalSection(&playersCriticalSection);
		for (auto& p : activePlayers)
		{
			if (count >= MAX_SENSE_PLAYERS)
				break;
			if (p.npcGuid == 0 || !p.npc)
				continue;

			const uint32_t npcAddr = p.npc->getObjectPtr();
			if (npcAddr == 0)
				continue;

			auto it = ringSnapshot.find(p.npcGuid);
			g_senseSnapshot[count].guid = p.npcGuid;
			g_senseSnapshot[count].npcAddr = npcAddr;
			g_senseSnapshot[count].hidden = (it != ringSnapshot.end()) && it->second != 0;
			++count;
		}
		LeaveCriticalSection(&playersCriticalSection);
	}

	g_senseSnapshotCount = count;
}

static const RemotePlayerSense* findRemoteSense(uint64_t guid)
{
	if (guid == 0)
		return nullptr;
	for (int i = 0; i < g_senseSnapshotCount; ++i)
	{
		if (g_senseSnapshot[i].guid == guid)
			return &g_senseSnapshot[i];
	}
	return nullptr;
}

// Can this observer legitimately SEE this remote player right now?
// Applies the stealth grant the engine reserves for the local Bilbo, then runs the
// engine's own FOV + LOS check via the signature swap described above.
// Sight only - smell is deliberately not part of this, see remotePlayerSensedBy.
static bool remotePlayerVisibleTo(void* sense, const RemotePlayerSense& player, int checkEnemy)
{
	// Stealth, mirroring what CanSee does for Bilbo: hidden, and this NPC is not
	// one of the ones flagged to see through hiding.
	if (player.hidden)
	{
		uint8_t* observer = static_cast<uint8_t*>(game_SenseGetOwner(sense, nullptr));
		if (observer && *reinterpret_cast<int*>(observer + NPC_IGNORES_STEALTH_OFF) == 0)
			return false;
	}

	const uint32_t npcAddr = player.npcAddr;

	// Objects can be destroyed mid-level and their slot reused, so re-check the type
	// tag before writing to the address. If it no longer looks like an NPC, fall back
	// to asking the engine unmodified rather than corrupting whatever lives there.
	if (npcAddr == 0 ||
		*reinterpret_cast<uint8_t*>(npcAddr + OBJ_TYPE_OFFSET) != OBJ_TYPE_NPC)
	{
		return oSenseCanSee(sense, nullptr, player.guid, checkEnemy, 1) != 0;
	}

	uint32_t* signature = reinterpret_cast<uint32_t*>(npcAddr + OBJ_SIGNATURE_OFF);
	uint32_t* flags = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(sense) + SENSE_FLAGS_OFF);

	const uint32_t savedSignature = *signature;
	const uint32_t savedFlags = *flags;

	// Not an NPC for the length of this call, but still a character.
	*signature = (savedSignature & ~OBJ_SIG_NPC_BIT) | OBJ_SIG_BILBO_BIT;
	// Force the raycast on: whether it runs is gated by each observer's own DoLOS
	// property, and we do not want a guard authored with DoLOS 0 to keep wallhacking.
	*flags = savedFlags | SENSE_FLAG_DOLOS;

	const int seen = oSenseCanSee(sense, nullptr, player.guid, checkEnemy, 1);

	*signature = savedSignature;
	*flags = savedFlags;

	return seen != 0;
}

// Can this observer sense this remote player at all - by sight OR by smell?
//
// Smell is the engine's point-blank catch and it is deliberately NOT stealthed:
// SenseController::CanSmell @0x005AB3F0 tests a vertical band plus a horizontal
// (XZ-only) distance against SmellRange, with no FOV, no LOS, and no hiding check
// anywhere in it. That is why the base game still busts a ring-wearing Bilbo who
// walks into a goblin - UpdateSensedArray's Bilbo half is `CanSee || CanSmell`.
// We mirror that here so a remote player with the ring on is invisible at range
// but still gets caught on contact, instead of being strictly stealthier than the
// real Bilbo.
static bool remotePlayerSensedBy(void* sense, const RemotePlayerSense& player, int checkEnemy)
{
	if (remotePlayerVisibleTo(sense, player, checkEnemy))
		return true;
	return game_SenseCanSmell(sense, nullptr, player.guid, checkEnemy) != 0;
}

static int __fastcall hkSenseCanSee(void* self, void* edx, uint64_t targetGuid, int checkEnemy, int doLOS)
{
	const RemotePlayerSense* player = findRemoteSense(targetGuid);
	if (!player)
		return oSenseCanSee(self, edx, targetGuid, checkEnemy, doLOS);

	return remotePlayerVisibleTo(self, *player, checkEnemy) ? 1 : 0;
}

static void __fastcall hkSenseUpdateSensedArray(void* self, void* edx, int arg, float scanOthers)
{
	oSenseUpdateSensedArray(self, edx, arg, scanOthers);

	// The scan over other NPCs has no FOV and no raycast, so audit what it picked.
	// The sensed GUID is written at the very end of the function and is not read
	// again until the StateController updates, which happens after the whole sense
	// pass, so clearing it here is in time.
	//
	// Audit with sight OR smell: the scan has a smell branch of its own alongside
	// the sight one, so validating with sight alone would throw away legitimate
	// contact-range detections and make the ring far stronger than it is for Bilbo.
	uint8_t* sense = static_cast<uint8_t*>(self);
	const uint64_t target = *reinterpret_cast<uint64_t*>(sense + SENSE_TARGET_GUID_OFF);

	const RemotePlayerSense* player = findRemoteSense(target);
	if (!player)
		return;
	if (remotePlayerSensedBy(self, *player, 1))
		return;

	*reinterpret_cast<uint64_t*>(sense + SENSE_TARGET_GUID_OFF) = 0;
	*reinterpret_cast<uint32_t*>(sense + SENSE_FLAGS_OFF) &= ~SENSE_FLAG_SENSING_OTHER;
}

void InstallSenseHooks()
{
	MH_Initialize();                  // harmless if MinHook is already initialized

	void* canSee = reinterpret_cast<void*>(SENSE_CANSEE_ADDR);
	MH_CreateHook(canSee, &hkSenseCanSee,
		reinterpret_cast<void**>(&oSenseCanSee));
	MH_EnableHook(canSee);

	void* updateSensedArray = reinterpret_cast<void*>(SENSE_UPDATESENSEDARRAY_ADDR);
	MH_CreateHook(updateSensedArray, &hkSenseUpdateSensedArray,
		reinterpret_cast<void**>(&oSenseUpdateSensedArray));
	MH_EnableHook(updateSensedArray);
}

// ===========================================================================
//  Remote-Player Damage Types (pain table)
// ===========================================================================
//
// Every attack gets both its damage AND its damage type from one text table,
// AI\PAINTABLE.INFO, looked up by a string key:
//
//   event_mgr::HandlePainEvent @0x00574C00 composes  "<who>_<AnimEvent>[_<level>]"
//   event_mgr::ApplyPainFromTable @0x00572CE0 looks that key up and turns the
//   matched row's flag columns into the PainData::ePainType it hands to
//   obj_mgr::ApplyPain:
//       Sting 1  -> type 1          Freeze 1 -> type 18 (frost effect + freeze timer)
//       Stick 1  -> type 2          Fire   1 -> type 19 ("Burn" effect on the bones)
//       Webbed 1 -> type 10         BreakShields 1 -> the "charged" variant (4 / 5)
//
// "<who>" is the literal "BILBO" only when the attacker's object type tag
// (object+0x7C) is 0x12, i.e. the real bilbo object. Our fake bilbos are NPC
// objects, so they take the other branch and the key comes out as
// "<CharacterTypeName>_Sting_Swing" - and without the "_<level>" suffix, because
// the swing-level getters only run in the 0x12 branch. Nothing in the table
// matches that, so ApplyPainFromTable takes its "no row" path: pain type 0
// (Unknown) plus the hard-coded 5000 damage that applyFakeBilboDamage() patches
// down to myFakeBilboDamage. That fallback is exactly why remote players' hits do
// one flat amount and never freeze, burn, web or break shields.
//
// Thrown stones have the same problem one level down: FUN_005750E0
// (event_mgr::GeneratePain) gates on the same 0x12 tag, so a remote fire / freeze
// / explode stone degrades to "<CharacterTypeName>_Rocks" - one key shared by all
// four stone types, which is why every remote stone hits identically.
//
// The fix here is to rewrite the key so it matches the rows the engine would have
// used for a real Bilbo. We deliberately do NOT fake the 0x12 type tag instead:
// that branch reads the LOCAL player's equipped weapon (bilbo+0x3C0 sting /
// +0x3D0 staff) and will rewrite Sting_Swing into Staff_Swing - or drop the pain
// event entirely - based on what WE are holding, which is the wrong answer for
// somebody else's swing. So we compose the key ourselves from the weapon that was
// networked to us along with the rest of that player's state.
//
// Key lookups are case-insensitive: both the table keys (at load) and the query
// key (in ApplyPainFromTable) are folded through x_strlwr @0x00648080, so the
// casing used below does not matter.
//
// This runs on EVERY peer, not only the host. Enemy health is host-authoritative
// (EnemiesStateMessage overwrites it ~15x a second), so the host is what decides
// how much damage ultimately sticks on an enemy - but the freeze / burn / stun /
// knockback reactions, the impact FX, and any pain dealt to the LOCAL bilbo are
// produced by each machine's own engine from its own pain type. Each machine
// therefore needs the correct type, or remote fire stones look and feel inert on
// everyone else's screen.

static constexpr uint32_t PAIN_APPLYFROMTABLE_ADDR = 0x00572CE0; // event_mgr::ApplyPainFromTable
static constexpr uint32_t PAIN_GENERATEPAIN_ADDR = 0x005750E0;   // event_mgr::GeneratePain (projectiles)
static constexpr uint32_t BILBO_INVENTORY_ADDR = 0x0075BBE0;     // bilboInventory singleton

// Weapon ids as they travel in PlayerStateMessage (see NPC::setWeapon).
static constexpr uint32_t PAIN_WEAPON_STING = 0;
static constexpr uint32_t PAIN_WEAPON_STAFF = 1;

// Projectile types passed to Projectile::CreateProjectile (see hkCreateStoneProjectile).
static constexpr int PAIN_PROJ_STONE_NORMAL = 0x19;
static constexpr int PAIN_PROJ_STONE_FIRE = 0x1A;
static constexpr int PAIN_PROJ_STONE_EXPLODE = 0x1B;
static constexpr int PAIN_PROJ_STONE_FREEZE = 0x1C;

// bilboInventory upgrade-tier getters. __thiscall with no arguments (ECX = the
// inventory singleton, result in ST0), so they are declared __fastcall with a
// filler EDX exactly like the other detoured __thiscall members in this file.
// These are the same getters bilbo::GetSwingLevel/GetJumpLevel call, so the tier
// we append is the one the engine itself would have produced.
typedef float(__fastcall* InvLevel_t)(void* inv, void* edx);
static const InvLevel_t game_GetStingSwingLevel = reinterpret_cast<InvLevel_t>(0x004373B0);
static const InvLevel_t game_GetStingJumpLevel = reinterpret_cast<InvLevel_t>(0x00437420);
static const InvLevel_t game_GetStaffSwingLevel = reinterpret_cast<InvLevel_t>(0x00437490);
static const InvLevel_t game_GetStaffJumpLevel = reinterpret_cast<InvLevel_t>(0x00437500);
static const InvLevel_t game_GetStaffAEJumpLevel = reinterpret_cast<InvLevel_t>(0x00437570);
static const InvLevel_t game_GetStoneThrowLevel = reinterpret_cast<InvLevel_t>(0x004375E0);

// Upgrade tiers are the one part of a remote swing we cannot know: they live in
// that player's own inventory, which is not networked. 0 mirrors our own tier for
// the same move (right in co-op, where progression is shared); 1..3 forces one.
static int g_remotePainLevelOverride = 0;

// Per-frame snapshot of who the fake bilbos are and what they are holding. Built
// on the game thread from activePlayers, read on the game thread by the two hooks
// below, so it needs no locking of its own - same arrangement as g_senseSnapshot.
struct RemotePlayerPain
{
	uint64_t guid = 0;
	uint32_t meleeWeapon = PAIN_WEAPON_STAFF;
};

static constexpr int MAX_PAIN_PLAYERS = 16;
static RemotePlayerPain g_painSnapshot[MAX_PAIN_PLAYERS];
static int              g_painSnapshotCount = 0;

// The networked weapon is whatever that player currently has selected, which
// includes stones and "none" - neither of which tells us which melee weapon their
// swing belongs to. So we remember the last real melee weapon we saw per player
// and keep using it while they are holding something else. Game thread only.
static std::unordered_map<uint64_t, uint32_t> g_lastMeleeWeapon;

static void refreshRemotePainSnapshot()
{
	int count = 0;

	EnterCriticalSection(&playersCriticalSection);
	for (auto& p : activePlayers)
	{
		if (count >= MAX_PAIN_PLAYERS)
			break;
		if (p.npcGuid == 0 || !p.npc)
			continue;

		if (p.bilboWeapon == PAIN_WEAPON_STING || p.bilboWeapon == PAIN_WEAPON_STAFF)
			g_lastMeleeWeapon[p.npcGuid] = p.bilboWeapon;

		auto remembered = g_lastMeleeWeapon.find(p.npcGuid);

		g_painSnapshot[count].guid = p.npcGuid;
		g_painSnapshot[count].meleeWeapon = (remembered != g_lastMeleeWeapon.end())
			? remembered->second : PAIN_WEAPON_STAFF;   // staff is what Bilbo starts with
		++count;
	}
	LeaveCriticalSection(&playersCriticalSection);

	g_painSnapshotCount = count;
}

static const RemotePlayerPain* findRemotePain(uint64_t guid)
{
	if (guid == 0)
		return nullptr;
	for (int i = 0; i < g_painSnapshotCount; ++i)
	{
		if (g_painSnapshot[i].guid == guid)
			return &g_painSnapshot[i];
	}
	return nullptr;
}

// Rows only exist for tiers 1..3.
static int clampPainLevel(float raw)
{
	int level = static_cast<int>(raw);
	if (level < 1) level = 1;
	if (level > 3) level = 3;
	return level;
}

static int remotePainLevel(InvLevel_t getter)
{
	if (g_remotePainLevelOverride >= 1 && g_remotePainLevelOverride <= 3)
		return g_remotePainLevelOverride;
	return clampPainLevel(getter(reinterpret_cast<void*>(BILBO_INVENTORY_ADDR), nullptr));
}

// Does this key end with "_<suffix>"? The key at this point is
// "<CharacterTypeName>_<AnimEvent>" with every space already turned into an
// underscore, so anchoring on the trailing event name is safer than splitting on
// the first underscore (character type names can contain spaces).
static bool painKeyEndsWith(const char* key, const char* suffix)
{
	const size_t keyLen = strlen(key);
	const size_t sufLen = strlen(suffix);
	return keyLen > sufLen + 1
		&& key[keyLen - sufLen - 1] == '_'
		&& _stricmp(key + keyLen - sufLen, suffix) == 0;
}

// Map a fake bilbo's pain key onto the move the Bilbo_* rows are written for.
// Returns the event name to use, or nullptr when this key is not one of the melee
// events the table covers (Throw, enemy events, anything already rewritten).
//
// An explicit "Staff_*" event is trusted as-is; "Sting_*" is the shared/default
// spelling the engine itself remaps by equipped weapon, so that is where the
// networked weapon decides.
static const char* remoteBilboPainEvent(const char* key, uint32_t meleeWeapon, int* levelOut)
{
	const bool staff = (meleeWeapon == PAIN_WEAPON_STAFF);

	if (painKeyEndsWith(key, "Staff_AE_Jump"))
	{
		*levelOut = remotePainLevel(game_GetStaffAEJumpLevel);
		return "Staff_AE_Jump";
	}
	if (painKeyEndsWith(key, "Staff_Swing"))
	{
		*levelOut = remotePainLevel(game_GetStaffSwingLevel);
		return "Staff_Swing";
	}
	if (painKeyEndsWith(key, "Staff_Jump"))
	{
		*levelOut = remotePainLevel(game_GetStaffJumpLevel);
		return "Staff_Jump";
	}
	if (painKeyEndsWith(key, "Sting_Swing"))
	{
		*levelOut = remotePainLevel(staff ? game_GetStaffSwingLevel : game_GetStingSwingLevel);
		return staff ? "Staff_Swing" : "Sting_Swing";
	}
	if (painKeyEndsWith(key, "Sting_Jump"))
	{
		*levelOut = remotePainLevel(staff ? game_GetStaffJumpLevel : game_GetStingJumpLevel);
		return staff ? "Staff_Jump" : "Sting_Jump";
	}

	return nullptr;
}

// public: void __thiscall event_mgr::ApplyPainFromTable(char* key, unsigned lo,
//         unsigned hi, vector3 pos /*by value*/, float radius, unsigned classMask,
//         int excludeTypeTag)
typedef void(__fastcall* ApplyPainFromTable_t)(
	void* mgr, void* edx, const char* key,
	uint32_t guidLo, uint32_t guidHi,
	float x, float y, float z,
	float radius, uint32_t classMask, int excludeTypeTag);
static ApplyPainFromTable_t oApplyPainFromTable = nullptr;

// private: void __thiscall event_mgr::GeneratePain(unsigned lo, unsigned hi,
//          int projectileType, const vector3* pos, float radius,
//          unsigned classMask, int excludeTypeTag)
typedef void(__fastcall* GeneratePain_t)(
	void* mgr, void* edx,
	uint32_t guidLo, uint32_t guidHi, int projType,
	const Vector3* pos, float radius, uint32_t classMask, int excludeTypeTag);
static GeneratePain_t oGeneratePain = nullptr;

static void __fastcall hkApplyPainFromTable(
	void* mgr, void* edx, const char* key,
	uint32_t guidLo, uint32_t guidHi,
	float x, float y, float z,
	float radius, uint32_t classMask, int excludeTypeTag)
{
	char rewritten[64];

	if (key)
	{
		const uint64_t owner = (static_cast<uint64_t>(guidHi) << 32) | guidLo;
		if (const RemotePlayerPain* p = findRemotePain(owner))
		{
			int level = 1;
			if (const char* event = remoteBilboPainEvent(key, p->meleeWeapon, &level))
			{
				sprintf_s(rewritten, sizeof(rewritten), "BILBO_%s_%d", event, level);
				key = rewritten;
			}
		}
	}

	oApplyPainFromTable(mgr, edx, key, guidLo, guidHi, x, y, z,
		radius, classMask, excludeTypeTag);
}

static void __fastcall hkGeneratePain(
	void* mgr, void* edx,
	uint32_t guidLo, uint32_t guidHi, int projType,
	const Vector3* pos, float radius, uint32_t classMask, int excludeTypeTag)
{
	const uint64_t owner = (static_cast<uint64_t>(guidHi) << 32) | guidLo;

	if (pos && findRemotePain(owner))
	{
		char key[64];
		bool haveKey = true;

		switch (projType)
		{
		case PAIN_PROJ_STONE_FIRE:
			strcpy_s(key, sizeof(key), "BILBO_STONE_THROW_FIRE");
			break;
		case PAIN_PROJ_STONE_EXPLODE:
			strcpy_s(key, sizeof(key), "BILBO_STONE_THROW_EXPLODE");
			break;
		case PAIN_PROJ_STONE_FREEZE:
			strcpy_s(key, sizeof(key), "BILBO_STONE_THROW_FREEZE");
			break;
		case PAIN_PROJ_STONE_NORMAL:
			// Plain stones are tiered like the melee moves. The engine reads this
			// tier off the LOCAL bilbo even for a projectile it did not throw, so
			// mirroring our own inventory here is not an approximation - it is
			// what the original code does.
			sprintf_s(key, sizeof(key), "BILBO_STONE_THROW_%d",
				remotePainLevel(game_GetStoneThrowLevel));
			break;
		default:
			haveKey = false;   // staff/sting jump projectiles: no Bilbo_* rows exist
			break;
		}

		if (haveKey && oApplyPainFromTable)
		{
			// Straight to the table with our key. The trampoline is used so this
			// does not re-enter hkApplyPainFromTable.
			oApplyPainFromTable(mgr, edx, key, guidLo, guidHi,
				pos->x, pos->y, pos->z, radius, classMask, excludeTypeTag);
			return;
		}
	}

	oGeneratePain(mgr, edx, guidLo, guidHi, projType, pos, radius, classMask, excludeTypeTag);
}

void InstallPainHooks()
{
	MH_Initialize();                  // harmless if MinHook is already initialized

	void* applyFromTable = reinterpret_cast<void*>(PAIN_APPLYFROMTABLE_ADDR);
	MH_CreateHook(applyFromTable, &hkApplyPainFromTable,
		reinterpret_cast<void**>(&oApplyPainFromTable));
	MH_EnableHook(applyFromTable);

	void* generatePain = reinterpret_cast<void*>(PAIN_GENERATEPAIN_ADDR);
	MH_CreateHook(generatePain, &hkGeneratePain,
		reinterpret_cast<void**>(&oGeneratePain));
	MH_EnableHook(generatePain);
}

// ===========================================================================
//  Cinema (cutscene) Synchronization
// ===========================================================================
//
// Stealth levels punish a blown sneak with a cutscene. In multiplayer only the
// player who tripped it sees it, so the group falls out of step. This shares a
// whitelist of cinemas: when one fires on any client, every client plays it.
//
// Detection is a detour on cinema::Start @0x0046A550 (void __thiscall, so the
// only argument is the cinema object itself). Cinema objects are authored into
// the level, so the object GUID at +0x8 is the same on every machine and is all
// that needs to travel.
//
// Replaying is safe by construction - cinema::Start refuses up front when the
// cinema is already running or already finished and non-repeatable:
//   cinema+0x194 flags: bit 0x1 = repeatable, bit 0x2 = finished (cinema::IsFinished
//   returns (flags >> 1) & 1), bit 0x4 = currently active
// We still check those before calling so we do not spam its warning log.
//
// Echo guard: applying a remote cinema calls the MinHook TRAMPOLINE, not the raw
// address, so a replayed cinema never re-enters the hook and re-broadcasts. Same
// trick as playFxLocal.
static constexpr uint32_t CINEMA_START_ADDR = 0x0046A550;
static constexpr uint32_t CINEMA_GUID_OFF = 0x08;    // object+0x8: GUID
static constexpr uint32_t CINEMA_FLAGS_OFF = 0x194;  // cinema+0x194: state flags
static constexpr uint32_t CINEMA_FLAG_REPEATABLE = 0x1;
static constexpr uint32_t CINEMA_FLAG_FINISHED = 0x2;
static constexpr uint32_t CINEMA_FLAG_ACTIVE = 0x4;

// The GUIDs to share, one per line in "XXXXXXXX_XXXXXXXX" form - same format as
// FAKE_BILBO_GUID.txt. Missing file simply means the feature is off.
static constexpr const char* SYNCED_CINEMA_FILE = "SYNCED_CINEMAS.txt";

// public: void __thiscall cinema::Start(void)
typedef void(__fastcall* CinemaStart_t)(void* self, void* edx);
static CinemaStart_t oCinemaStart = nullptr;

CRITICAL_SECTION cinemaCriticalSection;
// Guarded by cinemaCriticalSection: /reloadcinemas can replace it at any time while
// the game thread is reading it from the cinema::Start hook.
static std::unordered_set<uint64_t> g_syncedCinemaGuids;
static std::vector<uint64_t> g_outgoingCinemaStarts;
static std::vector<uint64_t> g_incomingCinemaStarts;

// Load (or re-load) the whitelist, returning how many GUIDs are now active and
// writing the file it came from to loadedPath. Deliberately does NOT use
// loadGuidsFromFile() from shared.h: that one prompts on stdin when the file is
// missing, which would hang the game.
//
// Parses into a local set and swaps it in at the end, so the file I/O happens
// outside the lock and a failed/missing read leaves the previous list untouched.
static size_t loadSyncedCinemaGuids(std::string* loadedPath = nullptr)
{
	std::vector<std::string> candidates;
	appendUniqueConfigCandidate(candidates, SkinSync::fs::path(SYNCED_CINEMA_FILE));

	const std::string exeDirectory = getModuleDirectory(nullptr);
	if (!exeDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(exeDirectory) / SYNCED_CINEMA_FILE);

	const std::string dllDirectory = getModuleDirectory(moduleInstance);
	if (!dllDirectory.empty())
		appendUniqueConfigCandidate(candidates, SkinSync::fs::path(dllDirectory) / SYNCED_CINEMA_FILE);

	for (const std::string& candidate : candidates)
	{
		std::ifstream file(candidate);
		if (!file.is_open())
			continue;

		std::unordered_set<uint64_t> parsed;
		std::string line;
		while (std::getline(file, line))
		{
			if (line.find('_') == std::string::npos)
				continue;   // blank line or a comment - the format always has one

			uint64_t guid = guidFromString(line);
			if (guid != 0)
				parsed.insert(guid);
		}

		const size_t count = parsed.size();

		EnterCriticalSection(&cinemaCriticalSection);
		g_syncedCinemaGuids.swap(parsed);
		LeaveCriticalSection(&cinemaCriticalSection);

		if (loadedPath)
			*loadedPath = candidate;

		dprintf("Synced cinemas: %zu GUID(s) loaded from %s\n", count, candidate.c_str());
		return count;
	}

	dprintf("Synced cinemas: no %s found, cutscene sync disabled\n", SYNCED_CINEMA_FILE);
	return 0;
}

static bool isSyncedCinema(uint64_t cinemaGuid)
{
	if (cinemaGuid == 0)
		return false;

	EnterCriticalSection(&cinemaCriticalSection);
	const bool synced = g_syncedCinemaGuids.count(cinemaGuid) != 0;
	LeaveCriticalSection(&cinemaCriticalSection);

	return synced;
}

static void __fastcall hkCinemaStart(void* self, void* edx)
{
	(void)edx;

	uint64_t cinemaGuid = 0;
	if (self && processAnalyzer)
	{
		cinemaGuid = processAnalyzer->readData<uint64_t>(
			reinterpret_cast<uint32_t>(self) + CINEMA_GUID_OFF);
	}

	oCinemaStart(self, nullptr);

	// Log every cinema, whitelisted or not - this is how you harvest the GUIDs to
	// put in SYNCED_CINEMAS.txt: play the level, trip the cutscene, copy the line.
	// Printed high-dword-first so it can be pasted into the file verbatim, which
	// is the form guidFromString() parses.
	const bool synced = isSyncedCinema(cinemaGuid);
	dprintf("[cinema] start %08X_%08X %s\n",
		static_cast<uint32_t>(cinemaGuid >> 32),
		static_cast<uint32_t>(cinemaGuid),
		synced ? "(SYNCED)" : "(local only)");

	// Queue AFTER the original: if the engine rejected the start (already active,
	// already finished, Bilbo dead) we would rather not have told anyone. It sets
	// the active bit itself, so a rejected start leaves the flags untouched.
	if (!synced)
		return;

	EnterCriticalSection(&cinemaCriticalSection);
	g_outgoingCinemaStarts.push_back(cinemaGuid);
	LeaveCriticalSection(&cinemaCriticalSection);
}

void InstallCinemaHook()
{
	MH_Initialize();                  // harmless if MinHook is already initialized
	void* target = reinterpret_cast<void*>(CINEMA_START_ADDR);
	MH_CreateHook(target, &hkCinemaStart,
		reinterpret_cast<void**>(&oCinemaStart));
	MH_EnableHook(target);
}

// Send any locally-triggered synced cinemas (network thread, from the client loop).
static void sendCinemaSync(Client& client)
{
	EnterCriticalSection(&cinemaCriticalSection);
	std::vector<uint64_t> pending;
	pending.swap(g_outgoingCinemaStarts);
	LeaveCriticalSection(&cinemaCriticalSection);

	if (pending.empty())
		return;
	if (!client.IsConnected() || myGuid == 0)
		return;

	for (uint64_t cinemaGuid : pending)
	{
		auto* msg = static_cast<CinemaSyncMessage*>(client.CreateMessage(CINEMA_SYNC));
		if (!msg)
			return;
		msg->cinemaGuid = cinemaGuid;
		msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		client.SendMessage(channels::Gameplay, msg);
	}
}

// Store an incoming cinema start (network thread).
static void processCinemaSync(CinemaSyncMessage* msg)
{
	if (msg->cinemaGuid == 0)
		return;

	EnterCriticalSection(&cinemaCriticalSection);
	g_incomingCinemaStarts.push_back(msg->cinemaGuid);
	LeaveCriticalSection(&cinemaCriticalSection);
}

// Play any remote-triggered cinemas (game thread - cinema::Start drives cameras,
// the state machine and the cinema queue, none of which is thread safe).
static void applyQueuedCinemaStarts()
{
	if (!oCinemaStart || !processAnalyzer)
		return;

	EnterCriticalSection(&cinemaCriticalSection);
	std::vector<uint64_t> pending;
	pending.swap(g_incomingCinemaStarts);
	LeaveCriticalSection(&cinemaCriticalSection);

	if (pending.empty() || !gameManager.isOnLevel())
		return;

	for (uint64_t cinemaGuid : pending)
	{
		uint32_t cinemaAddress = processAnalyzer->findGameObjByGUID(cinemaGuid);
		if (cinemaAddress == 0)
			continue;   // not in this level - nothing to play

		const uint32_t flags = processAnalyzer->readData<uint32_t>(cinemaAddress + CINEMA_FLAGS_OFF);
		if (flags & CINEMA_FLAG_ACTIVE)
			continue;   // already playing here
		if ((flags & CINEMA_FLAG_FINISHED) && !(flags & CINEMA_FLAG_REPEATABLE))
			continue;   // already played here and cannot repeat

		// Trampoline, not the hook - otherwise this would re-broadcast.
		oCinemaStart(reinterpret_cast<void*>(cinemaAddress), nullptr);

		dprintf("[cinema] applied remote %08X_%08X\n",
			static_cast<uint32_t>(cinemaGuid >> 32),
			static_cast<uint32_t>(cinemaGuid));
	}
}

// ===========================================================================
//  Layer load -> automatic animation resync (host only)
// ===========================================================================
//
// Level geometry is streamed in "layers". A LoadTrigger brings a set of them in,
// and the objects in those layers start animating on whichever machine activated
// them, so they drift apart - the same problem /syncanim exists to fix, just
// triggered by streaming instead of by a late join.
//
// So: when the host fires a load trigger, request the same one-shot anim
// broadcast /syncanim does. Host only, because every client runs the same level
// scripts and would otherwise all broadcast the same snapshot at once.
//
// No waiting is needed before snapshotting. Objects are instantiated ONCE per
// level: layer_mgr::LoadObjects @0x0055A400 is called from exactly one place,
// layer_mgr::LoadLevel @0x0055ABC0. Runtime layer traffic only flips objects
// active/inactive - layer_mgr::Flush calls ActivateObjects/DeactivateObjects,
// which set or clear bit 0x01 of the layer record at layerEntry+0x42, and
// object::GetIsLayerActive @0x004AC5D0 just reads that bit back. So every object
// is already in the object list from level load and the scan finds it either way.
static constexpr uint32_t LOAD_TRIGGER_EXECUTE_ADDR = 0x00497D60;

// void __thiscall load_trigger::Execute(void) - the only argument is the trigger.
// Declared __fastcall so ECX/EDX reach the trampoline untouched.
typedef void(__fastcall* LoadTriggerExecute_t)(void* self, void* edx);
static LoadTriggerExecute_t oLoadTriggerExecute = nullptr;

static void __fastcall hkLoadTriggerExecute(void* self, void* edx)
{
	oLoadTriggerExecute(self, edx);

	if (isHost != 1)
		return;

	// Same one-shot request /syncanim raises; the capture + broadcast happens on
	// the network thread in sendAnimSync(), which is also a natural short delay
	// after the layer flush that this trigger queued.
	g_animSyncRequested.store(true);

	uint64_t triggerGuid = 0;
	if (self && processAnalyzer)
	{
		triggerGuid = processAnalyzer->readData<uint64_t>(
			reinterpret_cast<uint32_t>(self) + CINEMA_GUID_OFF);   // object+0x8, same for every object
	}

	dprintf("[layer] load trigger %08X_%08X fired - broadcasting anim frames\n",
		static_cast<uint32_t>(triggerGuid >> 32),
		static_cast<uint32_t>(triggerGuid));
}

void InstallLayerHooks()
{
	MH_Initialize();                  // harmless if MinHook is already initialized

	void* execute = reinterpret_cast<void*>(LOAD_TRIGGER_EXECUTE_ADDR);
	MH_CreateHook(execute, &hkLoadTriggerExecute,
		reinterpret_cast<void**>(&oLoadTriggerExecute));
	MH_EnableHook(execute);
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
	dprintf("Spawned RigidInstance GUID %llu, broadcasting\n", created.Guid);
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

static FxFireAndForget_t oFxFireAndForget = nullptr;   // set by InstallFxHook (trampoline)

// Play one FX now (game thread only — calls the engine directly).
// NOTE: goes through the hook TRAMPOLINE when the FX hook is installed, so replaying
// a remote effect never re-enters our hook and gets re-broadcast (no echo storm).
static void playFxLocal(const PendingFx& fx)
{
	if (fx.name[0] == 0)
		return;
	vector3 pos{ fx.x, fx.y, fx.z };
	radian3 rot{ fx.pitch, fx.yaw, fx.roll };
	vector3 scale{ fx.scaleX, fx.scaleY, fx.scaleZ };
	FxFireAndForget_t fn = oFxFireAndForget ? oFxFireAndForget : game_fxFireAndForget;
	fn(fx.name, &pos, &rot, &scale);
}

// Queue an FX for the network only (our own screen already shows it).
static void BroadcastFxOnly(const char* name, float x, float y, float z,
	float pitch, float yaw, float roll,
	float scaleX, float scaleY, float scaleZ)
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
	g_outgoingFx.push_back(fx);
	LeaveCriticalSection(&fxCriticalSection);
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

// ---------------------------------------------------------------------------
// Synced one-shot FX
// ---------------------------------------------------------------------------
// Some effects are spawned only on the screen of the player who caused them. We
// hook the engine's FX spawner and rebroadcast a whitelist of them, so every peer
// sees the same effect at the same spot with the exact position/rotation/scale the
// game itself computed.
//
// Each name below was verified (across all 14,860 recovered functions) to be
// spawned by exactly ONE function, so the name alone is a false-positive-free
// trigger for that event:
//   ExplosionSmall / PoisonSpitImpact -> dlg_pickLock::OnUpdate  (lockpick FAILED)
//   Bilbo_LevelUp                     -> bilboInventory::Update  (player LEVELED UP)
//
// To sync another effect later, just add its name here.
static const char* const kSyncedFxNames[] = {
	"ExplosionSmall",
	"PoisonSpitImpact",
	"Bilbo_LevelUp",
};

static bool isSyncedFxName(const char* n)
{
	if (!n)
		return false;
	for (const char* s : kSyncedFxNames)
		if (strcmp(n, s) == 0)
			return true;
	return false;
}

// Height lift applied to the BROADCAST copy of an effect, per effect name.
//
// The level-up effect spawns at Bilbo's object origin (ground level) and the game
// then re-anchors ITS OWN copy to the "Bilbo_Dummy_Root" bone — peers only get the
// raw spawn position, so their copy renders low. Lift it to roughly body height.
//
// The lockpick explosion needs no lift: dlg_pickLock::OnUpdate already builds a
// +85 Y offset into the position it passes in.
static constexpr float FX_LEVELUP_Y_OFFSET = 50.0f;

static float fxBroadcastYOffset(const char* name)
{
	if (strcmp(name, "Bilbo_LevelUp") == 0)
		return FX_LEVELUP_Y_OFFSET;
	return 0.0f;
}

static uint64_t __cdecl hkFxFireAndForget(const char* name, const vector3* pos,
	const radian3* rot, const vector3* scale)
{
	uint64_t result = oFxFireAndForget(name, pos, rot, scale);

	// Only broadcast whitelisted effects, and only ones WE triggered. (Remote FX are
	// replayed through the trampoline, so they never re-enter this hook — no echo.)
	if (pos && isSyncedFxName(name))
	{
		BroadcastFxOnly(name, pos->X, pos->Y + fxBroadcastYOffset(name), pos->Z,
			rot ? rot->X : 0.0f, rot ? rot->Y : 0.0f, rot ? rot->Z : 0.0f,
			scale ? scale->X : 1.0f, scale ? scale->Y : 1.0f, scale ? scale->Z : 1.0f);
		dprintf("Broadcasting FX '%s'\n", name);
	}

	return result;
}

void InstallFxHook()
{
	MH_Initialize();                  // harmless if MinHook is already initialized
	void* target = reinterpret_cast<void*>(FX_FIREANDFORGET_ADDR); // fx_object::FireAndForget
	MH_CreateHook(target, &hkFxFireAndForget,
		reinterpret_cast<void**>(&oFxFireAndForget));
	MH_EnableHook(target);
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

// The client takes its server address from "server_ip=" in the settings file.
// config.txt is still read as a fallback: server.exe uses it for bind_ip /
// public_ip, so a machine that hosts and plays keeps working with one file.
static std::string readSecureServerIP()
{
	// Our own search runs first because it also looks next to the DLL, which can
	// sit somewhere the exe-relative search below would not find.
	std::string ip = readConfiguredServerIp();
	if (!ip.empty())
	{
		printf("Server IP: %s (from %s)\n", ip.c_str(), resolveLocalProfileConfigPath().c_str());
		return ip;
	}

	std::string sourceFile;
	ip = SecureConnect::getClientServerIp(&sourceFile);
	printf("Server IP: %s (from %s)\n", ip.c_str(),
		sourceFile.empty() ? "built-in default" : sourceFile.c_str());
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

	dprintf("Skin mapped: GUID %llu -> texture '%s'\n", msg->playerGuid, textureName.c_str());
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

		dprintf("Saved skin file for GUID %llu to %s\n", msg->playerGuid, savedPath.c_str());
	}
}

// A player left, so their GUID no longer has an announced skin. This only drops
// the mapping - the installed file in common\props is deliberately left alone.
//
// Deleting it used to be the behaviour, and it was the wrong trade: the file name
// is what the level data binds slot N's geometry to, so removing it leaves the
// engine pointing at a resource that no longer exists until someone with a skin
// takes that slot. A stale texture on disk costs nothing (the next occupant with
// a skin overwrites it), and it also means a config that points file_path
// straight at common\props\bilb<N>[d].xbmp can no longer lose its source file.
//
// Consequence to be aware of: a player who takes over a slot without uploading a
// skin of their own inherits whatever the previous occupant left behind.
static void processSkinClear(SkinClearMessage* msg)
{
	playerTextureNames.erase(msg->playerGuid);
	playerSkinFilePaths.erase(msg->playerGuid);

	if (Player* player = findPlayerByGuid(msg->playerGuid))
	{
		player->textureName.clear();
		player->textureFilePath.clear();
	}

	dprintf("Cleared skin mapping for GUID %llu\n", msg->playerGuid);
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

	dprintf("Uploaded local skin '%s' (%zu bytes) for GUID %llu\n",
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
	dcout() << "Player " << std::hex << msg->playerGuid << std::dec
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
	dcout() << "processNicknameUpdate\r\n";
	dcout() << "player GUID = " << msg->player_guid << "\r\n";
	dcout() << "new name = " << newName << "\r\n";

	pendingNicknames[msg->player_guid] = newName;

	Player* pl = findPlayerByGuid(msg->player_guid);
	if (pl) {
		dcout() << "playerFound\r\n";
		pl->nickname = newName;
		if (pl->nickname_marker) {
			dcout() << "markerFound\r\n";
			pl->nickname_marker->setText(newName.c_str());
		}
	}
}

static void processStatusUpdate(StatusUpdateMessage* msg)
{
	std::string newStatus = sanitizeIdentityValue(msg->new_status, sizeof(StatusUpdateMessage::new_status));
	dcout() << "processStatusUpdate\r\n";
	dcout() << "player GUID = " << msg->player_guid << "\r\n";
	dcout() << "new status = " << newStatus << "\r\n";

	pendingStatuses[msg->player_guid] = newStatus;

	Player* pl = findPlayerByGuid(msg->player_guid);
	if (pl) {
		dcout() << "playerFound\r\n";
		pl->status = newStatus;
		if (pl->status_marker) {
			dcout() << "markerFound\r\n";
			pl->status_marker->setText(newStatus.c_str());
		}
	}
}

static void processChatMessage(ChatMsgMessage* msg)
{
	std::string chatText = sanitizeIdentityValue(msg->msg, sizeof(ChatMsgMessage::msg));
	dcout() << "processChatMessage\r\n";
	dcout() << "player GUID = " << msg->player_guid << "\r\n";
	dcout() << "msg = " << chatText << "\r\n";

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
		dprintf("Assigned my GUID: %llu\n", myGuid);

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
	case CINEMA_SYNC:
		if (gameManager.isOnLevel()) processCinemaSync(static_cast<CinemaSyncMessage*>(message));
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

	//	dcout() << "sent chat message\r\n";
	//	dcout() << "player GUID = " << myGuid << "\r\n";
	//	dcout() << "message = " << xmsg->msg << "\r\n";
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

	//	dcout() << "sent nickname update\r\n";
	//	dcout() << "player GUID = " << myGuid << "\r\n";
	//	dcout() << "new name = " << myNickname << "\r\n";
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

// Re-read SYNCED_CINEMAS.txt so cutscene GUIDs can be added or removed without
// restarting the game. The list is swapped in under cinemaCriticalSection, so it
// is safe to do this while the cinema hook is live.
static void ChatCommandReloadCinemas(const std::string&)
{
	std::string loadedPath;
	const size_t count = loadSyncedCinemaGuids(&loadedPath);

	char message[512];
	if (loadedPath.empty())
	{
		snprintf(message, sizeof(message),
			"[System] No %s found - cutscene sync is off.", SYNCED_CINEMA_FILE);
	}
	else
	{
		snprintf(message, sizeof(message),
			"[System] Reloaded %zu synced cutscene(s) from %s",
			count, loadedPath.c_str());
	}

	g_ChatOverlay.AddSystemMessage(message);
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
	// The player-facing set /help lists in a normal session. Everything marked
	// DebugOnly below still works if typed - it is just kept out of the list
	// unless debug=1, so /help stays short enough to read on screen.
	g_ChatOverlay.AddCommand("/name", "<nickname> - Change your nickname", ChatCommandNickname);
	g_ChatOverlay.AddCommand("/status", "<status> - Change your status", ChatCommandStatus);
	g_ChatOverlay.AddCommand("/reconnect", "- Try to reconnect to the server", ChatCommandReconnect);
	g_ChatOverlay.AddCommand("/setTeam", "<0,1,2> - Set fake Bilbo's team", ChatCommandSetTeam);
	g_ChatOverlay.AddCommand("/spawn", "[template] - Spawn a rigid instance (synced to all players)", ChatCommandSpawn);

	g_ChatOverlay.AddCommand("/ai", "<mode> - Change AI mode", ChatCommandChangeAIMode,
		ChatCommandListing::DebugOnly);
	g_ChatOverlay.AddCommand("/damage", "<value> - Set damage of fake Bilbo", ChatCommandDamage,
		ChatCommandListing::DebugOnly);
	g_ChatOverlay.AddCommand("/syncanim", "- Sync animation frame of all animated objects on the level", ChatCommandSyncAnim,
		ChatCommandListing::DebugOnly);
	g_ChatOverlay.AddCommand("/reloadcinemas", "- Re-read SYNCED_CINEMAS.txt (synced cutscene list)", ChatCommandReloadCinemas,
		ChatCommandListing::DebugOnly);
	g_ChatOverlay.AddCommand("/spawnfx", "[fxName] - Play an FX effect (synced to all players)", ChatCommandSpawnFx,
		ChatCommandListing::DebugOnly);

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
	InitializeCriticalSection(&cinemaCriticalSection);
	loadSyncedCinemaGuids();
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
				dprintf("Client ID from server token: %.16" PRIx64 "\n", clientId);
				client.Connect(clientId, connectToken);

				char addrStr[256];
				client.GetAddress().ToString(addrStr, sizeof(addrStr));
				dprintf("Client address: %s\n", addrStr);
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
				dprintf("Reconnecting client with Client ID: %.16" PRIx64 "\n", clientId);
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

					// Broadcast any whitelisted cutscene that fired on us
					sendCinemaSync(client);

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
						dcout() << "hoistable acquire\r\n";
					}

					if (g_currentHoistable && (hoist_guid.Guid == 0 || pBilbo->_get_state() != BS_HOISTING)) {
						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(HOISTABLE_RELEASE));
						msg->hoistableGuid = g_currentHoistable->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);

						delete g_currentHoistable;
						g_currentHoistable = nullptr;
						dcout() << "hoistable release\r\n";
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
						dcout() << "pushblock acquire\r\n";
					}

					if (g_currentPushBlock && pBilbo->_get_state() != BS_PUSH_BLOCK) {
						auto* msg = static_cast<HoistableAcquireReleaseMessage*>(client.CreateMessage(PUSHBLOCK_RELEASE));
						msg->hoistableGuid = g_currentPushBlock->getGUID();
						msg->playerGuid = myGuid;
						msg->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msg);

						delete g_currentPushBlock;
						g_currentPushBlock = nullptr;
						dcout() << "pushblock release\r\n";
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

						dprintf("SENT UPDATE PUSHBOX %.2f %.2f %.2f\n", msgUpdate->x, msgUpdate->y, msgUpdate->z);

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

	// Before anything else logs: decide whether the development output is wanted.
	loadDebugLoggingFlag();

	std::cout << "Injected.\n";
	if (g_debugLogging)
		std::cout << "Debug logging enabled (debug=1 in "
		          << SkinSync::LocalConfigFile << ").\n";

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
	// LOAD SETTINGS
	//-----------------------------------------------------

	localSkinLoaded =
		loadLocalSkinDefinitionFromKnownPaths(
			localSkinDefinition,
			localConfigPath,
			localSkinError);

	// Report the settings file itself first - it carries server_ip, debug, the
	// player profile and the skin, so "no skin configured" must not read like
	// "no settings found".
	const std::string settingsPath = resolveLocalProfileConfigPath();
	std::error_code settingsPathError;
	if (SkinSync::fs::exists(SkinSync::fs::path(settingsPath), settingsPathError))
	{
		std::cout
			<< "Settings from: "
			<< settingsPath
			<< "\n";
	}
	else
	{
		std::cout
			<< "No "
			<< SkinSync::LocalConfigFile
			<< " found (current folder, game folder, DLL folder) - using defaults.\n";
	}

	if (localSkinLoaded)
	{
		std::cout
			<< "Skin upload: "
			<< localSkinDefinition.fileName
			<< "\n";
	}
	else
	{
		std::cout
			<< "Skin upload disabled: "
			<< localSkinError
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

	// yojimbo does its own per-packet logging; keep it to errors unless we are
	// debugging, otherwise it prints over everything else on its own.
	yojimbo_log_level(
		g_debugLogging ? YOJIMBO_LOG_LEVEL_INFO : YOJIMBO_LOG_LEVEL_ERROR);

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
