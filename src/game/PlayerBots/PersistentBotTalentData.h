// Persistent bot talent builds (data only). Generated/validated against 1.12.1 Talent.dbc + TalentTab.dbc.
// link = level-60 target, per-tab digit strings joined by '-' (tab order: TalentTab OrderIndex; Mage: Arcane, Fire, Frost).
// Digit i of a tab = target rank of the i-th talent when the tab's talents are sorted by (Tier/Row, Column) ascending
// (cmangos TalentSpec::sortTalentMap order); trailing zeros omitted; empty segment allowed.
// NOTE: TalentTab.dbc has OrderIndex 0 for both mage Arcane(81) and Fire(41); Fire is treated as tab 1 (cmangos hack).
#ifndef MANGOS_PERSISTENTBOTTALENTDATA_H
#define MANGOS_PERSISTENTBOTTALENTDATA_H

#include "Common.h"
#include "SharedDefines.h"

struct PersistentBotTalentBuild { uint8 classId; uint8 tab; char const* name; char const* link; bool isDefault; };

static PersistentBotTalentBuild const g_persistentBotTalentBuilds[] =
{
    { CLASS_WARRIOR, 0, "arms", "303050213520105001-0505300502", true }, // cmangos 1.0 pve arms
    { CLASS_WARRIOR, 1, "fury", "30305001302-05050005525010051", false }, // cmangos 1.1 pve fury
    { CLASS_WARRIOR, 2, "protection", "05005001-05-55250110530101051", false }, // cmangos 1.2 pve prot
    { CLASS_PALADIN, 0, "holy", "0550312152105-503251000131", false }, // cmangos 2.2 pve heal holy
    { CLASS_PALADIN, 1, "protection", "500501-053050335001051-05004", false }, // cmangos 2.6 pvp tank prot
    { CLASS_PALADIN, 2, "retribution", "550001-503-542300512003151", true }, // cmangos 2.0 pve dps ret
    { CLASS_HUNTER, 0, "beastmastery", "5000322050521231-054510005", true }, // cmangos 3.3 pve dps bm
    { CLASS_HUNTER, 1, "marksmanship", "53000200505-05351030503051", false }, // cmangos 3.4 pve dps mm
    { CLASS_HUNTER, 2, "survival", "-5105103051-330005241030315", false }, // cmangos 3.5 pvp dps surv
    { CLASS_ROGUE, 0, "assassination", "00532310505105-320305002001-05", false }, // cmangos 4.0 pve dps assassination
    { CLASS_ROGUE, 1, "combat", "00530310503-0230550100050130231", true }, // cmangos 4.1 pve dps combat
    { CLASS_ROGUE, 2, "subtlety", "-0230550100-50225300003212151", false }, // HAND-BUILT hemorrhage build (cmangos has no subtlety); validated vs DBC
    { CLASS_PRIEST, 0, "discipline", "505230130505101-0250510313", false }, // cmangos 5.0 pve heal disc
    { CLASS_PRIEST, 1, "holy", "5012301305001-025051031300055", false }, // cmangos 5.1 pve heal holy
    { CLASS_PRIEST, 2, "shadow", "50520013--5032504103501051", true }, // cmangos 5.2 pve dps shadow
    { CLASS_SHAMAN, 0, "elemental", "550331050002151--05204301005", false }, // cmangos 7.0 pve dps elem
    { CLASS_SHAMAN, 1, "enhancement", "0523015003-5025230104003151", true }, // cmangos 7.8 pvp dps enhan (2hand)
    { CLASS_SHAMAN, 2, "restoration", "-5-550350512553151", false }, // cmangos 7.3 pve heal resto
    { CLASS_MAGE, 0, "arcane", "2300450310031531--053500030013", false }, // cmangos 8.0 pve dps arcane
    { CLASS_MAGE, 1, "fire", "230055000002-5052000123033151-003", false }, // cmangos 8.1 pve dps fire
    { CLASS_MAGE, 2, "frost", "230045200003--05050033132301051", true }, // cmangos 8.8 pve dps frost
    { CLASS_WARLOCK, 0, "affliction", "55002530122010051-2351050102", true }, // cmangos 9.4 pve dps affli
    { CLASS_WARLOCK, 1, "demonology", "35000232122-2050310152501051", false }, // cmangos 9.7 pvp dps demo (soul link/nightfall)
    { CLASS_WARLOCK, 2, "destruction", "35000231122-001-5052005102005141", false }, // cmangos 9.11 pvp dps destro (conflagrate)
    { CLASS_DRUID, 0, "balance", "0143503002551351--5005021", false }, // cmangos 11.9 balance
    { CLASS_DRUID, 1, "feral", "014005301-5500021323202151-05", true }, // cmangos 11.0 pve dps feral
    { CLASS_DRUID, 2, "restoration", "0143002002--505503105315051", false }, // cmangos 11.2 pve dps resto (swiftmend)
};

#endif
