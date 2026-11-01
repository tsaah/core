#include "PersistentBotAI.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "Pet.h"
#include "Item.h"
#include "Bag.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "World.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "ReputationMgr.h"
#include "CharacterDatabaseCache.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridSearchers.h"
#include "Server/Packets/Item.h"
#include "Server/Packets/Spell.h"

#include <algorithm>
#include <ctime>

namespace
{
    constexpr float NPC_SEARCH_RANGE = 30.0f;
    constexpr float VENDOR_TRADE_DIST = 4.0f;
    constexpr uint32 SPELL_TAME_BEAST_KNOWN = 1515;
    constexpr uint32 SPELL_TAME_BEAST_CAST = 13481;
    constexpr uint32 AMMO_TARGET = 1000;
    constexpr uint32 AMMO_REBUY_BELOW = 500;
    constexpr uint32 REAGENT_STACK_MULT = 10;
    constexpr uint32 TRAINER_SCAN_INTERVAL = 10;
    constexpr uint32 MAX_TRAINER_PASSES = 12;
    constexpr uint32 VENDOR_TRIP_TIMEOUT = 45;
    constexpr uint32 MAX_SELLS_PER_TRIP = 60;
    constexpr time_t GRIMOIRE_USE_INTERVAL = 5;

    #define BOT_SKILL_INDEX(x) (PLAYER_SKILL_INFO_1_1 + ((x) * 3))

    std::string FormatMoney(uint32 copper)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%ug %us %uc", copper / 10000, (copper / 100) % 100, copper % 100);
        return buf;
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

