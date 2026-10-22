# ArkEventsRevival

An ARK: Survival Evolved (ArkApi 3.56) server plugin that brings back the seasonal events - Fear Evolved,
Turkey Trial, Winter Wonderland, Love Evolved, Eggcellent Adventure, ARKaeology, Summer Bash, ARK's
Anniversary and New Year's - with event creatures, bosses, drops and Discord announcements.

## Install
1. Copy this `ArkEventsRevival` folder into your server's `ShooterGame/Binaries/Win64/ArkApi/Plugins/`.
2. Each map has its own config: `config_<Map>.json` (`config_TheIsland.json`, `config_Gen2.json`, ...).
   The plugin works out which map it is on at start-up and loads the matching file, so one folder can be
   copied to every server in a cluster.
3. To post events to Discord, put your own webhook URL in each config under `Discord` -> `WebhookUrl`.
   **The webhook URLs in this repo are blank on purpose.**
4. Start the server, or run `plugins.load ArkEventsRevival` in the console.

## Configuring
`config-commented.jsonc` explains every setting (it is documentation only; the plugin reads the
`config_<Map>.json` files). Creatures, bosses, levels, crate loot and spawn regions are all editable per map and
per event, and a mod map can be added by copying an existing file and renaming it to the map's name.

Event creatures never spawn in water or in a cave. Events start on their own dates, or can be forced with
`-event=<name>` on the startup line.

## How it works

**Events.** With `AutoDetectActiveEvent` on, each event runs by itself between its own dates (`AutoDateRanges` in
every config, set to each event's most recent official run). You can also force one: set `ActiveEvent` in the
config, start the server with `-event=<name>`, or use the console commands below. `ForceOfficialServerFlag`
(off by default) makes ARK's own "official server" check true while an event runs, which is what unlocks the
event cosmetics and recipes.

**Wild event creatures.** The plugin learns real spawn points from the game's own wild spawns, kept per region.
Each event lists creatures per region in `Spawns` -> `Regions`: `SpawnLike:<native creature>` spawns where that
native creature spawns on the map, a named region uses that region's points, and `Anywhere` uses any of them.
Spawns are paced by `CheckIntervalSeconds` and `SpawnOneEverySeconds`, and a small chance (`OtherRegionChance`,
10% by default) sends a spawn to a different region instead, so they turn up everywhere but far less than at home.
Creatures get a random level in steps of 5 between `MinLevel` and `MaxLevel` (wyverns up to `WyvernMaxLevel`);
bosses use their own `Level`.

**Where they appear.** Always on the ground: the plugin finds the real surface, checks there is room, spaces
spawns apart (`SpawnSpacing`), keeps clear of structures (`KeepAwayFromStructures`) and the map edge, and nudges a
creature to a clear spot if it would overlap another. Nothing spawns in water or in a cave. Caves are spotted by
region name, by biome zones named for a cave, and by the game's cave building/flyer-block volumes.

**Kill drops.** Every few kills (`EveryKills`) a supply crate holding random event items (`KillDrops`) drops where
the creature died. It finds the ground first, so a flyer killed high up still drops its crate on the ground.

**Bosses.** Each boss spawns at its `SpawnAtHours` (in-game hours) in an arena it clears (`ClearRadius`,
`ArenaFoundations`), with minions (`MinionClasses`, `MaxMinions`, `MinionRespawnSeconds`). It is pulled back if it
wanders off (`LeashFoundations`), heals if nobody damages it for `HealAfterMinutesNoDamage`, can have a shield dome,
and drops `DefeatCrates` when it dies. Players joining a map are told where its boss is. The messages for all of this
are in `Announcements`, and `Discord` posts them to your webhook if you set one.

**Console commands** (run on the server): `ArkEventsRevival.Reload` re-reads the configs, `StartEvent` and
`StopEvent` force or end an event, `SpawnBossNow` spawns a boss for testing, `ListRegions` and `ShowSpawnLike`
show what the spawn pool holds, `CaveCheck` says whether the spot you stand on counts as a cave, and `DomeSize`
helps size a boss's shield dome. Turn on `DebugMode` in a config to log what the plugin is doing.

The wild-creature remap is marked experimental in `PluginInfo.json`: try it with `DebugMode` on before relying on it.

Requires ARK: Survival Evolved with ArkApi 3.56.
