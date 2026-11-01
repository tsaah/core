#include "PersistentBotAI.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "Item.h"
#include "Bag.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Formulas.h"
#include "World.h"
#include "WorldSession.h"
#include "PathFinder.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridSearchers.h"
#include "DBCStores.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr float AFK_GRIND_RANGE = 40.0f;
    constexpr float VENDOR_SEARCH_RANGE = 2000.0f;
    constexpr float VENDOR_NEAR_DIST = 40.0f;
    constexpr float ANCHOR_ARRIVE_DIST = 5.0f;
    constexpr float REST_HEALTH_PCT = 60.0f;
    constexpr float REST_MANA_PCT = 40.0f;
    constexpr float MIN_DURABILITY_PCT = 25.0f;
    constexpr uint32 DEATH_WAIT = 30;
    constexpr uint32 NO_TARGET_IDLE = 300;
    constexpr uint32 SCAN_INTERVAL = 2;
    constexpr uint32 IDLE_SCAN_INTERVAL = 10;
    constexpr uint32 TOWN_RETRY_INTERVAL = 300;
    constexpr uint32 TOWN_COOLDOWN = 600;
    constexpr uint32 TOWN_BLOCKED_COOLDOWN = 1800;
    constexpr uint32 VENDOR_FAIL_COOLDOWN = 30;
    constexpr uint32 UNREACHABLE_MEMORY = 60;
    constexpr uint32 UNREACHABLE_BACKOFF = 10;
    constexpr uint32 MAX_RETURN_FAILURES = 3;
    constexpr float STRANDED_RADIUS_MULT = 1.5f;
    constexpr uint32 TRAVEL_TIMEOUT = 900;
    constexpr uint32 TRAVEL_REISSUE = 5;
    constexpr uint32 TRAVEL_MAX_STRIKES = 3;
    constexpr uint32 MAX_PATH_CHECKS = 5;

    enum TravelResult : uint8
    {
        TRAVEL_ARRIVED = 0,
        TRAVEL_MOVING = 1,
        TRAVEL_FAILED = 2,  // no progress / unreachable
        TRAVEL_TIMED_OUT = 3,
    };

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

    float GetAverageDurabilityPct(Player const* pPlayer)
    {
        uint32 cur = 0;
        uint32 max = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 const itemMax = pItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
                if (!itemMax)
                    continue;
                cur += pItem->GetUInt32Value(ITEM_FIELD_DURABILITY);
                max += itemMax;
            }
        }
        return max ? (100.0f * cur / max) : 100.0f;
    }

    template <typename F>
    void ForEachInventoryItem(Player const* pPlayer, F fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(pItem);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pBag = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* pItem = pPlayer->GetItemByPos(bagSlot, slot))
                    fn(pItem);
        }
    }

    // Repair cost without any vendor discount (same formula as Player::DurabilityRepair).
    uint32 GetRepairCost(Player const* pPlayer)
    {
        uint32 total = 0;
        auto add = [&total](Item* pItem)
        {
            uint32 const maxDur = pItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            uint32 const curDur = pItem->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (!maxDur || curDur >= maxDur)
                return;

            ItemPrototype const* pProto = pItem->GetProto();
            DurabilityCostsEntry const* pCost = sDurabilityCostsStore.LookupEntry(pProto->ItemLevel);
            DurabilityQualityEntry const* pQuality = sDurabilityQualityStore.LookupEntry((pProto->Quality + 1) * 2);
            if (!pCost || !pQuality)
                return;

            uint32 cost = uint32((maxDur - curDur) * pCost->multiplier[ItemSubClassToDurabilityMultiplierId(pProto->Class, pProto->SubClass)] * pQuality->quality_mod);
            total += cost ? cost : 1;
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                add(pItem);
        ForEachInventoryItem(pPlayer, add);
        return total;
    }

    struct VendorSpawn
    {
        uint32 low;
        ObjectGuid guid;
        float x, y, z;
        uint32 faction;
    };

    // Vendor + repair spawns per map, built once from the creature spawn data.
    std::mutex s_vendorLock;
    std::unordered_map<uint32, std::vector<VendorSpawn>> s_vendorCache;

    std::vector<VendorSpawn> const& GetVendorSpawns(uint32 mapId)
    {
        std::lock_guard<std::mutex> guard(s_vendorLock);
        auto itr = s_vendorCache.find(mapId);
        if (itr != s_vendorCache.end())
            return itr->second;

        std::vector<VendorSpawn>& spawns = s_vendorCache[mapId];
        uint32 const needed = UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR;
        auto worker = [&](CreatureDataPair const& pair) -> bool
        {
            CreatureData const& data = pair.second;
            if (data.position.mapId != mapId)
                return false;
            CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(data.creature_id[0]);
            if (!pInfo || (pInfo->npc_flags & needed) != needed)
                return false;
            spawns.push_back({ pair.first, data.GetObjectGuid(pair.first), data.position.x, data.position.y, data.position.z, pInfo->faction });
            return false;
        };
        sObjectMgr.DoCreatureData(worker);
        return spawns;
    }

    void StopMovement(Player* me)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
    }
}

