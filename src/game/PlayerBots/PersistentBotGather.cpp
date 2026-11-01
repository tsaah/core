#include "PersistentBotAI.h"
#include "Bag.h"
#include "Chat.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridSearchers.h"
#include "Item.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathFinder.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Packets/Loot.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    float const GATHER_RANGE = 30.0f;          // targets must be this close to the gathering center (owner or farm anchor)
    float const FISH_OWNER_RANGE = 30.0f;      // fishing stops when the owner is farther than this
    float const ARRIVE_DIST = 3.0f;            // interaction distance for nodes, corpses and the owner
    float const ROUTE_STEP = 5.0f;             // safety samples along the route every this many yards
    float const AGGRO_MARGIN = 2.0f;           // added to a creature's aggro distance when checking the route
    float const ROUTE_START_SKIP = 5.0f;       // route points this close to the bot are not checked: it already stands there
    float const ROUTE_AGGRO_SEARCH = 60.0f;    // creatures this far beyond the route are looked at for aggro
    time_t const REJECT_LOG_INTERVAL = 30;     // a rejection reason is logged once per node this often (diagnostics)
    time_t const SCAN_INTERVAL = 2;
    time_t const SAFE_CHECK_INTERVAL = 2;
    time_t const SKIP_TIME = 60;               // a failed target is left alone this long
    time_t const CLAIM_TIMEOUT = 120;          // a claim by a bot that never released it expires after this
    time_t const WALK_TIMEOUT = 60;
    time_t const CAST_TIMEOUT = 15;            // a gather cast still running after this long is given up
    uint8 const MAX_GATHER_ATTEMPTS = 5;       // casts on one target (failed attempts are retried) before it is skipped
    time_t const FARM_IDLE_TIME = 300;         // same as NO_TARGET_IDLE in PersistentBotFarm.cpp: a farm idle this long does not gather
    time_t const MOVE_REISSUE = 3;
    uint32 const MOVE_MAX_STRIKES = 3;
    uint32 const MAX_ROUTE_CHECKS = 3;
    uint32 const FISH_MAX_FAILURES = 3;

    uint32 const ITEM_MINING_PICK = 2901;
    uint32 const ITEM_SKINNING_KNIFE = 7005;
    uint32 const ITEM_FISHING_POLE = 6256;
    uint32 const ITEM_BLACKSMITH_HAMMER = 5956;
    uint32 const ITEM_ARCLIGHT_SPANNER = 6219;

    // Profession tools: the tool that must be in the bags (or equipped) while the bot knows the profession.
    PersistentBotToolNeed const TOOLS_FOR_PROFESSIONS[] =
    {
        { ITEM_MINING_PICK, SKILL_MINING },
        { ITEM_SKINNING_KNIFE, SKILL_SKINNING },
        { ITEM_FISHING_POLE, SKILL_FISHING },
        { ITEM_BLACKSMITH_HAMMER, SKILL_BLACKSMITHING },
    };

    struct GatherClaim
    {
        ObjectGuid owner;
        time_t since;
    };

    // Targets currently walked to or gathered by a persistent bot: a bot does not go after a target another bot holds.
    // Bots on different maps run on different map threads, so every access goes through s_claimLock.
    std::mutex s_claimLock;
    std::map<ObjectGuid, GatherClaim> s_claims;

    bool IsClaimedByOther(ObjectGuid const& target, ObjectGuid const& bot, time_t now)
    {
        std::lock_guard<std::mutex> guard(s_claimLock);
        auto itr = s_claims.find(target);
        return itr != s_claims.end() && itr->second.owner != bot && now - itr->second.since < CLAIM_TIMEOUT;
    }

    void ClaimTarget(ObjectGuid const& target, ObjectGuid const& bot, time_t now)
    {
        std::lock_guard<std::mutex> guard(s_claimLock);
        for (auto itr = s_claims.begin(); itr != s_claims.end();)
        {
            if (now - itr->second.since >= CLAIM_TIMEOUT)
                itr = s_claims.erase(itr);
            else
                ++itr;
        }
        s_claims[target] = GatherClaim{ bot, now };
    }

    void ReleaseClaim(ObjectGuid const& target, ObjectGuid const& bot)
    {
        std::lock_guard<std::mutex> guard(s_claimLock);
        auto itr = s_claims.find(target);
        if (itr != s_claims.end() && itr->second.owner == bot)
            s_claims.erase(itr);
    }

    float Dist2D(float x1, float y1, float x2, float y2)
    {
        float const dx = x1 - x2;
        float const dy = y1 - y2;
        return std::sqrt(dx * dx + dy * dy);
    }

    void SayParty(Player* me, std::string const& text)
    {
        Group* pGroup = me->GetGroup();
        if (!pGroup)
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName());
        pGroup->BroadcastPacket(&data, false);
    }

    // Diagnostics: why a node was passed over, logged once per node every REJECT_LOG_INTERVAL seconds (Server.log, BASIC).
    void LogGatherReject(Player* me, PersistentBotGatherState& g, ObjectGuid const& guid, time_t now, std::string const& text)
    {
        if (g.rejectLogged.size() > 128)
            g.rejectLogged.clear();

        auto itr = g.rejectLogged.find(guid);
        if (itr != g.rejectLogged.end() && now - itr->second < REJECT_LOG_INTERVAL)
            return;

        g.rejectLogged[guid] = now;
        std::string where;
        if (guid)
            where = " [node " + std::to_string(guid.GetEntry()) + "/" + std::to_string(guid.GetCounter()) + "]";
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[PersistentBot][Gather] %s: %s%s", me->GetName(), text.c_str(), where.c_str());
    }

    char const* GetProfessionName(uint32 skillId)
    {
        switch (skillId)
        {
            case SKILL_MINING: return "Mining";
            case SKILL_SKINNING: return "Skinning";
            case SKILL_FISHING: return "Fishing";
            case SKILL_BLACKSMITHING: return "Blacksmithing";
        }
        return "";
    }

    uint32 CountFreeSlots(Player const* pPlayer)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* pBag = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot)))
                free += pBag->GetFreeSlots();
        return free;
    }

    // Position of a free inventory slot: backpack first, then the bags; false if all of them are full.
    bool FindFreeInventoryPos(Player const* pPlayer, uint16& pos)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (!pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                pos = uint16((INVENTORY_SLOT_BAG_0 << 8) | slot);
                return true;
            }
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pBag = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (!pPlayer->GetItemByPos(bagSlot, slot))
                {
                    pos = uint16((bagSlot << 8) | slot);
                    return true;
                }
            }
        }
        return false;
    }

    // Fishing poles of vanilla 6256 kind are two-handed: they displace the off-hand item too.
    bool IsTwoHanded(Item const* pItem)
    {
        return pItem && pItem->GetProto()->InventoryType == INVTYPE_2HWEAPON;
    }

    bool IsFishingPole(ItemPrototype const* pProto)
    {
        return pProto && pProto->Class == ITEM_CLASS_WEAPON && pProto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    // A fishing pole equipped or in the bags.
    Item* FindFishingPole(Player const* pPlayer)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (IsFishingPole(pItem->GetProto()))
                    return pItem;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (IsFishingPole(pItem->GetProto()))
                    return pItem;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pBag = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* pItem = pPlayer->GetItemByPos(bagSlot, slot))
                    if (IsFishingPole(pItem->GetProto()))
                        return pItem;
        }
        return nullptr;
    }

    // Highest known rank of a spell by name; nullptr if the bot does not know it.
    SpellEntry const* FindKnownSpellByName(Player const* me, char const* name)
    {
        SpellEntry const* pBest = nullptr;
        for (auto const& spell : me->GetSpellMap())
        {
            if (spell.second.disabled || spell.second.state == PLAYERSPELL_REMOVED)
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

    // Gathering spell for a node the bot can open right now (herbalism or mining lock case within its skill).
    struct NodeGather
    {
        char const* spellName;
        char const* verb;
        uint32 skillId;
        uint32 toolId; // item that must be in the bags, 0 if none
    };

    bool GetNodeGather(Player* me, GameObject* pGo, NodeGather& out)
    {
        if (pGo->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            return false;

        uint32 const lockId = pGo->GetGOInfo()->GetLockId();
        LockEntry const* pLock = lockId ? sLockStore.LookupEntry(lockId) : nullptr;
        if (!pLock)
            return false;

        for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (pLock->Type[i] != LOCK_KEY_SKILL)
                continue;

            switch (pLock->Index[i])
            {
                case LOCKTYPE_HERBALISM:
                    out = { "Herb Gathering", "Gathered", SKILL_HERBALISM, 0 };
                    break;
                case LOCKTYPE_MINING:
                    out = { "Mining", "Mined", SKILL_MINING, ITEM_MINING_PICK };
                    break;
                default:
                    continue;
            }

            return me->HasSkill(out.skillId) && me->GetSkillValue(out.skillId) >= pLock->Skill[i] &&
                   (!out.toolId || me->GetItemCount(out.toolId) > 0);
        }
        return false;
    }

    // Skinning of a dead creature by this bot now: skinnable by it, body looted, skill and level, knife in the bags.
    bool CanSkinCorpse(Player* me, Creature* pCreature)
    {
        if (pCreature->IsAlive() || !pCreature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) || pCreature->lootForSkin ||
            !pCreature->IsSkinnableBy(me))
            return false;

        // the server refuses to skin a corpse whose body is still lootable (critters have no body loot)
        if (pCreature->GetCreatureType() != CREATURE_TYPE_CRITTER && !pCreature->loot.isLooted())
            return false;

        if (!me->HasSkill(SKILL_SKINNING) || me->GetItemCount(ITEM_SKINNING_KNIFE) == 0)
            return false;

        // same requirement as the skinning spell's cast check
        int32 const skill = me->GetSkillValue(SKILL_SKINNING);
        int32 const level = pCreature->GetLevel();
        int32 const required = skill < 100 ? (level - 10) * 10 : level * 5;
        return required <= skill;
    }

    struct AnyGameObjectCheck
    {
        bool operator()(GameObject*) const { return true; }
    };

    struct AnyCreatureCheck
    {
        bool operator()(Creature*) const { return true; }
    };

    struct GatherPoint
    {
        float x;
        float y;
        float z;
    };

    // Samples of the pathfinder route from the bot to (x, y, z), one every ROUTE_STEP yards. False if there is no route.
    bool BuildRoute(Player* me, float x, float y, float z, std::vector<GatherPoint>& route, uint32* pPathType = nullptr)
    {
        PathFinder path(me);
        path.calculate(x, y, z);
        if (pPathType)
            *pPathType = path.getPathType();
        if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
            return false;

        route.clear();
        GatherPoint from = { me->GetPositionX(), me->GetPositionY(), me->GetPositionZ() };
        route.push_back(from);
        for (auto const& point : path.getPath())
        {
            float const dist = Dist2D(from.x, from.y, point.x, point.y);
            uint32 const steps = uint32(dist / ROUTE_STEP);
            for (uint32 i = 1; i <= steps; ++i)
            {
                float const t = (ROUTE_STEP * i) / dist;
                route.push_back({ from.x + (point.x - from.x) * t, from.y + (point.y - from.y) * t, from.z + (point.z - from.z) * t });
            }
            route.push_back({ point.x, point.y, point.z });
            from = { point.x, point.y, point.z };
        }
        route.push_back({ x, y, z });
        return true;
    }

    // False if an alive hostile creature could pull the bot on the part of the route it still has to walk (the bot's own
    // spot is not checked: it already stands there). The aggro distance is the creature's own (GetAttackDistance, level
    // scaled) plus AGGRO_MARGIN. Critters, civilians, dead, evading, passive or defensive creatures and creatures already
    // fighting the bot's group never start a pull, so they are ignored. why gets the blocking creature when there is one.
    bool IsRouteSafe(Player* me, std::vector<GatherPoint> const& route, std::string* why = nullptr)
    {
        std::vector<GatherPoint> walk;
        for (GatherPoint const& point : route)
            if (Dist2D(me->GetPositionX(), me->GetPositionY(), point.x, point.y) > ROUTE_START_SKIP)
                walk.push_back(point);
        if (walk.empty())
            return true;

        float reach = 0.0f;
        for (GatherPoint const& point : walk)
            reach = std::max(reach, Dist2D(me->GetPositionX(), me->GetPositionY(), point.x, point.y));

        AnyCreatureCheck check;
        std::list<Creature*> found;
        MaNGOS::CreatureListSearcher<AnyCreatureCheck> searcher(found, check);
        Cell::VisitGridObjects(me, searcher, reach + ROUTE_AGGRO_SEARCH);

        Group* pGroup = me->GetGroup();
        for (Creature* pCreature : found)
        {
            CreatureInfo const* pInfo = pCreature->GetCreatureInfo();
            if (!pInfo || !pCreature->IsAlive() || pInfo->type == CREATURE_TYPE_CRITTER || pCreature->IsCivilian() || !pCreature->IsHostileTo(me))
                continue;
            if (pCreature->IsInEvadeMode() || pCreature->GetReactState() != REACT_AGGRESSIVE)
                continue;

            if (pGroup)
            {
                Unit* pVictim = pCreature->GetVictim();
                Player* pVictimPlayer = pVictim ? (pVictim->IsPlayer() ? static_cast<Player*>(pVictim) : pVictim->GetCharmerOrOwnerPlayer()) : nullptr;
                if (pVictimPlayer && pVictimPlayer->GetGroup() == pGroup)
                    continue;
            }

            float const aggroDist = pCreature->GetAttackDistance(me) + AGGRO_MARGIN;
            for (GatherPoint const& point : walk)
            {
                float const dist = Dist2D(pCreature->GetPositionX(), pCreature->GetPositionY(), point.x, point.y);
                if (dist <= aggroDist)
                {
                    if (why)
                    {
                        char text[160];
                        snprintf(text, sizeof(text), "route blocked by %s (entry %u, guid %u) %.1f yd away, aggro %.1f yd",
                            pCreature->GetName(), pCreature->GetEntry(), pCreature->GetGUIDLow(), dist, aggroDist);
                        *why = text;
                    }
                    return false;
                }
            }
        }
        return true;
    }

    // Takes every item of the loot window the bot has open on a node, fish or corpse (the auto-store clicks), then releases
    // it so a node despawns. Returns the taken items as "Name" or "Name x<N>", comma separated; empty if nothing was taken.
    std::string TakeOpenLoot(Player* me, Loot& loot, ObjectGuid const& guid)
    {
        uint32 const lowGuid = me->GetGUIDLow();
        uint32 const slots = loot.GetMaxSlotInLootFor(lowGuid);

        std::vector<uint32> itemIds;
        for (uint32 slot = 0; slot < slots; ++slot)
        {
            LootItem* pLootItem = loot.LootItemInSlot(slot, lowGuid);
            if (pLootItem && std::find(itemIds.begin(), itemIds.end(), pLootItem->itemid) == itemIds.end())
                itemIds.push_back(pLootItem->itemid);
        }

        std::vector<uint32> before;
        for (uint32 itemId : itemIds)
            before.push_back(me->GetItemCount(itemId));

        WorldSession* pSession = me->GetSession();
        for (uint32 slot = 0; slot < slots; ++slot)
        {
            WorldPackets::Loot::AutoStoreLootItem packet;
            packet.lootSlot = uint8(slot);
            pSession->HandleAutostoreLootItemOpcode(packet);
        }
        pSession->DoLootRelease(guid);

        std::string text;
        for (size_t i = 0; i < itemIds.size(); ++i)
        {
            uint32 const count = me->GetItemCount(itemIds[i]);
            uint32 const taken = count > before[i] ? count - before[i] : 0;
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemIds[i]);
            if (!taken || !pProto)
                continue;

            if (!text.empty())
                text += ", ";
            text += pProto->Name1;
            if (taken > 1)
                text += " x" + std::to_string(taken);
        }
        return text;
    }

    void HaltMovement(Player* me)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
    }

    enum WalkResult : uint8
    {
        WALK_ARRIVED = 0,
        WALK_MOVING = 1,
        WALK_FAILED = 2,
    };

    // One step of a walk: MovePoint (with pathfinding) is re-issued every few seconds; no progress for several checks fails.
    WalkResult WalkTo(Player* me, PersistentBotGatherState& g, float x, float y, float z)
    {
        float const dist = me->GetDistance(x, y, z);
        if (dist <= ARRIVE_DIST)
        {
            HaltMovement(me);
            return WALK_ARRIVED;
        }

        time_t const now = time(nullptr);
        if (now - g.walkStarted > WALK_TIMEOUT)
        {
            HaltMovement(me);
            return WALK_FAILED;
        }

        if (!g.moveIssued || now - g.moveIssued >= MOVE_REISSUE)
        {
            if (g.moveIssued)
            {
                if (dist > g.moveDist - 1.0f)
                {
                    if (++g.moveStrikes >= MOVE_MAX_STRIKES)
                    {
                        HaltMovement(me);
                        return WALK_FAILED;
                    }
                }
                else
                    g.moveStrikes = 0;
            }

            g.moveIssued = now;
            g.moveDist = dist;
            if (me->GetStandState() != UNIT_STAND_STATE_STAND)
                me->SetStandState(UNIT_STAND_STATE_STAND);
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
        }
        return WALK_MOVING;
    }

    // Puts the fishing pole in the main hand (it may already be equipped in any slot the fishing spell accepts).
    // A two-handed pole first moves the off-hand item to the bags, remembering it for the restore.
    bool EquipFishingPole(Player* me, PersistentBotGatherState& g)
    {
        Item* pMain = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (pMain && IsFishingPole(pMain->GetProto()))
            return true;

        Item* pPole = FindFishingPole(me);
        if (!pPole)
            return false;
        if (pPole->IsEquipped())
            return true;

        if (IsTwoHanded(pPole))
        {
            if (Item* pOff = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            {
                uint16 freePos = 0;
                if (!FindFreeInventoryPos(me, freePos))
                    return false;
                g.savedOffHand = pOff->GetObjectGuid();
                me->SwapItem(pOff->GetPos(), freePos);
                if (me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                    return false;
            }
        }

        uint16 dest = 0;
        if (me->CanEquipItem(EQUIPMENT_SLOT_MAINHAND, dest, pPole, true) != EQUIP_ERR_OK)
            return false;
        me->SwapItem(pPole->GetPos(), dest);
        return true;
    }

    // Puts back what fishing displaced: the saved main hand (in place of the pole) and the saved off-hand item. Without a
    // saved main hand the pole goes to a free inventory slot.
    void RestoreFishingGear(Player* me, PersistentBotGatherState& g)
    {
        Item* pMain = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (pMain && IsFishingPole(pMain->GetProto()))
        {
            bool restored = false;
            if (Item* pSaved = me->GetItemByGuid(g.savedMainHand))
            {
                uint16 dest = 0;
                if (!pSaved->IsEquipped() && me->CanEquipItem(EQUIPMENT_SLOT_MAINHAND, dest, pSaved, true) == EQUIP_ERR_OK)
                {
                    me->SwapItem(pSaved->GetPos(), dest);
                    restored = true;
                }
            }

            uint16 freePos = 0;
            if (!restored && FindFreeInventoryPos(me, freePos))
                me->SwapItem(pMain->GetPos(), freePos);
        }

        // after the pole has left the main hand: a two-handed pole would block the off-hand
        if (Item* pSavedOff = me->GetItemByGuid(g.savedOffHand))
        {
            uint16 dest = 0;
            if (!pSavedOff->IsEquipped() && me->CanEquipItem(EQUIPMENT_SLOT_OFFHAND, dest, pSavedOff, true) == EQUIP_ERR_OK)
                me->SwapItem(pSavedOff->GetPos(), dest);
        }
    }
}

bool IsGatherTool(ItemPrototype const* pProto)
{
    if (!pProto)
        return false;
    if (IsFishingPole(pProto))
        return true;

    switch (pProto->ItemId)
    {
        case ITEM_MINING_PICK:
        case ITEM_SKINNING_KNIFE:
        case ITEM_BLACKSMITH_HAMMER:
        case ITEM_ARCLIGHT_SPANNER:
            return true;
    }
    return false;
}

std::vector<PersistentBotToolNeed> GetMissingTools(Player const* pPlayer)
{
    std::vector<PersistentBotToolNeed> missing;
    for (PersistentBotToolNeed const& tool : TOOLS_FOR_PROFESSIONS)
    {
        if (!pPlayer->HasSkill(tool.skillId))
            continue;

        bool const has = tool.itemId == ITEM_FISHING_POLE ? FindFishingPole(pPlayer) != nullptr : pPlayer->GetItemCount(tool.itemId) > 0;
        if (!has)
            missing.push_back(tool);
    }
    return missing;
}

// why names the first gate that refuses gathering now (diagnostics); nullptr when gathering is allowed.
bool PersistentBotAI::GatherAllowed(float& cx, float& cy, float& cz, float& range, char const*& why) const
{
    why = nullptr;
    if (!me->IsInWorld() || !me->IsAlive() || me->IsInCombat() || me->IsMounted() || me->InBattleGround() || me->IsBeingTeleported())
    {
        why = !me->IsAlive() ? "dead" : me->IsInCombat() ? "in combat" : me->IsMounted() ? "mounted" : "not in world or teleporting";
        return false;
    }

    // only the bot's own gathering loot window may be open
    if (me->GetTradeData() || (me->GetLootGuid() && me->GetLootGuid() != m_gatherState.target))
    {
        why = "trade or loot window open";
        return false;
    }

    if (m_economyState.vendorStage != PersistentBotEconomyState::VENDOR_IDLE)
    {
        why = "vendor trip";
        return false;
    }
    if (NeedsRest())
    {
        why = "needs rest (health or mana low)";
        return false;
    }

    if (m_farmState.farming)
    {
        // farm mode: only while grinding around the anchor (not walking to or from a vendor, not blocked from town, not idle:
        // the farm leash pulls an idle or blocked bot back to its anchor, which would fight a gather walk)
        if (m_farmState.phase != PersistentBotFarmState::PHASE_GRIND || m_farmState.townBlocked ||
            time(nullptr) - m_farmState.lastTargetSeen > FARM_IDLE_TIME)
        {
            why = "farm not grinding";
            return false;
        }

        // the bot stands up to ARRIVE_DIST from its target: the target must sit that much inside the leash, or the bot would leave it
        range = std::min(GATHER_RANGE, m_farmState.radius - ARRIVE_DIST);
        if (range <= 0.0f)
        {
            why = "farm leash too small";
            return false;
        }

        cx = m_farmState.anchorX;
        cy = m_farmState.anchorY;
        cz = m_farmState.anchorZ;
        return true;
    }

    // following: gather around the owner, who must be on this map before any of its state is read, and out of combat
    Player* pOwner = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pOwner || !pOwner->IsInWorld() || pOwner->GetMapId() != me->GetMapId())
    {
        why = "owner not on this map";
        return false;
    }
    if (!pOwner->IsAlive() || pOwner->IsInCombat() || !GetPartyLeader())
    {
        why = !GetPartyLeader() ? "not in the owner's group" : "owner dead or in combat";
        return false;
    }

    range = GATHER_RANGE;
    cx = pOwner->GetPositionX();
    cy = pOwner->GetPositionY();
    cz = pOwner->GetPositionZ();
    return true;
}

