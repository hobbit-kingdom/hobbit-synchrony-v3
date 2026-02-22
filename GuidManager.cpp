/*
    GuidManager.cpp
    Implementation of GUID loading and client assignment.
*/

#include "GuidManager.h"
#include <fstream>
#include <iostream>
#include <filesystem>

bool GuidManager::loadFromFile(const std::string& filePath)
{
    std::ifstream file;
    std::string   currentPath = filePath;
    bool          foundOnFirstTry = true;

    // Keep prompting until the file is opened or the user quits
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
        std::getline(std::cin, input);
        if (input == "q" || input == "Q")
            return false;

        currentPath = input;
    }

    // Parse each line: format is "XXXXXXXX_XXXXXXXX" (hex)
    std::string line;
    while (std::getline(file, line))
    {
        size_t underscorePos = line.find('_');
        if (underscorePos == std::string::npos)
            continue;

        // "AABBCCDD_EEFFGGHH" → combined = "AABBCCDDEEFFGGHH"
        std::string part1 = line.substr(0, underscorePos);
        std::string part2 = line.substr(underscorePos + 1);
        std::string combined = part1 + part2;

        uint64_t guid = std::stoull(combined, nullptr, 16);
        allGuids_.push_back(guid);
    }

    // All loaded GUIDs are initially available
    availableGuids_ = allGuids_;

    if (foundOnFirstTry)
        printf("GUID file loaded successfully (%zu GUIDs).\n", allGuids_.size());

    return !allGuids_.empty();
}

uint64_t GuidManager::assignGuid(int clientIndex)
{
    // Already assigned?
    auto it = clientGuidMap_.find(clientIndex);
    if (it != clientGuidMap_.end())
        return it->second;

    if (availableGuids_.empty())
        return 0;

    uint64_t guid = availableGuids_.back();
    availableGuids_.pop_back();
    clientGuidMap_[clientIndex] = guid;

    printf("Assigned GUID %llu to client %d\n", guid, clientIndex);
    return guid;
}

void GuidManager::releaseGuid(int clientIndex)
{
    auto it = clientGuidMap_.find(clientIndex);
    if (it == clientGuidMap_.end())
        return;

    availableGuids_.push_back(it->second);
    clientGuidMap_.erase(it);
    printf("Released GUID for client %d\n", clientIndex);
}

uint64_t GuidManager::getGuid(int clientIndex) const
{
    auto it = clientGuidMap_.find(clientIndex);
    return (it != clientGuidMap_.end()) ? it->second : 0;
}
