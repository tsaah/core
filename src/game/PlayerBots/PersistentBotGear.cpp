#include "PersistentBotAI.h"
#include "PersistentBotWeightData.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "Bag.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Packets/Loot.h"
#include "Packets/Chat.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

#include <algorithm>
#include <list>

namespace
{
    float const UPGRADE_MARGIN = 1.05f;
    time_t const LEADER_ROLL_WAIT = 20;
    time_t const LOOT_WALK_TIMEOUT = 20;
    time_t const LOOT_IGNORE_TIME = 180;
    time_t const LOOT_PLAYER_GRACE = 3;
    float const LOOT_SEARCH_RANGE = 30.0f;
    float const LOOT_LEADER_RANGE = 40.0f;
    uint32 const BAGS_ALMOST_FULL_SLOTS = 2;

    Roll* FindRoll(Group* pGroup, ObjectGuid const& target, uint32 slot)
    {
        for (Roll* pRoll : pGroup->GetRolls())
            if (pRoll->isValid() && pRoll->lootedTargetGUID == target && pRoll->itemSlot == slot)
                return pRoll;
        return nullptr;
    }

    bool IsGearProto(ItemPrototype const* pProto)
    {
        return pProto && (pProto->Class == ITEM_CLASS_WEAPON || pProto->Class == ITEM_CLASS_ARMOR);
    }