bool PersistentBotAI::UpdateGather()
{
    PersistentBotGatherState& g = m_gatherState;
    time_t const now = time(nullptr);

    NagMissingTools();

    if (g.fishing)
        return UpdateFishing();

    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float range = 0.0f;
    char const* why = nullptr;
    if (!GatherAllowed(cx, cy, cz, range, why))
    {
        // diagnostics: the reason is logged when it changes and at most once per REJECT_LOG_INTERVAL
        if (why != g.idleReason || now - g.idleLogged >= REJECT_LOG_INTERVAL)
        {
            g.idleReason = why;
            g.idleLogged = now;
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[PersistentBot][Gather] %s: gathering paused: %s", me->GetName(), why);
        }
        CancelGather(false);
        return false;
    }
    g.idleReason = nullptr;

    if (g.phase == PersistentBotGatherState::PHASE_IDLE)
    {
        if (now < g.nextScan)
            return false;
        g.nextScan = now + SCAN_INTERVAL;
        return StartGatherTarget(now, cx, cy, cz, range);
    }

    return UpdateGatherTarget(now, cx, cy, cz, range);
}

// Looks for the nearest gatherable node or skinnable corpse around the center, checks the route, and starts walking to it.
bool PersistentBotAI::StartGatherTarget(time_t now, float cx, float cy, float cz, float range)
{
    PersistentBotGatherState& g = m_gatherState;
    ObjectGuid const myGuid = me->GetObjectGuid();

    for (auto itr = g.skipped.begin(); itr != g.skipped.end();)
    {
        if (itr->second <= now)
            itr = g.skipped.erase(itr);
        else
            ++itr;
    }

    // a full bag would leave the loot in the node: wait until there is room
    if (me->IsNonMeleeSpellCasted(false))
        return false;
    if (!CountFreeSlots(me))
    {
        LogGatherReject(me, g, ObjectGuid(), now, "no free bag slot, not gathering");
        return false;
    }

    struct Candidate
    {
        ObjectGuid guid;
        float x;
        float y;
        float z;
        float dist;
        bool corpse;
        char const* verb;
    };
    std::vector<Candidate> cands;

    float const visit = me->GetDistance(cx, cy, cz) + range;

    std::list<GameObject*> gameObjects;
    AnyGameObjectCheck goCheck;
    MaNGOS::GameObjectListSearcher<AnyGameObjectCheck> goSearcher(gameObjects, goCheck);
    Cell::VisitGridObjects(me, goSearcher, visit);

    for (GameObject* pGo : gameObjects)
    {
        if (!pGo->isSpawned() || pGo->getLootState() != GO_READY || Dist2D(pGo->GetPositionX(), pGo->GetPositionY(), cx, cy) > range)
            continue;
        if (g.skipped.find(pGo->GetObjectGuid()) != g.skipped.end())
            continue;
        if (IsClaimedByOther(pGo->GetObjectGuid(), myGuid, now))
        {
            LogGatherReject(me, g, pGo->GetObjectGuid(), now, "node claimed by another bot");
            continue;
        }

        NodeGather node;
        if (!GetNodeGather(me, pGo, node))
        {
            LogGatherReject(me, g, pGo->GetObjectGuid(), now, "node is not gatherable by this bot (lock type or skill)");
            continue;
        }
        if (!FindKnownSpellByName(me, node.spellName))
        {
            LogGatherReject(me, g, pGo->GetObjectGuid(), now, std::string("no known spell '") + node.spellName + "'");
            continue;
        }

        cands.push_back({ pGo->GetObjectGuid(), pGo->GetPositionX(), pGo->GetPositionY(), pGo->GetPositionZ(), me->GetDistance(pGo), false, node.verb });
    }

    std::list<Creature*> creatures;
    AnyCreatureCheck crCheck;
    MaNGOS::CreatureListSearcher<AnyCreatureCheck> crSearcher(creatures, crCheck);
    Cell::VisitGridObjects(me, crSearcher, visit);

    for (Creature* pCreature : creatures)
    {
        if (pCreature->IsAlive() || Dist2D(pCreature->GetPositionX(), pCreature->GetPositionY(), cx, cy) > range)
            continue;
        if (g.skipped.find(pCreature->GetObjectGuid()) != g.skipped.end() || IsClaimedByOther(pCreature->GetObjectGuid(), myGuid, now))
            continue;
        if (!CanSkinCorpse(me, pCreature) || !FindKnownSpellByName(me, "Skinning"))
            continue;

        cands.push_back({ pCreature->GetObjectGuid(), pCreature->GetPositionX(), pCreature->GetPositionY(), pCreature->GetPositionZ(),
            me->GetDistance(pCreature), true, "Skinned" });
    }

    std::sort(cands.begin(), cands.end(), [](Candidate const& a, Candidate const& b) { return a.dist < b.dist; });

    uint32 checked = 0;
    for (Candidate const& cand : cands)
    {
        if (++checked > MAX_ROUTE_CHECKS)
            break;

        std::vector<GatherPoint> route;
        uint32 pathType = 0;
        if (!BuildRoute(me, cand.x, cand.y, cand.z, route, &pathType))
        {
            LogGatherReject(me, g, cand.guid, now, "no walkable path to node (path type " + std::to_string(pathType) + ")");
            g.skipped[cand.guid] = now + SKIP_TIME;
            continue;
        }
        std::string blocker;
        if (!IsRouteSafe(me, route, &blocker))
        {
            LogGatherReject(me, g, cand.guid, now, blocker);
            g.skipped[cand.guid] = now + SKIP_TIME;
            continue;
        }

        ClaimTarget(cand.guid, myGuid, now);
        g.phase = PersistentBotGatherState::PHASE_WALK;
        g.target = cand.guid;
        g.targetCorpse = cand.corpse;
        g.targetX = cand.x;
        g.targetY = cand.y;
        g.targetZ = cand.z;
        g.gatherVerb = cand.verb;
        g.walkStarted = now;
        g.attempts = 0;
        g.moveIssued = 0;
        g.moveDist = 0.0f;
        g.moveStrikes = 0;
        g.nextSafeCheck = now + SAFE_CHECK_INTERVAL;
        return true;
    }
    return false;
}

