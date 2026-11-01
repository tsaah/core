#ifndef MANGOS_PERSISTENTBOTQUESTS_H
#define MANGOS_PERSISTENTBOTQUESTS_H

#include "Common.h"
#include <ctime>
#include <unordered_set>

struct PersistentBotQuestState
{
    time_t nextDeliveryCheck = 0;
    std::unordered_set<uint32> skippedQuests; // already announced in party chat
    std::unordered_set<uint32> completeAnnounced; // announced as ready to turn in; cleared once the quest is no longer complete
    std::unordered_set<uint32> logFullAnnounced; // announced as not takeable because the log is full and nothing could be abandoned
};

#endif