void PersistentBotAI::TellOwner(char const* text) const
{
    Player* pOwner = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pOwner || !pOwner->IsInWorld() || !pOwner->GetSession())
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, text, LANG_UNIVERSAL, CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName());
    pOwner->GetSession()->SendPacket(&data);
}

void PersistentBotAI::SetGrindRole(bool grind)
{
    PersistentBotFarmState& s = m_farmState;
    if (grind)
    {
        // healers never start fights; a grinding bot has to
        if (m_role == ROLE_HEALER)
        {
            m_role = ROLE_RANGE_DPS;
            s.roleOverridden = true;
        }
    }
    else if (s.roleOverridden)
    {
        s.roleOverridden = false;
        AutoAssignRole();
    }
}

void PersistentBotAI::UpdateAfkState()
{
    PersistentBotFarmState& s = m_farmState;
    bool afk = false;
    if (!s.farming)
    {
        Player* pLeader = ObjectAccessor::FindPlayer(m_leaderGuid);
        afk = pLeader && pLeader->IsInWorld() && pLeader->IsAFK() && pLeader->IsAlive() &&
              pLeader->GetMap() == me->GetMap() && !me->InBattleGround() && GetPartyLeader() == pLeader;
    }

    if (afk == s.afkGrinding)
        return;

    s.afkGrinding = afk;
    s.nextScan = 0;
    if (!afk && !s.farming && !me->IsInCombat())
    {
        // leader is back: drop the grind target and let the follow logic take over
        if (me->GetVictim())
            me->AttackStop();
        StopMovement(me);
    }
}

bool PersistentBotAI::IsValidGrindTarget(Creature* pCreature) const
{
    if (!pCreature->IsInWorld() || !pCreature->IsAlive())
        return false;

    CreatureInfo const* pInfo = pCreature->GetCreatureInfo();
    if (!pInfo || pInfo->type == CREATURE_TYPE_CRITTER)
        return false;

    if (pCreature->IsPet() || pCreature->IsTotem() || pCreature->IsCivilian() || !pCreature->GetOwnerGuid().IsEmpty() ||
        pCreature->GetCharmerGuid() || pCreature->IsInEvadeMode())
        return false;

    uint32 const myLevel = me->GetLevel();
    uint32 const level = pCreature->GetLevel();
    if (level <= MaNGOS::XP::GetGrayLevel(myLevel) || level > myLevel + 2)
        return false;

    if ((pCreature->IsElite() || pCreature->IsWorldBoss()) && myLevel < level + 5)
        return false;

    if (pCreature->HasLootRecipient() && !pCreature->IsTappedBy(me))
        return false;

    if (pCreature->IsFriendlyTo(me) || !IsValidHostileTarget(pCreature))
        return false;

    return true;
}

