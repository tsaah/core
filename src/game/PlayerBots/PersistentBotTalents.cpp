#include "PersistentBotAI.h"
#include "PersistentBotTalentData.h"
#include "Chat.h"
#include "Player.h"
#include "DBCStores.h"
#include <algorithm>
#include <cctype>

namespace
{
    // TalentTab ids of the class in link order. Mage Arcane(81) and Fire(41) share an order index: Arcane first.
    std::vector<uint32> GetClassTalentTabs(uint8 classId)
    {
        std::vector<std::pair<uint32, uint32>> tabs; // (tabpage, id)
        for (uint32 id = 0; id < sTalentTabStore.GetNumRows(); ++id)
        {
            TalentTabEntry const* pTab = sTalentTabStore.LookupEntry(id);
            if (pTab && (pTab->ClassMask & (1u << (classId - 1))))
                tabs.push_back({ pTab->tabpage, id });
        }
        std::sort(tabs.begin(), tabs.end(), [classId](std::pair<uint32, uint32> const& a, std::pair<uint32, uint32> const& b)
        {
            if (a.first != b.first)
                return a.first < b.first;
            return classId == CLASS_MAGE ? a.second > b.second : a.second < b.second;
        });

        std::vector<uint32> result;
        for (auto const& itr : tabs)
            result.push_back(itr.second);
        return result;
    }

    // Talents of a tab ordered by (row, column).
    std::vector<TalentEntry const*> GetTabTalents(uint32 tabId)
    {
        std::vector<TalentEntry const*> talents;
        for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
        {
            TalentEntry const* pTalent = sTalentStore.LookupEntry(id);
            if (pTalent && pTalent->TalentTab == tabId)
                talents.push_back(pTalent);
        }
        std::sort(talents.begin(), talents.end(), [](TalentEntry const* a, TalentEntry const* b)
        {
            return a->Row != b->Row ? a->Row < b->Row : a->Col < b->Col;
        });
        return talents;
    }

    uint8 GetMaxRank(TalentEntry const* pTalent)
    {
        uint8 ranks = 0;
        while (ranks < MAX_TALENT_RANK && pTalent->RankID[ranks])
            ++ranks;
        return ranks;
    }

    uint8 GetCurrentRank(Player const* pPlayer, TalentEntry const* pTalent)
    {
        for (int32 k = MAX_TALENT_RANK - 1; k >= 0; --k)
            if (pTalent->RankID[k] && pPlayer->HasSpell(pTalent->RankID[k]))
                return k + 1;
        return 0;
    }

    uint32 GetSpentPoints(Player const* pPlayer, uint32 tabId)
    {
        uint32 spent = 0;
        for (TalentEntry const* pTalent : GetTabTalents(tabId))
            spent += GetCurrentRank(pPlayer, pTalent);
        return spent;
    }

    PersistentBotTalentBuild const* FindBuild(uint8 classId, uint8 tab)
    {
        for (PersistentBotTalentBuild const& build : g_persistentBotTalentBuilds)
            if (build.classId == classId && build.tab == tab)
                return &build;
        return nullptr;
    }

    // Digits of the tab-th '-' separated segment of the link.
    std::string GetLinkSegment(char const* link, uint8 tab)
    {
        std::string segment;
        uint8 current = 0;
        for (; *link; ++link)
        {
            if (*link == '-')
                ++current;
            else if (current == tab)
                segment += *link;
        }
        return segment;
    }

    void AppendTab(PersistentBotTalentState& state, uint32 tabId, std::string const& digits, bool primary)
    {
        std::vector<TalentEntry const*> talents = GetTabTalents(tabId);
        for (size_t i = 0; i < talents.size(); ++i)
        {
            uint8 maxRank = GetMaxRank(talents[i]);
            uint8 target = (i < digits.size() && digits[i] >= '0' && digits[i] <= '9') ? uint8(digits[i] - '0') : 0;
            target = std::min(target, maxRank);
            state.targets.push_back({ talents[i]->TalentID, maxRank, target });
            if (primary)
                state.fallback.push_back({ talents[i]->TalentID, maxRank, maxRank });
        }
    }

    bool TryLearnNext(Player* pPlayer, std::vector<PersistentBotTalentState::Target> const& list)
    {
        for (auto const& target : list)
        {
            TalentEntry const* pTalent = sTalentStore.LookupEntry(target.talentId);
            if (!pTalent)
                continue;
            uint8 current = GetCurrentRank(pPlayer, pTalent);
            if (current < target.targetRank && pPlayer->LearnTalent(target.talentId, current))
                return true;
        }
        return false;
    }

    bool EqualsNoCase(char const* a, char const* b)
    {
        for (; *a && *b; ++a, ++b)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
        return *a == *b;
    }
}

