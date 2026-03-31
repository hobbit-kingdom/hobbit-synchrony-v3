/*
	server.cpp
	Hobbit Multiplayer Server

	Accepts client connections, assigns each client a unique NPC GUID,
	and broadcasts position updates between all connected clients.
*/

#include "shared.h"
#include "GuidManager.h"

#undef SendMessage

#include <cstdio>
#include <csignal>
#include <fstream>
#include <string>

using namespace yojimbo;

// ===========================================================================
//  Globals
// ===========================================================================

static volatile int quit = 0;

static void interruptHandler(int) { quit = 1; }

static GuidManager guidManager;

// ===========================================================================
//  Server Adapter — handles connect/disconnect events
// ===========================================================================

class ServerAdapter : public Adapter
{
public:
	Server* server = nullptr;

	MessageFactory* CreateMessageFactory(Allocator& allocator) override
	{
		return YOJIMBO_NEW(allocator, GameMessageFactory, allocator);
	}

	void OnServerClientConnected(int clientIndex) override
	{
		uint64_t guid = guidManager.assignGuid(clientIndex);
		if (guid == 0)
		{
			printf("No available GUIDs for client %d!\n", clientIndex);
			return;
		}

		// Tell the client which GUID they've been assigned
		if (server)
		{
			auto* msg = static_cast<GuidAssignMessage*>(
				server->CreateMessage(clientIndex, GUID_ASSIGN));
			msg->guid = guid;
			server->SendMessage(clientIndex, 0, msg);
		}
	}

	void OnServerClientDisconnected(int clientIndex) override
	{
		guidManager.releaseGuid(clientIndex);
	}
};

// ===========================================================================
//  Message Handling
// ===========================================================================

static void broadcastPosition(Server& server, int senderIndex, PositionMessage* msg)
{
	uint64_t senderGuid = guidManager.getGuid(senderIndex);

	for (int i = 0; i < NetDefaults::MAX_CLIENTS; i++)
	{
		if (i == senderIndex || !server.IsClientConnected(i))
			continue;

		auto* broadcast = static_cast<PositionMessage*>(
			server.CreateMessage(i, POSITION_UPDATE));

		broadcast->x = msg->x;
		broadcast->y = msg->y;
		broadcast->z = msg->z;
		broadcast->rotationY = msg->rotationY;
		broadcast->animation = msg->animation;
		broadcast->animFrame = msg->animFrame;
		broadcast->lastAnimFrame = msg->lastAnimFrame;
		broadcast->bilboWeapon = msg->bilboWeapon;
		broadcast->nowLevel = msg->nowLevel;
		broadcast->playerGuid = senderGuid;

		server.SendMessage(i, 0, broadcast);
	}
}



static void broadcastHoistableUpdate(Server& server, int senderIndex, HoistableStateMessage* msg)
{
	for (int i = 0; i < NetDefaults::MAX_CLIENTS; i++)
	{
		if (i == senderIndex || !server.IsClientConnected(i))
			continue;

		auto* broadcast = static_cast<HoistableStateMessage*>(
			server.CreateMessage(i, HOISTABLE_UPDATE));

		broadcast->x = msg->x;
		broadcast->y = msg->y;
		broadcast->z = msg->z;
		broadcast->rotationY = msg->rotationY;

		broadcast->hoistableGuid = msg->hoistableGuid;

		broadcast->nowLevel = msg->nowLevel;

		server.SendMessage(i, 0, broadcast);
	}
}


static void broadcastEnemyUpdate(Server& server, int senderIndex, EnemiesStateMessage* msg)
{
	for (int i = 0; i < NetDefaults::MAX_CLIENTS; i++)
	{
		if (i == senderIndex || !server.IsClientConnected(i))
			continue;

		auto* broadcast = static_cast<EnemiesStateMessage*>(
			server.CreateMessage(i, ENEMIES_UPDATE));

		broadcast->enemies = msg->enemies;

		broadcast->nowLevel = msg->nowLevel;

		server.SendMessage(i, 0, broadcast);
	}
}

static void processMessage(Server& server, int clientIndex, Message* message)
{
	switch (message->GetType())
	{
	case POSITION_UPDATE:
		broadcastPosition(server, clientIndex, static_cast<PositionMessage*>(message));
		break;
	case ENEMIES_UPDATE:
		broadcastEnemyUpdate(server, clientIndex, static_cast<EnemiesStateMessage*>(message));
		break;
	case HOISTABLE_UPDATE:
		broadcastHoistableUpdate(server, clientIndex, static_cast<HoistableStateMessage*>(message));
		break;
	default:
		break;
	}
}

// ===========================================================================
//  Server Main Loop
// ===========================================================================

static int serverMain()
{
	printf("Starting server on port %d (insecure)\n", ServerPort);

	// Load NPC GUIDs from file
	if (!guidManager.loadFromFile())
	{
		printf("No GUIDs loaded — exiting.\n");
		return 1;
	}

	ServerAdapter serverAdapter;

	double time = NetDefaults::INITIAL_TIME;
	ClientServerConfig config;

	uint8_t privateKey[KeyBytes] = {};

	// Read server bind IP from config.txt
	std::string bindIP = NetDefaults::DEFAULT_IP;
	std::ifstream configFile(NetDefaults::CONFIG_FILE);
	if (configFile.is_open())
	{
		std::string line;
		if (std::getline(configFile, line) && !line.empty())
		{
			bindIP = line;
			printf("Loaded bind IP from config.txt: %s\n", bindIP.c_str());
		}
		configFile.close();
	}
	else
	{
		printf("%s not found, using default IP: %s\n", NetDefaults::CONFIG_FILE, bindIP.c_str());
	}

	Server server(GetDefaultAllocator(), privateKey,
		Address(bindIP.c_str(), ServerPort),
		config, serverAdapter, time);

	server.Start(NetDefaults::MAX_CLIENTS);
	serverAdapter.server = &server;

	char addrStr[256];
	server.GetAddress().ToString(addrStr, sizeof(addrStr));
	printf("Server address: %s\n", addrStr);

	signal(SIGINT, interruptHandler);

	// --- Server loop ---
	while (!quit)
	{
		server.SendPackets();
		server.ReceivePackets();

		// Process messages from all connected clients
		for (int i = 0; i < NetDefaults::MAX_CLIENTS; i++)
		{
			if (!server.IsClientConnected(i))
				continue;

			for (int ch = 0; ch < config.numChannels; ch++)
			{
				Message* msg = server.ReceiveMessage(i, ch);
				while (msg)
				{
					processMessage(server, i, msg);
					server.ReleaseMessage(i, msg);
					msg = server.ReceiveMessage(i, ch);
				}
			}
		}

		time += NetDefaults::DELTA_TIME;
		server.AdvanceTime(time);

		if (!server.IsRunning())
			break;

		yojimbo_sleep(NetDefaults::DELTA_TIME);
	}

	server.Stop();
	return 0;
}

// ===========================================================================
//  Entry Point
// ===========================================================================

int main()
{
	if (!InitializeYojimbo())
	{
		printf("Error: failed to initialize Yojimbo!\n");
		return 1;
	}

	yojimbo_log_level(YOJIMBO_LOG_LEVEL_INFO);
	srand(static_cast<unsigned int>(::time(NULL)));

	int result = serverMain();

	ShutdownYojimbo();
	return result;
}
