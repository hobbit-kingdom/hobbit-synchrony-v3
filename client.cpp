/*
	client.cpp
	Hobbit Multiplayer Client

	Connects to the game server, reads local player (Bilbo) state from the
	Hobbit game process, sends position updates, and interpolates remote
	player positions onto NPC entities.
*/

#define _CRT_SECURE_NO_WARNINGS
#include "shared.h"
#include "Player.h"
#include "NPC.h"
#include "HobbitGameManager/HobbitGameManager.h"
#include "HobbitGameManager/HobbitProcessAnalyzer.h"

#undef SetPort
#undef SendMessage

#include <cstdio>
#include <csignal>
#include <fstream>
#include <unordered_map>

#include <algorithm>
#include <vector>
using namespace yojimbo;

// ===========================================================================
//  Globals
// ===========================================================================

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

// --- Remote players ---
static std::vector<Player> activePlayers;

static std::vector<uint64_t> playerGuids;

// --- Animation data caches ---
std::unordered_map<uint32_t, uint32_t> animDataMap;
static std::unordered_map<uint32_t, float> animFrameRanges;


std::unordered_map<uint64_t, NPC*> enemies;
const uint32_t X_POSITION_PTR = 0x0075BA3C;

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

	std::vector<uint32_t> allFriendAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000100000001, 0x184 + 0x8 * 0x4); //put the values that indicate that thing
	std::vector<uint32_t> allEnemieAddrs = processAnalyzer->findAllGameObjByPattern<uint64_t>(0x0000000200000002, 0x184 + 0x8 * 0x4); //put the values that indicate that thing

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

	printf("Enemies Found: %d", enemies.size());
}

static void readLocalPlayerState()
{
	localPos.x = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C4);
	localPos.y = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7C8);
	localPos.z = processAnalyzer->readData<float>(bilboPosBasePtr + 0x7CC);
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

		temp[enemy.first] = { ePos.x, ePos.y, ePos.z, eRot, eAnim, eHealth };
	}

	return temp;
}

// ===========================================================================
//  Server IP Config
// ===========================================================================

static std::string readServerIP()
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

	player.targetAnimation = msg->animation;
	player.lerpStartTime = currentTime;
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
	}

	activePlayers.push_back(newPlayer);
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

	std::cout << "Player " << msg->playerGuid
		<< ": pos(" << msg->x << ", "
		<< msg->y << ", "
		<< msg->z << ") rot("
		<< msg->rotationY << ") anim("
		<< msg->animation << ") frame("
		<< msg->animFrame << ") weapon("
		<< msg->bilboWeapon << ") level("
		<< msg->nowLevel << ")"
		<< "\n";
}

static void processEnemiesUpdate(EnemiesStateMessage* msg, double currentTime)
{
	if (msg->nowLevel != nowLevel) return;

	std::unordered_map<uint64_t, Enemy> updates = msg->enemies;

	for (auto enemyUpdate : updates)
	{
		if (enemies[enemyUpdate.first] == nullptr || !enemies[enemyUpdate.first]->isValid()) continue;

		enemies[enemyUpdate.first]->setPosition(enemyUpdate.second.x, enemyUpdate.second.y, enemyUpdate.second.z);
		enemies[enemyUpdate.first]->setRotationY(enemyUpdate.second.rot);
		enemies[enemyUpdate.first]->setHealth(enemyUpdate.second.health);
		if (enemies[enemyUpdate.first]->getAnimation() != enemyUpdate.second.anim)
			enemies[enemyUpdate.first]->setNPCAnim(enemyUpdate.second.anim);

	}
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
	case GUID_ASSIGN:
		myGuid = static_cast<GuidAssignMessage*>(message)->guid;
		printf("Assigned my GUID: %llu\n", myGuid);
		break;

	default:
		break;
	}
}

// ===========================================================================
//  Client Main Loop
// ===========================================================================

