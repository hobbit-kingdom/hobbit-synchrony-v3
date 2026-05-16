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
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include <algorithm>
#include <vector>

#include "Hoistable.h"


#include <Windows.h>
#include <string>

#include "ChatOverlay.h"

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
static yojimbo::Client*        g_Client = nullptr;

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

// --- Remote players ---
static std::vector<Player> activePlayers;

static std::vector<uint64_t> playerGuids;
static std::unordered_map<uint64_t, std::string> playerTextureNames;
static std::unordered_map<uint64_t, std::string> playerSkinFilePaths;

static SkinSync::LocalSkinDefinition localSkinDefinition;
static bool localSkinLoaded = false;
static bool localSkinUploadAttempted = false;
static std::string localSkinConfigPath;
static std::string localSkinConfigError;

static std::vector<uint64_t> hoistableGuidTest;

// --- Animation data caches ---
std::unordered_map<uint32_t, uint32_t> animDataMap;
static std::unordered_map<uint32_t, float> animFrameRanges;


std::unordered_map<uint64_t, NPC*> enemies;
std::unordered_map<uint64_t, Hoistable*> hoistables;
const uint32_t X_POSITION_PTR = 0x0075BA3C;

Hoistable* testHoistable;

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


// ===========================================================================
//  Game Memory Reading
// ===========================================================================

static void readGamePointers()
{
	activePlayers.clear();
	nowLevel = processAnalyzer->readData<uint32_t>(0x00762B5C);


	bilboPosBasePtr = processAnalyzer->readData<uint32_t>(X_POSITION_PTR);
	bilboAnimPtr = 0x8 + processAnalyzer->readData<uint32_t>(
		0x560 + processAnalyzer->readData<uint32_t>(X_POSITION_PTR));

	//Enemies 
	enemies.clear();
	printf("List Enemies\n");

	//
	hoistables.clear();

	std::vector<uint32_t> allFriendAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000100000001, 0x184 + 0x8 * 0x4); //put the values that indicate that thing
	std::vector<uint32_t> allEnemieAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000200000002, 0x184 + 0x8 * 0x4); //put the values that indicate that thing
	std::vector<uint32_t> allRigidInstances = processAnalyzer->findAllGameObjByPattern<uint8_t>(0x05, 0x7c); //put the values that indicate that thing
	std::cout << "SIZE OF ALL RIGID: " << allRigidInstances.size() << '\n';

	std::vector<uint32_t> allHoistables;
	for (auto rigidIn : allRigidInstances)
	{
		if (processAnalyzer->readData<uint8_t>(rigidIn + 0x13) == 2)
		{
			allHoistables.push_back(rigidIn);
		}
	}

	for (uint32_t hoisAddr : allHoistables)
	{
		Hoistable* ho = new Hoistable(processAnalyzer);;
		ho->initializeByAddress(hoisAddr);

		hoistables.emplace(ho->getGUID(), ho);
	}

	std::cout << "SIZE OF ALL HOISTABLES: " << allHoistables.size() << '\n';

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

		NPC* enemy = new NPC(processAnalyzer);;
		enemy->initializeByAddress(e);

		enemies.emplace(enemy->getGUID(), enemy);
	}

	// testHoistable = new Hoistable(processAnalyzer);
	// hoistableGuidTest = loadGuidsFromFile("hoistable.txt");
	// testHoistable->initializeByGuid(hoistableGuidTest[0]);

	printf("Enemies Found: %d", enemies.size());

}

static Vector3 getBilboPos(void)
{
	Vector3 pos;
	pos.x = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C4);
	pos.y = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C8);
	pos.z = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7CC);
	return pos;
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

	// Read weapon 
	bilboWeapon = processAnalyzer->readData<uint32_t>(0x0075C738);
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