    std::string GetSpellNameFor(Player* me, uint32 spellId)
    {
        SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId);
        if (!pSpell)
            return "?";
        std::string const& name = pSpell->SpellName[me->GetSession()->GetSessionDbcLocale()];
        return name.empty() ? pSpell->SpellName[0] : name;
    }

    // CanInteractWithNPC without the distance check.
    bool CanUseNpc(Player* me, Creature* pCreature, uint32 npcFlagMask)
    {
        if (!pCreature || !pCreature->IsAlive() || pCreature->IsInCombat() || pCreature->GetCharmerGuid())
            return false;
        if (npcFlagMask && !pCreature->HasFlag(UNIT_NPC_FLAGS, npcFlagMask))
            return false;
        if (pCreature->IsInvisibleForAlive() || pCreature->IsHostileTo(me))
            return false;
        if (pCreature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))
            return false;

        if (FactionTemplateEntry const* factionTemplate = sObjectMgr.GetFactionTemplateEntry(pCreature->GetFactionTemplateId()))
            if (factionTemplate->faction)
                if (FactionEntry const* faction = sObjectMgr.GetFactionEntry(factionTemplate->faction))
                    if (faction->reputationListID >= 0 && me->GetReputationMgr().GetRank(faction) <= REP_UNFRIENDLY)
                        return false;
        return true;
    }

    // Usable NPCs with the flag, nearest first.
    std::vector<Creature*> FindNpcs(Player* me, uint32 npcFlagMask, float range)
    {
        std::list<Creature*> found;
        MaNGOS::AnyUnitInObjectRangeCheck check(me, range);
        MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(found, check);
        Cell::VisitGridObjects(me, searcher, range);

        std::vector<Creature*> result;
        for (Creature* pCreature : found)
        {
            if (CanUseNpc(me, pCreature, npcFlagMask))
                result.push_back(pCreature);
        }
        std::sort(result.begin(), result.end(), [me](Creature* a, Creature* b)
        {
            return me->GetDistance(a) < me->GetDistance(b);
        });
        return result;
    }

    template <typename F>
    void ForEachInventoryItem(Player* me, F fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(pItem);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pBag = static_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* pItem = me->GetItemByPos(bagSlot, slot))
                    fn(pItem);
        }
    }

    uint32 CountFreeBagSlots(Player* me)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* pBag = static_cast<Bag*>(me->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (!me->GetItemByPos(bagSlot, slot))
                    ++free;
        }
        return free;
    }

    bool NeedsRepair(Player* me)
    {
        bool damaged = false;
        auto check = [&damaged](Item* pItem)
        {
            uint32 maxDur = pItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maxDur && pItem->GetUInt32Value(ITEM_FIELD_DURABILITY) < maxDur)
                damaged = true;
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                check(pItem);
        ForEachInventoryItem(me, check);
        return damaged;
    }

    uint32 GetAverageDurabilityPct(Player* me)
    {
        uint32 cur = 0;
        uint32 max = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                cur += pItem->GetUInt32Value(ITEM_FIELD_DURABILITY);
                max += pItem->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            }
        }
        return max ? (cur * 100 / max) : 100;
    }

    struct ReagentNeed
    {
        uint32 itemId;
        uint32 have;
        uint32 want;
    };

    std::vector<ReagentNeed> GetMissingReagents(PersistentBotAI const* pAI)
    {
        std::vector<ReagentNeed> needs;
        Player* me = pAI->me;
        for (SpellEntry const* pSpell : pAI->m_spells.raw.spells)
        {
            if (!pSpell)
                continue;

            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (pSpell->Reagent[i] <= 0 || !pSpell->ReagentCount[i])
                    continue;

                uint32 itemId = uint32(pSpell->Reagent[i]);
                ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
                if (!pProto)
                    continue;

                uint32 want = std::min(pSpell->ReagentCount[i] * REAGENT_STACK_MULT, std::max(pProto->Stackable, 1u));
                auto itr = std::find_if(needs.begin(), needs.end(), [itemId](ReagentNeed const& n) { return n.itemId == itemId; });
                if (itr != needs.end())
                {
                    itr->want = std::max(itr->want, want);
                    continue;
                }
                needs.push_back({ itemId, me->GetItemCount(itemId), want });
            }
        }

        needs.erase(std::remove_if(needs.begin(), needs.end(), [](ReagentNeed const& n) { return n.have >= n.want; }), needs.end());
        return needs;
    }

    // Subclass of the ammo needed by the equipped ranged weapon; 0 if none.
    uint32 GetNeededAmmoSubClass(Player* me)
    {
        Item* pRanged = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!pRanged || !pRanged->GetProto() || pRanged->GetProto()->Class != ITEM_CLASS_WEAPON)
            return 0;

        switch (pRanged->GetProto()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return ITEM_SUBCLASS_ARROW;
            case ITEM_SUBCLASS_WEAPON_GUN:
                return ITEM_SUBCLASS_BULLET;
        }
        return 0;
    }

    uint32 CountAmmo(Player* me, uint32 subClass)
    {
        uint32 count = 0;
        ForEachInventoryItem(me, [&](Item* pItem)
        {
            ItemPrototype const* pProto = pItem->GetProto();
            if (pProto && pProto->Class == ITEM_CLASS_PROJECTILE && pProto->SubClass == subClass)
                count += pItem->GetCount();
        });
        return count;
    }

    VendorItem const* FindVendorItem(Creature* pVendor, uint32 itemId)
    {
        if (VendorItemData const* pItems = pVendor->GetVendorItems())
            if (VendorItem const* pItem = pItems->FindItem(itemId))
                return pItem;
        if (VendorItemData const* pItems = pVendor->GetVendorTemplateItems())
            return pItems->FindItem(itemId);
        return nullptr;
    }

    void ForEachVendorItem(Creature* pVendor, std::function<void(VendorItem const*)> fn)
    {
        if (VendorItemData const* pItems = pVendor->GetVendorItems())
            for (VendorItem const* pItem : pItems->m_items)
                fn(pItem);
        if (VendorItemData const* pItems = pVendor->GetVendorTemplateItems())
            for (VendorItem const* pItem : pItems->m_items)
                fn(pItem);
    }

    struct BuyOrder
    {
        uint32 itemId;
        uint32 lots;
    };

    uint32 GetGoldReserve(Player* me)
    {
        return me->GetLevel() * me->GetLevel() * 10;
    }

    // Plans purchases of missing reagents and ammo at this vendor, within budget (money minus repair reserve).
    std::vector<BuyOrder> BuildBuyOrders(PersistentBotAI const* pAI, Creature* pVendor)
    {
        std::vector<BuyOrder> orders;
        Player* me = pAI->me;
        uint32 reserve = GetGoldReserve(me);
        uint32 budget = me->GetMoney() > reserve ? me->GetMoney() - reserve : 0;
        if (!budget)
            return orders;

        float discount = me->GetReputationPriceDiscount(pVendor);

        auto plan = [&](uint32 itemId, uint32 units)
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
            VendorItem const* pVendorItem = FindVendorItem(pVendor, itemId);
            if (!pProto || !pVendorItem || !pProto->BuyCount || !pProto->BuyPrice || !units)
                return;

            uint32 lots = (units + pProto->BuyCount - 1) / pProto->BuyCount;
            uint32 lotPrice = uint32(pProto->BuyPrice * discount + 0.5f);
            if (lotPrice)
                lots = std::min(lots, budget / lotPrice);
            if (pVendorItem->maxcount)
                lots = std::min(lots, pVendor->GetVendorItemCurrentCount(pVendorItem) / pProto->BuyCount);
            lots = std::min(lots, 255u);
            if (!lots)
                return;

            budget -= lots * lotPrice;
            orders.push_back({ itemId, lots });
        };

        if (me->GetClass() == CLASS_HUNTER)
        {
            if (uint32 ammoSub = GetNeededAmmoSubClass(me))
            {
                uint32 have = CountAmmo(me, ammoSub);
                if (have < AMMO_REBUY_BELOW)
                {
                    ItemPrototype const* pBest = nullptr;
                    ForEachVendorItem(pVendor, [&](VendorItem const* pVendorItem)
                    {
                        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(pVendorItem->item);
                        if (!pProto || pProto->Class != ITEM_CLASS_PROJECTILE || pProto->SubClass != ammoSub ||
                            !pProto->BuyCount || me->CanUseAmmo(pProto->ItemId) != EQUIP_ERR_OK)
                            return;
                        uint32 lotPrice = uint32(pProto->BuyPrice * discount + 0.5f);
                        if (lotPrice > budget)
                            return;
                        if (!pBest || pProto->RequiredLevel > pBest->RequiredLevel ||
                            (pProto->RequiredLevel == pBest->RequiredLevel && pProto->ItemLevel > pBest->ItemLevel))
                            pBest = pProto;
                    });
                    if (pBest)
                        plan(pBest->ItemId, AMMO_TARGET - have);
                }
            }
        }

        for (ReagentNeed const& need : GetMissingReagents(pAI))
            plan(need.itemId, need.want - need.have);

        return orders;
    }

    // Grimoire: an on-use item whose spell teaches a pet spell (SPELL_EFFECT_LEARN_PET_SPELL, detected by effect).
    bool GetGrimoire(ItemPrototype const* pProto, uint32& spellSlot, uint32& learnSpellId)
    {
        for (uint32 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            if (!pProto->Spells[slot].SpellId || pProto->Spells[slot].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(pProto->Spells[slot].SpellId);
            if (!pSpell)
                continue;

            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (pSpell->Effect[i] == SPELL_EFFECT_LEARN_PET_SPELL && sSpellMgr.GetSpellEntry(pSpell->EffectTriggerSpell[i]))
                {
                    spellSlot = slot;
                    learnSpellId = pSpell->EffectTriggerSpell[i];
                    return true;
                }
            }
        }
        return false;
    }

    // The pet knows this spell or a higher rank of its chain.
    bool PetKnowsRankOrHigher(Pet const* pPet, uint32 spellId)
    {
        SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId);
        uint32 const chain = sSpellMgr.GetFirstSpellInChain(spellId);
        for (auto const& itr : pPet->m_petSpells)
        {
            if (itr.second.state == PETSPELL_REMOVED)
                continue;

            SpellEntry const* pKnown = sSpellMgr.GetSpellEntry(itr.first);
            if (pKnown && pSpell && sSpellMgr.GetFirstSpellInChain(itr.first) == chain && pKnown->GetRank() >= pSpell->GetRank())
                return true;
        }
        return false;
    }

    // The checks EffectLearnPetSpell and Spell::CheckCast make for a grimoire (pet family skill, active spell slots,
    // level, training points), plus not known yet.
    bool PetCanTakeGrimoireSpell(Pet* pPet, SpellEntry const* pUseSpell, uint32 learnSpellId)
    {
        if (pUseSpell->spellLevel > pPet->GetLevel())
            return false;
        if (!pPet->CanLearnPetSpell(learnSpellId) || !pPet->CanTakeMoreActiveSpells(learnSpellId) || !pPet->HasTPForSpell(learnSpellId))
            return false;
        return !PetKnowsRankOrHigher(pPet, learnSpellId);
    }

    // A grimoire in the bags that teaches this chain at the same or a higher rank (bought or looted, not used yet).
    bool HasGrimoireInBagsFor(Player* me, uint32 learnSpellId)
    {
        uint32 const chain = sSpellMgr.GetFirstSpellInChain(learnSpellId);
        uint32 const rank = sSpellMgr.GetSpellEntry(learnSpellId)->GetRank();
        bool found = false;
        ForEachInventoryItem(me, [&](Item* pItem)
        {
            uint32 spellSlot = 0;
            uint32 bagLearnSpell = 0;
            if (!found && GetGrimoire(pItem->GetProto(), spellSlot, bagLearnSpell) &&
                sSpellMgr.GetFirstSpellInChain(bagLearnSpell) == chain && sSpellMgr.GetSpellEntry(bagLearnSpell)->GetRank() >= rank)
                found = true;
        });
        return found;
    }

    // Grimoires this vendor sells for the summoned demon: lowest rank first, one per pet spell chain, within the gold reserve.
    std::vector<uint32> BuildGrimoireBuys(PersistentBotAI const* pAI, Creature* pVendor)
    {
        std::vector<uint32> buys;
        Player* me = pAI->me;
        Pet* pPet = me->GetPet();
        if (!pPet || pPet->GetPetType() != SUMMON_PET || !pPet->IsAlive())
            return buys;

        uint32 reserve = GetGoldReserve(me);
        uint32 budget = me->GetMoney() > reserve ? me->GetMoney() - reserve : 0;
        if (!budget)
            return buys;

        float discount = me->GetReputationPriceDiscount(pVendor);

        struct GrimoireCandidate
        {
            uint32 itemId;
            uint32 rank;
            uint32 chain;
            uint32 price;
        };
        std::vector<GrimoireCandidate> cands;
        ForEachVendorItem(pVendor, [&](VendorItem const* pVendorItem)
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(pVendorItem->item);
            uint32 spellSlot = 0;
            uint32 learnSpellId = 0;
            if (!pProto || !pProto->BuyCount || !pProto->BuyPrice || !GetGrimoire(pProto, spellSlot, learnSpellId))
                return;
            if (me->CanUseItem(pProto) != EQUIP_ERR_OK)
                return;
            if (pVendorItem->maxcount && pVendor->GetVendorItemCurrentCount(pVendorItem) < pProto->BuyCount)
                return;

            SpellEntry const* pUseSpell = sSpellMgr.GetSpellEntry(pProto->Spells[spellSlot].SpellId);
            if (!pUseSpell || !PetCanTakeGrimoireSpell(pPet, pUseSpell, learnSpellId) || HasGrimoireInBagsFor(me, learnSpellId))
                return;

            cands.push_back({ pProto->ItemId, sSpellMgr.GetSpellEntry(learnSpellId)->GetRank(),
                sSpellMgr.GetFirstSpellInChain(learnSpellId), uint32(pProto->BuyPrice * discount + 0.5f) });
        });

        std::stable_sort(cands.begin(), cands.end(), [](GrimoireCandidate const& a, GrimoireCandidate const& b)
        {
            return a.rank < b.rank;
        });

        std::vector<uint32> chains;
        for (GrimoireCandidate const& c : cands)
        {
            if (std::find(chains.begin(), chains.end(), c.chain) != chains.end())
                continue;
            chains.push_back(c.chain);

            if (c.price > budget)
                continue;
            budget -= c.price;
            buys.push_back(c.itemId);
        }
        return buys;
    }

    // Missing profession tools this vendor sells. The gold reserve does not apply: tools are cheap and the bot needs them to gather.
    std::vector<uint32> BuildToolBuys(PersistentBotAI const* pAI, Creature* pVendor)
    {
        std::vector<uint32> buys;
        Player* me = pAI->me;
        // no free bag slot: a tool could not be stored, and the trip would repeat every time
        if (!CountFreeBagSlots(me))
            return buys;

        float discount = me->GetReputationPriceDiscount(pVendor);
        uint32 budget = me->GetMoney();

        for (PersistentBotToolNeed const& need : GetMissingTools(me))
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(need.itemId);
            VendorItem const* pVendorItem = FindVendorItem(pVendor, need.itemId);
            if (!pProto || !pVendorItem || (pVendorItem->maxcount && !pVendor->GetVendorItemCurrentCount(pVendorItem)))
                continue;

            uint32 const price = uint32(pProto->BuyPrice * discount + 0.5f);
            if (price > budget)
                continue;

            budget -= price;
            buys.push_back(need.itemId);
        }
        return buys;
    }

    std::vector<Item*> CollectSellableJunk(PersistentBotAI const* pAI)
    {
        std::vector<Item*> junk;
        ForEachInventoryItem(pAI->me, [&](Item* pItem)
        {
            ItemPrototype const* pProto = pItem->GetProto();
            if (pProto && pProto->SellPrice && pAI->IsJunk(pItem))
                junk.push_back(pItem);
        });
        return junk;
    }

    void StopWalking(Player* me)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
    }

    struct TrainerCandidate
    {
        TrainerSpell const* pSpell;
        uint32 trigger;
        uint32 cost;
        bool upgrade;
        TrainerSpellState state;
    };

    uint32 GetReqLevel(TrainerCandidate const& c)
    {
        if (c.pSpell->reqLevel)
            return c.pSpell->reqLevel;
        SpellEntry const* pTrigger = sSpellMgr.GetSpellEntry(c.trigger);
        return pTrigger ? pTrigger->spellLevel : 0;
    }

    void CollectTrainerSpells(Player* me, Creature* pTrainer, std::vector<TrainerCandidate>& out)
    {
        out.clear();
        float discount = me->GetReputationPriceDiscount(pTrainer);

        auto add = [&](TrainerSpellData const* pData)
        {
            if (!pData)
                return;
            for (auto const& itr : pData->spellList)
            {
                TrainerSpell const* pSpell = &itr.second;
                SpellEntry const* pEntry = sSpellMgr.GetSpellEntry(pSpell->spell);
                if (!pEntry || !sSpellMgr.GetSpellEntry(pEntry->EffectTriggerSpell[0]))
                    continue;

                uint32 trigger = pEntry->EffectTriggerSpell[0];
                if (!me->IsSpellFitByClassAndRace(trigger))
                    continue;

                bool duplicate = false;
                for (TrainerCandidate const& c : out)
                    if (c.pSpell->spell == pSpell->spell)
                        duplicate = true;
                if (duplicate)
                    continue;

                TrainerSpellState state = me->GetTrainerSpellState(pSpell);
                if (state != TRAINER_SPELL_GREEN && state != TRAINER_SPELL_GREEN_DISABLED)
                    continue;

                bool upgrade = false;
                if (SpellChainNode const* pChain = sSpellMgr.GetSpellChainNode(trigger))
                    upgrade = pChain->prev && me->HasSpell(pChain->prev);

                out.push_back({ pSpell, trigger, uint32(pSpell->spellCost * discount + 0.5f), upgrade, state });
            }
        };
        add(pTrainer->GetTrainerSpells());
        add(pTrainer->GetTrainerTemplateSpells());
    }

    // Same as HandleTrainerBuySpellOpcode: cast the trainer spell on the player, pay only on success.
    bool LearnFromTrainer(Player* me, Creature* pTrainer, TrainerCandidate const& c)
    {
        if (me->GetTrainerSpellState(c.pSpell) != TRAINER_SPELL_GREEN || me->GetMoney() < c.cost)
            return false;

        SpellEntry const* proto = sSpellMgr.GetSpellEntry(c.pSpell->spell);
        if (!proto)
            return false;

        me->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
        me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

        Spell* spell;
        if (proto->SpellVisual == 222)
            spell = new Spell(me, proto, false);
        else
            spell = new Spell(pTrainer, proto, false);

        SpellCastTargets targets;
        targets.setUnitTarget(me);

        SpellCastResult result = spell->prepare(std::move(targets));
        spell->update(1);

        if (result != SPELL_CAST_OK)
            return false;

        me->ModifyMoney(-int32(c.cost));
        return true;
    }

    void SortCandidates(std::vector<TrainerCandidate>& cands, bool upgradesFirst)
    {
        std::sort(cands.begin(), cands.end(), [upgradesFirst](TrainerCandidate const& a, TrainerCandidate const& b)
        {
            if (upgradesFirst && a.upgrade != b.upgrade)
                return a.upgrade;
            uint32 la = GetReqLevel(a);
            uint32 lb = GetReqLevel(b);
            if (la != lb)
                return la < lb;
            return a.cost < b.cost;
        });
    }
}

