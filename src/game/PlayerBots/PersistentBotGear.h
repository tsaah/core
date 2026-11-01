#ifndef MANGOS_PERSISTENTBOTGEAR_H
#define MANGOS_PERSISTENTBOTGEAR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <map>
#include <vector>

struct PersistentBotPendingRoll
{
    ObjectGuid lootedTarget;
    uint32 itemSlot = 0;
    uint32 itemId = 0;
    time_t since = 0;
};

struct PersistentBotGearState
{
    std::vector<PersistentBotPendingRoll> pendingRolls;
    ObjectGuid lootTarget;
    time_t lootStarted = 0;
    time_t lootMoveIssued = 0;
    float lootMoveDist = 0.0f;
    std::map<ObjectGuid, time_t> lootDone;
    // when each corpse was first seen lootable; starts its player grace period
    std::map<ObjectGuid, time_t> lootSeen;
    // bag warnings are said once per crossing; re-armed only once free slots rise back above the threshold
    bool bagsAlmostFullSaid = false;
    bool bagsFullSaid = false;
};

#endif
