#ifndef MANGOS_PERSISTENTBOTGATHER_H
#define MANGOS_PERSISTENTBOTGATHER_H

#include "Common.h"
#include "ObjectGuid.h"
#include <ctime>
#include <map>
#include <set>
#include <vector>

class Player;
struct ItemPrototype;

// Auto gathering (herbalism, mining, skinning) and explicit fishing; the logic is in PersistentBotGather.cpp
struct PersistentBotGatherState
{
    enum Phase : uint8
    {
        PHASE_IDLE = 0,
        PHASE_WALK = 1,       // walking to a gather target, or to the owner before fishing
        PHASE_CAST = 2,       // gather spell in progress, loot is taken when it ends
        PHASE_FISH_CAST = 3,  // a fishing cast is to be started
        PHASE_FISH_WAIT = 4,  // bobber in the water, waiting for the bite
    };

    uint8 phase = PHASE_IDLE;

    // auto gather target: a gathering node (GameObject) or a dead creature to skin
    ObjectGuid target;
    bool targetCorpse = false;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    char const* gatherVerb = nullptr; // "Gathered", "Mined" or "Skinned" for the party line
    std::map<ObjectGuid, time_t> skipped; // targets that failed recently (expiry time)
    // diagnostics only: last time a rejection reason was logged per node, and the last idle reason logged for the bot
    std::map<ObjectGuid, time_t> rejectLogged;
    char const* idleReason = nullptr;
    time_t idleLogged = 0;
    time_t nextScan = 0;
    time_t nextSafeCheck = 0;
    time_t walkStarted = 0;
    time_t castStarted = 0;
    uint8 attempts = 0;         // gather casts on the current target; reset when a target is chosen
    time_t moveIssued = 0;
    float moveDist = 0.0f;
    uint32 moveStrikes = 0;

    // profession tools already asked for in party this session
    std::set<uint32> toolsNagged;

    // explicit fishing (chat 'fish'), never started automatically
    bool fishing = false;
    // the fishing pole is two-handed: the main hand and off-hand items it displaced go back on fishing end
    ObjectGuid savedMainHand;
    ObjectGuid savedOffHand;
    bool bobberSeen = false;    // the current cast put a bobber in the water
    uint32 fishFailures = 0;    // casts that never reached water (not fishable) in a row
};

// A profession tool the bot knows the profession for but does not carry (fishing: any fishing pole counts).
struct PersistentBotToolNeed
{
    uint32 itemId;
    uint32 skillId;
};

std::vector<PersistentBotToolNeed> GetMissingTools(Player const* pPlayer);

// Mining Pick, Skinning Knife, Blacksmith's Hammer, Arclight Spanner and every fishing pole: never sold, junked or equipped by upgrade logic.
bool IsGatherTool(ItemPrototype const* pProto);

#endif
