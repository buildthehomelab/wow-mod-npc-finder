#!/usr/bin/env python3
"""
mod-npc-finder: adds the Town Guide spell to a 3.3.5a client Spell.dbc and packs it into an MPQ.

The 3.3.5 client only casts spells in its own Spell.dbc, and a patch MPQ replaces the whole
file, so the row has to go into the Spell.dbc your realm patch already ships (patch-P on this
server). Start from that patch so its other spell changes are kept:

    python3 build_patch.py --from-mpq patch-P.MPQ --out patch-P.MPQ.new
    python3 build_patch.py --dbc Spell.dbc --out-dbc Spell.dbc.new     # just the DBC
    python3 build_patch.py --sql                                       # print the server SQL

Packing needs StormLib (libstorm). Point STORMLIB at it if it isn't /usr/local/lib/libstorm.dylib.

The spell is a copy of "Dummy Spell" (18282): instant, targets yourself, no cost, no cooldown,
and a Dummy effect that does nothing. The server module opens the menu when it's cast.
Running it again replaces the row, so it's safe to rebuild.

Released under the MIT License.
"""

import argparse
import ctypes
import os
import shutil
import struct
import sys
import tempfile

SPELL_ID = 90050
TEMPLATE_ID = 18282  # "Dummy Spell"

NAME = "Town Guide"
DESCRIPTION = "Ask where to find trainers, flight masters, the bank and other townsfolk. " \
              "Search by name, title or zone and the place is flagged on your map."

FIELDS = 234
RECORD_SIZE = FIELDS * 4

# Spell.dbc field numbers (3.3.5a, build 12340)
F_ID = 0
F_ATTRIBUTES = 4
F_ATTRIBUTES_EX = 5
F_SPELL_ICON = 133
F_NAME = 136            # 16 locale strings, then a flags field
F_NAME_SUBTEXT = 153
F_DESCRIPTION = 170
F_AURA_DESCRIPTION = 187
F_START_RECOVERY_CATEGORY = 205
F_START_RECOVERY_TIME = 206

STRING_FIELDS = [F_NAME + i for i in range(16)] + [F_NAME_SUBTEXT + i for i in range(16)] \
    + [F_DESCRIPTION + i for i in range(16)] + [F_AURA_DESCRIPTION + i for i in range(16)]
# Speed, EffectRealPointsPerLevel, EffectPointsCombo, EffectMultipleValue, DmgMultiplier,
# EffectBonusMultiplier: the float columns of AzerothCore's spell_dbc table.
FLOAT_FIELDS = {47, 77, 78, 79, 101, 102, 103, 119, 120, 121, 216, 217, 218, 229, 230, 231}

SPELL_ATTR0_ALLOW_WHILE_MOUNTED = 0x01000000
SPELL_ATTR0_ALLOW_WHILE_SITTING = 0x08000000
SPELL_ATTR1_ALLOW_WHILE_STEALTHED = 0x00000020

ICON_INV_MISC_MAP_01 = 2578      # SpellIcon.dbc, as on 45853 "Survey Sinkholes"
GCD_CATEGORY = 133               # the normal global cooldown, as on 61304
GCD_TIME = 1500


