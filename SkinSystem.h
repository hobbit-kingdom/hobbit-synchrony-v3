#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#if defined(_MSVC_LANG) && _MSVC_LANG >= 201703L
#include <filesystem>
#elif __cplusplus >= 201703L
#include <filesystem>
#else
#include <experimental/filesystem>
#endif

namespace channels
{
	constexpr int Gameplay = 0;
	constexpr int Skin = 1;
}

namespace SkinSync
{
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
	namespace fs = std::filesystem;
#else
	namespace fs = std::experimental::filesystem;
#endif

	constexpr int MaxTextureNameLength = 64;
	constexpr int MaxFileNameLength = 128;
	constexpr int MaxSkinFileBytes = 256 * 1024;

	constexpr const char* LocalSkinConfigFile = "skin_config.txt";
	constexpr const char* InstalledPropsDirectory = "common/props";

	struct LocalSkinDefinition
	{
		bool enabled = false;
		std::string textureName;
		std::string filePath;
		std::string fileName;
		std::vector<uint8_t> fileBytes;
	};

	inline std::string trim(const std::string& value)
	{
		size_t start = 0;
		while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
			++start;

		size_t end = value.size();
		while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
			--end;

		return value.substr(start, end - start);
	}

	inline std::string stripQuotes(const std::string& value)
	{
		if (value.size() >= 2)
		{
			const char first = value.front();
			const char last = value.back();
			if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
				return value.substr(1, value.size() - 2);
		}

		return value;
	}

	inline void copyStringToBuffer(const std::string& value, char* buffer, size_t bufferSize)
	{
		if (!buffer || bufferSize == 0)
			return;

		std::memset(buffer, 0, bufferSize);
		if (value.empty())
			return;

		const size_t bytesToCopy = std::min(bufferSize - 1, value.size());
		std::memcpy(buffer, value.data(), bytesToCopy);
	}

	inline std::string sanitizeToken(const std::string& value)
	{
		std::string result;
		result.reserve(value.size());

		for (unsigned char ch : value)
		{
			if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '[' || ch == ']')
				result.push_back(static_cast<char>(ch));
			else if (!std::isspace(ch))
				result.push_back('_');
		}

