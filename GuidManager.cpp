/*
	GuidManager.cpp
	Implementation of GUID loading and client assignment.
*/

#include <algorithm>

#include "GuidManager.h"
#include "shared.h"

bool GuidManager::loadFromFile(const std::string& filePath)
{
	allGuids_ = loadGuidsFromFile(filePath);
	availableGuids_ = allGuids_;
	std::reverse(availableGuids_.begin(), availableGuids_.end());

	return !allGuids_.empty();
}

uint64_t GuidManager::assignGuid(int clientIndex)
{
	// Already assigned?
	auto it = clientGuidMap_.find(clientIndex);
	if (it != clientGuidMap_.end())
		return it->second.guid;

	if (availableGuids_.empty())
		return 0;

	uint64_t guid = availableGuids_.back();
	availableGuids_.pop_back();
	clientGuidMap_.emplace(clientIndex, ClientData(guid));

	printf("Assigned GUID %llu to client %d\n", guid, clientIndex);
	return guid;
}

void GuidManager::releaseGuid(int clientIndex)
{
	auto it = clientGuidMap_.find(clientIndex);
	if (it == clientGuidMap_.end())
		return;

	availableGuids_.push_back(it->second.guid);
	clientGuidMap_.erase(it);
	printf("Released GUID for client %d\n", clientIndex);
}

uint64_t GuidManager::getGuid(int clientIndex) const
{
	auto it = clientGuidMap_.find(clientIndex);
	return (it != clientGuidMap_.end()) ? it->second.guid : 0;
}

int GuidManager::getGuidSlot(uint64_t guid) const
{
	return SkinSync::getGuidSlotIndex(allGuids_, guid);
}

std::string GuidManager::getSkinFileName(uint64_t guid) const
{
	return SkinSync::getGuidBoundSkinFileName(allGuids_, guid);
}

const std::string& GuidManager::getNickname(int clientIndex) const
{
	auto it = clientGuidMap_.find(clientIndex);
	if (it == clientGuidMap_.end())
		return "Username";
		
	return it->second.nickname;
}

void GuidManager::setNickname(int clientIndex, const std::string &newName)
{
	auto it = clientGuidMap_.find(clientIndex);
	if (it == clientGuidMap_.end())
		return;
		
	it->second.nickname = newName;
}

const std::string& GuidManager::getStatus(int clientIndex) const
{
	auto it = clientGuidMap_.find(clientIndex);
	if (it == clientGuidMap_.end())
		return "Status";
		
	return it->second.status;
}

void GuidManager::setStatus(int clientIndex, const std::string &newStatus)
{
	auto it = clientGuidMap_.find(clientIndex);
	if (it == clientGuidMap_.end())
		return;
		
	it->second.status = newStatus;
}
