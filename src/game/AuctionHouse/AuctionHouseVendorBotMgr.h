#ifndef _AUCTION_HOUSE_VENDOR_BOT_MGR_H
#define _AUCTION_HOUSE_VENDOR_BOT_MGR_H

#include "Policies/Singleton.h"
#include "SharedDefines.h"
#include "ObjectGuid.h"
#include <unordered_map>

class Item;
class AuctionEntry;
class Player;
class CreatureInfo;
class ItemPrototype;
class AuctionHouseEntry;

struct ItemTrackInfo {
    const Player* player{ nullptr };
    const Item* item{ nullptr };
    const ItemPrototype* itemProto{ nullptr };
    const CreatureInfo* vendorInfo{ nullptr };
};

class AuctionHouseVendorBotMgr {
public:
    AuctionHouseVendorBotMgr() = default;
    ~AuctionHouseVendorBotMgr();

    void Load();

    void onItemAddedToBuyBack(Player* player, Item* item, uint32 money, ObjectGuid vendorGuid);
    void onItemDiscardedFromBuyBack(Player* player, Item* item);
    void onItemBoughtBackFromBuyBack(Player* player, Item* item);
    void onAuctionExpired(AuctionEntry* auction);
    void onAuctionSuccessfull(AuctionEntry* auction);

protected:
    bool isAuctionable(const Item* item) const;
    bool isAuctionable(const ItemPrototype* itemProto) const;
    bool isTracked(Item* item) const;
    Item* cloneItem(const Item* item) const;
    void createAuction(Item* item, const AuctionHouseEntry* auctionHouseEntry) const;
    uint32 calculateBuyout(const Item* item, uint32 deposit) const;
    uint32 calculateBid(const Item* item, uint32 buyout) const;

    std::unordered_map<Item*, ItemTrackInfo> itemTrackInfoHash_;
    bool loaded_{ false };

    static const uint32 AUCTION_DURATION;
};

#define sAuctionHouseVendorBotMgr MaNGOS::Singleton<AuctionHouseVendorBotMgr>::Instance()
#endif
