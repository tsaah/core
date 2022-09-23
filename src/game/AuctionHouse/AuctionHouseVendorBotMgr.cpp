#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Policies/SingletonImp.h"
#include "Item.h"
#include "AuctionHouseMgr.h"
#include "Creature.h"
#include "AuctionHouseVendorBotMgr.h"
#include "Player.h"
#include "ObjectMgr.h"

INSTANTIATE_SINGLETON_1(AuctionHouseVendorBotMgr);

const uint32 AuctionHouseVendorBotMgr::AUCTION_DURATION = 172800u;

AuctionHouseVendorBotMgr::~AuctionHouseVendorBotMgr() {
}

void AuctionHouseVendorBotMgr::Load() {
    sLog.outString();
    sLog.outString(">> AuctionHouseVendorBot");
    sLog.outString();

    loaded_ = true;
}

void AuctionHouseVendorBotMgr::onItemAddedToBuyBack(Player* player, Item* item, uint32 money, ObjectGuid vendorGuid) {
    MANGOS_ASSERT(player);

    MANGOS_ASSERT(item);
    if (!isAuctionable(item)) {
        return;
    }

    const auto* itemProto = item->GetProto();
    MANGOS_ASSERT(itemProto);
    if (!isAuctionable(itemProto)) {
        return;
    }

    const auto* vendor = player->GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR);
    MANGOS_ASSERT(vendor);

    const auto* vendorInfo = vendor->GetCreatureInfo();
    MANGOS_ASSERT(vendorInfo);

    ItemTrackInfo itemTrackInfo;
    itemTrackInfo.player = player;
    itemTrackInfo.item = item;
    itemTrackInfo.itemProto = itemProto;
    itemTrackInfo.vendorInfo = vendorInfo;

    itemTrackInfoHash_.emplace(std::make_pair(item, ItemTrackInfo{ player, item, itemProto, vendorInfo }));
    // AddItemToBuyBackSlot
}

void AuctionHouseVendorBotMgr::onItemDiscardedFromBuyBack(Player* player, Item* item) {
    MANGOS_ASSERT(item);

    if (!isTracked(item)) {
        return;
    }

    const auto& itemTrackInfo = itemTrackInfoHash_[item];

    const auto& vendorName = itemTrackInfo.vendorInfo->name;
    const auto& vendorFaction = itemTrackInfo.vendorInfo->faction;

    auto* newItem = cloneItem(itemTrackInfo.item);

    const auto* auctionHouseEntry = sAuctionMgr.GetAuctionHouseEntry(vendorFaction);
    MANGOS_ASSERT(auctionHouseEntry);

    createAuction(newItem, auctionHouseEntry);

    itemTrackInfoHash_.erase(item);
    // RemoveItemFromBuyBackSlot(slot, true);
}

void AuctionHouseVendorBotMgr::onItemBoughtBackFromBuyBack(Player* player, Item* item) {
    const auto* itemProto = item->GetProto();
    sLog.outString(">> !!!!!!!!!!!!!!!!!!! onItemBoughtBackFromBuyBack  ==>> %s x%d", itemProto->Name1, item->GetCount());

    itemTrackInfoHash_.erase(item);
    // RemoveItemFromBuyBackSlot(slot, false);
}

void AuctionHouseVendorBotMgr::onAuctionExpired(AuctionEntry* auction) {
    MANGOS_ASSERT(auction);

    if (auction->owner != 0) {
        return;
    }

    const auto* item = sAuctionMgr.GetAItem(auction->itemGuidLow);
    MANGOS_ASSERT(item);

    auto* newItem = cloneItem(item);

    const auto* itemProto = newItem->GetProto();
    MANGOS_ASSERT(itemProto);

    createAuction(newItem, auction->auctionHouseEntry);
}

void AuctionHouseVendorBotMgr::onAuctionSuccessfull(AuctionEntry* auction) {
    MANGOS_ASSERT(auction);

    if (auction->owner != 0) {
        return;
    }

    const auto* item = sAuctionMgr.GetAItem(auction->itemGuidLow);
    MANGOS_ASSERT(item);
    const auto* itemProto = item->GetProto();
    MANGOS_ASSERT(itemProto);

    sLog.outString(">> [+] AHVendorBot sold auction for %s x%d [ g%u s%u c%u]",
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
    auto* newItem = item->CloneItem(item->GetCount());
    newItem->SaveToDB();
    return newItem;
}

void AuctionHouseVendorBotMgr::createAuction(Item* item, const AuctionHouseEntry* auctionHouseEntry) const {
    MANGOS_ASSERT(item);
    MANGOS_ASSERT(auctionHouseEntry);
    const auto currentTime = time(nullptr);

    auto* auctionHouse = sAuctionMgr.GetAuctionsMap(auctionHouseEntry);
    MANGOS_ASSERT(auctionHouse);

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

    sLog.outString(">> [<] AHVendorBot created auction for %s x%d [ g%u s%u c%u]",
        item->GetProto()->Name1,
        item->GetCount(),
        buyout / 100000, (buyout / 100) % 100, buyout % 100
    );
}

uint32 AuctionHouseVendorBotMgr::calculateBuyout(const Item* item, uint32 deposit) const {
    const auto* itemProto = item->GetProto();
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
