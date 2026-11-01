#include "PersistentBotAI.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Formulas.h"
#include "GridSearchers.h"
#include "Database/DatabaseEnv.h"
#include "World.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr float QUEST_BOT_INTERACT_RANGE = 30.0f;
constexpr time_t QUEST_DELIVERY_INTERVAL = 3;

class QuestEnderCreatureCheck
{
    public:
        QuestEnderCreatureCheck(Player const* pPlayer, uint32 questId) : m_player(pPlayer), m_questId(questId) {}
        bool operator()(Creature* pCreature)
        {
            return pCreature->IsAlive() && !pCreature->IsHostileTo(m_player) &&
                   pCreature->HasInvolvedQuest(m_questId) && m_player->IsWithinDist(pCreature, QUEST_BOT_INTERACT_RANGE, false);
        }
    private:
        Player const* m_player;
        uint32 m_questId;
};

class QuestEnderGameObjectCheck
{
    public:
        QuestEnderGameObjectCheck(Player const* pPlayer, uint32 questId) : m_player(pPlayer), m_questId(questId) {}
        bool operator()(GameObject* pGo)
        {
            return pGo->HasInvolvedQuest(m_questId) && m_player->IsWithinDist(pGo, QUEST_BOT_INTERACT_RANGE, false);
        }
    private:
        Player const* m_player;
        uint32 m_questId;
};

std::mutex s_questFilterLock;
std::unordered_map<uint32, bool> s_itemDropCache;
std::unordered_map<uint32, bool> s_refDropCache;
std::unordered_map<uint32, bool> s_triggerQuestCache;

// True if any creature_loot_template row references reference_loot_template entry refId, directly or via nested references.
// Results are cached per reference id; visited guards against reference cycles within one walk.
bool ReferenceDropsFromCreatures(uint32 refId, uint32 depth, std::unordered_set<uint32>& visited)
{
    auto const cached = s_refDropCache.find(refId);
    if (cached != s_refDropCache.end())
        return cached->second;
    if (depth >= 4 || !visited.insert(refId).second)
        return false;

    uint32 const patch = sWorld.GetWowPatch();
    bool drops = WorldDatabase.PQuery("SELECT 1 FROM creature_loot_template WHERE mincountOrRef = -%u AND %u >= patch_min AND %u <= patch_max LIMIT 1", refId, patch, patch) != nullptr;
    if (!drops)
    {
        std::unique_ptr<QueryResult> result = WorldDatabase.PQuery("SELECT DISTINCT entry FROM reference_loot_template WHERE mincountOrRef = -%u AND %u >= patch_min AND %u <= patch_max", refId, patch, patch);
        if (result)
        {
            do
            {
                if (ReferenceDropsFromCreatures(result->Fetch()[0].GetUInt32(), depth + 1, visited))
                {
                    drops = true;
                    break;
                }
            } while (result->NextRow());
        }
    }
    s_refDropCache[refId] = drops;
    return drops;
}

// Lazy per-item world DB lookup (the loot stores keep their templates private and are not iterable); cached for the process lifetime.
bool ItemDropsFromCreatures(uint32 itemId)
{
    auto const cached = s_itemDropCache.find(itemId);
    if (cached != s_itemDropCache.end())
        return cached->second;

    uint32 const patch = sWorld.GetWowPatch();
    // items forbidden for the current patch are never loaded into the loot stores (see LootMgr.cpp)
    bool drops = false;
    if (!WorldDatabase.PQuery("SELECT 1 FROM forbidden_items WHERE entry = %u AND ((after_or_before = 0 AND patch <= %u) OR (after_or_before = 1 AND patch >= %u)) LIMIT 1", itemId, patch, patch))
    {
        drops = WorldDatabase.PQuery("SELECT 1 FROM creature_loot_template WHERE item = %u AND mincountOrRef > 0 AND %u >= patch_min AND %u <= patch_max LIMIT 1", itemId, patch, patch) != nullptr;
        if (!drops)
        {
            std::unique_ptr<QueryResult> result = WorldDatabase.PQuery("SELECT DISTINCT entry FROM reference_loot_template WHERE item = %u AND mincountOrRef > 0 AND %u >= patch_min AND %u <= patch_max", itemId, patch, patch);
            if (result)
            {
                std::unordered_set<uint32> visited;
                do
                {
                    if (ReferenceDropsFromCreatures(result->Fetch()[0].GetUInt32(), 0, visited))
                    {
                        drops = true;
                        break;
                    }
                } while (result->NextRow());
            }
        }
    }
    s_itemDropCache[itemId] = drops;
    return drops;
}

