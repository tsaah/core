#include "Common.h"
#include <cmath>
#include "Chat.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "AccountMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "Utilities/Random.h"
#include "PlayerBotMgr.h"
#include "PersistentBotAI.h"

namespace
{
    std::vector<std::string> SplitArgs(char* args)
    {
        std::vector<std::string> tokens;
        if (!args)
            return tokens;

        char* token = strtok(args, " \t");
        while (token)
        {
            tokens.push_back(token);
            token = strtok(nullptr, " \t");
        }
        return tokens;
    }

    PersistentBotAI* GetPersistentBotAI(Player* pBot, Player const* pOwner)
    {
        if (!pBot || !pBot->IsInWorld())
            return nullptr;

        PersistentBotAI* pAI = dynamic_cast<PersistentBotAI*>(pBot->AI());
        if (!pAI || pAI->m_leaderGuid != pOwner->GetObjectGuid())
            return nullptr;
        return pAI;
    }

    // All online persistent bots of the owner (bots in farm mode are not necessarily in the owner's group).
    std::vector<PersistentBotAI*> FindOwnedPersistentBots(Player const* pOwner)
    {
        std::vector<PersistentBotAI*> bots;
        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
        {
            if (PersistentBotAI* pAI = GetPersistentBotAI(itr.second, pOwner))
                bots.push_back(pAI);
        }
        return bots;
    }
}

PersistentBotAI* ChatHandler::ResolvePersistentBot(std::string const& nameArg)
{
    if (!PersistentBotAI::IsDatabaseReady())
    {
        SendSysMessage("Persistent bots are disabled: the characters database has no `persistent_bot` table. Apply sql/persistentbot.sql to it.");
        return nullptr;
    }

    Player* pOwner = m_session->GetPlayer();
    if (!pOwner)
        return nullptr;

    if (!nameArg.empty())
    {
        std::string name = nameArg;
        if (!normalizePlayerName(name))
        {
            SendSysMessage(LANG_PLAYER_NOT_FOUND);
            SetSentErrorMessage(true);
            return nullptr;
        }

        PersistentBotAI* pAI = GetPersistentBotAI(sObjectAccessor.FindPlayerByName(name.c_str()), pOwner);
        if (!pAI)
        {
            PSendSysMessage("%s is not one of your persistent bots.", name.c_str());
            SetSentErrorMessage(true);
        }
        return pAI;
    }

    if (Player* pSelected = GetSelectedPlayer())
    {
        if (PersistentBotAI* pAI = GetPersistentBotAI(pSelected, pOwner))
            return pAI;
    }

    PersistentBotAI* pFound = nullptr;
    for (PersistentBotAI* pAI : FindOwnedPersistentBots(pOwner))
    {
        if (pFound)
        {
            SendSysMessage("You have several persistent bots, specify the bot name.");
            SetSentErrorMessage(true);
            return nullptr;
        }
        pFound = pAI;
    }

    if (!pFound)
    {
        SendSysMessage("No persistent bot found, select one or specify the bot name.");
        SetSentErrorMessage(true);
    }
    return pFound;
}