    void CollectBagItems(Player const* pPlayer, std::vector<Item*>& items)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back(pItem);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            if (Bag* pBag = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot)))
                for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                    if (Item* pItem = pBag->GetItemByPos(slot))
                        items.push_back(pItem);
        }
    }

    uint32 FreeBagSlots(Player const* pPlayer)
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

    bool HasRoomFor(Player const* pPlayer, uint32 itemId)
    {
        ItemPosCountVec dest;
        return pPlayer->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) == EQUIP_ERR_OK;
    }

    void SayInParty(Player* pPlayer, std::string const& text)
    {
        auto msg = std::make_unique<WorldPackets::Chat::ChatMessage>();
        msg->type = CHAT_MSG_PARTY;
        msg->lang = pPlayer->GetTeam() == HORDE ? LANG_ORCISH : LANG_COMMON;
        msg->message = text;
        pPlayer->GetSession()->QueuePacket(std::move(msg));
    }

    // Bags only compete with bags of their own kind: normal bags, each special container subclass, quivers and pouches
    bool SameBagKind(ItemPrototype const* pProto, ItemPrototype const* pBagProto)
    {
        if (pProto->Class != pBagProto->Class)
            return false;
        return pProto->Class == ITEM_CLASS_QUIVER || pProto->SubClass == pBagProto->SubClass;
    }

    bool IsNormalBag(ItemPrototype const* pProto)
    {
        return pProto->Class == ITEM_CLASS_CONTAINER && pProto->SubClass == ITEM_SUBCLASS_CONTAINER;
    }

    // Special bags only for what the bot stores in them: soul shards, its profession supplies, its ranged ammo
    bool CanUseBag(Player const* pPlayer, ItemPrototype const* pProto)
    {
        if (pProto->Class == ITEM_CLASS_QUIVER)
        {
            Item* pRanged = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
            if (pPlayer->GetClass() != CLASS_HUNTER || !pRanged)
                return false;
            uint32 const ammo = pRanged->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_GUN ? BAG_FAMILY_BULLETS : BAG_FAMILY_ARROWS;
            return pProto->BagFamily == ammo;
        }

        if (pProto->Class != ITEM_CLASS_CONTAINER)
            return false;

        switch (pProto->SubClass)
        {
            case ITEM_SUBCLASS_CONTAINER:                return true;
            case ITEM_SUBCLASS_SOUL_CONTAINER:           return pPlayer->GetClass() == CLASS_WARLOCK;
            case ITEM_SUBCLASS_HERB_CONTAINER:           return pPlayer->HasSkill(SKILL_HERBALISM);
            case ITEM_SUBCLASS_ENCHANTING_CONTAINER:     return pPlayer->HasSkill(SKILL_ENCHANTING);
            default:                                     return false;
        }
    }

    // A normal bag is surplus once every bag slot is taken and it is no bigger than the smallest equipped normal bag
    bool IsSurplusBag(Player const* pPlayer, ItemPrototype const* pProto)
    {
        uint32 smallest = 0;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Item* pBag = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
            if (!pBag)
                return false;
            if (IsNormalBag(pBag->GetProto()) && (!smallest || pBag->GetProto()->ContainerSlots < smallest))
                smallest = pBag->GetProto()->ContainerSlots;
        }
        return !smallest || pProto->ContainerSlots <= smallest;
    }

    // Bag slot a bag goes into: a quiver/pouch takes the place of the equipped one, otherwise an empty slot,
    // otherwise the smallest equipped bag of its kind (the caller checks that the new one is bigger). NULL_SLOT if none.
    uint8 PickBagSlot(Player const* pPlayer, ItemPrototype const* pProto)
    {
        uint8 empty = NULL_SLOT;
        uint8 smallest = NULL_SLOT;
        uint32 smallestSlots = 0;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Item* pBag = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
            if (!pBag)
            {
                if (empty == NULL_SLOT)
                    empty = bagSlot;
            }
            else if (SameBagKind(pProto, pBag->GetProto()) && (smallest == NULL_SLOT || pBag->GetProto()->ContainerSlots < smallestSlots))
            {
                smallest = bagSlot;
                smallestSlots = pBag->GetProto()->ContainerSlots;
            }
        }

        // only one quiver or pouch can be equipped, so a new one can only go where the equipped one is
        if (pProto->Class == ITEM_CLASS_QUIVER && smallest != NULL_SLOT)
            return smallest;
        return empty != NULL_SLOT ? empty : smallest;
    }

    // A free slot outside the given bag for one of its items: backpack first, then other bags that take the item
    bool FindSlotOutside(Player const* pPlayer, Bag const* pBag, Item const* pItem, uint16& dest)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (!pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                dest = (INVENTORY_SLOT_BAG_0 << 8) | slot;
                return true;
            }
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pOther = static_cast<Bag*>(pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pOther || pOther == pBag || !ItemCanGoIntoBag(pItem->GetProto(), pOther->GetProto()))
                continue;

            for (uint32 slot = 0; slot < pOther->GetBagSize(); ++slot)
            {
                if (!pOther->GetItemByPos(slot))
                {
                    dest = (bagSlot << 8) | slot;
                    return true;
                }
            }
        }
        return false;
    }

    // One step towards equipping a bag: into an empty bag slot, or over a smaller bag of its kind once that bag is empty
    // (its items leave one per call, into free space outside it). Returns true when this call used the tick.
    bool TryEquipBag(Player* pPlayer, Item* pBag)
    {
        ItemPrototype const* pProto = pBag->GetProto();
        uint8 bagSlot = PickBagSlot(pPlayer, pProto);
        if (bagSlot == NULL_SLOT)
            return false;

        Item* pOld = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (pOld && pOld->GetProto()->ContainerSlots >= pProto->ContainerSlots)
            return false;

        uint16 dest = 0;
        if (pPlayer->CanEquipItem(bagSlot, dest, pBag, true) != EQUIP_ERR_OK)
            return false;

        if (pOld && !static_cast<Bag*>(pOld)->IsEmpty())
        {
            // the server only takes an empty bag out of its slot
            Bag* pOldBag = static_cast<Bag*>(pOld);
            uint32 const items = pOldBag->GetBagSize() - pOldBag->GetFreeSlots();
            if (FreeBagSlots(pPlayer) - pOldBag->GetFreeSlots() < items)
                return false;

            Item* pItem = nullptr;
            for (uint32 slot = 0; slot < pOldBag->GetBagSize() && !pItem; ++slot)
                pItem = pOldBag->GetItemByPos(slot);

            uint16 target = 0;
            if (!FindSlotOutside(pPlayer, pOldBag, pItem, target))
                return false;

            uint16 const src = pItem->GetPos();
            pPlayer->SwapItem(src, target);
            return pItem->GetPos() != src;
        }

        pPlayer->SwapItem(pBag->GetPos(), dest);
        if (pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot) != pBag)
            return false;

        if (pPlayer->GetGroup())
            SayInParty(pPlayer, "Equipped " + std::string(pProto->Name1) + " (" + std::to_string(pProto->ContainerSlots) + " slots).");
        return true;
    }

    void SendRollVote(Player* pPlayer, ObjectGuid const& target, uint32 slot, RollVote vote)
    {
        auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
        data->lootedTarget = target;
        data->itemSlot = slot;
        data->rollType = vote;
        pPlayer->GetSession()->QueuePacket(std::move(data));
    }

    float ScoreAuras(float const* w, SpellEntry const* pSpell)
    {
        float score = 0.0f;
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (pSpell->Effect[i] != SPELL_EFFECT_APPLY_AURA)
                continue;

            float value = float(pSpell->EffectBasePoints[i] + 1);
            int32 misc = pSpell->EffectMiscValue[i];
            switch (pSpell->EffectApplyAuraName[i])
            {
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    if (misc & ~int32(SPELL_SCHOOL_MASK_NORMAL))
                        score += value * w[PBW_SP];
                    break;
                case SPELL_AURA_MOD_HEALING_DONE:
                case SPELL_AURA_MOD_HEALING:
                    score += value * w[PBW_HEAL];
                    break;
                case SPELL_AURA_MOD_ATTACK_POWER:
                    score += value * w[PBW_AP];
                    break;
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                    score += value * w[PBW_RAP];
                    break;
                case SPELL_AURA_MOD_HIT_CHANCE:
                    score += value * w[PBW_HIT];
                    break;
                case SPELL_AURA_MOD_SPELL_HIT_CHANCE:
                    score += value * w[PBW_SPELL_HIT];
                    break;
                case SPELL_AURA_MOD_CRIT_PERCENT:
                    score += value * w[PBW_CRIT];
                    break;
                case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                    score += value * w[PBW_SPELL_CRIT];
                    break;
                case SPELL_AURA_MOD_POWER_REGEN:
                    if (misc == POWER_MANA)
                        score += value * w[PBW_MP5];
                    break;
                case SPELL_AURA_MOD_SKILL:
                    if (misc == SKILL_DEFENSE)
                        score += value * w[PBW_DEFENSE];
                    break;
                case SPELL_AURA_MOD_SHIELD_BLOCKVALUE:
                    score += value * w[PBW_BLOCK];
                    break;
            }
        }
        return score;
    }

    float SlotScore(PersistentBotAI const* pAI, Player const* pPlayer, uint8 slot)
    {
        Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return pItem ? pAI->ScoreItem(pItem->GetProto()) : 0.0f;
    }

    // Picks the slot the item would go into and the score it has to beat there
    // (weaker ring/trinket/weapon slot, or MH+OH sum for two-handers).
    uint8 PickSlot(PersistentBotAI const* pAI, Player const* pPlayer, ItemPrototype const* pProto, float& current)
    {
        uint8 slots[4];
        pProto->GetAllowedEquipSlots(slots, pPlayer->GetClass(), pPlayer->CanDualWield());

        if (pProto->InventoryType == INVTYPE_2HWEAPON)
        {
            current = SlotScore(pAI, pPlayer, EQUIPMENT_SLOT_MAINHAND) + SlotScore(pAI, pPlayer, EQUIPMENT_SLOT_OFFHAND);
            return slots[0];
        }

        Item* pMainHand = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool mainHandIs2H = pMainHand && pMainHand->GetProto()->InventoryType == INVTYPE_2HWEAPON;

        uint8 best = NULL_SLOT;
        for (uint8 slot : slots)
        {
            if (slot == NULL_SLOT)
                continue;

            if (slot == EQUIPMENT_SLOT_OFFHAND)
            {
                if (mainHandIs2H)
                    continue;

                // a dual wield weapon never replaces a shield or held item
                if (pProto->Class == ITEM_CLASS_WEAPON)
                {
                    if (!pPlayer->CanDualWield())
                        continue;

                    Item* pOffHand = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                    if (pOffHand && pOffHand->GetProto()->Class != ITEM_CLASS_WEAPON)
                        continue;
                }
            }

            float score = SlotScore(pAI, pPlayer, slot);
            if (best == NULL_SLOT || score < current)
            {
                best = slot;
                current = score;
            }
        }
        return best;
    }

    // Shared (non personal) loot of a corpse is only the bot's turn under the group's loot method
    bool MaySharedLoot(Player* pPlayer, Loot const& loot)
    {
        Group* pGroup = pPlayer->GetGroup();
        if (!pGroup)
            return true;

        switch (pGroup->GetLootMethod())
        {
            case FREE_FOR_ALL:
                return true;
            case ROUND_ROBIN:
            case GROUP_LOOT:
            case NEED_BEFORE_GREED:
                // the round robin looter's turn, or anyone's once it has released the corpse
                return !loot.roundRobinPlayer || loot.roundRobinPlayer == pPlayer->GetObjectGuid().GetRawValue();
            default:
                // master loot: shared items are the master looter's to hand out, bots never take them
                return false;
        }
    }

    // Whether the bot should ask for this loot slot of a corpse it opened.
    // Quest items are only listed for their owner; free-for-all and conditional items are per player;
    // everything else is shared and follows MaySharedLoot.
    bool MayTakeSlot(Player* pPlayer, Loot const& loot, uint32 slot)
    {
        if (slot >= loot.items.size())
            return true;

        LootItem const& item = loot.items[slot];
        if (item.freeforall || item.conditionId)
            return !item.is_looted && item.AllowedForPlayer(pPlayer, loot.GetLootTarget());

        if (item.is_looted || item.is_blocked || !MaySharedLoot(pPlayer, loot))
            return false;

        // above-threshold items under group/need-before-greed go to the roll system, not to a looter
        Group* pGroup = pPlayer->GetGroup();
        bool const rolled = pGroup && (pGroup->GetLootMethod() == GROUP_LOOT || pGroup->GetLootMethod() == NEED_BEFORE_GREED);
        return item.is_underthreshold || !rolled;
    }

    struct LootableCreatureCheck
    {
        LootableCreatureCheck(Player* pPlayer, std::map<ObjectGuid, time_t> const& done, std::map<ObjectGuid, time_t>& seen, time_t now, uint32 freeSlots)
            : m_player(pPlayer), m_done(done), m_seen(seen), m_now(now), m_freeSlots(freeSlots) {}

        bool operator()(Unit* pUnit)
        {
            Creature* pCreature = static_cast<Creature*>(pUnit);
            if (pCreature->IsAlive() || !pCreature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) || pCreature->lootForSkin)
                return false;
            if (m_done.find(pCreature->GetObjectGuid()) != m_done.end())
                return false;
            if (!m_player->IsWithinDistInMap(pCreature, LOOT_SEARCH_RANGE))
                return false;

            // the first time a corpse is seen lootable starts its player grace period
            time_t const seenAt = m_seen.emplace(pCreature->GetObjectGuid(), m_now).first->second;

            Loot const& loot = pCreature->loot;
            if (!m_player->IsAllowedToLoot(pCreature) || loot.HasPlayersLooting())
                return false;
            bool const botsTurn = loot.roundRobinPlayer == m_player->GetObjectGuid().GetRawValue();
            if (m_player->GetGroup() && !botsTurn && m_now - seenAt < LOOT_PLAYER_GRACE)
                return false;

            if (loot.gold && MaySharedLoot(m_player, loot))
                return true;
            if (loot.hasItemFor(m_player))
                return true;

            if (!m_freeSlots)
                return false;
            for (uint32 slot = 0; slot < loot.items.size(); ++slot)
                if (MayTakeSlot(m_player, loot, slot))
                    return true;
            return false;
        }

        Player* m_player;
        std::map<ObjectGuid, time_t> const& m_done;
        std::map<ObjectGuid, time_t>& m_seen;
        time_t m_now;
        uint32 m_freeSlots;
    };

    struct OwnedCorpseCheck
    {
        explicit OwnedCorpseCheck(Player* pPlayer) : m_player(pPlayer) {}

        bool operator()(Creature* pCreature)
        {
            return !pCreature->IsAlive() && pCreature->loot.roundRobinPlayer == m_player->GetObjectGuid().GetRawValue();
        }

        Player* m_player;
    };
}

