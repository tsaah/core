#include "PersistentBotAI.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Pet.h"
#include "Group.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Database/DatabaseEnv.h"
#include "Opcodes.h"
#include "WorldPacket.h"

#include <atomic>
#include <list>
#include <set>
#include <vector>

namespace
{
    float const DRAIN_SOUL_MAX_HEALTH_PCT = 25.0f;
    float const PET_SACRIFICE_MAX_HEALTH_PCT = 20.0f;
    float const PET_CONSUME_SHADOWS_MAX_HEALTH_PCT = 70.0f;
    float const PET_HELP_RANGE = 30.0f;
    size_t const SUFFERING_MIN_ATTACKERS = 3;

    // Pet spells matched by name (all ranks). Autocast goes on for PET_AUTOCAST_ON and off for PET_AUTOCAST_OFF;
    // any other pet spell keeps its setting.
    char const* const PET_AUTOCAST_ON[] = { "Firebolt", "Blood Pact", "Fire Shield", "Torment", "Lash of Pain", "Devour Magic", "Tainted Blood" };
    char const* const PET_AUTOCAST_OFF[] = { "Phase Shift", "Lesser Invisibility", "Soothing Kiss", "Seduction", "Suffering", "Sacrifice", "Spell Lock", "Consume Shadows", "Paranoia" };

    template <size_t N>
    bool IsNamedIn(std::string const& name, char const* const (&names)[N])
    {
        for (char const* listed : names)
            if (name == listed)
                return true;
        return false;
    }

    // Highest known rank of a pet spell; nullptr if the pet does not know it.
    SpellEntry const* FindPetSpellByName(Pet const* pPet, char const* name)
    {
        SpellEntry const* pBest = nullptr;
        for (auto const& spell : pPet->m_petSpells)
        {
            if (spell.second.state == PETSPELL_REMOVED)
                continue;

            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spell.first);
            if (!pSpell || pSpell->SpellName[0] != name)
                continue;

            if (!pBest || pSpell->GetRank() > pBest->GetRank() ||
                (pSpell->GetRank() == pBest->GetRank() && pSpell->Id > pBest->Id))
                pBest = pSpell;
        }
        return pBest;
    }

    // Casts the pet's known rank of a spell at pTarget through the pet's normal cast path and checks; true if the cast started.
    bool CastPetSpell(Pet* pPet, char const* name, Unit* pTarget)
    {
        SpellEntry const* pSpell = FindPetSpellByName(pPet, name);
        if (!pSpell || !pTarget || !pPet->IsAlive() || pPet->IsNonMeleeSpellCasted() ||
            !pPet->IsSpellReady(pSpell) || pPet->HasGCD(pSpell))
            return false;

        Powers const powerType = Powers(pSpell->powerType);
        if (powerType != POWER_HEALTH && pPet->GetPower(powerType) < Spell::CalculatePowerCost(pSpell, pPet))
            return false;

        return pPet->CastSpell(pTarget, pSpell, false) == SPELL_CAST_OK;
    }

    // Sets autocast on every known rank of the PET_AUTOCAST_ON spells and off for PET_AUTOCAST_OFF.
    // ToggleAutocast marks the spell changed, so the setting is saved with the pet (Player::SaveToDB -> SavePetToDB).
    void SetPetAutocast(Pet* pPet)
    {
        for (auto const& spell : pPet->m_petSpells)
        {
            if (spell.second.state == PETSPELL_REMOVED || !Spells::IsAutocastable(spell.first))
                continue;

            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spell.first);
            if (!pSpell)
                continue;

            bool apply;
            if (IsNamedIn(pSpell->SpellName[0], PET_AUTOCAST_ON))
                apply = true;
            else if (IsNamedIn(pSpell->SpellName[0], PET_AUTOCAST_OFF))
                apply = false;
            else
                continue;

            if ((spell.second.active == ACT_ENABLED) == apply)
                continue;

            pPet->ToggleAutocast(spell.first, apply);
            pPet->GetCharmInfo()->SetSpellAutocast(spell.first, apply);
        }
    }

    bool IsPartyMember(std::vector<Player*> const& party, ObjectGuid const& guid)
    {
        for (Player const* pMember : party)
            if (pMember->GetObjectGuid() == guid)
                return true;
        return false;
    }

    // Highest rank of Drain Soul the bot knows; nullptr if none.
    SpellEntry const* FindKnownDrainSoul(Player const* me)
    {
        SpellEntry const* pBest = nullptr;
        for (auto const& spell : me->GetSpellMap())
        {
            if (spell.second.disabled || spell.second.state == PLAYERSPELL_REMOVED)
                continue;

            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spell.first);
            if (!pSpell || pSpell->SpellName[0] != "Drain Soul")
                continue;

            if (!pBest || pSpell->GetRank() > pBest->GetRank() ||
                (pSpell->GetRank() == pBest->GetRank() && pSpell->Id > pBest->Id))
                pBest = pSpell;
        }
        return pBest;
    }
}

