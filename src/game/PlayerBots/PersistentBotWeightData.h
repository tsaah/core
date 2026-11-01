// Persistent bot stat weights (data only). Source: cmangos playerbots sql/world/classic/ai_playerbot_weightscales.sql
// (ai_playerbot_weightscale_data). Per entry, every value is divided by that scale's largest of str/agi/sta/int/spi
// so the primary stat = 1.0. Units: per 1 point of stat/armor/AP/spell damage/healing/defense/block value/mp5;
// per 1% for hit/crit/spell hit/spell crit; per 1 weapon DPS for melee/ranged dps. Fields without a cmangos
// equivalent (RAP except hunter, where RAP = AP) are 0. Dodge/parry/expertise/penetration/haste rows are dropped.
#ifndef MANGOS_PERSISTENTBOTWEIGHTDATA_H
#define MANGOS_PERSISTENTBOTWEIGHTDATA_H

#include "Common.h"
#include "SharedDefines.h"

enum PersistentBotWeightField
{
    PBW_STR, PBW_AGI, PBW_STA, PBW_INT, PBW_SPI, PBW_ARMOR, PBW_AP, PBW_RAP, PBW_SP, PBW_HEAL, PBW_HIT, PBW_CRIT, PBW_SPELL_HIT, PBW_SPELL_CRIT, PBW_MP5, PBW_DEFENSE, PBW_BLOCK, PBW_MELEE_DPS, PBW_RANGED_DPS, PBW_MAX
};

struct PersistentBotStatWeights { uint8 classId; uint8 tab; float w[PBW_MAX]; };