float PersistentBotAI::ScoreItem(ItemPrototype const* pProto) const
{
    if (!IsGearProto(pProto) || me->CanUseItem(pProto) != EQUIP_ERR_OK)
        return 0.0f;

    uint8 tab = DetectSpecTab();
    PersistentBotStatWeights const* pWeights = nullptr;
    for (PersistentBotStatWeights const& entry : g_persistentBotStatWeights)
    {
        if (entry.classId != me->GetClass())
            continue;
        if (!pWeights || entry.tab == tab)
            pWeights = &entry;
    }

    float w[PBW_MAX] = {};
    if (pWeights)
        for (uint32 i = 0; i < PBW_MAX; ++i)
            w[i] = pWeights->w[i];

    float primary = std::max(std::max(w[PBW_STR], w[PBW_AGI]), std::max(w[PBW_INT], w[PBW_SPI]));
    if (w[PBW_STA] <= 0.0f)
        w[PBW_STA] = 0.3f * (primary > 0.0f ? primary : 1.0f);
    if (w[PBW_ARMOR] <= 0.0f)
        w[PBW_ARMOR] = 0.01f;
    if (w[PBW_MELEE_DPS] <= 0.0f)
        w[PBW_MELEE_DPS] = 0.1f;
    if (w[PBW_RANGED_DPS] <= 0.0f)
        w[PBW_RANGED_DPS] = (w[PBW_SP] > 0.0f || w[PBW_HEAL] > 0.0f) ? 1.0f : 0.3f;

    float score = 0.0f;
    for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        float value = float(pProto->ItemStat[i].ItemStatValue);
        switch (pProto->ItemStat[i].ItemStatType)
        {
            case ITEM_MOD_AGILITY:   score += value * w[PBW_AGI]; break;
            case ITEM_MOD_STRENGTH:  score += value * w[PBW_STR]; break;
            case ITEM_MOD_INTELLECT: score += value * w[PBW_INT]; break;
            case ITEM_MOD_SPIRIT:    score += value * w[PBW_SPI]; break;
            case ITEM_MOD_STAMINA:   score += value * w[PBW_STA]; break;
        }
    }

    score += float(pProto->Armor) * w[PBW_ARMOR];
    score += float(pProto->Block) * w[PBW_BLOCK];

    if (pProto->Class == ITEM_CLASS_WEAPON && pProto->Delay && pProto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
    {
        float damage = 0.0f;
        for (_ItemDamage const& dmg : pProto->Damage)
            damage += (dmg.DamageMin + dmg.DamageMax) * 0.5f;

        float dps = damage * 1000.0f / float(pProto->Delay);
        switch (pProto->InventoryType)
        {
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_THROWN:
                score += dps * w[PBW_RANGED_DPS];
                break;
            default:
                score += dps * w[PBW_MELEE_DPS];
                break;
        }
    }

    for (_ItemSpell const& itemSpell : pProto->Spells)
    {
        if (!itemSpell.SpellId || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_EQUIP)
            continue;
        if (SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(itemSpell.SpellId))
            score += ScoreAuras(w, pSpell);
    }

    return score > 0.0f ? score : 0.0f;
}

