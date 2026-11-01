#ifndef MANGOS_PERSISTENTBOTFARM_H
#define MANGOS_PERSISTENTBOTFARM_H

#include "Common.h"
#include "ObjectGuid.h"
#include <ctime>
#include <map>
#include <set>

struct PersistentBotFarmState
{
    enum Phase : uint8
    {
        PHASE_GRIND = 0,
        PHASE_TO_VENDOR = 1,
        PHASE_VENDOR = 2,
        PHASE_RETURN = 3,
    };

    // Farm mode (anchored grinding, works without the leader)
    bool farming = false;
    uint32 anchorMap = 0;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    float anchorZ = 0.0f;
    float radius = 60.0f;
    uint8 phase = PHASE_GRIND;

    // AFK mode (grinding around an AFK leader), recomputed every tick
    bool afkGrinding = false;

    // Grinding
    time_t nextScan = 0;
    time_t lastTargetSeen = 0;
    bool idleNotified = false;
    bool roleOverridden = false;

    // Death
    time_t deathSince = 0;

    // Vendor trip
    ObjectGuid vendorGuid;
    float vendorX = 0.0f;
    float vendorY = 0.0f;
    float vendorZ = 0.0f;
    std::set<uint32> badVendors; // spawn guids that failed this session
    time_t nextVendorSearch = 0;
    time_t nextTripAllowed = 0;
    bool noVendorNotified = false;
    bool townBlocked = false; // needs a town trip but no vendor is reachable: stop grinding

    // Targets that failed pathing recently (expiry time)
    std::map<ObjectGuid, time_t> unreachable;
    uint32 returnFailures = 0;

    // Long-distance travel
    time_t travelStarted = 0;
    time_t travelLastMove = 0;
    float travelLastDist = 0.0f;
    uint32 travelStrikes = 0;
};

#endif