def read_dbc(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, count, fields, size, strsize = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC" or fields != FIELDS or size != RECORD_SIZE:
        sys.exit(f"{path}: not a 3.3.5a Spell.dbc ({magic}, {fields} fields, {size} bytes a row)")
    rows = [list(struct.unpack_from(f"<{FIELDS}I", data, 20 + i * RECORD_SIZE)) for i in range(count)]
    strings = data[20 + count * RECORD_SIZE:20 + count * RECORD_SIZE + strsize]
    return rows, bytearray(strings)


def write_dbc(path, rows, strings):
    rows = sorted(rows, key=lambda r: r[F_ID])
    with open(path, "wb") as f:
        f.write(struct.pack("<4s4I", b"WDBC", len(rows), FIELDS, RECORD_SIZE, len(strings)))
        for row in rows:
            f.write(struct.pack(f"<{FIELDS}I", *row))
        f.write(strings)


def add_string(strings, text):
    if not text:
        return 0
    offset = len(strings)
    strings += text.encode("utf-8") + b"\0"
    return offset


def town_guide_row(rows, strings):
    template = next((r for r in rows if r[F_ID] == TEMPLATE_ID), None)
    if template is None:
        sys.exit(f"template spell {TEMPLATE_ID} not found")

    row = list(template)
    row[F_ID] = SPELL_ID
    row[F_ATTRIBUTES] |= SPELL_ATTR0_ALLOW_WHILE_MOUNTED | SPELL_ATTR0_ALLOW_WHILE_SITTING
    row[F_ATTRIBUTES_EX] |= SPELL_ATTR1_ALLOW_WHILE_STEALTHED
    row[F_SPELL_ICON] = ICON_INV_MISC_MAP_01
    row[F_START_RECOVERY_CATEGORY] = GCD_CATEGORY
    row[F_START_RECOVERY_TIME] = GCD_TIME

    for field in STRING_FIELDS:
        row[field] = 0
    row[F_NAME] = add_string(strings, NAME)
    row[F_DESCRIPTION] = add_string(strings, DESCRIPTION)
    return row


def add_spell(src, dst):
    rows, strings = read_dbc(src)
    rows = [r for r in rows if r[F_ID] != SPELL_ID]
    rows.append(town_guide_row(rows, strings))
    write_dbc(dst, rows, strings)
    print(f"{dst}: {len(rows)} spells, {SPELL_ID} '{NAME}' added")


def sql_value(field, value):
    if field in STRING_FIELDS:
        return "''"
    if field in FLOAT_FIELDS:
        return repr(round(struct.unpack("<f", struct.pack("<I", value))[0], 6))
    return str(struct.unpack("<i", struct.pack("<I", value))[0])


def print_sql(dbc):
    rows, strings = read_dbc(dbc)
    row = town_guide_row(rows, strings)
    values = [sql_value(i, v) for i, v in enumerate(row)]
    values[F_NAME] = "'" + NAME.replace("'", "''") + "'"
    values[F_DESCRIPTION] = "'" + DESCRIPTION.replace("'", "''") + "'"
    print(f"DELETE FROM `spell_dbc` WHERE `ID` = {SPELL_ID};")
    print("INSERT INTO `spell_dbc` VALUES (" + ", ".join(values) + ");")


# --- MPQ ------------------------------------------------------------------------------------

def stormlib():
    lib = ctypes.CDLL(os.environ.get("STORMLIB", "/usr/local/lib/libstorm.dylib"))
    handle = ctypes.c_void_p
    lib.SFileOpenArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(handle)]
    lib.SFileCreateArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(handle)]
    lib.SFileExtractFile.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    lib.SFileAddFileEx.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
    lib.SFileCloseArchive.argtypes = [handle]

    class FindData(ctypes.Structure):
        _fields_ = [("cFileName", ctypes.c_char * 1024), ("szPlainName", ctypes.c_char_p),
                    ("dwHashIndex", ctypes.c_uint), ("dwBlockIndex", ctypes.c_uint),
                    ("dwFileSize", ctypes.c_uint), ("dwFileFlags", ctypes.c_uint),
                    ("dwCompSize", ctypes.c_uint), ("dwFileTimeLo", ctypes.c_uint),
                    ("dwFileTimeHi", ctypes.c_uint), ("lcLocale", ctypes.c_uint)]

    lib.SFileFindFirstFile.argtypes = [handle, ctypes.c_char_p, ctypes.POINTER(FindData), ctypes.c_char_p]
    lib.SFileFindFirstFile.restype = handle
    lib.SFileFindNextFile.argtypes = [handle, ctypes.POINTER(FindData)]
    lib.SFileFindClose.argtypes = [handle]
    return lib, handle, FindData


