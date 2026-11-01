#include "PersistentBotAI.h"
#include "PlayerBotMgr.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
constexpr float DEFAULT_FARM_RADIUS = 60.0f; // same default as .persistentbot farm

// Replies are sent as the bot: party lines are broadcast to the group, whisper lines go straight to the owner.
// Packets are built directly instead of queued as CMSG_MESSAGECHAT, so multi-line output never trips the chat spam limiter.
class PersistentBotChatHandler : public ChatHandler
{
    public:
        PersistentBotChatHandler(WorldSession* pOwnerSession, Player* pBot, bool whisper)
            : ChatHandler(pOwnerSession), m_bot(pBot), m_whisper(whisper) {}

        using ChatHandler::SendSysMessage;
        void SendSysMessage(char const* str) override
        {
            std::string const text(str);
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                SendLine(text.substr(start, end - start));
                start = end + 1;
            }
        }

        // the owner's selected creature: the trainer for learn, the beast for tame
        Creature* GetOwnerTarget() const { return GetSelectedCreature(); }

    private:
        void SendLine(std::string const& line)
        {
            WorldPacket data;
            if (m_whisper)
            {
                ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, line.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, m_bot->GetObjectGuid(), m_bot->GetName());
                GetSession()->SendPacket(&data);
                return;
            }

            Group* pGroup = m_bot->GetGroup();
            if (!pGroup)
                return;

            ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, line.c_str(), LANG_UNIVERSAL, m_bot->GetChatTag(), m_bot->GetObjectGuid(), m_bot->GetName());
            pGroup->BroadcastPacket(&data, false);
        }

        Player* m_bot;
        bool m_whisper;
};

std::vector<std::string> SplitChatArgs(std::string const& msg)
{
    std::vector<std::string> args;
    size_t pos = 0;
    while (pos < msg.size())
    {
        size_t const begin = msg.find_first_not_of(" \t", pos);
        if (begin == std::string::npos)
            break;
        size_t const end = msg.find_first_of(" \t", begin);
        args.push_back(msg.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        pos = end;
    }
    return args;
}

std::string ToLower(std::string str)
{
    for (char& c : str)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

bool ParseRadius(std::string const& arg, float& radius)
{
    char* end = nullptr;
    double const value = strtod(arg.c_str(), &end);
    if (end == arg.c_str() || *end != 0 || !std::isfinite(value))
        return false;
    radius = float(value);
    return true;
}

// Quest ids a chat line refers to: |Hquest:<id>...| hyperlinks by id, [Title] brackets by case-insensitive title match against the bot's log.
std::vector<uint32> FindQuestReferences(Player* pBot, std::string const& msg)
{
    std::vector<uint32> ids;
    auto const add = [&ids](uint32 id)
    {
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    };

    std::string const linkPrefix = "|Hquest:";
    for (size_t pos = msg.find(linkPrefix); pos != std::string::npos; pos = msg.find(linkPrefix, pos + 1))
    {
        char const* const digits = msg.c_str() + pos + linkPrefix.size();
        char* end = nullptr;
        unsigned long const id = strtoul(digits, &end, 10);
        if (end != digits)
            add(uint32(id));
    }

    for (size_t open = msg.find('['); open != std::string::npos; open = msg.find('[', open + 1))
    {
        size_t const close = msg.find(']', open + 1);
        if (close == std::string::npos)
            break;

        std::string const title = ToLower(msg.substr(open + 1, close - open - 1));
        for (auto const& itr : pBot->GetQuestStatusMap())
        {
            Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr.first);
            if (pQuest && ToLower(pQuest->GetTitle()) == title)
                add(itr.first);
        }
    }
    return ids;
}
}