void PersistentBotAI::InitTalentsOnSpawn()
{
    if (m_autoTalents)
        return;

    for (uint32 tabId : GetClassTalentTabs(me->GetClass()))
    {
        if (GetSpentPoints(me, tabId))
        {
            m_autoTalents = true;
            SaveSettings();
            return;
        }
    }
}

void PersistentBotAI::UpdateTalents()
{
    if (!m_autoTalents || !me->GetFreeTalentPoints())
        return;

    uint8 spec = DetectSpecTab();
    if (m_talentState.cachedTab != spec || m_talentState.targets.empty())
    {
        m_talentState.targets.clear();
        m_talentState.fallback.clear();
        m_talentState.cachedTab = spec;

        std::vector<uint32> tabs = GetClassTalentTabs(me->GetClass());
        if (spec >= tabs.size())
            return;

        PersistentBotTalentBuild const* pBuild = FindBuild(me->GetClass(), spec);
        if (!pBuild)
            return;

        AppendTab(m_talentState, tabs[spec], GetLinkSegment(pBuild->link, spec), true);
        for (uint8 tab = 0; tab < tabs.size(); ++tab)
            if (tab != spec)
                AppendTab(m_talentState, tabs[tab], GetLinkSegment(pBuild->link, tab), false);
    }

    while (me->GetFreeTalentPoints())
    {
        if (TryLearnNext(me, m_talentState.targets))
            continue;
        // Character diverged from the build: never leave points stuck.
        if (TryLearnNext(me, m_talentState.fallback))
            continue;
        break;
    }
}

uint8 PersistentBotAI::DetectSpecTab() const
{
    if (m_specTab != 255)
        return m_specTab;

    std::vector<uint32> tabs = GetClassTalentTabs(me->GetClass());
    uint8 best = 255;
    uint32 bestSpent = 0;
    for (uint8 tab = 0; tab < tabs.size(); ++tab)
    {
        uint32 spent = GetSpentPoints(me, tabs[tab]);
        if (spent > bestSpent)
        {
            bestSpent = spent;
            best = tab;
        }
    }
    if (best != 255)
        return best;

    for (PersistentBotTalentBuild const& build : g_persistentBotTalentBuilds)
        if (build.classId == me->GetClass() && build.isDefault)
            return build.tab;
    return 0;
}

char const* PersistentBotAI::GetSpecName() const
{
    if (PersistentBotTalentBuild const* pBuild = FindBuild(me->GetClass(), DetectSpecTab()))
        return pBuild->name;
    return "unknown";
}

void PersistentBotAI::HandleSpecCommand(ChatHandler* handler, char const* specArg)
{
    uint8 tab = 255;
    bool valid = false;
    if (EqualsNoCase(specArg, "auto"))
        valid = true;
    else if (specArg[0] >= '1' && specArg[0] <= '3' && !specArg[1])
    {
        tab = specArg[0] - '1';
        valid = FindBuild(me->GetClass(), tab) != nullptr;
    }
    else
    {
        for (PersistentBotTalentBuild const& build : g_persistentBotTalentBuilds)
        {
            if (build.classId == me->GetClass() && EqualsNoCase(specArg, build.name))
            {
                tab = build.tab;
                valid = true;
                break;
            }
        }
    }

    if (!valid)
    {
        std::string names;
        for (PersistentBotTalentBuild const& build : g_persistentBotTalentBuilds)
        {
            if (build.classId != me->GetClass())
                continue;
            if (!names.empty())
                names += ", ";
            names += build.name;
        }
        handler->PSendSysMessage("Unknown spec '%s'. Valid: %s, 1|2|3, auto.", specArg, names.c_str());
        return;
    }

    m_specTab = tab;
    m_autoTalents = true;
    m_talentState.targets.clear();
    SaveSettings();

    if (tab == 255)
        handler->PSendSysMessage("%s: spec set to auto (%s).", me->GetName(), GetSpecName());
    else
        handler->PSendSysMessage("%s: spec set to %s.", me->GetName(), GetSpecName());

    std::vector<uint32> tabs = GetClassTalentTabs(me->GetClass());
    uint8 chosen = DetectSpecTab();
    for (uint8 other = 0; other < tabs.size(); ++other)
    {
        if (other != chosen && GetSpentPoints(me, tabs[other]))
        {
            handler->SendSysMessage("Points are already spent in another tab; use .persistentbot resetspec to respec.");
            break;
        }
    }
}

void PersistentBotAI::HandleResetSpecCommand(ChatHandler* handler)
{
    me->ResetTalents(true);
    m_autoTalents = false;
    m_talentState.targets.clear();
    SaveSettings();
    handler->PSendSysMessage("%s: talents reset. They will not be auto-spent until the character spends points or .persistentbot spec is used.", me->GetName());
}