bool PersistentBotAI::IsUpgrade(ItemPrototype const* pProto) const
{
    // a pole or a profession tool has no score; it is kept in the bags for its gathering spell
    if (!IsGearProto(pProto) || IsGatherTool(pProto))
        return false;

    float score = ScoreItem(pProto);
    if (score <= 0.0f)
        return false;

    float current = 0.0f;
    uint8 slot = PickSlot(this, me, pProto, current);
    if (slot == NULL_SLOT)
        return false;

    uint16 dest = 0;
    switch (me->CanEquipNewItem(slot, dest, pProto->ItemId, true))
    {
        case EQUIP_ERR_CANT_EQUIP_SKILL:
        case EQUIP_ERR_NO_REQUIRED_PROFICIENCY:
        case EQUIP_ERR_CANT_DUAL_WIELD:
        case EQUIP_ERR_CANT_CARRY_MORE_OF_THIS:
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM:
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM2:
        case EQUIP_ERR_ITEM_CANT_BE_EQUIPPED:
        case EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT:
            return false;
        default:
            break;
    }

    return score > current * UPGRADE_MARGIN;
}

bool PersistentBotAI::IsJunk(Item const* pItem) const
{
    ItemPrototype const* pProto = pItem->GetProto();
    if (!pProto->SellPrice || pItem->IsEquipped())
        return false;

    // mining pick, skinning knife, fishing poles, hammers: never sold, in any mode
    if (IsGatherTool(pProto))
        return false;

    if (pProto->Class == ITEM_CLASS_QUEST || pProto->Class == ITEM_CLASS_PROJECTILE || pProto->StartQuest || me->HasQuestForItem(pProto->ItemId))
        return false;

    // soul shards are kept for Drain Soul (see PersistentBotAI::UpdateInCombatAI_Warlock), whether or not a known spell lists them
    if (pProto->ItemId == SOUL_SHARD_ITEM_ID)
        return false;

    // farm mode sells every vendorable item it has no use for, not just grey items and outdated gear
    bool const farming = m_farmState.farming;
    if (farming && (pProto->Class == ITEM_CLASS_CONSUMABLE || pProto->Class == ITEM_CLASS_CONTAINER || pProto->Class == ITEM_CLASS_KEY) &&
        pProto->Quality != ITEM_QUALITY_POOR)
        return false;

    if (farming || pProto->Quality == ITEM_QUALITY_POOR)
    {
        // totems and reagents of known spells
        for (auto const& spell : me->GetSpellMap())
        {
            if (spell.second.state == PLAYERSPELL_REMOVED)
                continue;
            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spell.first);
            if (!pSpell)
                continue;
            for (int32 reagent : pSpell->Reagent)
                if (reagent == int32(pProto->ItemId))
                    return false;
            for (uint32 totem : pSpell->Totem)
                if (totem == pProto->ItemId)
                    return false;
        }
        if (pProto->Quality == ITEM_QUALITY_POOR)
            return true;
    }

    // normal bags are kept only while one would still be equipped over the smallest equipped bag
    if (IsNormalBag(pProto))
        return IsSurplusBag(me, pProto);

    if (!IsGearProto(pProto))
        return farming;

    // keep what the bot will grow into
    if (me->CanUseItem(pProto) == EQUIP_ERR_CANT_EQUIP_LEVEL_I && pProto->Quality >= ITEM_QUALITY_UNCOMMON &&
        pProto->RequiredLevel <= me->GetLevel() + 5)
        return false;

    if (IsUpgrade(pProto))
        return false;

    if (pItem->IsSoulBound())
    {
        float current = 0.0f;
        if (PickSlot(this, me, pProto, current) != NULL_SLOT && ScoreItem(pProto) >= current)
            return false;
    }

    return true;
}