bool PersistentBotAI::UpdateClassTrainer()
{
    PersistentBotEconomyState& s = m_economyState;
    time_t now = time(nullptr);
    if (now < s.nextTrainerCheck || !me->IsAlive() || me->IsInCombat())
        return false;
    s.nextTrainerCheck = now + TRAINER_SCAN_INTERVAL;

    Creature* pTrainer = nullptr;
    for (Creature* pCreature : FindNpcs(me, UNIT_NPC_FLAG_TRAINER, NPC_SEARCH_RANGE))
    {
        if (pCreature->GetCreatureInfo()->trainer_type == TRAINER_TYPE_CLASS &&
            pCreature->IsTrainerOf(me, false) && me->IsWithinLOSInMap(pCreature))
        {
            pTrainer = pCreature;
            break;
        }
    }
    if (!pTrainer)
        return false;

    uint32 minSkipped = 0;
    for (PersistentBotSkippedSpell const& skipped : s.trainerSkipped)
        minSkipped = minSkipped ? std::min(minSkipped, skipped.cost) : skipped.cost;
    bool canAffordSkipped = minSkipped && me->GetMoney() >= minSkipped;

    uint32 spellCount = me->GetSpellMap().size();
    if (pTrainer->GetObjectGuid() == s.lastTrainerGuid && me->GetLevel() == s.lastTrainerLevel &&
        spellCount == s.lastTrainerSpellCount && !canAffordSkipped)
        return false;

    uint32 learned = 0;
    uint32 spent = 0;
    std::vector<TrainerCandidate> cands;
    for (uint32 pass = 0; pass < MAX_TRAINER_PASSES; ++pass)
    {
        CollectTrainerSpells(me, pTrainer, cands);
        SortCandidates(cands, true);

        bool progress = false;
        for (TrainerCandidate const& c : cands)
        {
            if (c.state != TRAINER_SPELL_GREEN || me->GetMoney() < c.cost)
                continue;
            if (LearnFromTrainer(me, pTrainer, c))
            {
                ++learned;
                spent += c.cost;
                progress = true;
            }
        }
        if (!progress)
            break;
    }

    s.trainerSkipped.clear();
    CollectTrainerSpells(me, pTrainer, cands);
    for (TrainerCandidate const& c : cands)
        if (c.state == TRAINER_SPELL_GREEN && me->GetMoney() < c.cost)
            s.trainerSkipped.push_back({ c.trigger, c.cost });

    s.lastTrainerGuid = pTrainer->GetObjectGuid();
    s.lastTrainerLevel = me->GetLevel();
    s.lastTrainerSpellCount = me->GetSpellMap().size();

    if (learned)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "Trained %u spells for ", learned);
        SayParty(me, buf + FormatMoney(spent));
    }
    return false;
}

