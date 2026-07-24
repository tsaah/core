/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef USE_ACHIEVEMENTS

#include "Chat.h"
#include "Player.h"

// GM commands for the achievement system, distinct from the player-facing, read-only
// `.achievements get*` data-export command table: `.achievement` (singular) grants/re-evaluates
// achievements, `.achievements` (plural) only ever dumps data for a companion addon.

bool ChatHandler::HandleAchievementAddCommand(char* args)
{
    uint32 achievementId = 0;
    if (!ExtractUInt32(&args, achievementId))
        return false;

    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    AchievementEntry const* achievement = sAchievementStore.LookupEntry<AchievementEntry>(achievementId);
    if (!achievement)
    {
        PSendSysMessage("Achievement %u not found.", achievementId);
        SetSentErrorMessage(true);
        return false;
    }

    target->CompletedAchievement(achievement);
    return true;
}

bool ChatHandler::HandleAchievementCheckAllCommand(char* args)
{
    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    target->CheckAllAchievementCriteria();
    return true;
}

#endif
