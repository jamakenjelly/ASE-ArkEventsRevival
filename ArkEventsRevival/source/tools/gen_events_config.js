// Generates ArkEventsRevival's config_<Map>.json - one complete config per map.
//
// Event content comes from event_content.json (next to this script): every
// ARK: Survival Evolved year of each event on the new wiki (ark.wiki.gg/wiki/Events),
// creatures and items merged, with blueprint paths from its Creature IDs and
// Item IDs pages. ARK: Survival Ascended events are left out.
// Usage: node gen_events_config.js <plugin folder>
const fs = require('fs');
const path = require('path');

const content = JSON.parse(fs.readFileSync(path.join(__dirname, 'event_content.json'), 'utf8'));
// Some wiki paths have a stray space ("/Game/ PrimalEarth/...").
const bp = p => `Blueprint'${p.replace(/\s+/g, '')}'`;
const creaturePath = (event, name) => {
  const hit = content[event].c.find(([n]) => n === name) ||
              Object.values(content).flatMap(v => v.c).find(([n]) => n === name);
  if (!hit) throw new Error('no path for creature ' + name);
  return bp(hit[1]);
};

const WhiteCrate = bp('/Game/PrimalEarth/Structures/SupplyCrate_Level03.SupplyCrate_Level03');
const GiftCrate  = bp('/Game/PrimalEarth/Structures/SupplyCrate_Gift.SupplyCrate_Gift');
const RedCrate   = bp('/Game/PrimalEarth/Structures/SupplyCrate_Level60.SupplyCrate_Level60');
// The event crates (Dino Hat / Player Hat supply crates, in the game files at
// PrimalEarth/Structures). If one won't load or spawn, EventCrate is used instead.
const DinoHatCrate   = bp('/Game/PrimalEarth/Structures/SupplyCrate_DinoHat.SupplyCrate_DinoHat');
const PlayerHatCrate = bp('/Game/PrimalEarth/Structures/SupplyCrate_PlayerHat.SupplyCrate_PlayerHat');
// The fallback crate for both.
const EventCrate = bp('/Game/PrimalEarth/Structures/SupplyCrate_UnderwaterCaveTier3.SupplyCrate_UnderwaterCaveTier3');

// The cluster's maps, as the game names them (case and "_P" don't matter).
const MAPS = ['TheIsland', 'TheCenter', 'ScorchedEarth', 'Aberration', 'Extinction', 'Genesis',
              'Gen2', 'Ragnarok', 'Valguero', 'CrystalIsles', 'Fjordur', 'LostIsland'];

// ---- items: how often each kind turns up ----
// [MinQuantity, MaxQuantity, Chance]. In a Drops crate Chance is rolled per
// crate; in a boss's DefeatCrates it is the item's share of the picks.
const CORE = {
  'Dino Candy Corn': [1, 3, 0.8], 'Dinosaur Bone': [5, 15, 1.0], 'Wishbone': [1, 5, 1.0],
  'Thanksgiving Candy': [1, 3, 0.8], 'Mistletoe': [1, 3, 1.0], 'Coal': [1, 5, 1.0],
  'Festive Dino Candy': [1, 3, 0.8], "Box o' Chocolates": [1, 3, 1.0], 'Valentines Dino Candy': [1, 3, 0.8],
  'Bunny Egg': [1, 3, 1.0], 'Festive Dino Candy (Easter)': [1, 3, 0.8], 'Summer Swirl Taffy': [1, 3, 0.8],
  'Cake Slice': [1, 5, 1.0], 'Birthday Candle': [1, 3, 0.6], 'Element': [1, 5, 0.8],
  'Summon DodoRex': [1, 1, 0.03],
};
const STRUCTURE = /^(Pumpkin|Scarecrow|Stolen Headstone|Gift Box|Holiday Lights|Holiday Stocking|Holiday Tree|Snowman|Wreath|Birthday Cake|ARK Anniversary Surprise Cake)$/;
const CHIBI = /Chibi|Teeny Tiny Titano/;
const kindOf = name => CORE[name] ? 'core' : STRUCTURE.test(name) ? 'structure' : /Egg$/.test(name) ? 'egg'
                     : CHIBI.test(name) ? 'chibi' : 'skin';