// Reusable vendor trip: walk to the given vendor, sell junk, repair, buy reagents/ammo. Drive with
// PersistentBotVendorUpdateTrip every tick until it returns false.
bool PersistentBotVendorStartTrip(PersistentBotAI* pAI, ObjectGuid vendorGuid)
{
    PersistentBotEconomyState& s = pAI->m_economyState;
    Player* me = pAI->me;
    Creature* pVendor = me->GetMap()->GetCreature(vendorGuid);
    if (!pVendor || !CanUseNpc(me, pVendor, UNIT_NPC_FLAG_VENDOR) || !me->IsAlive())
        return false;

    time_t now = time(nullptr);
    s.vendorGuid = vendorGuid;
    s.vendorStage = PersistentBotEconomyState::VENDOR_WALKING;
    s.vendorDeadline = now + VENDOR_TRIP_TIMEOUT;
    s.vendorSold = 0;
    s.vendorMoveIssued = 0;
    s.vendorMoveDist = 0.0f;
    return true;
}

// Advances an active trip; returns true while the trip is still in progress (movement owned by the trip).
bool PersistentBotVendorUpdateTrip(PersistentBotAI* pAI)
{
    PersistentBotEconomyState& s = pAI->m_economyState;
    Player* me = pAI->me;
    time_t now = time(nullptr);

    if (s.vendorStage == PersistentBotEconomyState::VENDOR_IDLE)
        return false;

    Creature* pVendor = pAI->me->GetMap()->GetCreature(s.vendorGuid);
    if (!pVendor || !CanUseNpc(me, pVendor, UNIT_NPC_FLAG_VENDOR) || now > s.vendorDeadline || !pAI->me->IsAlive())
    {
        StopWalking(me);
        s.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
        s.nextVisitAllowed = now + 60;
        return false;
    }

    if (s.vendorStage == PersistentBotEconomyState::VENDOR_WALKING)
    {
        if (!pVendor->IsWithinDistInMap(me, VENDOR_TRADE_DIST))
        {
            if (now - s.vendorMoveIssued >= 3)
            {
                float const dist = pAI->me->GetDistance(pVendor);
                if (s.vendorMoveIssued && dist > s.vendorMoveDist - 1.0f)
                {
                    StopWalking(me);
                    s.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
                    s.nextVisitAllowed = now + 60;
                    return false;
                }
                s.vendorMoveIssued = now;
                s.vendorMoveDist = dist;
                pAI->me->GetMotionMaster()->MovePoint(0, pVendor->GetPositionX(), pVendor->GetPositionY(), pVendor->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
            }
            return true;
        }

        StopWalking(me);
        uint32 sold = 0;
        s.vendorMoneyAtSell = pAI->me->GetMoney();
        for (Item* pItem : CollectSellableJunk(pAI))
        {
            if (sold >= MAX_SELLS_PER_TRIP)
                break;
            auto packet = std::make_unique<WorldPackets::Item::SellItem>();
            packet->vendorGuid = pVendor->GetObjectGuid();
            packet->itemGuid = pItem->GetObjectGuid();
            packet->count = 0; // whole stack
            pAI->me->GetSession()->QueuePacket(std::move(packet));
            ++sold;
        }
        s.vendorSold = sold;
        s.vendorStage = PersistentBotEconomyState::VENDOR_SOLD;
        s.vendorDeadline = now + 15;
        return true;
    }

    // Sell packets have been processed by now; repair, then buy within the remaining budget.
    s.vendorStage = PersistentBotEconomyState::VENDOR_IDLE;
    s.nextVisitAllowed = now + 120;

    uint32 const moneyAfterSell = pAI->me->GetMoney();
    uint32 const earned = s.vendorSold && moneyAfterSell > s.vendorMoneyAtSell ? moneyAfterSell - s.vendorMoneyAtSell : 0;

    if (pVendor->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR) && NeedsRepair(me))
        pAI->me->DurabilityRepairAll(true, pAI->me->GetReputationPriceDiscount(pVendor));

    uint32 const repaired = moneyAfterSell > pAI->me->GetMoney() ? moneyAfterSell - pAI->me->GetMoney() : 0;
    for (uint32 itemId : BuildToolBuys(pAI, pVendor))
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        uint32 const moneyBeforeTool = pAI->me->GetMoney();
        if (pProto && pAI->me->BuyItemFromVendor(pVendor->GetObjectGuid(), itemId, 1, NULL_BAG, NULL_SLOT))
            SayParty(me, "Bought " + std::string(pProto->Name1) + " for " + FormatMoney(moneyBeforeTool - pAI->me->GetMoney()));
    }

    for (uint32 itemId : BuildGrimoireBuys(pAI, pVendor))
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
        uint32 const moneyBeforeGrimoire = pAI->me->GetMoney();
        if (pAI->me->BuyItemFromVendor(pVendor->GetObjectGuid(), itemId, 1, NULL_BAG, NULL_SLOT))
            SayParty(me, "Bought " + std::string(pProto->Name1) + " for " + FormatMoney(moneyBeforeGrimoire - pAI->me->GetMoney()));
    }

    uint32 boughtItems = 0;
    uint32 moneyBefore = pAI->me->GetMoney();
    uint32 ammoSub = pAI->me->GetClass() == CLASS_HUNTER ? GetNeededAmmoSubClass(me) : 0;
    for (BuyOrder const& order : BuildBuyOrders(pAI, pVendor))
    {
        if (pAI->me->BuyItemFromVendor(pVendor->GetObjectGuid(), order.itemId, uint8(order.lots), NULL_BAG, NULL_SLOT))
        {
            ++boughtItems;
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(order.itemId);
            if (ammoSub && pProto && pProto->Class == ITEM_CLASS_PROJECTILE && pProto->SubClass == ammoSub)
            {
                ItemPrototype const* pCurrent = sObjectMgr.GetItemPrototype(pAI->me->GetUInt32Value(PLAYER_AMMO_ID));
                if (!pCurrent || !pAI->me->HasItemCount(pCurrent->ItemId, 1) || pProto->RequiredLevel > pCurrent->RequiredLevel)
                    pAI->me->SetAmmo(order.itemId);
            }
        }
    }

    uint32 const moneyAfter = pAI->me->GetMoney();
    if (s.vendorSold)
        SayParty(me, "Sold " + std::to_string(s.vendorSold) + " junk items for " + FormatMoney(earned));
    if (repaired)
        SayParty(me, "Repaired equipment for " + FormatMoney(repaired));
    if (boughtItems)
        SayParty(me, "Bought " + std::to_string(boughtItems) + " item types for " + FormatMoney(moneyBefore > moneyAfter ? moneyBefore - moneyAfter : 0));
    return false;

}

