#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Policies/SingletonImp.h"
#include "Item.h"
#include "AuctionHouseMgr.h"
#include "Creature.h"
#include "AuctionHouseVendorBotMgr.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Config/Config.h"

INSTANTIATE_SINGLETON_1(AuctionHouseVendorBotMgr);

const uint32 AuctionHouseVendorBotMgr::AUCTION_DURATION = 172800u;
const uint32 MIN_RESTOCK_INTERVAL = 3600;
const uint32 MAX_RESTOCK_INTERVAL = 86400;

AuctionHouseVendorBotMgr::~AuctionHouseVendorBotMgr() {}

void AuctionHouseVendorBotMgr::Load() {
    loaded_ = false;

    config_.enable = sConfig.GetBoolDefault("AHVendorBot.Enable", true);
    config_.minRestockInterval = static_cast<uint32>(sConfig.GetIntDefault("AHVendorBot.minRestockInterval", MIN_RESTOCK_INTERVAL));
    if (config_.minRestockInterval < MIN_RESTOCK_INTERVAL) { config_.minRestockInterval = MIN_RESTOCK_INTERVAL; }
    if (config_.minRestockInterval > MAX_RESTOCK_INTERVAL) { config_.minRestockInterval = MAX_RESTOCK_INTERVAL; }

    if (!config_.enable) { return; }

    // load
    const auto reservedItems = 0;

    loaded_ = true;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "AuctionHouseVendorBot::Load() : loaded %u reserved items.", reservedItems);
}

void AuctionHouseVendorBotMgr::onItemAddedToBuyBack(Player* player, Item* item, uint32 money, ObjectGuid vendorGuid) {
    if (!loaded_) { return; }

    ASSERT(player);
    ASSERT(item);

    if (!isAuctionable(item)) { return; }

    const auto itemProto = item->GetProto();
    if (!itemProto) {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "AuctionHouseVendorBotMgr::onItemAddedToBuyBack() : cannot get itemProto");
        return;
    }
    if (!isAuctionable(itemProto)) { return; }

    const auto vendor = player->GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR);
    if (!vendor) {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "AuctionHouseVendorBotMgr::onItemAddedToBuyBack() : cannot get vendor");
        return;
    }
    const auto vendorInfo = vendor->GetCreatureInfo();
    if (!vendorInfo) {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "AuctionHouseVendorBotMgr::onItemAddedToBuyBack() : cannot get vendorInfo");
        return;
    }

    itemTrackInfoHash_.emplace(std::pair<Item*, ItemTrackInfo>{ item, { player, item, itemProto, vendorInfo } });
    // AddItemToBuyBackSlot
}

void AuctionHouseVendorBotMgr::onItemDiscardedFromBuyBack(Player* player, Item* item) {
    if (!loaded_) { return; }

    ASSERT(player);
    ASSERT(item);

    if (!isTracked(item)) { return; }

    const auto& itemTrackInfo = itemTrackInfoHash_[item];
    const auto& vendorName = itemTrackInfo.vendorInfo->name;
    const auto& vendorFaction = itemTrackInfo.vendorInfo->faction;

    // IDEA: optional ignore if vendor is not affiliated with AH by lore (list for vendors to ignore)

    auto newItem = cloneItem(itemTrackInfo.item);

    const auto auctionHouseEntry = sAuctionMgr.GetAuctionHouseEntry(vendorFaction);
    if (!auctionHouseEntry) {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "AuctionHouseVendorBotMgr::onItemDiscardedFromBuyBack() : cannot get auctionHouseEntry");
        return;
    }

    createAuction(newItem, auctionHouseEntry);

    itemReserveInfoHash_.emplace(itemTrackInfoHash_[item]);
    itemTrackInfoHash_.erase(item);
    // RemoveItemFromBuyBackSlot(slot, true);
}

void AuctionHouseVendorBotMgr::onItemBoughtBackFromBuyBack(Player* player, Item* item) {
    if (!loaded_) { return; }

    const auto itemProto = item->GetProto();

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, ">> !!!!!!!!!!!!!!!!!!! onItemBoughtBackFromBuyBack  ==>> %s x%d", itemProto->Name1, item->GetCount());

    itemTrackInfoHash_.erase(item);
    // RemoveItemFromBuyBackSlot(slot, false);
}