def extract_all(mpq, folder):
    lib, handle, FindData = stormlib()
    h = handle()
    if not lib.SFileOpenArchive(mpq.encode(), 0, 0x100, ctypes.byref(h)):
        sys.exit(f"can't open {mpq}")

    names = []
    found = FindData()
    search = lib.SFileFindFirstFile(h, b"*", ctypes.byref(found), None)
    while search:
        names.append(found.cFileName.decode())
        if not lib.SFileFindNextFile(search, ctypes.byref(found)):
            break
    if search:
        lib.SFileFindClose(search)

    files = []
    for name in names:
        if name in ("(listfile)", "(attributes)", "(signature)"):
            continue
        dst = os.path.join(folder, *name.split("\\"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not lib.SFileExtractFile(h, name.encode(), dst.encode(), 0):
            sys.exit(f"can't extract {name}")
        files.append((dst, name))
    lib.SFileCloseArchive(h)
    return files


def pack(out, files):
    lib, handle, _ = stormlib()
    if os.path.exists(out):
        os.remove(out)
    h = handle()
    # MPQ v1 with a listfile and attributes; each file zlib-compressed, like the realm's patches.
    if not lib.SFileCreateArchive(out.encode(), 0x00300000, max(16, len(files) * 2), ctypes.byref(h)):
        sys.exit(f"can't create {out}")
    for src, name in files:
        if not lib.SFileAddFileEx(h, src.encode(), name.encode(), 0x80000200, 0x02, 0x02):
            sys.exit(f"can't add {name}")
    lib.SFileCloseArchive(h)


def build_mpq(src_mpq, out):
    folder = tempfile.mkdtemp(prefix="npc-finder-")
    try:
        files = extract_all(src_mpq, folder)
        spell = next((f for f in files if f[1].lower() == "dbfilesclient\\spell.dbc"), None)
        if spell is None:
            sys.exit(f"{src_mpq} has no DBFilesClient\\Spell.dbc; use --dbc with the client's own Spell.dbc")
        add_spell(spell[0], spell[0])
        # Keep the original order, with Spell.dbc last as the realm's patch-P has it.
        files.sort(key=lambda f: f[1].lower() == "dbfilesclient\\spell.dbc")
        pack(out, files)
        print(f"{out}: {', '.join(name for _, name in files)}")
    finally:
        shutil.rmtree(folder)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from-mpq", help="patch MPQ that already ships Spell.dbc")
    parser.add_argument("--out", help="MPQ to write (with --from-mpq, or with --dbc)")
    parser.add_argument("--dbc", help="a Spell.dbc to start from")
    parser.add_argument("--out-dbc", help="where to write the new Spell.dbc (with --dbc)")
    parser.add_argument("--sql", action="store_true", help="print the server's spell_dbc SQL")
    args = parser.parse_args()

    if args.sql:
        dbc = args.dbc
        if not dbc and args.from_mpq:
            folder = tempfile.mkdtemp(prefix="npc-finder-")
            files = extract_all(args.from_mpq, folder)
            dbc = next(f[0] for f in files if f[1].lower() == "dbfilesclient\\spell.dbc")
        if not dbc:
            sys.exit("--sql needs --dbc or --from-mpq")
        print_sql(dbc)
    elif args.from_mpq and args.out:
        build_mpq(args.from_mpq, args.out)
    elif args.dbc and args.out_dbc:
        add_spell(args.dbc, args.out_dbc)
    elif args.dbc and args.out:
        folder = tempfile.mkdtemp(prefix="npc-finder-")
        try:
            spell = os.path.join(folder, "Spell.dbc")
            add_spell(args.dbc, spell)
            pack(args.out, [(spell, "DBFilesClient\\Spell.dbc")])
        finally:
            shutil.rmtree(folder)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