bool HasAreaTrigger(uint32 questId)
{
    auto const cached = s_triggerQuestCache.find(questId);
    if (cached != s_triggerQuestCache.end())
        return cached->second;
    bool const found = WorldDatabase.PQuery("SELECT 1 FROM areatrigger_involvedrelation WHERE quest = %u LIMIT 1", questId) != nullptr;
    s_triggerQuestCache[questId] = found;
    return found;
}

// Bots can only kill and loot: reject quests needing gameobjects, spell casts, quest-item use, non-creature item sources or area exploration.
bool CanBotFinishQuest(Quest const* pQuest)
{
    std::lock_guard<std::mutex> guard(s_questFilterLock);

    if (pQuest->HasQuestFlag(QUEST_FLAGS_EXPLORATION) ||
        (pQuest->HasSpecialFlag(QUEST_SPECIAL_FLAG_EXPLORATION_OR_EVENT) && HasAreaTrigger(pQuest->GetQuestId())))
        return false;

    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        if (pQuest->ReqCreatureOrGOId[i] < 0 || pQuest->ReqSpell[i])
            return false;
    }

    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const itemId = pQuest->ReqItemId[i];
        if (itemId && itemId != pQuest->GetSrcItemId() && !ItemDropsFromCreatures(itemId))
            return false;
    }
    return true;
}

void SayToParty(Player* pBot, std::string const& text)
{
    Group* pGroup = pBot->GetGroup();
    if (!pGroup)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text.c_str(), LANG_UNIVERSAL, pBot->GetChatTag(), pBot->GetObjectGuid(), pBot->GetName());
    pGroup->BroadcastPacket(&data, false);
}

// Announces each completed quest once while the bot is grouped; quests that left the log or were turned in are forgotten so repeatables can announce again
void AnnounceCompletedQuests(Player* pBot, PersistentBotQuestState& state)
{
    if (!pBot->GetGroup())
        return;

    std::unordered_set<uint32> complete;
    for (auto const& itr : pBot->GetQuestStatusMap())
    {
        if (itr.second.m_status != QUEST_STATUS_COMPLETE || itr.second.m_rewarded)
            continue;

        complete.insert(itr.first);
        if (!state.completeAnnounced.insert(itr.first).second)
            continue;

        if (Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr.first))
            SayToParty(pBot, "Quest complete: " + pQuest->GetTitle() + " - ready to turn in.");
    }

    for (auto itr = state.completeAnnounced.begin(); itr != state.completeAnnounced.end();)
    {
        if (complete.count(*itr))
            ++itr;
        else
            itr = state.completeAnnounced.erase(itr);
    }
}
}

bool PersistentBotAI::UpdateQuestDelivery()
{
    time_t const now = time(nullptr);
    if (now < m_questState.nextDeliveryCheck)
        return false;
    m_questState.nextDeliveryCheck = now + QUEST_DELIVERY_INTERVAL;

    AnnounceCompletedQuests(me, m_questState);

    if (!me->IsAlive() || me->IsInCombat())
        return false;

    std::vector<uint32> completed;
    for (auto const& itr : me->GetQuestStatusMap())
    {
        if (itr.second.m_status == QUEST_STATUS_COMPLETE && !itr.second.m_rewarded)
            completed.push_back(itr.first);
    }

    for (uint32 const questId : completed)
    {

        Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);
        if (!pQuest)
            continue;

        WorldObject* pEnder = nullptr;

        std::list<Creature*> creatures;
        QuestEnderCreatureCheck creatureCheck(me, questId);
        MaNGOS::CreatureListSearcher<QuestEnderCreatureCheck> creatureSearcher(creatures, creatureCheck);
        Cell::VisitGridObjects(me, creatureSearcher, QUEST_BOT_INTERACT_RANGE);
        if (!creatures.empty())
            pEnder = creatures.front();
        else
        {
            std::list<GameObject*> objects;
            QuestEnderGameObjectCheck goCheck(me, questId);
            MaNGOS::GameObjectListSearcher<QuestEnderGameObjectCheck> goSearcher(objects, goCheck);
            Cell::VisitGridObjects(me, goSearcher, QUEST_BOT_INTERACT_RANGE);
            if (!objects.empty())
                pEnder = objects.front();
        }

        if (!pEnder)
            continue;

        uint32 choice = 0;
        if (pQuest->GetRewChoiceItemsCount() > 1)
        {
            float bestScore = 0.0f;
            uint32 bestPrice = 0;
            bool foundUpgrade = false;
            for (uint32 i = 0; i < pQuest->GetRewChoiceItemsCount(); ++i)
            {
                ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(pQuest->RewChoiceItemId[i]);
                if (!pProto)
                    continue;

                if (IsUpgrade(pProto))
                {
                    float const score = ScoreItem(pProto);
                    if (!foundUpgrade || score > bestScore)
                    {
                        foundUpgrade = true;
                        bestScore = score;
                        choice = i;
                    }
                }
                else if (!foundUpgrade && pProto->SellPrice > bestPrice)
                {
                    bestPrice = pProto->SellPrice;
                    choice = i;
                }
            }
        }

        if (!me->CanRewardQuest(pQuest, choice, false))
            continue;

        me->RewardQuest(pQuest, choice, pEnder, true);
        SayToParty(me, "Completed: " + pQuest->GetTitle());
    }

    return false;
}