bool PersistentBotAI::UpdateVendors()
{
    PersistentBotEconomyState& s = m_economyState;
    time_t now = time(nullptr);

    if (s.vendorStage != PersistentBotEconomyState::VENDOR_IDLE)
        return PersistentBotVendorUpdateTrip(this);

    if (now < s.nextVendorCheck || !me->IsAlive() || me->IsInCombat())
        return false;
    s.nextVendorCheck = now + 5;

    // Only act on vendors the player walked to: vendor near the bot and the leader near the vendor.
    Player* pLeader = GetPartyLeader();
    if (!pLeader || pLeader->GetMap() != me->GetMap())
        return false;

    std::vector<Creature*> npcs = FindNpcs(me, UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR, NPC_SEARCH_RANGE);
    npcs.erase(std::remove_if(npcs.begin(), npcs.end(), [pLeader](Creature* pNpc) { return !pLeader->IsWithinDist(pNpc, NPC_SEARCH_RANGE); }), npcs.end());
    if (npcs.empty())
        return false;

    if (now >= s.nextRepairAllowed && NeedsRepair(me))
    {
        for (Creature* pNpc : npcs)
        {
            if (!pNpc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR) || !me->IsWithinLOSInMap(pNpc))
                continue;
            uint32 moneyBefore = me->GetMoney();
            me->DurabilityRepairAll(true, me->GetReputationPriceDiscount(pNpc));
            uint32 spent = moneyBefore - me->GetMoney();
            s.nextRepairAllowed = now + 60;
            if (spent)
                SayParty(me, "Repaired equipment for " + FormatMoney(spent));
            break;
        }
    }

    if (now < s.nextVisitAllowed)
        return false;

    for (Creature* pNpc : npcs)
    {
        if (!pNpc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR) || !me->IsWithinLOSInMap(pNpc))
            continue;

        if (CollectSellableJunk(this).empty() && BuildBuyOrders(this, pNpc).empty() && BuildGrimoireBuys(this, pNpc).empty() &&
            BuildToolBuys(this, pNpc).empty())
            continue;

        return PersistentBotVendorStartTrip(this, pNpc->GetObjectGuid());
    }
    return false;
}

