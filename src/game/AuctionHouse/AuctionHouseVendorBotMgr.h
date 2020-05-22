#ifndef _AUCTION_HOUSE_VENDOR_BOT_MGR_H
#define _AUCTION_HOUSE_VENDOR_BOT_MGR_H

#include "Policies/Singleton.h"
#include "SharedDefines.h"

#include <memory>

class Player;

struct AuctionHouseVendorBotEntry {
    uint32 itemGuid;
    uint32 auctionHouseId;
    uint64 infoTimestamp;
    uint32 state;
};

struct AuctionHouseVendorBotConfig {
    uint32 botAccount;
    uint32 maxAuctions;
    bool enabled;
};

class AuctionHouseVendorBotMgr {
public :
    AuctionHouseVendorBotMgr() = default;
    ~AuctionHouseVendorBotMgr();

    void load(); // load items from db
    void addPendingItemInfo(Unit* vendor, Item* item); // on Player::AddItemToBuyBackSlot 
    void removeItemInfo(Item* item); // on Player::RemoveItemFromBuyBackSlot with del = false or AuctionHouseMgr::SendAuctionSuccessfulMail
    void createAuction(Item* item); // on Player::RemoveItemFromBuyBackSlot with del = true or on Player::_SaveInventory()
    void recreateExpiredAuction(Item* item); // on AuctionHouseMgr::SendAuctionExpiredMail
    bool isEnabled() const;

protected:
    std::unique_ptr<AuctionHouseVendorBotConfig> m_config;
    std::unordered_map<uint32, AuctionHouseVendorBotEntry> m_infos;
    bool m_loaded = false;

private:
    bool isItemResellable(Item* item) const;
    uint32 calculatePrice(Item* item) const;
    bool haveInfo(Item* item) const;
    void trimInfos();
    uint32 getAuctionHouseId(Unit* vendor) const;
};

#define sAuctionHouseVendorBotMgr MaNGOS::Singleton<AuctionHouseVendorBotMgr>::Instance()
#endif