const RARITY = { structure: [1, 2, 0.3], egg: [1, 1, 0.05], chibi: [1, 1, 0.02], skin: [1, 1, 0.04] };

const item = (name, p, scale = 1) => {
  const [min, max, chance] = CORE[name] || RARITY[kindOf(name)];
  return { Class: bp(p), MinQuantity: min, MaxQuantity: max, Chance: Math.round(chance * scale * 1000) / 1000 };
};
const itemsOf = event => content[event].i.map(([n, p]) => item(n, p));

// New Year's (2022) brought back the skins and chibis of every earlier event.
const newYearItems = (() => {
  const seen = new Set();
  const list = content.NewYear.i.map(([n, p]) => { seen.add(n); return item(n, p); });
  for (const ev of ['FearEvolved', 'TurkeyTrial', 'WinterWonderland', 'vday', 'easter', 'birthday', 'Summer', 'ARKaeology'])
    for (const [n, p] of content[ev].i)
      if (!seen.has(n) && (kindOf(n) === 'skin' || kindOf(n) === 'chibi')) { seen.add(n); list.push(item(n, p, 0.4)); }
  return list;
})();

// ---- creatures ----
const COUNTS = {
  'Zomdodo': [2, 10], 'Skeletal Raptor': [2, 8], 'Skeletal Carnotaurus': [1, 5], 'Skeletal Trike': [2, 8],
  'Skeletal Stego': [1, 6], 'Skeletal Bronto': [1, 4], 'Skeletal Rex': [1, 3], 'Skeletal Giganotosaurus': [0, 1],
  'Skeletal Quetzal': [1, 3], 'Skeletal Jerboa': [2, 8], 'Direwolf Ghost': [1, 6], 'Rex Ghost': [1, 3],
  'Mantis Ghost': [1, 5], 'Basilisk Ghost': [1, 3], 'Bulbdog Ghost': [1, 6], 'Snow Owl Ghost': [1, 4],
  'Surface Reaper King Ghost': [0, 1], 'Zombie Fire Wyvern': [0, 2], 'Zombie Lightning Wyvern': [0, 2],
  'Zombie Poison Wyvern': [0, 2], 'Onyc': [2, 8], 'Araneo': [2, 8], 'Turkey': [3, 15], 'GachaClaus': [1, 3],
  'Bunny Dodo': [3, 12], 'Bunny Oviraptor': [2, 8], 'Party Dodo': [3, 12], 'Tek Rex': [0, 2],
};
const BOSSES = new Set(['DodoRex', 'Dodo Wyvern', 'Bone Fire Wyvern', 'Super Turkey']);
const BOSS_SETUP = {
  'DodoRex':          { name: 'The DodoRex', minutes: 60, minions: ['Zomdodo', 'Skeletal Raptor', 'Direwolf Ghost'] },
  'Dodo Wyvern':      { name: 'The DodoWyvern', minutes: 90, minions: ['Zombie Fire Wyvern', 'Zombie Lightning Wyvern', 'Zombie Poison Wyvern'] },
  'Bone Fire Wyvern': { name: 'A Skeletal Fire Wyvern', level: 150, minutes: 120, minions: ['Skeletal Raptor', 'Skeletal Carnotaurus', 'Direwolf Ghost'] },
  'Super Turkey':     { name: 'The Super Turkey', minutes: 60, minions: ['Turkey'] },
};