void AuctionHouseVendorBotMgr::onAuctionExpired(AuctionEntry* auction) {
    if (!loaded_) { return; }
    ASSERT(auction);

    if (auction->owner != 0) {
        return;
    }

    const auto item = sAuctionMgr.GetAItem(auction->itemGuidLow);
    ASSERT(item);

    auto newItem = cloneItem(item);

    const auto itemProto = newItem->GetProto();
    ASSERT(itemProto);

    createAuction(newItem, auction->auctionHouseEntry);
}

void AuctionHouseVendorBotMgr::onAuctionSuccessfull(AuctionEntry* auction) {
    if (!loaded_) { return; }
    ASSERT(auction);

    if (auction->owner != 0) {
        return;
    }

    const auto item = sAuctionMgr.GetAItem(auction->itemGuidLow);
    ASSERT(item);
    const auto itemProto = item->GetProto();
    ASSERT(itemProto);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, ">> [+] AHVendorBot sold auction for %s x%d [ g%u s%u c%u]",
        itemProto->Name1,
        item->GetCount(),
        auction->buyout / 100000, (auction->buyout / 100) % 100, auction->buyout % 100
    );
}

bool AuctionHouseVendorBotMgr::isAuctionable(const Item* item) const {
    return !item->IsSoulBound()
        && !item->IsCharter();
}

bool AuctionHouseVendorBotMgr::isAuctionable(const ItemPrototype* itemProto) const {
    return !itemProto->IsConjuredConsumable()
        && itemProto->Quality != 0;
}

bool AuctionHouseVendorBotMgr::isTracked(Item* item) const {
    return itemTrackInfoHash_.count(item) == 1;
}

Item* AuctionHouseVendorBotMgr::cloneItem(const Item* item) const {
    auto newItem = item->CloneItem(item->GetCount());
    newItem->SaveToDB();
    return newItem;
}

void AuctionHouseVendorBotMgr::createAuction(Item* item, const AuctionHouseEntry* auctionHouseEntry) const {
    ASSERT(item);
    ASSERT(auctionHouseEntry);
    const auto currentTime = time(nullptr);

    auto* auctionHouse = sAuctionMgr.GetAuctionsMap(auctionHouseEntry);
    ASSERT(auctionHouse);

    const auto deposit = sAuctionMgr.GetAuctionDeposit(auctionHouseEntry, AUCTION_DURATION, item);
    const auto buyout = calculateBuyout(item, deposit);
    const auto bid = calculateBid(item, buyout);

    AuctionEntry* auctionEntry       = new AuctionEntry;
    auctionEntry->Id                 = sObjectMgr.GenerateAuctionID();
    auctionEntry->auctionHouseEntry  = auctionHouseEntry;
    auctionEntry->itemGuidLow        = item->GetGUIDLow();
    auctionEntry->itemTemplate       = item->GetEntry();
    auctionEntry->owner              = 0;
    auctionEntry->startbid           = bid;
    auctionEntry->buyout             = buyout;
    auctionEntry->bidder             = 0;
    auctionEntry->bid                = 0;
    auctionEntry->deposit            = deposit;
    auctionEntry->depositTime        = currentTime;
    auctionEntry->expireTime         = currentTime + static_cast<time_t>(AUCTION_DURATION);

    sAuctionMgr.AddAItem(item);
    auctionHouse->AddAuction(auctionEntry);
    auctionEntry->SaveToDB();

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, ">> [<] AHVendorBot created auction for %s x%d [ g%u s%u c%u]",
        item->GetProto()->Name1,
        item->GetCount(),
        buyout / 100000, (buyout / 100) % 100, buyout % 100
    );
}

uint32 AuctionHouseVendorBotMgr::calculateBuyout(const Item* item, uint32 deposit) const {
    const auto itemProto = item->GetProto();
    const auto basePrice = itemProto->BuyPrice;
    const auto itemCount = item->GetCount();
    const auto quality = itemProto->Quality;
    const auto qualityWeight = 1.0 / (1.0 + 0.7 * quality * log(quality));
    const auto flatWeight = 1.0 / (1.0 + log(basePrice));
    const auto flat = 1000;

    const auto value = deposit + basePrice * itemCount * quality * qualityWeight + flat * flatWeight;
    return static_cast<uint32>(value);
}

uint32 AuctionHouseVendorBotMgr::calculateBid(const Item* item, uint32 buyout) const {
    const auto bidWeight = 0.9;
    const auto value = buyout * bidWeight;
    return static_cast<uint32>(value);
}
