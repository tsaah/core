#ifndef MANGOS_PERSISTENTBOTAI_H
#define MANGOS_PERSISTENTBOTAI_H

#include "PartyBotAI.h"
#include "PersistentBotTalents.h"
#include "PersistentBotGear.h"
#include "PersistentBotQuests.h"
#include "PersistentBotEconomy.h"
#include "PersistentBotFarm.h"
#include "PersistentBotGather.h"

class ChatHandler;
class Creature;
class Item;
class Player;
class Quest;
struct ItemPrototype;

class PersistentBotAI : public PartyBotAI
{
public:
    PersistentBotAI(Player* pLeader, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : PartyBotAI(pLeader, mapId, instanceId, x, y, z, o)
    {
        m_persistent = true;
        m_persistentTimer.Reset(PERSISTENT_BOT_UPDATE_INTERVAL);
        m_petSpellTimer.Reset(PERSISTENT_BOT_UPDATE_INTERVAL);
    }

    void UpdateAI(uint32 const diff) override;
    void OnPacketReceived(WorldPacket const* packet) override;
    void OnGroupLeaderQuestAccepted(Quest const* pQuest, WorldObject* pGiver) override;
    bool IsLeaderless() const override { return m_farmState.farming; }
    Unit* SelectGrindTarget() override;
    void UpdateInCombatAI_Warlock() override;

    static bool IsDatabaseReady();
    void LoadSettings();
    void SaveSettings();
    // persistent_bot.owner_guid/active: the bots the owner had, restored when the owner logs in (see PlayerBotMgr::RestoreOwnedPersistentBots)
    static void MarkActive(uint32 guid, uint32 ownerGuid);
    static void MarkInactive(uint32 guid);
    // removes this bot (saved) and forgets it: it is not restored on the owner's next login
    void Dismiss();
    void OnKickedFromGroup() override { Dismiss(); }

    // PersistentBotChat.cpp
    void OnChatFromPlayer(Player* pSender, uint32 chatType, std::string const& msg) override;

    // PersistentBotTalents.cpp
    void InitTalentsOnSpawn();
    void UpdateTalents();
    uint8 DetectSpecTab() const;
    char const* GetSpecName() const;
    void HandleSpecCommand(ChatHandler* handler, char const* specArg);
    void HandleResetSpecCommand(ChatHandler* handler);

    // PersistentBotGear.cpp
    float ScoreItem(ItemPrototype const* pProto) const;
    bool IsUpgrade(ItemPrototype const* pProto) const;
    bool IsJunk(Item const* pItem) const;
    void OnLootRollStart(WorldPacket const* packet);
    void UpdateLootRolls();
    void UpdateBagStatus();
    bool UpdateLooting();
    void UpdateEquipment();

    // PersistentBotQuests.cpp
    bool UpdateQuestDelivery();
    std::string GetQuestStatusLine(uint32 questId) const;
    void HandleGetQuestsCommand(ChatHandler* handler, Creature* pGiver);

    // PersistentBotEconomy.cpp
    bool UpdateVendors();
    bool UpdateClassTrainer();
    void UpdateGrimoires();
    void HandleStatusCommand(ChatHandler* handler);
    void HandleLearnCommand(ChatHandler* handler, Creature* pTrainer);
    void HandleUnlearnCommand(ChatHandler* handler, uint32 index);
    void HandleTameCommand(ChatHandler* handler, Creature* pTarget);
    void HandleAbandonPetCommand(ChatHandler* handler);

    // PersistentBotFarm.cpp
    bool UpdateFarm();
    void UpdateFarmDeath();
    bool IsGrinding() const { return m_farmState.farming || m_farmState.afkGrinding; }
    void StartFarm(ChatHandler* handler, float radius);
    void StopFarm(ChatHandler* handler);
    bool HandleFarmCommand(ChatHandler* handler, float radius);
    void HandleFollowCommand(ChatHandler* handler);

    // PersistentBotGather.cpp
    bool UpdateGather();
    void CancelGather(bool announceFishStop);
    void HandleFishCommand(ChatHandler* handler);
    void HandleFishStopCommand(ChatHandler* handler);

    static constexpr uint32 PERSISTENT_BOT_UPDATE_INTERVAL = 1000;
    static constexpr uint32 SOUL_SHARD_ITEM_ID = 6265;
    static constexpr uint32 SOUL_SHARDS_WANTED = 10;

    PersistentBotTalentState m_talentState;
    PersistentBotGearState m_gearState;
    PersistentBotQuestState m_questState;
    PersistentBotEconomyState m_economyState;
    PersistentBotFarmState m_farmState;
    PersistentBotGatherState m_gatherState;
    uint8 m_specTab = 255; // 255 = auto
    bool m_autoTalents = true;

private:
    // PersistentBotFarm.cpp
    void UpdateAfkState();
    void SetGrindRole(bool grind);
    bool IsValidGrindTarget(Creature* pCreature) const;
    bool NeedsTown() const;
    bool IsTownTripUseful() const;
    bool NeedsRest() const;
    Creature* FindVendorInRange() const;
    bool FindFarVendor();
    uint8 TravelTo(float x, float y, float z, float arriveDist);
    void ResetTravel();
    void TellOwner(char const* text) const;
    // PersistentBotQuests.cpp
    bool TryTakeQuest(Quest const* pQuest, bool bypassFilter);
    Quest const* AbandonQuestForRoom();
    // PersistentBotAI.cpp: warlock pet autocast and pet spells
    void UpdateWarlockPets();
    // PersistentBotGather.cpp
    bool GatherAllowed(float& cx, float& cy, float& cz, float& range, char const*& why) const;
    bool StartGatherTarget(time_t now, float cx, float cy, float cz, float range);
    bool UpdateGatherTarget(time_t now, float cx, float cy, float cz, float range);
    bool StartGatherCast();
    bool IsGatherTargetValid(float cx, float cy, float range);
    void SkipGatherTarget(time_t now);
    void ReleaseGatherClaim();
    bool UpdateFishing();
    void NagMissingTools();

    ShortTimeTracker m_persistentTimer;
    ShortTimeTracker m_petSpellTimer;
    ObjectGuid m_petAutocastGuid;
    uint32 m_petAutocastSpellCount = 0;
    bool m_settingsLoaded = false;
    bool m_talentsInitialized = false;
    bool m_busy = false;
};

#endif