// The target still exists and can be gathered by this bot now (node spawned and ready, or corpse skinnable), and is
// within range of the gathering center.
bool PersistentBotAI::IsGatherTargetValid(float cx, float cy, float range)
{
    PersistentBotGatherState const& g = m_gatherState;

    bool valid;
    if (g.targetCorpse)
    {
        Creature* pCorpse = me->GetMap()->GetCreature(g.target);
        valid = pCorpse && CanSkinCorpse(me, pCorpse) && FindKnownSpellByName(me, "Skinning");
    }
    else
    {
        GameObject* pGo = me->GetMap()->GetGameObject(g.target);
        NodeGather node;
        valid = pGo && pGo->isSpawned() && pGo->getLootState() == GO_READY && GetNodeGather(me, pGo, node) && FindKnownSpellByName(me, node.spellName);
    }
    return valid && Dist2D(g.targetX, g.targetY, cx, cy) <= range;
}

// Walking to a target and waiting for its cast; the loot is taken as soon as the cast has ended.
bool PersistentBotAI::UpdateGatherTarget(time_t now, float cx, float cy, float cz, float range)
{
    PersistentBotGatherState& g = m_gatherState;

    if (g.phase == PersistentBotGatherState::PHASE_CAST)
    {
        // a cast that does not end is given up; SkipGatherTarget interrupts it
        if (now - g.castStarted > CAST_TIMEOUT)
        {
            SkipGatherTarget(now);
            return false;
        }

        if (me->IsNonMeleeSpellCasted(false))
            return true;

        if (me->GetLootGuid() != g.target)
        {
            // nothing was opened: a failed attempt (a chance near the skill requirement) leaves the node in place, so cast
            // again while the target is still there; otherwise give it up. The claim is kept across the retries.
            if (++g.attempts < MAX_GATHER_ATTEMPTS && IsGatherTargetValid(cx, cy, range) &&
                me->GetDistance(g.targetX, g.targetY, g.targetZ) <= ARRIVE_DIST && StartGatherCast())
            {
                g.castStarted = now;
                return true;
            }

            SkipGatherTarget(now);
            return false;
        }

        std::string taken;
        if (g.targetCorpse)
        {
            if (Creature* pCorpse = me->GetMap()->GetCreature(g.target))
                taken = TakeOpenLoot(me, pCorpse->loot, g.target);
        }
        else if (GameObject* pGo = me->GetMap()->GetGameObject(g.target))
            taken = TakeOpenLoot(me, pGo->loot, g.target);

        if (!taken.empty())
            SayParty(me, std::string(g.gatherVerb) + " " + taken + ".");

        // the node is released now; it despawns on its own
        ReleaseGatherClaim();
        g.phase = PersistentBotGatherState::PHASE_IDLE;
        g.target = ObjectGuid();
        g.nextScan = now;
        return false;
    }

    // walking: the target must still be there and in range of the center
    if (!IsGatherTargetValid(cx, cy, range))
    {
        CancelGather(false);
        return false;
    }

    // the route is checked again every few seconds while walking
    if (now >= g.nextSafeCheck)
    {
        g.nextSafeCheck = now + SAFE_CHECK_INTERVAL;
        std::vector<GatherPoint> route;
        std::string blocker;
        uint32 pathType = 0;
        if (!BuildRoute(me, g.targetX, g.targetY, g.targetZ, route, &pathType))
        {
            LogGatherReject(me, g, g.target, now, "walk aborted: no walkable path (path type " + std::to_string(pathType) + ")");
            SkipGatherTarget(now);
            return false;
        }
        if (!IsRouteSafe(me, route, &blocker))
        {
            LogGatherReject(me, g, g.target, now, "walk aborted: " + blocker);
            SkipGatherTarget(now);
            return false;
        }
    }

    WalkResult const walk = WalkTo(me, g, g.targetX, g.targetY, g.targetZ);
    if (walk == WALK_MOVING)
        return true;
    if (walk == WALK_FAILED || !StartGatherCast())
    {
        SkipGatherTarget(now);
        return false;
    }

    g.phase = PersistentBotGatherState::PHASE_CAST;
    g.castStarted = now;
    return true;
}