// Every wild event creature: once killed down to 10 alive, topped back up to
// a random 20-30 (per creature type, per map). COUNTS above is kept for
// reference if some types should differ again.
// Filled one at a time up to 30; once killed down to 5, a 30-minute wait,
// then back up to 30.
const WILD_MIN = 5, WILD_RESPAWN_TO = 30, WILD_MAX = 30, WILD_RESPAWN_DELAY_MINUTES = 30;
// Where each event creature's ordinary counterpart spawns - the plugin puts
// it where the game's own wild ones of that class appear (class name part,
// matched case-insensitively). ONLY there - a map without that creature gets
// none of this event dino (Reaper/Basilisk/Bulbdog ghosts stay on the maps
// those live on). The Reaper one is the wild Reaper Queen class, so
// Extinction's corrupted Reapers don't count.
const SPAWN_LIKE = {
  'Zomdodo': 'Dodo_Character_BP', 'Bunny Dodo': 'Dodo_Character_BP', 'Party Dodo': 'Dodo_Character_BP', 'Turkey': 'Dodo_Character_BP',
  'Skeletal Raptor': 'Raptor_Character_BP', 'Skeletal Carnotaurus': 'Carno_Character_BP', 'Skeletal Trike': 'Trike_Character_BP',
  'Skeletal Stego': 'Stego_Character_BP', 'Skeletal Bronto': 'Sauropod_Character_BP', 'Skeletal Rex': 'Rex_Character_BP',
  'Skeletal Giganotosaurus': 'Gigant_Character_BP', 'Skeletal Quetzal': 'Quetz_Character_BP', 'Skeletal Jerboa': 'Jerboa_Character_BP',
  'Direwolf Ghost': 'Direwolf_Character_BP', 'Rex Ghost': 'Rex_Character_BP', 'Mantis Ghost': 'Mantis_Character_BP',
  'Basilisk Ghost': 'Basilisk_Character_BP', 'Bulbdog Ghost': 'LanternPug_Character_BP', 'Snow Owl Ghost': 'Owl_Character_BP',
  'Surface Reaper King Ghost': 'Xenomorph_Character_BP_Female', 'Zombie Fire Wyvern': 'Wyvern_Character_BP',
  'Zombie Lightning Wyvern': 'Wyvern_Character_BP', 'Zombie Poison Wyvern': 'Wyvern_Character_BP',
  'Onyc': 'Bat_Character_BP', 'Araneo': 'SpiderS_Character_BP', 'GachaClaus': 'Gacha_Character_BP',
  'Bunny Oviraptor': 'Oviraptor_Character_BP', 'Tek Rex': 'Rex_Character_BP',
};
const dino = (event, name) => {
  const [min, max] = [WILD_MIN, WILD_MAX];
  return { Class: creaturePath(event, name), MinCount: min, RespawnTo: WILD_RESPAWN_TO, MaxCount: max,
           RespawnDelayMinutes: WILD_RESPAWN_DELAY_MINUTES, _like: SPAWN_LIKE[name] };
};

const FLYERS = new Set(['Skeletal Quetzal', 'Zombie Fire Wyvern', 'Zombie Lightning Wyvern', 'Zombie Poison Wyvern', 'Snow Owl Ghost']
  .map(n => creaturePath('FearEvolved', n)));