// Uses one grimoire from the bags for the summoned demon per call: lowest rank first, out of combat only. The item is
// used like a click on it (CMSG_USE_ITEM), the server then teaches the pet spell (EffectLearnPetSpell).
void PersistentBotAI::UpdateGrimoires()
{
    PersistentBotEconomyState& s = m_economyState;
    time_t now = time(nullptr);

    Pet* pPet = me->GetPet();
    if (!pPet || pPet->GetPetType() != SUMMON_PET || !pPet->IsAlive() || me->IsInCombat() || me->IsNonMeleeSpellCasted())
        return;

    if (s.grimoireLearnSpell && pPet->HasSpell(s.grimoireLearnSpell))
    {
        SayParty(me, "Taught " + std::string(pPet->GetName()) + " " + GetSpellNameFor(me, s.grimoireLearnSpell) + ".");
        s.grimoireLearnSpell = 0;
    }

    if (now < s.nextGrimoireUse)
        return;
    s.grimoireLearnSpell = 0; // the last use did not teach its spell in time

    Item* pBest = nullptr;
    uint32 bestSpellSlot = 0;
    uint32 bestLearnSpell = 0;
    uint32 bestRank = 0;
    ForEachInventoryItem(me, [&](Item* pItem)
    {
        ItemPrototype const* pProto = pItem->GetProto();
        uint32 spellSlot = 0;
        uint32 learnSpellId = 0;
        if (!pProto || !GetGrimoire(pProto, spellSlot, learnSpellId) || me->CanUseItem(pProto) != EQUIP_ERR_OK)
            return;

        SpellEntry const* pUseSpell = sSpellMgr.GetSpellEntry(pProto->Spells[spellSlot].SpellId);
        if (!pUseSpell || !PetCanTakeGrimoireSpell(pPet, pUseSpell, learnSpellId))
            return;

        uint32 const rank = sSpellMgr.GetSpellEntry(learnSpellId)->GetRank();
        if (!pBest || rank < bestRank)
        {
            pBest = pItem;
            bestSpellSlot = spellSlot;
            bestLearnSpell = learnSpellId;
            bestRank = rank;
        }
    });
    if (!pBest)
        return;

    // the client's use packet: bag and slot, the spell's index on the item, target left at self (the pet is implicit)
    auto packet = std::make_unique<WorldPackets::Spell::UseItem>();
    packet->bagIndex = pBest->GetBagSlot();
    packet->slot = pBest->GetSlot();
    packet->spellSlot = uint8(bestSpellSlot);
    me->GetSession()->QueuePacket(std::move(packet));

    s.grimoireLearnSpell = bestLearnSpell;
    s.nextGrimoireUse = now + GRIMOIRE_USE_INTERVAL;
}