void PersistentBotAI::OnGroupLeaderQuestAccepted(Quest const* pQuest, WorldObject* pGiver)
{
    if (!pQuest || !pGiver || !me->IsAlive() || !me->IsInWorld())
        return;

    if (!pGiver->IsInWorld() || pGiver->GetMap() != me->GetMap() || !me->IsWithinDist(pGiver, QUEST_BOT_INTERACT_RANGE, false))
        return;

    if (!pGiver->HasQuest(pQuest->GetQuestId()))
        return;

    // Party-accept quests are offered to the whole group: range is measured to the accepting player, not the giver.
    // The handler runs this before it hands out share info, and CanTakeQuest rejects an owned quest, so it is never added twice.
    bool const partyAccept = pQuest->HasQuestFlag(QUEST_FLAGS_PARTY_ACCEPT);
    if (partyAccept)
    {
        bool playerNear = false;
        if (Group* pGroup = me->GetGroup())
        {
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pMember = itr->getSource();
                if (pMember && pMember != me && !pMember->GetSession()->GetBot() && pMember->IsInMap(me) &&
                    me->IsWithinDist(pMember, QUEST_BOT_INTERACT_RANGE, false))
                {
                    playerNear = true;
                    break;
                }
            }
        }
        if (!playerNear)
            return;
    }

    TryTakeQuest(pQuest, partyAccept);
}

// Takes pQuest for the bot. Unless bypassFilter, unfinishable quests are skipped; a full log is the one refusal fixed by abandoning a quest.
bool PersistentBotAI::TryTakeQuest(Quest const* pQuest, bool bypassFilter)
{
    bool const logFull = !me->SatisfyQuestLog(false);
    if (!me->CanTakeQuest(pQuest, false) || (!logFull && !me->CanAddQuest(pQuest, false)))
        return false;

    // Escort/event quests (exploration-or-event flag without an area trigger) complete through group event credit.
    if (!bypassFilter && !CanBotFinishQuest(pQuest))
    {
        if (m_questState.skippedQuests.insert(pQuest->GetQuestId()).second)
            SayToParty(me, "Skipping " + pQuest->GetTitle() + " - I can't do that kind of quest.");
        return false;
    }

    if (logFull)
    {
        Quest const* pAbandoned = AbandonQuestForRoom();
        if (!pAbandoned)
        {
            if (m_questState.logFullAnnounced.insert(pQuest->GetQuestId()).second)
                SayToParty(me, "My quest log is full, can't take " + pQuest->GetTitle() + ".");
            return false;
        }
        SayToParty(me, "Abandoned " + pAbandoned->GetTitle() + " to make room for " + pQuest->GetTitle() + ".");
    }

    if (!me->CanAddQuest(pQuest, false))
        return false;

    me->AddQuest(pQuest, nullptr);

    if (me->CanCompleteQuest(pQuest->GetQuestId()))
        me->CompleteQuest(pQuest->GetQuestId());

    if (pQuest->GetSrcSpell() > 0)
        me->CastSpell(me, pQuest->GetSrcSpell(), true);

    SayToParty(me, "Accepted: " + pQuest->GetTitle());
    return true;
}