// Dinos grouped by where they go: "SpawnLike:<class>" groups (where the
// game's own creatures of that class spawn), plus the given base groups.
const byWhere = (dinos, base) => {
  const regions = {};
  for (const d of dinos) {
    const key = d._like ? 'SpawnLike:' + d._like : 'Anywhere';
    const { _like, ...entry } = d;
    (regions[key] = regions[key] || group()).Dinos.push(entry);
  }
  // Ground (non-flying) event dinos ALSO roam anywhere on land, as their own
  // smaller entry - but only on maps where their normal version is native
  // (RequireNative). Flyers stay where their normal versions live.
  for (const d of dinos) {
    if (!d._like || FLYERS.has(d.Class)) continue;
    const { _like, ...entry } = d;
    (regions.Anywhere = regions.Anywhere || group()).Dinos.push({ ...entry, MinCount: 2, RespawnTo: 15, MaxCount: 15, RequireNative: _like });
  }
  for (const [key, g] of Object.entries(base)) {
    const into = regions[key] = regions[key] || group();
    into.Dinos.push(...g.Dinos); into.Drops.push(...g.Drops); into.Bosses.push(...g.Bosses);
  }
  return regions;
};
// The events' own DodoRex versions (the ones the official event managers
// spawned), found in the server's game files.
const EVENT_BOSS_CLASS = {
  'FearEvolved|DodoRex': bp('/Game/PrimalEarth/Dinos/DodoRex/DodoRex_Character_BP_FearEvolved.DodoRex_Character_BP_FearEvolved'),
  'TurkeyTrial|DodoRex': bp('/Game/PrimalEarth/Dinos/DodoRex/DodoRex_Character_BP_TurkeyTrials.DodoRex_Character_BP_TurkeyTrials'),
};
const boss = (event, name, nightOnly, loot) => {
  const s = BOSS_SETUP[name];
  return {
    Class: EVENT_BOSS_CLASS[event + '|' + name] || creaturePath(event, name), Name: s.name, Amount: 1,
    // Appears only at these in-game hours (11:00 and 12:00), and only if it
    // isn't already alive.
    SpawnAtHours: [11, 12], Level: s.level || 1,
    MinionClasses: s.minions.map(m => creaturePath(event, m)), MinionLevel: 60,
    // 5 minions around the boss, one at a time; when any die, 2 minutes, then
    // back to 5 one at a time - until the boss is dead.
    MaxMinions: 5, RandomMinions: true, WaveCount: 10, MinionRespawnSeconds: 120, ClearRadius: 1000,
    // The dome is 100 foundations out. Crossing it starts a 10 s countdown
    // (players around are warned); still outside at 0, or past 130 foundations
    // or in water, the boss is sent back to the center. Was: past 130 foundations
    // (or in water) the boss is put back at its spawn point.
    // Flying bosses (the wyverns) get an arena half as big again: 150 foundations, leash 195.
    ArenaFoundations: /wyvern/i.test(name) ? 150 : 100, LeashFoundations: /wyvern/i.test(name) ? 195 : 130, ReturnCountdownSeconds: 10,
    // A shield dome over the arena - see-through, players and tames walk in and
    // out; attacks from outside do nothing. DomeColor = red, green, blue (0-1).
    ShieldDome: false, // no orbital-drop dome - the arena is shown by the raid-base style rings
    // Untouched for 10 minutes, the boss heals back to full.
    HealAfterMinutesNoDamage: 10,
    DefeatCrates: { Crate: RedCrate, FallbackCrate: EventCrate, Count: 10, ItemsPerCrate: 10, IncludeCrateLoot: false, RingRadius: 700, AroundArena: true, Items: loot },
  };
};
// 5 crates of a kind, each with 10 event items, placed beside where the
// event's dinos spawn (anywhere on land for events without dinos); back up to
// 5 once looted down to 1.
const crate = (cls, items) => ({ Crate: cls, FallbackCrate: EventCrate, MinCount: 1, RespawnTo: 5, MaxCount: 5, NearEventDinos: true,
                                 ItemsPerCrate: 10, IncludeCrateLoot: false, Items: items });

// One region group - all three lists always present, empty when unused, so
// there is a place to add to.
const group = (dinos = [], drops = [], bosses = []) => ({ Dinos: dinos, Drops: drops, Bosses: bosses });
const range = (sm, sd, em, ed) => [{ StartMonth: sm, StartDay: sd, EndMonth: em, EndDay: ed }];

// Ordinary creatures the wiki pages mention (Fear Evolved's cave Onycs and
// Araneos that came above ground, the Tek Rex released during ARKaeology) -
// only EVENT creatures are spawned, so these are left out.
const NOT_EVENT_CREATURES = new Set(['Onyc', 'Araneo', 'Tek Rex']);

// Everything the wiki lists for the event: wild creatures, bosses, one crate type.
function fullEvent(event, crateClass, { nightBosses = false, extraCreatures = [], items = null } = {}) {
  const names = [...content[event].c.map(([n]) => n), ...extraCreatures].filter(n => !NOT_EVENT_CREATURES.has(n));
  const loot = items || itemsOf(event);
  const wild = names.filter(n => !BOSSES.has(n)).map(n => dino(event, n));
  const bosses = names.filter(n => BOSSES.has(n)).map(n => boss(event, n, nightBosses, loot));
  // No random drops any more - loot comes from kills (KillDrops) and bosses.
  return { wild, drops: [], bosses, loot };
}
const everywhere = e => Object.assign(() => byWhere(e.wild, { Anywhere: group([], e.drops, e.bosses) }), { loot: e.loot });

// Events that had no event creatures of their own get these, so there is
// something to hunt for the kill drops.
const DROP_DINOS = ['Party Dodo', 'Bunny Dodo', 'Bunny Oviraptor'];