// Opens the node with its gathering spell (the server then fills its loot), or casts Skinning on the corpse.
bool PersistentBotAI::StartGatherCast()
{
    PersistentBotGatherState& g = m_gatherState;

    if (g.targetCorpse)
    {
        Creature* pCorpse = me->GetMap()->GetCreature(g.target);
        SpellEntry const* pSkinning = FindKnownSpellByName(me, "Skinning");
        return pCorpse && pSkinning && DoCastSpell(pCorpse, pSkinning) == SPELL_CAST_OK;
    }

    GameObject* pGo = me->GetMap()->GetGameObject(g.target);
    NodeGather node;
    if (!pGo || !GetNodeGather(me, pGo, node))
        return false;

    SpellEntry const* pSpell = FindKnownSpellByName(me, node.spellName);
    if (!pSpell)
        return false;

    SpellCastTargets targets;
    targets.setGOTarget(pGo);
    Spell* pCast = new Spell(me, pSpell, false);
    SpellCastResult const result = pCast->prepare(std::move(targets));
    if (result != SPELL_CAST_OK)
        LogGatherReject(me, g, g.target, time(nullptr), "cast refused, result " + std::to_string(uint32(result)));
    return result == SPELL_CAST_OK;
}

void PersistentBotAI::SkipGatherTarget(time_t now)
{
    m_gatherState.skipped[m_gatherState.target] = now + SKIP_TIME;
    CancelGather(false);
}

