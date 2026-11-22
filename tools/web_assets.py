"""Package the browser demo's WAD and the optional model download."""

import re
import struct
import sys
from pathlib import Path

ENTRY = struct.Struct("<ii8s")


def name(value):
    return value.encode("ascii").ljust(8, b"\0")


def ambush(map_name, barrels, east_wall, *, start=(48, 96), door=True, supplies=()):
    # Clockwise perimeters, with the front sector on each line's right.
    # The marine starts west of a manual door. Its dark eastern room
    # holds a barrel south of the route and room to summon north of it.
    vertices = [
        (0, 0), (0, 224), (224, 224), (224, 144), (224, 48), (224, 0),
        (256, 144), (256, 48), (256, 224), (east_wall, 224), (east_wall, 0), (256, 0),
    ]
    edges = [
        (0, 1, 0), (1, 2, 0), (2, 3, 0), (3, 4, 0, 1, int(door)),
        (4, 5, 0), (5, 0, 0), (3, 6, 1), (7, 4, 1),
        (6, 7, 1, 2), (6, 8, 2), (8, 9, 2), (9, 10, 2, None, 11),
        (10, 11, 2), (11, 7, 2),
    ]
    lines, sides = bytearray(), bytearray()

    def side(sector, upper="-", middle="-"):
        index = len(sides) // 30
        sides.extend(struct.pack("<hh8s8s8sh", 0, 0, name(upper), name("-"), name(middle), sector))
        return index

    for edge in edges:
        a, b, front, *rest = edge
        back = rest[0] if rest else None
        special = rest[1] if len(rest) > 1 else 0
        if back is None:
            flags = 1
            right = side(front, middle="SW1BRCOM" if special == 11 else "STARTAN3")
            left = 0xffff
        else:
            flags = 4
            right = side(front, upper="BIGDOOR2")
            left = side(back, upper="BIGDOOR2")
        lines.extend(struct.pack("<7H", a, b, flags, special, 0, right, left))

    things = [(*start, 0, 1, 7), *supplies]
    things.extend((x, y, 0, 2035, 7) for x, y in barrels)
    return [
        (map_name, b""),
        ("THINGS", b"".join(struct.pack("<5h", *thing) for thing in things)),
        ("LINEDEFS", lines),
        ("SIDEDEFS", sides),
        ("VERTEXES", b"".join(struct.pack("<hh", *xy) for xy in vertices)),
        ("SECTORS", b"".join(
            struct.pack("<hh8s8shhh", 0, ceiling, name("FLOOR4_8"), name("CEIL3_5"), light, 0, 0)
            for ceiling, light in [(96, 175), (0 if door else 96, 135), (96, 110)]
        )),
    ] + [(lump, b"") for lump in ("SEGS", "SSECTORS", "NODES", "REJECT", "BLOCKMAP")]


def demo_wad(source, output):
    data = source.read_bytes()
    count, directory = struct.unpack_from("<ii", data, 4)
    lumps = []
    keep = True
    for start, size, raw_name in ENTRY.iter_unpack(data[directory:directory + count * ENTRY.size]):
        lump = raw_name.rstrip(b"\0").decode("ascii")
        if re.fullmatch(r"E[1-9]M[1-9]", lump):
            keep = lump.startswith("E1")
        if keep and not lump.startswith("D_"):
            lumps.append((lump, data[start:start + size]))
        if lump == "BLOCKMAP":
            keep = True

    lumps.extend(ambush("FIRST", [], 720, door=False))
    lumps.extend(ambush("SHADOW", [], 480))
    lumps.extend(ambush("HOLD", [], 590))
    lumps.extend(ambush("CROSSFIR", [], 610))
    lumps.extend(ambush("GAUNTLET", [(335, 68), (385, 80)], 720,
                         supplies=[(620, 105, 0, 2012, 7)]))
    lumps.extend(ambush("AMBUSH", [(335, 68)], 640))
    lumps.extend(ambush("CHAIN", [(335, 68), (420, 80)], 720))
    body, entries = bytearray(12), bytearray()
    for lump, payload in lumps:
        entries.extend(ENTRY.pack(len(body), len(payload), name(lump)))
        body.extend(payload)
    struct.pack_into("<4sii", body, 0, b"IWAD", len(entries) // ENTRY.size, len(body))
    output.write_bytes(body + entries)


def models_pack(directory, output):
    with output.open("wb") as pack:
        for path in sorted(directory.glob("*.glb")):
            filename = path.name.encode("ascii")
            data = path.read_bytes()
            pack.write(struct.pack("<II", len(filename), len(data)))
            pack.write(filename)
            pack.write(data)


if __name__ == "__main__":
    mode, source, output = sys.argv[1:]
    {"wad": demo_wad, "models": models_pack}[mode](Path(source), Path(output))
