// ArkEventsRevival
//
// Revives ARK: Survival Evolved's seasonal events (Fear Evolved, Turkey
// Trial, Winter Wonderland, Love Evolved, Eggcellent Adventure, Summer
// Bash, ARK's Anniversary, ARKaeology) on a private server.
//
// WHAT IT DOES, BY PIECE:
//
// 1. COSMETICS/RECIPES/CHIBIS (native): Blueprint-side checks against the
//    native ActiveEvent FName, which this plugin pins on the game mode and
//    game state - the same thing "-activeevent=" does on an unofficial
//    server. The server is never made to look official (that would stop
//    admins getting in with enablecheats).
//
// 2. EVENT SPAWNS (custom, below): per map, per spawn region (a
//    DinoSpawnEntries... name, or "Anywhere"), config lists event
//    creatures, supply crates filled with event items, and bosses.
//    Creatures and crates are refilled when killed/looted down to their
//    MinCount; bosses come back on a timer, are announced to the server
//    with their coordinates, bring waves of minions, and drop a ring of
//    loot crates when killed - like RaidBaseBuilder, but with no base. None
//    of this uses the native event spawn tables, which are documented as
//    broken on ASE even for official servers (FearEvolvedForever's README),
//    and nothing is written to Game.ini. Nothing is ever put in water or in
//    a cave.
//
// Everything the plugin spawns is recorded in spawned_<map>.json, so after
// a restart or reload it picks exactly those back up - or removes them if
// their event has ended - and never touches the map's own wild creatures.
//
// Creature/item/crate paths in config.json come from the ARK wiki's
// Creature IDs, Item IDs and Beacon IDs pages and can be pasted exactly as
// the wiki shows them. The local Dev Kit copy is older and is missing many
// of these assets. Any path that fails to load is logged once by name.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

#include <API/ARK/Ark.h>
#include <ICommands.h>
#include <IApiUtils.h>
#include <Logger/Logger.h>
#include <Requests.h>

// The plugin's folder name = its DLL's name, so the same build can be
// installed a second time under another name (e.g. ArkEventsRevivalpve) with
// its own configs and state files.
static std::string PluginName()
{
    static const std::string name = []
    {
        static const int anchor = 0;
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&anchor), &module) && GetModuleFileNameA(module, path, MAX_PATH))
            return std::filesystem::path(path).stem().string();
        return std::string{};
    }();
    return name.empty() ? std::string{"ArkEventsRevival"} : name;
}
#include <Tools.h>
#include <json.hpp>

namespace
{
    using json = nlohmann::json;

    FString ToFString(const std::string& text) { return FString(ArkApi::Tools::Utf8Decode(text).c_str()); }

    std::string ToStdString(const FString& text)
    {
        if (text.Len() <= 0) return {}; // an empty FString has a null buffer; std::wstring(nullptr) crashes
        return ArkApi::Tools::Utf8Encode(std::wstring(*const_cast<FString&>(text)));
    }

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string Trim(const std::string& text)
    {
        const auto begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return {};
        const auto end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    void ReplaceAll(std::string& text, const std::string& token, const std::string& with)
    {
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos)
        {
            text.replace(pos, token.size(), with);
            pos += with.size();
        }
    }

    // Map names are compared in this form everywhere - live name and config
    // keys alike - so casing and the "_P" suffix never matter: "Gen2_P",
    // "Gen2" and "gen2" are the same map.
    std::string NormaliseMapName(const std::string& raw)
    {
        std::string name = ToLower(Trim(raw));
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_p") == 0)
            name.resize(name.size() - 2);
        return name;
    }

    std::string CurrentMapNameLower(UWorld* world)
    {
        if (!world) return {};
        FString map_name;
        world->GetMapName(&map_name);
        return NormaliseMapName(ToStdString(map_name));
    }

    // Region as stored on pool points: lowercase, "_C" dropped - so a name
    // pasted as "DinoSpawnEntriesGrassland_C" matches the recorded
    // "dinospawnentriesgrassland" (and "..._gen2"), and a short word like
    // "grassland" still matches too.
    std::string NormaliseTag(const std::string& raw)
    {
        std::string tag = ToLower(Trim(raw));
        if (tag.size() > 2 && tag.compare(tag.size() - 2, 2, "_c") == 0) tag.resize(tag.size() - 2);
        return tag;
    }

    // ARK's cave spawners are named for it - DinoSpawnEntriesCave1,
    // ...CaveTek_Hard, ...RedwoodCave, ...SnowCave_Lost and so on.
    bool IsCaveRegion(const std::string& zone_tag)
    {
        return zone_tag.find("cave") != std::string::npos;
    }

    FVector ActorLocation(AActor* actor)
    {
        return actor && actor->RootComponentField() ? actor->RootComponentField()->RelativeLocationField() : FVector{};
    }

    // The in-game GPS reading for a world position, latitude first.
    std::string Coords(const FVector& at)
    {
        const auto coords = ArkApi::GetApiUtils().FVectorToCoords(at);
        char text[64];
        std::snprintf(text, sizeof(text), "lat %.1f lon %.1f", coords.y, coords.x);
        return text;
    }

    // Pins a class against garbage collection for as long as the server
    // runs. A class this plugin loaded itself may have nothing else in the
    // game referencing it (event creatures/items outside their event), so
    // GC could free it while LoadClass's cache still holds the pointer, and
    // the next spawn would use freed memory. RF_RootSet is what UE 4.5's
    // AddToRoot() sets - the SDK exposes the flags field but not AddToRoot.
    void KeepLoaded(UObject* object)
    {
        using Flags = std::underlying_type_t<EObjectFlags>;
        auto& flags = object->ObjectFlagsField();
        flags = static_cast<EObjectFlags>(static_cast<Flags>(flags) | static_cast<Flags>(EObjectFlags::RF_RootSet));
    }

    // The config path each loaded class was first loaded by - what
    // spawned_<map>.json records for a crate, so a restart looks for the
    // class that was really spawned even if the config has changed since.
    std::unordered_map<UClass*, std::string> g_class_paths;

    // Established pattern (RaidBaseBuilder's LoadClass): try several path
    // forms since blueprint paths get recorded/typed inconsistently.
    UClass* LoadClass(const std::string& path)
    {
        static std::unordered_map<std::string, UClass*> resolved;
        if (path.empty()) return nullptr;

        const auto found = resolved.find(path);
        if (found != resolved.end()) return found->second;

        // Accept paths pasted straight from the ARK wiki - quoted and
        // wrapped: "Blueprint'/Game/.../X.X'" - by unwrapping to the bare
        // /Game/... form first. Left wrapped, the ".X'" tail would be taken
        // for the asset suffix and the rebuilt canonical path would still
        // start with "Blueprint'", which never loads.
        std::string bare = Trim(path);
        if (bare.size() >= 2 && bare.front() == '"' && bare.back() == '"') bare = bare.substr(1, bare.size() - 2);
        for (const char* wrapper : { "BlueprintGeneratedClass'", "Blueprint'" })
        {
            const std::string prefix = wrapper;
            if (bare.compare(0, prefix.size(), prefix) == 0)
            {
                bare = bare.substr(prefix.size());
                if (!bare.empty() && bare.back() == '\'') bare.pop_back();
                break;
            }
        }

        // Only well-formed content paths get as far as the engine's loader.
        // A malformed package name (a "//" typo) can hit a fatal check
        // inside the engine's package code instead of just failing.
        if ((bare.compare(0, 6, "/Game/") != 0 && bare.compare(0, 8, "/Script/") != 0) || bare.find("//") != std::string::npos)
        {
            resolved[path] = nullptr;
            return nullptr;
        }

        // Strip any existing ".AssetName" / "_C" suffix so the canonical
        // form can be rebuilt correctly below, regardless of how the path
        // was originally typed.
        const auto dot_pos = bare.find_last_of('.');
        if (dot_pos != std::string::npos) bare = bare.substr(0, dot_pos);
        if (bare.size() > 2 && bare.compare(bare.size() - 2, 2, "_C") == 0)
            bare = bare.substr(0, bare.size() - 2);

        const auto slash_pos = bare.find_last_of('/');
        const std::string asset_name = slash_pos != std::string::npos ? bare.substr(slash_pos + 1) : bare;
        // The actual canonical UE4 Blueprint class path is
        // "PackagePath.AssetName_C" - the asset name is repeated after a
        // dot, NOT just "_C" tacked onto the end of the package path.
        const std::string canonical = bare + "." + asset_name + "_C";

        const std::vector<std::string> variants = {
            path,
            canonical,
            bare,
            bare + "_C",
            "Blueprint'" + bare + "'",
            "BlueprintGeneratedClass'" + canonical + "'",
            "Blueprint'" + bare + "_C'",
        };

        for (const std::string& variant : variants)
        {
            FString as_text = ToFString(variant);
            // BPLoadClass can hand back the Blueprint ASSET rather than its
            // generated class - not a class at all, and treating it as one
            // reads garbage (TransferBufferGuard crashed on exactly that).
            // Held as UObject* so the UObject* overload of HasClassCastFlags
            // runs ("is this object a class?"); the UClass* overload tests
            // the class's own flags and rejected every real class.
            UObject* found = UVictoryCore::BPLoadClass(&as_text);
            if (found && Globals::HasClassCastFlags(found, ClassCastFlags::CASTCLASS_UClass))
            {
                UClass* loaded = static_cast<UClass*>(found);
                KeepLoaded(loaded);
                resolved[path] = loaded;
                g_class_paths.emplace(loaded, path);
                return loaded;
            }
        }

        // Not in memory - load it from disk. BPLoadClass only FINDS classes
        // something else already loaded, which is why every event-only asset
        // (Halloween items, event creatures outside their event) "would not
        // load" while ordinary craftable items loaded fine. Same call
        // CrossChat uses to load its icon textures.
        UObject* from_disk = Globals::StaticLoadObject(UObject::StaticClass(), nullptr,
            ArkApi::Tools::Utf8Decode(canonical).c_str(), nullptr, 0, 0, true);
        if (from_disk && Globals::HasClassCastFlags(from_disk, ClassCastFlags::CASTCLASS_UClass))
        {
            UClass* loaded = static_cast<UClass*>(from_disk);
            KeepLoaded(loaded);
            resolved[path] = loaded;
            g_class_paths.emplace(loaded, path);
            Log::GetLog()->info("ArkEventsRevival: loaded '{}' from disk.", canonical);
            return loaded;
        }

        resolved[path] = nullptr;
        return nullptr;
    }

    enum class ClassKind { Creature, Item, Container };

    // LoadClass plus a check that the path is the right KIND of blueprint -
    // an item path pasted where a creature goes (or the other way round)
    // would otherwise be spawned as that type and crash the server. Each
    // problem is warned about once per path.
    UClass* LoadClassOf(const std::string& path, ClassKind kind, const char* what)
    {
        static std::unordered_set<std::string> warned;
        static std::unordered_map<std::string, std::string> corrected; // path as written -> the spelling that loaded
        UClass* loaded = nullptr;
        if (const auto known = corrected.find(path); known != corrected.end()) loaded = LoadClass(known->second);
        if (!loaded) loaded = LoadClass(path);

        // A path like Blueprint'/dir/Package.Asset' whose two names differ is often a typo
        // in the wiki's ID lists (e.g. ..._Tentacle.PrimalItemSkin_..._Tentacles). If it
        // will not load as written, try it with the asset name used for both, then with
        // the package name used for both, and remember the spelling that works.
        if (!loaded)
        {
            const size_t open = path.find('\''), close = path.rfind('\'');
            if (open != std::string::npos && close != std::string::npos && close > open + 1)
            {
                const std::string inner = path.substr(open + 1, close - open - 1);
                const size_t dot = inner.rfind('.');
                const size_t slash = dot == std::string::npos ? std::string::npos : inner.rfind('/', dot);
                if (dot != std::string::npos && slash != std::string::npos)
                {
                    const std::string dir = inner.substr(0, slash + 1);
                    const std::string package = inner.substr(slash + 1, dot - slash - 1);
                    const std::string asset = inner.substr(dot + 1);
                    if (package != asset)
                        for (const std::string& name : { asset, package })
                        {
                            const std::string alternative = path.substr(0, open + 1) + dir + name + "." + name + "'";
                            if (UClass* found = LoadClass(alternative))
                            {
                                loaded = found;
                                corrected[path] = alternative;
                                Log::GetLog()->info("ArkEventsRevival: {} '{}' would not load as written - using '{}' instead.",
                                    what, path, alternative);
                                break;
                            }
                        }
                }
            }
        }
        if (!loaded)
        {
            if (warned.insert(path).second)
                Log::GetLog()->warn("ArkEventsRevival: {} '{}' would not load - check the path against the ARK wiki ID pages.", what, path);
            return nullptr;
        }

        UClass* base = nullptr;
        const char* needs = "";
        switch (kind)
        {
        case ClassKind::Creature:  base = APrimalDinoCharacter::GetPrivateStaticClass(L"/Script/ShooterGame"); needs = "a creature (Creature IDs)"; break;
        case ClassKind::Item:      base = UPrimalItem::GetPrivateStaticClass(L"/Script/ShooterGame"); needs = "an item (Item IDs)"; break;
        case ClassKind::Container: base = APrimalStructureItemContainer::GetPrivateStaticClass(L"/Script/ShooterGame"); needs = "a crate (Beacon IDs)"; break;
        }
        if (base && loaded->IsChildOf(base)) return loaded;

        if (warned.insert(path).second)
            Log::GetLog()->warn("ArkEventsRevival: {} '{}' is the wrong kind of blueprint - it needs {} path.", what, path, needs);
        return nullptr;
    }

    // ---- config ----
    //
    // Everything spawnable lives under Spawns.Maps.<map>.<region>, where the
    // region is a DinoSpawnEntries name (or "Anywhere"). Each dino, crate or
    // boss entry is tracked on its own.

    // One item a crate is filled with. For Drops, Chance is rolled per crate;
    // for a boss's DefeatCrates it is the item's share of the picks.
    struct CrateItem
    {
        std::string class_path;
        int min_quantity{1};
        int max_quantity{1};
        float chance{1.f};
    };

    // One event creature. Once it is killed down to min_count alive, 1 to
    // (max_count - alive) more appear in its region.
    struct WildSpawnDino
    {
        std::string class_path;
        // The region key it was listed under, normalised (see NormaliseTag)
        // and substring-matched against pool points. Empty ("Anywhere") =
        // any spawn point on the map outside water and caves.
        std::vector<std::string> regions;
        int min_count{1};
        int max_count{10};
        // "RespawnTo": once down to min_count, it is topped back up to a random
        // total between this and max_count. 0 = anywhere from min_count + 1.
        int respawn_to{0};
        // "RespawnDelayMinutes": once killed down to min_count, wait this long
        // before topping back up (the first fill at event start never waits).
        float respawn_delay_seconds{0.f};
        // "SpawnLike": spawn ONLY where the game's own creatures of this kind
        // spawn - lowercase substrings of their class name ("rex_character_bp"
        // = wherever wild Rexes appear). A map with none of them gets none of
        // this dino; the region is not used for it.
        std::vector<std::string> spawn_like;
        // "RequireNative": spawn (anywhere in its region) only on maps where the
        // game's own creatures of this kind spawn - class name part, lowercase.
        std::vector<std::string> require_native;
        // "dino|<region>|<path>" - what spawned_<map>.json records it under.
        std::string key;
        // "GameSpawn": the game's own spawn zones spawn this creature (it is added to their spawn
        // tables while the event runs) instead of the plugin placing it, so it spreads over the whole
        // region like normal wildlife. MinCount/MaxCount/RespawnTo then do nothing for it.
        bool game_spawn{false};
        // "GameWeight": about this share (0-1) of the zone's normal spawns becomes this creature.
        float game_weight{0.25f};
    };

    // A supply crate (the Beacon IDs wiki list) filled with Items. An item
    // is an inventory object, not an actor, so it cannot be put into the
    // world on its own - only inside something. A crate stops counting once
    // it has been looted empty or has despawned; refilled like a dino.
    struct RegionDrop
    {
        std::string crate_path;
        // Used instead when crate_path won't load or spawn.
        std::string fallback_crate_path;
        std::vector<std::string> regions; // as WildSpawnDino::regions
        int min_count{1};
        int max_count{5};
        // Once down to min_count, topped back up to a random total between
        // this and max_count. 0 = anywhere from min_count + 1.
        int respawn_to{0};
        // Place crates beside spots where this event's dinos spawned, rather
        // than at random points of the region (which stays the fallback).
        bool near_event_dinos{false};
        // > 0: exactly this many items per crate, picked from Items with
        // Chance as each one's share (as a boss's DefeatCrates). 0: every item
        // rolled on its own Chance.
        int items_per_crate{0};
        // Also roll the crate's own normal loot on top of Items.
        bool include_crate_loot{false};
        std::vector<CrateItem> items;
        std::string key; // "drop|<region>|<path>"
    };

    // The ring of crates a boss leaves where it dies - count crates, each
    // holding items_per_crate picks from items.
    struct DefeatCrates
    {
        std::string crate_path;
        std::string fallback_crate_path; // as RegionDrop's
        int count{10};
        int items_per_crate{10};
        bool include_crate_loot{false};
        float ring_radius{700.f};
        // true: the crates are spread evenly round the arena edge, just inside
        // the stacked lines, instead of a small ring where the boss fell.
        bool around_arena{false};
        std::vector<CrateItem> items;
    };

    // A boss. Whenever fewer than `amount` are alive, a timer runs; after
    // respawn_seconds the missing ones appear in its region (so the first
    // one appears respawn_seconds after the event starts). Each boss sends
    // waves of minions - minions_per_wave at a time, every
    // wave_interval_seconds, up to wave_count waves - for as long as it lives.
    struct RegionBoss
    {
        std::string class_path;
        // What announcements call it. Empty = the creature's own name.
        std::string name;
        std::vector<std::string> regions; // as WildSpawnDino::regions
        int amount{1};
        float respawn_seconds{7200.f};
        // Once due, waits for in-game night (21:50 - 05:15) before appearing.
        bool night_only{false};
        int level{150};
        std::vector<std::string> minion_classes;
        int minion_level{60};
        int minions_per_wave{5};
        int wave_count{10};
        // "RandomMinions": the minions are random creatures native to the map (the event's wild
        // creature list) instead of MinionClasses.
        bool random_minions{true};
        float wave_interval_seconds{45.f};
        // Removes WILD creatures within this radius before the boss appears
        // so it isn't crowded - never tamed ones, never another boss. 0 = off.
        float clear_radius{1000.f};
        // Dragged or lured further than this from its spawn point (or into
        // water), the boss is put back at its spawn point. 0 = off.
        float leash_radius{12000.f};   // 40 foundations
        // A red ring on the ground this far out (ARK units; 9000 = 30
        // foundations) shows players the boss's fight area. 0 = no ring.
        float arena_radius{9000.f};
        // A shield dome over the arena (Extinction's orbital-drop shield, as the
        // SafeZone plugin uses): see-through, players and tames walk in and
        // out; only damage from outside is stopped. false = draw rings instead.
        // No damage taken for this long, the boss heals back to full. 0 = never.
        float heal_after_seconds{600.f};
        // In-game hours (0-23) the boss may appear at, e.g. {11, 12}. When set,
        // RespawnMinutes/NightOnly are not used: at each listed hour one is
        // spawned only if fewer than Amount are alive.
        std::vector<int> spawn_hours;
        // Crossing the dome edge starts this countdown; still outside at 0,
        // the boss is sent back to the center. Past LeashFoundations (or in
        // water) it goes back at once.
        float return_countdown_seconds{10.f};
        bool shield_dome{false};
        FLinearColor dome_color{ 1.0f, 0.4f, 0.0f, 1.0f };
        DefeatCrates defeat_crates;
        std::string key; // "boss|<region>|<path>"; its minions are "minion|" + key, its crates "reward|" + key
    };

    struct MapSpawns
    {
        std::vector<WildSpawnDino> dinos;
        std::vector<RegionDrop> drops;
        std::vector<RegionBoss> bosses;
    };

    // One crate dropped where an event dino is killed, holding MinItems to
    // MaxItems DIFFERENT event items. Crates lists this map's supply crates;
    // one is picked at random per kill (FallbackCrate if it won't spawn).
    struct KillDrops
    {
        std::vector<std::string> crate_paths;
        std::string fallback_crate_path;
        int min_items{2};
        int max_items{5};
        // Chance (0-1) that a kill drops a crate at all. 1 = every kill.
        float chance{1.f};
        // > 0: exactly every Nth event dino kill drops one (Chance ignored).
        int every_kills{0};
        std::vector<CrateItem> items;
    };

    // Lootable mounds kept around each player (e.g. ARKaeology's fossil
    // piles). Mounds vanish when no player is near, so they follow players.
    struct PlayerMounds
    {
        std::string class_path;
        int per_player{3};
        float min_distance{1000.f};
        float max_distance{3000.f};
        int min_items{1};
        int max_items{3};
        std::vector<CrateItem> items;
    };

    struct SpawnsConfig
    {
        bool enabled{false};
        float interval_seconds{60.f};
        int min_level{5};
        int max_level{150};
        KillDrops kill_drops;
        PlayerMounds mounds;
        // Buffs every living player carries while the event runs - ARK's own
        // event logic, e.g. Fear Evolved's Buff_FearEvolvedMoundSpawner, which
        // spawns the Pumpkin / Scarecrow / Gravestone mounds around each player
        // exactly as official servers did. Taken off when the event ends.
        std::vector<std::string> player_buffs;
        // "GameWeight": the share of the game's own spawns that also bring an event creature.
        float game_weight{0.25f};
        // Keyed by NormaliseMapName - only the map this server runs is
        // used, since region names differ per map. A map with no key gets
        // nothing.
        std::unordered_map<std::string, MapSpawns> by_map;
    };

    const MapSpawns* MapSpawnsFor(const SpawnsConfig& cfg, const std::string& map_name)
    {
        if (!cfg.enabled) return nullptr;
        const auto it = cfg.by_map.find(map_name);
        if (it == cfg.by_map.end()) return nullptr;
        const MapSpawns& here = it->second;
        return here.dinos.empty() && here.drops.empty() && here.bosses.empty() ? nullptr : &here;
    }

    // A real-world calendar window (month/day, year-independent) an event
    // is due during - e.g. Fear Evolved roughly late Oct to early Nov.
    // Wraps year-end when end < start (Winter Wonderland: Dec 14 - Jan 6).
    struct DateRange
    {
        int start_month{1}, start_day{1};
        int end_month{1}, end_day{1};
    };

    struct EventConfig
    {
        SpawnsConfig spawns;
        std::vector<DateRange> auto_date_ranges; // only consulted when Settings.auto_detect_by_date is true
    };

    struct Settings
    {
        bool enabled{true};
        bool debug_mode{false};
        // One of: FearEvolved, TurkeyTrial, WinterWonderland, vday, easter,
        // Summer, birthday, ARKaeology - confirmed literal tokens from the
        // Dev Kit's TestGameMode.uasset ActiveEvent switch. Empty disables
        // everything. Ignored (overwritten from the real calendar date
        // instead) whenever auto_detect_by_date is true.
        std::string active_event;
        // When true, active_event is driven entirely by the real-world
        // date against each event's AutoDateRanges (see DateRange) - the
        // plugin can just be left running and will switch events on its
        // own.
        bool auto_detect_by_date{false};
        // Announcements - chat plus an on-screen notification to everyone.
        // "{boss}" = the boss's name, "{lat}" = its GPS position. An empty
        // message is not sent.
        std::string chat_sender{"ARK Events"};
        std::string boss_spawned_message{"{boss} has appeared at {lat}! Defeat it for event loot."};
        // Sent to players near a boss when it crosses its arena ring.
        std::string boss_leaving_message{"{boss} is leaving the event area! Returning to the center in {seconds}s..."};
        std::string player_entered_message{"You entered the {boss} event area - you can fight it here!"};
        std::string player_left_message{"You left the {boss} event area - you can't damage it from outside the dome."};
        // Told to a player whose hit on a boss or its minions was blocked
        // because they are outside the boss's arena ring.
        std::string outside_arena_message{"{boss} can't be damaged from here - you must be inside the event range (the red ring)!"};
        std::string boss_defeated_message{"{boss} has been defeated!"};
        // Told to a player a little after they join the map, once for each boss that is
        // up right now ("{boss}" = its name, "{lat}" = where it is at that moment). Nothing
        // is sent when no boss is up, or the boss has been killed. Empty = never.
        std::string boss_on_map_message{"{boss} is out there right now at {lat} - defeat it for event loot!"};
        // Seconds after a player joins before they are told (15 = the same moment as the
        // WelcomeMessage plugin's default, after the loading screen).
        float boss_on_map_delay_seconds{15.f};
        // A Discord webhook message the day before each event starts. Turn it
        // on in ONE server's config only - every server runs this plugin.
        bool discord_enabled{false};
        std::string discord_webhook_url;
        std::string discord_message{"**{event}** starts in 24 hours! Admins: add **-event={token}** to the server startup line before the next restart."};
        std::string discord_ended_message{"**{event}** has ended. Thanks for playing - see you next time!"};
        std::string discord_ending_message{"**{event}** ends in 24 hours. Admins: if it was started with **-event={token}**, remove that from the server startup line within 24 hours to let it end - or leave it there to keep the event going."};
        std::string discord_started_message{"**{event}** has started! Event creatures, bosses and loot are live."};
        // Event creatures never spawn within this distance of a player-built
        // structure (ARK units; 5000 = 50 m). 0 = off.
        float structure_clearance{5000.f};
        // Nothing spawns within this many GPS units of the map edge (5 = only
        // between lat/lon 5 and 95). 0 = off.
        float map_edge_margin{5.f};
        // After a creature spawns, its real size is checked against the world and
        // against other creatures: one buried in rock or overlapping another is
        // moved to clear ground nearby, or removed if there is none. On by default.
        bool spawn_clearance{true};
        // Event creatures only spawn on ground with no water on or under it.
        bool land_only{true};
        // Wild event creatures normally spawn where their entry says (the regions it names,
        // or where a native creature of its kind spawns). This is the chance (0 to 1) that
        // a spawn instead goes to ANY other region of the map, so they turn up everywhere
        // but far less often than at home. Never in water or a cave. 0 = off.
        float other_region_chance{0.10f};
        // Non-boss event creatures (wild ones and boss minions) get a random level, in steps
        // of 5 like a normal wild spawn, between the event's MinLevel and MaxLevel (5-150).
        // Wyverns may go up to this (ModifiedSpawnLevelDistribution gives them 190).
        // Bosses always use their own Level.
        int wyvern_max_level{190};
        // Wild event creatures keep at least this far (ARK units; 10000 = 100 m) from
        // every other event creature. 0 = off. It relaxes if no spot is that open.
        float spawn_spacing{10000.f};
        std::unordered_map<std::string, EventConfig> events; // keyed by the tokens above
    } g_settings;