void PersistentBotAI::ReleaseGatherClaim()
{
    if (!m_gatherState.target)
        return;

    ReleaseClaim(m_gatherState.target, me->GetObjectGuid());
}

// Drops whatever the bot is doing for gathering or fishing: interrupts its cast, stops walking, gives its claim back
// and restores the main hand after fishing. announceFishStop tells the party that fishing ended.
void PersistentBotAI::CancelGather(bool announceFishStop)
{
    PersistentBotGatherState& g = m_gatherState;

    if (me->IsInWorld())
    {
        if ((g.phase == PersistentBotGatherState::PHASE_CAST || g.fishing) && me->IsNonMeleeSpellCasted(false))
            me->InterruptNonMeleeSpells(false);

        if (g.fishing)
        {
            if (announceFishStop)
                SayParty(me, std::string(me->GetName()) + " stopped fishing.");
            RestoreFishingGear(me, g);
        }

        if (g.phase == PersistentBotGatherState::PHASE_WALK)
            HaltMovement(me);
    }

    ReleaseGatherClaim();
    g.phase = PersistentBotGatherState::PHASE_IDLE;
    g.target = ObjectGuid();
    g.targetCorpse = false;
    g.gatherVerb = nullptr;
    g.fishing = false;
    g.savedMainHand = ObjectGuid();
    g.savedOffHand = ObjectGuid();
    g.bobberSeen = false;
    g.fishFailures = 0;
}

