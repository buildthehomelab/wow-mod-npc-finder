/*
 * mod-npc-finder
 *
 * A town guide for every player. Cast Town Guide from the spellbook (or type .find) and a menu
 * opens: search by name, title or zone, or pick a category such as your class trainer, a
 * profession trainer, a flight master or the bank. Pick a result and it's flagged on your map
 * and minimap.
 *
 * Everything comes from the server's own spawn data, read at startup, so custom NPCs are found
 * like any other. Only friendly service NPCs are listed: nothing hostile to you, and nothing on
 * a continent mod-individual-progression hasn't opened for you yet.
 *
 * Commands:
 *   .find            open the menu
 *   .find <words>    search straight away; every word must match the name, title or zone
 *
 * Released under the MIT License.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "GameObjectData.h"
#include "GossipDef.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Trainer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // The menu is a gossip window whose source is the player. The core only accepts a pick from
    // it when the menu id matches the one it was sent with.
    constexpr uint32 GOSSIP_MENU_TOWN_GUIDE = 9500500;

    // npc_text rows from data/sql/db-world: the text at the top of each kind of page.
    constexpr uint32 NPC_TEXT_MAIN = 9500500;
    constexpr uint32 NPC_TEXT_RESULTS = 9500501;
    constexpr uint32 NPC_TEXT_NO_RESULTS = 9500502;

    // What a map marker looks like: the red flag the city guards use.
    constexpr uint32 POI_FLAGS = 99;
    constexpr uint32 POI_ICON_RED_FLAG = 7;

    constexpr uint32 MAP_OUTLAND = 530;
    constexpr uint32 MAP_NORTHREND = 571;
    constexpr uint32 MAP_EBON_HOLD = 609;

    constexpr uint32 ZONE_ISLE_OF_QUEL_DANAS = 4080;

    // Map 530 also holds the blood elf and draenei starting zones, which are open from level 1.
    constexpr std::array<uint32, 6> ZONES_OPEN_ON_OUTLAND_MAP = {
        3430, // Eversong Woods
        3433, // Ghostlands
        3487, // Silvermoon City
        3524, // Azuremyst Isle
        3525, // Bloodmyst Isle
        3557, // The Exodar
    };

    // mod-individual-progression keeps a player's progress as rewarded quests 66000 + state.
    constexpr uint32 IP_PROGRESSION_QUEST_BASE = 66000;
    constexpr uint8 IP_STATE_MAX = 18;
    constexpr uint8 IP_STATE_OUTLAND = 8;
    constexpr uint8 IP_STATE_QUEL_DANAS = 12;
    constexpr uint8 IP_STATE_NORTHREND = 13;

    // Flags a spawn can have without being someone you'd go and talk to.
    constexpr uint32 NOT_A_SERVICE = UNIT_NPC_FLAG_SPELLCLICK | UNIT_NPC_FLAG_PLAYER_VEHICLE;

    struct Config
    {
        bool enabled = true;
        uint32 spellId = 90050;
        bool learnSpell = true;
        uint32 resultsPerPage = 10;
        uint32 maxResults = 60;
        bool hideLockedContinents = true;
        bool saveZoneData = true;
        bool progressionEnabled = false; // IndividualProgression.Enable
    };

    Config config;

    // Menu senders: which part of the menu a pick came from. The action says what was picked.
    enum Sender : uint32
    {
        SENDER_MAIN        = 1, // action: a Submenu
        SENDER_CATEGORY    = 2, // action: a Category
        SENDER_PROFESSION  = 3, // action: a skill line
        SENDER_RESULT      = 4, // action: index into the player's current results
        SENDER_PAGE        = 5, // action: page number
        SENDER_SEARCH      = 6, // the search box
    };

    enum Submenu : uint32
    {
        SUBMENU_MAIN        = 0,
        SUBMENU_PROFESSIONS = 1,
        SUBMENU_MORE        = 2,
    };

    enum class TrainerKind : uint8
    {
        None,
        Class,        // value: class id
        Profession,   // value: skill line
        Riding,
        WeaponMaster,
        Pet,
    };

    enum Category : uint32
    {
        CATEGORY_CLASS_TRAINER = 1,
        CATEGORY_RIDING,
        CATEGORY_WEAPON_MASTER,
        CATEGORY_PET_TRAINER,
        CATEGORY_FLIGHT_MASTER,
        CATEGORY_INNKEEPER,
        CATEGORY_BANK,
        CATEGORY_AUCTION_HOUSE,
        CATEGORY_MAILBOX,
        CATEGORY_REPAIR,
        CATEGORY_STABLE_MASTER,
        CATEGORY_GUILD_MASTER,
        CATEGORY_TABARD,
        CATEGORY_GUILD_VAULT,
        CATEGORY_BATTLEMASTER,
        CATEGORY_BARBER,
        CATEGORY_REAGENTS,
        CATEGORY_FOOD,
        CATEGORY_GENERAL_GOODS,
        CATEGORY_POISONS,
    };

    struct CategoryDef
    {
        Category id;
        char const* label;
        uint8 icon;
        uint32 npcflag;       // matches creatures with this flag
        uint32 goType;        // or game objects of this type (0 = none)
        TrainerKind trainer;  // or trainers of this kind
        bool mainMenu;        // on the first page, not under "More"
        uint8 onlyClass;      // shown only to this class (0 = everyone)
    };

    // Class trainers and profession trainers get their own entries on the main menu.
    constexpr std::array<CategoryDef, 19> CATEGORIES = {{
        { CATEGORY_FLIGHT_MASTER, "Flight master",   GOSSIP_ICON_TAXI,      UNIT_NPC_FLAG_FLIGHTMASTER,    0,                            TrainerKind::None,         true,  0 },
        { CATEGORY_INNKEEPER,     "Innkeeper",       GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_INNKEEPER,      0,                            TrainerKind::None,         true,  0 },
        { CATEGORY_BANK,          "Bank",            GOSSIP_ICON_MONEY_BAG, UNIT_NPC_FLAG_BANKER,          0,                            TrainerKind::None,         true,  0 },
        { CATEGORY_AUCTION_HOUSE, "Auction house",   GOSSIP_ICON_MONEY_BAG, UNIT_NPC_FLAG_AUCTIONEER,      0,                            TrainerKind::None,         true,  0 },
        { CATEGORY_MAILBOX,       "Mailbox",         GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_MAILBOX,        GAMEOBJECT_TYPE_MAILBOX,      TrainerKind::None,         true,  0 },
        { CATEGORY_REPAIR,        "Repairs",         GOSSIP_ICON_VENDOR,    UNIT_NPC_FLAG_REPAIR,          0,                            TrainerKind::None,         true,  0 },
        { CATEGORY_RIDING,        "Riding trainer",  GOSSIP_ICON_TRAINER,   0,                             0,                            TrainerKind::Riding,       false, 0 },
        { CATEGORY_WEAPON_MASTER, "Weapon master",   GOSSIP_ICON_TRAINER,   0,                             0,                            TrainerKind::WeaponMaster, false, 0 },
        { CATEGORY_PET_TRAINER,   "Pet trainer",     GOSSIP_ICON_TRAINER,   0,                             0,                            TrainerKind::Pet,          false, CLASS_HUNTER },
        { CATEGORY_STABLE_MASTER, "Stable master",   GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_STABLEMASTER,   0,                            TrainerKind::None,         false, CLASS_HUNTER },
        { CATEGORY_GUILD_MASTER,  "Guild master",    GOSSIP_ICON_TABARD,    UNIT_NPC_FLAG_PETITIONER,      0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_TABARD,        "Tabard designer", GOSSIP_ICON_TABARD,    UNIT_NPC_FLAG_TABARDDESIGNER,  0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_GUILD_VAULT,   "Guild vault",     GOSSIP_ICON_MONEY_BAG, UNIT_NPC_FLAG_GUILD_BANKER,    GAMEOBJECT_TYPE_GUILD_BANK,   TrainerKind::None,         false, 0 },
        { CATEGORY_BATTLEMASTER,  "Battlemaster",    GOSSIP_ICON_BATTLE,    UNIT_NPC_FLAG_BATTLEMASTER,    0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_BARBER,        "Barber",          GOSSIP_ICON_INTERACT_1, 0,                            GAMEOBJECT_TYPE_BARBER_CHAIR, TrainerKind::None,         false, 0 },
        { CATEGORY_REAGENTS,      "Reagents",        GOSSIP_ICON_VENDOR,    UNIT_NPC_FLAG_VENDOR_REAGENT,  0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_FOOD,          "Food and drink",  GOSSIP_ICON_VENDOR,    UNIT_NPC_FLAG_VENDOR_FOOD,     0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_GENERAL_GOODS, "General goods",   GOSSIP_ICON_VENDOR,    UNIT_NPC_FLAG_VENDOR_AMMO,     0,                            TrainerKind::None,         false, 0 },
        { CATEGORY_POISONS,       "Poisons",         GOSSIP_ICON_VENDOR,    UNIT_NPC_FLAG_VENDOR_POISON,   0,                            TrainerKind::None,         false, CLASS_ROGUE },
    }};

    // Extra words a spawn can be found by, from its npc flags.
    struct FlagTag
    {
        uint32 flag;
        char const* words;
    };

    constexpr std::array<FlagTag, 17> FLAG_TAGS = {{
        { UNIT_NPC_FLAG_QUESTGIVER,       "quest" },
        { UNIT_NPC_FLAG_VENDOR,           "vendor" },
        { UNIT_NPC_FLAG_VENDOR_AMMO,      "general goods ammo" },
        { UNIT_NPC_FLAG_VENDOR_FOOD,      "food drink" },
        { UNIT_NPC_FLAG_VENDOR_POISON,    "poisons" },
        { UNIT_NPC_FLAG_VENDOR_REAGENT,   "reagents" },
        { UNIT_NPC_FLAG_REPAIR,           "repairs" },
        { UNIT_NPC_FLAG_FLIGHTMASTER,     "flight master taxi" },
        { UNIT_NPC_FLAG_SPIRITHEALER,     "spirit healer" },
        { UNIT_NPC_FLAG_INNKEEPER,        "innkeeper inn" },
        { UNIT_NPC_FLAG_BANKER,           "bank banker" },
        { UNIT_NPC_FLAG_PETITIONER,       "guild master charter" },
        { UNIT_NPC_FLAG_TABARDDESIGNER,   "tabard designer" },
        { UNIT_NPC_FLAG_BATTLEMASTER,     "battlemaster battleground" },
        { UNIT_NPC_FLAG_AUCTIONEER,       "auction house auctioneer" },
        { UNIT_NPC_FLAG_STABLEMASTER,     "stable master" },
        { UNIT_NPC_FLAG_GUILD_BANKER,     "guild vault bank" },
    }};

    // Professions in the order the menu lists them.
    constexpr std::array<uint32, 14> PROFESSIONS = {
        SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_HERBALISM,
        SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING,
        SKILL_TAILORING, SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING,
    };

    constexpr std::array<char const*, 7> RANK_NAMES = {
        "", "Apprentice", "Journeyman", "Expert", "Artisan", "Master", "Grand Master",
    };

    // ChrClasses.dbc names aren't loaded by the core.
    char const* ClassName(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Class";
        }
    }

    // A creature or game object template that has at least one spawn in the townIndex.
    struct Entry
    {
        bool gameObject = false;
        uint32 id = 0;
        std::string name;
        std::string subname;
        std::string search;       // lower-case name, subname and tags
        uint32 faction = 0;       // creatures only
        uint32 goType = 0;        // game objects only
        TrainerKind trainer = TrainerKind::None;
        uint32 trainerValue = 0;  // class id or skill line
        uint8 trainerRank = 0;    // highest profession rank taught, 1 (Apprentice) to 6 (Grand Master)
    };

    // One spawn.
    struct Place
    {
        uint32 entry = 0;         // index into Index::entries
        uint32 npcflag = 0;
        uint16 map = 0;
        uint32 zone = 0;
        uint32 area = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint32 phaseMask = 0;
        int16 event = 0;          // > 0: only while that event runs; < 0: gone while it runs
    };

    // Built once at startup and never changed afterwards, so map threads can read it freely.
    struct Index
    {
        std::vector<Entry> entries;
        std::vector<Place> places;
        std::unordered_map<uint32, std::string> areaNames;       // zone/area id -> name
        std::unordered_map<uint32, std::string> areaNamesLower;
    };

    Index townIndex;

    // What a player is looking at, kept on the Player so each map thread only touches its own.
    struct FinderState : public DataMap::Base
    {
        std::vector<uint32> results; // indexes into Index::places
        uint32 page = 0;
    };

    // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
    // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
    // neither. Looking for both at compile time lets the module build on all of them.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    std::string Lower(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    }

    std::vector<std::string> Words(std::string_view text)
    {
        std::vector<std::string> words;
        std::string lower = Lower(text);
        size_t pos = 0;
        while (pos < lower.size())
        {
            while (pos < lower.size() && std::isspace((unsigned char)lower[pos]))
                ++pos;
            size_t end = pos;
            while (end < lower.size() && !std::isspace((unsigned char)lower[end]))
                ++end;
            if (end > pos)
                words.emplace_back(lower.substr(pos, end - pos));
            pos = end;
        }
        return words;
    }

    char const* DbcString(char const* const (&names)[16])
    {
        char const* name = names[sWorld->GetDefaultDbcLocale()];
        return name && *name ? name : names[LOCALE_enUS];
    }

    std::string const& AreaName(uint32 areaId)
    {
        static std::string const unknown = "Unknown";
        auto itr = townIndex.areaNames.find(areaId);
        return itr != townIndex.areaNames.end() ? itr->second : unknown;
    }

    void RememberAreaName(uint32 areaId)
    {
        if (!areaId || townIndex.areaNames.count(areaId))
            return;

        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
        {
            std::string name = DbcString(area->area_name);
            townIndex.areaNamesLower[areaId] = Lower(name);
            townIndex.areaNames[areaId] = std::move(name);
        }
    }

    std::string ContinentName(uint32 mapId)
    {
        if (MapEntry const* map = sMapStore.LookupEntry(mapId))
            return DbcString(map->name);
        return "another world";
    }

    // --- Trainers -----------------------------------------------------------------------------

    // Which skill lines a trainer spell belongs to, and which profession rank it teaches.
    void CollectSkills(uint32 spellId, std::map<uint32, uint32>& votes, std::map<uint32, uint8>& ranks, bool followLearn)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell)
            return;

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            ++votes[itr->second->SkillLine];

        for (SpellEffectInfo const& effect : spell->Effects)
        {
            // Apprentice Alchemy and friends: the skill, and a step of 75 points per rank.
            if (effect.Effect == SPELL_EFFECT_SKILL_STEP && effect.MiscValue > 0)
            {
                uint32 skill = uint32(effect.MiscValue);
                votes[skill] += 10;
                ranks[skill] = std::max<uint8>(ranks[skill], uint8(std::clamp(effect.CalcValue(), 0, 6)));
            }
            else if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && followLearn && effect.TriggerSpell)
                CollectSkills(effect.TriggerSpell, votes, ranks, false);
        }
    }

    bool IsProfessionSkill(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line && (line->categoryId == SKILL_CATEGORY_PROFESSION || line->categoryId == SKILL_CATEGORY_SECONDARY);
    }

    bool IsWeaponSkill(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line && line->categoryId == SKILL_CATEGORY_WEAPON;
    }

    // Reads what kind of trainer a creature is from the trainer tables: class trainers carry
    // their class, profession trainers and weapon masters are recognised by the skills their
    // spells belong to.
    void ClassifyTrainer(Entry& entry, uint32 npcflag)
    {
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry.id);
        if (!trainer)
        {
            // Since 3.0 pet trainers only reset pet talents, and many have no trainer data left.
            if ((npcflag & UNIT_NPC_FLAG_TRAINER) && Lower(entry.subname) == "pet trainer")
                entry.trainer = TrainerKind::Pet;
            return;
        }

        std::map<uint32, uint32> votes;
        std::map<uint32, uint8> ranks;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (spell.ReqSkillLine)
                ++votes[spell.ReqSkillLine];
            CollectSkills(spell.SpellId, votes, ranks, true);
        }

        // Weapon masters are stored as class or as tradeskill trainers, depending on the data.
        if (std::any_of(votes.begin(), votes.end(), [](auto const& vote) { return IsWeaponSkill(vote.first); }))
        {
            entry.trainer = TrainerKind::WeaponMaster;
            return;
        }

        switch (trainer->GetTrainerType())
        {
            case Trainer::Type::Class:
                if (trainer->GetTrainerRequirement())
                {
                    entry.trainer = TrainerKind::Class;
                    entry.trainerValue = trainer->GetTrainerRequirement();
                }
                break;
            case Trainer::Type::Mount:
                entry.trainer = TrainerKind::Riding;
                break;
            case Trainer::Type::Pet:
                entry.trainer = TrainerKind::Pet;
                break;
            case Trainer::Type::Tradeskill:
            {
                uint32 best = 0;
                uint32 bestVotes = 0;
                for (auto const& [skill, count] : votes)
                    if (count > bestVotes && IsProfessionSkill(skill))
                    {
                        best = skill;
                        bestVotes = count;
                    }

                if (best)
                {
                    entry.trainer = TrainerKind::Profession;
                    entry.trainerValue = best;
                    entry.trainerRank = ranks[best];
                }
                break;
            }
            default:
                break;
        }
    }

    std::string SkillName(uint32 skill)
    {
        if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
            return DbcString(line->name);
        return "Profession";
    }

    // --- Building the index -------------------------------------------------------------------

    bool IsWorldMap(uint32 mapId)
    {
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        return map && map->IsWorldMap();
    }

    // Test and placeholder creatures that Blizzard left in the data.
    bool LooksLikePlaceholder(std::string const& name)
    {
        return name.empty() || name.front() == '[' || name.find("DND") != std::string::npos
            || name.find("(PH)") != std::string::npos;
    }

    std::string BuildSearchText(Entry const& entry, uint32 npcflags)
    {
        std::string text = entry.name + " " + entry.subname;

        for (FlagTag const& tag : FLAG_TAGS)
            if (npcflags & tag.flag)
                text += std::string(" ") + tag.words;

        switch (entry.trainer)
        {
            case TrainerKind::Class:
                text += std::string(" trainer ") + ClassName(uint8(entry.trainerValue));
                break;
            case TrainerKind::Profession:
                text += " trainer " + SkillName(entry.trainerValue);
                if (entry.trainerRank)
                    text += std::string(" ") + RANK_NAMES[entry.trainerRank];
                break;
            case TrainerKind::Riding:
                text += " riding trainer";
                break;
            case TrainerKind::WeaponMaster:
                text += " weapon master trainer";
                break;
            case TrainerKind::Pet:
                text += " pet trainer";
                break;
            default:
                break;
        }

        return Lower(text);
    }

    struct SpawnZones
    {
        std::unordered_map<uint32, std::pair<uint32, uint32>> creatures;
        std::unordered_map<uint32, std::pair<uint32, uint32>> objects;
    };

    // Zone and area ids the database already has (the core fills them in when
    // Calculate.Creature.Zone.Area.Data is on, and so does this module). The casts pin the column
    // types, which the Field getters check.
    void LoadSavedZones(std::unordered_map<uint32, std::pair<uint32, uint32>>& zones, char const* table)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(zoneId AS UNSIGNED), "
            "CAST(areaId AS UNSIGNED) FROM {} WHERE zoneId <> 0", table))
            do
            {
                Field* fields = result->Fetch();
                zones[uint32(fields[0].Get<uint64>())] = { uint32(fields[1].Get<uint64>()), uint32(fields[2].Get<uint64>()) };
            } while (result->NextRow());
    }

    std::unordered_map<uint32, int16> LoadEvents(char const* table)
    {
        std::unordered_map<uint32, int16> events;
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(eventEntry AS SIGNED) FROM {}", table))
            do
            {
                Field* fields = result->Fetch();
                events[uint32(fields[0].Get<uint64>())] = int16(fields[1].Get<int64>());
            } while (result->NextRow());
        return events;
    }

    // Zone and area for a spawn: saved in the database, or worked out from the map files (and
    // then saved, so the next start is quick). Only safe before the maps start updating.
    std::pair<uint32, uint32> ZoneOf(SpawnData const& data, std::unordered_map<uint32, std::pair<uint32, uint32>> const& saved,
        char const* table, WorldDatabaseTransaction trans, uint32& computed)
    {
        auto itr = saved.find(data.spawnId);
        if (itr != saved.end())
            return itr->second;

        uint32 zone = 0;
        uint32 area = 0;
        sMapMgr->GetZoneAndAreaId(data.phaseMask, zone, area, data.mapid, data.posX, data.posY, data.posZ);
        ++computed;

        if (config.saveZoneData && zone)
            trans->Append("UPDATE {} SET zoneId = {}, areaId = {} WHERE guid = {}", table, zone, area, data.spawnId);

        return { zone, area };
    }

    void BuildIndex()
    {
        uint32 oldMSTime = getMSTime();

        SpawnZones zones;
        LoadSavedZones(zones.creatures, "creature");
        LoadSavedZones(zones.objects, "gameobject");
        std::unordered_map<uint32, int16> creatureEvents = LoadEvents("game_event_creature");
        std::unordered_map<uint32, int16> objectEvents = LoadEvents("game_event_gameobject");

        WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
        uint32 computed = 0;

        std::unordered_map<uint32, uint32> creatureEntries; // template entry -> index into entries
        std::unordered_map<uint32, uint32> objectEntries;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (!IsWorldMap(data.mapid))
                continue;

            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
            if (!creature || (creature->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) || LooksLikePlaceholder(creature->Name))
                continue;

            uint32 npcflag = 0;
            uint32 unitFlags = 0;
            uint32 dynamicFlags = 0;
            ObjectMgr::ChooseCreatureFlags(creature, npcflag, unitFlags, dynamicFlags, &data);
            if (!(npcflag & ~NOT_A_SERVICE) || (unitFlags & UNIT_FLAG_NOT_SELECTABLE))
                continue;

            auto [itr, isNew] = creatureEntries.try_emplace(data.id, uint32(townIndex.entries.size()));
            if (isNew)
            {
                Entry entry;
                entry.id = data.id;
                entry.name = creature->Name;
                entry.subname = creature->SubName;
                entry.faction = creature->faction;
                ClassifyTrainer(entry, npcflag);
                entry.search = BuildSearchText(entry, npcflag);
                townIndex.entries.push_back(std::move(entry));
            }

            Place place;
            place.entry = itr->second;
            place.npcflag = npcflag;
            place.map = data.mapid;
            place.x = data.posX;
            place.y = data.posY;
            place.phaseMask = data.phaseMask;
            std::tie(place.zone, place.area) = ZoneOf(data, zones.creatures, "creature", trans, computed);
            if (auto event = creatureEvents.find(spawnId); event != creatureEvents.end())
                place.event = event->second;

            RememberAreaName(place.zone);
            RememberAreaName(place.area);
            townIndex.places.push_back(place);
        }

        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            if (!IsWorldMap(data.mapid))
                continue;

            GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!object || LooksLikePlaceholder(object->name))
                continue;

            if (object->type != GAMEOBJECT_TYPE_MAILBOX && object->type != GAMEOBJECT_TYPE_BARBER_CHAIR
                && object->type != GAMEOBJECT_TYPE_GUILD_BANK)
                continue;

            auto [itr, isNew] = objectEntries.try_emplace(data.id, uint32(townIndex.entries.size()));
            if (isNew)
            {
                Entry entry;
                entry.gameObject = true;
                entry.id = data.id;
                entry.name = object->name;
                entry.goType = object->type;
                entry.search = Lower(entry.name + (object->type == GAMEOBJECT_TYPE_MAILBOX ? " mail"
                    : object->type == GAMEOBJECT_TYPE_BARBER_CHAIR ? " barber" : " guild vault bank"));
                townIndex.entries.push_back(std::move(entry));
            }

            Place place;
            place.entry = itr->second;
            place.map = data.mapid;
            place.x = data.posX;
            place.y = data.posY;
            place.phaseMask = data.phaseMask;
            std::tie(place.zone, place.area) = ZoneOf(data, zones.objects, "gameobject", trans, computed);
            if (auto event = objectEvents.find(spawnId); event != objectEvents.end())
                place.event = event->second;

            RememberAreaName(place.zone);
            RememberAreaName(place.area);
            townIndex.places.push_back(place);
        }

        if (trans->GetSize())
            WorldDatabase.CommitTransaction(trans);

        LOG_INFO("server.loading", ">> mod-npc-finder: indexed {} places ({} kinds of NPC and object); worked out {} zones in {} ms",
            townIndex.places.size(), townIndex.entries.size(), computed, GetMSTimeDiffToNow(oldMSTime));
    }

    // --- Who sees what ------------------------------------------------------------------------

    struct Viewer
    {
        Player* player = nullptr;
        FactionTemplateEntry const* faction = nullptr;
        uint8 progression = IP_STATE_MAX;
        bool everything = false;
    };

    uint8 ProgressionState(Player* player)
    {
        uint8 state = 0;
        for (uint8 i = 1; i <= IP_STATE_MAX; ++i)
            if (player->GetQuestStatus(IP_PROGRESSION_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                state = i;
        return state;
    }

    Viewer MakeViewer(Player* player)
    {
        Viewer viewer;
        viewer.player = player;
        viewer.faction = player->GetFactionTemplateEntry();
        viewer.everything = player->IsGameMaster();
        if (config.hideLockedContinents && config.progressionEnabled && !viewer.everything)
            viewer.progression = ProgressionState(player);
        return viewer;
    }

    bool IsOpen(Viewer const& viewer, Place const& place)
    {
        if (viewer.everything)
            return true;

        switch (place.map)
        {
            case MAP_OUTLAND:
                if (std::find(ZONES_OPEN_ON_OUTLAND_MAP.begin(), ZONES_OPEN_ON_OUTLAND_MAP.end(), place.zone) != ZONES_OPEN_ON_OUTLAND_MAP.end())
                    return true;
                if (place.zone == ZONE_ISLE_OF_QUEL_DANAS)
                    return viewer.progression >= IP_STATE_QUEL_DANAS;
                return viewer.progression >= IP_STATE_OUTLAND;
            case MAP_NORTHREND:
                return viewer.progression >= IP_STATE_NORTHREND;
            case MAP_EBON_HOLD:
                return viewer.player->getClass() == CLASS_DEATH_KNIGHT;
            default:
                return true;
        }
    }

    bool IsFriendly(Viewer const& viewer, Entry const& entry)
    {
        if (entry.gameObject || !viewer.faction)
            return true;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(entry.faction);
        if (!faction)
            return true;

        if (faction->IsHostileTo(*viewer.faction))
            return false;

        // Reputation can make an otherwise friendly faction hostile.
        return viewer.player->GetReputationRank(faction->faction) > REP_HOSTILE;
    }

    bool IsThere(Viewer const& viewer, Place const& place)
    {
        if (!(place.phaseMask & (PHASEMASK_NORMAL | viewer.player->GetPhaseMask())))
            return false;

        if (place.event > 0)
            return sGameEventMgr->IsActiveEvent(uint16(place.event));
        if (place.event < 0)
            return !sGameEventMgr->IsActiveEvent(uint16(-place.event));
        return true;
    }

    bool CanSee(Viewer const& viewer, Place const& place)
    {
        return IsOpen(viewer, place) && IsThere(viewer, place) && IsFriendly(viewer, townIndex.entries[place.entry]);
    }

    // --- Searching ----------------------------------------------------------------------------

    struct Query
    {
        std::vector<std::string> words;         // every word must match
        CategoryDef const* category = nullptr;
        bool classTrainer = false;
        uint32 profession = 0;                  // skill line
    };

    bool Matches(Query const& query, Viewer const& viewer, Place const& place)
    {
        Entry const& entry = townIndex.entries[place.entry];

        if (query.classTrainer)
            return entry.trainer == TrainerKind::Class && entry.trainerValue == viewer.player->getClass();

        if (query.profession)
            return entry.trainer == TrainerKind::Profession && entry.trainerValue == query.profession;

        if (CategoryDef const* category = query.category)
        {
            if (category->trainer != TrainerKind::None)
                return entry.trainer == category->trainer;
            if (entry.gameObject)
                return category->goType && entry.goType == category->goType;
            return category->npcflag && (place.npcflag & category->npcflag);
        }

        for (std::string const& word : query.words)
        {
            if (entry.search.find(word) != std::string::npos)
                continue;

            auto zone = townIndex.areaNamesLower.find(place.zone);
            if (zone != townIndex.areaNamesLower.end() && zone->second.find(word) != std::string::npos)
                continue;

            auto area = townIndex.areaNamesLower.find(place.area);
            if (area != townIndex.areaNamesLower.end() && area->second.find(word) != std::string::npos)
                continue;

            return false;
        }

        return !query.words.empty();
    }

    std::vector<uint32> Search(Player* player, Query const& query)
    {
        Viewer viewer = MakeViewer(player);

        struct Hit
        {
            uint32 place;
            bool sameMap;
            float distance;
        };

        // The nearest spawn of each NPC in each zone, so a wandering guard shows up once.
        std::map<std::pair<uint32, uint32>, Hit> nearest;
        for (uint32 i = 0; i < townIndex.places.size(); ++i)
        {
            Place const& place = townIndex.places[i];
            if (!Matches(query, viewer, place) || !CanSee(viewer, place))
                continue;

            bool sameMap = place.map == player->GetMapId();
            float distance = sameMap ? player->GetExactDist2d(place.x, place.y) : 0.0f;

            auto key = std::make_pair(place.entry, place.zone);
            auto itr = nearest.find(key);
            if (itr == nearest.end() || (sameMap && distance < itr->second.distance))
                nearest[key] = { i, sameMap, distance };
        }

        std::vector<Hit> hits;
        hits.reserve(nearest.size());
        for (auto const& [key, hit] : nearest)
            hits.push_back(hit);

        // Your own continent first, nearest first; then the rest by continent and zone.
        std::sort(hits.begin(), hits.end(), [](Hit const& a, Hit const& b)
        {
            if (a.sameMap != b.sameMap)
                return a.sameMap;
            if (a.sameMap)
                return a.distance < b.distance;

            Place const& pa = townIndex.places[a.place];
            Place const& pb = townIndex.places[b.place];
            if (pa.map != pb.map)
                return pa.map < pb.map;
            if (pa.zone != pb.zone)
                return AreaName(pa.zone) < AreaName(pb.zone);
            return townIndex.entries[pa.entry].name < townIndex.entries[pb.entry].name;
        });

        if (hits.size() > config.maxResults)
            hits.resize(config.maxResults);

        std::vector<uint32> results;
        results.reserve(hits.size());
        for (Hit const& hit : hits)
            results.push_back(hit.place);
        return results;
    }

    // --- Describing a place -------------------------------------------------------------------

    // Map coordinates as the world map shows them, or false if the zone has no map of its own.
    bool ZoneCoordinates(Place const& place, float& x, float& y)
    {
        x = place.x;
        y = place.y;
        Map2ZoneCoordinates(x, y, place.zone);
        return place.zone && x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f
            && !(x == place.x && y == place.y);
    }

    std::string Where(Place const& place)
    {
        std::string where = AreaName(place.zone);
        if (place.area && place.area != place.zone)
            where += ", " + AreaName(place.area);

        float x;
        float y;
        if (ZoneCoordinates(place, x, y))
            where += Acore::StringFormat(" ({:.0f}, {:.0f})", x, y);

        return where;
    }

    char const* Compass(Player* player, Place const& place)
    {
        static constexpr std::array<char const*, 8> directions = {
            "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west",
        };

        // World x points north and world y points west.
        float north = place.x - player->GetPositionX();
        float east = player->GetPositionY() - place.y;
        float degrees = std::atan2(east, north) * 180.0f / float(M_PI);
        if (degrees < 0.0f)
            degrees += 360.0f;

        return directions[uint32(std::lround(degrees / 45.0f)) % directions.size()];
    }

    std::string Title(Entry const& entry)
    {
        std::string title = entry.name;
        if (!entry.subname.empty())
            title += " <" + entry.subname + ">";
        return title;
    }

    std::string ResultText(Player* player, Place const& place)
    {
        Entry const& entry = townIndex.entries[place.entry];
        std::string text = Title(entry) + "\n";

        if (place.map == player->GetMapId())
        {
            text += Where(place);
            text += Acore::StringFormat(" - {} yd", uint32(player->GetExactDist2d(place.x, place.y)));
        }
        else
            text += ContinentName(place.map) + ": " + Where(place);

        if (entry.trainer == TrainerKind::Profession && entry.trainerRank)
            text += std::string(" - up to ") + RANK_NAMES[entry.trainerRank];

        return text;
    }

    // Flags a spot on the map and minimap, like a city guard's directions do.
    void SendMapMarker(Player* player, Place const& place, std::string const& name)
    {
        WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + name.size() + 1);
        data << uint32(POI_FLAGS);
        data << float(place.x);
        data << float(place.y);
        data << uint32(POI_ICON_RED_FLAG);
        data << uint32(0);
        data << name;
        player->SendDirectMessage(&data);
    }

    void ShowPlace(Player* player, Place const& place)
    {
        Entry const& entry = townIndex.entries[place.entry];
        ChatHandler handler(player->GetSession());

        if (place.map == player->GetMapId())
        {
            SendMapMarker(player, place, entry.name);
            handler.PSendSysMessage("|cffffd000Town Guide:|r {} - {}, {} yards {}. Marked on your map.",
                Title(entry), Where(place), uint32(player->GetExactDist2d(place.x, place.y)), Compass(player, place));
        }
        else
            handler.PSendSysMessage("|cffffd000Town Guide:|r {} is in {}: {}.", Title(entry), ContinentName(place.map), Where(place));
    }

    // --- The menu -----------------------------------------------------------------------------

    FinderState& GetState(Player* player)
    {
        return *player->CustomData.GetDefault<FinderState>("mod-npc-finder");
    }

    void StartMenu(Player* player)
    {
        ClearGossipMenuFor(player);
        player->PlayerTalkClass->GetGossipMenu().SetMenuId(GOSSIP_MENU_TOWN_GUIDE);
    }

    void AddSearchBox(Player* player, char const* label)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, SENDER_SEARCH, 0,
            "Type a name, a title such as \"mage trainer\", or a zone.", 0, true);
    }

    void ShowMainMenu(Player* player)
    {
        StartMenu(player);
        AddSearchBox(player, "Search by name, title or zone...");
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, std::string(ClassName(player->getClass())) + " trainer", SENDER_CATEGORY, CATEGORY_CLASS_TRAINER);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Profession trainers...", SENDER_MAIN, SUBMENU_PROFESSIONS);

        for (CategoryDef const& category : CATEGORIES)
            if (category.mainMenu && (!category.onlyClass || category.onlyClass == player->getClass()))
                AddGossipItemFor(player, category.icon, category.label, SENDER_CATEGORY, category.id);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "More...", SENDER_MAIN, SUBMENU_MORE);
        SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
    }

    void ShowMoreMenu(Player* player)
    {
        StartMenu(player);
        for (CategoryDef const& category : CATEGORIES)
            if (!category.mainMenu && (!category.onlyClass || category.onlyClass == player->getClass()))
                AddGossipItemFor(player, category.icon, category.label, SENDER_CATEGORY, category.id);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", SENDER_MAIN, SUBMENU_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
    }

    void ShowProfessionMenu(Player* player)
    {
        StartMenu(player);
        for (uint32 skill : PROFESSIONS)
        {
            std::string label = SkillName(skill);
            if (uint16 max = player->GetMaxSkillValue(skill))
                label += Acore::StringFormat(" (yours: {})", RANK_NAMES[std::clamp<uint32>(max / 75, 1, 6)]);
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label, SENDER_PROFESSION, skill);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", SENDER_MAIN, SUBMENU_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
    }

    void ShowResultsPage(Player* player)
    {
        FinderState& state = GetState(player);
        StartMenu(player);

        if (state.results.empty())
        {
            AddSearchBox(player, "Search again...");
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the menu", SENDER_MAIN, SUBMENU_MAIN);
            SendGossipMenuFor(player, NPC_TEXT_NO_RESULTS, player->GetGUID());
            return;
        }

        uint32 perPage = config.resultsPerPage;
        uint32 pages = (uint32(state.results.size()) + perPage - 1) / perPage;
        state.page = std::min(state.page, pages - 1);

        uint32 first = state.page * perPage;
        uint32 last = std::min<uint32>(first + perPage, uint32(state.results.size()));
        for (uint32 i = first; i < last; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_DOT, ResultText(player, townIndex.places[state.results[i]]), SENDER_RESULT, i);

        if (state.page + 1 < pages)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Next page ({}/{})", state.page + 2, pages), SENDER_PAGE, state.page + 1);
        if (state.page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", SENDER_PAGE, state.page - 1);

        AddSearchBox(player, "New search...");
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the menu", SENDER_MAIN, SUBMENU_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_RESULTS, player->GetGUID());
    }

    void RunQuery(Player* player, Query const& query)
    {
        FinderState& state = GetState(player);
        state.results = Search(player, query);
        state.page = 0;
        ShowResultsPage(player);
    }

    void SearchText(Player* player, std::string_view text)
    {
        Query query;
        query.words = Words(text);
        if (query.words.empty())
        {
            ShowMainMenu(player);
            return;
        }

        RunQuery(player, query);
    }

    CategoryDef const* FindCategory(uint32 id)
    {
        for (CategoryDef const& category : CATEGORIES)
            if (category.id == id)
                return &category;
        return nullptr;
    }

    void HandleMenuPick(Player* player, uint32 sender, uint32 action)
    {
        switch (sender)
        {
            case SENDER_MAIN:
                if (action == SUBMENU_PROFESSIONS)
                    ShowProfessionMenu(player);
                else if (action == SUBMENU_MORE)
                    ShowMoreMenu(player);
                else
                    ShowMainMenu(player);
                break;
            case SENDER_CATEGORY:
            {
                Query query;
                if (action == CATEGORY_CLASS_TRAINER)
                    query.classTrainer = true;
                else if (!(query.category = FindCategory(action)))
                {
                    ShowMainMenu(player);
                    break;
                }
                RunQuery(player, query);
                break;
            }
            case SENDER_PROFESSION:
            {
                Query query;
                query.profession = action;
                RunQuery(player, query);
                break;
            }
            case SENDER_RESULT:
            {
                FinderState const& state = GetState(player);
                CloseGossipMenuFor(player);
                if (action < state.results.size())
                    ShowPlace(player, townIndex.places[state.results[action]]);
                break;
            }
            case SENDER_PAGE:
                GetState(player).page = action;
                ShowResultsPage(player);
                break;
            case SENDER_SEARCH: // the search box sent back empty
                ShowMainMenu(player);
                break;
            default:
                CloseGossipMenuFor(player);
                break;
        }
    }

    void OpenTownGuide(Player* player)
    {
        if (!config.enabled)
        {
            ChatHandler(player->GetSession()).SendSysMessage("The Town Guide is turned off on this server.");
            return;
        }

        ShowMainMenu(player);
    }
}

class NpcFinderWorldScript : public WorldScript
{
public:
    NpcFinderWorldScript() : WorldScript("NpcFinderWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("NpcFinder.Enable", true);
        config.spellId = sConfigMgr->GetOption<uint32>("NpcFinder.SpellId", 90050);
        config.learnSpell = sConfigMgr->GetOption<bool>("NpcFinder.LearnSpell", true);
        config.resultsPerPage = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("NpcFinder.ResultsPerPage", 10), 1, 25);
        config.maxResults = std::max<uint32>(sConfigMgr->GetOption<uint32>("NpcFinder.MaxResults", 60), 1);
        config.hideLockedContinents = sConfigMgr->GetOption<bool>("NpcFinder.HideLockedContinents", true);
        config.saveZoneData = sConfigMgr->GetOption<bool>("NpcFinder.SaveZoneData", true);
        config.progressionEnabled = sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false);
    }

    // Runs after every spawn is loaded and before the maps start updating, which is what makes
    // it safe to look up zones from the map files here.
    void OnStartup() override
    {
        if (config.enabled)
            BuildIndex();
    }
};

class NpcFinderPlayerScript : public PlayerScript
{
public:
    NpcFinderPlayerScript() : PlayerScript("NpcFinderPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_GOSSIP_SELECT,
        PLAYERHOOK_ON_GOSSIP_SELECT_CODE,
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!config.enabled || !config.learnSpell || IsBotSession(player->GetSession()))
            return;

        if (config.spellId && !player->HasSpell(config.spellId) && sSpellMgr->GetSpellInfo(config.spellId))
            player->learnSpell(config.spellId);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (config.spellId && spell->GetSpellInfo()->Id == config.spellId)
            OpenTownGuide(player);
    }

    void OnPlayerGossipSelect(Player* player, uint32 menuId, uint32 sender, uint32 action) override
    {
        if (menuId != GOSSIP_MENU_TOWN_GUIDE)
            return;

        HandleMenuPick(player, sender, action);
    }

    void OnPlayerGossipSelectCode(Player* player, uint32 menuId, uint32 sender, uint32 /*action*/, char const* code) override
    {
        if (menuId != GOSSIP_MENU_TOWN_GUIDE)
            return;

        if (sender == SENDER_SEARCH && code)
            SearchText(player, code);
        else
            CloseGossipMenuFor(player);
    }
};

class NpcFinderCommandScript : public CommandScript
{
public:
    NpcFinderCommandScript() : CommandScript("NpcFinderCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "find", HandleFindCommand, SEC_PLAYER, Console::No },
        };

        return commandTable;
    }

    static bool HandleFindCommand(ChatHandler* handler, Tail text)
    {
        Player* player = handler->GetPlayer();
        if (!config.enabled)
        {
            handler->SendSysMessage("The Town Guide is turned off on this server.");
            return true;
        }

        if (std::string_view(text).empty())
            ShowMainMenu(player);
        else
            SearchText(player, text);

        return true;
    }
};

void AddNpcFinderScripts()
{
    new NpcFinderWorldScript();
    new NpcFinderPlayerScript();
    new NpcFinderCommandScript();
}
