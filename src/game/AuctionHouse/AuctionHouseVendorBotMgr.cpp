#include "Database/DatabaseEnv.h"
#include "World.h"
#include "Log.h"
#include "ProgressBar.h"
#include "Policies/SingletonImp.h"
#include "Player.h"
#include "Item.h"
#include "AuctionHouseMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "AuctionHouseVendorBotMgr.h"
#include "Config/Config.h"
#include "Chat.h"

#include "Database/DatabaseEnv.h"

INSTANTIATE_SINGLETON_1(AuctionHouseVendorBotMgr);

AuctionHouseVendorBotMgr::~AuctionHouseVendorBotMgr() {
    m_infos.clear();

    if (m_config) {
        m_config.reset();
    }
}

void AuctionHouseVendorBotMgr::load() {
    /* 1 - DELETE */
    m_infos.clear();
    m_loaded = false;

    if (m_config) { m_config.reset(); }

    /* CONFIG */
    m_config = std::make_unique<AuctionHouseVendorBotConfig>();
    m_config->enabled = sConfig.GetBoolDefault("AHVendorBot.Enable", true);
    m_config->botAccount = sConfig.GetIntDefault("AHVendorBot.bot.account", 0);
    m_config->maxAuctions = sConfig.GetIntDefault("AHVendorBot.bot.maxAuctions", 4096);

    if (!m_config->enabled) { return; }

    /* create table */
    WorldDatabase.Query(R"(CREATE TABLE IF NOT EXISTS `mangos`.`auctionhousevendorbot` (
        `itemGuid` INT(11) UNSIGNED NOT NULL,
        `factionTemplateId` TINYINT(3) UNSIGNED NULL,
        `infoTimestamp` BIGINT(40) UNSIGNED NOT NULL,
        `state` TINYINT(3) UNSIGNED NOT NULL DEFAULT 1)
        ENGINE = MyISAM
        DEFAULT CHARACTER SET = latin1
        COLLATE = latin1_bin;)");
    

    /*2 - LOAD */
    auto result = WorldDatabase.Query("SELECT `itemGuid`, `factionTemplateId`, `infoTimestamp`, `state` FROM `auctionhousevendorbot`");
    if (!result) {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded 0 AHVendorBot infos and auctions");
        return;
    }

    uint32 infoCount = 0;
    BarGoLink bar(result->GetRowCount());
    Field* fields = nullptr;
    do {
        bar.step();
        fields = result->Fetch();
        AuctionHouseVendorBotEntry e;
        e.itemGuid = fields[0].GetUInt32();
        e.factionTemplateId = fields[1].GetUInt32();
        e.infoTimestamp = fields[2].GetUInt64();
        e.state = fields[3].GetUInt32();

        m_infos.insert({ e.itemGuid, e });
        ++infoCount;

    } while (result->NextRow());

    sLog.outString();
    sLog.outString(">> Loaded %u AHVendorBot infos", infoCount);
    
    m_loaded = true;
}