    const EventConfig* CurrentEventConfig()
    {
        if (g_settings.active_event.empty()) return nullptr;
        const auto it = g_settings.events.find(g_settings.active_event);
        return it != g_settings.events.end() ? &it->second : nullptr;
    }

    // The config's own spelling of an event name typed in any case - the
    // game ignores case for the event, so "fearevolved" must find the
    // "FearEvolved" settings too. Unknown names come back unchanged.
    std::string ResolveEventToken(const std::string& token, const std::unordered_map<std::string, EventConfig>& events)
    {
        const std::string wanted = ToLower(token);
        for (const auto& entry : events)
            if (ToLower(entry.first) == wanted) return entry.first;
        return token;
    }

    // ---- water and caves ----
    //
    // Nothing is ever spawned in water or in a cave. Water is asked of ARK
    // directly. Caves are recognised two ways: a spawn region named for one
    // (see IsCaveRegion), and ARK's own cave marker - the post-process
    // volumes that darken and fog a cave are typed Cave
    // (APostProcessVolume::VolumeType, EPostProcessVolumeType::Cave = 1),
    // and a point inside one is in a cave.

    // Is this point in water? Asked of ARK rather than guessed from a zone
    // name - a sea-level number only knows about the ocean, and misses
    // rivers, lakes and flooded cave sections. Same native call
    // RaidBaseBuilder already uses successfully in this codebase for the
    // same "don't place this on water" question.
    bool PointIsUnderwater(UWorld* world, const FVector& point)
    {
        if (!world) return false;
        FVector scratch = point;
        return NativeCall<bool, UWorld*, FVector*, bool, float, bool, bool, bool>(
            nullptr, "ABiomeZoneVolume.IsPointUnderwater",
            world, &scratch,
            false,  // take the accurate path
            0.0f,   // any depth counts
            true, true, true);
    }

    struct CaveVolume
    {
        TWeakObjectPtr<AActor> volume;
        FVector min;
        FVector max;
        int kind{0};        // 0 = brush volume, 1 = sphere, 2 = box
        FVector centre{};   // sphere: its middle
        float radius{0.f};  // sphere: its radius
    };
    std::vector<CaveVolume> g_cave_volumes;
    bool g_cave_volumes_built{false};

    // Finds this map's cave volumes once. Read out of the Dev Kit's own map files
    // (TheIsland, ScorchedEarth, Aberration, Extinction, Genesis), a cave is marked by:
    //
    // 1. A BIOME ZONE VOLUME whose BiomeZoneName says so: "Cave" and "Lava Cave" on The
    //    Island, "Teeming Caverns - Tunnels", "Hidden Grotto", "Old Railway Cave" on
    //    Aberration. Biome zones drive temperature on the server, so they are always there
    //    (the Post Process Volumes the old version looked for are not).
    //
    // 2. A STRUCTURE PREVENTION ZONE that is a cave's: the designers keep a cave's building
    //    and flyer blocks as prevention volumes (Cave2StructurePreventionVolume,
    //    SwampCave_NoBuildVolume, and the unnamed ones in folders like "Cave03_Volumes"
    //    which all share one trait: they stop flyers, but not every structure, and are on
    //    by default). Boss arenas, the volcano and starting zones stop ALL structures and
    //    are left out; optional, disabled and classic-PvP zones too. A zone whose own
    //    name says cave, cavern, grotto or dungeon counts whatever its flags.
    //
    // 2. In the Dev Kit a cave is also a Post Process
    // Volume whose Volume Type is set to Cave (EPostProcessVolumeType: Generic 0,
    // Cave 1, Water 2). The game also has SPHERE and BOX versions of that volume
    // (ASpherePostProcessVolume, ARuntimeSpherePostProcessVolume,
    // ARuntimeBoxPostProcessVolume): they are subclasses, and are taken too - each
    // tested against its own shape. The SDK has no APostProcessVolume (or
    // FBoxSphereBounds layout), so everything is read by the names and offsets in
    // the server's own PDB: VolumeType is a one-byte enum, ABrush.BrushComponent the
    // volume's shape, and USceneComponent.Bounds is { FVector Origin; FVector
    // BoxExtent; float SphereRadius; }.
    void BuildCaveVolumes(UWorld* world)
    {
        if (!world) return;
        g_cave_volumes_built = true;
        g_cave_volumes.clear();

        UClass* volume_class = NativeCall<UClass*>(nullptr, "APostProcessVolume.StaticClass");
        if (!volume_class) return;

        TArray<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(world, volume_class, &actors);
        int by_type[3] = {0, 0, 0};
        std::unordered_map<std::string, int> cave_classes;
        for (AActor* actor : actors)
        {
            if (!actor || !actor->IsValidLowLevelFast(false) || !actor->IsA(volume_class)) continue;
            const uint8 type = *GetNativePointerField<uint8*>(actor, "APostProcessVolume.VolumeType");
            if (type < 3) ++by_type[type];
            if (type != 1) continue; // 1 = Cave

            std::string class_name;
            if (actor->ClassField())
            {
                FString name;
                actor->ClassField()->NameField().ToString(&name);
                class_name = ToStdString(name);
                std::transform(class_name.begin(), class_name.end(), class_name.begin(), [](unsigned char c) { return std::tolower(c); });
            }
            const int kind = class_name.find("sphere") != std::string::npos ? 1
                : class_name.find("box") != std::string::npos ? 2 : 0;

            FVector origin{}, extent{};
            bool have_bounds = false;
            if (kind == 0)
            {
                if (auto* shape = *GetNativePointerField<UPrimitiveComponent**>(actor, "ABrush.BrushComponent"))
                {
                    const char* bounds = GetNativePointerField<char*>(shape, "USceneComponent.Bounds");
                    origin = *reinterpret_cast<const FVector*>(bounds);
                    extent = *reinterpret_cast<const FVector*>(bounds + 12);
                    have_bounds = true;
                }
            }
            if (!have_bounds) actor->GetActorBounds(false, &origin, &extent); // sphere and box volumes: the actor's own bounds
            if (!std::isfinite(origin.X) || !std::isfinite(extent.X) || extent.X <= 0.f) continue;

            CaveVolume cave{ GetWeakReference(actor), origin - extent, origin + extent };
            cave.kind = kind;
            cave.centre = origin;
            cave.radius = (std::max)({extent.X, extent.Y, extent.Z});
            g_cave_volumes.push_back(cave);
            ++cave_classes[class_name];
        }

        // Adds a brush volume as a cave, tested by its box.
        auto add_cave_box = [&](AActor* volume, const char* source)
        {
            FVector origin{}, extent{};
            bool have_bounds = false;
            if (auto* shape = *GetNativePointerField<UPrimitiveComponent**>(volume, "ABrush.BrushComponent"))
            {
                const char* bounds = GetNativePointerField<char*>(shape, "USceneComponent.Bounds");
                origin = *reinterpret_cast<const FVector*>(bounds);
                extent = *reinterpret_cast<const FVector*>(bounds + 12);
                have_bounds = true;
            }
            if (!have_bounds) volume->GetActorBounds(false, &origin, &extent);
            if (!std::isfinite(origin.X) || !std::isfinite(extent.X) || extent.X <= 0.f) return;

            CaveVolume cave{ GetWeakReference(volume), origin - extent, origin + extent };
            cave.kind = 2;                   // its box: no inside-the-shape call to rely on
            cave.centre = origin;
            cave.radius = (std::max)({extent.X, extent.Y, extent.Z});
            g_cave_volumes.push_back(cave);
            ++cave_classes[source];
        };

        auto lowered = [](std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        };
        auto cave_word = [&](const std::string& text)
        {
            const std::string low = lowered(text);
            for (const char* word : { "cave", "cavern", "grotto", "dungeon" })
                if (low.find(word) != std::string::npos) return true;
            return false;
        };

        // 1. Biome zone volumes named for a cave.
        int biomes_seen = 0, biomes_cave = 0;
        {
            UClass* biome_class = ABiomeZoneVolume::GetPrivateStaticClass(L"/Script/ShooterGame");
            TArray<AActor*> biomes;
            if (biome_class) UGameplayStatics::GetAllActorsOfClass(world, biome_class, &biomes);
            for (AActor* biome : biomes)
            {
                if (!biome || !biome->IsValidLowLevelFast(false) || !biome->IsA(biome_class)) continue;
                ++biomes_seen;
                const FString& zone_name = *GetNativePointerField<FString*>(biome, "ABiomeZoneVolume.BiomeZoneName");
                if (!cave_word(ToStdString(zone_name))) continue;
                ++biomes_cave;
                add_cave_box(biome, "biome zone");
            }
        }

        // 2. Structure prevention zones that are a cave's. Loaded by their native path rather
        // than by a StaticClass call, so a name the offset cache does not know cannot crash.
        int zones_seen = 0, zones_cave = 0;
        UObject* zone_class_object = Globals::StaticLoadObject(UObject::StaticClass(), nullptr,
            L"/Script/ShooterGame.StructurePreventionZoneVolume", nullptr, 0, 0, true);
        if (zone_class_object && Globals::HasClassCastFlags(zone_class_object, ClassCastFlags::CASTCLASS_UClass))
        {
            UClass* zone_class = static_cast<UClass*>(zone_class_object);
            TArray<AActor*> zones;
            UGameplayStatics::GetAllActorsOfClass(world, zone_class, &zones);
            for (AActor* zone : zones)
            {
                if (!zone || !zone->IsValidLowLevelFast(false) || !zone->IsA(zone_class)) continue;
                ++zones_seen;

                auto flag = [&](const char* name)
                {
                    return GetNativeBitField<bool, unsigned __int32>(zone, std::string("AStructurePreventionZoneVolume.") + name);
                };
                FString zone_name;
                zone->NameField().ToString(&zone_name);

                const bool named_cave = cave_word(ToStdString(zone_name));
                const bool cave_traits = flag("bPreventionVolumePreventsFlyers") && !flag("bPreventAllStructures")
                    && !flag("bDisabled") && !flag("bOptionallyEnabled") && !flag("bForceEnabledWhenAllowCaveBuildingPVPIsFalse");
                if (!named_cave && !cave_traits) continue;

                ++zones_cave;
                add_cave_box(zone, "prevention zone");
            }
        }

        if (g_cave_volumes.empty())
            Log::GetLog()->warn("ArkEventsRevival: no cave volumes found on this map ({} biome zone(s), {} named for a cave; {} structure prevention zone(s), {} that are a cave's; {} post process volume(s)) - caves are recognised by region name only.",
                biomes_seen, biomes_cave, zones_seen, zones_cave, actors.Num());
        else if (g_settings.debug_mode)
        {
            std::string kinds;
            for (const auto& [name, count] : cave_classes) kinds += (kinds.empty() ? "" : ", ") + name + " x" + std::to_string(count);
            Log::GetLog()->info("ArkEventsRevival: {} cave volume(s) found on this map ({}) - nothing will spawn inside them.",
                g_cave_volumes.size(), kinds);
        }
    }

