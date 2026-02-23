/*
    GuidManager.cpp
    Implementation of GUID loading and client assignment.
*/

#include "GuidManager.h"
#include "shared.h"

bool GuidManager::loadFromFile(const std::string& filePath)
{
    allGuids_ = loadGuidsFromFile(filePath);
    availableGuids_ = allGuids_;

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