bool ChatHandler::HandlePersistentBotAddCommand(char* args)
{
    if (!PersistentBotAI::IsDatabaseReady())
    {
        SendSysMessage("Persistent bots are disabled: the characters database has no `persistent_bot` table. Apply sql/persistentbot.sql to it.");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    std::vector<std::string> tokens = SplitArgs(args);
    if (tokens.size() != 1)
    {
        SendSysMessage("Usage: .persistentbot add <character>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string charName = tokens[0];
    if (!normalizePlayerName(charName))
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    ObjectGuid const guid = sObjectMgr.GetPlayerGuidByName(charName);
    if (guid.IsEmpty())
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (char const* error = sPlayerBotMgr.SpawnPersistentBot(pPlayer, guid))
    {
        SendSysMessage(error);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Loading %s as persistent bot.", charName.c_str());
    return true;
}

bool ChatHandler::HandlePersistentBotRemoveCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    pAI->Dismiss();
    PSendSysMessage("Removing %s.", pAI->me->GetName());
    return true;
}

bool ChatHandler::HandlePersistentBotListCommand(char* /*args*/)
{
    Player* pOwner = m_session->GetPlayer();
    if (!pOwner)
        return false;

    uint32 count = 0;
    for (PersistentBotAI* pAI : FindOwnedPersistentBots(pOwner))
    {
        PSendSysMessage("%s - level %u %s%s", pAI->me->GetName(), pAI->me->GetLevel(), pAI->GetSpecName(), pAI->m_farmState.farming ? " (farming)" : "");
        ++count;
    }

    if (!count)
        SendSysMessage("You have no persistent bots.");
    return true;
}

bool ChatHandler::HandlePersistentBotStatusCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    pAI->HandleStatusCommand(this);
    return true;
}

bool ChatHandler::HandlePersistentBotLearnCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    Creature* pTrainer = GetSelectedCreature();
    if (!pTrainer)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    pAI->HandleLearnCommand(this, pTrainer);
    return true;
}

bool ChatHandler::HandlePersistentBotUnlearnCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    uint32 const index = tokens.empty() ? 0 : uint32(atoi(tokens[0].c_str()));
    if (index < 1 || index > 2 || tokens.size() < 2 || tokens[1] != "confirm")
    {
        SendSysMessage("Usage: .persistentbot unlearn <1|2> confirm [name]");
        SetSentErrorMessage(true);
        return false;
    }

    PersistentBotAI* pAI = ResolvePersistentBot(tokens.size() > 2 ? tokens[2] : std::string());
    if (!pAI)
        return false;

    pAI->HandleUnlearnCommand(this, index);
    return true;
}

bool ChatHandler::HandlePersistentBotTameCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    Creature* pTarget = GetSelectedCreature();
    if (!pTarget)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    pAI->HandleTameCommand(this, pTarget);
    return true;
}

bool ChatHandler::HandlePersistentBotAbandonPetCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    pAI->HandleAbandonPetCommand(this);
    return true;
}

bool ChatHandler::HandlePersistentBotSpecCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    if (tokens.empty())
    {
        SendSysMessage("Usage: .persistentbot spec <name|1|2|3|auto> [name]");
        SetSentErrorMessage(true);
        return false;
    }

    PersistentBotAI* pAI = ResolvePersistentBot(tokens.size() > 1 ? tokens[1] : std::string());
    if (!pAI)
        return false;

    pAI->HandleSpecCommand(this, tokens[0].c_str());
    return true;
}

bool ChatHandler::HandlePersistentBotResetSpecCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    pAI->HandleResetSpecCommand(this);
    return true;
}

bool ChatHandler::HandlePersistentBotFarmCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);

    float radius = 60.0f;
    size_t nameIdx = 0;
    if (!tokens.empty())
    {
        char* end = nullptr;
        double const value = strtod(tokens[0].c_str(), &end);
        if (end != tokens[0].c_str() && *end == 0 && std::isfinite(value))
        {
            radius = float(value);
            nameIdx = 1;
        }
    }

    if (tokens.size() > nameIdx + 1)
    {
        SendSysMessage("Usage: .persistentbot farm [radius 20-150] [name]");
        SetSentErrorMessage(true);
        return false;
    }

    PersistentBotAI* pAI = ResolvePersistentBot(tokens.size() > nameIdx ? tokens[nameIdx] : std::string());
    if (!pAI)
        return false;

    if (!pAI->HandleFarmCommand(this, radius))
    {
        SetSentErrorMessage(true);
        return false;
    }
    return true;
}

bool ChatHandler::HandlePersistentBotFollowCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    PersistentBotAI* pAI = ResolvePersistentBot(tokens.empty() ? std::string() : tokens[0]);
    if (!pAI)
        return false;

    pAI->HandleFollowCommand(this);
    return true;
}

bool ChatHandler::HandlePersistentBotFishCommand(char* args)
{
    std::vector<std::string> tokens = SplitArgs(args);
    bool const stop = !tokens.empty() && tokens[0] == "stop";
    size_t const nameIdx = stop ? 1 : 0;
    if (tokens.size() > nameIdx + 1)
    {
        SendSysMessage("Usage: .persistentbot fish [stop] [name]");
        SetSentErrorMessage(true);
        return false;
    }

    PersistentBotAI* pAI = ResolvePersistentBot(tokens.size() > nameIdx ? tokens[nameIdx] : std::string());
    if (!pAI)
        return false;

    if (stop)
        pAI->HandleFishStopCommand(this);
    else
        pAI->HandleFishCommand(this);
    return true;
}