static void updateExistingPlayer(Player& player, PositionMessage* msg, double currentTime)
{
	// --- Snapshot previous position ---
	player.prevX = player.x;
	player.prevY = player.y;
	player.prevZ = player.z;
	player.targetX = msg->x;
	player.targetY = msg->y;
	player.targetZ = msg->z;


	if (player.bilboWeapon != msg->bilboWeapon) player.setWeapon(msg->bilboWeapon);
	player.nowLevel = msg->nowLevel;

	// Don't render NPC for the local player
	if (player.npcGuid == myGuid)
		player.npc = nullptr;

	// --- Rotation ---
	player.targetRotationY = msg->rotationY;

	// --- Animation ---
	if (player.animation == msg->animation)
	{
		// Same animation — smoothly lerp frames
		float range = getClampedFrameRange(msg->animation, msg->lastAnimFrame);

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
		player.targetLastAnimFrame = range;
	}
	else
	{
		// Different animation — snap immediately
		float range = getClampedFrameRange(msg->animation, msg->lastAnimFrame);

		player.animation = msg->animation;
		player.animFrame = msg->animFrame;
		player.lastAnimFrame = range;
		player.prevAnimFrame = msg->animFrame;
		player.targetAnimFrame = msg->animFrame;
		player.prevLastAnimFrame = player.lastAnimFrame;
		player.targetLastAnimFrame = player.lastAnimFrame;

		// Apply new animation to NPC right away
		if (player.npc && player.npc->isValid())
		{
			if (msg->animation >= 0 && msg->animation <= 200)
				player.npc->setNPCAnim(msg->animation);
		}
	}

	player.nickname_marker->setPosition(msg->x, msg->y + 100.f, msg->z);

	player.targetAnimation = msg->animation;
	player.lerpStartTime = currentTime;
	applySkinMetadataToPlayer(player);
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

		if (newPlayer.npc->isValid())
		{
			newPlayer.npc->setPosition(msg->x, msg->y, msg->z);
			newPlayer.npc->setRotationY(msg->rotationY);
			if (msg->animation >= 0 && msg->animation <= 200)
			{
				newPlayer.npc->setNPCAnim(msg->animation);
				//newPlayer.npc->setAnimFrames(msg->animFrame, msg->lastAnimFrame);
			}
			newPlayer.npc->setWeapon(msg->bilboWeapon);
		}

		uint64_t nicknameGuid = (newPlayer.npcGuid & 0xFFFFFFFF) | 0x0D8AD91100000000ull;

		newPlayer.nickname_marker = new Marker(processAnalyzer);
		newPlayer.nickname_marker->initializeByGuid(nicknameGuid);
		newPlayer.nickname_marker->setPosition(msg->x, msg->y + 100.f, msg->z);
	}

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
	{
		return;
	}

	Player* existing = findPlayerByGuid(msg->playerGuid);
	if (existing)
		updateExistingPlayer(*existing, msg, currentTime);
	else
		addNewPlayer(msg, currentTime);
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

static void processHoistableUpdate(HoistableStateMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel)
	{
		return;
	}

	const auto hoistableIt = hoistables.find(msg->hoistableGuid);
	if (hoistableIt == hoistables.end() || hoistableIt->second == nullptr || !hoistableIt->second->isValid())
		return;

	hoistableIt->second->setPosition(msg->x, msg->y, msg->z);
	// hoistables[msg->hoistableGuid]->setRotationY(msg->rotationY);

}

static void processEnemiesUpdate(EnemiesStateMessage* msg, double /*currentTime*/)
{
	if (msg->nowLevel != nowLevel) return;

	std::unordered_map<uint64_t, Enemy> updates = msg->enemies;

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
}

static void processNicknameUpdate(NicknameUpdateMessage* msg)
{
	std::cout << "processNicknameUpdate\r\n";
	std::cout << "player GUID = " << msg->player_guid << "\r\n";
	std::cout << "new name = " << msg->new_name << "\r\n";

	Player* pl = findPlayerByGuid(msg->player_guid);
	if (pl) {
		std::cout << "playerFound\r\n";
		if (pl->nickname_marker) {
			std::cout << "markerFound\r\n";
			pl->nickname_marker->setText(msg->new_name);
		}
	}
}

static void processMessage(Client& /*client*/, Message* message, double time)
{
	switch (message->GetType())
	{
	case POSITION_UPDATE:
		if (gameManager.isOnLevel()) processPositionUpdate(static_cast<PositionMessage*>(message), time);
		break;
	case ENEMIES_UPDATE:
		if (gameManager.isOnLevel()) processEnemiesUpdate(static_cast<EnemiesStateMessage*>(message), time);
		break;
	case HOISTABLE_UPDATE:
		if (gameManager.isOnLevel()) processHoistableUpdate(static_cast<HoistableStateMessage*>(message), time);
		break;
	case GUID_ASSIGN:
		myGuid = static_cast<GuidAssignMessage*>(message)->guid;
		localSkinUploadAttempted = false;
		printf("Assigned my GUID: %llu\n", myGuid);

		myNicknameGuid = (myGuid & 0xFFFFFFFF) | 0x0D8AD91100000000ull;

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

	default:
		break;
	}
}

static void OnChatNickname(const std::string& name)
{
	if (!g_Client || myGuid == 0)
		return;

	// Update local marker
	Marker marker(processAnalyzer);
	marker.initializeByGuid(myNicknameGuid);
	Vector3 p = getBilboPos();
	p.y += 100.f;
	marker.setPosition(p.x, p.y, p.z);
	marker.setText(name.c_str());

	// Send network message
	auto* xmsg = static_cast<NicknameUpdateMessage*>(g_Client->CreateMessage(NICKNAME_UPDATE));
	xmsg->player_guid = myGuid;
	strncpy(xmsg->new_name, name.c_str(), sizeof(xmsg->new_name));
	xmsg->new_name[31] = '\0';
	g_Client->SendMessage(channels::Gameplay, xmsg);
}

