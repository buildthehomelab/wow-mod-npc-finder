# NPC Finder (Town Guide)

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that gives every player a
**Town Guide** spell. Cast it and a menu opens where you can:

- **search** by name, title or zone (`alchemy trainer`, `bank orgrimmar`, `thrall`)
- pick **your class trainer**, a **profession trainer**, or a service: flight master, innkeeper,
  bank, auction house, mailbox, repairs, and under *More...* riding trainer, weapon master, pet
  trainer, stable master, guild master, tabard designer, guild vault, battlemaster, barber,
  reagents, food and drink, general goods and poisons

Pick a result and it's **flagged on your map and minimap**. Chat also gives you the zone,
map coordinates, distance and direction. Results on your own continent come first, nearest
first; places on other continents are listed with their coordinates.

`.find` opens the same menu, and `.find <words>` searches straight away.

## Built from your server's data

Nothing is hard-coded. At startup the module reads every creature and object spawn on the
continents, so NPCs other modules or you have added are found like any other:

- **Trainers** are recognised from the trainer tables: class trainers by the class they teach,
  profession trainers by the skills their spells belong to. Profession results show the highest
  rank the trainer teaches ("up to Artisan"), and the profession menu shows your own rank.
- **Services** come from each NPC's flags (innkeeper, banker, flight master...). Mailboxes,
  barber chairs and guild vaults are game objects and are found too.
- **Only what you can use**: NPCs hostile to your faction or your reputation are left out, as are
  holiday NPCs outside their event, phased NPCs you can't see, triggers and Blizzard's
  placeholder creatures.
- **Progression aware**: with [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression),
  Outland stays hidden until the player reaches the TBC stage, the Isle of Quel'Danas until
  Sunwell and Northrend until WotLK. The blood elf and draenei starting zones are always shown.
  Game masters see everything.

If an NPC stands in several spots in one zone (a wandering guard), only the nearest one is shown.

## Requirements

- [AzerothCore](https://www.azerothcore.org/) wotlk (master) and a WoW 3.3.5a (12340) client.
- The client patch for the Town Guide spell (see *Client* below). Python 3 and
  [StormLib](https://github.com/ladislav-zezula/StormLib) are needed to build it. `.find` works
  without the patch.
- Optional: [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  to hide continents the player hasn't reached yet.

## Install

### Server

Clone it into your AzerothCore `modules` folder **as `mod-npc-finder`**, without the repo's `wow-`
prefix. AzerothCore finds the module's entry point from the folder name.

```bash
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-npc-finder.git mod-npc-finder
```

Rebuild the worldserver, then copy `conf/mod_npc_finder.conf.dist` to your config folder as
`mod_npc_finder.conf`. The world database update adds the spell to `spell_dbc` and the menu text
to `npc_text` on the next start.

The first start takes a little longer: the stock database has no zone for most spawns, so the
module works them out from the map files and saves them to the `creature` and `gameobject`
tables (`NpcFinder.SaveZoneData`). Later starts read them back.

### Client

Town Guide is a new spell, and the 3.3.5 client only knows spells in its own `Spell.dbc`. A patch
MPQ replaces the whole file, so the spell has to go into the `Spell.dbc` your realm patch already
ships. `client/build_patch.py` adds it (it needs Python 3 and [StormLib](https://github.com/ladislav-zezula/StormLib)):

```bash
python3 client/build_patch.py --from-mpq patch-P.MPQ --out patch-P.MPQ.new
```

That keeps everything else in the patch and adds spell 90050. If your realm has no patch with a
`Spell.dbc` yet, start from the client's own (extract `DBFilesClient\Spell.dbc` from
`Data/enUS/patch-enUS-3.MPQ` or the newest patch that has it):

```bash
python3 client/build_patch.py --dbc Spell.dbc --out patch-P.MPQ
```

Players put the MPQ in `World of Warcraft/Data/`. Until they have it, the spell shows up blank in
their spellbook, but `.find` works for everyone.

If you change the spell in the script, regenerate the server's row and put it in the SQL file:

```bash
python3 client/build_patch.py --from-mpq patch-P.MPQ.new --sql
```

## The spell

Town Guide (90050) is instant, targets yourself, costs nothing and has no cooldown beyond the
global one. It works while mounted, sitting, shapeshifted or stealthed and doesn't break stealth.
It's taught to every character at login (`NpcFinder.LearnSpell`) and sits in the General tab of
the spellbook. Bots don't get it.

It's a copy of Blizzard's unused "Dummy Spell" (18282) with the INV_Misc_Map_01 icon; casting it
does nothing but open the menu.

## Configuration

See `conf/mod_npc_finder.conf.dist`:

| Option | Default | What it does |
| --- | --- | --- |
| `NpcFinder.Enable` | 1 | Master switch. |
| `NpcFinder.SpellId` | 90050 | The Town Guide spell. 0 leaves the spell out. |
| `NpcFinder.LearnSpell` | 1 | Teach the spell at login. |
| `NpcFinder.ResultsPerPage` | 10 | Results on one menu page (1-25). |
| `NpcFinder.MaxResults` | 60 | Most results a search returns. |
| `NpcFinder.HideLockedContinents` | 1 | Hide continents mod-individual-progression hasn't opened. |
| `NpcFinder.SaveZoneData` | 1 | Save worked-out zones to the database. |

## IDs used

- spell 90050 (`spell_dbc`, and the client patch)
- `npc_text` 9500500-9500502
- gossip menu id 9500500 (in code only; not in the database)

## Notes

- The spawn list is read once at startup. NPCs added later with `.npc add` show up after a
  restart.
- The map flag only works on the continent you're on; for other continents you get the zone and
  coordinates in chat.
- Names are the database's English names.

## Troubleshooting

- **The Town Guide spell is blank in the spellbook.** The client only knows spells in its own
  `Spell.dbc`, so the player needs the client patch MPQ in `World of Warcraft/Data/`. `.find` works
  without it.
- **The first start is slow.** The module works out the zone of every spawn from the map files and
  saves it to the `creature` and `gameobject` tables (`NpcFinder.SaveZoneData`). Later starts read
  the saved zones back.
- **An NPC added with `.npc add` isn't found.** The spawn list is read once at startup, so
  restart the worldserver.
- **No map flag appears.** The flag only works on the continent you're on; for other continents
  you get the zone and coordinates in chat.
- **Outland or Northrend results are missing.** With mod-individual-progression they stay hidden
  until the player reaches that stage. Set `NpcFinder.HideLockedContinents = 0` to show them.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