void PersistentBotAI::OnChatFromPlayer(Player* pSender, uint32 chatType, std::string const& msg)
{
    // only the owner gives commands; other players are ignored, and bots never reach here since their replies are not chat requests
    if (!pSender || !me->IsInWorld() || pSender->GetObjectGuid() != m_leaderGuid)
        return;

    bool const whisper = chatType == CHAT_MSG_WHISPER;
    std::vector<std::string> args = SplitChatArgs(msg);
    std::string const cmd = args.empty() ? std::string() : ToLower(args[0]);

    // built on the owner's session so the owner's selected creature (learn, tame) resolves
    PersistentBotChatHandler handler(pSender->GetSession(), me, whisper);

    if (cmd == "status")
        HandleStatusCommand(&handler);
    else if (cmd == "spec")
    {
        if (args.size() == 2)
            HandleSpecCommand(&handler, args[1].c_str());
        else
            handler.SendSysMessage("Usage: spec <name|1|2|3|auto>");
    }
    else if (cmd == "resetspec")
        HandleResetSpecCommand(&handler);
    else if (cmd == "farm")
    {
        float radius = DEFAULT_FARM_RADIUS;
        if (args.size() == 1 || (args.size() == 2 && ParseRadius(args[1], radius)))
            HandleFarmCommand(&handler, radius);
        else
            handler.SendSysMessage("Usage: farm [radius 20-150]");
    }
    else if (cmd == "follow")
        HandleFollowCommand(&handler);
    else if (cmd == "learn")
    {
        if (Creature* pTrainer = handler.GetOwnerTarget())
            HandleLearnCommand(&handler, pTrainer);
        else
            handler.SendSysMessage("Select a trainer first.");
    }
    else if (cmd == "unlearn")
    {
        uint32 const index = args.size() > 1 ? uint32(atoi(args[1].c_str())) : 0;
        if (index >= 1 && index <= 2 && args.size() == 3 && args[2] == "confirm")
            HandleUnlearnCommand(&handler, index);
        else
            handler.SendSysMessage("Usage: unlearn <1|2> confirm");
    }
    else if (cmd == "tame")
    {
        if (Creature* pTarget = handler.GetOwnerTarget())
            HandleTameCommand(&handler, pTarget);
        else
            handler.SendSysMessage("Select a beast first.");
    }
    else if (cmd == "abandonpet")
        HandleAbandonPetCommand(&handler);
    else if (cmd == "quests")
    {
        uint32 count = 0;
        for (auto const& itr : me->GetQuestStatusMap())
        {
            std::string const line = GetQuestStatusLine(itr.first);
            if (line.empty())
                continue;
            handler.SendSysMessage(line.c_str());
            ++count;
        }
        if (!count)
            handler.SendSysMessage("I have no quests.");
    }
    else if (cmd == "getquests" || (cmd == "get" && args.size() == 2 && ToLower(args[1]) == "quests"))
    {
        if (Creature* pGiver = handler.GetOwnerTarget())
            HandleGetQuestsCommand(&handler, pGiver);
        else
            handler.SendSysMessage("Select a quest giver first.");
    }
    else if (cmd == "fish")
    {
        if (args.size() == 1)
            HandleFishCommand(&handler);
        else if (args.size() == 2 && ToLower(args[1]) == "stop")
            HandleFishStopCommand(&handler);
        else
            handler.SendSysMessage("Usage: fish [stop]");
    }
    else if (cmd == "remove")
    {
        Dismiss();
        handler.PSendSysMessage("Removing %s.", me->GetName());
    }
    else if (cmd == "help")
    {
        handler.SendSysMessage("Persistent bot commands (party: every bot answers, whisper: only this bot):");
        handler.SendSysMessage("status, spec <name|1|2|3|auto>, resetspec, farm [radius], follow, fish, fish stop");
        handler.SendSysMessage("learn, unlearn <1|2> confirm, tame, abandonpet, quests, get quests (select a quest giver), remove, help");
    }
    else
    {
        // quest references are not commands: every bot holding the quest reports its status, the rest stay silent
        std::vector<uint32> const questIds = FindQuestReferences(me, msg);
        if (!questIds.empty())
        {
            for (uint32 const questId : questIds)
            {
                std::string const line = GetQuestStatusLine(questId);
                if (!line.empty())
                    handler.SendSysMessage(line.c_str());
            }
        }
        else if (whisper)
            handler.SendSysMessage("Unknown command, say help.");
    }
}