void PersistentBotAI::HandleStatusCommand(ChatHandler* handler)
{
    handler->PSendSysMessage("%s: %s", me->GetName(), FormatMoney(me->GetMoney()).c_str());
    handler->PSendSysMessage("Level %u, XP %u/%u", me->GetLevel(), me->GetUInt32Value(PLAYER_XP), me->GetUInt32Value(PLAYER_NEXT_LEVEL_XP));
    handler->PSendSysMessage("Spec: %s, free talent points: %u, auto talents: %s", GetSpecName(), me->GetFreeTalentPoints(), m_autoTalents ? "on" : "off");

    LocaleConstant locale = me->GetSession()->GetSessionDbcLocale();
    std::string primary;
    std::string secondary;
    uint32 primaryIndex = 0;
    for (uint32 i = 0; i < PLAYER_MAX_SKILLS; ++i)
    {
        uint32 skillId = me->GetUInt32Value(BOT_SKILL_INDEX(i)) & 0xFFFF;
        if (!skillId)
            continue;
        SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(skillId);
        if (!pSkill || !me->GetSkillMaxPure(skillId))
            continue;

        char buf[96];
        std::string name = pSkill->name[locale] ? pSkill->name[locale] : "?";
        if (IsPrimaryProfessionSkill(skillId))
        {
            snprintf(buf, sizeof(buf), "%s[%u] %s %u/%u", primary.empty() ? "" : ", ", ++primaryIndex, name.c_str(), me->GetSkillValuePure(skillId), me->GetSkillMaxPure(skillId));
            primary += buf;
        }
        else if (pSkill->categoryId == SKILL_CATEGORY_SECONDARY)
        {
            snprintf(buf, sizeof(buf), "%s%s %u/%u", secondary.empty() ? "" : ", ", name.c_str(), me->GetSkillValuePure(skillId), me->GetSkillMaxPure(skillId));
            secondary += buf;
        }
    }
    handler->PSendSysMessage("Professions: %s", primary.empty() ? "none" : primary.c_str());
    handler->PSendSysMessage("Secondary: %s", secondary.empty() ? "none" : secondary.c_str());

    std::string reagents;
    for (ReagentNeed const& need : GetMissingReagents(this))
    {
        ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(need.itemId);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s%s %u/%u", reagents.empty() ? "" : ", ", pProto ? pProto->Name1 : "?", need.have, need.want);
        reagents += buf;
    }
    handler->PSendSysMessage("Missing reagents: %s", reagents.empty() ? "none" : reagents.c_str());

    if (me->GetClass() == CLASS_HUNTER)
    {
        uint32 ammoSub = GetNeededAmmoSubClass(me);
        if (ammoSub)
            handler->PSendSysMessage("Ammo: %u %s", CountAmmo(me, ammoSub), ammoSub == ITEM_SUBCLASS_ARROW ? "arrows" : "bullets");
        else
            handler->SendSysMessage("Ammo: no ranged weapon equipped");
    }

    handler->PSendSysMessage("Durability: %u%%, free bag slots: %u", GetAverageDurabilityPct(me), CountFreeBagSlots(me));

    if (!m_economyState.trainerSkipped.empty())
    {
        std::string skipped;
        for (PersistentBotSkippedSpell const& spell : m_economyState.trainerSkipped)
            skipped += (skipped.empty() ? "" : ", ") + GetSpellNameFor(me, spell.spellId) + " (" + FormatMoney(spell.cost) + ")";
        handler->PSendSysMessage("Waiting for gold at trainer: %s", skipped.c_str());
    }
}