Unit* PersistentBotAI::SelectGrindTarget()
{
    PersistentBotFarmState& s = m_farmState;
    if (!IsGrinding() || !me->IsAlive() || me->IsInCombat() || me->IsMounted())
        return nullptr;

    if (s.farming && (s.phase != PersistentBotFarmState::PHASE_GRIND || s.townBlocked))
        return nullptr;

    time_t const now = time(nullptr);
    if (now < s.nextScan)
        return nullptr;

    // rest first
    if (NeedsRest())
        return nullptr;

    // stranded far from the anchor: walk back before taking new targets
    if (s.farming)
    {
        float const adx = me->GetPositionX() - s.anchorX;
        float const ady = me->GetPositionY() - s.anchorY;
        float const maxDist = s.radius * STRANDED_RADIUS_MULT;
        if (adx * adx + ady * ady > maxDist * maxDist)
            return nullptr;
    }

    float centerX, centerY, range;
    if (s.farming)
    {
        centerX = s.anchorX;
        centerY = s.anchorY;
        range = s.radius;
    }
    else
    {
        Player* pLeader = ObjectAccessor::FindPlayer(m_leaderGuid);
        if (!pLeader || !pLeader->IsInWorld() || pLeader->IsInCombat() || !me->IsWithinDistInMap(pLeader, AFK_GRIND_RANGE))
            return nullptr;
        centerX = pLeader->GetPositionX();
        centerY = pLeader->GetPositionY();
        range = AFK_GRIND_RANGE;
    }

    std::list<Creature*> found;
    MaNGOS::AnyUnitInObjectRangeCheck check(me, range);
    MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(found, check);
    Cell::VisitGridObjects(me, searcher, range);

    std::vector<Creature*> candidates;
    for (Creature* pCreature : found)
    {
        float const dx = pCreature->GetPositionX() - centerX;
        float const dy = pCreature->GetPositionY() - centerY;
        if (dx * dx + dy * dy > range * range)
            continue;
        if (IsValidGrindTarget(pCreature))
            candidates.push_back(pCreature);
    }

    std::sort(candidates.begin(), candidates.end(), [this](Creature* a, Creature* b)
    {
        return me->GetDistance(a) < me->GetDistance(b);
    });

    for (auto itr = s.unreachable.begin(); itr != s.unreachable.end();)
    {
        if (itr->second <= now)
            itr = s.unreachable.erase(itr);
        else
            ++itr;
    }

    uint32 checked = 0;
    for (Creature* pCreature : candidates)
    {
        if (s.unreachable.find(pCreature->GetObjectGuid()) != s.unreachable.end())
            continue;
        if (++checked > MAX_PATH_CHECKS)
            break;

        PathFinder path(me);
        path.calculate(pCreature->GetPositionX(), pCreature->GetPositionY(), pCreature->GetPositionZ());
        if (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
        {
            s.unreachable[pCreature->GetObjectGuid()] = now + UNREACHABLE_MEMORY;
            continue;
        }

        s.lastTargetSeen = now;
        s.idleNotified = false;
        return pCreature;
    }

    bool const idle = s.farming && now - s.lastTargetSeen > NO_TARGET_IDLE;
    uint32 interval = idle ? IDLE_SCAN_INTERVAL : SCAN_INTERVAL;
    if (checked && interval < UNREACHABLE_BACKOFF)
        interval = UNREACHABLE_BACKOFF; // candidates existed but none could be reached
    s.nextScan = now + interval;
    return nullptr;
}

bool PersistentBotAI::NeedsTown() const
{
    return !CountFreeSlots(me) || GetAverageDurabilityPct(me) < MIN_DURABILITY_PCT;
}

// A trip only makes sense if it can free bag space (something sellable) or pay for the repair.
bool PersistentBotAI::IsTownTripUseful() const
{
    if (!CountFreeSlots(me))
    {
        bool sellable = false;
        ForEachInventoryItem(me, [&](Item* pItem)
        {
            if (!sellable && IsJunk(pItem))
                sellable = true;
        });
        if (sellable)
            return true;
    }

    return GetAverageDurabilityPct(me) < MIN_DURABILITY_PCT && me->GetMoney() >= GetRepairCost(me);
}

bool PersistentBotAI::NeedsRest() const
{
    return me->GetHealthPercent() < REST_HEALTH_PCT ||
           (me->GetPowerType() == POWER_MANA && me->GetPowerPercent(POWER_MANA) < REST_MANA_PCT);
}

Creature* PersistentBotAI::FindVendorInRange() const
{
    Creature* pVendor = me->GetMap()->GetCreature(m_farmState.vendorGuid);
    if (!pVendor || !pVendor->IsAlive() || !pVendor->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR) ||
        pVendor->IsHostileTo(me) || me->GetDistance(pVendor) > VENDOR_NEAR_DIST)
        return nullptr;
    return pVendor;
}