bool PersistentBotAI::IsDatabaseReady()
{
    // written from the map threads: several maps can run this at once
    static std::atomic<bool> ready(false);
    if (ready)
        return true;

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query("SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'persistent_bot' AND COLUMN_NAME IN ('guid', 'spec_tab', 'auto_talents', 'owner_guid', 'active')"));
    ready = result && result->Fetch()[0].GetUInt32() == 5;
    return ready;
}

void PersistentBotAI::LoadSettings()
{
    if (!IsDatabaseReady())
        return;

    std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery("SELECT `spec_tab`, `auto_talents` FROM `persistent_bot` WHERE `guid`=%u", me->GetGUIDLow()));
    if (!result)
        return;

    Field* fields = result->Fetch();
    m_specTab = fields[0].GetUInt8();
    m_autoTalents = fields[1].GetUInt8() != 0;
}

void PersistentBotAI::SaveSettings()
{
    if (!IsDatabaseReady())
        return;

    // not a REPLACE: owner_guid and active are kept
    CharacterDatabase.PExecute("INSERT INTO `persistent_bot` (`guid`, `spec_tab`, `auto_talents`) VALUES (%u, %u, %u) ON DUPLICATE KEY UPDATE `spec_tab`=VALUES(`spec_tab`), `auto_talents`=VALUES(`auto_talents`)",
        me->GetGUIDLow(), uint32(m_specTab), uint32(m_autoTalents ? 1 : 0));
}

void PersistentBotAI::MarkActive(uint32 guid, uint32 ownerGuid)
{
    if (!IsDatabaseReady())
        return;

    CharacterDatabase.PExecute("INSERT INTO `persistent_bot` (`guid`, `owner_guid`, `active`) VALUES (%u, %u, 1) ON DUPLICATE KEY UPDATE `owner_guid`=VALUES(`owner_guid`), `active`=1",
        guid, ownerGuid);
}

void PersistentBotAI::MarkInactive(uint32 guid)
{
    if (!IsDatabaseReady())
        return;

    CharacterDatabase.PExecute("UPDATE `persistent_bot` SET `active`=0 WHERE `guid`=%u", guid);
}

void PersistentBotAI::Dismiss()
{
    CancelGather(false);
    MarkInactive(me->GetGUIDLow());
    botEntry->requestRemoval = true;
}

void PersistentBotAI::OnPacketReceived(WorldPacket const* packet)
{
    switch (packet->GetOpcode())
    {
        case SMSG_LOOT_START_ROLL:
        {
            if (me && m_initialized)
                OnLootRollStart(packet);
            return;
        }
        case SMSG_PARTYKILLLOG:
            return;
    }

    PartyBotAI::OnPacketReceived(packet);
}

// A dying target the bot can get XP/honor from (and tapped, for creatures) drops a soul shard when drained, so Drain Soul
// goes first while the bags hold fewer than SOUL_SHARDS_WANTED shards. PartyBotAI::UpdateAI skips the combat AI while a
// spell is being cast, so a started channel is not interrupted.
void PersistentBotAI::UpdateInCombatAI_Warlock()
{
    Unit* pVictim = me->GetVictim();
    if (pVictim && pVictim->GetHealthPercent() <= DRAIN_SOUL_MAX_HEALTH_PCT &&
        me->GetItemCount(SOUL_SHARD_ITEM_ID) < SOUL_SHARDS_WANTED &&
        me->isHonorOrXPTarget(pVictim) &&
        (pVictim->GetTypeId() != TYPEID_UNIT || pVictim->ToCreature()->IsTappedBy(me)))
    {
        if (SpellEntry const* pDrainSoul = FindKnownDrainSoul(me))
        {
            if (CanTryToCastSpell(pVictim, pDrainSoul) && DoCastSpell(pVictim, pDrainSoul) == SPELL_CAST_OK)
                return;
        }
    }

    PartyBotAI::UpdateInCombatAI_Warlock();
}

