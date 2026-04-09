#pragma once

#include "shared.h"
#include "netcode.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace SecureConnect
{
	constexpr int ConnectTokenPort = ServerPort + 1;
	constexpr int ConnectTokenExpirySeconds = 30;
	constexpr int SocketTimeoutMs = 5000;
	constexpr const char* PrivateKeyFile = "server_private_key.txt";

	struct NetworkSettings
	{
		std::string bindIp = NetDefaults::DEFAULT_IP;
		std::string publicIp = NetDefaults::DEFAULT_IP;
	};

	inline std::string trim(const std::string& value)
	{
		const size_t start = value.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return {};

		const size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	inline std::string toLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}

	inline NetworkSettings loadNetworkSettings()
	{
		NetworkSettings settings;

		std::ifstream configFile(NetDefaults::CONFIG_FILE);
		if (!configFile.is_open())
			return settings;

		bool sawKeyValue = false;
		std::string legacyIp;
		std::string line;
		while (std::getline(configFile, line))
		{
			const std::string trimmed = trim(line);
			if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
				continue;

			const size_t equals = trimmed.find('=');
			if (equals == std::string::npos)
			{
				if (!sawKeyValue && legacyIp.empty())
					legacyIp = trimmed;
				continue;
			}

			sawKeyValue = true;

			const std::string key = toLower(trim(trimmed.substr(0, equals)));
			const std::string value = trim(trimmed.substr(equals + 1));
			if (value.empty())
				continue;

			if (key == "bind_ip")
				settings.bindIp = value;
			else if (key == "public_ip" || key == "server_ip")
				settings.publicIp = value;
		}

		if (!sawKeyValue && !legacyIp.empty())
		{
			settings.bindIp = legacyIp;
			settings.publicIp = legacyIp;
		}
		else if (settings.publicIp.empty())
		{
			settings.publicIp = settings.bindIp;
		}

		return settings;
	}

	inline std::string getClientServerIp()
	{
		NetworkSettings settings = loadNetworkSettings();
		if (!settings.publicIp.empty())
			return settings.publicIp;
		return settings.bindIp.empty() ? std::string(NetDefaults::DEFAULT_IP) : settings.bindIp;
	}

	inline bool parseHexPrivateKey(const std::string& hex, uint8_t key[yojimbo::KeyBytes])
	{
		std::string compact;
		compact.reserve(hex.size());
		for (char c : hex)
		{
			if (!std::isspace(static_cast<unsigned char>(c)))
				compact.push_back(c);
		}

		if (compact.size() != static_cast<size_t>(yojimbo::KeyBytes * 2))
			return false;

		for (int i = 0; i < yojimbo::KeyBytes; ++i)
		{
			const std::string byteString = compact.substr(i * 2, 2);
			char* end = nullptr;
			const long value = std::strtol(byteString.c_str(), &end, 16);
			if (!end || *end != '\0' || value < 0 || value > 255)
				return false;

			key[i] = static_cast<uint8_t>(value);
		}

		return true;
	}

	inline std::string privateKeyToHex(const uint8_t key[yojimbo::KeyBytes])
	{
		static const char* digits = "0123456789abcdef";
		std::string hex;
		hex.resize(yojimbo::KeyBytes * 2);

		for (int i = 0; i < yojimbo::KeyBytes; ++i)
		{
			hex[i * 2] = digits[(key[i] >> 4) & 0x0F];
			hex[i * 2 + 1] = digits[key[i] & 0x0F];
		}

		return hex;
	}

	inline bool loadOrCreatePrivateKey(const std::string& path, uint8_t key[yojimbo::KeyBytes], bool& created)
	{
		created = false;

		std::ifstream existing(path);
		if (existing.is_open())
		{
			std::string hex;
			std::getline(existing, hex);
			return parseHexPrivateKey(hex, key);
		}

		yojimbo_random_bytes(key, yojimbo::KeyBytes);

		std::ofstream output(path, std::ios::trunc);
		if (!output.is_open())
			return false;

		output << privateKeyToHex(key) << "\n";
		created = true;
		return true;
	}

	inline std::string buildServerAddressString(const std::string& host, int port)
	{
		yojimbo::Address address(host.c_str(), port);
		if (!address.IsValid())
			return {};

		char buffer[yojimbo::MaxAddressLength];
		address.ToString(buffer, sizeof(buffer));
		return buffer;
	}

#pragma pack(push, 1)
	struct ConnectTokenRequestPacket
	{
		uint32_t magic = 0x314B5459u;
		uint32_t version = 1;
	};

	struct ConnectTokenResponsePacket
	{
		uint32_t magic = 0x31525459u;
		uint32_t version = 1;
		int32_t status = 0;
		uint64_t clientId = 0;
		uint8_t connectToken[yojimbo::ConnectTokenBytes] = {};
	};
#pragma pack(pop)

	enum ConnectTokenStatus : int32_t
	{
		ConnectTokenStatus_Ok = 0,
		ConnectTokenStatus_ServerError = 1,
		ConnectTokenStatus_BadRequest = 2
	};

	inline bool isValidRequest(const ConnectTokenRequestPacket& request)
	{
		return request.magic == 0x314B5459u && request.version == 1;
	}

	inline bool isValidResponse(const ConnectTokenResponsePacket& response)
	{
		return response.magic == 0x31525459u && response.version == 1;
	}

	inline bool sendAll(SOCKET socketHandle, const void* data, int bytes)
	{
		const char* cursor = static_cast<const char*>(data);
		int remaining = bytes;
		while (remaining > 0)
		{
			const int sent = send(socketHandle, cursor, remaining, 0);
			if (sent == SOCKET_ERROR || sent == 0)
				return false;

			cursor += sent;
			remaining -= sent;
		}

		return true;
	}

	inline bool recvAll(SOCKET socketHandle, void* data, int bytes)
	{
		char* cursor = static_cast<char*>(data);
		int remaining = bytes;
		while (remaining > 0)
		{
			const int received = recv(socketHandle, cursor, remaining, 0);
			if (received == SOCKET_ERROR || received == 0)
				return false;

			cursor += received;
			remaining -= received;
		}

		return true;
	}

	inline void applySocketTimeouts(SOCKET socketHandle, int timeoutMs)
	{
		setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
		setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
			reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
	}

	inline bool connectWithTimeout(SOCKET socketHandle, const sockaddr* address, int addressLength, int timeoutMs)
	{
		u_long nonBlocking = 1;
		if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0)
			return false;

		int result = connect(socketHandle, address, addressLength);
		if (result == SOCKET_ERROR)
		{
			const int error = WSAGetLastError();
			if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL)
			{
				nonBlocking = 0;
				ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
				return false;
			}

			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(socketHandle, &writeSet);

			TIMEVAL timeout;
			timeout.tv_sec = timeoutMs / 1000;
			timeout.tv_usec = (timeoutMs % 1000) * 1000;

			result = select(0, nullptr, &writeSet, nullptr, &timeout);
			if (result <= 0)
			{
				nonBlocking = 0;
				ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
				return false;
			}

			int socketError = 0;
			int socketErrorSize = sizeof(socketError);
			if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
				reinterpret_cast<char*>(&socketError), &socketErrorSize) != 0 || socketError != 0)
			{
				nonBlocking = 0;
				ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
				return false;
			}
		}

		nonBlocking = 0;
		ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
		applySocketTimeouts(socketHandle, timeoutMs);
		return true;
	}

	inline bool generateConnectToken(const uint8_t privateKey[yojimbo::KeyBytes],
		const std::string& publicIp,
		const yojimbo::ClientServerConfig& config,
		uint64_t& clientId,
		uint8_t connectToken[yojimbo::ConnectTokenBytes])
	{
		const std::string serverAddress = buildServerAddressString(publicIp, ServerPort);
		if (serverAddress.empty())
			return false;

		yojimbo_random_bytes(reinterpret_cast<uint8_t*>(&clientId), sizeof(clientId));

		const char* serverAddressPointers[NETCODE_MAX_SERVERS_PER_CONNECT] = {};
		serverAddressPointers[0] = serverAddress.c_str();

		uint8_t userData[NETCODE_USER_DATA_BYTES] = {};
		return netcode_generate_connect_token(1,
			serverAddressPointers,
			serverAddressPointers,
			ConnectTokenExpirySeconds,
			config.timeout,
			clientId,
			config.protocolId,
			privateKey,
			userData,
			connectToken) == NETCODE_OK;
	}

	inline bool requestConnectTokenFromServer(const std::string& serverIp,
		uint64_t& clientId,
		uint8_t connectToken[yojimbo::ConnectTokenBytes])
	{
		addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo* result = nullptr;
		const std::string portString = std::to_string(ConnectTokenPort);
		if (getaddrinfo(serverIp.c_str(), portString.c_str(), &hints, &result) != 0)
			return false;

		SOCKET socketHandle = INVALID_SOCKET;
		bool connected = false;
		for (addrinfo* it = result; it != nullptr; it = it->ai_next)
		{
			socketHandle = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
			if (socketHandle == INVALID_SOCKET)
				continue;

			if (connectWithTimeout(socketHandle, it->ai_addr, static_cast<int>(it->ai_addrlen), SocketTimeoutMs))
			{
				connected = true;
				break;
			}

			closesocket(socketHandle);
			socketHandle = INVALID_SOCKET;
		}

		freeaddrinfo(result);

		if (!connected || socketHandle == INVALID_SOCKET)
			return false;

		ConnectTokenRequestPacket request;
		ConnectTokenResponsePacket response;
		const bool ok = sendAll(socketHandle, &request, sizeof(request))
			&& recvAll(socketHandle, &response, sizeof(response))
			&& isValidResponse(response)
			&& response.status == ConnectTokenStatus_Ok;

		if (ok)
		{
			clientId = response.clientId;
			memcpy(connectToken, response.connectToken, sizeof(response.connectToken));
		}

		closesocket(socketHandle);
		return ok;
	}
}