// Each map's own supply crates (ARK wiki Beacon IDs; no cave, beaver dam,
// artifact, deep-sea, orbital or dungeon crates). A kill drop is one of these
// at random, with only event items inside.
const P = n => bp('/Game/PrimalEarth/Structures/' + n + '.' + n);
const STANDARD = ['SupplyCrate_Level03', 'SupplyCrate_Level15', 'SupplyCrate_Level25', 'SupplyCrate_Level35',
                  'SupplyCrate_Level45', 'SupplyCrate_Level60'].map(P);
const SE = n => bp('/Game/ScorchedEarth/CoreBlueprints/ItemLootSets/SupplyCrates/' + n + '.' + n);
const AB = n => bp('/Game/Aberration/CoreBlueprints/ItemLootSets/SupplyCrate/' + n + '.' + n);
const LI = n => bp('/Game/LostIsland/Structures/SupplyCrates/' + n + '.' + n);
const MAP_CRATES = {
  ScorchedEarth: ['SupplyCrate_Level03_ScorchedEarth', 'SupplyCrate_Level15_ScorchedEarth', 'SupplyCrate_Level30_ScorchedEarth',
                  'SupplyCrate_Level45_ScorchedEarth', 'SupplyCrate_Level55_ScorchedEarth', 'SupplyCrate_Level70_ScorchedEarth'].map(SE),
  Aberration: ['SupplyCrate_Level35_Aberrant_Surface', 'SupplyCrate_Level50_Aberrant_Surface',
               'SupplyCrate_Level65_Aberrant_Surface', 'SupplyCrate_Level80_Aberrant_Surface'].map(AB),
  LostIsland: [...STANDARD, ...['SupplyCrate_Level45_LostIsland', 'SupplyCrate_Level60_LostIsland'].map(LI)],
};
const cratesFor = m => MAP_CRATES[m] || STANDARD;

const fe = fullEvent('FearEvolved', EventCrate, { nightBosses: true });

// Maps with their own wyvern spawn regions (Dev Kit spawners), and the
// Fear Evolved creatures that belong there.
const WYVERN_MAPS = new Set(['ScorchedEarth', 'Ragnarok', 'Valguero', 'CrystalIsles', 'Fjordur', 'LostIsland', 'Gen2']);
const WYVERN_CLASSES = new Set(['Zombie Fire Wyvern', 'Zombie Lightning Wyvern', 'Zombie Poison Wyvern', 'Dodo Wyvern', 'Bone Fire Wyvern']
  .map(n => creaturePath('FearEvolved', n)));
const skeletalNames = ['Skeletal Raptor', 'Skeletal Carnotaurus', 'Skeletal Trike', 'Skeletal Stego', 'Skeletal Bronto',
                       'Skeletal Rex', 'Skeletal Giganotosaurus', 'Skeletal Quetzal', 'Skeletal Jerboa'];