// Autocast is set again whenever the pet is summoned or learns a spell (a grimoire); then the bot casts the pet spells
// that autocast does not cover. Called from UpdateAI in and out of combat.
void PersistentBotAI::UpdateWarlockPets()
{
    Pet* pPet = me->GetPet();
    if (!pPet || pPet->GetPetType() != SUMMON_PET)
        return;

    uint32 spellCount = 0;
    for (auto const& spell : pPet->m_petSpells)
    {
        if (spell.second.state != PETSPELL_REMOVED)
            ++spellCount;
    }

    if (pPet->GetObjectGuid() != m_petAutocastGuid || spellCount != m_petAutocastSpellCount)
    {
        m_petAutocastGuid = pPet->GetObjectGuid();
        m_petAutocastSpellCount = spellCount;
        SetPetAutocast(pPet);
    }

    std::vector<Player*> party{ me };
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (pMember != me)
                    party.push_back(pMember);
            }
        }
    }

    // Sacrifice: the bot is nearly dead in a fight.
    if (me->IsInCombat() && me->GetHealthPercent() < PET_SACRIFICE_MAX_HEALTH_PCT &&
        CastPetSpell(pPet, "Sacrifice", me))
        return;

    // Spell Lock: an enemy in range is casting on the bot or a party member.
    if (SpellEntry const* pSpellLock = FindPetSpellByName(pPet, "Spell Lock"))
    {
        std::list<Unit*> enemies;
        pPet->GetEnemyListInRadiusAround(pPet, Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(pSpellLock->rangeIndex)), enemies);
        for (Unit* pEnemy : enemies)
        {
            if (pEnemy->IsNonMeleeSpellCasted(false, false, true) && IsPartyMember(party, pEnemy->GetTargetGuid()) &&
                CastPetSpell(pPet, "Spell Lock", pEnemy))
                return;
        }
    }

    // Suffering: 3+ enemies are attacking the bot or a non-tank party member near the pet.
    std::set<Unit*> attackers;
    for (Player* pMember : party)
    {
        if (pMember != me && (!pMember->IsAlive() || IsTankClass(pMember->GetClass()) ||
            !pPet->IsWithinDistInMap(pMember, PET_HELP_RANGE)))
            continue;

        for (Unit* pAttacker : pMember->GetAttackers())
        {
            if (pAttacker->IsAlive())
                attackers.insert(pAttacker);
        }
    }
    if (attackers.size() >= SUFFERING_MIN_ATTACKERS)
    {
        Unit* pVictim = pPet->GetVictim();
        Unit* pTarget = attackers.count(pVictim) ? pVictim : *attackers.begin();
        if (CastPetSpell(pPet, "Suffering", pTarget))
            return;
    }

    // Consume Shadows: out of combat, the pet is hurt.
    if (!me->IsInCombat() && pPet->GetHealthPercent() < PET_CONSUME_SHADOWS_MAX_HEALTH_PCT)
        CastPetSpell(pPet, "Consume Shadows", pPet);
}

void PersistentBotAI::UpdateAI(uint32 const diff)
{
    if (!me->IsInWorld() || me->IsBeingTeleported())
    {
        PartyBotAI::UpdateAI(diff);
        return;
    }

    if (!m_settingsLoaded)
    {
        m_settingsLoaded = true;
        LoadSettings();
    }

    if (m_initialized && !m_talentsInitialized)
    {
        m_talentsInitialized = true;
        InitTalentsOnSpawn();
    }

    if (m_farmState.farming && m_initialized && me->IsDead())
    {
        m_busy = false;
        CancelGather(true);
        UpdateFarmDeath();
        return;
    }

    // not in the busy/out-of-combat gating below: pet spells are also cast in combat and while the bot walks
    if (m_initialized && me->IsAlive() && me->GetClass() == CLASS_WARLOCK)
    {
        m_petSpellTimer.Update(diff);
        if (m_petSpellTimer.Passed())
        {
            m_petSpellTimer.Reset(PERSISTENT_BOT_UPDATE_INTERVAL);
            UpdateWarlockPets();
        }
    }

    if (!m_initialized || !me->IsAlive() || me->IsInCombat())
    {
        // combat and death end any gathering walk, cast or fishing at once
        m_busy = false;
        if (m_initialized)
            CancelGather(true);
    }
    else
    {
        m_persistentTimer.Update(diff);
        if (m_persistentTimer.Passed())
        {
            m_persistentTimer.Reset(PERSISTENT_BOT_UPDATE_INTERVAL);

            UpdateTalents();
            UpdateEquipment();
            UpdateGrimoires();
            UpdateLootRolls();

            UpdateQuestDelivery();
            // before looting: the loot window of a node or a fish is taken here, and looting would release it
            m_busy = UpdateFarm() || UpdateGather() || UpdateLooting() || UpdateClassTrainer() || UpdateVendors();
        }
    }

    if (m_busy)
    {
        // The bot is walking on its own; only the leader-gone removal is still handled here (not in farm mode).
        if (!IsLeaderless() && !GetPartyLeader())
        {
            // the removal leaves the world: put the fishing gear back first
            m_busy = false;
            CancelGather(false);
            botEntry->requestRemoval = true;
            return;
        }

        // rest on the way (the farm tick holds the bot in place while it needs to)
        if (me->IsAlive() && !me->IsInCombat() && NeedsRest())
            DrinkAndEat();
        return;
    }

    PartyBotAI::UpdateAI(diff);
}