void PersistentBotAI::HandleLearnCommand(ChatHandler* handler, Creature* pTrainer)
{
    if (!pTrainer || pTrainer->GetCreatureInfo()->trainer_type != TRAINER_TYPE_TRADESKILLS || !pTrainer->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER))
    {
        handler->SendSysMessage("Select a profession trainer first.");
        return;
    }
    if (!me->IsAlive() || !CanUseNpc(me, pTrainer, UNIT_NPC_FLAG_TRAINER) || !pTrainer->IsTrainerOf(me, false))
    {
        handler->PSendSysMessage("%s can't use %s right now.", me->GetName(), pTrainer->GetName());
        return;
    }
    if (!me->IsWithinDist(pTrainer, NPC_SEARCH_RANGE) || !me->IsWithinLOSInMap(pTrainer))
    {
        handler->PSendSysMessage("%s is too far from %s.", me->GetName(), pTrainer->GetName());
        return;
    }

    uint32 learned = 0;
    uint32 spent = 0;
    std::string names;
    std::vector<TrainerCandidate> cands;
    for (uint32 pass = 0; pass < MAX_TRAINER_PASSES; ++pass)
    {
        CollectTrainerSpells(me, pTrainer, cands);
        SortCandidates(cands, false);

        bool progress = false;
        for (TrainerCandidate const& c : cands)
        {
            if (c.state != TRAINER_SPELL_GREEN || me->GetMoney() < c.cost)
                continue;
            if (LearnFromTrainer(me, pTrainer, c))
            {
                if (learned < 8)
                    names += (names.empty() ? "" : ", ") + GetSpellNameFor(me, c.trigger);
                ++learned;
                spent += c.cost;
                progress = true;
            }
        }
        if (!progress)
            break;
    }

    uint32 noSlot = 0;
    uint32 noGold = 0;
    CollectTrainerSpells(me, pTrainer, cands);
    for (TrainerCandidate const& c : cands)
    {
        if (c.state == TRAINER_SPELL_GREEN_DISABLED)
            ++noSlot;
        else if (me->GetMoney() < c.cost)
            ++noGold;
    }

    handler->PSendSysMessage("%s learned %u spells from %s for %s%s%s", me->GetName(), learned, pTrainer->GetName(), FormatMoney(spent).c_str(), names.empty() ? "" : ": ", names.c_str());
    if (noSlot)
        handler->PSendSysMessage("%u profession(s) not learned: no free profession slot. Use .persistentbot unlearn <1|2> confirm to drop one.", noSlot);
    if (noGold)
        handler->PSendSysMessage("%u spell(s) skipped: not enough gold.", noGold);
}

void PersistentBotAI::HandleUnlearnCommand(ChatHandler* handler, uint32 index)
{
    uint32 current = 0;
    for (uint32 i = 0; i < PLAYER_MAX_SKILLS; ++i)
    {
        uint32 skillId = me->GetUInt32Value(BOT_SKILL_INDEX(i)) & 0xFFFF;
        if (!skillId || !IsPrimaryProfessionSkill(skillId) || !me->GetSkillMaxPure(skillId))
            continue;

        if (++current != index)
            continue;

        SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(skillId);
        std::string name = pSkill && pSkill->name[me->GetSession()->GetSessionDbcLocale()] ? pSkill->name[me->GetSession()->GetSessionDbcLocale()] : "?";
        me->SetSkill(skillId, 0, 0);
        handler->PSendSysMessage("%s unlearned %s.", me->GetName(), name.c_str());
        return;
    }
    handler->PSendSysMessage("%s has no primary profession at index %u.", me->GetName(), index);
}

void PersistentBotAI::HandleTameCommand(ChatHandler* handler, Creature* pTarget)
{
    if (me->GetClass() != CLASS_HUNTER)
    {
        handler->PSendSysMessage("%s is not a hunter.", me->GetName());
        return;
    }
    if (!me->HasSpell(SPELL_TAME_BEAST_KNOWN))
    {
        handler->PSendSysMessage("%s doesn't know Tame Beast yet.", me->GetName());
        return;
    }
    if (me->GetPetGuid() || me->GetCharmGuid() || sCharacterDatabaseCache.GetCharacterPetByOwner(me->GetGUIDLow()))
    {
        handler->PSendSysMessage("%s already has a pet, use .persistentbot abandonpet.", me->GetName());
        return;
    }
    if (!pTarget || !pTarget->IsAlive() || pTarget->IsPet() || pTarget->IsCharmed())
    {
        handler->SendSysMessage("Select a living wild beast to tame.");
        return;
    }
    if (pTarget->GetLevel() > me->GetLevel())
    {
        handler->PSendSysMessage("%s is too high level for %s to tame.", pTarget->GetName(), me->GetName());
        return;
    }
    if (!pTarget->GetCreatureInfo()->IsTameable())
    {
        handler->PSendSysMessage("%s can't be tamed.", pTarget->GetName());
        return;
    }
    if (!me->IsWithinDist(pTarget, NPC_SEARCH_RANGE) || !me->IsWithinLOSInMap(pTarget))
    {
        handler->PSendSysMessage("%s is out of range or line of sight.", pTarget->GetName());
        return;
    }

    me->CastSpell(pTarget, SPELL_TAME_BEAST_CAST, true);
    handler->PSendSysMessage("%s is taming %s.", me->GetName(), pTarget->GetName());
}

void PersistentBotAI::HandleAbandonPetCommand(ChatHandler* handler)
{
    Pet* pPet = me->GetPet();
    if (!pPet)
    {
        CharacterPetCache const* pStored = sCharacterDatabaseCache.GetCharacterPetByOwner(me->GetGUIDLow());
        if (!pStored)
        {
            handler->PSendSysMessage("%s has no pet.", me->GetName());
            return;
        }
        if (pStored->petType != HUNTER_PET)
        {
            handler->PSendSysMessage("%s's pet is not a hunter pet.", me->GetName());
            return;
        }

        std::string storedName = pStored->name;
        Pet::DeleteFromDB(pStored->id);
        handler->PSendSysMessage("%s abandoned %s.", me->GetName(), storedName.c_str());
        return;
    }
    if (pPet->GetPetType() != HUNTER_PET)
    {
        handler->PSendSysMessage("%s's pet is not a hunter pet.", me->GetName());
        return;
    }

    std::string name = pPet->GetName();
    pPet->Unsummon(PET_SAVE_AS_DELETED, me);
    handler->PSendSysMessage("%s abandoned %s.", me->GetName(), name.c_str());
}