void PersistentBotAI::OnLootRollStart(WorldPacket const* packet)
{
    if (packet->size() < 16)
        return;

    uint8 const* data = packet->contents();
    uint64 guid;
    uint32 slot;
    uint32 itemId;
    memcpy(&guid, data, 8);
    memcpy(&slot, data + 8, 4);
    memcpy(&itemId, data + 12, 4);

    ObjectGuid target(guid);
    ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
    if (!pProto)
    {
        SendRollVote(me, target, slot, ROLL_PASS);
        return;
    }

    if (IsUpgrade(pProto) && HasRoomFor(me, itemId))
    {
        Player* pLeader = GetPartyLeader();
        Group* pGroup = me->GetGroup();
        if (pLeader && pGroup && pLeader->CanUseItem(pProto) == EQUIP_ERR_OK)
        {
            Roll* pRoll = FindRoll(pGroup, target, slot);
            if (pRoll)
            {
                auto itr = pRoll->playerVote.find(pLeader->GetObjectGuid());
                if (itr != pRoll->playerVote.end() && itr->second == ROLL_NOT_EMITED_YET)
                {
                    static char const* const qualityColors[] = { "9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000" };
                    char text[256];
                    snprintf(text, sizeof(text), "I could use |cff%s|Hitem:%u:0:0:0|h[%s]|h|r - roll Need if you want it.",
                        qualityColors[std::min<uint32>(pProto->Quality, 5)], itemId, pProto->Name1);
                    SayInParty(me, text);

                    PersistentBotPendingRoll pending;
                    pending.lootedTarget = target;
                    pending.itemSlot = slot;
                    pending.itemId = itemId;
                    pending.since = time(nullptr);
                    m_gearState.pendingRolls.push_back(pending);
                    return;
                }
            }
        }

        SendRollVote(me, target, slot, ROLL_NEED);
        return;
    }

    if (pProto->SellPrice && HasRoomFor(me, itemId))
        SendRollVote(me, target, slot, ROLL_GREED);
    else
        SendRollVote(me, target, slot, ROLL_PASS);
}