// Nearest friendly vendor + repair spawn on this map, from the creature spawn data (so it works for unloaded grids).
bool PersistentBotAI::FindFarVendor()
{
    PersistentBotFarmState& s = m_farmState;
    FactionTemplateEntry const* pMyFaction = me->GetFactionTemplateEntry();
    if (!pMyFaction)
        return false;

    float const myX = me->GetPositionX();
    float const myY = me->GetPositionY();
    float bestDistSq = VENDOR_SEARCH_RANGE * VENDOR_SEARCH_RANGE;
    VendorSpawn const* pBest = nullptr;

    for (VendorSpawn const& spawn : GetVendorSpawns(me->GetMapId()))
    {
        if (s.badVendors.find(spawn.low) != s.badVendors.end())
            continue;

        float const dx = spawn.x - myX;
        float const dy = spawn.y - myY;
        float const distSq = dx * dx + dy * dy;
        if (distSq >= bestDistSq)
            continue;

        FactionTemplateEntry const* pFaction = sObjectMgr.GetFactionTemplateEntry(spawn.faction);
        if (!pFaction || pMyFaction->IsHostileTo(*pFaction) || !(pFaction->IsFriendlyTo(*pMyFaction) || pMyFaction->IsFriendlyTo(*pFaction)))
            continue;

        bestDistSq = distSq;
        pBest = &spawn;
    }

    if (!pBest)
        return false;

    s.vendorGuid = pBest->guid;
    s.vendorX = pBest->x;
    s.vendorY = pBest->y;
    s.vendorZ = pBest->z;
    return true;
}

void PersistentBotAI::ResetTravel()
{
    PersistentBotFarmState& s = m_farmState;
    s.travelStarted = 0;
    s.travelLastMove = 0;
    s.travelLastDist = 0.0f;
    s.travelStrikes = 0;
}

// Long-distance walk: MovePoint with pathfinding is re-issued every few seconds (partial paths get extended as the
// bot gets closer); gives up after repeated no-progress checks or a global timeout.
uint8 PersistentBotAI::TravelTo(float x, float y, float z, float arriveDist)
{
    PersistentBotFarmState& s = m_farmState;
    time_t const now = time(nullptr);
    float const dist = me->GetDistance(x, y, z);

    if (dist <= arriveDist)
    {
        StopMovement(me);
        ResetTravel();
        return TRAVEL_ARRIVED;
    }

    if (!s.travelStarted)
    {
        ResetTravel();
        s.travelStarted = now;
    }
    else
    {
        // the tick does not run while fighting: do not count that time against the timeout
        if (s.travelLastMove && now - s.travelLastMove > 30)
            s.travelStarted += now - s.travelLastMove;

        if (now - s.travelStarted > TRAVEL_TIMEOUT)
        {
            StopMovement(me);
            ResetTravel();
            return TRAVEL_TIMED_OUT;
        }
    }

    if (!s.travelLastMove || now - s.travelLastMove >= TRAVEL_REISSUE)
    {
        // a long gap means the bot was fighting in between: restart the progress baseline
        if (s.travelLastMove && now - s.travelLastMove <= 30)
        {
            if (dist > s.travelLastDist - 3.0f)
                ++s.travelStrikes;
            else
                s.travelStrikes = 0;

            if (s.travelStrikes >= TRAVEL_MAX_STRIKES)
            {
                StopMovement(me);
                ResetTravel();
                return TRAVEL_FAILED;
            }
        }
        else
            s.travelStrikes = 0;

        s.travelLastMove = now;
        s.travelLastDist = dist;
        if (me->GetStandState() != UNIT_STAND_STATE_STAND)
            me->SetStandState(UNIT_STAND_STATE_STAND);
        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
    }
    return TRAVEL_MOVING;
}