// Frees a log slot by abandoning one quest the owner does not have: failed first, then gray quests, then lowest level. Complete quests are never abandoned.
// Returns the abandoned quest, or nullptr when the owner is offline or no candidate could be dropped.
Quest const* PersistentBotAI::AbandonQuestForRoom()
{
    Player* pOwner = GetPartyLeader();
    if (!pOwner)
        return nullptr;

    struct Candidate
    {
        Quest const* pQuest;
        bool failed;
        bool gray;
        uint32 level;
    };

    uint32 const grayLevel = MaNGOS::XP::GetGrayLevel(me->GetLevel());
    std::vector<Candidate> candidates;
    for (auto const& itr : me->GetQuestStatusMap())
    {
        if (itr.second.m_status != QUEST_STATUS_INCOMPLETE && itr.second.m_status != QUEST_STATUS_FAILED)
            continue;

        Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr.first);
        if (!pQuest || pOwner->IsCurrentQuest(itr.first) || pOwner->GetQuestStatus(itr.first) == QUEST_STATUS_FAILED)
            continue;

        uint32 const level = me->GetQuestLevelForPlayer(pQuest);
        candidates.push_back({ pQuest, itr.second.m_status == QUEST_STATUS_FAILED, level <= grayLevel, level });
    }

    std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
    {
        if (a.failed != b.failed)
            return a.failed;
        if (a.gray != b.gray)
            return a.gray;
        if (a.level != b.level)
            return a.level < b.level;
        return a.pQuest->GetQuestId() < b.pQuest->GetQuestId();
    });

    for (Candidate const& candidate : candidates)
    {
        // same work as WorldSession::HandleQuestLogRemoveQuest: RemoveQuestAtSlot (source items, timed quest, status and slot reset) plus the abandon criterion
        me->RemoveQuest(candidate.pQuest->GetQuestId());
#ifdef USE_ACHIEVEMENTS
        me->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_QUEST_ABANDONED, 1);
#endif
        if (me->SatisfyQuestLog(false))
            return candidate.pQuest;
    }
    return nullptr;
}

// "get quests": every bot in range takes each quest the owner's selected quest giver offers, under the same rules as an owner-accepted quest.
void PersistentBotAI::HandleGetQuestsCommand(ChatHandler* handler, Creature* pGiver)
{
    QuestRelationsMapBounds const bounds = sObjectMgr.GetCreatureQuestRelationsMapBounds(pGiver->GetEntry());
    if (bounds.first == bounds.second)
    {
        handler->SendSysMessage("Select a quest giver first.");
        return;
    }

    if (!me->IsAlive() || !me->IsInWorld())
        return;

    if (pGiver->GetMap() != me->GetMap() || !me->IsWithinDist(pGiver, QUEST_BOT_INTERACT_RANGE, false))
    {
        handler->PSendSysMessage("I'm too far from %s.", pGiver->GetName());
        return;
    }

    uint32 taken = 0;
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr->second);
        if (pQuest && TryTakeQuest(pQuest, false))
            ++taken;
    }

    if (taken)
        handler->PSendSysMessage("Took %u quests from %s.", taken, pGiver->GetName());
    else
        handler->PSendSysMessage("No quests I can take from %s.", pGiver->GetName());
}

// One line per quest in the bot's log; empty when the quest is not in the log, so the bot stays silent.
std::string PersistentBotAI::GetQuestStatusLine(uint32 questId) const
{
    QuestStatus const status = me->GetQuestStatus(questId);
    if (status != QUEST_STATUS_COMPLETE && status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_FAILED)
        return std::string();

    Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);
    QuestStatusData const* pData = me->GetQuestStatusData(questId);
    if (!pQuest || !pData || (status == QUEST_STATUS_COMPLETE && pData->m_rewarded))
        return std::string();

    std::string const& title = pQuest->GetTitle();
    if (status == QUEST_STATUS_COMPLETE)
        return title + ": complete - ready to turn in.";
    if (status == QUEST_STATUS_FAILED)
        return title + ": failed.";

    std::string line = title + ":";
    bool first = true;
    auto addObjective = [&](std::string const& name, uint32 have, uint32 need)
    {
        if (!need)
            return;
        line += first ? " " : ", ";
        first = false;
        line += (name.empty() ? std::string("objective") : name) + " " + std::to_string(std::min(have, need)) + "/" + std::to_string(need);
    };

    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const entry = pQuest->ReqCreatureOrGOId[i];
        if (!entry)
            continue;

        std::string name;
        if (entry > 0)
        {
            if (CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(uint32(entry)))
                name = pInfo->name;
        }
        else if (GameObjectInfo const* pInfo = sObjectMgr.GetGameObjectTemplate(uint32(-entry)))
            name = pInfo->name;

        addObjective(name, pData->m_creatureOrGOcount[i], pQuest->ReqCreatureOrGOCount[i]);
    }

    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const itemId = pQuest->ReqItemId[i];
        if (!itemId)
            continue;

        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        std::string const name = pProto && pProto->Name1 ? std::string(pProto->Name1) : std::string();
        addObjective(name, pData->m_itemcount[i], pQuest->ReqItemCount[i]);
    }

    if (first)
        return title + ": in progress.";
    return line;
}