// Party warnings as the bags fill: "almost full" at BAGS_ALMOST_FULL_SLOTS or fewer free, "full" at none.
// Each is said once and re-armed only after free slots rise above that threshold again.
void PersistentBotAI::UpdateBagStatus()
{
    if (!me->GetGroup())
        return;

    PersistentBotGearState& state = m_gearState;
    uint32 const free = FreeBagSlots(me);

    if (free > BAGS_ALMOST_FULL_SLOTS)
        state.bagsAlmostFullSaid = false;
    if (free > 0)
        state.bagsFullSaid = false;

    if (free == 0)
    {
        if (!state.bagsFullSaid)
            SayInParty(me, "My bags are full.");
        state.bagsFullSaid = true;
        state.bagsAlmostFullSaid = true;
    }
    else if (free <= BAGS_ALMOST_FULL_SLOTS && !state.bagsAlmostFullSaid)
    {
        state.bagsAlmostFullSaid = true;
        SayInParty(me, "My bags are almost full (" + std::to_string(free) + " slots left).");
    }
}

void PersistentBotAI::UpdateLootRolls()
{
    // runs every out-of-combat tick; UpdateLooting is skipped while the farm tick is busy
    UpdateBagStatus();

    auto& pending = m_gearState.pendingRolls;
    if (pending.empty())
        return;

    Group* pGroup = me->GetGroup();
    Player* pLeader = GetPartyLeader();
    time_t now = time(nullptr);

    for (auto itr = pending.begin(); itr != pending.end();)
    {
        Roll* pRoll = pGroup ? FindRoll(pGroup, itr->lootedTarget, itr->itemSlot) : nullptr;
        if (!pRoll)
        {
            itr = pending.erase(itr);
            continue;
        }

        auto myVote = pRoll->playerVote.find(me->GetObjectGuid());
        if (myVote == pRoll->playerVote.end() || myVote->second != ROLL_NOT_EMITED_YET)
        {
            itr = pending.erase(itr);
            continue;
        }

        RollVote leaderVote = ROLL_PASS;
        if (pLeader)
        {
            auto leaderItr = pRoll->playerVote.find(pLeader->GetObjectGuid());
            if (leaderItr != pRoll->playerVote.end())
                leaderVote = leaderItr->second;
        }

        if (leaderVote == ROLL_NOT_EMITED_YET && now - itr->since < LEADER_ROLL_WAIT)
        {
            ++itr;
            continue;
        }

        if (leaderVote == ROLL_NEED)
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itr->itemId);
            bool greed = pProto && pProto->SellPrice && HasRoomFor(me, itr->itemId);
            SendRollVote(me, itr->lootedTarget, itr->itemSlot, greed ? ROLL_GREED : ROLL_PASS);
        }
        else
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itr->itemId);
            RollVote vote = ROLL_PASS;
            if (pProto && IsUpgrade(pProto) && HasRoomFor(me, itr->itemId))
                vote = ROLL_NEED;
            else if (pProto && pProto->SellPrice && HasRoomFor(me, itr->itemId))
                vote = ROLL_GREED;
            SendRollVote(me, itr->lootedTarget, itr->itemSlot, vote);
        }

        itr = pending.erase(itr);
    }
}

