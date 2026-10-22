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

Requires ARK: Survival Evolved with ArkApi 3.56.