// Fishing is explicit only: walk to the owner, take their orientation, equip the pole, cast, reel in the bites.
bool PersistentBotAI::UpdateFishing()
{
    PersistentBotGatherState& g = m_gatherState;

    Player* pOwner = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pOwner || !pOwner->IsInWorld() || pOwner->GetMapId() != me->GetMapId() || me->GetDistance(pOwner) > FISH_OWNER_RANGE ||
        !me->IsAlive() || me->IsInCombat() || !GetPartyLeader() || !CountFreeSlots(me))
    {
        CancelGather(true);
        return false;
    }

    switch (g.phase)
    {
        case PersistentBotGatherState::PHASE_WALK:
        {
            WalkResult const walk = WalkTo(me, g, pOwner->GetPositionX(), pOwner->GetPositionY(), pOwner->GetPositionZ());
            if (walk == WALK_MOVING)
                return true;
            if (walk == WALK_FAILED || !EquipFishingPole(me, g))
            {
                SayParty(me, std::string(me->GetName()) + " can't fish here.");
                CancelGather(false);
                return false;
            }
            me->SetFacingTo(pOwner->GetOrientation());
            g.phase = PersistentBotGatherState::PHASE_FISH_CAST;
            return true;
        }

        case PersistentBotGatherState::PHASE_FISH_CAST:
        {
            if (me->IsNonMeleeSpellCasted(false))
                return true;

            SpellEntry const* pFishing = FindKnownSpellByName(me, "Fishing");
            if (!pFishing)
            {
                SayParty(me, std::string(me->GetName()) + " can't fish here.");
                CancelGather(false);
                return false;
            }

            g.bobberSeen = false;
            if (me->CastSpell(me, pFishing, false) == SPELL_CAST_OK)
            {
                g.phase = PersistentBotGatherState::PHASE_FISH_WAIT;
                return true;
            }

            if (++g.fishFailures >= FISH_MAX_FAILURES)
            {
                SayParty(me, std::string(me->GetName()) + " can't fish here.");
                CancelGather(false);
                return false;
            }
            return true;
        }

        case PersistentBotGatherState::PHASE_FISH_WAIT:
        {
            ObjectGuid const bobberGuid = me->GetChannelObjectGuid();
            GameObject* pBobber = bobberGuid ? me->GetMap()->GetGameObject(bobberGuid) : nullptr;
            if (!pBobber)
            {
                if (me->IsNonMeleeSpellCasted(false))
                    return true;

                // the cast ended without a bobber: it never reached fishable water (counted), or it was reeled in already
                if (!g.bobberSeen && ++g.fishFailures >= FISH_MAX_FAILURES)
                {
                    SayParty(me, std::string(me->GetName()) + " can't fish here.");
                    CancelGather(false);
                    return false;
                }
                g.phase = PersistentBotGatherState::PHASE_FISH_CAST;
                return true;
            }

            g.bobberSeen = true;
            g.fishFailures = 0;
            if (pBobber->getLootState() != GO_READY)
                return true; // no bite yet

            // the bite window is open: reel in as the owner's click on the bobber would
            pBobber->Use(me);
            // a fishing hole under the bobber takes the catch, so the loot window can be on the hole instead of the bobber
            ObjectGuid const lootGuid = me->GetLootGuid();
            if (lootGuid)
            {
                if (GameObject* pLootGo = me->GetMap()->GetGameObject(lootGuid))
                {
                    std::string const caught = TakeOpenLoot(me, pLootGo->loot, lootGuid);
                    if (!caught.empty())
                        SayParty(me, "Caught " + caught + ".");
                }
                else
                    me->GetSession()->DoLootRelease(lootGuid);
            }
            g.phase = PersistentBotGatherState::PHASE_FISH_CAST;
            return true;
        }

        default:
            CancelGather(true);
            return false;
    }
}