bool PersistentBotAI::UpdateLooting()
{
    PersistentBotGearState& state = m_gearState;
    time_t now = time(nullptr);

    for (auto itr = state.lootDone.begin(); itr != state.lootDone.end();)
    {
        if (now - itr->second > LOOT_IGNORE_TIME)
            itr = state.lootDone.erase(itr);
        else
            ++itr;
    }

    // a corpse pruned here while it is still the walk target would restart its grace period
    for (auto itr = state.lootSeen.begin(); itr != state.lootSeen.end();)
    {
        if (now - itr->second > LOOT_IGNORE_TIME && itr->first != state.lootTarget)
            itr = state.lootSeen.erase(itr);
        else
            ++itr;
    }

    uint32 freeSlots = FreeBagSlots(me);

    // a bot that can't take the rest of a corpse gives up its round robin turn so the party can loot it
    {
        std::list<Creature*> owned;
        CellPair pair(MaNGOS::ComputeCellPair(me->GetPositionX(), me->GetPositionY()));
        Cell cell(pair);
        cell.SetNoCreate();
        OwnedCorpseCheck ownedCheck(me);
        MaNGOS::CreatureListSearcher<OwnedCorpseCheck> ownedSearcher(owned, ownedCheck);
        TypeContainerVisitor<MaNGOS::CreatureListSearcher<OwnedCorpseCheck>, GridTypeMapContainer> ownedVisitor(ownedSearcher);
        cell.Visit(pair, ownedVisitor, *me->GetMap(), *me, LOOT_SEARCH_RANGE * 2);
        for (Creature* pCorpse : owned)
        {
            if (pCorpse->loot.gold || pCorpse->loot.HasPlayersLooting())
                continue;
            if (freeSlots && state.lootDone.find(pCorpse->GetObjectGuid()) == state.lootDone.end())
                continue;
            pCorpse->loot.roundRobinPlayer = 0;
            pCorpse->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
        }
    }

    Player* pLeader = GetPartyLeader();
    if (!m_farmState.farming && (!pLeader || !pLeader->IsInWorld() || pLeader->GetMapId() != me->GetMapId() || me->GetDistance(pLeader) > LOOT_LEADER_RANGE))
    {
        state.lootTarget = ObjectGuid();
        return false;
    }

    if (me->GetLootGuid())
    {
        auto release = std::make_unique<WorldPackets::Loot::LootRelease>();
        release->guid = me->GetLootGuid();
        me->GetSession()->QueuePacket(std::move(release));
        return true;
    }

    LootableCreatureCheck check(me, state.lootDone, state.lootSeen, now, freeSlots);

    Creature* pTarget = nullptr;
    if (state.lootTarget)
    {
        pTarget = me->GetMap()->GetCreature(state.lootTarget);
        if (pTarget && pTarget->loot.HasPlayersLooting())
        {
            // a player opened it while the bot was walking over: back off and retry later, not marked done
            state.lootTarget = ObjectGuid();
            pTarget = nullptr;
        }
        else if (!pTarget || now - state.lootStarted > LOOT_WALK_TIMEOUT || !check(pTarget))
        {
            if (pTarget)
                state.lootDone[state.lootTarget] = now;
            state.lootTarget = ObjectGuid();
            pTarget = nullptr;
        }
    }

    if (!pTarget)
    {
        std::list<Creature*> candidates;
        CellPair pair(MaNGOS::ComputeCellPair(me->GetPositionX(), me->GetPositionY()));
        Cell cell(pair);
        cell.SetNoCreate();
        MaNGOS::CreatureListSearcher<LootableCreatureCheck> searcher(candidates, check);
        TypeContainerVisitor<MaNGOS::CreatureListSearcher<LootableCreatureCheck>, GridTypeMapContainer> visitor(searcher);
        cell.Visit(pair, visitor, *me->GetMap(), *me, LOOT_SEARCH_RANGE);

        float bestDist = 0.0f;
        for (Creature* pCreature : candidates)
        {
            float dist = me->GetDistance(pCreature);
            if (!pTarget || dist < bestDist)
            {
                pTarget = pCreature;
                bestDist = dist;
            }
        }

        if (!pTarget)
            return false;

        state.lootTarget = pTarget->GetObjectGuid();
        state.lootStarted = now;
        state.lootMoveIssued = 0;
        state.lootMoveDist = 0.0f;
    }

    if (!pTarget->IsWithinDistInMap(me, me->GetMaxLootDistance(pTarget), true, SizeFactor::None))
    {
        if (now - state.lootMoveIssued >= 3)
        {
            float const dist = me->GetDistance(pTarget);
            if (state.lootMoveIssued && dist > state.lootMoveDist - 1.0f)
            {
                state.lootDone[state.lootTarget] = now;
                state.lootTarget = ObjectGuid();
                me->StopMoving();
                return false;
            }
            state.lootMoveIssued = now;
            state.lootMoveDist = dist;
            me->GetMotionMaster()->MovePoint(0, pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ(),
                MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
        }
        return true;
    }

    me->StopMoving();

    ObjectGuid guid = pTarget->GetObjectGuid();
    WorldSession* pSession = me->GetSession();

    auto open = std::make_unique<WorldPackets::Loot::LootUnit>();
    open->guid = guid;
    pSession->QueuePacket(std::move(open));

    Loot const& loot = pTarget->loot;
    uint32 slots = loot.GetMaxSlotInLootFor(me->GetGUIDLow());
    for (uint32 slot = 0; slot < slots; ++slot)
    {
        // with full bags only the quest item slots (after the regular items) are still worth a try
        if (!freeSlots && slot < loot.items.size())
            continue;
        if (!MayTakeSlot(me, loot, slot))
            continue;

        auto item = std::make_unique<WorldPackets::Loot::AutoStoreLootItem>();
        item->lootSlot = slot;
        pSession->QueuePacket(std::move(item));
    }

    if (loot.gold && MaySharedLoot(me, loot))
        pSession->QueuePacket(std::make_unique<NullClientPacket>(CMSG_LOOT_MONEY));

    auto release = std::make_unique<WorldPackets::Loot::LootRelease>();
    release->guid = guid;
    pSession->QueuePacket(std::move(release));

    state.lootDone[guid] = now;
    state.lootTarget = ObjectGuid();
    return true;
}

void PersistentBotAI::UpdateEquipment()
{
    // while fishing the main hand holds the pole, which must not be swapped out
    if (me->IsInCombat() || !me->IsAlive() || me->IsNonMeleeSpellCasted(false) || m_gatherState.fishing)
        return;

    // not while a loot window, a trade or a vendor trip is open
    if (me->GetLootGuid() || me->GetTradeData() || m_economyState.vendorStage != PersistentBotEconomyState::VENDOR_IDLE)
        return;

    std::vector<Item*> items;
    CollectBagItems(me, items);

    for (Item* pItem : items)
    {
        ItemPrototype const* pProto = pItem->GetProto();
        if (!IsUpgrade(pProto))
            continue;

        float current = 0.0f;
        uint8 slot = PickSlot(this, me, pProto, current);
        uint16 dest = 0;
        if (slot == NULL_SLOT || me->CanEquipItem(slot, dest, pItem, true) != EQUIP_ERR_OK)
            continue;

        me->SwapItem(pItem->GetPos(), dest);
        return;
    }

    // biggest bags first; one that cannot be equipped yet falls through to the next
    std::vector<Item*> bags;
    for (Item* pItem : items)
        if (pItem->GetProto()->InventoryType == INVTYPE_BAG && CanUseBag(me, pItem->GetProto()))
            bags.push_back(pItem);

    std::sort(bags.begin(), bags.end(), [](Item const* a, Item const* b)
    {
        return a->GetProto()->ContainerSlots > b->GetProto()->ContainerSlots;
    });

    for (Item* pBag : bags)
        if (TryEquipBag(me, pBag))
            return;
}
