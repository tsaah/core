#ifndef MANGOS_PERSISTENTBOTECONOMY_H
#define MANGOS_PERSISTENTBOTECONOMY_H

#include "Common.h"
#include "ObjectGuid.h"
#include <ctime>
#include <vector>

class PersistentBotAI;

struct PersistentBotSkippedSpell
{
    uint32 spellId;
    uint32 cost;
};

struct PersistentBotEconomyState
{
    // Class trainer
    time_t nextTrainerCheck = 0;
    ObjectGuid lastTrainerGuid;
    uint32 lastTrainerLevel = 0;
    uint32 lastTrainerSpellCount = 0;
    std::vector<PersistentBotSkippedSpell> trainerSkipped;

    // Vendors
    enum VendorStage : uint8
    {
        VENDOR_IDLE = 0,
        VENDOR_WALKING = 1,
        VENDOR_SOLD = 2,
    };
    uint8 vendorStage = VENDOR_IDLE;
    ObjectGuid vendorGuid;
    time_t vendorDeadline = 0;
    time_t vendorMoveIssued = 0;
    float vendorMoveDist = 0.0f;
    time_t nextVendorCheck = 0;
    time_t nextVisitAllowed = 0;
    time_t nextRepairAllowed = 0;
    uint32 vendorSold = 0;
    uint32 vendorMoneyAtSell = 0;

    // Grimoires: pet spell taught by the last grimoire used, reported as "Taught" once the demon knows it
    uint32 grimoireLearnSpell = 0;
    time_t nextGrimoireUse = 0;
};

// Reusable vendor trip (walk to vendorGuid, sell junk, repair, buy reagents/ammo). State lives in
// PersistentBotAI::m_economyState (vendorStage/vendorGuid/vendorDeadline/...).
bool PersistentBotVendorStartTrip(PersistentBotAI* pAI, ObjectGuid vendorGuid);
bool PersistentBotVendorUpdateTrip(PersistentBotAI* pAI); // true while the trip is in progress

#endif