bool PersistentBotAI::UpdateFarm()
{
    PersistentBotFarmState& s = m_farmState;
    time_t const now = time(nullptr);

    UpdateAfkState();
    SetGrindRole(IsGrinding());

    if (!s.farming)
        return false;

    s.deathSince = 0;

    if (me->GetMapId() != s.anchorMap)
    {
        // moved to another map by something else: farm around the new position instead
        s.anchorMap = me->GetMapId();
        s.anchorX = me->GetPositionX();
        s.anchorY = me->GetPositionY();
        s.anchorZ = me->GetPositionZ();
        s.phase = PersistentBotFarmState::PHASE_GRIND;
        ResetTravel();
    }

    // hold position while eating/drinking on a trip
    if (s.phase != PersistentBotFarmState::PHASE_GRIND && NeedsRest())
        return true;

    switch (s.phase)
    {
        case PersistentBotFarmState::PHASE_VENDOR:
        {
            if (m_economyState.vendorStage != PersistentBotEconomyState::VENDOR_IDLE && PersistentBotVendorUpdateTrip(this))
                return true;

            s.phase = PersistentBotFarmState::PHASE_RETURN;
            ResetTravel();
            if (NeedsTown())
            {
                // nothing left to sell or not enough money for the repair: stop grinding instead of looping
                s.townBlocked = true;
                s.nextTripAllowed = now + TOWN_BLOCKED_COOLDOWN;
                if (!s.noVendorNotified)
                {
                    s.noVendorNotified = true;
                    TellOwner("I visited a vendor but my bags are still full or my gear is still worn out (nothing left to sell or not enough money). I will wait at my spot.");
                }
            }
            else
                s.nextTripAllowed = now + TOWN_COOLDOWN;
            return true;
        }
        case PersistentBotFarmState::PHASE_TO_VENDOR:
        {
            if (Creature* pVendor = FindVendorInRange())
            {
                if (PersistentBotVendorStartTrip(this, pVendor->GetObjectGuid()))
                {
                    s.phase = PersistentBotFarmState::PHASE_VENDOR;
                    ResetTravel();
                    return true;
                }
                // the vendor in range (s.vendorGuid) is unusable for this bot
                s.badVendors.insert(s.vendorGuid.GetCounter());
                s.phase = PersistentBotFarmState::PHASE_GRIND;
                s.nextTripAllowed = now + VENDOR_FAIL_COOLDOWN;
                ResetTravel();
                return false;
            }

            uint8 const result = TravelTo(s.vendorX, s.vendorY, s.vendorZ, 3.0f);
            if (result == TRAVEL_MOVING)
                return true;

            // a plain timeout says nothing about the vendor; no progress or arriving without a vendor does
            if (result != TRAVEL_TIMED_OUT)
                s.badVendors.insert(s.vendorGuid.GetCounter());
            s.phase = PersistentBotFarmState::PHASE_GRIND;
            s.nextTripAllowed = now + VENDOR_FAIL_COOLDOWN;
            return false;
        }
        case PersistentBotFarmState::PHASE_RETURN:
        {
            uint8 const result = TravelTo(s.anchorX, s.anchorY, s.anchorZ, ANCHOR_ARRIVE_DIST);
            if (result == TRAVEL_MOVING)
                return true;

            s.phase = PersistentBotFarmState::PHASE_GRIND;
            if (result == TRAVEL_ARRIVED)
                s.returnFailures = 0;
            else if (++s.returnFailures >= MAX_RETURN_FAILURES)
            {
                // the anchor cannot be reached from here: farm around the current position instead
                s.returnFailures = 0;
                s.anchorX = me->GetPositionX();
                s.anchorY = me->GetPositionY();
                s.anchorZ = me->GetPositionZ();
            }
            return false;
        }
        default:
            break;
    }

    // PHASE_GRIND
    if (!NeedsTown())
    {
        s.townBlocked = false;
        s.noVendorNotified = false;
    }
    else if (now >= s.nextTripAllowed)
    {
        if (!IsTownTripUseful())
        {
            s.townBlocked = true;
            s.nextTripAllowed = now + TOWN_BLOCKED_COOLDOWN;
            if (!s.noVendorNotified)
            {
                s.noVendorNotified = true;
                TellOwner("My bags are full or my gear is worn out, but I have nothing to sell and cannot afford the repair. I will wait at my spot.");
            }
        }
        else if (FindFarVendor())
        {
            s.townBlocked = false;
            s.phase = PersistentBotFarmState::PHASE_TO_VENDOR;
            ResetTravel();
            return true;
        }

        s.townBlocked = true;
        s.nextTripAllowed = now + TOWN_RETRY_INTERVAL;
        if (!s.noVendorNotified)
        {
            s.noVendorNotified = true;
            TellOwner("My bags are full or my gear is worn out, but I can't find a friendly repair vendor on this map. I'll wait here.");
        }
    }

    bool const noTargets = now - s.lastTargetSeen > NO_TARGET_IDLE;
    if (noTargets && !s.townBlocked && !s.idleNotified)
    {
        s.idleNotified = true;
        TellOwner("I can't find anything to kill around here, waiting at my spot.");
    }

    bool const idle = s.townBlocked || noTargets;
    float const dx = me->GetPositionX() - s.anchorX;
    float const dy = me->GetPositionY() - s.anchorY;
    float const maxDist = idle ? ANCHOR_ARRIVE_DIST : s.radius;
    if (!me->GetVictim() && dx * dx + dy * dy > maxDist * maxDist)
    {
        s.phase = PersistentBotFarmState::PHASE_RETURN;
        ResetTravel();
        return true;
    }
    return false;
}