static int clientMain()
{
	printf("\nConnecting client (insecure)...\n");

	double time = NetDefaults::INITIAL_TIME;
	double lastSend = time;

	// Generate a random client ID
	uint64_t clientId = 0;
	yojimbo_random_bytes(reinterpret_cast<uint8_t*>(&clientId), 8);
	printf("Client ID: %.16" PRIx64 "\n", clientId);

	// Read server address
	std::string serverIP = readServerIP();
	Address serverAddress(serverIP.c_str(), ServerPort);

	// Allow command-line override
	// if (argc == 2)
	// {
	// 	Address cliAddr(argv[1]);
	// 	if (cliAddr.IsValid())
	// 	{
	// 		if (cliAddr.GetPort() == 0)
	// 			cliAddr.SetPort(ServerPort);
	// 		serverAddress = cliAddr;
	// 	}
	// }

	// Connect
	ClientServerConfig config;
	Client client(GetDefaultAllocator(), Address("0.0.0.0"), config, gameAdapter, time);

	uint8_t privateKey[KeyBytes] = {};
	client.InsecureConnect(privateKey, clientId, serverAddress);

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
				msg->x = localPos.x;
				msg->y = localPos.y;
				msg->z = localPos.z;
				msg->rotationY = localRot.y;
				msg->animation = localAnimation;
				msg->animFrame = localAnimFrame;
				msg->lastAnimFrame = localLastAnimFrame;
				msg->playerGuid = myGuid;

				msg->bilboWeapon = bilboWeapon;
				msg->nowLevel = nowLevel;

				client.SendMessage(0, msg);

				if (isHost == 1)
				{
					//ENEMIES
					//////////////////////////////////
					std::unordered_map<uint64_t, Enemy> enemiesPosSnap = readEnemiesState();

					auto* msgEnemy = static_cast<EnemiesStateMessage*>(client.CreateMessage(ENEMIES_UPDATE));

					msgEnemy->enemies = enemiesPosSnap;

					msgEnemy->nowLevel = nowLevel;

					client.SendMessage(0, msgEnemy);
					//////////////////////////////////
				}

				lastSend = time;
			}
		}

		time += NetDefaults::DELTA_TIME;
		client.AdvanceTime(time);

		if (client.ConnectionFailed())
			break;

		yojimbo_sleep(NetDefaults::DELTA_TIME);
	}

	client.Disconnect();
	return 0;
}


void SendKeyPress(HWND hwnd, WPARAM key) {
	// Send a WM_KEYDOWN message
	PostMessage(hwnd, WM_KEYDOWN, key, 0);

	// Send a WM_KEYUP message
	PostMessage(hwnd, WM_KEYUP, key, 0);
}

std::string GetKeyName(int vkCode)
{
	char keyName[32] = { 0 };
	if (GetKeyNameTextA(MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC) << 16, keyName, sizeof(keyName)))
	{
		return std::string(keyName);
	}
	return "Unknown";
}
// ===========================================================================
//  Entry Point
// ===========================================================================

int mainThread()
{
	AllocConsole();
	freopen("CONOUT$", "w",
		stdout);
	freopen("CONIN$", "r",
		stdin);

	// Attach to the Hobbit game process
	gameManager.start();
	processAnalyzer = gameManager.getHobbitProcessAnalyzer();

	playerGuids = loadGuidsFromFile("FAKE_BILBO_GUID.txt");

	std::cout << "SIZE " << playerGuids.size() << "\n";

	std::cout << "Are you the host? (yes - 1 / no - 0)\n";
	std::cin >> isHost;

	if (!InitializeYojimbo())
	{
		printf("Error: failed to initialize Yojimbo!\n");
		return 1;
	}

	yojimbo_log_level(YOJIMBO_LOG_LEVEL_INFO);
	srand(static_cast<unsigned int>(::time(NULL)));

	int result = clientMain();

	ShutdownYojimbo();
	return result;
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD fdwReason, LPVOID)
{
	DisableThreadLibraryCalls(hInstance);

	if (fdwReason == DLL_PROCESS_ATTACH) {
		CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)mainThread, NULL, 0, NULL));
	}

	return TRUE;
}