static PersistentBotStatWeights const g_persistentBotStatWeights[] =
{
    { CLASS_WARRIOR, 0, { 1.0f, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 10.0f, 10.0f, 0, 0, 0, 0, 0, 7.0f, 0 } }, // warrior arms: cmangos weightscale id 1, norm /4
    { CLASS_WARRIOR, 1, { 1.0f, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 10.0f, 10.0f, 0, 0, 0, 0, 0, 7.0f, 0 } }, // warrior fury: cmangos weightscale id 2, norm /4
    { CLASS_WARRIOR, 2, { 1.0f, 0.571f, 0.857f, 0, 0, 0, 0.571f, 0, 0, 0, 14.286f, 11.571f, 0, 0, 0, 0.143f, 0.429f, 4.0f, 0 } }, // warrior protection: cmangos weightscale id 3, norm /7
    { CLASS_PALADIN, 0, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 0, 1.0f, 0, 0, 0, 20.0f, 3.0f, 0, 0, 0, 0 } }, // paladin holy: cmangos weightscale id 4, norm /5
    { CLASS_PALADIN, 1, { 0.167f, 0.833f, 1.0f, 0.167f, 0, 0.167f, 1.0f, 0, 0.333f, 0, 0.167f, 0.5f, 0.167f, 0, 0, 1.333f, 0.167f, 0, 0 } }, // paladin protection: cmangos weightscale id 5, norm /6
    { CLASS_PALADIN, 2, { 1.0f, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 10.0f, 7.5f, 0, 0, 0, 0, 0, 7.0f, 0 } }, // paladin retribution: cmangos weightscale id 6, norm /4
    { CLASS_HUNTER, 0, { 0, 1.0f, 0, 0, 0, 0, 0.25f, 0.25f, 0, 0, 5.5f, 7.25f, 0, 0, 0, 0, 0, 0, 3.5f } }, // hunter beastmastery: cmangos weightscale id 7, norm /4
    { CLASS_HUNTER, 1, { 0, 1.0f, 0, 0, 0, 0, 0.25f, 0.25f, 0, 0, 5.5f, 7.25f, 0, 0, 0, 0, 0, 0, 3.5f } }, // hunter marksmanship: cmangos weightscale id 8, norm /4
    { CLASS_HUNTER, 2, { 0, 1.0f, 0, 0, 0, 0, 0.25f, 0.25f, 0, 0, 5.5f, 7.25f, 0, 0, 0, 0, 0, 0, 3.5f } }, // hunter survival: cmangos weightscale id 9, norm /4
    { CLASS_ROGUE, 0, { 0.5f, 1.0f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 9.0f, 11.5f, 0, 0, 0, 0, 0, 2.5f, 0 } }, // rogue assassination: cmangos weightscale id 10, norm /4
    { CLASS_ROGUE, 1, { 0.5f, 1.0f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 9.0f, 11.5f, 0, 0, 0, 0, 0, 2.5f, 0 } }, // rogue combat: cmangos weightscale id 11, norm /4
    { CLASS_ROGUE, 2, { 0.5f, 1.0f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 9.0f, 11.5f, 0, 0, 0, 0, 0, 2.5f, 0 } }, // rogue subtlety: cmangos weightscale id 12, norm /4
    { CLASS_PRIEST, 0, { 0, 0, 0, 0.971f, 1.0f, 0, 0, 0, 0.853f, 0, 0, 0, 0, 0.706f, 2.941f, 0, 0, 0, 0 } }, // priest discipline: cmangos weightscale id 13, norm /34
    { CLASS_PRIEST, 1, { 0, 0, 0, 0.971f, 1.0f, 0, 0, 0, 0, 0.853f, 0, 0, 0, 0, 2.941f, 0, 0, 0, 0 } }, // priest holy: cmangos weightscale id 14, norm /34
    { CLASS_PRIEST, 2, { 0, 0, 0, 1.0f, 0.5f, 0, 0, 0, 5.0f, 0, 0, 0, 50.0f, 12.0f, 0.5f, 0, 0, 0, 0 } }, // priest shadow: cmangos weightscale id 15, norm /2
    { CLASS_SHAMAN, 0, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 10.0f, 0, 0, 0, 100.0f, 80.0f, 0, 0, 0, 0, 0 } }, // shaman elemental: cmangos weightscale id 20, norm /1
    { CLASS_SHAMAN, 1, { 1.0f, 0.5f, 0, 0, 0, 0, 8.75f, 0, 0, 0, 12.0f, 11.75f, 0, 0, 0, 0, 0, 7.0f, 0 } }, // shaman enhancement: cmangos weightscale id 21, norm /4
    { CLASS_SHAMAN, 2, { 0, 0, 0, 1.0f, 0.05f, 0, 0, 0, 0.85f, 0.85f, 0, 0, 0, 0, 5.0f, 0, 0, 0, 0 } }, // shaman restoration: cmangos weightscale id 22, norm /20
    { CLASS_MAGE, 0, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 7.0f, 0, 0, 0, 100.0f, 81.0f, 0, 0, 0, 0, 0 } }, // mage arcane: cmangos weightscale id 23, norm /1
    { CLASS_MAGE, 1, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 7.0f, 0, 0, 0, 100.0f, 81.0f, 0, 0, 0, 0, 0 } }, // mage fire: cmangos weightscale id 24, norm /1
    { CLASS_MAGE, 2, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 7.0f, 0, 0, 0, 100.0f, 81.0f, 0, 0, 0, 0, 0 } }, // mage frost: cmangos weightscale id 25, norm /1
    { CLASS_WARLOCK, 0, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 5.0f, 0, 0, 0, 100.0f, 65.0f, 0, 0, 0, 0, 0 } }, // warlock affliction: cmangos weightscale id 26, norm /1
    { CLASS_WARLOCK, 1, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 5.0f, 0, 0, 0, 100.0f, 65.0f, 0, 0, 0, 0, 0 } }, // warlock demonology: cmangos weightscale id 27, norm /1
    { CLASS_WARLOCK, 2, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 5.0f, 0, 0, 0, 100.0f, 65.0f, 0, 0, 0, 0, 0 } }, // warlock destruction: cmangos weightscale id 28, norm /1
    { CLASS_DRUID, 0, { 0, 0, 0, 1.0f, 0, 0, 0, 0, 11.0f, 0, 0, 0, 100.0f, 74.0f, 0, 0, 0, 0, 0 } }, // druid balance: cmangos weightscale id 29, norm /1
    { CLASS_DRUID, 1, { 1.0f, 0.909f, 0, 0, 0, 0, 0.455f, 0, 0, 0, 6.182f, 9.091f, 0, 0, 0, 0, 0, 0, 0 } }, // druid feral: cmangos weightscale id 32, norm /11
    { CLASS_DRUID, 2, { 0, 0, 0, 0.6f, 1.0f, 0, 0, 0, 2.0f, 2.0f, 0, 0, 0, 20.0f, 6.0f, 0, 0, 0, 0 } }, // druid restoration: cmangos weightscale id 31, norm /5
};

#endif