void PersistentBotAI::UpdateFarmDeath()
{
    PersistentBotFarmState& s = m_farmState;
    time_t const now = time(nullptr);

    if (!s.deathSince)
    {
        s.deathSince = now;
        m_economyState.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
        s.phase = PersistentBotFarmState::PHASE_GRIND;
        ResetTravel();
        return;
    }

    if (now - s.deathSince < DEATH_WAIT || me->GetDeathState() == JUST_DIED)
        return;

    s.deathSince = 0;
    s.nextScan = now + SCAN_INTERVAL;
    me->ResurrectPlayer(0.5f);
    me->SpawnCorpseBones();
}

void PersistentBotAI::StartFarm(ChatHandler* handler, float radius)
{
    PersistentBotFarmState& s = m_farmState;

    // farming takes over from fishing and gathering
    CancelGather(true);

    SetGrindRole(false);
    s = PersistentBotFarmState();
    s.farming = true;
    s.anchorMap = me->GetMapId();
    s.anchorX = me->GetPositionX();
    s.anchorY = me->GetPositionY();
    s.anchorZ = me->GetPositionZ();
    s.radius = radius;
    s.lastTargetSeen = time(nullptr);
    m_economyState.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
    m_busy = false;

    if (!me->IsInCombat())
        StopMovement(me);

    if (handler)
        handler->PSendSysMessage("%s will farm within %.0f yards of this spot. Use .persistentbot follow to call them back.", me->GetName(), radius);
}

void PersistentBotAI::StopFarm(ChatHandler* handler)
{
    Player* pOwner = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pOwner || !pOwner->IsInWorld())
    {
        if (handler)
            handler->PSendSysMessage("%s's owner is not online.", me->GetName());
        return;
    }

    Group* pGroup = pOwner->GetGroup();
    if (pGroup && pGroup != me->GetGroup() && pGroup->IsFull())
    {
        if (handler)
            handler->PSendSysMessage("Your group is full, %s cannot rejoin it.", me->GetName());
        return;
    }

    if (!pGroup || pGroup != me->GetGroup())
        AddToPlayerGroup();

    SetGrindRole(false);
    m_farmState = PersistentBotFarmState();
    m_economyState.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
    m_busy = false;
    if (!me->IsInCombat())
        StopMovement(me);

    if (handler)
        handler->PSendSysMessage("%s is following you again.", me->GetName());
}

bool PersistentBotAI::HandleFarmCommand(ChatHandler* handler, float radius)
{
    if (!me->IsAlive() || me->InBattleGround() || me->IsBeingTeleported())
    {
        handler->PSendSysMessage("%s cannot start farming right now.", me->GetName());
        return false;
    }

    StartFarm(handler, std::max(20.0f, std::min(150.0f, radius)));
    return true;
}

void PersistentBotAI::HandleFollowCommand(ChatHandler* handler)
{
    if (!m_farmState.farming)
    {
        handler->PSendSysMessage("%s is not in farm mode.", me->GetName());
        return;
    }

    StopFarm(handler);
}