const events = {
  FearEvolved: {
    AutoDateRanges: range(10, 19, 11, 2),
    loot: fe.loot,
    // Each dino goes where its ordinary counterpart spawns (SpawnLike groups -
    // zombie wyverns in the wyvern trenches, skeletal Rexes with the Rexes).
    // The DodoWyvern / Skeletal Fire Wyvern bosses use the "Wyvern" region
    // (every region with "wyvern" in its name) on maps that have one.
    perMap: m => {
      const isWyvern = x => WYVERN_CLASSES.has(x.Class);
      if (!WYVERN_MAPS.has(m)) return byWhere(fe.wild, { Anywhere: group([], fe.drops, fe.bosses) });
      return byWhere(fe.wild, { Wyvern: group([], [], fe.bosses.filter(isWyvern)),
                                Anywhere: group([], fe.drops, fe.bosses.filter(x => !isWyvern(x))) });
    },
  },
  TurkeyTrial:      { AutoDateRanges: range(11, 22, 12, 7), perMap: everywhere(fullEvent('TurkeyTrial', EventCrate)) },
  WinterWonderland: { AutoDateRanges: range(12, 14, 12, 30),  perMap: everywhere(fullEvent('WinterWonderland', EventCrate)) },
  vday:             { AutoDateRanges: range(2, 10, 2, 17),  perMap: everywhere(fullEvent('vday', EventCrate, { extraCreatures: DROP_DINOS })) },
  easter:           { AutoDateRanges: range(4, 4, 4, 24),   perMap: everywhere(fullEvent('easter', EventCrate)) },
  Summer:           { AutoDateRanges: range(7, 1, 7, 26),   perMap: everywhere(fullEvent('Summer', EventCrate, { extraCreatures: DROP_DINOS })) },
  birthday:         { AutoDateRanges: range(6, 2, 6, 6),    perMap: everywhere(fullEvent('birthday', EventCrate)) },
  // Dates are each event's most recent ARK: Survival Evolved run (ark.wiki.gg
  // calendar). Where two are due at once the shorter one runs (see
  // DetectEventForToday) - so Winter Wonderland ends Dec 30 and New Year's (Dec 31 - Jan 1) follows it.
  // ARKaeology (2018) had fossil piles of Dinosaur Bone and bone
  // costumes; the skeletal creatures stand in for the dig.
  ARKaeology:           { AutoDateRanges: range(6, 15, 6, 30), perMap: everywhere(fullEvent('ARKaeology', EventCrate, { extraCreatures: skeletalNames })) },
  NewYear:              { AutoDateRanges: range(12, 31, 1, 1), perMap: everywhere(fullEvent('NewYear', EventCrate, { items: newYearItems, extraCreatures: DROP_DINOS })) },
};

// Boss defeat crates: each map's own double-item red beacon.
const BOSS_CRATE = {
  ScorchedEarth: bp('/Game/ScorchedEarth/CoreBlueprints/ItemLootSets/SupplyCrates/SupplyCrate_Level70_Double_ScorchedEarth.SupplyCrate_Level70_Double_ScorchedEarth'),
  Aberration: bp('/Game/Aberration/CoreBlueprints/ItemLootSets/SupplyCrate/SupplyCrate_Level80_Aberrant_Surface_Double.SupplyCrate_Level80_Aberrant_Surface_Double'),
  LostIsland: bp('/Game/LostIsland/Structures/SupplyCrates/SupplyCrate_Level60_LostIsland_Double.SupplyCrate_Level60_LostIsland_Double'),
  Valguero: bp('/Game/Mods/Valguero/Assets/SupplyCrates/Val_SupplyCrate_Level60_Double.Val_SupplyCrate_Level60_Double'),
};
const bossCrateFor = m => BOSS_CRATE[m] || bp('/Game/PrimalEarth/Structures/SupplyCrate_Level60_Double.SupplyCrate_Level60_Double');
const withBossCrate = (regions, m) => Object.fromEntries(Object.entries(regions).map(([k, grp]) => [k, { ...grp,
  Bosses: grp.Bosses.map(b => ({ ...b, DefeatCrates: { ...b.DefeatCrates, Crate: bossCrateFor(m), FallbackCrate: RedCrate } })) }]));

// The one map whose server posts the Discord reminders (every server runs the
// plugin, so only one may send).
const DISCORD_MAP = 'TheIsland';
const DISCORD_WEBHOOK = 'https://discord.com/api/webhooks/YOUR_ID/YOUR_TOKEN';