		return result;
	}

	inline std::string sanitizeFileName(const std::string& value)
	{
		return sanitizeToken(fs::path(value).filename().string());
	}

	inline std::string guidToString(uint64_t guid)
	{
		std::ostringstream stream;
		stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << guid;
		return stream.str();
	}

	inline int getGuidSlotIndex(const std::vector<uint64_t>& guidList, uint64_t guid)
	{
		for (size_t i = 0; i < guidList.size(); ++i)
		{
			if (guidList[i] == guid)
				return static_cast<int>(i) + 1;
		}

		return 0;
	}

	inline std::string getBilboGeometryNameForSlot(int slotIndex)
	{
		if (slotIndex <= 0)
			return {};

		return "bilbo" + std::to_string(slotIndex) + ".npcgeom";
	}

	inline std::string getBilboDiffuseFileNameForSlot(int slotIndex)
	{
		if (slotIndex <= 0)
			return {};

		return "bilb" + std::to_string(slotIndex) + "[d].xbmp";
	}

	inline std::string getGuidBoundSkinFileName(const std::vector<uint64_t>& guidList, uint64_t guid)
	{
		return getBilboDiffuseFileNameForSlot(getGuidSlotIndex(guidList, guid));
	}

	inline bool readBinaryFile(const fs::path& path, std::vector<uint8_t>& bytes)
	{
		bytes.clear();

		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open())
			return false;

		const std::streamsize fileSize = file.tellg();
		if (fileSize <= 0)
			return false;

		file.seekg(0, std::ios::beg);

		bytes.resize(static_cast<size_t>(fileSize));
		return file.read(reinterpret_cast<char*>(bytes.data()), fileSize).good();
	}

	inline bool writeBinaryFile(const fs::path& path, const uint8_t* data, size_t bytes)
	{
		if (!data || bytes == 0)
			return false;

		std::error_code error;
		fs::create_directories(path.parent_path(), error);

		std::ofstream file(path, std::ios::binary);
		if (!file.is_open())
			return false;

		file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
		return file.good();
	}

	inline fs::path getInstalledSkinPath(const std::string& fileName)
	{
		const std::string safeName = sanitizeFileName(fileName.empty() ? "skin.bin" : fileName);
		return fs::path(InstalledPropsDirectory) / safeName;
	}

	inline bool saveInstalledSkinFile(const std::string& fileName, const uint8_t* data, size_t bytes, std::string& savedPath)
	{
		const fs::path outputPath = getInstalledSkinPath(fileName);
		if (!writeBinaryFile(outputPath, data, bytes))
			return false;

		savedPath = outputPath.string();
		return true;
	}

	inline bool removeInstalledSkinFileByPath(const std::string& path)
	{
		if (path.empty())
			return false;

		std::error_code error;
		return fs::remove(fs::path(path), error);
	}

	inline bool loadLocalSkinDefinition(LocalSkinDefinition& outSkin, const std::string& configPath = LocalSkinConfigFile, std::string* errorMessage = nullptr)
	{
		outSkin = {};

		auto setError = [&](const std::string& message)
		{
			if (errorMessage)
				*errorMessage = message;
		};

		std::ifstream configFile(configPath);
		if (!configFile.is_open())
		{
			setError("could not open config file: " + fs::absolute(fs::path(configPath)).string());
			return false;
		}

		std::string textureName;
		std::string filePath;
		bool enabled = true;

		std::string line;
		while (std::getline(configFile, line))
		{
			line = trim(line);
			if (line.empty() || line[0] == '#' || line[0] == ';')
				continue;

			const size_t separator = line.find('=');
			if (separator == std::string::npos)
				continue;

			std::string key = trim(line.substr(0, separator));
			std::string value = stripQuotes(trim(line.substr(separator + 1)));

			std::transform(key.begin(), key.end(), key.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

			if (key == "texture_name" || key == "texture")
				textureName = sanitizeToken(value);
			else if (key == "file_path" || key == "file")
				filePath = value;
			else if (key == "enabled")
				enabled = !(value == "0" || value == "false" || value == "FALSE");
		}

		if (!enabled)
		{
			setError("skin sync is disabled in config: " + fs::absolute(fs::path(configPath)).string());
			return false;
		}

		if (filePath.empty())
		{
			setError("missing file_path in config: " + fs::absolute(fs::path(configPath)).string());
			return false;
		}

		fs::path resolvedFilePath = fs::path(filePath);
		if (resolvedFilePath.is_relative())
		{
			const fs::path configDirectory = fs::absolute(fs::path(configPath)).parent_path();
			resolvedFilePath = configDirectory / resolvedFilePath;
		}

		resolvedFilePath = fs::absolute(resolvedFilePath);

		if (!fs::exists(resolvedFilePath))
		{
			setError("skin file not found: " + resolvedFilePath.string());
			return false;
		}

		std::vector<uint8_t> fileBytes;
		if (!readBinaryFile(resolvedFilePath, fileBytes))
		{
			setError("could not read skin file: " + resolvedFilePath.string());
			return false;
		}

		if (fileBytes.size() > static_cast<size_t>(MaxSkinFileBytes))
		{
			setError("skin file is too large: " + resolvedFilePath.string());
			return false;
		}

		const std::string resolvedFileName = sanitizeFileName(resolvedFilePath.filename().string());
		if (textureName.empty())
			textureName = sanitizeToken(resolvedFilePath.stem().string());

		if (textureName.empty() || resolvedFileName.empty())
		{
			setError("invalid skin file name or derived texture name: " + resolvedFilePath.string());
			return false;
		}

		outSkin.enabled = true;
		outSkin.textureName = textureName;
		outSkin.filePath = resolvedFilePath.string();
		outSkin.fileName = resolvedFileName;
		outSkin.fileBytes = std::move(fileBytes);
		if (errorMessage)
			errorMessage->clear();
		return true;
	}
}