void AuctionHouseVendorBotMgr::createAuction(Item* item) {
	if (!isEnabled() || !haveInfo(item)) { return; }

    auto& entry = m_infos.at(item->GetGUIDLow());
    auto* auctionHouseEntry = sAuctionMgr.GetAuctionHouseEntry(entry.factionTemplateId);

    auto const* prototype = item->GetProto();
    if (prototype == nullptr) {
        sLog.outInfo("AHVendorBot::createAuction(): Item prototype for guid %u does not exist.", item->GetGUIDLow());
        removeItemInfo(item);
        return;
    }

    Item* newItem = Item::CreateItem(prototype->ItemId, item->GetCount(), nullptr);
    if (!newItem) {
        sLog.outInfo("AHVendorBot::createAuction(): Cannot create item.");
        removeItemInfo(item);
        return;
    }

    newItem->SetItemRandomProperties(item->GetItemRandomPropertyId());
    for (uint32 i = 0; i < 5; ++i) {
        newItem->SetSpellCharges(i, item->GetSpellCharges(i));
    }
    {
		EnchantmentSlot i = PERM_ENCHANTMENT_SLOT;
		newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
        i = TEMP_ENCHANTMENT_SLOT;
        newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
        i = PROP_ENCHANTMENT_SLOT_0;
        newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
        i = PROP_ENCHANTMENT_SLOT_1;
        newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
        i = PROP_ENCHANTMENT_SLOT_2;
        newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
        i = PROP_ENCHANTMENT_SLOT_3;
        newItem->SetEnchantment(i, item->GetEnchantmentId(i), item->GetEnchantmentDuration(i), item->GetEnchantmentCharges(i));
    }
    newItem->SetUInt32Value(ITEM_FIELD_FLAGS, item->GetUInt32Value(ITEM_FIELD_FLAGS));
    newItem->SetUInt32Value(ITEM_FIELD_DURATION, item->GetUInt32Value(ITEM_FIELD_DURATION));
    newItem->SetUInt32Value(ITEM_FIELD_DURABILITY, item->GetUInt32Value(ITEM_FIELD_DURABILITY));

    const uint32 auctionTime = 172800;
    const uint32 dep = sAuctionMgr.GetAuctionDeposit(auctionHouseEntry, auctionTime, newItem);
    const uint32 price = dep + calculatePrice(newItem);

    auto* auctionEntry              = new AuctionEntry;
    auctionEntry->Id                = sObjectMgr.GenerateAuctionID();
    auctionEntry->itemGuidLow       = newItem->GetGUIDLow();// GetObjectGuid().GetCounter();
    auctionEntry->itemTemplate      = newItem->GetEntry();
    auctionEntry->owner             = 0;
    auctionEntry->ownerAccount      = m_config->botAccount;
    auctionEntry->startbid          = price;
    auctionEntry->bidder            = 0;
    auctionEntry->bid               = 0;
    auctionEntry->buyout            = price;
    auctionEntry->depositTime       = time(nullptr);
    auctionEntry->expireTime        = static_cast<time_t>(auctionTime) + time(nullptr);
    auctionEntry->deposit           = dep;
    auctionEntry->auctionHouseEntry = auctionHouseEntry;
    
    newItem->SaveToDB();

	sAuctionMgr.AddAItem(newItem);
    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(auctionHouseEntry);
    auctionHouse->AddAuction(auctionEntry);
    auctionEntry->SaveToDB();


    entry.state = 2;
    entry.itemGuid = newItem->GetGUIDLow();
    WorldDatabase.PExecute("UPDATE `auctionhousevendorbot` SET `itemGuid` = '%u', `state` = '%u' WHERE `itemGuid` = '%u'", entry.itemGuid, entry.state, item->GetGUIDLow());

    sLog.outInfo("AHVendorBot::createAuction(): Created auction for item %u '%s' x%u.", item->GetGUIDLow(), item->GetProto()->Name1, item->GetCount());
}

void AuctionHouseVendorBotMgr::addPendingItemInfo(Unit* vendor, Item* item) { // on WorldSession::HandleSellItemOpcode 
	if (!vendor || !item || !isItemResellable(item)) { return; }

    trimInfos();

    AuctionHouseVendorBotEntry e;
    e.factionTemplateId = vendor->GetFactionTemplateId();
    e.itemGuid = item->GetGUIDLow();
    e.infoTimestamp = static_cast<uint64>(time(nullptr));
    e.state = 1;

    WorldDatabase.PExecute("INSERT INTO `auctionhousevendorbot` (`itemGuid`, `factionTemplateId`, `infoTimestamp`, `state`) VALUES ('%u', '%u', '" UI64FMTD "', '%u')", e.itemGuid, e.factionTemplateId, e.infoTimestamp, e.state);
    m_infos.insert({ e.itemGuid, e });
}

void AuctionHouseVendorBotMgr::removeItemInfo(Item* item) { // on Player::RemoveItemFromBuyBackSlot with del = false
    if (!isEnabled() || item == nullptr) { return; }
    const auto itemGuid = item->GetGUIDLow();
    WorldDatabase.PExecute("DELETE FROM `auctionhousevendorbot` WHERE `itemGuid` = '%u'", itemGuid);
    m_infos.erase(itemGuid);
}

void AuctionHouseVendorBotMgr::recreateExpiredAuction(Item* item) { // on AuctionHouseMgr::SendAuctionExpiredMail
    if (!isEnabled() || !haveInfo(item)) { return; }

    createAuction(item);
}

