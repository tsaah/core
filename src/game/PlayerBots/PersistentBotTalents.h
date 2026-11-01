#ifndef MANGOS_PERSISTENTBOTTALENTS_H
#define MANGOS_PERSISTENTBOTTALENTS_H

#include "Common.h"
#include <vector>

struct PersistentBotTalentState
{
    struct Target
    {
        uint32 talentId;
        uint8 maxRank;
        uint8 targetRank;
    };

    uint8 cachedTab = 255;
    // Primary tab talents first (row, column ascending), then the other tabs the same way.
    std::vector<Target> targets;
    // Primary tab only, row by row, used when nothing in targets is learnable.
    std::vector<Target> fallback;
};

#endif
