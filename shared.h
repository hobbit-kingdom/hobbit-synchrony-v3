/*
    shared.h
    Convenience header — includes all game networking headers at once.

    Individual headers:
      - GameTypes.h       : Vector3, NetDefaults, guidFromString()
      - MathUtils.h       : lerp, lerpAngle, clamp
      - NetworkMessages.h : PositionMessage, GuidAssignMessage
      - NetworkConfig.h   : Protocol config, message factory, adapter
*/

#ifndef SHARED_H
#define SHARED_H

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>

#include "GameTypes.h"
#include "MathUtils.h"
#include "NetworkMessages.h"
#include "NetworkConfig.h"

/// Shared utility to load GUIDs from a file.
/// If the file is not found, it prompts the user until a valid path is provided or they quit.
inline std::vector<uint64_t> loadGuidsFromFile(const std::string& filePath)
{
    std::vector<uint64_t> guids;
    std::ifstream file;
    std::string   currentPath = filePath;
    bool          foundOnFirstTry = true;

    while (!file.is_open())
    {
        file.open(currentPath);
        if (file.is_open())
            break;

        foundOnFirstTry = false;
        auto fullPath = std::filesystem::absolute(currentPath);
        printf("File not found at: %s\n", fullPath.string().c_str());
        printf("Enter the path to FAKE_BILBO_GUID.txt (or 'q' to quit): ");

        std::string input;
        if (!std::getline(std::cin, input) || input == "q" || input == "Q")
            return guids;

        currentPath = input;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.find('_') == std::string::npos)
            continue;

        guids.push_back(guidFromString(line));
    }

    if (foundOnFirstTry)
        printf("GUID file loaded successfully (%zu GUIDs).\n", guids.size());

    return guids;
}

#endif // SHARED_H