// ===========================================================================
//  Client Main Loop
// ===========================================================================

static int clientMain()
{
	printf("\nConnecting client with a server-issued token...\n");

	double time = NetDefaults::INITIAL_TIME;
	double lastSend = time;

	// Read server address
	std::string serverIP = readSecureServerIP();

	// Connect
	ClientServerConfig config;
	ConfigureGameNetworking(config);
	Client client(GetDefaultAllocator(), Address("0.0.0.0"), config, gameAdapter, time);

	g_Client = &client;
	ChatOverlay_SetNicknameCallback(OnChatNickname);

	uint64_t clientId = 0;
	uint8_t connectToken[ConnectTokenBytes] = {};
	if (!SecureConnect::requestConnectTokenFromServer(serverIP, clientId, connectToken))
	{
		printf("Failed to request connect token from %s:%d\n", serverIP.c_str(), SecureConnect::ConnectTokenPort);
		return 1;
	}

	printf("Client ID from server token: %.16" PRIx64 "\n", clientId);
	client.Connect(clientId, connectToken);

	char addrStr[256];
	client.GetAddress().ToString(addrStr, sizeof(addrStr));
	printf("Client address: %s\n", addrStr);

	signal(SIGINT, interruptHandler);

	// --- Game loop ---
	while (!quit)
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
			activePlayers.clear();
			enemies.clear();
		}

		// Process incoming messages on all channels
		for (int ch = 0; ch < config.numChannels; ch++)
		{
			Message* msg = client.ReceiveMessage(ch);
			while (msg)
			{
				processMessage(client, msg, time);

				client.ReleaseMessage(msg);
				msg = client.ReceiveMessage(ch);
			}
		}

		sendLocalSkinData(client);

		// Interpolate all remote players
		for (auto& player : activePlayers)
		{
			double elapsed = time - player.lerpStartTime;
			float t = static_cast<float>(elapsed / NetDefaults::LERP_DURATION);
			if (t > 1.0f) t = 1.0f;
			player.tickLerp(t);
		}

		if (client.IsDisconnected())
			break;

		// Send local player state at the configured tick rate
		if (client.IsConnected() && myGuid != 0)
		{
			if (time - lastSend > NetDefaults::SEND_INTERVAL && gameManager.isOnLevel())
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
					//////////////////////////////////
					std::unordered_map<uint64_t, Enemy> enemiesPosSnap = readEnemiesState();

					auto* msgEnemy = static_cast<EnemiesStateMessage*>(client.CreateMessage(ENEMIES_UPDATE));

					msgEnemy->enemies = enemiesPosSnap;

					msgEnemy->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

					client.SendMessage(channels::Gameplay, msgEnemy);
					//////////////////////////////////

					for (auto hoists : hoistables)
					{
						auto* msgHoistable = static_cast<HoistableStateMessage*>(client.CreateMessage(HOISTABLE_UPDATE));

						Vector3 v = hoists.second->getPosition();

						msgHoistable->x = NetworkClamp::sanitizePosition(v.x);
						msgHoistable->y = NetworkClamp::sanitizePosition(v.y);
						msgHoistable->z = NetworkClamp::sanitizePosition(v.z);

						msgHoistable->rotationY = NetworkClamp::sanitizeRotationRadians(hoists.second->getRotationY());

						msgHoistable->hoistableGuid = hoists.first;
						msgHoistable->nowLevel = NetworkClamp::sanitizeLevel(nowLevel);

						client.SendMessage(channels::Gameplay, msgHoistable);
					}
				}




				lastSend = time;
			}
		}

		// update marker
		{
			Marker marker(processAnalyzer);
			marker.initializeByGuid(myNicknameGuid);

			Vector3 p = getBilboPos();
			p.y += 100.f;
			marker.setPosition(p.x, p.y, p.z);

			if (kbhit()) {
				char buf[128];
				fgets(buf, 128, stdin);
				marker.setText(buf);

				{
					auto* xmsg = static_cast<NicknameUpdateMessage*>(client.CreateMessage(NICKNAME_UPDATE));

					xmsg->player_guid = myGuid;
					strncpy(xmsg->new_name, buf, sizeof(xmsg->new_name));
					xmsg->new_name[31] = '\0';

					client.SendMessage(channels::Gameplay, xmsg);

					std::cout << "sent nickname update\r\n";
					std::cout << "player GUID = " << myGuid << "\r\n";
					std::cout << "new name = " << xmsg->new_name << "\r\n";
				}
			}
		}

		time += NetDefaults::DELTA_TIME;
		client.AdvanceTime(time);

		if (client.ConnectionFailed())
			break;

		yojimbo_sleep(NetDefaults::DELTA_TIME);
	}

	client.Disconnect();
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

	ChatOverlay_Init(moduleInstance);

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

	ChatOverlay_Shutdown();

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