bool AuctionHouseVendorBotMgr::isEnabled() const {
    return m_config->enabled;
}

bool AuctionHouseVendorBotMgr::isItemResellable(Item* item) const {
    return item != nullptr
        // && !sAuctionMgr.GetAItem(pItem->GetObjectGuid().GetCounter())
        // && !_player->IsBankPos(pItem->GetPos())
        && item->CanBeTraded()
        && !(item->GetProto()->Flags & ITEM_FLAG_CONJURED)
        && !item->GetUInt32Value(ITEM_FIELD_DURATION)
        // && pItem->GetProto()->Quality > 0
	;
}

uint32 AuctionHouseVendorBotMgr::calculatePrice(Item* item) const {
    auto* proto = item->GetProto();
    auto i = proto->ItemId;
    auto result = WorldDatabase.PQuery(R"(
		select (a.nc * a.cc + a.nf * a.cf + a.ni * a.ci + a.nm * a.cm + a.ns * a.cs + a.np * a.cp + a.ng * a.cg) / (a.nc + a.nf + a.ni + a.nm + a.ns + a.np + a.ng) / 100.0 as chance from (
        select
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.creature_loot_template where item = '%u'), 0) as cc,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.fishing_loot_template where item = '%u'), 0) as cf,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.item_loot_template where item = '%u'), 0) as ci,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.mail_loot_template where item = '%u'), 0) as cm,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.skinning_loot_template where item = '%u'), 0) as cs,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.pickpocketing_loot_template where item = '%u'), 0) as cp,
        IFNULL((SELECT avg(ChanceOrQuestChance) FROM mangos.gameobject_loot_template where item = '%u'), 0) as cg,
        (SELECT count(item) FROM mangos.creature_loot_template where item = '%u') as nc,
        (SELECT count(item) FROM mangos.fishing_loot_template where item = '%u') as nf,
        (SELECT count(item) FROM mangos.item_loot_template where item = '%u') as ni,
        (SELECT count(item) FROM mangos.mail_loot_template where item = '%u') as nm,
        (SELECT count(item) FROM mangos.skinning_loot_template where item = '%u') as ns,
        (SELECT count(item) FROM mangos.pickpocketing_loot_template where item = '%u') as np,
        (SELECT count(item) FROM mangos.gameobject_loot_template where item = '%u') as ng
    ) as a)", i, i, i, i, i, i, i, i, i, i, i, i, i, i); // that is my best sql don't curse me please
    auto chance = 0.0f;
    if (result) {
        Field* fields = nullptr;
        do {
            fields = result->Fetch();
            chance = fields[0].GetFloat();
        } while (result->NextRow());
    }
    if (chance < 0.00001f) { chance = 0.00001f; }

    return static_cast<uint32>(static_cast<float>(item->GetCount()) * static_cast<float>(proto->BuyPrice) * (1.0f + 3.0f * (static_cast<float>(proto->Quality) + 1.0f) / 7.0f) * (1.0f + 1.0f * logf(1.0f / chance)));
}

bool AuctionHouseVendorBotMgr::haveInfo(Item* item) const {
    return m_infos.find(item->GetGUIDLow()) != m_infos.end();
}

void AuctionHouseVendorBotMgr::trimInfos() {
	const auto count = static_cast<int64>(m_infos.size()) - static_cast<int64>(m_config->maxAuctions + 1);
    if (count > 0) {
        auto result = WorldDatabase.PQuery("SELECT `itemGuid`, `infoTimestamp` FROM `auctionhousevendorbot` ODER BY `infoTimestamp` ASC LIMIT '%u'", count);
        if (!result) { return; }

        uint64 infoTimestamp = 0;
        Field* fields = nullptr;
        do {
            fields = result->Fetch();
            auto itemGuid = fields[0].GetUInt32();
            infoTimestamp = fields[1].GetUInt64();
            m_infos.erase(itemGuid);
        } while (result->NextRow());
        WorldDatabase.PExecute("DELETE FROM `auctionhousevendorbot` WHERE `infoTimestamp` <= '%u'", infoTimestamp);
    }

}