    // Inside any cave volume? A box test first (cheap, and rules out almost
    // every volume), then ARK's own inside-the-shape test for the few whose
    // box holds the point.
    bool PointIsInCave(UWorld* world, const FVector& point)
    {
        if (!g_cave_volumes_built) BuildCaveVolumes(world);
        for (CaveVolume& cave : g_cave_volumes)
        {
            if (point.X < cave.min.X || point.Y < cave.min.Y || point.Z < cave.min.Z
                || point.X > cave.max.X || point.Y > cave.max.Y || point.Z > cave.max.Z)
                continue;
            AActor* volume = cave.volume.Get();
            if (!volume) continue;
            if (cave.kind == 2) return true; // a box volume: inside its box is inside it
            if (cave.kind == 1)              // a sphere volume: within its radius of the middle
            {
                const float dx = point.X - cave.centre.X, dy = point.Y - cave.centre.Y, dz = point.Z - cave.centre.Z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <= cave.radius) return true;
                continue;
            }
            // (FVector Point, float SphereRadius, float* OutDistanceToPoint,
            // float* ...) - ARK's signature per the PDB; both outs optional.
            if (NativeCall<bool, FVector, float, float*, float*>(volume, "APostProcessVolume.EncompassesPoint", point, 0.f, nullptr, nullptr))
                return true;
        }
        return false;
    }

    // ---- spawn-point pool ----
    //
    // Real wild-spawn locations, built passively from every native wild dino
    // spawn (see HookSpawnFromEntries) - so spawns can use wherever the
    // game's own wildlife already appears instead of hand-typed coordinates.
    // Kept per region (the spawning NPCSpawnEntriesContainer's class name,
    // e.g. DinoSpawnEntriesGrassland_C), newest points only, so a region
    // whose creatures rarely respawn is never crowded out by busy ones.
    struct WildSpawnPoint
    {
        FVector location;
        // -1 unknown, 0 no, 1 yes - worked out the first time the point is
        // actually tried (see PointUsable), then remembered.
        int underwater{-1};
        int cave{-1};
    };
    std::unordered_map<std::string, std::vector<WildSpawnPoint>> g_pool_by_region; // matching form -> points
    // The same kind of points again, but by WHICH creature spawned there:
    // lowercase class name without "_C" ("rex_character_bp") -> points. Cave
    // regions' points are never stored here.
    std::unordered_map<std::string, std::vector<WildSpawnPoint>> g_pool_by_class;
    // Every spawn region seen on this map since load: matching form ->
    // the game's own name ("DinoSpawnEntriesGrassland_C"), for ListRegions.
    std::unordered_map<std::string, std::string> g_seen_regions;
    constexpr size_t kPointsPerRegion = 60;

    void RecordWildSpawnPoint(const FVector& location, const std::string& region_name, const std::string& creature_class)
    {
        if (!creature_class.empty() && !IsCaveRegion(NormaliseTag(region_name)))
        {
            auto& by_class = g_pool_by_class[NormaliseTag(creature_class)];
            if (by_class.size() >= kPointsPerRegion) by_class.erase(by_class.begin());
            by_class.push_back({location});
        }

        const std::string zone_tag = NormaliseTag(region_name);
        if (!zone_tag.empty() && g_seen_regions.emplace(zone_tag, region_name).second && g_settings.debug_mode)
            Log::GetLog()->info("ArkEventsRevival: new spawn region seen: '{}'.", region_name);

        auto& points = g_pool_by_region[zone_tag];
        if (points.size() >= kPointsPerRegion)
            points.erase(points.begin()); // drop oldest, keep the pool fresh
        points.push_back({location});
    }

    // Any player-built structure (tribe/player team - wild and plugin-placed
    // crates are not) within structure_clearance of the point? Asked of ARK's
    // own server octree, the fast spatial index of structures.
    bool NearPlayerStructure(UWorld* world, const FVector& point)
    {
        if (!world || g_settings.structure_clearance <= 0.f) return false;
        TArray<AActor*> found;
        UVictoryCore::ServerOctreeOverlapActors(&found, world, point, g_settings.structure_clearance, EServerOctreeGroup::STRUCTURES, true);
        for (AActor* actor : found)
            if (actor && actor->TargetingTeamField() >= 50000) return true;
        return false;
    }

    // In water, OR in the air above water: a flyer's spawn point can hang over a
    // lake or the sea without being underwater itself, so the ground straight
    // below is checked too. No ground found below counts as water (open sea).
    bool PointIsOverWater(UWorld* world, const FVector& point)
    {
        if (PointIsUnderwater(world, point)) return true;
        FVector ground{};
        FVector probe = point;
        FVector up{0.f, 0.f, 0.f};
        FVector down{0.f, 0.f, -100000.f};
        if (!UVictoryCore::GetGroundLocation(world, &ground, &probe, &up, &down) || !std::isfinite(ground.Z)) return true;
        return PointIsUnderwater(world, FVector{ground.X, ground.Y, ground.Z + 50.f});
    }

    // Too close to the world border? Measured in in-game GPS (0-100).
    bool NearMapEdge(const FVector& point)
    {
        const float m = g_settings.map_edge_margin;
        if (m <= 0.f) return false;
        const auto c = ArkApi::GetApiUtils().FVectorToCoords(point);
        return c.x < m || c.y < m || c.x > 100.f - m || c.y > 100.f - m;
    }

    bool PointUsable(UWorld* world, WildSpawnPoint& point)
    {
        if (point.underwater < 0) point.underwater = PointIsOverWater(world, point.location) ? 1 : 0;
        if (point.underwater == 1) return false;
        if (point.cave < 0) point.cave = PointIsInCave(world, point.location) ? 1 : 0;
        if (point.cave == 1) return false;
        if (NearMapEdge(point.location)) return false;
        return !NearPlayerStructure(world, point.location);
    }

    // Random usable point from a candidate list - water and cave checked only
    // as each is tried; checking every point up front cost native traces per
    // point per pick.
    bool PickUsable(UWorld* world, std::vector<WildSpawnPoint*>& candidates, FVector& out_location)
    {
        for (int attempt = 0; attempt < 25 && !candidates.empty(); ++attempt)
        {
            const size_t slot = static_cast<size_t>(rand()) % candidates.size();
            WildSpawnPoint& point = *candidates[slot];
            if (!PointUsable(world, point))
            {
                candidates[slot] = candidates.back();
                candidates.pop_back();
                continue;
            }
            out_location = point.location;
            return true;
        }
        return false;
    }

    // A point where the game's own creatures matching ANY of like (class
    // name substrings) have spawned. False if none seen on this map yet.
    bool PickLikeLocation(const std::vector<std::string>& like, FVector& out_location)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world || like.empty()) return false;
        std::vector<WildSpawnPoint*> candidates;
        for (auto& [class_tag, points] : g_pool_by_class)
            for (const auto& wanted : like)
                if (class_tag.find(wanted) != std::string::npos)
                {
                    for (auto& point : points) candidates.push_back(&point);
                    break;
                }
        return PickUsable(world, candidates, out_location);
    }

    // Picks a random pool point whose region contains ANY of tag_filters
    // (case-insensitive substrings; an empty list matches anything), never
    // in water or a cave. Returns false if the pool has no such point yet -
    // e.g. right after server start, before enough wildlife has spawned to
    // seed it.
    bool PickPoolLocation(const std::vector<std::string>& tag_filters, FVector& out_location)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world) return false;

        std::vector<WildSpawnPoint*> candidates;
        for (auto& [zone_tag, points] : g_pool_by_region)
        {
            if (IsCaveRegion(zone_tag)) continue;
            bool matches = tag_filters.empty();
            for (const auto& tag : tag_filters)
                if (zone_tag.find(tag) != std::string::npos) { matches = true; break; }
            if (!matches) continue;
            for (auto& point : points) candidates.push_back(&point);
        }
        return PickUsable(world, candidates, out_location);
    }

    // A point in a RANDOM region of this map: every non-cave region that has points is
    // equally likely, whatever its size, so no region is left out. Never in water or a
    // cave. False if the pool has nothing usable yet.
    bool PickAnyRegionLocation(FVector& out_location)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world) return false;

        std::vector<const std::string*> tags;
        for (auto& [zone_tag, points] : g_pool_by_region)
            if (!points.empty() && !IsCaveRegion(zone_tag)) tags.push_back(&zone_tag);

        for (int attempt = 0; attempt < 6 && !tags.empty(); ++attempt)
        {
            const size_t slot = static_cast<size_t>(rand()) % tags.size();
            std::vector<WildSpawnPoint*> candidates;
            for (auto& point : g_pool_by_region[*tags[slot]]) candidates.push_back(&point);
            if (PickUsable(world, candidates, out_location)) return true;
            tags[slot] = tags.back(); // nowhere usable in that one (water, caves): try another
            tags.pop_back();
        }
        return false;
    }

    // ---- official gate / ActiveEvent ----
    //
    // Pins the event on the game mode and game state every tick (see
    // PinActiveEventFields) since we don't know how many places re-derive
    // it - the established pattern in this codebase (ExtinctionEventManager
    // re-pins bDoExtinctionEvent every tick for the same reason).

    AShooterGameState* ShooterGameState()
    {
        auto* world = ArkApi::GetApiUtils().GetWorld();
        if (!world || !world->GameStateField() ||
            !world->GameStateField()->IsA(AShooterGameState::GetPrivateStaticClass(L"/Script/ShooterGame")))
            return nullptr;
        return static_cast<AShooterGameState*>(world->GameStateField());
    }

    // True until the official flag has been put back to false after this
    // plugin (this load, or an older build before a reload) last forced it.
    // Starts true so the first tick also undoes what an earlier build left.
    bool g_official_flag_needs_reset{true};
    // True while the game mode/state carry an event this plugin put there.
    bool g_event_pinned{false};

    // The plugin never makes the server look official (that would stop admins getting in with
    // enablecheats). This only puts the flag back to false once on load, to undo an older build.
    void ResetOfficialFlag()
    {
        if (auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode())
            game_mode->bIsOfficialServerField() = false;
        if (auto* game_state = ShooterGameState())
            game_state->bIsOfficialServerField() = false;
    }

    // Takes the event back off the game mode/state (NAME_None = no event),
    // so its native cosmetics end with it rather than at the next restart.
    void ClearPinnedEvent()
    {
        if (auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode())
            game_mode->ActiveEventField() = FName();
        if (auto* game_state = ShooterGameState())
            game_state->ActiveEventField() = FName();
        g_event_pinned = false;
    }

    void PinActiveEventFields()
    {
        if (g_official_flag_needs_reset)
        {
            ResetOfficialFlag();
            g_official_flag_needs_reset = false;
        }

        if (!g_settings.enabled || g_settings.active_event.empty())
        {
            if (g_event_pinned) ClearPinnedEvent();
            return;
        }

        const FName token(g_settings.active_event.c_str(), EFindName::FNAME_Add);

        auto* game_mode = ArkApi::GetApiUtils().GetShooterGameMode();
        if (game_mode)
        {
            game_mode->ActiveEventField() = token;
        }

        // The same field -activeevent= sets on an unofficial server - this is
        // what clients read for the event's cosmetics and menus.
        if (auto* game_state = ShooterGameState())
            game_state->ActiveEventField() = token;
        g_event_pinned = true;
    }

    // ---- announcements ----

    // Chat because it stays there to be read by somebody who was tabbed out,
    // and a notification because chat scrolls past in seconds on a busy
    // server - RaidBaseBuilder's pattern.
    void Announce(const std::string& pattern, const std::string& boss_name, const FVector& at)
    {
        if (pattern.empty()) return;
        std::string text = pattern;
        ReplaceAll(text, "{boss}", boss_name);
        ReplaceAll(text, "{lat}", Coords(at));

        ArkApi::GetApiUtils().SendChatMessageToAll(ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
        ArkApi::GetApiUtils().SendNotificationToAll(
            FLinearColor{ 1.0f, 0.25f, 0.25f, 1.0f }, 1.4f, 12.0f, nullptr, L"{}", *ToFString(text));
        if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: announced: {}", text);
    }

    // ---- event spawns: tracking ----
    //
    // Tracking is per entry of THIS map's lists, in the same order, for the
    // active event only. Raw pointers are never kept across ticks - only
    // TWeakObjectPtr, which resolves to nullptr once its actor is gone.
    // Never create one with vector(n)/resize(n) or call Get() on a
    // default-constructed one: this SDK leaves its fields uninitialised.

    struct ActiveBoss
    {
        TWeakObjectPtr<APrimalDinoCharacter> boss;
        FVector location{};
        int waves_sent{0};
        float wave_timer{0.f};   // seconds toward the next minion while refilling
        // Minions: filled one at a time to MaxMinions when the boss appears;
        // once any are killed, a MinionRespawnSeconds wait, then one at a
        // time back to MaxMinions - for as long as the boss lives.
        float leash_timer{0.f};
        float ring_timer{0.f};
        float since_damage{0.f}; // seconds since it last took damage
        TWeakObjectPtr<AActor> dome; // the shield dome over this boss's arena, if any
        bool warned_leaving{false}; // unused (kept for layout)
        float return_in{-1.f};      // countdown seconds left while outside the dome; -1 = inside
        std::unordered_set<uint64_t> players_inside; // for the entered / left messages
        bool minion_refill{true}; // unused (kept for layout)
        float minion_wait{-1.f};
        int wave_spawned{0};      // minions put out so far in the current wave
        // Announced only once it has stood for kAnnounceDelaySeconds - a
        // spawn that failed or was removed straight away is never announced.
        float age{0.f};
        bool announced{false};
    };
    constexpr float kAnnounceDelaySeconds = 3.f;

    std::vector<std::vector<TWeakObjectPtr<APrimalDinoCharacter>>> g_dino_tracked;
    // SpawnThroughGame: where an event creature is to be put, beside a creature the game just spawned.
    struct GameSpawnRequest
    {
        size_t index;
        FVector location;
    };
    std::vector<GameSpawnRequest> g_game_queue;
    std::vector<std::vector<TWeakObjectPtr<AActor>>> g_crate_tracked;
    std::vector<std::vector<ActiveBoss>> g_boss_tracked;
    std::vector<std::vector<TWeakObjectPtr<APrimalDinoCharacter>>> g_minion_tracked; // per boss entry
    std::vector<std::vector<TWeakObjectPtr<AActor>>> g_reward_tracked;              // per boss entry: its defeat crates
    std::vector<float> g_boss_timers; // per boss entry: seconds counted toward its respawn
    // Only one boss is up at a time on a map; whose turn it is to spawn next
    // goes round the map's boss list, one step per spawn. Starts at a random
    // boss so a restart doesn't always begin with the first one.
    size_t g_boss_turn{0};
    bool AnyBossAlive()
    {
        for (auto& list : g_boss_tracked) if (!list.empty()) return true;
        return false;
    }
    std::vector<int> g_boss_hour_done; // per boss entry: the SpawnAtHours hour already handled (-1 = none)
    // Per dino entry: how many still to spawn (one at a time), seconds left
    // of the respawn wait (-1 = not waiting), and whether it has been filled
    // once (the first fill never waits).
    std::vector<int> g_dino_pending;
    std::vector<float> g_dino_wait;
    std::vector<char> g_dino_filled;
    // Buff classes handed to players by the running event (see PlayerBuffs).
    std::vector<UClass*> g_buffs_given;
    float g_buff_timer{0.f};

    UClass* BuffBaseClass()
    {
        return NativeCall<UClass*, const wchar_t*>(nullptr, "APrimalBuff.GetPrivateStaticClass", L"/Script/ShooterGame");
    }

    // Every living player's character, once each.
    template <typename F>
    void ForEachPlayerCharacter(F&& action)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world) return;
        UClass* pc_class = AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame");
        for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField())
        {
            APlayerController* pc = weak_pc.Get();
            if (!pc || !pc_class || !pc->IsA(pc_class)) continue;
            AShooterCharacter* character = static_cast<AShooterPlayerController*>(pc)->GetPlayerCharacter();
            if (!character || character->IsDead()) continue;
            action(character);
        }
    }

    // Takes every buff this plugin handed out back off every player.
    void RemovePlayerBuffs()
    {
        if (g_buffs_given.empty()) return;
        ForEachPlayerCharacter([](AShooterCharacter* character)
        {
            for (UClass* buff_class : g_buffs_given)
                if (APrimalBuff* buff = character->GetBuff(TSubclassOf<APrimalBuff>(buff_class)))
                    buff->Deactivate();
        });
        g_buffs_given.clear();
    }

    // Every few seconds: anyone alive without the event's buffs gets them
    // (new logins, respawns).
    void TickPlayerBuffs(float delta_seconds, const SpawnsConfig& cfg)
    {
        if (cfg.player_buffs.empty()) return;
        g_buff_timer += delta_seconds;
        if (g_buff_timer < 15.f) return;
        g_buff_timer = 0.f;

        UClass* buff_base = BuffBaseClass();
        std::vector<UClass*> classes;
        for (const std::string& path : cfg.player_buffs)
        {
            UClass* c = LoadClass(path);
            if (!c || !buff_base || !c->IsChildOf(buff_base))
            {
                static std::unordered_set<std::string> warned;
                if (warned.insert(path).second)
                    Log::GetLog()->warn("ArkEventsRevival: PlayerBuffs entry '{}' would not load or is not a buff.", path);
                continue;
            }
            classes.push_back(c);
            if (std::find(g_buffs_given.begin(), g_buffs_given.end(), c) == g_buffs_given.end()) g_buffs_given.push_back(c);
        }

        int added = 0;
        ForEachPlayerCharacter([&classes, &added](AShooterCharacter* character)
        {
            for (UClass* buff_class : classes)
                if (!character->HasBuff(TSubclassOf<APrimalBuff>(buff_class), true)
                    && APrimalBuff::StaticAddBuff(TSubclassOf<APrimalBuff>(buff_class), character, nullptr, nullptr, false))
                    ++added;
        });
        if (added > 0 && g_settings.debug_mode)
            Log::GetLog()->info("ArkEventsRevival: gave {} event buff(s) to players (e.g. the Fear Evolved mound spawner).", added);
    }

    // Kill-drop crates on the ground - removed when looted empty or the event ends.
    std::vector<TWeakObjectPtr<AActor>> g_kill_crates;
    std::vector<TWeakObjectPtr<AActor>> g_mounds;
    float g_mound_timer{0.f};
    // Where this event's wild dinos have spawned (and, after a restart, where
    // the ones picked back up stand) - for NearEventDinos drops. Newest kept.
    std::vector<FVector> g_event_dino_spots;
    constexpr size_t kEventDinoSpotsCap = 300;
    void NoteEventDinoSpot(const FVector& at)
    {
        if (g_event_dino_spots.size() >= kEventDinoSpotsCap) g_event_dino_spots.erase(g_event_dino_spots.begin());
        g_event_dino_spots.push_back(at);
    }
    std::string g_spawns_event;       // which event the tracking belongs to
    float g_spawns_timer{0.f};
    // Set whenever something is spawned; spawned_<map>.json is rewritten.
    bool g_spawned_dirty{false};
    // Until spawned_<map>.json has been read back after a load/reload (see
    // Reconcile), nothing is spawned and the file is not overwritten.
    bool g_reconcile_pending{true};
    float g_reconcile_delay{0.f};

    // Still roaming wild - not dead, not destroyed, not tamed. A tamed one
    // belongs to a player now: it stops counting and is never removed.
    bool IsEventWild(APrimalDinoCharacter* dino)
    {
        return dino && !dino->IsDead() && !dino->BPIsTamed();
    }

    bool IsTrackedBoss(APrimalDinoCharacter* dino)
    {
        for (auto& list : g_boss_tracked)
            for (auto& active : list)
                if (active.boss.Get() == dino) return true;
        return false;
    }

    std::string BossName(const RegionBoss& entry, APrimalDinoCharacter* boss)
    {
        if (!entry.name.empty()) return entry.name;
        if (boss)
        {
            const std::string own = Trim(ToStdString(boss->DescriptiveNameField()));
            if (!own.empty()) return own;
        }
        return "A boss";
    }

    uint64_t CreatureId(APrimalDinoCharacter* dino)
    {
        return (static_cast<uint64_t>(dino->DinoID1Field()) << 32) | dino->DinoID2Field();
    }

    std::string SpawnedFilePath(const std::string& map_name)
    {
        return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" + PluginName() + "/spawned_" + map_name + ".json";
    }

    const MapSpawns* ActiveMapSpawns(UWorld* world)
    {
        const EventConfig* event_cfg = g_settings.enabled ? CurrentEventConfig() : nullptr;
        return event_cfg ? MapSpawnsFor(event_cfg->spawns, CurrentMapNameLower(world)) : nullptr;
    }

    // Writes out every creature and crate currently tracked, so the next
    // load can pick exactly those back up (or remove them).
    void SaveSpawned()
    {
        if (g_reconcile_pending) return; // the file still holds what the last run left - not read back yet
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const std::string map_name = CurrentMapNameLower(world);
        if (map_name.empty()) return;

        const MapSpawns* here = ActiveMapSpawns(world);
        json creatures = json::array();
        json crates = json::array();

        auto add_creature = [&creatures](APrimalDinoCharacter* dino, const std::string& key, bool announced = true)
        {
            if (!IsEventWild(dino)) return;
            const uint64_t id = CreatureId(dino);
            if (id != 0) creatures.push_back({ {"Key", key}, {"Id", std::to_string(id)}, {"Announced", announced} });
        };
        // The crate's own class, as it was loaded - not whatever the config
        // says now, which may have changed since it was placed.
        auto add_crate = [&crates](AActor* crate, const std::string& key, const std::string& config_path)
        {
            if (!crate) return;
            const auto known = g_class_paths.find(crate->ClassField());
            const std::string& class_path = known != g_class_paths.end() ? known->second : config_path;
            const FVector at = ActorLocation(crate);
            crates.push_back({ {"Key", key}, {"Class", class_path}, {"X", at.X}, {"Y", at.Y}, {"Z", at.Z} });
        };

        if (here && g_spawns_event == g_settings.active_event)
        {
            for (size_t i = 0; i < g_dino_tracked.size() && i < here->dinos.size(); ++i)
                for (auto& ref : g_dino_tracked[i]) add_creature(ref.Get(), here->dinos[i].key);
            for (size_t i = 0; i < g_crate_tracked.size() && i < here->drops.size(); ++i)
                for (auto& ref : g_crate_tracked[i]) add_crate(ref.Get(), here->drops[i].key, here->drops[i].crate_path);
            for (size_t i = 0; i < g_boss_tracked.size() && i < here->bosses.size(); ++i)
            {
                const RegionBoss& entry = here->bosses[i];
                for (auto& active : g_boss_tracked[i]) add_creature(active.boss.Get(), entry.key, active.announced);
                if (i < g_minion_tracked.size())
                    for (auto& ref : g_minion_tracked[i]) add_creature(ref.Get(), "minion|" + entry.key);
                if (i < g_reward_tracked.size())
                    for (auto& ref : g_reward_tracked[i]) add_crate(ref.Get(), "reward|" + entry.key, entry.defeat_crates.crate_path);
            }
        }

        g_spawned_dirty = false;
        try
        {
            const json root = { {"Event", g_spawns_event}, {"Creatures", creatures}, {"Crates", crates} };
            std::ofstream file(SpawnedFilePath(map_name), std::ios::trunc);
            if (file.is_open()) file << root.dump(1);
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->warn("ArkEventsRevival: could not write spawned_{}.json: {}", map_name, ex.what());
        }
    }

    void ResetTracking()
    {
        g_dino_pending.clear();
        g_dino_wait.clear();
        g_dino_filled.clear();
        g_kill_crates.clear();
        g_mounds.clear();
        g_event_dino_spots.clear();
        g_dino_tracked.clear();
        g_game_queue.clear();
        g_crate_tracked.clear();
        g_boss_tracked.clear();
        g_minion_tracked.clear();
        g_reward_tracked.clear();
        g_boss_timers.clear();
        g_spawns_event.clear();
    }

    // Fresh, empty tracking sized to this map's lists for the active event.
    void StartTracking(const EventConfig* event_cfg, const MapSpawns& here)
    {
        ResetTracking();
        g_spawns_event = g_settings.active_event;
        g_dino_tracked.assign(here.dinos.size(), {});
        g_dino_pending.assign(here.dinos.size(), 0);
        g_dino_wait.assign(here.dinos.size(), -1.f);
        g_dino_filled.assign(here.dinos.size(), 0);
        g_crate_tracked.assign(here.drops.size(), {});
        g_boss_tracked.assign(here.bosses.size(), {});
        g_minion_tracked.assign(here.bosses.size(), {});
        g_reward_tracked.assign(here.bosses.size(), {});
        g_boss_timers.assign(here.bosses.size(), 0.f);
        g_boss_hour_done.assign(here.bosses.size(), -1);
        g_boss_turn = here.bosses.empty() ? 0 : static_cast<size_t>(rand()) % here.bosses.size();
        g_spawns_timer = event_cfg->spawns.interval_seconds; // first dino/crate check right away

        for (const auto& entry : here.bosses)
        {
            if (!entry.spawn_hours.empty())
            {
                std::string hours;
                for (int h : entry.spawn_hours) hours += (hours.empty() ? "" : ", ") + std::to_string(h) + ":00";
                if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: boss '{}' appears at in-game {} if not already up - ArkEventsRevival.SpawnBossNow spawns it now.",
                    entry.class_path, hours);
            }
            else
                if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: boss '{}' first due in {} minute(s){} - ArkEventsRevival.SpawnBossNow spawns it now.",
                    entry.class_path, static_cast<int>(entry.respawn_seconds / 60.f), entry.night_only ? " (at night)" : "");
        }
    }

    void RemoveDome(ActiveBoss& active); // with the bosses, below
    void ClearAllDomes();

    // Removes everything the ending event spawned that is still wild, and
    // its crates. Tamed creatures stay with their owners.
    void ClearSpawns()
    {
        int removed = 0;
        auto remove = [&removed](APrimalDinoCharacter* dino) { if (IsEventWild(dino)) { dino->Destroy(false, true); ++removed; } };
        auto remove_crates = [](std::vector<std::vector<TWeakObjectPtr<AActor>>>& lists)
        {
            for (auto& list : lists)
                for (auto& ref : list)
                    if (AActor* crate = ref.Get()) crate->Destroy(false, true);
        };
        for (auto& list : g_dino_tracked) for (auto& ref : list) remove(ref.Get());
        for (auto& list : g_minion_tracked) for (auto& ref : list) remove(ref.Get());
        for (auto& list : g_boss_tracked) for (auto& active : list) { remove(active.boss.Get()); RemoveDome(active); }
        remove_crates(g_crate_tracked);
        remove_crates(g_reward_tracked);
        for (auto& ref : g_kill_crates) if (AActor* crate = ref.Get()) crate->Destroy(false, true);
        for (auto& ref : g_mounds) if (AActor* mound = ref.Get()) mound->Destroy(false, true);
        RemovePlayerBuffs();
    ClearAllDomes();

        ResetTracking();
        SaveSpawned();
        if (removed > 0)
            Log::GetLog()->info("ArkEventsRevival: event ended - removed {} untamed event creature(s) and its crates.", removed);
    }

    // Puts a creature read back from spawned_<map>.json into the entry it
    // was recorded under. False = that entry no longer exists (config
    // changed) or is already full, and the creature should go.
    bool AdoptCreature(const MapSpawns& here, const std::string& key, APrimalDinoCharacter* dino, bool announced)
    {
        for (size_t i = 0; i < here.dinos.size(); ++i)
            if (here.dinos[i].key == key && static_cast<int>(g_dino_tracked[i].size()) < here.dinos[i].max_count)
            {
                g_dino_tracked[i].push_back(GetWeakReference(dino));
                NoteEventDinoSpot(ActorLocation(dino));
                return true;
            }
        for (size_t i = 0; i < here.bosses.size(); ++i)
        {
            const RegionBoss& entry = here.bosses[i];
            if (entry.key == key && static_cast<int>(g_boss_tracked[i].size()) < entry.amount)
            {
                ActiveBoss active;
                active.boss = GetWeakReference(dino);
                active.location = ActorLocation(dino);
                active.minion_refill = false;          // its minions came before the restart
                active.waves_sent = 1;                 // ...as the wave that was out
                active.wave_spawned = entry.minions_per_wave;
                active.announced = announced;         // announced now if the reload came first
                g_boss_tracked[i].push_back(active);
                return true;
            }
            if ("minion|" + entry.key == key
                && static_cast<int>(g_minion_tracked[i].size()) < entry.minions_per_wave)
            {
                g_minion_tracked[i].push_back(GetWeakReference(dino));
                return true;
            }
        }
        return false;
    }

    bool CrateHasItems(APrimalStructureItemContainer* crate)
    {
        UPrimalInventoryComponent* inventory = crate->MyInventoryComponentField();
        if (!inventory) return false;
        TArray<UPrimalItem*> items = inventory->InventoryItemsField();
        return items.Num() > 0;
    }

    // As AdoptCreature, for a crate: a Drops crate back into its entry, a
    // boss's defeat crate back with that boss entry.
    bool AdoptCrate(const MapSpawns& here, const std::string& key, AActor* crate)
    {
        for (size_t i = 0; i < here.drops.size(); ++i)
            if (here.drops[i].key == key && static_cast<int>(g_crate_tracked[i].size()) < here.drops[i].max_count)
            {
                g_crate_tracked[i].push_back(GetWeakReference(crate));
                return true;
            }
        for (size_t i = 0; i < here.bosses.size(); ++i)
            if ("reward|" + here.bosses[i].key == key)
            {
                g_reward_tracked[i].push_back(GetWeakReference(crate));
                return true;
            }
        return false;
    }

    // Reads spawned_<map>.json back after a load or reload: everything in it
    // that is still wild is either taken back into tracking (same event, and
    // its config entry still exists) or removed (the event ended, changed,
    // or the entry was taken out of config). Only creatures and crates this
    // plugin recorded are ever touched - the map's own wildlife never is.
    void OnBossDefeated(UWorld* world, size_t index, const RegionBoss& entry, APrimalDinoCharacter* boss, const FVector& fallback); // with the bosses, below

    void Reconcile(UWorld* world)
    {
        g_reconcile_pending = false;
        ClearAllDomes(); // left from before the restart/reload; live bosses get new ones
        const std::string map_name = CurrentMapNameLower(world);

        // Read into plain values first: a damaged file must never throw
        // inside the game tick, only mean "nothing to pick back up".
        struct SavedCrate { std::string key, class_path; FVector at; };
        struct SavedCreature { std::string key; bool announced{true}; };
        std::string saved_event;
        std::unordered_map<uint64_t, SavedCreature> saved_creatures;
        std::vector<SavedCrate> saved_crates;
        try
        {
            json root;
            std::ifstream file(SpawnedFilePath(map_name));
            if (file.is_open()) file >> root;
            if (root.is_object())
            {
                saved_event = root.value("Event", std::string{});
                if (root.contains("Creatures") && root["Creatures"].is_array())
                    for (const auto& node : root["Creatures"])
                        if (node.is_object())
                            saved_creatures[std::stoull(node.value("Id", std::string{"0"}))] =
                                { node.value("Key", std::string{}), node.value("Announced", true) };
                if (root.contains("Crates") && root["Crates"].is_array())
                    for (const auto& node : root["Crates"])
                        if (node.is_object())
                            saved_crates.push_back({ node.value("Key", std::string{}), node.value("Class", std::string{}),
                                FVector{node.value("X", 0.f), node.value("Y", 0.f), node.value("Z", 0.f)} });
            }
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->warn("ArkEventsRevival: spawned_{}.json unreadable ({}) - picking up what could be read.", map_name, ex.what());
        }
        saved_creatures.erase(0);

        const EventConfig* event_cfg = g_settings.enabled ? CurrentEventConfig() : nullptr;
        const MapSpawns* here = event_cfg ? MapSpawnsFor(event_cfg->spawns, map_name) : nullptr;
        const bool same_event = here && saved_event == g_settings.active_event;
        if (here) StartTracking(event_cfg, *here);
        else ResetTracking();

        int adopted = 0, removed = 0;

        if (!saved_creatures.empty())
        {
            TArray<AActor*> actors;
            UGameplayStatics::GetAllActorsOfClass(world, APrimalDinoCharacter::GetPrivateStaticClass(L"/Script/ShooterGame"), &actors);
            for (AActor* actor : actors)
            {
                if (!actor || !actor->IsValidLowLevelFast(false)) continue;
                auto* dino = static_cast<APrimalDinoCharacter*>(actor);
                if (dino->BPIsTamed()) continue;
                const auto found = saved_creatures.find(CreatureId(dino));
                if (found == saved_creatures.end()) continue;
                const SavedCreature& saved = found->second;

                // A boss killed while nothing was tracking it (the plugin was
                // reloading, or had only just loaded) still gets its loot -
                // unless its crates are already on the ground.
                if (dino->IsDead())
                {
                    if (!same_event) continue;
                    const bool rewarded = std::any_of(saved_crates.begin(), saved_crates.end(),
                        [&saved](const SavedCrate& c) { return c.key == "reward|" + saved.key; });
                    for (size_t i = 0; i < here->bosses.size() && !rewarded; ++i)
                        if (here->bosses[i].key == saved.key) { OnBossDefeated(world, i, here->bosses[i], dino, ActorLocation(dino)); break; }
                    continue;
                }

                if (same_event && AdoptCreature(*here, saved.key, dino, saved.announced)) { ++adopted; continue; }
                dino->Destroy(false, true);
                ++removed;
            }
        }

        {
            UClass* container_class = APrimalStructureItemContainer::GetPrivateStaticClass(L"/Script/ShooterGame");
            std::unordered_map<UClass*, std::vector<AActor*>> by_class;
            for (const SavedCrate& saved : saved_crates)
            {
                UClass* crate_class = LoadClass(saved.class_path);
                if (!crate_class) continue;

                auto list_it = by_class.find(crate_class);
                if (list_it == by_class.end())
                {
                    TArray<AActor*> actors;
                    UGameplayStatics::GetAllActorsOfClass(world, crate_class, &actors);
                    std::vector<AActor*> list;
                    for (AActor* actor : actors)
                        if (actor && actor->IsValidLowLevelFast(false) && container_class && actor->IsA(container_class))
                            list.push_back(actor);
                    list_it = by_class.emplace(crate_class, std::move(list)).first;
                }

                // The crate that stands where this one was recorded. Crates
                // don't move, so a match within a metre is the same crate.
                auto& candidates = list_it->second;
                auto match = std::find_if(candidates.begin(), candidates.end(),
                    [&saved](AActor* actor) { return FVector::Distance(ActorLocation(actor), saved.at) < 100.f; });
                if (match == candidates.end()) continue;
                AActor* crate = *match;
                candidates.erase(match);

                if (same_event && CrateHasItems(static_cast<APrimalStructureItemContainer*>(crate)) && AdoptCrate(*here, saved.key, crate))
                {
                    ++adopted;
                    continue;
                }
                crate->Destroy(false, true);
                ++removed;
            }
        }

        SaveSpawned();
        if (adopted > 0 || removed > 0)
            if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: from before the restart/reload - picked back up {}, removed {} (event over or no longer in config).",
                adopted, removed);
    }

    // ---- event spawns: spawning ----

    void ClearWildDinosNear(UWorld* world, const FVector& location, float radius)
    {
        if (!world || radius <= 0.f) return;

        TArray<AActor*> actors;
        UGameplayStatics::GetAllActorsOfClass(world, APrimalDinoCharacter::GetPrivateStaticClass(L"/Script/ShooterGame"), &actors);

        int cleared = 0;
        for (auto* actor : actors)
        {
            if (!actor || !actor->IsValidLowLevelFast(false) || !actor->RootComponentField()) continue;
            if (FVector::Distance(location, ActorLocation(actor)) > radius) continue;
            auto* dino = static_cast<APrimalDinoCharacter*>(actor);
            if (dino->BPIsTamed() || IsTrackedBoss(dino)) continue; // never player pets, never another boss
            // Nor one a player is busy with: knocked out (being tamed), or
            // carried / carrying.
            if (!dino->IsConscious() || dino->bIsCarried()() || dino->CarriedCharacterField().Get()) continue;
            dino->Destroy(false, true);
            ++cleared;
        }

        if (g_settings.debug_mode && cleared > 0)
            Log::GetLog()->info("ArkEventsRevival: cleared {} wild dino(s) around a boss spawn point.", cleared);
    }

    // The point straight below on the ground, lifted by "above" - spawn
    // points learned from flying creatures can hang high in the sky. If no
    // ground is found, the point as it was.
    FVector OnGround(UWorld* world, const FVector& point, float above)
    {
        FVector ground{};
        FVector probe{ point.X, point.Y, point.Z + 200.f };
        FVector up{0.f, 0.f, 0.f};
        FVector down{0.f, 0.f, -100000.f};
        if (!world || !UVictoryCore::GetGroundLocation(world, &ground, &probe, &up, &down) || !std::isfinite(ground.Z))
            return point;
        return FVector{ point.X, point.Y, ground.Z + above };
    }

    // ---- Room for a creature ----------------------------------------------
    // OnGround only knows the landscape height. A rock, a cliff or a cave roof
    // is not in it, so a spawn "150 units above the landscape" can sit inside a
    // mesh, and nothing stopped a creature appearing on top of another. These
    // look at what is really there: traces on ECC_GameTraceChannel2, the channel
    // that sees rock, cliffs, caves and structures, and the creatures around.
    struct SolidHit
    {
        bool hit{false};
        float distance{0.f};
        bool structure{false}; // what it hit is a structure, not rock or terrain
    };

    SolidHit TraceSolid(UWorld* world, const FVector& from, const FVector& to, AActor* ignore)
    {
        SolidHit out;
        // FHitResult is heap-allocated at its REAL size: the SDK's struct is an
        // empty shell, and a stack FHitResult crashed this server once.
        const int size = GetStructSize<FHitResult>();
        if (!world || size <= 0) return out;
        std::vector<unsigned char> buffer(static_cast<size_t>(size), 0);
        auto* hit = reinterpret_cast<FHitResult*>(buffer.data());
        TArray<AActor*> ignores;
        if (ignore) ignores.Add(ignore);
        FVector start = from, end = to;
        if (!UVictoryCore::VTraceSingleBP_IgnoreActorsArray(world, hit, &start, &end, &ignores, ignore,
            ECollisionChannel::ECC_GameTraceChannel2, 0, FName(), false, true, -1.0f))
            return out;
        // Another creature in the way is not part of the world.
        AActor* blocker = hit->ActorField().Get();
        if (blocker && blocker->IsA(APrimalCharacter::GetPrivateStaticClass())) return out;
        out.hit = true;
        out.distance = hit->TimeField() * FVector::Distance(from, to);
        out.structure = blocker && blocker->IsA(APrimalStructure::GetPrivateStaticClass());
        return out;
    }

    // 0 = room to stand, 1 = tight (a ceiling or a wall close by), 2 = buried
    // (inside rock, or the ground is not where it was thought to be).
    // `feet` is the lowest point of the creature, `radius` and `height` its
    // real size.
    int RoomAt(UWorld* world, const FVector& feet, float radius, float height, AActor* ignore)
    {
        // Straight up from the feet: a trace that STARTS inside solid geometry
        // hits at once, so a hit within a few units means buried.
        const float headroom = (std::min)(height, 500.f) + 100.f;
        const SolidHit up = TraceSolid(world, FVector{feet.X, feet.Y, feet.Z + 10.f},
            FVector{feet.X, feet.Y, feet.Z + headroom}, ignore);
        if (up.hit && up.distance < 40.f) return 2;

        int severity = up.hit ? 1 : 0; // a roof within reach: tight, not buried

        // Walls close around it, at mid height.
        const float reach = (std::min)(radius, 500.f);
        const FVector mid{feet.X, feet.Y, feet.Z + (std::min)(height * 0.5f, 250.f)};
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i) * 0.785398f;
            const SolidHit side = TraceSolid(world, mid,
                FVector{mid.X + std::cos(angle) * reach, mid.Y + std::sin(angle) * reach, mid.Z}, ignore);
            if (side.hit && side.distance < reach * 0.6f) severity = (std::max)(severity, 1);
        }

        // Something to stand on (not needed in water).
        if (!PointIsUnderwater(world, mid) &&
            !TraceSolid(world, FVector{feet.X, feet.Y, feet.Z + 50.f}, FVector{feet.X, feet.Y, feet.Z - 300.f}, ignore).hit)
            severity = (std::max)(severity, 1);
        return severity;
    }

    // Is another creature (wild or tamed) where this one would stand?
    bool OverlapsCreature(UWorld* world, AActor* self, const FVector& centre, const FVector& extent)
    {
        TArray<AActor*> found;
        const float own = (std::max)(extent.X, extent.Y);
        UVictoryCore::ServerOctreeOverlapActors(&found, world, centre, own + 3000.f, EServerOctreeGroup::DINOPAWNS, true);
        for (AActor* other : found)
        {
            if (!other || other == self) continue;
            FVector origin{}, other_extent{};
            other->GetActorBounds(true, &origin, &other_extent);
            const float flat = std::sqrt((origin.X - centre.X) * (origin.X - centre.X) + (origin.Y - centre.Y) * (origin.Y - centre.Y));
            if (flat < (own + (std::max)(other_extent.X, other_extent.Y)) * 0.9f
                && std::fabs(origin.Z - centre.Z) < extent.Z + other_extent.Z)
                return true;
        }
        return false;
    }

    // The real surface at a point: the first solid thing found going down from
    // probe_up above it - rock, a cliff, terrain or a structure. GetGroundLocation
    // only knows the landscape, so a crate put "on the ground" there ended up under
    // a rock, or hanging in the air where a creature died on top of one. If the point
    // itself is inside solid geometry (a corpse sunk into a rock) the surface ABOVE
    // it is used. With nothing solid found - or CheckSpawnClearance off - it is the
    // landscape lookup as before.
    // max_drop is how far below the point the ground may be: a flyer that died
    // high up needs a long way down, but a crate beside a boss at a cliff edge must
    // not end up in the valley far below, so most callers keep it short.
    bool SurfaceAt(UWorld* world, const FVector& location, float probe_up, float& out_z, float max_drop = 3000.f)
    {
        const FVector from{location.X, location.Y, location.Z + probe_up};
        if (g_settings.spawn_clearance)
        {
            const SolidHit below = TraceSolid(world, from, FVector{location.X, location.Y, location.Z - max_drop}, nullptr);
            if (below.hit && below.distance > 30.f)
            {
                out_z = from.Z - below.distance;
                return true;
            }
            if (below.hit) // the trace started inside solid geometry
            {
                const FVector high{location.X, location.Y, location.Z + 5000.f};
                const SolidHit top = TraceSolid(world, high, location, nullptr);
                if (top.hit)
                {
                    out_z = high.Z - top.distance;
                    return true;
                }
            }
        }
        FVector ground{};
        FVector probe = from;
        FVector up{0.f, 0.f, 0.f};
        FVector down{0.f, 0.f, -100000.f};
        if (!UVictoryCore::GetGroundLocation(world, &ground, &probe, &up, &down) || !std::isfinite(ground.Z)) return false;
        if (g_settings.spawn_clearance && location.Z - ground.Z > max_drop) return false; // too far below to be "where it died"
        out_z = ground.Z;
        return true;
    }

    // After a creature has spawned: its real size against the world and the other
    // creatures. If it is buried or sitting on another creature it is moved to
    // clear ground nearby; false means no clear spot was found, so the caller
    // removes it. A creature that is merely in a tight spot is left where it is.
    bool SettleCreature(UWorld* world, APrimalDinoCharacter* dino, const FVector& around,
        bool surface_known = false, float surface_z = 0.f, float stagger = 0.f)
    {
        if (!g_settings.spawn_clearance || !world || !dino) return true;

        FVector origin{}, extent{};
        dino->GetActorBounds(true, &origin, &extent);
        if (extent.X < 1.f && extent.Y < 1.f) return true; // no bounds to judge by

        // Exactly the right height: the ground, then the creature's own height, so
        // its LOWEST point is just above the surface (10 cm, plus the stagger for a
        // spawn close to a recent one) and it drops onto the ground from there. A big
        // creature that spawned half in the ground is raised; a small one that spawned
        // higher than it needs to is lowered.
        if (surface_known)
        {
            const float lowest = origin.Z - extent.Z;
            const float wanted = surface_z + 10.f + stagger;
            const float delta = wanted - lowest;
            if (std::fabs(delta) > 20.f && std::fabs(delta) < 3000.f)
            {
                FVector where = ActorLocation(dino);
                where.Z += delta;
                FRotator facing = dino->RootComponentField() ? dino->RootComponentField()->RelativeRotationField() : FRotator{};
                if (!dino->TeleportTo(&where, &facing, false, true)) dino->SetActorLocation(&where, false);
                origin.Z += delta;
                if (g_settings.debug_mode)
                    Log::GetLog()->info("ArkEventsRevival: set a spawn's height so it starts {} cm above the ground ({} cm {}).",
                        static_cast<int>(10.f + stagger), static_cast<int>(std::fabs(delta)), delta > 0.f ? "up" : "down");
            }
        }
        const float radius = (std::max)(extent.X, extent.Y);
        const float height = extent.Z * 2.f;
        const FVector feet_now{origin.X, origin.Y, origin.Z - extent.Z};
        // From the lowest point up to where the actor's own origin sits.
        const float origin_above_feet = ActorLocation(dino).Z - feet_now.Z;

        const int severity_now = RoomAt(world, feet_now, radius, height, dino);
        const bool crowded_now = OverlapsCreature(world, dino, origin, extent);
        if (severity_now == 0 && !crowded_now) return true;

        auto move_to = [&](const FVector& feet)
        {
            FVector where{feet.X, feet.Y, feet.Z + origin_above_feet + 5.f};
            FRotator facing = dino->RootComponentField() ? dino->RootComponentField()->RelativeRotationField() : FRotator{};
            if (!dino->TeleportTo(&where, &facing, false, true)) dino->SetActorLocation(&where, false);
        };

        // Buried in rock, or under a cliff or a cave roof: bring it UP onto the top of
        // the collision above it - the first solid surface found coming down from
        // high up - as long as that spot is clear, dry and not someone's building.
        if (severity_now == 2)
        {
            const FVector from{origin.X, origin.Y, origin.Z + 5000.f};
            const SolidHit surface = TraceSolid(world, from, FVector{origin.X, origin.Y, origin.Z}, dino);
            if (surface.hit && !surface.structure)
            {
                const FVector feet{origin.X, origin.Y, from.Z - surface.distance + 10.f};
                const FVector centre{origin.X, origin.Y, feet.Z + extent.Z};
                const bool dry = !g_settings.land_only || !PointIsOverWater(world, FVector{feet.X, feet.Y, feet.Z + 100.f});
                if (dry && RoomAt(world, feet, radius, height, dino) == 0 && !OverlapsCreature(world, dino, centre, extent))
                {
                    move_to(feet);
                    if (g_settings.debug_mode)
                        Log::GetLog()->info("ArkEventsRevival: brought a buried spawn up onto the ground above it, {}m higher.",
                            static_cast<int>((feet.Z - feet_now.Z) / 100.f));
                    return true;
                }
            }
        }

        // Rings of candidate spots around where it was meant to go, nearest first.
        const bool aquatic = !g_settings.land_only && PointIsUnderwater(world, origin);
        const float phase = static_cast<float>(rand() % 360) * 0.0174533f;
        bool have_tight = false;
        FVector tight_feet{};
        for (int ring = 0; ring < 3; ++ring)
        {
            const float distance = radius * 2.2f * static_cast<float>(ring + 1) + 200.f;
            for (int i = 0; i < 8; ++i)
            {
                const float angle = phase + static_cast<float>(i) * 0.785398f;
                const float x = around.X + std::cos(angle) * distance;
                const float y = around.Y + std::sin(angle) * distance;
                // The real ground there: first solid thing below, within 5 m either way.
                const SolidHit ground = TraceSolid(world, FVector{x, y, around.Z + 500.f}, FVector{x, y, around.Z - 500.f}, dino);
                if (!ground.hit) continue;
                const FVector feet{x, y, around.Z + 500.f - ground.distance + 10.f};
                if (!aquatic && PointIsOverWater(world, FVector{x, y, feet.Z + 100.f})) continue;

                const int severity = RoomAt(world, feet, radius, height, dino);
                if (severity == 2) continue;
                const FVector centre{x, y, feet.Z + extent.Z};
                if (OverlapsCreature(world, dino, centre, extent)) continue;
                if (severity == 0)
                {
                    move_to(feet);
                    if (g_settings.debug_mode)
                        Log::GetLog()->info("ArkEventsRevival: moved a spawn that was {} to clear ground {}m away.",
                            severity_now == 2 ? "buried" : (crowded_now ? "overlapping another creature" : "in a tight spot"),
                            static_cast<int>(distance / 100.f));
                    return true;
                }
                if (!have_tight) { have_tight = true; tight_feet = feet; }
            }
        }

        // No fully clear spot. A merely tight one is fine for a creature that was
        // buried or overlapping; one that was only tight stays where it is.
        if (have_tight && (severity_now == 2 || crowded_now))
        {
            move_to(tight_feet);
            return true;
        }
        return severity_now < 2 && !crowded_now ? true : false;
    }

    // Creatures that spawn close together drop in one after another instead of
    // all appearing at one height: each spawn within kStaggerRadius of a recent one
    // (within kStaggerWindow) starts kStaggerStep higher than the last, up to
    // kStaggerMax. They fall to the ground, so they do not overlap on arrival.
    std::vector<std::pair<FVector, std::chrono::steady_clock::time_point>> g_recent_spawns;
    constexpr float kStaggerRadius = 2000.f;  // 20 m
    constexpr float kStaggerStep = 100.f;     // 1 m
    constexpr float kStaggerMax = 400.f;      // 4 m: a short drop
    constexpr auto kStaggerWindow = std::chrono::seconds(45);

    float StaggerHeight(const FVector& at)
    {
        const auto now = std::chrono::steady_clock::now();
        g_recent_spawns.erase(std::remove_if(g_recent_spawns.begin(), g_recent_spawns.end(),
            [now](const auto& entry) { return now - entry.second > kStaggerWindow; }), g_recent_spawns.end());
        int close_count = 0;
        for (const auto& entry : g_recent_spawns)
        {
            const float dx = entry.first.X - at.X, dy = entry.first.Y - at.Y;
            if (std::sqrt(dx * dx + dy * dy) < kStaggerRadius) ++close_count;
        }
        return (std::min)(static_cast<float>(close_count) * kStaggerStep, kStaggerMax);
    }

    APrimalDinoCharacter* SpawnCreature(UWorld* world, UClass* dino_class, const FVector& location, int level, bool wander)
    {
        const FRotator facing{0.f, static_cast<float>(rand() % 360), 0.f};
        // The ground height first (rock and cliffs included, not just the landscape),
        // then the creature goes in just above it - plus a little more for each
        // recent spawn close by, so they drop in one after another.
        float surface_z = 0.f;
        const bool surface_known = g_settings.spawn_clearance && SurfaceAt(world, location, 500.f, surface_z);
        const float stagger = StaggerHeight(location);
        const float base_z = surface_known ? surface_z + 150.f : location.Z;
        const FVector spawn_at{location.X, location.Y, base_z + stagger};
        // SpawnDino rather than SpawnActorInWorld: it sets the creature up as
        // the game would, where a bare actor spawn leaves it half-initialised.
        // Same arguments RaidBaseBuilder uses for its defenders.
        // SpawnDino rather than SpawnActorInWorld: it sets the creature up as
        // the game would, where a bare actor spawn leaves it half-initialised.
        // The level is GIVEN - the game does not roll one for a spawn made this way.
        APrimalDinoCharacter* dino = APrimalDinoCharacter::SpawnDino(
            world, TSubclassOf<APrimalDinoCharacter>(dino_class), spawn_at, facing,
            1.0f,       // level multiplier
            0,          // extra level offset
            false,      // add offset before multiplier
            true,       // override the base NPC level
            level,
            !wander,    // "do not wander"
            1.0f,       // AI range multiplier
            level,
            false);
        if (dino && !SettleCreature(world, dino, location, surface_known, surface_z, stagger))
        {
            if (g_settings.debug_mode)
                Log::GetLog()->info("ArkEventsRevival: removed a spawn at {} that was buried or overlapping with no clear spot nearby.",
                    Coords(location));
            dino->Destroy(true, true);
            return nullptr;
        }
        if (dino)
        {
            g_spawned_dirty = true;
            g_recent_spawns.push_back({location, std::chrono::steady_clock::now()});
        }
        return dino;
    }

    // Same call and arguments RaidBaseBuilder uses to fill its vaults.
    UPrimalItem* AddCrateItem(UPrimalInventoryComponent* inventory, const CrateItem& item)
    {
        UClass* item_class = LoadClassOf(item.class_path, ClassKind::Item, "crate Items entry");
        if (!item_class) return nullptr;

        const int span = (std::max)(1, item.max_quantity - item.min_quantity + 1);
        const int quantity = (std::max)(1, item.min_quantity + rand() % span);

        return UPrimalItem::AddNewItem(
            TSubclassOf<UPrimalItem>(item_class), inventory, false, false,
            0.0f, true, quantity, false,
            0.0f, false, TSubclassOf<UPrimalItem>(nullptr),
            0.0f, true, true);
    }

    // Drops: each item rolled on its own Chance.
    void FillDropCrate(APrimalStructureItemContainer* crate, const std::vector<CrateItem>& items)
    {
        UPrimalInventoryComponent* inventory = crate->MyInventoryComponentField();
        if (!inventory) return;
        for (const CrateItem& item : items)
        {
            if (item.chance < 1.f && static_cast<float>(rand() % 1000) / 1000.f >= item.chance) continue;
            AddCrateItem(inventory, item);
        }
    }

    // Defeat crates: exactly `picks` items, each picked from the list with
    // Chance as its share - an item with Chance 1.0 turns up twice as often
    // as one with 0.5.
    void FillPickedCrate(APrimalStructureItemContainer* crate, const std::vector<CrateItem>& items, int picks)
    {
        UPrimalInventoryComponent* inventory = crate->MyInventoryComponentField();
        if (!inventory || items.empty()) return;

        float total = 0.f;
        for (const CrateItem& item : items) total += (std::max)(0.f, item.chance);

        for (int n = 0; n < picks; ++n)
        {
            size_t chosen = static_cast<size_t>(rand()) % items.size();
            if (total > 0.f)
            {
                float roll = static_cast<float>(rand() % 10000) / 10000.f * total;
                for (size_t i = 0; i < items.size(); ++i)
                {
                    roll -= (std::max)(0.f, items[i].chance);
                    if (roll < 0.f) { chosen = i; break; }
                }
            }
            AddCrateItem(inventory, items[chosen]);
        }
    }

    // The crate class to use: the main one, or the fallback when the main one
    // won't load (a path this server's game version doesn't have).
    UClass* CrateClassOf(const std::string& main_path, const std::string& fallback_path, const char* what)
    {
        if (UClass* main = LoadClassOf(main_path, ClassKind::Container, what)) return main;
        return fallback_path.empty() ? nullptr : LoadClassOf(fallback_path, ClassKind::Container, what);
    }

    // Kill drops: MinItems to MaxItems DIFFERENT items, each picked with
    // Chance as its share of the draw.
    void FillDistinctCrate(APrimalStructureItemContainer* crate, const std::vector<CrateItem>& items, int min_items, int max_items)
    {
        UPrimalInventoryComponent* inventory = crate->MyInventoryComponentField();
        if (!inventory || items.empty()) return;
        const int low = (std::max)(1, min_items);
        const int high = (std::max)(low, max_items);
        int wanted = (std::min)(static_cast<int>(items.size()), low + rand() % (high - low + 1));

        std::vector<size_t> pool(items.size());
        for (size_t i = 0; i < pool.size(); ++i) pool[i] = i;
        while (wanted-- > 0 && !pool.empty())
        {
            float total = 0.f;
            for (size_t i : pool) total += (std::max)(0.001f, items[i].chance);
            float roll = static_cast<float>(rand() % 10000) / 10000.f * total;
            size_t slot = pool.size() - 1;
            for (size_t k = 0; k < pool.size(); ++k)
            {
                roll -= (std::max)(0.001f, items[pool[k]].chance);
                if (roll < 0.f) { slot = k; break; }
            }
            AddCrateItem(inventory, items[pool[slot]]);
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(slot));
        }
    }

    // Puts one crate down ON the ground under location and fills it. A crate
    // is a structure and does not fall - spawned in the air it just hangs
    // there (RaidBaseBuilder found this out). The ground is looked for
    // straight DOWN from probe_up above the point, so a cave-floor point gets
    // the floor rather than the surface above it. Never in water or a cave.
    // Returns null if it couldn't place a usable crate; nothing half-made is
    // left behind.
    AActor* PlaceCrate(UWorld* world, UClass* crate_class, const FVector& location, float probe_up, bool include_crate_loot,
        const std::function<void(APrimalStructureItemContainer*)>& fill, UClass* fallback_class = nullptr,
        float max_drop = 3000.f)
    {
        float surface_z = 0.f;
        if (!SurfaceAt(world, location, probe_up, surface_z, max_drop)) return nullptr;
        // Not inside rock: the caller tries another spot.
        if (g_settings.spawn_clearance
            && RoomAt(world, FVector{location.X, location.Y, surface_z}, 60.f, 150.f, nullptr) == 2)
            return nullptr;
        const FVector spot{location.X, location.Y, surface_z + 150.f};
        if (PointIsUnderwater(world, spot) || PointIsInCave(world, spot)) return nullptr;
        const FRotator flat{0.f, 0.f, 0.f};

        AActor* spawned = UVictoryCore::SpawnActorInWorld(
            world, TSubclassOf<AActor>(crate_class), spot, flat,
            nullptr, 0, FName(), nullptr, nullptr);
        // The main crate wouldn't spawn - the fallback one (e.g. a red crate).
        if (!spawned && fallback_class && fallback_class != crate_class)
            spawned = UVictoryCore::SpawnActorInWorld(
                world, TSubclassOf<AActor>(fallback_class), spot, flat,
                nullptr, 0, FName(), nullptr, nullptr);
        if (!spawned) return nullptr;

        // A crate does not fall, so it has to be right: measure where its bottom is
        // and set it just above the surface, whatever its origin offset is.
        if (g_settings.spawn_clearance)
        {
            FVector origin{}, extent{};
            spawned->GetActorBounds(true, &origin, &extent);
            if (extent.Z > 5.f && extent.Z < 500.f)
            {
                const float delta = (surface_z + 5.f) - (origin.Z - extent.Z);
                if (std::fabs(delta) > 5.f && std::fabs(delta) < 400.f)
                {
                    FVector where = spot;
                    where.Z += delta;
                    spawned->SetActorLocation(&where, false);
                }
            }
        }

        auto* crate = static_cast<APrimalStructureItemContainer*>(spawned); // LoadClassOf checked it is a container
        UClass* supply_crate_class = APrimalStructureItemContainer_SupplyCrate::GetPrivateStaticClass(L"/Script/ShooterGame");
        UPrimalInventoryComponent* inventory = crate->MyInventoryComponentField();

        // Anything the crate put in by itself as it spawned - noted BEFORE the
        // event items go in.
        std::vector<UPrimalItem*> own;
        if (inventory && !include_crate_loot)
        {
            TArray<UPrimalItem*> before = inventory->InventoryItemsField();
            for (UPrimalItem* item : before) if (item && !item->bIsEngram()()) own.push_back(item);
        }
        if (include_crate_loot && supply_crate_class && spawned->IsA(supply_crate_class))
            NativeCall<void>(crate, "APrimalStructureItemContainer_SupplyCrate.GenerateCrateItems");

        // Event items in FIRST, then the crate's own loot out. A supply crate
        // that goes empty removes itself (that is how a looted drop vanishes)
        // - emptying it before filling is why the earlier crates disappeared
        // the moment they spawned.
        fill(crate);
        if (inventory && CrateHasItems(crate))
            for (UPrimalItem* item : own)
            {
                FItemNetID id = item->ItemIDField();
                inventory->RemoveItem(&id, false, false, true, false);
            }

        // Every chance roll missed - an empty box would count as looted at
        // once and just churn, so don't keep it.
        if (!CrateHasItems(crate))
        {
            crate->Destroy(false, true);
            return nullptr;
        }
        g_spawned_dirty = true;
        return spawned;
    }

    // Still counts while it exists and has something in it. Looted empty,
    // it is removed here so an empty box doesn't sit there forever. Only
    // containers are ever tracked, and a TWeakObjectPtr never resolves to a
    // different object, so the cast is safe.
    bool CrateStillCounts(TWeakObjectPtr<AActor>& ref)
    {
        AActor* actor = ref.Get();
        if (!actor) return false;
        auto* crate = static_cast<APrimalStructureItemContainer*>(actor);
        if (CrateHasItems(crate)) return true;
        crate->Destroy(false, true);
        return false;
    }

    // The level of a NON-BOSS event creature (wild ones and boss minions): a random
    // multiple of 5 between min_level and max_level, as a normal wild spawn on a
    // difficulty-5 server would have (5 to 150 by default). A wyvern may go up to
    // WyvernMaxLevel (190), the same as ModifiedSpawnLevelDistribution allows it.
    // Bosses are not given a level here - they use their own configured Level.
    int EventCreatureLevel(const std::string& class_path, int min_level, int max_level)
    {
        constexpr int step = 5;
        std::string lowered = class_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool wyvern = lowered.find("wyvern") != std::string::npos;
        const int top = wyvern ? (std::max)(max_level, g_settings.wyvern_max_level) : max_level;
        const int low = (std::max)(step, (min_level + step - 1) / step * step);
        const int high = (std::max)(low, top / step * step);
        return low + (rand() % ((high - low) / step + 1)) * step;
    }

    // ---- bosses ----

    bool IsNativeHere(const std::vector<std::string>& like); // with the game spawns, below

    // A boss sends its minions in waves: MaxMinions (minions_per_wave) at a time, one every few
    // seconds. When the whole wave has been killed, after MinionRespawnSeconds the next wave comes -
    // up to WaveCount waves (10) in all. Stops when the boss dies.
    void TickMinions(UWorld* world, size_t index, ActiveBoss& active, const RegionBoss& entry, float delta_seconds)
    {
        const int cap = entry.minions_per_wave;
        if (entry.minion_classes.empty() || cap <= 0) return;

        auto& minions = g_minion_tracked[index];
        minions.erase(std::remove_if(minions.begin(), minions.end(),
            [](TWeakObjectPtr<APrimalDinoCharacter>& ref) { return !IsEventWild(ref.Get()); }), minions.end());
        // Minions are held to the boss's area too: past the leash distance from
        // the boss's spawn point, or in water, they are put back near it.
        if (entry.leash_radius > 0.f && active.leash_timer == 0.f) // same once-a-second beat as the boss's leash
            for (auto& ref : minions)
                if (APrimalDinoCharacter* minion = ref.Get())
                {
                    const FVector at = ActorLocation(minion);
                    if (FVector::Distance(at, active.location) <= entry.leash_radius && !PointIsUnderwater(world, at)) continue;
                    const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
                    const float ring = 1000.f + static_cast<float>(rand() % 800);
                    FVector home = OnGround(world, FVector{ active.location.X + std::cos(angle) * ring,
                        active.location.Y + std::sin(angle) * ring, active.location.Z }, 150.f);
                    FRotator facing = minion->RootComponentField() ? minion->RootComponentField()->RelativeRotationField() : FRotator{};
                    if (!minion->TeleportTo(&home, &facing, false, true)) minion->SetActorLocation(&home, false);
                    SettleCreature(world, minion, home); // off the rock and off the others; left as it is if nowhere is clear
                }

        const int alive = static_cast<int>(minions.size());
        if (active.wave_spawned >= cap)
        {
            if (alive > 0) { active.minion_wait = -1.f; return; } // the wave is still out
            if (active.waves_sent >= entry.wave_count) return;    // every wave has come
            if (active.minion_wait < 0.f)
            {
                active.minion_wait = entry.wave_interval_seconds;
                if (g_settings.debug_mode)
                    Log::GetLog()->info("ArkEventsRevival: '{}' wave {}/{} killed - the next in {}s.", entry.class_path,
                        active.waves_sent, entry.wave_count, static_cast<int>(entry.wave_interval_seconds));
            }
            active.minion_wait -= delta_seconds;
            if (active.minion_wait > 0.f) return;
            active.minion_wait = -1.f;
            active.wave_spawned = 0;
        }
        if (active.wave_spawned == 0 && active.waves_sent >= entry.wave_count) return;

        active.wave_timer += delta_seconds;
        if (active.wave_timer < 3.f) return;
        active.wave_timer = 0.f;

        // Beside the boss where it stands now, a few metres out.
        APrimalDinoCharacter* boss = active.boss.Get();
        const FVector centre = boss ? ActorLocation(boss) : active.location;
        // A few tries for a spot on land (the boss may stand at a shore).
        FVector spot{};
        bool dry = false;
        for (int attempt = 0; attempt < 6 && !dry; ++attempt)
        {
            const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
            const float offset = 900.f + static_cast<float>(rand() % 900);
            spot = OnGround(world, FVector{ centre.X + std::cos(angle) * offset, centre.Y + std::sin(angle) * offset, centre.Z }, 150.f);
            dry = !PointIsOverWater(world, spot);
        }
        if (!dry) return; // try again on the next step

        std::string path = entry.minion_classes[static_cast<size_t>(rand()) % entry.minion_classes.size()];
        if (entry.random_minions)
        {
            // Any creature of the event's wild list that lives on this map.
            std::vector<const std::string*> pool;
            if (const MapSpawns* here = ActiveMapSpawns(world))
                for (const WildSpawnDino& wild : here->dinos)
                {
                    if (!wild.require_native.empty() && !IsNativeHere(wild.require_native)) continue;
                    if (!wild.spawn_like.empty() && !IsNativeHere(wild.spawn_like)) continue;
                    pool.push_back(&wild.class_path);
                }
            if (!pool.empty()) path = *pool[static_cast<size_t>(rand()) % pool.size()];
        }
        UClass* minion_class = LoadClassOf(path, ClassKind::Creature, "boss MinionClasses entry");
        if (!minion_class) return;
        const EventConfig* event_cfg = CurrentEventConfig();
        const int minion_level = event_cfg
            ? EventCreatureLevel(path, event_cfg->spawns.min_level, event_cfg->spawns.max_level)
            : entry.minion_level;
        if (APrimalDinoCharacter* minion = SpawnCreature(world, minion_class, spot, minion_level, true))
        {
            minions.push_back(GetWeakReference(minion));
            if (active.wave_spawned == 0) ++active.waves_sent;
            ++active.wave_spawned;
            if (g_settings.debug_mode)
                Log::GetLog()->info("ArkEventsRevival: '{}' wave {}/{} minion {}/{} ('{}').", entry.class_path,
                    active.waves_sent, entry.wave_count, active.wave_spawned, cap, path);
        }
    }

    // Spawns up to `count` of this boss entry at random points in its region.
    // Not announced here - see ActiveBoss::announced.
    int SpawnBosses(UWorld* world, size_t index, const RegionBoss& entry, int count)
    {
        UClass* boss_class = LoadClassOf(entry.class_path, ClassKind::Creature, "Bosses Class");
        if (!boss_class) return 0;

        int spawned = 0;
        for (int n = 0; n < count; ++n)
        {
            // Up to 10 tries: the exact spot it would stand on (on the ground)
            // is checked again for water and player structures; if either is
            // there, another spawn point is picked.
            FVector location;
            bool found = false;
            for (int attempt = 0; attempt < 10 && !found; ++attempt)
            {
                if (!PickPoolLocation(entry.regions, location)) break;
                location = OnGround(world, location, 150.f); // just above the ground, never up in the sky
                found = !PointIsOverWater(world, location) && !NearPlayerStructure(world, location) && !NearMapEdge(location);
            }
            if (!found) break;

            ClearWildDinosNear(world, location, entry.clear_radius);
            // Free to wander - it roams its event area; the leash keeps it inside.
            APrimalDinoCharacter* boss = SpawnCreature(world, boss_class, location, entry.level, true);
            if (!boss) continue;

            ActiveBoss active;
            active.boss = GetWeakReference(boss);
            active.location = location;
            g_boss_tracked[index].push_back(active);
            ++spawned;

            if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: boss '{}' spawned at X={} Y={} Z={} ({}).",
                entry.class_path, location.X, location.Y, location.Z, Coords(location));
        }
        if (spawned == 0 && count > 0 && g_settings.debug_mode)
            Log::GetLog()->info("ArkEventsRevival: boss '{}' is due but no spawn point matches its region yet - retrying in 30s.", entry.class_path);
        return spawned;
    }

    // A ring of crates where the boss fell, each filled with picks from the
    // boss's DefeatCrates items. A ring spot that is in water, in a cave or
    // has no ground under it tries closer in, then the spot the boss fell on,
    // then the same ring around where the boss first appeared (a spawn point
    // already known to be dry land) - so a boss that dies in a lake still
    // pays out.
    int DropDefeatCrates(UWorld* world, size_t index, const RegionBoss& entry, const FVector& at, const FVector& fallback)
    {
        const DefeatCrates& loot = entry.defeat_crates;
        if (loot.crate_path.empty() || loot.count <= 0 || loot.items.empty()) return 0;
        UClass* crate_class = CrateClassOf(loot.crate_path, loot.fallback_crate_path, "Bosses DefeatCrates Crate");
        if (!crate_class) return 0;

        // Round the arena edge: evenly spaced just inside the lines, each one
        // moved inward until it is on dry land. "fallback" is the arena centre.
        if (loot.around_arena && entry.arena_radius > 0.f)
        {
            UClass* fallback_edge = loot.fallback_crate_path.empty() ? nullptr
                : LoadClassOf(loot.fallback_crate_path, ClassKind::Container, "Bosses DefeatCrates FallbackCrate");
            auto fill_edge = [&loot](APrimalStructureItemContainer* crate) { FillPickedCrate(crate, loot.items, loot.items_per_crate); };
            int placed_edge = 0;
            for (int k = 0; k < loot.count; ++k)
            {
                const float angle = 6.2831853f * static_cast<float>(k) / static_cast<float>(loot.count);
                AActor* crate = nullptr;
                for (float scale : { 0.92f, 0.8f, 0.65f, 0.5f, 0.3f })
                {
                    const float r = entry.arena_radius * scale;
                    const FVector spot{ fallback.X + std::cos(angle) * r, fallback.Y + std::sin(angle) * r, fallback.Z };
                    crate = PlaceCrate(world, crate_class, spot, 3000.f, loot.include_crate_loot, fill_edge, fallback_edge);
                    if (crate) break;
                }
                if (!crate) continue;
                g_reward_tracked[index].push_back(GetWeakReference(crate));
                ++placed_edge;
            }
            if (placed_edge > 0) return placed_edge;
            // nothing fitted round the edge - fall through to the ring where it fell
        }
        UClass* fallback_class = loot.fallback_crate_path.empty() ? nullptr
            : LoadClassOf(loot.fallback_crate_path, ClassKind::Container, "Bosses DefeatCrates FallbackCrate");

        auto fill = [&loot](APrimalStructureItemContainer* crate) { FillPickedCrate(crate, loot.items, loot.items_per_crate); };

        // Where the boss died first; if that is water, the nearest dry ground
        // around it (searched outward, like an ordinary kill drop); its spawn
        // point only as a last resort.
        // A boss that died high up (a flyer): the ring goes round the GROUND straight
        // below where it died, found first however far down that is.
        FVector ground_at = at;
        float ground_z = 0.f;
        if (SurfaceAt(world, at, 400.f, ground_z, 100000.f)) ground_at.Z = ground_z;

        std::vector<FVector> near_spots;
        for (float distance : { 1000.f, 2000.f, 3000.f, 4500.f })
            for (int step = 0; step < 8; ++step)
            {
                const float a2 = 0.785398f * static_cast<float>(step);
                const FVector p{ ground_at.X + std::cos(a2) * distance, ground_at.Y + std::sin(a2) * distance, ground_at.Z };
                if (!PointIsOverWater(world, p)) near_spots.push_back(p);
                if (near_spots.size() >= 2) break;
            }
        std::vector<const FVector*> centres{ &ground_at };
        for (const FVector& p : near_spots) centres.push_back(&p);
        centres.push_back(&fallback);

        int placed = 0;
        for (int k = 0; k < loot.count; ++k)
        {
            const float angle = 6.2831853f * static_cast<float>(k) / static_cast<float>(loot.count);
            AActor* crate = nullptr;
            for (const FVector* centre : centres)
            {
                for (float radius : { loot.ring_radius, loot.ring_radius * 0.5f, 0.f })
                {
                    const FVector spot{centre->X + std::cos(angle) * radius, centre->Y + std::sin(angle) * radius, centre->Z};
                    crate = PlaceCrate(world, crate_class, spot, 400.f, loot.include_crate_loot, fill, fallback_class);
                    if (crate) break;
                }
                if (crate) break;
            }
            if (!crate) continue;
            g_reward_tracked[index].push_back(GetWeakReference(crate));
            ++placed;
        }
        return placed;
    }

    // fallback: where the boss first appeared - see DropDefeatCrates.
    void OnBossDefeated(UWorld* world, size_t index, const RegionBoss& entry, APrimalDinoCharacter* boss, const FVector& fallback)
    {
        const FVector at = ActorLocation(boss);
        const std::string name = BossName(entry, boss);
        const int placed = DropDefeatCrates(world, index, entry, at, fallback);
        g_spawned_dirty = true; // the dead boss leaves spawned_<map>.json either way
        if (placed > 0)
        {
            if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: boss '{}' defeated at {} - {} loot crate(s) dropped.", entry.class_path, Coords(at), placed);
            Announce(g_settings.boss_defeated_message, name, at);
        }
        else if (!entry.defeat_crates.items.empty())
            Log::GetLog()->warn("ArkEventsRevival: boss '{}' defeated at {} but no loot crate could be placed (no dry ground nearby).", entry.class_path, Coords(at));
    }

    // Chat line + on-screen message to the players within range of a point.
    void TellPlayersNear(const FVector& at, float range, const std::string& pattern, const std::string& boss_name, int seconds = 0)
    {
        if (pattern.empty()) return;
        std::string text = pattern;
        ReplaceAll(text, "{boss}", boss_name);
        ReplaceAll(text, "{seconds}", std::to_string(seconds));
        ReplaceAll(text, "{lat}", Coords(at));
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world) return;
        UClass* pc_class = AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame");
        for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField())
        {
            APlayerController* pc = weak_pc.Get();
            if (!pc || !pc_class || !pc->IsA(pc_class)) continue;
            auto* player = static_cast<AShooterPlayerController*>(pc);
            AShooterCharacter* body = player->GetPlayerCharacter();
            if (!body || FVector::Distance(ActorLocation(body), at) > range) continue;
            ArkApi::GetApiUtils().SendChatMessage(player, ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
            ArkApi::GetApiUtils().SendNotification(player, FLinearColor{ 1.0f, 0.6f, 0.1f, 1.0f }, 1.3f, 6.0f, nullptr, L"{}", *ToFString(text));
        }
    }

    void TellPlayer(AShooterPlayerController* player, const std::string& pattern, const std::string& boss_name, const FLinearColor& colour)
    {
        if (!player || pattern.empty()) return;
        std::string text = pattern;
        ReplaceAll(text, "{boss}", boss_name);
        ArkApi::GetApiUtils().SendChatMessage(player, ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
        ArkApi::GetApiUtils().SendNotification(player, colour, 1.3f, 5.0f, nullptr, L"{}", *ToFString(text));
    }

    // Once a second: players who have just come into or gone out of a boss's
    // event area are told so.
    void UpdatePlayersInArena(ActiveBoss& active, const RegionBoss& entry, const std::string& boss_name)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world || entry.arena_radius <= 0.f) return;
        UClass* pc_class = AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame");
        std::unordered_set<uint64_t> now_inside;
        for (TWeakObjectPtr<APlayerController> weak_pc : world->PlayerControllerListField())
        {
            APlayerController* pc = weak_pc.Get();
            if (!pc || !pc_class || !pc->IsA(pc_class)) continue;
            auto* player = static_cast<AShooterPlayerController*>(pc);
            AShooterCharacter* body = player->GetPlayerCharacter();
            if (!body || body->IsDead()) continue;
            const uint64_t id = ArkApi::GetApiUtils().GetSteamIdFromController(player);
            const bool inside = FVector::Distance(ActorLocation(body), active.location) <= entry.arena_radius;
            if (inside) now_inside.insert(id);
            const bool was_inside = active.players_inside.count(id) > 0;
            if (inside && !was_inside)
                TellPlayer(player, g_settings.player_entered_message, boss_name, FLinearColor{ 0.3f, 1.0f, 0.3f, 1.0f });
            else if (!inside && was_inside)
                TellPlayer(player, g_settings.player_left_message, boss_name, FLinearColor{ 1.0f, 0.6f, 0.1f, 1.0f });
        }
        active.players_inside = std::move(now_inside);
    }

    // A ring on the ground around a boss's spawn point - the same replicated
    // debug-line call RaidBaseBuilder draws its shield with, sent from the
    // game state so every client sees it.
    // Team stamped on this plugin's domes, so they are found and cleared after
    // a restart. Below the player-tribe range, so KeepAwayFromStructures and
    // the arena guard never treat a dome as a player's.
    constexpr int kDomeTeam = 4242;

    UClass* DomeClass()
    {
        return LoadClass("Blueprint'/Game/Extinction/CoreBlueprints/HordeCrates/StorageBox_HordeShield.StorageBox_HordeShield'");
    }

    // Writes a Blueprint variable by name, straight at its offset, after
    // checking it exists and is the right size.
    template <typename T>
    bool SetBlueprintValue(UObject* object, const char* name, const T& value)
    {
        if (!object || !object->ClassField()) return false;
        UProperty* prop = object->ClassField()->FindProperty(FName(name, EFindName::FNAME_Find));
        if (!prop || prop->ElementSizeField() != static_cast<int>(sizeof(T)) || prop->Offset_InternalField() <= 0) return false;
        *reinterpret_cast<T*>(reinterpret_cast<char*>(object) + prop->Offset_InternalField()) = value;
        return true;
    }

    template <typename T>
    bool GetBlueprintValue(UObject* object, const char* name, T& out)
    {
        if (!object || !object->ClassField()) return false;
        UProperty* prop = object->ClassField()->FindProperty(FName(name, EFindName::FNAME_Find));
        if (!prop || prop->ElementSizeField() != static_cast<int>(sizeof(T)) || prop->Offset_InternalField() <= 0) return false;
        out = *reinterpret_cast<T*>(reinterpret_cast<char*>(object) + prop->Offset_InternalField());
        return true;
    }

    // "Dome size value" handed to the shield's own resize event. Settable
    // live with ArkEventsRevival.DomeSize while finding the right number.
    int g_dome_size_value{-1};  // -1 = SafeZone's formula from the radius

    // Sizes a dome: its base radius (DefaultShieldRadius) set to the arena
    // radius, then the shield's resize event with the size value. Logs what
    // the shield reports, so the right numbers can be found in a test.
    void ResizeDome(AActor* actor, float radius, int size_value)
    {
        SetBlueprintValue(actor, "DefaultShieldRadius", radius);
        int arg = size_value;
        if (arg < 0)
        {
            const int r = static_cast<int>(radius);
            const int offset = r <= 1500 ? 35 : r <= 5000 ? 25 : r <= 10000 ? 20 : -10;
            arg = ((r / 8) - offset) / 10;
        }
        if (UFunction* resize = actor->FindFunctionChecked(FName("NetRefreshRadiusScale", EFindName::FNAME_Add)))
        {
            alignas(8) unsigned char params[64] = {};
            *reinterpret_cast<int*>(params) = arg;
            actor->ProcessEvent(resize, params);
        }
        float base = 0.f, now = 0.f;
        GetBlueprintValue(actor, "DefaultShieldRadius", base);
        GetBlueprintValue(actor, "CurrentShieldRadiusInterp", now);
        if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: dome sized - wanted radius {}, size value {}, DefaultShieldRadius now {}, CurrentShieldRadiusInterp {}.",
            radius, arg, base, now);
    }

    // The orbital-drop shield, set up the way the SafeZone plugin does: its
    // colour, "intermission" mode (no pawn pushing - players and tames walk
    // through), level 1, and sized to the arena with NetRefreshRadiusScale.
    AActor* SpawnDome(const FVector& centre, float radius, const FLinearColor& colour)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        UClass* dome_class = DomeClass();
        if (!world || !dome_class) return nullptr;

        FVector at = centre;
        FRotator rot{ 0.f, 0.f, 180.f };
        FActorSpawnParameters params;
        params.bDeferBeginPlay = true;
        AActor* actor = world->SpawnActor(dome_class, &at, &rot, &params);
        if (!actor) return nullptr;

        auto* structure = static_cast<APrimalStructureItemContainer*>(actor);
        SetBlueprintValue(structure, "ShieldColor", colour);
        structure->MulticastProperty(FName("ShieldColor", EFindName::FNAME_Add));
        structure->BeginPlay();
        structure->bDisableActivationUnderwater() = false;
        structure->SetContainerActive(true);
        structure->bCanBeDamaged() = false;
        structure->bDestroyOnStasis() = false;
        SetBlueprintValue(structure, "CurrentLevel", 1);
        SetBlueprintValue(structure, "bIntermissionShield", true);

        ResizeDome(actor, radius, g_dome_size_value);

        actor->TargetingTeamField() = kDomeTeam;
        actor->ForceReplicateNow(false, false);
        return actor;
    }

    void RemoveDome(ActiveBoss& active)
    {
        if (AActor* dome = active.dome.Get()) dome->Destroy(false, true);
    }

    // Domes whose boss has gone: kept up 2 more minutes (time to loot the
    // crates), then removed. {dome, seconds left}
    std::vector<std::pair<TWeakObjectPtr<AActor>, float>> g_fading_domes;

    // Arena rings kept on screen 2 minutes after their boss has gone:
    // {centre, radius, seconds left, seconds to next redraw}.
    struct FadingRings { FVector centre; float radius; float left; float redraw; };
    std::vector<FadingRings> g_fading_rings;

    void RetireDome(ActiveBoss& active, float radius = 0.f)
    {
        if (active.dome.Get()) g_fading_domes.push_back({ active.dome, 120.f });
        if (radius > 0.f) g_fading_rings.push_back({ active.location, radius, 120.f, 0.f });
    }

    void DrawBossRing(const FVector& centre, float radius, float lifetime); // below

    void TickFadingDomes(float delta_seconds)
    {
        for (auto it = g_fading_rings.begin(); it != g_fading_rings.end();)
        {
            it->left -= delta_seconds;
            it->redraw -= delta_seconds;
            if (it->left <= 0.f) { it = g_fading_rings.erase(it); continue; }
            if (it->redraw <= 0.f) { it->redraw = 5.f; DrawBossRing(it->centre, it->radius, 6.f); }
            ++it;
        }
        for (auto it = g_fading_domes.begin(); it != g_fading_domes.end();)
        {
            it->second -= delta_seconds;
            AActor* dome = it->first.Get();
            if (dome && it->second > 0.f) { ++it; continue; }
            if (dome) dome->Destroy(false, true);
            it = g_fading_domes.erase(it);
        }
    }

    // Every dome of ours in the world, gone - old ones left by a restart.
    void ClearAllDomes()
    {
        g_fading_domes.clear();
        g_fading_rings.clear();
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        UClass* dome_class = DomeClass();
        if (!world || !dome_class) return;
        TArray<AActor*> found;
        UGameplayStatics::GetAllActorsOfClass(world, dome_class, &found);
        for (AActor* actor : found)
            if (actor && actor->IsValidLowLevelFast(false) && actor->TargetingTeamField() == kDomeTeam)
                actor->Destroy(false, true);
    }

    void DrawBossRing(const FVector& centre, float radius, float lifetime)
    {
        // Drawn as a dome of stacked rings, like RaidBaseBuilder's shield - a
        // see-through "shield" players and tames walk through; only damage
        // from outside it is stopped (see HookDinoTakeDamage).
        AActor* sender = static_cast<AActor*>(ArkApi::GetApiUtils().GetGameState());
        if (!sender) return;
        const FLinearColor colour{ 0.2f, 0.6f, 1.0f, 1.0f };
        constexpr int kSegments = 48;
        constexpr int kRings = 5; // RaidBaseBuilder's shield: rings stacked up into a dome
        for (int ring = 0; ring < kRings; ++ring)
        {
            const float t = static_cast<float>(ring) / static_cast<float>(kRings);
            const float height = 100.f + t * radius * 0.6f;
            const float r = radius * std::sqrt(1.0f - t * t);
            for (int k = 0; k < kSegments; ++k)
            {
                const float a0 = 6.2831853f * k / kSegments;
                const float a1 = 6.2831853f * (k + 1) / kSegments;
                FVector from{ centre.X + std::cos(a0) * r, centre.Y + std::sin(a0) * r, centre.Z + height };
                FVector to{ centre.X + std::cos(a1) * r, centre.Y + std::sin(a1) * r, centre.Z + height };
                sender->MulticastDrawDebugLine(from, to, colour, lifetime, 40.0f, true);
            }
        }
    }

    // In-game clock, seconds into the day (0 - 86400).
    bool IsInGameNight()
    {
        AShooterGameState* game_state = ShooterGameState();
        if (!game_state) return true; // can't tell - don't hold the boss back forever
        const float day_time = game_state->DayTimeField();
        return day_time >= 78600.f || day_time < 18900.f;
    }

    void TickBosses(float delta_seconds, UWorld* world, const MapSpawns& here)
    {
        TickFadingDomes(delta_seconds);
        for (size_t i = 0; i < here.bosses.size(); ++i)
        {
            const RegionBoss& entry = here.bosses[i];
            auto& active_list = g_boss_tracked[i];

            // Gone, dead or tamed bosses stop counting (their minions stay
            // as event creatures until the event ends). Killed ones - the
            // corpse is still there, dead - drop their loot first.
            for (auto it = active_list.begin(); it != active_list.end();)
            {
                APrimalDinoCharacter* boss = it->boss.Get();
                if (IsEventWild(boss)) { ++it; continue; }
                if (boss && boss->IsDead()) OnBossDefeated(world, i, entry, boss, it->location);
                RetireDome(*it, entry.arena_radius); // rings (and any dome) stay 2 more minutes, then go
                it = active_list.erase(it);
            }

            for (ActiveBoss& active : active_list)
            {
                if (!active.announced)
                {
                    active.age += delta_seconds;
                    if (active.age >= kAnnounceDelaySeconds)
                    {
                        APrimalDinoCharacter* boss = active.boss.Get(); // still wild - checked just above
                        Announce(g_settings.boss_spawned_message, BossName(entry, boss), ActorLocation(boss));
                        active.announced = true;
                    }
                }

                // Leash: once a second, too far from home or in water -> back home.
                active.leash_timer += delta_seconds;
                if (entry.leash_radius > 0.f && active.leash_timer >= 1.f)
                {
                    active.leash_timer = 0.f;
                    if (APrimalDinoCharacter* boss = active.boss.Get())
                    {
                        const FVector now = ActorLocation(boss);
                        const float from_home = FVector::Distance(now, active.location);
                        const std::string name = BossName(entry, boss);
                        UpdatePlayersInArena(active, entry, name);

                        // Outside the dome: a countdown, told to the players
                        // around it; back inside cancels it.
                        bool send_back = from_home > entry.leash_radius || PointIsUnderwater(world, now);
                        if (!send_back && entry.arena_radius > 0.f && from_home > entry.arena_radius)
                        {
                            if (active.return_in < 0.f) active.return_in = entry.return_countdown_seconds;
                            else active.return_in -= 1.f;
                            const int left = static_cast<int>(std::ceil(active.return_in));
                            if (left <= 0) send_back = true;
                            else if (left == static_cast<int>(entry.return_countdown_seconds) || left <= 5)
                                TellPlayersNear(now, entry.arena_radius + 6000.f, g_settings.boss_leaving_message, name, left);
                        }
                        else if (!send_back) active.return_in = -1.f;

                        if (send_back)
                        {
                            active.return_in = -1.f;
                            FVector home{ active.location.X, active.location.Y, active.location.Z + 200.f };
                            FRotator facing = boss->RootComponentField() ? boss->RootComponentField()->RelativeRotationField() : FRotator{};
                            if (!boss->TeleportTo(&home, &facing, false, true)) boss->SetActorLocation(&home, false);
                            active.warned_leaving = false;
                            if (g_settings.debug_mode)
                                Log::GetLog()->info("ArkEventsRevival: boss '{}' strayed to {} - sent back to its spawn point.", entry.class_path, Coords(now));
                        }
                    }
                }

                // No damage for HealAfter... - back to full health.
                active.since_damage += delta_seconds;
                if (entry.heal_after_seconds > 0.f && active.since_damage >= entry.heal_after_seconds)
                {
                    active.since_damage = 0.f;
                    if (APrimalDinoCharacter* boss = active.boss.Get())
                        if (UPrimalCharacterStatusComponent* status = boss->MyCharacterStatusComponentField())
                        {
                            float* current = status->CurrentStatusValuesField()();
                            float* maximum = status->MaxStatusValuesField()();
                            if (current && maximum && current[0] < maximum[0])
                            {
                                current[0] = maximum[0]; // [0] = health
                                if (g_settings.debug_mode)
                                    Log::GetLog()->info("ArkEventsRevival: boss '{}' untouched for {} min - healed to full.",
                                        entry.class_path, static_cast<int>(entry.heal_after_seconds / 60.f));
                            }
                        }
                }

                // The arena ring, redrawn every few seconds (a debug line fades).
                active.ring_timer -= delta_seconds;
                if (entry.arena_radius > 0.f && active.ring_timer <= 0.f)
                {
                    active.ring_timer = 5.f;
                    if (entry.shield_dome)
                    {
                        // Put the dome up (again, if it was lost).
                        if (!active.dome.Get())
                            if (AActor* dome = SpawnDome(active.location, entry.arena_radius, entry.dome_color))
                                active.dome = GetWeakReference(dome);
                    }
                    // The rings are always drawn too, so the area can be seen
                    // whatever the dome does.
                    DrawBossRing(active.location, entry.arena_radius, 6.f);
                }

                TickMinions(world, i, active, entry, delta_seconds);
            }

            const int alive = static_cast<int>(active_list.size());
            const bool my_turn = i == g_boss_turn % here.bosses.size();

            // Set in-game hours: at each listed hour, spawn only if missing.
            if (!entry.spawn_hours.empty() && i < g_boss_hour_done.size())
            {
                AShooterGameState* state = ShooterGameState();
                const int hour = state ? static_cast<int>(state->DayTimeField() / 3600.f) % 24 : -1;
                const bool listed = std::find(entry.spawn_hours.begin(), entry.spawn_hours.end(), hour) != entry.spawn_hours.end();
                if (!listed) { g_boss_hour_done[i] = -1; continue; }
                if (!my_turn || g_boss_hour_done[i] == hour) continue; // not its turn / this hour already handled
                // A boss (any) still up when the hour comes: this hour is skipped.
                if (AnyBossAlive()) { g_boss_hour_done[i] = hour; continue; }
                g_boss_timers[i] -= delta_seconds; // retry pacing while no spawn point is free
                if (g_boss_timers[i] > 0.f) continue;
                if (SpawnBosses(world, i, entry, 1) > 0) { g_boss_hour_done[i] = hour; ++g_boss_turn; }
                else g_boss_timers[i] = 30.f;
                continue;
            }

            if (!my_turn || AnyBossAlive()) continue; // one boss at a time, in turn
            if (alive >= entry.amount)
            {
                g_boss_timers[i] = 0.f; // the respawn countdown starts when one is missing
                continue;
            }
            if (g_boss_timers[i] < entry.respawn_seconds) g_boss_timers[i] += delta_seconds;
            if (g_boss_timers[i] < entry.respawn_seconds) continue;
            if (entry.night_only && !IsInGameNight()) continue; // due - held until night

            const int spawned = SpawnBosses(world, i, entry, 1);
            if (spawned > 0) ++g_boss_turn;
            // Nowhere to put it yet (pool still filling after a start) -
            // try again shortly instead of waiting out the whole respawn.
            g_boss_timers[i] = spawned > 0 ? 0.f : (std::max)(0.f, entry.respawn_seconds - 30.f);
        }
    }

    // ---- wild creatures and crates ----

    // The per-map crate list for kill drops, resolved once per path.
    UClass* PickKillCrateClass(const KillDrops& drops)
    {
        std::vector<UClass*> usable;
        for (const std::string& path : drops.crate_paths)
            if (UClass* c = LoadClassOf(path, ClassKind::Container, "KillDrops Crates entry")) usable.push_back(c);
        if (!usable.empty()) return usable[static_cast<size_t>(rand()) % usable.size()];
        return drops.fallback_crate_path.empty() ? nullptr
            : LoadClassOf(drops.fallback_crate_path, ClassKind::Container, "KillDrops FallbackCrate");
    }

    // One crate where an event dino died, with a few different event items.
    void DropKillCrate(UWorld* world, const SpawnsConfig& cfg, APrimalDinoCharacter* dino)
    {
        const KillDrops& drops = cfg.kill_drops;
        if (drops.items.empty()) return;
        static int kills = 0;
        if (drops.every_kills > 0) { if (++kills % drops.every_kills != 0) return; }
        else if (drops.chance < 1.f && static_cast<float>(rand() % 10000) / 10000.f >= drops.chance) return;
        UClass* crate_class = PickKillCrateClass(drops);
        if (!crate_class) return;
        UClass* fallback_class = drops.fallback_crate_path.empty() ? nullptr
            : LoadClassOf(drops.fallback_crate_path, ClassKind::Container, "KillDrops FallbackCrate");

        // Where it died may be high in the air (a flyer shot down): find the ground
        // straight below that spot first, however far down it is, and place the crate
        // - and any nearby fallback spots - around THAT ground point.
        FVector at = ActorLocation(dino);
        float ground_z = 0.f;
        if (SurfaceAt(world, at, 300.f, ground_z, 100000.f)) at.Z = ground_z;
        auto fill = [&drops](APrimalStructureItemContainer* crate) { FillDistinctCrate(crate, drops.items, drops.min_items, drops.max_items); };
        AActor* crate = PlaceCrate(world, crate_class, at, 300.f, false, fill, fallback_class);
        if (!crate)
        {
            // Died in water or a cave - put it on the nearest dry ground a little way off.
            for (int attempt = 0; attempt < 6 && !crate; ++attempt)
            {
                const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
                const float offset = 500.f + static_cast<float>(attempt) * 400.f;
                crate = PlaceCrate(world, crate_class, { at.X + std::cos(angle) * offset, at.Y + std::sin(angle) * offset, at.Z },
                    600.f, false, fill, fallback_class);
            }
        }
        if (crate)
        {
            g_kill_crates.push_back(GetWeakReference(crate));
            if (g_settings.debug_mode)
            {
                FString class_name;
                crate->ClassField()->NameField().ToString(&class_name);
                if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: kill drop '{}' spawned at {} (X={} Y={} Z={}).",
                    ToStdString(class_name), Coords(at), at.X, at.Y, at.Z);
            }
        }
        else if (g_settings.debug_mode)
            Log::GetLog()->warn("ArkEventsRevival: kill drop at {} - no dry ground found nearby, none placed.", Coords(at));
    }

    // Every tick: event dinos that have died (corpse still there) drop their
    // crate and stop counting; gone or tamed ones just stop counting.
    void TickKills(UWorld* world, const SpawnsConfig& cfg, const MapSpawns& here)
    {
        for (size_t i = 0; i < here.dinos.size() && i < g_dino_tracked.size(); ++i)
        {
            auto& tracked = g_dino_tracked[i];
            for (auto it = tracked.begin(); it != tracked.end();)
            {
                APrimalDinoCharacter* dino = it->Get();
                if (IsEventWild(dino)) { ++it; continue; }
                if (dino && dino->IsDead()) DropKillCrate(world, cfg, dino);
                it = tracked.erase(it);
            }
        }
        g_kill_crates.erase(std::remove_if(g_kill_crates.begin(), g_kill_crates.end(),
            [](TWeakObjectPtr<AActor>& ref) { return !ref.Get(); }), g_kill_crates.end());
    }

    // Every check: decide how many of each dino still need spawning. Down to
    // MinCount starts the RespawnDelay wait (not before the first fill);
    // when it ends, the entry is queued back up to RespawnTo-MaxCount.
    void PlanDinos(float elapsed, const MapSpawns& here)
    {
        for (size_t i = 0; i < here.dinos.size(); ++i)
        {
            const WildSpawnDino& entry = here.dinos[i];
            if (entry.game_spawn) continue; // the game's own spawn zones do this one
            const int alive = static_cast<int>(g_dino_tracked[i].size());
            if (g_dino_pending[i] > 0) continue; // still filling
            if (alive > entry.min_count) { g_dino_wait[i] = -1.f; continue; }

            if (g_dino_filled[i] && entry.respawn_delay_seconds > 0.f)
            {
                if (g_dino_wait[i] < 0.f)
                {
                    g_dino_wait[i] = entry.respawn_delay_seconds;
                    if (g_settings.debug_mode)
                        Log::GetLog()->info("ArkEventsRevival: '{}' down to {} - back up in {} minute(s).",
                            entry.class_path, alive, static_cast<int>(entry.respawn_delay_seconds / 60.f));
                    continue;
                }
                g_dino_wait[i] -= elapsed;
                if (g_dino_wait[i] > 0.f) continue;
            }
            g_dino_wait[i] = -1.f;

            const int low = (std::max)(alive + 1, entry.respawn_to > 0 ? entry.respawn_to : entry.min_count + 1);
            const int target = low >= entry.max_count ? entry.max_count : low + rand() % (entry.max_count - low + 1);
            g_dino_pending[i] = (std::max)(0, target - alive);
        }
    }

    // A spot for the next dino of an entry, kept apart from its others so a
    // fill spreads out instead of piling up on one spawn point.
    // Has the game spawned any creature matching one of these class parts here?
    bool IsNativeHere(const std::vector<std::string>& like)
    {
        for (const auto& [class_tag, points] : g_pool_by_class)
            for (const auto& wanted : like)
                if (!points.empty() && class_tag.find(wanted) != std::string::npos) return true;
        return false;
    }

    // Distance from a point to the nearest tracked event creature of any kind (a huge number if none).
    float NearestEventCreature(const FVector& point)
    {
        float nearest = 1.0e9f;
        for (auto& list : g_dino_tracked)
            for (auto& ref : list)
                if (APrimalDinoCharacter* other = ref.Get())
                    nearest = (std::min)(nearest, FVector::Distance(ActorLocation(other), point));
        return nearest;
    }

    // ---- spawning alongside the game's own spawns ----
    //
    // "SpawnThroughGame": instead of the plugin picking a spot, every creature the game's own spawn zones
    // spawn is a chance for one event creature, put right beside it. So the event creatures appear
    // wherever the normal wildlife of that kind spawns, spread over the whole region the way the game
    // spreads it, and keep coming back as the game respawns its own. (An earlier version added the
    // creatures to the game's spawn tables instead; the table layout of this game version does not match
    // the API headers, so that was dropped - nothing here reads or changes the game's tables.)

    bool GameEntryMatches(const WildSpawnDino& entry, const std::string& region_tag, const std::string& creature_tag)
    {
        if (!region_tag.empty() && IsCaveRegion(region_tag)) return false;
        if (!entry.require_native.empty() && !IsNativeHere(entry.require_native)) return false;
        if (!entry.spawn_like.empty())
        {
            for (const auto& like : entry.spawn_like)
                if (creature_tag.find(like) != std::string::npos) return true;
            return false;
        }
        if (entry.regions.empty()) return true; // "Anywhere"
        for (const auto& region : entry.regions)
            if (region_tag.find(region) != std::string::npos) return true;
        return false;
    }

    // As above, plus now and then a game spawn in ANY other (non-cave) region: OtherRegionChance.
    bool GameEntryAccepts(const WildSpawnDino& entry, const std::string& region_tag, const std::string& creature_tag)
    {
        if (GameEntryMatches(entry, region_tag, creature_tag)) return true;
        const bool has_home = !entry.spawn_like.empty() || !entry.regions.empty();
        // Only creatures native to this map, wherever on it they turn up.
        if (!entry.require_native.empty() && !IsNativeHere(entry.require_native)) return false;
        if (!entry.spawn_like.empty() && !IsNativeHere(entry.spawn_like)) return false;
        return has_home && !IsCaveRegion(region_tag) && !region_tag.empty() && g_settings.other_region_chance > 0.f
            && static_cast<float>(rand() % 1000) < g_settings.other_region_chance * 1000.f;
    }

    // Called for every creature the game's own spawn zones spawn.
    void NoteNativeSpawn(const std::string& region_name, const std::string& creature_class, const FVector& at)
    {
        if (!g_settings.enabled || g_game_queue.size() >= 100) return;
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const MapSpawns* here = world ? ActiveMapSpawns(world) : nullptr;
        if (!here || g_spawns_event != g_settings.active_event || g_dino_tracked.size() != here->dinos.size()) return;

        const std::string region_tag = NormaliseTag(region_name);
        const std::string creature_tag = NormaliseTag(creature_class);
        for (size_t i = 0; i < here->dinos.size(); ++i)
        {
            const WildSpawnDino& entry = here->dinos[i];
            if (!entry.game_spawn) continue;
            if (static_cast<float>(rand() % 10000) >= entry.game_weight * 10000.f) continue;
            size_t queued = 0;
            for (const auto& request : g_game_queue) if (request.index == i) ++queued;
            if (static_cast<int>(g_dino_tracked[i].size() + queued) >= entry.max_count) continue;
            if (!GameEntryAccepts(entry, region_tag, creature_tag)) continue;
            g_game_queue.push_back({ i, at });
            if (g_settings.debug_mode)
            {
                static int shown = 0;
                if (shown++ < 30)
                    Log::GetLog()->info("ArkEventsRevival: queued '{}' beside a game spawn of '{}' in '{}'.", entry.class_path, creature_class, region_name);
            }
        }
        if (g_settings.debug_mode)
        {
            static int seen = 0;
            if (++seen % 200 == 1)
                Log::GetLog()->info("ArkEventsRevival: {} game spawns seen so far (latest '{}' in '{}'), {} event creature(s) waiting.", seen, creature_class, region_name, g_game_queue.size());
        }
    }

    // A few per tick: the spawn is put beside where the game just spawned one of its own.
    void ProcessGameQueue(UWorld* world, const SpawnsConfig& cfg, const MapSpawns& here)
    {
        for (int done = 0; done < 3 && !g_game_queue.empty(); ++done)
        {
            const GameSpawnRequest request = g_game_queue.front();
            g_game_queue.erase(g_game_queue.begin());
            if (request.index >= here.dinos.size() || request.index >= g_dino_tracked.size()) continue;
            const WildSpawnDino& entry = here.dinos[request.index];
            if (static_cast<int>(g_dino_tracked[request.index].size()) >= entry.max_count) continue;

            UClass* dino_class = LoadClassOf(entry.class_path, ClassKind::Creature, "Dinos Class");
            if (!dino_class) continue;

            // Beside the game's own spawn, not on top of it.
            FVector location = request.location;
            const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
            const float jitter = 300.f + static_cast<float>(rand() % 1200);
            location.X += std::cos(angle) * jitter;
            location.Y += std::sin(angle) * jitter;

            if (g_settings.land_only && PointIsOverWater(world, OnGround(world, location, 150.f))) continue;
            if (g_settings.spawn_spacing > 0.f && NearestEventCreature(location) < g_settings.spawn_spacing * 0.5f) continue;

            const int level = EventCreatureLevel(entry.class_path, cfg.min_level, cfg.max_level);
            const FVector spot = OnGround(world, location, 150.f);
            if (APrimalDinoCharacter* dino = SpawnCreature(world, dino_class, spot, level, true))
            {
                g_dino_tracked[request.index].push_back(GetWeakReference(dino));
                NoteEventDinoSpot(location);
                static int shown = 0;
                if (shown < 5 || g_settings.debug_mode)
                {
                    ++shown;
                    Log::GetLog()->info("ArkEventsRevival: spawned '{}' beside a game spawn at {} ({} alive).",
                        entry.class_path, Coords(location), g_dino_tracked[request.index].size());
                }
            }
        }
    }

    constexpr float kMinCrateSpacing = 1500.f; // 15 m - no crates stacked on one spawn point

    void RefillCrates(UWorld* world, const MapSpawns& here)
    {
        // Looted-empty defeat crates are cleared away on the same check.
        for (auto& list : g_reward_tracked)
            list.erase(std::remove_if(list.begin(), list.end(),
                [](TWeakObjectPtr<AActor>& ref) { return !CrateStillCounts(ref); }), list.end());

        std::vector<FVector> taken;
        for (auto& list : g_crate_tracked)
        {
            list.erase(std::remove_if(list.begin(), list.end(),
                [](TWeakObjectPtr<AActor>& ref) { return !CrateStillCounts(ref); }), list.end());
            for (auto& ref : list) taken.push_back(ActorLocation(ref.Get()));
        }

        for (size_t i = 0; i < here.drops.size(); ++i)
        {
            const RegionDrop& drop = here.drops[i];
            auto& tracked = g_crate_tracked[i];

            const int alive = static_cast<int>(tracked.size());
            if (alive > drop.min_count) continue; // not looted down to the refill point yet
            const int room = drop.max_count - alive;
            if (room <= 0) continue;

            UClass* crate_class = CrateClassOf(drop.crate_path, drop.fallback_crate_path, "Drops Crate");
            if (!crate_class) continue;
            UClass* fallback_class = drop.fallback_crate_path.empty() ? nullptr
                : LoadClassOf(drop.fallback_crate_path, ClassKind::Container, "Drops FallbackCrate");

            auto fill = [&drop](APrimalStructureItemContainer* crate)
            {
                if (drop.items_per_crate > 0) FillPickedCrate(crate, drop.items, drop.items_per_crate);
                else FillDropCrate(crate, drop.items);
            };
            // Top up to a random total between RespawnTo and MaxCount.
            const int low = (std::max)(alive + 1, drop.respawn_to > 0 ? drop.respawn_to : drop.min_count + 1);
            const int target = low >= drop.max_count ? drop.max_count : low + rand() % (drop.max_count - low + 1);
            const int count = (std::max)(1, target - alive);
            int placed = 0;
            for (int n = 0; n < count; ++n)
            {
                FVector location;
                bool found = false;
                for (int attempt = 0; attempt < 12 && !found; ++attempt)
                {
                    if (drop.near_event_dinos && !g_event_dino_spots.empty() && attempt < 8)
                    {
                        // A few metres from where an event dino spawned.
                        const FVector& spot = g_event_dino_spots[static_cast<size_t>(rand()) % g_event_dino_spots.size()];
                        const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
                        const float offset = 300.f + static_cast<float>(rand() % 500);
                        location = { spot.X + std::cos(angle) * offset, spot.Y + std::sin(angle) * offset, spot.Z };
                    }
                    else if (!PickPoolLocation(drop.regions, location)) break;
                    found = std::none_of(taken.begin(), taken.end(),
                        [&location](const FVector& other) { return FVector::Distance(location, other) < kMinCrateSpacing; });
                }
                if (!found) break;
                if (AActor* crate = PlaceCrate(world, crate_class, location, 150.f, drop.include_crate_loot, fill, fallback_class))
                {
                    tracked.push_back(GetWeakReference(crate));
                    taken.push_back(location);
                    ++placed;
                }
            }

            if (g_settings.debug_mode && placed > 0)
                Log::GetLog()->info("ArkEventsRevival: '{}' down to {} - placed {} more ({} out).",
                    drop.crate_path, alive, placed, tracked.size());
        }
    }

    // Once a minute: every living player has up to PerPlayer mounds within
    // MaxDistance; missing ones are placed on dry land MinDistance-MaxDistance
    // away, filled with MinItems-MaxItems different event items. Looted-empty
    // mounds are removed.
    void TickMounds(float delta_seconds, UWorld* world, const SpawnsConfig& cfg)
    {
        const PlayerMounds& m = cfg.mounds;
        if (m.class_path.empty() || m.per_player <= 0 || m.items.empty()) return;
        g_mound_timer += delta_seconds;
        if (g_mound_timer < 60.f) return;
        g_mound_timer = 0.f;

        g_mounds.erase(std::remove_if(g_mounds.begin(), g_mounds.end(),
            [](TWeakObjectPtr<AActor>& ref) { return !CrateStillCounts(ref); }), g_mounds.end());

        UClass* mound_class = LoadClassOf(m.class_path, ClassKind::Container, "Mounds Class");
        if (!mound_class) return;
        auto fill = [&m](APrimalStructureItemContainer* mound) { FillDistinctCrate(mound, m.items, m.min_items, m.max_items); };

        int placed = 0;
        ForEachPlayerCharacter([&](AShooterCharacter* character)
        {
            const FVector at = ActorLocation(character);
            int nearby = 0;
            for (auto& ref : g_mounds)
                if (AActor* mound = ref.Get())
                    if (FVector::Distance(ActorLocation(mound), at) <= m.max_distance) ++nearby;
            for (int n = nearby; n < m.per_player; ++n)
                for (int attempt = 0; attempt < 6; ++attempt)
                {
                    const float angle = static_cast<float>(rand() % 360) * 0.0174533f;
                    const float span = (std::max)(1.f, m.max_distance - m.min_distance);
                    const float distance = m.min_distance + static_cast<float>(rand() % static_cast<int>(span));
                    const FVector spot{ at.X + std::cos(angle) * distance, at.Y + std::sin(angle) * distance, at.Z };
                    if (NearPlayerStructure(world, spot) || NearMapEdge(spot)) continue;
                    if (AActor* mound = PlaceCrate(world, mound_class, spot, 1500.f, false, fill))
                    {
                        g_mounds.push_back(GetWeakReference(mound));
                        ++placed;
                        break;
                    }
                }
        });
        if (placed > 0 && g_settings.debug_mode)
            Log::GetLog()->info("ArkEventsRevival: {} mound(s) placed around players.", placed);
    }

    void TickSpawns(float delta_seconds, UWorld* world, const EventConfig* event_cfg)
    {
        if (!event_cfg || !event_cfg->spawns.enabled) return;
        const SpawnsConfig& cfg = event_cfg->spawns;
        const std::string map_name = CurrentMapNameLower(world);
        const MapSpawns* here = MapSpawnsFor(cfg, map_name);
        if (!here)
        {
            static std::unordered_set<std::string> noted; // event|map, logged once each
            if (g_settings.debug_mode && noted.insert(g_settings.active_event + "|" + map_name).second)
                Log::GetLog()->info("ArkEventsRevival: '{}' has no Spawns for map '{}' - nothing spawns here.",
                    g_settings.active_event, map_name);
            return;
        }

        if (g_spawns_event != g_settings.active_event || g_dino_tracked.size() != here->dinos.size()
            || g_dino_pending.size() != here->dinos.size()
            || g_crate_tracked.size() != here->drops.size() || g_boss_tracked.size() != here->bosses.size()
            || g_minion_tracked.size() != here->bosses.size() || g_reward_tracked.size() != here->bosses.size())
            StartTracking(event_cfg, *here);

        TickBosses(delta_seconds, world, *here); // every tick: waves and announcements have their own short timers
        TickKills(world, cfg, *here);            // every tick: a corpse drops its crate straight away
        TickPlayerBuffs(delta_seconds, cfg);
        TickMounds(delta_seconds, world, cfg);
        ProcessGameQueue(world, cfg, *here); // creatures put beside the game's own spawns (SpawnThroughGame)

        g_spawns_timer += delta_seconds;
        if (g_spawns_timer >= cfg.interval_seconds)
        {
            PlanDinos(g_spawns_timer, *here);
            g_spawns_timer = 0.f;
            RefillCrates(world, *here);
        }

        if (g_spawned_dirty) SaveSpawned();
    }

    // ---- arena damage guard ----
    //
    // A boss or one of its minions only takes damage from players (or their
    // tames, turrets and riders) standing INSIDE the boss's arena ring. From
    // outside, the hit is blocked and the player is told why - so nobody can
    // snipe the boss from far away or from their base.

    // The arena a creature belongs to: its boss entry and that boss's spawn
    // point. False if it is not a tracked boss or minion.
    bool ArenaOf(APrimalDinoCharacter* dino, const RegionBoss*& entry_out, FVector& centre_out)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const MapSpawns* here = ActiveMapSpawns(world);
        if (!here || g_spawns_event != g_settings.active_event) return false;
        for (size_t i = 0; i < here->bosses.size() && i < g_boss_tracked.size(); ++i)
        {
            bool mine = false;
            for (auto& active : g_boss_tracked[i]) if (active.boss.Get() == dino) { mine = true; break; }
            if (!mine && i < g_minion_tracked.size())
                for (auto& ref : g_minion_tracked[i]) if (ref.Get() == dino) { mine = true; break; }
            if (!mine || g_boss_tracked[i].empty()) continue;
            entry_out = &here->bosses[i];
            centre_out = g_boss_tracked[i].front().location;
            return true;
        }
        return false;
    }

    std::unordered_map<uint64_t, std::time_t> g_outside_told; // player id -> last told, so it is not spammed

    DECLARE_HOOK(APrimalDinoCharacter_TakeDamage, float, APrimalDinoCharacter*, float, FDamageEvent*, AController*, AActor*);
    float HookDinoTakeDamage(APrimalDinoCharacter* victim, float damage, FDamageEvent* damage_event, AController* instigator, AActor* causer)
    {
        const RegionBoss* entry = nullptr;
        FVector centre{};
        if (g_settings.enabled && victim && (instigator || causer) && ArenaOf(victim, entry, centre) && entry->arena_radius > 0.f)
        {
            // Where the attacker really is. A player's (or rider's) own body
            // first; otherwise whatever fired it - a turret, or the turret or
            // player that owns a projectile (a shot lands next to the boss, so
            // the projectile itself would always look "inside"). Only the
            // player side - tribe/player team - is ever blocked, never wild.
            AActor* source = instigator ? static_cast<AActor*>(instigator->PawnField()) : nullptr;
            if (!source && causer)
            {
                source = causer;
                for (int hop = 0; hop < 3; ++hop)
                {
                    AActor* owner = source->OwnerField();
                    if (!owner || owner == source) break;
                    source = owner;
                }
            }
            const bool player_side = source && source->TargetingTeamField() >= 50000;
            if (player_side && FVector::Distance(ActorLocation(source), centre) > entry->arena_radius)
            {
                UClass* pc_class = AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame");
                if (instigator && pc_class && instigator->IsA(pc_class))
                {
                    auto* player = static_cast<AShooterPlayerController*>(instigator);
                    const uint64_t id = ArkApi::GetApiUtils().GetSteamIdFromController(player);
                    const std::time_t now = std::time(nullptr);
                    if (now - g_outside_told[id] >= 5)
                    {
                        g_outside_told[id] = now;
                        std::string text = g_settings.outside_arena_message;
                        ReplaceAll(text, "{boss}", BossName(*entry, victim));
                        if (!text.empty())
                        {
                            ArkApi::GetApiUtils().SendChatMessage(player, ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
                            ArkApi::GetApiUtils().SendNotification(player, FLinearColor{ 1.0f, 0.3f, 0.3f, 1.0f }, 1.3f, 4.0f, nullptr, L"{}", *ToFString(text));
                        }
                    }
                }
                return 0.f; // blocked
            }
        }
        // A boss taking real damage starts its no-damage heal clock again.
        if (entry)
            for (auto& list : g_boss_tracked)
                for (ActiveBoss& active : list)
                    if (active.boss.Get() == victim) active.since_damage = 0.f;
        return APrimalDinoCharacter_TakeDamage_original(victim, damage, damage_event, instigator, causer);
    }

    // ---- spawn-point pool feed ----
    //
    // Only records where the game's own wild creatures spawn (see
    // g_pool_by_region) - the spawn itself is left completely untouched.

    DECLARE_HOOK(ANPCZoneManager_SpawnFromEntries, APrimalDinoCharacter*, ANPCZoneManager*, int, TArray<FNPCSpawnEntry>*, FVector*, TArray<APrimalDinoCharacter*>*, float);
    APrimalDinoCharacter* HookSpawnFromEntries(ANPCZoneManager* zone_manager, int spawn_volume_entry,
        TArray<FNPCSpawnEntry>* spawn_entries, FVector* at_spawn_point, TArray<APrimalDinoCharacter*>* all_spawned_npcs, float override_yaw)
    {
        APrimalDinoCharacter* spawned = ANPCZoneManager_SpawnFromEntries_original(
            zone_manager, spawn_volume_entry, spawn_entries, at_spawn_point, all_spawned_npcs, override_yaw);

        if (!g_settings.enabled || !spawned) return spawned;

        // Feed the spawn-point pool from every real wild spawn, regardless
        // of whether an event is active - so the pool is already warm by
        // the time an event starts.
        std::string region_name;
        if (UClass* container_class = zone_manager ? zone_manager->NPCSpawnEntriesContainerObjectField().uClass : nullptr)
        {
            // container_class IS the class object itself here, not an
            // instance - its own NameField() is what we want, not
            // ClassField()->NameField() (which would just give UClass's own
            // metaclass name).
            FString class_name;
            container_class->NameField().ToString(&class_name);
            region_name = ToStdString(class_name);
        }
        const FVector point_location = spawned->RootComponentField()
            ? spawned->RootComponentField()->RelativeLocationField()
            : *at_spawn_point;
        std::string creature_class;
        if (UClass* spawned_class = spawned->ClassField())
        {
            FString class_name;
            spawned_class->NameField().ToString(&class_name);
            creature_class = ToStdString(class_name);
        }
        if (!creature_class.empty()) NoteNativeSpawn(region_name, creature_class, point_location);
        RecordWildSpawnPoint(point_location, region_name, creature_class);

        return spawned;
    }

    // ---- calendar / switching ----

    // month is 1-12, day is 1-31. Compared as a single (month,day) pair so
    // a range that wraps year-end (e.g. Dec 14 - Jan 6) just needs
    // end < start to trigger the "OR" branch instead of "AND".
    bool DateInRange(int month, int day, const DateRange& r)
    {
        const int today = month * 100 + day;
        const int start = r.start_month * 100 + r.start_day;
        const int end = r.end_month * 100 + r.end_day;
        return start <= end ? (today >= start && today <= end) : (today >= start || today <= end);
    }

    void TodayMonthDay(int& month, int& day)
    {
        const std::time_t now = std::time(nullptr);
        std::tm local_tm{};
        localtime_s(&local_tm, &now);
        month = local_tm.tm_mon + 1;
        day = local_tm.tm_mday;
    }

    // Roughly how many days a range covers - only used to compare ranges.
    int RangeLengthDays(const DateRange& r)
    {
        const int start = (r.start_month - 1) * 31 + r.start_day;
        const int end = (r.end_month - 1) * 31 + r.end_day;
        return end >= start ? end - start : end + 12 * 31 - start;
    }

    // Real-world (not in-game) calendar date. When two events are both due
    // (New Year's inside Winter Wonderland, ARKaeology overlapping Summer
    // Bash) the shorter one wins, so the brief special day still happens.
    std::string DetectEventForToday()
    {
        int month = 0, day = 0;
        TodayMonthDay(month, day);
        std::string best;
        int best_length = 1 << 30;
        for (const auto& [token, event_cfg] : g_settings.events)
            for (const auto& range : event_cfg.auto_date_ranges)
                if (DateInRange(month, day, range) && RangeLengthDays(range) < best_length)
                {
                    best = token;
                    best_length = RangeLengthDays(range);
                }
        return best;
    }

    // Shared by every way the event can change - clears whatever the
    // previous event had running first, so its spawns never carry over.
    void SwitchActiveEvent(const std::string& token)
    {
        if (token == g_settings.active_event) return;
        ClearSpawns(); // still the event that is ending
        g_settings.active_event = token;
    }

    float g_auto_detect_timer{0.f};
    constexpr float kAutoDetectCheckIntervalSeconds = 300.f; // 5 real minutes - the date doesn't change any faster
    // Set by StartEventCommand, cleared by StopEventCommand - fully pauses
    // the calendar check so a manual test event isn't reverted mid-test.
    bool g_testing_override{false};
    // Set from a "-event=<token>" / "-ActiveEvent=<token>" launch argument
    // (see ReadActiveEventFromCommandLine). That event runs for as long as
    // the argument stays on the server's startup line, whatever the date -
    // take it off and restart, and the calendar decides again. StopEvent
    // also drops it until the next restart.
    std::string g_launch_arg_active_event;

    void TickAutoDetect(float delta_seconds)
    {
        if (!g_settings.auto_detect_by_date || g_testing_override) return;

        g_auto_detect_timer += delta_seconds;
        if (g_auto_detect_timer < kAutoDetectCheckIntervalSeconds && !g_settings.active_event.empty()) return;
        // On the very first check (active_event still unset) run
        // immediately rather than waiting out the interval, so a fresh
        // server start doesn't sit idle for 5 minutes before picking up
        // whatever event is actually due today.
        g_auto_detect_timer = 0.f;

        std::string detected;
        if (!g_launch_arg_active_event.empty())
            detected = g_launch_arg_active_event; // on the startup line - stays until removed
        else
            detected = DetectEventForToday();

        if (detected == g_settings.active_event) return;

        SwitchActiveEvent(detected);
        Log::GetLog()->info("ArkEventsRevival: active event set to '{}' ({}).",
            detected.empty() ? "(none)" : detected,
            !g_launch_arg_active_event.empty() ? "from -event= on the startup line" : "from today's date");
    }

    // ---- Discord reminder ----

    // Readable event names for the message; unknown tokens show as-is.
    std::string EventDisplayName(const std::string& token)
    {
        static const std::unordered_map<std::string, std::string> names = {
            {"FearEvolved", "Fear Evolved"}, {"TurkeyTrial", "Turkey Trial"}, {"WinterWonderland", "Winter Wonderland"},
            {"vday", "Love Evolved"}, {"easter", "Eggcellent Adventure"}, {"Summer", "Summer Bash"},
            {"birthday", "ARK's Anniversary"}, {"ARKaeology", "ARKaeology"}, {"NewYear", "New Year's"} };
        const auto it = names.find(token);
        return it != names.end() ? it->second : token;
    }

    std::string PluginDir(); // with the config loader, below

    float g_discord_timer{-1.f}; // < 0 = check on the first tick
    std::string g_discord_last_event; // the event running when last looked

    // Posts one message for an event unless this key was sent before (kept in
    // discord_sent.txt, so restarts don't repeat it).
    void SendDiscordOnce(const std::string& key, const std::string& pattern, const std::string& token,
        const std::string& title, int color)
    {
        if (!g_settings.discord_enabled || g_settings.discord_webhook_url.empty() || pattern.empty()) return;
        const std::string sent_path = PluginDir() + "/discord_sent.txt";
        {
            std::ifstream in(sent_path);
            for (std::string line; std::getline(in, line);) if (Trim(line) == key) return;
        }

        std::string text = pattern;
        ReplaceAll(text, "{event}", EventDisplayName(token));
        ReplaceAll(text, "{token}", token);
        std::string body;
        // An embed: coloured card, title, message underneath.
        try
        {
            std::string heading = title;
            ReplaceAll(heading, "{event}", EventDisplayName(token));
            body = json{ {"embeds", json::array({ { {"title", heading}, {"description", text}, {"color", color} } })} }.dump();
        }
        catch (...) { return; }

        const bool queued = API::Requests::Get().CreatePostRequest(g_settings.discord_webhook_url,
            [key](bool ok, std::string result)
            {
                // Discord answers a successful webhook post with "204 No Content".
                const bool delivered = ok || result.rfind("204", 0) == 0;
                if (delivered) { if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: Discord message sent ({}).", key); }
                else Log::GetLog()->warn("ArkEventsRevival: Discord webhook failed ({}): {}", key, result);
            }, body, "application/json");
        if (!queued) return;
        std::ofstream out(sent_path, std::ios::app);
        if (out.is_open()) out << key << "\n";
    }

    std::string DateKey(const std::tm& t)
    {
        return std::to_string(t.tm_year + 1900) + "-" + std::to_string(t.tm_mon + 1) + "-" + std::to_string(t.tm_mday);
    }

    // Events whose "ended" message is waiting for -event= to come off the
    // startup line. Kept in discord_end_pending.txt (one token per line) so a
    // restart - which is how the argument gets removed - does not forget it.
    std::vector<std::string> ReadPendingEnds()
    {
        std::vector<std::string> tokens;
        std::ifstream in(PluginDir() + "/discord_end_pending.txt");
        for (std::string line; std::getline(in, line);)
            if (!Trim(line).empty()) tokens.push_back(Trim(line));
        return tokens;
    }

    void WritePendingEnds(const std::vector<std::string>& tokens)
    {
        std::ofstream out(PluginDir() + "/discord_end_pending.txt", std::ios::trunc);
        for (const std::string& token : tokens) out << token << "\n";
    }

    void AddPendingEnd(const std::string& token)
    {
        std::vector<std::string> tokens = ReadPendingEnds();
        if (std::find(tokens.begin(), tokens.end(), token) != tokens.end()) return;
        tokens.push_back(token);
        WritePendingEnds(tokens);
    }

    void RemovePendingEnd(const std::string& token)
    {
        std::vector<std::string> tokens = ReadPendingEnds();
        tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
        WritePendingEnds(tokens);
    }

    // Every tick (cheap): when a real event switches on - calendar or -event= -
    // tell Discord it has started. StartEvent tests are never announced.
    void TickDiscordStarted()
    {
        if (g_settings.active_event == g_discord_last_event) return;
        g_discord_last_event = g_settings.active_event;
        if (g_settings.active_event.empty() || g_testing_override) return;
        const std::time_t now = std::time(nullptr);
        std::tm today{};
        localtime_s(&today, &now);
        // Keyed by the START of this run of the event (not today), so restarts
        // on later days of the same event don't send it again. A range that
        // started last year (Winter Wonderland in January) counts as last
        // year's run. An event outside its dates (-event=) is keyed by year.
        std::string run = std::to_string(today.tm_year + 1900);
        const auto cfg_it = g_settings.events.find(g_settings.active_event);
        if (cfg_it != g_settings.events.end())
            for (const DateRange& range : cfg_it->second.auto_date_ranges)
                if (DateInRange(today.tm_mon + 1, today.tm_mday, range))
                {
                    const bool began_last_year = (today.tm_mon + 1) * 100 + today.tm_mday < range.start_month * 100 + range.start_day;
                    run = std::to_string(today.tm_year + 1900 - (began_last_year ? 1 : 0)) + "-"
                        + std::to_string(range.start_month) + "-" + std::to_string(range.start_day);
                    break;
                }
        SendDiscordOnce(g_settings.active_event + " started " + run, g_settings.discord_started_message, g_settings.active_event,
            "{event} has started!", 0x2ECC71); // green
    }

    // Once an hour: an event whose AutoDateRanges start TOMORROW gets one
    // Discord message. What has been sent is kept in discord_sent.txt (event
    // and date), so a restart doesn't send it again.
    void TickDiscordReminder(float delta_seconds)
    {
        if (!g_settings.discord_enabled || g_settings.discord_webhook_url.empty()) return;
        if (g_discord_timer >= 0.f)
        {
            g_discord_timer += delta_seconds;
            if (g_discord_timer < 3600.f) return;
        }
        g_discord_timer = 0.f;

        const std::time_t tomorrow_time = std::time(nullptr) + 24 * 60 * 60;
        std::tm tomorrow{};
        localtime_s(&tomorrow, &tomorrow_time);
        const int month = tomorrow.tm_mon + 1, day = tomorrow.tm_mday;

        for (const auto& [token, event_cfg] : g_settings.events)
            for (const DateRange& range : event_cfg.auto_date_ranges)
                if (range.start_month == month && range.start_day == day)
                    SendDiscordOnce(token + " " + DateKey(tomorrow), g_settings.discord_message, token,
                        "{event} starts tomorrow", 0xF39C12); // orange

        // The day before an event's last day: 24 hours' notice that it ends, so
        // an admin who pinned it with -event= can remove it - or leave it to
        // keep the event going.
        for (const auto& [token, event_cfg] : g_settings.events)
            for (const DateRange& range : event_cfg.auto_date_ranges)
                if (range.end_month == month && range.end_day == day)
                    SendDiscordOnce(token + " ending " + DateKey(tomorrow), g_settings.discord_ending_message, token,
                        "{event} ends tomorrow", 0xE67E22); // dark orange

        // The day after an event's last day: it has ended.
        const std::time_t yesterday_time = std::time(nullptr) - 24 * 60 * 60;
        std::tm yesterday{};
        localtime_s(&yesterday, &yesterday_time);
        for (const auto& [token, event_cfg] : g_settings.events)
            for (const DateRange& range : event_cfg.auto_date_ranges)
                if (range.end_month == yesterday.tm_mon + 1 && range.end_day == yesterday.tm_mday)
                {
                    // Pinned by -event= on the startup line: it has NOT ended,
                    // whatever the calendar says. Remember it and say so only
                    // once the argument has been taken off (see below).
                    if (token == g_launch_arg_active_event) AddPendingEnd(token);
                    else SendDiscordOnce(token + " ended " + DateKey(yesterday), g_settings.discord_ended_message, token,
                        "{event} has ended", 0x95A5A6); // grey
                }

        // Events whose end message was held back because -event= kept them
        // running: once the argument is gone, they have ended - tell Discord.
        std::tm now_tm{};
        const std::time_t now_time = std::time(nullptr);
        localtime_s(&now_tm, &now_time);
        const std::string today_key = DateKey(now_tm);
        for (const std::string& token : ReadPendingEnds())
            if (token != g_launch_arg_active_event)
            {
                SendDiscordOnce(token + " ended " + today_key, g_settings.discord_ended_message, token,
                    "{event} has ended", 0x95A5A6);
                RemovePendingEnd(token);
            }
    }

    // Plugins load before the map, so which config_<map>.json to use is only known
    // once the world exists - on the first tick that has one.
    bool g_map_config_pending{true};
    bool ApplyConfig(const std::string& map_name); // below, with the config loader

    // ---- where the boss is, for players who join ----
    // A player who joins the map (or arrives from another map) is told, a little
    // after they are in, where each boss that is UP is right now. No boss, or one
    // that has been killed: nothing is sent. New players are spotted by watching the
    // player list - no extra hook on the game's login.
    std::unordered_set<uint64_t> g_players_known;
    std::unordered_map<uint64_t, float> g_join_wait; // new player -> seconds until their message
    float g_join_scan_timer{0.f};
    bool g_join_primed{false};                       // players already online at load are not "joining"

    void SendBossLocations(UWorld* world, AShooterPlayerController* player)
    {
        const EventConfig* event_cfg = CurrentEventConfig();
        if (!event_cfg || !event_cfg->spawns.enabled || g_reconcile_pending) return;
        const MapSpawns* here = MapSpawnsFor(event_cfg->spawns, CurrentMapNameLower(world));
        if (!here || g_boss_tracked.size() != here->bosses.size()) return;

        for (size_t i = 0; i < here->bosses.size(); ++i)
            for (ActiveBoss& active : g_boss_tracked[i])
            {
                APrimalDinoCharacter* boss = active.boss.Get();
                if (!boss || boss->IsDead() || !active.announced) continue; // not up, or already killed

                std::string text = g_settings.boss_on_map_message;
                ReplaceAll(text, "{boss}", BossName(here->bosses[i], boss));
                ReplaceAll(text, "{lat}", Coords(ActorLocation(boss)));
                ArkApi::GetApiUtils().SendChatMessage(player, ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
                ArkApi::GetApiUtils().SendNotification(player, FLinearColor{ 1.0f, 0.25f, 0.25f, 1.0f }, 1.4f, 10.0f, nullptr,
                    L"{}", *ToFString(text));
                if (g_settings.debug_mode) Log::GetLog()->info("ArkEventsRevival: told a joining player: {}", text);
            }
    }

    void TickJoiners(float delta_seconds, UWorld* world)
    {
        if (g_settings.boss_on_map_message.empty()) return;

        // Players whose wait is over.
        if (!g_join_wait.empty())
        {
            std::vector<uint64_t> due;
            for (auto& entry : g_join_wait)
            {
                entry.second -= delta_seconds;
                if (entry.second <= 0.f) due.push_back(entry.first);
            }
            for (uint64_t id : due)
            {
                g_join_wait.erase(id);
                for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField())
                {
                    APlayerController* pc = weak.Get();
                    if (!pc || !pc->IsA(AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame"))) continue;
                    auto* player = static_cast<AShooterPlayerController*>(pc);
                    if (ArkApi::GetApiUtils().GetSteamIdFromController(player) != id) continue;
                    SendBossLocations(world, player);
                    break;
                }
            }
        }

        // Once a second: who is new on the list?
        g_join_scan_timer += delta_seconds;
        if (g_join_scan_timer < 1.f) return;
        g_join_scan_timer = 0.f;

        std::unordered_set<uint64_t> present;
        for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField())
        {
            APlayerController* pc = weak.Get();
            if (!pc || !pc->IsA(AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame"))) continue;
            const uint64_t id = ArkApi::GetApiUtils().GetSteamIdFromController(static_cast<AShooterPlayerController*>(pc));
            if (id == 0) continue;
            present.insert(id);
            if (g_players_known.insert(id).second && g_join_primed) g_join_wait[id] = g_settings.boss_on_map_delay_seconds;
        }
        g_join_primed = true;
        for (auto it = g_players_known.begin(); it != g_players_known.end();)
        {
            if (present.count(*it)) { ++it; continue; }
            g_join_wait.erase(*it);
            it = g_players_known.erase(it);
        }
    }

    void Tick(float delta_seconds)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (g_map_config_pending)
        {
            if (!world) return;
            g_map_config_pending = false;
            ApplyConfig(CurrentMapNameLower(world));
        }

        if (g_settings.enabled) TickAutoDetect(delta_seconds);
        if (g_settings.enabled) TickDiscordReminder(delta_seconds);
        if (g_settings.enabled) TickDiscordStarted();
        PinActiveEventFields();

        if (!world) return;

        if (g_reconcile_pending)
        {
            // After a fresh load, give the world a moment to finish coming
            // up before looking for last run's creatures.
            g_reconcile_delay -= delta_seconds;
            if (g_reconcile_delay > 0.f) return;
            Reconcile(world);
        }

        if (!g_settings.enabled) return;
        TickSpawns(delta_seconds, world, CurrentEventConfig());
        TickJoiners(delta_seconds, world);
    }

    // ---- config parsing ----

    // The region key as stored on entries: "Anywhere" (or blank) = no filter.
    std::string RegionKey(const std::string& key)
    {
        const std::string region = NormaliseTag(key);
        return region.empty() ? "anywhere" : region;
    }

    // Refills trigger at MinCount, so it must sit below MaxCount or a refill
    // would never have room to spawn anything. MaxCount 0 switches the entry
    // off - returns false.
    bool ClampCounts(int& min_count, int& max_count)
    {
        if (max_count <= 0) return false;
        min_count = (std::max)(0, min_count);
        if (min_count >= max_count) min_count = max_count - 1;
        return true;
    }

    std::vector<CrateItem> ParseItems(const json& node)
    {
        std::vector<CrateItem> items;
        if (!node.contains("Items") || !node["Items"].is_array()) return items;
        for (const auto& item_node : node["Items"])
        {
            if (!item_node.is_object()) continue;
            CrateItem item;
            item.class_path = item_node.value("Class", std::string{});
            item.min_quantity = (std::max)(1, item_node.value("MinQuantity", item.min_quantity));
            item.max_quantity = (std::max)(item.min_quantity, item_node.value("MaxQuantity", item.max_quantity));
            item.chance = item_node.value("Chance", item.chance);
            if (!item.class_path.empty()) items.push_back(std::move(item));
        }
        return items;
    }

    // A region key "SpawnLike:<class name part>" (e.g. "SpawnLike:Rex_Character_BP")
    // is a group whose dinos spawn only where the game's own creatures of that
    // class spawn - so they stay on the maps those creatures are native to.
    constexpr const char* kSpawnLikePrefix = "spawnlike:";

    // The event's GameWeight while its regions are parsed.
    float g_parse_game_weight{0.25f};

    void ParseRegionGroup(const json& group, const std::string& region_key, MapSpawns& here)
    {
        const bool like_group = region_key.compare(0, std::strlen(kSpawnLikePrefix), kSpawnLikePrefix) == 0;
        const std::string group_like = like_group ? NormaliseTag(region_key.substr(std::strlen(kSpawnLikePrefix))) : std::string{};

        if (!like_group && IsCaveRegion(region_key))
            Log::GetLog()->warn("ArkEventsRevival: region '{}' is a cave - caves are never used, so nothing listed under it will spawn.", region_key);

        const std::vector<std::string> regions = region_key == "anywhere" || like_group
            ? std::vector<std::string>{} : std::vector<std::string>{ region_key };

        if (group.contains("Dinos") && group["Dinos"].is_array())
            for (const auto& node : group["Dinos"])
            {
                if (!node.is_object()) continue;
                WildSpawnDino dino;
                dino.class_path = node.value("Class", std::string{});
                dino.regions = regions;
                dino.min_count = node.value("MinCount", dino.min_count);
                dino.max_count = node.value("MaxCount", dino.max_count);
                dino.respawn_to = (std::max)(0, node.value("RespawnTo", 0));
                dino.respawn_delay_seconds = (std::max)(0.f, node.value("RespawnDelayMinutes", 0.f)) * 60.f;
                dino.game_spawn = true; // the only way wild event creatures are spawned now
                dino.game_weight = std::clamp(node.value("GameWeight", g_parse_game_weight), 0.001f, 1.f);
                if (node.contains("SpawnLike") && node["SpawnLike"].is_string())
                {
                    const std::string like = NormaliseTag(node["SpawnLike"].get<std::string>());
                    if (!like.empty()) dino.spawn_like.push_back(like);
                }
                if (dino.spawn_like.empty() && !group_like.empty()) dino.spawn_like.push_back(group_like);
                if (node.contains("RequireNative") && node["RequireNative"].is_string())
                {
                    const std::string native = NormaliseTag(node["RequireNative"].get<std::string>());
                    if (!native.empty()) dino.require_native.push_back(native);
                }
                dino.key = "dino|" + region_key + "|" + dino.class_path;
                if (!dino.class_path.empty() && ClampCounts(dino.min_count, dino.max_count)) here.dinos.push_back(std::move(dino));
            }

        if (group.contains("Drops") && group["Drops"].is_array())
            for (const auto& node : group["Drops"])
            {
                if (!node.is_object()) continue;
                RegionDrop drop;
                drop.crate_path = node.value("Crate", std::string{});
                drop.fallback_crate_path = node.value("FallbackCrate", std::string{});
                drop.regions = regions;
                drop.min_count = node.value("MinCount", drop.min_count);
                drop.max_count = node.value("MaxCount", drop.max_count);
                drop.include_crate_loot = node.value("IncludeCrateLoot", drop.include_crate_loot);
                drop.respawn_to = (std::max)(0, node.value("RespawnTo", 0));
                drop.near_event_dinos = node.value("NearEventDinos", false);
                drop.items_per_crate = (std::max)(0, node.value("ItemsPerCrate", 0));
                drop.items = ParseItems(node);
                drop.key = "drop|" + region_key + "|" + drop.crate_path;
                if (!drop.crate_path.empty() && ClampCounts(drop.min_count, drop.max_count)) here.drops.push_back(std::move(drop));
            }

        if (group.contains("Bosses") && group["Bosses"].is_array())
            for (const auto& node : group["Bosses"])
            {
                if (!node.is_object()) continue;
                RegionBoss boss;
                boss.class_path = node.value("Class", std::string{});
                boss.name = Trim(node.value("Name", std::string{}));
                boss.regions = regions;
                boss.amount = node.value("Amount", boss.amount);
                boss.respawn_seconds = (std::max)(1.0f, node.value("RespawnMinutes", boss.respawn_seconds / 60.f)) * 60.f;
                boss.night_only = node.value("NightOnly", boss.night_only);
                if (node.contains("SpawnAtHours") && node["SpawnAtHours"].is_array())
                    for (const auto& h : node["SpawnAtHours"]) if (h.is_number_integer()) boss.spawn_hours.push_back(h.get<int>() % 24);
                boss.level = (std::max)(1, node.value("Level", boss.level));
                boss.minion_level = (std::max)(1, node.value("MinionLevel", boss.minion_level));
                boss.minions_per_wave = (std::max)(0, node.value("MaxMinions", node.value("MinionsPerWave", boss.minions_per_wave)));
                boss.wave_count = (std::max)(0, node.value("WaveCount", boss.wave_count));
                boss.random_minions = node.value("RandomMinions", boss.random_minions);
                boss.wave_interval_seconds = (std::max)(1.0f, node.value("MinionRespawnSeconds", node.value("WaveIntervalSeconds", boss.wave_interval_seconds)));
                boss.clear_radius = node.value("ClearRadius", boss.clear_radius);
                // Distances in foundations (1 = 300 units); LeashRadius in units still read.
                boss.leash_radius = (std::max)(0.f, node.value("LeashRadius", boss.leash_radius));
                if (node.contains("LeashFoundations")) boss.leash_radius = (std::max)(0.f, node.value("LeashFoundations", 40.f)) * 300.f;
                if (node.contains("ArenaFoundations")) boss.arena_radius = (std::max)(0.f, node.value("ArenaFoundations", 30.f)) * 300.f;
                boss.shield_dome = node.value("ShieldDome", boss.shield_dome);
                boss.heal_after_seconds = (std::max)(0.f, node.value("HealAfterMinutesNoDamage", boss.heal_after_seconds / 60.f)) * 60.f;
                boss.return_countdown_seconds = (std::max)(1.f, node.value("ReturnCountdownSeconds", boss.return_countdown_seconds));
                if (node.contains("DomeColor") && node["DomeColor"].is_array() && node["DomeColor"].size() >= 3)
                    boss.dome_color = FLinearColor{ node["DomeColor"][0].get<float>(), node["DomeColor"][1].get<float>(),
                                                    node["DomeColor"][2].get<float>(), 1.0f };
                boss.key = "boss|" + region_key + "|" + boss.class_path;
                if (node.contains("MinionClasses") && node["MinionClasses"].is_array())
                    for (const auto& minion : node["MinionClasses"])
                        if (minion.is_string()) boss.minion_classes.push_back(minion.get<std::string>());
                if (node.contains("DefeatCrates") && node["DefeatCrates"].is_object())
                {
                    const json& loot_node = node["DefeatCrates"];
                    DefeatCrates& loot = boss.defeat_crates;
                    loot.crate_path = loot_node.value("Crate", std::string{});
                    loot.fallback_crate_path = loot_node.value("FallbackCrate", std::string{});
                    loot.count = (std::max)(0, loot_node.value("Count", loot.count));
                    loot.items_per_crate = (std::max)(1, loot_node.value("ItemsPerCrate", loot.items_per_crate));
                    loot.include_crate_loot = loot_node.value("IncludeCrateLoot", loot.include_crate_loot);
                    loot.ring_radius = (std::max)(0.f, loot_node.value("RingRadius", loot.ring_radius));
                    loot.around_arena = loot_node.value("AroundArena", loot.around_arena);
                    loot.items = ParseItems(loot_node);
                }
                if (!boss.class_path.empty() && boss.amount > 0) here.bosses.push_back(std::move(boss));
            }
    }

    // map_name is the map this server runs (see NormaliseMapName). "Regions"
    // are that map's spawns - the per-map config file holds only its own
    // map. An older all-maps "Maps": { "<map>": { regions } } block is still
    // read too.
    void ParseSpawns(const json& node, SpawnsConfig& cfg, const std::string& map_name)
    {
        if (!node.is_object()) return;
        cfg.enabled = node.value("Enabled", false);
        cfg.interval_seconds = (std::max)(10.0f, node.value("CheckIntervalSeconds", cfg.interval_seconds));
        cfg.min_level = (std::max)(1, node.value("MinLevel", cfg.min_level));
        cfg.max_level = (std::max)(cfg.min_level, node.value("MaxLevel", cfg.max_level));
        cfg.game_weight = std::clamp(node.value("GameWeight", 0.25f), 0.001f, 1.f);
        g_parse_game_weight = cfg.game_weight;
        cfg.player_buffs.clear();
        if (node.contains("PlayerBuffs") && node["PlayerBuffs"].is_array())
            for (const auto& b : node["PlayerBuffs"]) if (b.is_string()) cfg.player_buffs.push_back(b.get<std::string>());
        if (node.contains("Mounds") && node["Mounds"].is_object())
        {
            const json& mn = node["Mounds"];
            PlayerMounds& m = cfg.mounds;
            m.class_path = mn.value("Class", std::string{});
            m.per_player = (std::max)(0, mn.value("PerPlayer", m.per_player));
            m.min_distance = (std::max)(0.f, mn.value("MinDistance", m.min_distance));
            m.max_distance = (std::max)(m.min_distance + 1.f, mn.value("MaxDistance", m.max_distance));
            m.min_items = (std::max)(1, mn.value("MinItems", m.min_items));
            m.max_items = (std::max)(m.min_items, mn.value("MaxItems", m.max_items));
            m.items = ParseItems(mn);
        }
        if (node.contains("KillDrops") && node["KillDrops"].is_object())
        {
            const json& k = node["KillDrops"];
            KillDrops& drops = cfg.kill_drops;
            if (k.contains("Crates") && k["Crates"].is_array())
                for (const auto& c : k["Crates"]) if (c.is_string()) drops.crate_paths.push_back(c.get<std::string>());
            drops.fallback_crate_path = k.value("FallbackCrate", std::string{});
            drops.min_items = (std::max)(1, k.value("MinItems", drops.min_items));
            drops.max_items = (std::max)(drops.min_items, k.value("MaxItems", drops.max_items));
            drops.chance = k.value("Chance", drops.chance);
            drops.every_kills = (std::max)(0, k.value("EveryKills", 0));
            drops.items = ParseItems(k);
        }

        // "<DinoSpawnEntries region>": { "Dinos": [...], "Drops": [...],
        // "Bosses": [...] } - the region key "Anywhere" means any spawn point
        // on the map outside water and caves. The same thing under two
        // regions is two separate entries, each with its own counts.
        auto parse_regions = [](const json& regions, MapSpawns& here)
        {
            for (auto region_it = regions.begin(); region_it != regions.end(); ++region_it)
                if (region_it.value().is_object())
                    ParseRegionGroup(region_it.value(), RegionKey(region_it.key()), here);
        };

        cfg.by_map.clear();
        if (node.contains("Maps") && node["Maps"].is_object())
            for (auto map_it = node["Maps"].begin(); map_it != node["Maps"].end(); ++map_it)
                if (map_it.value().is_object())
                    parse_regions(map_it.value(), cfg.by_map[NormaliseMapName(map_it.key())]);

        if (!map_name.empty() && node.contains("Regions") && node["Regions"].is_object())
        {
            MapSpawns& here = cfg.by_map[map_name];
            here = MapSpawns{};
            parse_regions(node["Regions"], here);
        }
    }

    void ParseEventConfig(const json& node, EventConfig& event_cfg, const std::string& map_name)
    {
        if (!node.is_object()) return;

        if (node.contains("Spawns")) ParseSpawns(node["Spawns"], event_cfg.spawns, map_name);

        event_cfg.auto_date_ranges.clear();
        if (node.contains("AutoDateRanges") && node["AutoDateRanges"].is_array())
            for (const auto& range_node : node["AutoDateRanges"])
            {
                if (!range_node.is_object()) continue;
                DateRange range;
                range.start_month = range_node.value("StartMonth", 1);
                range.start_day = range_node.value("StartDay", 1);
                range.end_month = range_node.value("EndMonth", 1);
                range.end_day = range_node.value("EndDay", 1);
                event_cfg.auto_date_ranges.push_back(range);
            }
    }

    std::string PluginDir() { return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" + PluginName(); }

    // Which file the running config came from - logged when it changes.
    std::string g_config_file_used;

    // Each map has its own complete config: config_TheIsland.json,
    // config_Gen2.json, config_ScorchedEarth.json... (case and a "_P" in the
    // name don't matter), so one plugin folder copied to every server in the
    // cluster gives each server its own map's setup. Returns "" if this map
    // has none.
    std::string FindMapConfigFile(const std::string& map_name)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (map_name.empty()) return {};
        for (const auto& entry : fs::directory_iterator(fs::path(PluginDir()), ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = entry.path().filename().string();
            const std::string lower = ToLower(name);
            if (lower.size() <= 12 || lower.compare(0, 7, "config_") != 0 || lower.compare(lower.size() - 5, 5, ".json") != 0) continue;
            if (NormaliseMapName(lower.substr(7, lower.size() - 12)) == map_name) return name;
        }
        return {};
    }

    // Loads this map's config_<Map>.json, or config.json when the map has
    // none. Parsed into a separate Settings and only swapped in once the
    // whole file has read cleanly - a typo half way through leaves the
    // running config exactly as it was. Only called once the world exists
    // (plugins load before the map) - see g_map_config_pending.
    bool LoadConfig(const std::string& map_name)
    {
        // A map with no config of its own uses config_TheIsland.json (then config.json). Regions are
        // read for whatever map this is, and SpawnLike / RequireNative keep to what lives there.
        std::string map_file = FindMapConfigFile(map_name);
        const bool own_config = !map_file.empty();
        if (!own_config) map_file = FindMapConfigFile(NormaliseMapName("TheIsland"));
        const std::string file_name = map_file.empty() ? "config.json" : map_file;
        try
        {
            std::ifstream file(PluginDir() + "/" + file_name);
            if (!file.is_open())
            {
                Log::GetLog()->error("ArkEventsRevival: map '{}' detected, but there is no config_<map>.json for it and no config.json.", map_name);
                return false;
            }
            json root; file >> root;
            const auto& cfg = root.at("ArkEventsRevival");

            Settings parsed;
            parsed.enabled = cfg.value("Enabled", true);
            parsed.debug_mode = cfg.value("DebugMode", false);
            parsed.auto_detect_by_date = cfg.value("AutoDetectActiveEvent", false);
            parsed.structure_clearance = (std::max)(0.f, cfg.value("KeepAwayFromStructures", parsed.structure_clearance));
            parsed.map_edge_margin = (std::max)(0.f, cfg.value("KeepAwayFromMapEdge", parsed.map_edge_margin));
            parsed.spawn_clearance = cfg.value("CheckSpawnClearance", parsed.spawn_clearance);
            parsed.land_only = cfg.value("SpawnOnLandOnly", parsed.land_only);
            parsed.other_region_chance = std::clamp(cfg.value("OtherRegionChance", parsed.other_region_chance), 0.f, 1.f);
            parsed.wyvern_max_level = (std::max)(5, cfg.value("WyvernMaxLevel", parsed.wyvern_max_level));
            parsed.spawn_spacing = (std::max)(0.f, cfg.value("SpawnSpacing", parsed.spawn_spacing));

            if (cfg.contains("Discord") && cfg["Discord"].is_object())
            {
                const json& discord = cfg["Discord"];
                parsed.discord_enabled = discord.value("Enabled", false);
                parsed.discord_webhook_url = Trim(discord.value("WebhookUrl", std::string{}));
                parsed.discord_message = discord.value("EventTomorrowMessage", parsed.discord_message);
                parsed.discord_started_message = discord.value("EventStartedMessage", parsed.discord_started_message);
                parsed.discord_ended_message = discord.value("EventEndedMessage", parsed.discord_ended_message);
                parsed.discord_ending_message = discord.value("EventEndingMessage", parsed.discord_ending_message);
            }

            if (cfg.contains("Announcements") && cfg["Announcements"].is_object())
            {
                const json& messages = cfg["Announcements"];
                parsed.chat_sender = messages.value("ChatSender", parsed.chat_sender);
                parsed.boss_spawned_message = messages.value("BossSpawned", parsed.boss_spawned_message);
                parsed.boss_defeated_message = messages.value("BossDefeated", parsed.boss_defeated_message);
                parsed.boss_on_map_message = messages.value("BossOnMap", parsed.boss_on_map_message);
                parsed.boss_on_map_delay_seconds = (std::max)(1.f, messages.value("BossOnMapDelaySeconds", parsed.boss_on_map_delay_seconds));
                parsed.boss_leaving_message = messages.value("BossLeavingArena", parsed.boss_leaving_message);
                parsed.player_entered_message = messages.value("PlayerEnteredArena", parsed.player_entered_message);
                parsed.player_left_message = messages.value("PlayerLeftArena", parsed.player_left_message);
                parsed.outside_arena_message = messages.value("OutsideArenaNoDamage", parsed.outside_arena_message);
            }

            if (cfg.contains("Events") && cfg["Events"].is_object())
                for (auto it = cfg["Events"].begin(); it != cfg["Events"].end(); ++it)
                {
                    EventConfig event_cfg;
                    ParseEventConfig(it.value(), event_cfg, map_name);
                    parsed.events[it.key()] = std::move(event_cfg);
                }

            // AutoDetectActiveEvent fully overrides ActiveEvent - left empty
            // here so the next tick checks the calendar straight away (see
            // TickAutoDetect).
            if (!parsed.auto_detect_by_date)
                parsed.active_event = ResolveEventToken(Trim(cfg.value("ActiveEvent", std::string{})), parsed.events);

            if (!own_config) parsed.discord_enabled = false; // another map's config: no Discord posts
            if (file_name != g_config_file_used)
            {
                if (!own_config)
                    Log::GetLog()->warn("ArkEventsRevival: map '{}' detected - no config_{}.json, using {}.", map_name, map_name, file_name);
                else
                    Log::GetLog()->info("ArkEventsRevival: map '{}' detected - using {}.", map_name, map_file);
            }
            g_config_file_used = file_name;

            g_settings = std::move(parsed);
            g_auto_detect_timer = 0.f;
            return true;
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->error("ArkEventsRevival: error in {}: {} - config not changed.", file_name, ex.what());
            return false;
        }
    }

    // ---- launch argument ----

    // "-event=<token>" or "-ActiveEvent=<token>" (any case) on the server's
    // startup line. A plain process argument (like Wildcard's own documented
    // -activeevent= flag), not a UWorld/AShooterGameMode
    // URL option - GetCommandLineA is the standard, reliable way for any
    // native DLL to read the exe's actual launch command line, independent
    // of ArkApi/UE4 accessors. Lets an admin bake the event choice into
    // their server's own start script/launch parameters, surviving
    // restarts without touching config.json.
    std::string ReadActiveEventFromCommandLine()
    {
        const char* cmd_line = GetCommandLineA();
        if (!cmd_line) return {};

        const std::string cmd = cmd_line;
        const std::string cmd_lower = ToLower(cmd);
        // The dash must start an argument, so "-event=" is never found
        // inside some other flag.
        size_t pos = std::string::npos, needle_size = 0;
        for (const std::string needle : { "-activeevent=", "-event=" })
        {
            for (size_t at = cmd_lower.find(needle); at != std::string::npos; at = cmd_lower.find(needle, at + 1))
                if (at == 0 || cmd_lower[at - 1] == ' ' || cmd_lower[at - 1] == '\t' || cmd_lower[at - 1] == '"')
                {
                    pos = at;
                    needle_size = needle.size();
                    break;
                }
            if (pos != std::string::npos) break;
        }
        if (pos == std::string::npos) return {};

        const auto value_start = pos + needle_size;
        auto value_end = cmd.find_first_of(" \t\r\n?\"", value_start);
        if (value_end == std::string::npos) value_end = cmd.size();
        return Trim(cmd.substr(value_start, value_end - value_start));
    }

    // Re-checks the actual, current process command line and applies (or
    // clears) the launch-arg override accordingly - called from both
    // Plugin_Init and ReloadCommand so the two paths behave identically.
    void ApplyLaunchArgActiveEvent()
    {
        if (g_testing_override) return; // an explicit StartEvent test in progress takes priority

        const std::string launch_event = ResolveEventToken(ReadActiveEventFromCommandLine(), g_settings.events);
        if (launch_event.empty())
        {
            // Not present this run - end the pin (if any) and let the
            // calendar (if AutoDetectActiveEvent is on) decide normally.
            if (!g_launch_arg_active_event.empty())
                Log::GetLog()->info("ArkEventsRevival: -event= is no longer on the startup line - the calendar picks the event again.");
            g_launch_arg_active_event.clear();
            return;
        }

        g_launch_arg_active_event = launch_event;
        g_settings.active_event = launch_event;
        Log::GetLog()->info(
            "ArkEventsRevival: event '{}' is on the server startup line - running it until it is removed from there (then the calendar picks again).",
            launch_event);
    }

    // Loads this map's config file and re-applies what sits
    // on top of them: a running StartEvent test is kept, and the launch
    // argument is checked again. Shared by the first tick and Reload.
    bool ApplyConfig(const std::string& map_name)
    {
        const std::string testing_event = g_settings.active_event;
        if (!LoadConfig(map_name)) return false;
        if (g_testing_override) g_settings.active_event = ResolveEventToken(testing_event, g_settings.events); // a StartEvent test survives
        ApplyLaunchArgActiveEvent();
        return true;
    }

    // ---- console commands ----

    // ArkApi hands a console command the WHOLE line it was typed as
    // ("ArkEventsRevival.StartEvent FearEvolved") - the argument is
    // everything after the first space.
    std::string CommandArgument(FString* message)
    {
        if (!message) return {};
        const std::string line = Trim(ToStdString(*message));
        const auto space = line.find_first_of(" \t");
        return space == std::string::npos ? std::string{} : Trim(line.substr(space + 1));
    }

    // Log line, plus a chat line to the admin who ran it when there is one
    // (an RCON command has no player).
    void Reply(APlayerController* player, const std::string& text)
    {
        Log::GetLog()->info("ArkEventsRevival: {}", text);
        if (player && player->IsA(AShooterPlayerController::GetPrivateStaticClass(L"/Script/ShooterGame")))
            ArkApi::GetApiUtils().SendChatMessage(static_cast<AShooterPlayerController*>(player),
                ToFString(g_settings.chat_sender), L"{}", *ToFString(text));
    }

    void ReloadCommand(APlayerController*, FString*, bool)
    {
        // Nothing the event spawned is removed on a reload: it is written out
        // and read back under the new config on the next tick (see
        // Reconcile), which keeps what still fits and removes the rest.
        SaveSpawned();
        ResetTracking();

        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (world) ApplyConfig(CurrentMapNameLower(world));
        else g_map_config_pending = true;
        g_reconcile_pending = true;
        g_reconcile_delay = 0.f;
        g_cave_volumes_built = false;

        Log::GetLog()->info("ArkEventsRevival: reloaded - ActiveEvent='{}', {} event(s) configured.",
            g_settings.active_event, static_cast<int>(g_settings.events.size()));
    }

    // Starts a specific event for testing, regardless of what the real
    // calendar date says. Pauses AutoDetectActiveEvent (see
    // g_testing_override) so it doesn't get silently reverted a few
    // minutes into a test session - StopEvent resumes the calendar.
    void StartEventCommand(APlayerController*, FString* message, bool)
    {
        const std::string token = ResolveEventToken(CommandArgument(message), g_settings.events);
        if (token.empty())
        {
            Log::GetLog()->warn("ArkEventsRevival: usage: ArkEventsRevival.StartEvent <token> - e.g. FearEvolved, TurkeyTrial, WinterWonderland, vday, easter, Summer, birthday, ARKaeology.");
            return;
        }

        g_testing_override = true;
        SwitchActiveEvent(token);

        const bool has_custom_config = g_settings.events.count(token) > 0;
        Log::GetLog()->info(
            "ArkEventsRevival: TEST started for event '{}'.{} Calendar auto-detection is paused - run ArkEventsRevival.StopEvent when done testing.",
            token, has_custom_config ? "" : " No matching Events entry in config - only the native cosmetic/recipe unlock will apply.");
    }

    // Ends a StartEvent test and resumes normal operation - clears the
    // active event and, if AutoDetectActiveEvent is on, lets the calendar
    // take back over on the next check instead of staying stuck on
    // whatever was being tested.
    void StopEventCommand(APlayerController*, FString*, bool)
    {
        g_testing_override = false;
        // An unconditional manual stop - also drops the launch-arg pin
        // (if any) so it doesn't just get silently re-applied on the very
        // next calendar check. If -ActiveEvent= is still on the launch
        // command line, it comes back on the next actual server restart,
        // same as any other launch argument would.
        g_launch_arg_active_event.clear();
        SwitchActiveEvent(std::string{});
        g_auto_detect_timer = 0.f; // re-check the calendar right away rather than waiting out the interval

        Log::GetLog()->info("ArkEventsRevival: stopped, active event cleared.{}",
            g_settings.auto_detect_by_date ? " Calendar auto-detection resumed." : "");
    }

    // Spawns every boss this map's list has for the active event right now,
    // topping each entry up to its Amount - for testing without waiting out
    // RespawnMinutes or for night. Announced like any other boss.
    void SpawnBossNowCommand(APlayerController* player, FString*, bool)
    {
        const EventConfig* event_cfg = g_settings.enabled ? CurrentEventConfig() : nullptr; // off = nothing would tick it
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const std::string map_name = CurrentMapNameLower(world);
        const MapSpawns* here = event_cfg ? MapSpawnsFor(event_cfg->spawns, map_name) : nullptr;
        if (!world || !here || here->bosses.empty() || g_reconcile_pending)
        {
            Reply(player, "No Bosses for the active event ('" + g_settings.active_event + "') on map '" + map_name + "'"
                + (g_reconcile_pending ? " yet - the plugin is still starting up, try again in a few seconds." : "."));
            return;
        }

        if (g_spawns_event != g_settings.active_event || g_boss_tracked.size() != here->bosses.size())
            StartTracking(event_cfg, *here);

        // One boss at a time: the one whose turn it is, if none is up.
        int spawned = 0;
        if (!AnyBossAlive())
        {
            const size_t i = g_boss_turn % here->bosses.size();
            spawned = SpawnBosses(world, i, here->bosses[i], 1);
            if (spawned > 0) ++g_boss_turn;
            g_boss_timers[i] = 0.f;
        }
        SaveSpawned();
        Reply(player, "SpawnBossNow - " + std::to_string(spawned) + " boss(es) spawned"
            + (spawned == 0 ? " (all already up, or no usable spawn point yet)." : " - announced in a few seconds."));
    }

    // Prints this map's name and every spawn region seen on it since the
    // plugin loaded, in the exact form to paste into Spawns.Maps, with how
    // many of its points can be used. Filled as wild creatures spawn - a
    // server started with -forcerespawndinos has nearly all of them within a
    // minute or two. Up to 20 points per region are checked for water and
    // caves for the count, so a first run takes a moment.
    void ListRegionsCommand(APlayerController* player, FString*, bool)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world) return;
        if (!g_cave_volumes_built) BuildCaveVolumes(world);

        std::vector<std::pair<std::string, std::string>> regions; // game's name, matching form
        for (const auto& seen : g_seen_regions) regions.emplace_back(seen.second, seen.first);
        std::sort(regions.begin(), regions.end());

        Reply(player, "Map '" + CurrentMapNameLower(world) + "' - " + std::to_string(regions.size()) + " spawn region(s) seen so far, "
            + std::to_string(g_cave_volumes.size()) + " cave volume(s), "
            + std::to_string(g_pool_by_class.size()) + " creature type(s) seen spawning (for SpawnLike):");
        for (const auto& [name, tag] : regions)
        {
            if (IsCaveRegion(tag)) { Log::GetLog()->info("ArkEventsRevival:   {} - cave, never used", name); continue; }
            auto& points = g_pool_by_region[tag];
            int checked = 0, water = 0, cave = 0;
            for (auto& point : points)
            {
                if (checked >= 20) break;
                ++checked;
                PointUsable(world, point);
                if (point.underwater == 1) ++water;
                else if (point.cave == 1) ++cave;
            }
            Log::GetLog()->info("ArkEventsRevival:   {} - {} point(s); of {} checked: {} usable, {} water, {} cave",
                name, points.size(), checked, checked - water - cave, water, cave);
        }
    }

    // Lists the active event's SpawnLike dinos on this map and whether their
    // ordinary counterpart spawns here - i.e. which event dinos this map gets.
    // Spots are learned as the game spawns its own wildlife, so give a fresh
    // server a few minutes first.
    void ShowSpawnLikeCommand(APlayerController* player, FString*, bool)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const EventConfig* event_cfg = g_settings.enabled ? CurrentEventConfig() : nullptr;
        const MapSpawns* here = event_cfg && world ? MapSpawnsFor(event_cfg->spawns, CurrentMapNameLower(world)) : nullptr;
        if (!here)
        {
            Reply(player, "No active event with spawns on this map ('" + g_settings.active_event + "').");
            return;
        }

        Reply(player, "Event '" + g_settings.active_event + "' on '" + CurrentMapNameLower(world) + "' - "
            + std::to_string(g_pool_by_class.size()) + " creature type(s) seen spawning so far:");
        for (const WildSpawnDino& entry : here->dinos)
        {
            if (entry.spawn_like.empty()) continue;
            size_t spots = 0;
            for (const auto& [class_tag, points] : g_pool_by_class)
                for (const auto& wanted : entry.spawn_like)
                    if (class_tag.find(wanted) != std::string::npos) { spots += points.size(); break; }
            const auto slash = entry.class_path.find_last_of('/');
            const std::string short_name = slash == std::string::npos ? entry.class_path : entry.class_path.substr(slash + 1);
            Log::GetLog()->info("ArkEventsRevival:   {} like '{}' - {}", short_name, entry.spawn_like.front(),
                spots > 0 ? std::to_string(spots) + " native spot(s) - spawns here" : std::string("none seen - not spawned on this map"));
        }
    }

    // Test tool: resizes every live boss dome with this size value (e.g.
    // ArkEventsRevival.DomeSize 50) and logs what the shield reports. -1 =
    // back to the automatic value. Used on new domes too, until a reload.
    void DomeSizeCommand(APlayerController* player, FString* message, bool)
    {
        const std::string arg = CommandArgument(message);
        try { g_dome_size_value = arg.empty() ? -1 : std::stoi(arg); }
        catch (...) { Reply(player, "Usage: ArkEventsRevival.DomeSize <number>  (-1 = automatic)"); return; }

        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        const MapSpawns* here = ActiveMapSpawns(world);
        int resized = 0;
        if (here)
            for (size_t i = 0; i < g_boss_tracked.size() && i < here->bosses.size(); ++i)
                for (ActiveBoss& active : g_boss_tracked[i])
                    if (AActor* dome = active.dome.Get()) { ResizeDome(dome, here->bosses[i].arena_radius, g_dome_size_value); ++resized; }
        Reply(player, "DomeSize " + std::to_string(g_dome_size_value) + " - " + std::to_string(resized)
            + " dome(s) resized. The server log shows the radius the shield reports.");
    }

    // Stand somewhere and run this to see what the plugin makes of that
    // spot - the way to confirm cave detection on each map.
    void CaveCheckCommand(APlayerController* player, FString*, bool)
    {
        UWorld* world = ArkApi::GetApiUtils().GetWorld();
        if (!world || !player)
        {
            Log::GetLog()->warn("ArkEventsRevival: CaveCheck needs to be run in-game by an admin, standing where to check.");
            return;
        }
        if (!g_cave_volumes_built) BuildCaveVolumes(world);

        const FVector at = ArkApi::GetApiUtils().GetPosition(player);
        const bool water = PointIsUnderwater(world, at);
        const bool cave = PointIsInCave(world, at);
        Reply(player, "At " + Coords(at) + ": " + (water ? "in water" : "not in water") + ", "
            + (cave ? "IN A CAVE" : "not in a cave") + " (" + std::to_string(g_cave_volumes.size()) + " cave volume(s) on this map) - "
            + (water || cave ? "nothing would spawn here." : "event spawns could use this spot."));
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Init()
{
    Log::Get().Init("ArkEventsRevival");
    // Which config to use depends on the map, and at server start the map
    // isn't loaded yet - so the config is read on the first tick (see
    // g_map_config_pending), or straight away on a plugins.load into a
    // running server. Until then nothing runs.
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (world && !ApplyConfig(CurrentMapNameLower(world)))
    {
        Log::GetLog()->error("ArkEventsRevival was not loaded: its config file is missing or invalid.");
        return;
    }
    g_map_config_pending = world == nullptr;
    g_reconcile_pending = true;
    g_reconcile_delay = 20.f;

    ArkApi::GetHooks().SetHook("ANPCZoneManager.SpawnFromEntries", &HookSpawnFromEntries, &ANPCZoneManager_SpawnFromEntries_original);
    ArkApi::GetHooks().SetHook("APrimalDinoCharacter.TakeDamage", &HookDinoTakeDamage, &APrimalDinoCharacter_TakeDamage_original);
    ArkApi::GetCommands().AddOnTickCallback("ArkEventsRevival.Tick", &Tick);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.Reload", &ReloadCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.StartEvent", &StartEventCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.StopEvent", &StopEventCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.SpawnBossNow", &SpawnBossNowCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.ListRegions", &ListRegionsCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.CaveCheck", &CaveCheckCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.DomeSize", &DomeSizeCommand);
    ArkApi::GetCommands().AddConsoleCommand("ArkEventsRevival.ShowSpawnLike", &ShowSpawnLikeCommand);

    Log::GetLog()->info("ArkEventsRevival build {} {}.", __DATE__, __TIME__); // which DLL is really running
    Log::GetLog()->info("ArkEventsRevival loaded - ActiveEvent='{}', {} event(s) configured.",
        g_settings.active_event, static_cast<int>(g_settings.events.size()));
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload()
{
    // Everything spawned stays in the world and is written out, so the next
    // load (a plugin update, say) picks it straight back up.
    SaveSpawned();
    RemovePlayerBuffs();
    if (g_event_pinned) ClearPinnedEvent();

    ArkApi::GetCommands().RemoveOnTickCallback("ArkEventsRevival.Tick");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.Reload");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.StartEvent");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.StopEvent");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.SpawnBossNow");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.ListRegions");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.CaveCheck");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.DomeSize");
    ArkApi::GetCommands().RemoveConsoleCommand("ArkEventsRevival.ShowSpawnLike");
    ArkApi::GetHooks().DisableHook("ANPCZoneManager.SpawnFromEntries", &HookSpawnFromEntries);
    ArkApi::GetHooks().DisableHook("APrimalDinoCharacter.TakeDamage", &HookDinoTakeDamage);
}