// ---- one complete config per map: config_<Map>.json ----
const configFor = m => ({ ArkEventsRevival: {
  Map: m,
  Enabled: true, DebugMode: false, AutoDetectActiveEvent: true,
  // Event creatures never spawn within this distance of a player structure (5000 = 50 m).
  KeepAwayFromStructures: 15000,
  // Nothing spawns within this many GPS units of the map edge (5 = lat/lon 5-95 only).
  KeepAwayFromMapEdge: 5,
  // A Discord message the day before each event starts. Switch it on in ONE
  // map's config only - every server in the cluster runs this plugin.
  Discord: {
    Enabled: m === DISCORD_MAP,
    WebhookUrl: DISCORD_WEBHOOK, // in every config; only DISCORD_MAP has it enabled
    // Sent once the day before; EventStartedMessage once when it starts;
    // EventEndedMessage once the day after its last day. Nothing else.
    EventTomorrowMessage: '**{event}** starts in 24 hours! Admins: add **-event={token}** to the server startup line before the next restart. Remove **-event={token}** again on its end date - it stays on until you do.',
    EventStartedMessage: '**{event}** has started! Event creatures, bosses and loot are live.',
    EventEndedMessage: '**{event}** has ended. Thanks for playing - see you next time!',
  },
  Announcements: {
    ChatSender: 'ARK Events',
    BossSpawned: '{boss} has appeared at {lat}! Defeat it for event loot.',
    BossDefeated: '{boss} has been defeated!',
    BossLeavingArena: '{boss} is leaving the event area! Returning to the center in {seconds}s...',
    PlayerEnteredArena: 'You entered the {boss} event area - you can fight it here!',
    PlayerLeftArena: "You left the {boss} event area - you can't damage it from outside the dome.",
    OutsideArenaNoDamage: "{boss} can't be damaged from here - you must be inside the event range (the red ring)!",
  },
  Events: Object.fromEntries(Object.entries(events).map(([token, e]) => [token, {
    AutoDateRanges: e.AutoDateRanges,
    Spawns: {
      GameWeight: 0.25, Enabled: true, CheckIntervalSeconds: 60, MinLevel: 5, MaxLevel: 150,
      // One crate where each event dino is killed: a random one of this map's
      // supply crates, 2-5 different event items, nothing else.
      // ARK's own buffs every player carries during the event - Fear Evolved's
      // spawns the Pumpkin / Scarecrow / Gravestone mounds around players.
      PlayerBuffs: token === 'FearEvolved'
        ? [bp('/Game/PrimalEarth/CoreBlueprints/Buffs/Buff_FearEvolvedMoundSpawner.Buff_FearEvolvedMoundSpawner')] : [],
      // ARKaeology's fossil piles (the soil-pile container the Fear Evolved
      // mounds are built on), kept around each player with Dinosaur Bone and
      // a chance at the bone costumes.
      Mounds: token === 'ARKaeology' ? {
        Class: bp('/Game/PrimalEarth/Structures/Arkaeology/BonePile_FearEvolved.BonePile_FearEvolved'),
        PerPlayer: 3, MinDistance: 1000, MaxDistance: 3000, MinItems: 1, MaxItems: 3, Items: e.perMap.loot || e.loot || [],
      } : {},
      KillDrops: { Crates: cratesFor(m), FallbackCrate: RedCrate, EveryKills: 3, MinItems: 2, MaxItems: 5, Items: e.loot || e.perMap.loot || [] },
      Regions: withBossCrate(e.perMap(m), m),
    },
  }])),
} });

// Pretty-print, but keep any object/array made only of plain values on ONE
// line - one dino or item per line reads far better than five.
function fmt(v, ind) {
  const pad = '  '.repeat(ind), inner = '  '.repeat(ind + 1);
  const leaf = x => x === null || typeof x !== 'object';
  if (leaf(v)) return JSON.stringify(v);
  if (Array.isArray(v)) {
    if (v.length === 0) return '[]';
    if (v.every(leaf)) return '[' + v.map(x => JSON.stringify(x)).join(', ') + ']';
    return '[\n' + v.map(x => inner + fmt(x, ind + 1)).join(',\n') + '\n' + pad + ']';
  }
  const keys = Object.keys(v);
  if (keys.length === 0) return '{}';
  const oneLine = '{ ' + keys.map(k => JSON.stringify(k) + ': ' + JSON.stringify(v[k])).join(', ') + ' }';
  // Plain-value objects stay on one line - unless they are long prose, like
  // the announcement messages, which read better one per line.
  if (keys.every(k => leaf(v[k])) && (keys.includes('Class') || oneLine.length <= 120)) return oneLine;
  return '{\n' + keys.map(k => inner + JSON.stringify(k) + ': ' + fmt(v[k], ind + 1)).join(',\n') + '\n' + pad + '}';
}

const out = process.argv[2];
for (const m of MAPS) fs.writeFileSync(path.join(out, 'config_' + m + '.json'), fmt(configFor(m), 0) + '\n');
console.log('wrote', MAPS.length, 'config_<Map>.json files to', out);