void PersistentBotAI::NagMissingTools()
{
    if (!me->GetGroup())
        return;

    for (PersistentBotToolNeed const& need : GetMissingTools(me))
    {
        if (!m_gatherState.toolsNagged.insert(need.itemId).second)
            continue;

        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(need.itemId);
        if (pProto)
            SayParty(me, "I need a " + std::string(pProto->Name1) + " for " + GetProfessionName(need.skillId) + ".");
        return; // one line per tick
    }
}

void PersistentBotAI::HandleFishCommand(ChatHandler* handler)
{
    PersistentBotGatherState& g = m_gatherState;
    std::string const name = me->GetName();

    if (g.fishing)
    {
        handler->PSendSysMessage("%s starts fishing.", name.c_str());
        return;
    }

    if (!me->HasSkill(SKILL_FISHING))
    {
        handler->PSendSysMessage("%s doesn't know Fishing.", name.c_str());
        return;
    }

    if (!FindFishingPole(me))
    {
        handler->PSendSysMessage("%s has no fishing pole.", name.c_str());
        return;
    }

    Player* pOwner = ObjectAccessor::FindPlayer(m_leaderGuid);
    // a gather walk or cast in progress does not block fishing: it is cancelled below; only a loot window of something else does
    if (m_farmState.farming || !me->IsAlive() || me->IsInCombat() || me->IsMounted() ||
        !pOwner || !pOwner->IsInWorld() || pOwner->GetMapId() != me->GetMapId() || pOwner->IsInCombat() ||
        me->GetDistance(pOwner) > FISH_OWNER_RANGE || !GetPartyLeader() || !CountFreeSlots(me) ||
        (me->GetLootGuid() && me->GetLootGuid() != g.target) || me->GetTradeData() ||
        m_economyState.vendorStage != PersistentBotEconomyState::VENDOR_IDLE)
    {
        handler->PSendSysMessage("%s can't fish here.", name.c_str());
        return;
    }

    // room for what the pole displaces: the main hand weapon, and the off-hand item of a two-handed pole
    Item* pPole = FindFishingPole(me);
    Item* pMainItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* pOffItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    uint32 needed = 0;
    if (pMainItem && !IsFishingPole(pMainItem->GetProto()))
        ++needed;
    if (pOffItem && IsTwoHanded(pPole))
        ++needed;
    if (needed > CountFreeSlots(me))
    {
        handler->PSendSysMessage("%s has no room to unequip for fishing.", name.c_str());
        return;
    }

    // a gather walk or cast in progress is dropped first; the main hand is saved before the pole goes in
    CancelGather(false);
    Item* pMain = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (pMain && !IsFishingPole(pMain->GetProto()))
        g.savedMainHand = pMain->GetObjectGuid();

    g.fishing = true;
    g.phase = PersistentBotGatherState::PHASE_WALK;
    g.walkStarted = time(nullptr);
    g.moveIssued = 0;
    g.moveDist = 0.0f;
    g.moveStrikes = 0;
    g.fishFailures = 0;
    g.bobberSeen = false;

    handler->PSendSysMessage("%s starts fishing.", name.c_str());
}

void PersistentBotAI::HandleFishStopCommand(ChatHandler* handler)
{
    std::string const name = me->GetName();
    if (!m_gatherState.fishing)
    {
        handler->PSendSysMessage("%s is not fishing.", name.c_str());
        return;
    }

    CancelGather(false);
    handler->PSendSysMessage("%s stopped fishing.", name.c_str());
}
