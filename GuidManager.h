/*
    GuidManager.h
    Manages NPC GUID loading from file and assignment to connected clients.
    Used exclusively by the server.
*/

#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

class GuidManager
{
public:
    /// Load GUIDs from a file. Each line should be in "XXXXXXXX_XXXXXXXX" hex format.
    /// Returns true if at least one GUID was loaded.
    bool loadFromFile(const std::string& filePath = "FAKE_BILBO_GUID.txt");

    /// Try to assign a GUID to the given client index.
    /// Returns the assigned GUID, or 0 if none are available.
    uint64_t assignGuid(int clientIndex);

    /// Release a GUID when a client disconnects, making it available again.
    void releaseGuid(int clientIndex);

    /// Get the GUID assigned to a client. Returns 0 if not assigned.
    uint64_t getGuid(int clientIndex) const;

    /// Check if any GUIDs are available for assignment.
    bool hasAvailableGuids() const { return !availableGuids_.empty(); }

    /// Get total number of loaded GUIDs.
    size_t totalGuids() const { return allGuids_.size(); }

private:
    std::vector<uint64_t>                allGuids_;
    std::vector<uint64_t>                availableGuids_;
    std::unordered_map<int, uint64_t>    clientGuidMap_;
};
