-- Persistent bots: apply manually to the CHARACTERS database (not world/realmd).
-- Safe to run more than once: creates the table, or adds the columns missing from an existing one.
-- mangosd disables the .persistentbot commands until this table has all of its columns.
-- owner_guid: character guid of the owner (the one that added the bot); active: 1 = restored when the owner logs in.

CREATE TABLE IF NOT EXISTS `persistent_bot` (
  `guid` int(11) unsigned NOT NULL,
  `spec_tab` tinyint(3) unsigned NOT NULL DEFAULT 255,
  `auto_talents` tinyint(3) unsigned NOT NULL DEFAULT 1,
  `owner_guid` int(11) unsigned NOT NULL DEFAULT 0,
  `active` tinyint(3) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

ALTER TABLE `persistent_bot` ADD COLUMN IF NOT EXISTS `owner_guid` int(11) unsigned NOT NULL DEFAULT 0;
ALTER TABLE `persistent_bot` ADD COLUMN IF NOT EXISTS `active` tinyint(3) unsigned NOT NULL DEFAULT 0;
