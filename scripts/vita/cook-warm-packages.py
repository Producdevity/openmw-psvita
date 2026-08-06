#!/usr/bin/env python3
"""Cook common-asset warm packages from TES3 ESM files.

Scans CELL records (region, refs) and base records (model paths), emits:
  - commonwarm.txt: flat global top-N (freq = distinct-cell count) for the
    runtime's existing loader
  - warm_regions.txt: [general] + [region:<id>] sections for region-keyed
    warming (future runtime consumption)
"""
import struct, sys, collections

MODEL_RECORDS = {b'STAT', b'ACTI', b'CONT', b'DOOR', b'MISC', b'WEAP', b'ARMO',
                 b'CLOT', b'BOOK', b'INGR', b'ALCH', b'APPA', b'LOCK', b'PROB',
                 b'REPA', b'LIGH', b'CREA', b'BODY'}

def subrecords(data):
    off = 0
    while off + 8 <= len(data):
        name = data[off:off+4]
        size = struct.unpack_from('<I', data, off+4)[0]
        yield name, data[off+8:off+8+size]
        off += 8 + size

def cstr(b):
    return b.split(b'\0')[0].decode('latin-1').strip().lower()

def scan(paths):
    models = {}                      # base id -> model path
    lev = {}                         # leveled list id -> member ids
    npcs = {}                        # npc id -> (race, female, gear ids)
    wornparts = {}                   # armor/clothing id -> bodypart record ids
    cell_models = collections.defaultdict(set)   # (region, cellkey) accumulation
    for path in paths:
        data = open(path, 'rb').read()
        off = 0
        while off + 16 <= len(data):
            rtype = data[off:off+4]
            size = struct.unpack_from('<I', data, off+4)[0]
            rec = data[off+16:off+16+size]
            off += 16 + size
            if rtype in MODEL_RECORDS:
                rid = model = None
                parts = []
                seen_indx = False
                for sname, sdata in subrecords(rec):
                    if sname == b'NAME': rid = cstr(sdata)
                    elif sname == b'MODL': model = cstr(sdata).replace('\\', '/')
                    elif sname == b'INDX': seen_indx = True
                    elif sname in (b'BNAM', b'CNAM') and seen_indx and rtype in (b'ARMO', b'CLOT'):
                        parts.append(cstr(sdata))
                if rid and parts:
                    wornparts[rid] = parts
                if rid and model:
                    base = model.rsplit('/', 1)[-1]
                    if base.startswith('marker_') or base == 'editormarker.nif':
                        continue
                    models[rid] = 'meshes/' + model
            elif rtype == b'NPC_':
                rid = None; race = ''; female = False; gear = []
                for sname, sdata in subrecords(rec):
                    if sname == b'NAME': rid = cstr(sdata)
                    elif sname == b'RNAM': race = cstr(sdata)
                    elif sname == b'FLAG' and len(sdata) >= 4:
                        female = bool(struct.unpack_from('<I', sdata)[0] & 1)
                    elif sname in (b'BNAM', b'KNAM'): gear.append(cstr(sdata))
                    elif sname == b'NPCO' and len(sdata) >= 36:
                        gear.append(cstr(sdata[4:36]))
                if rid: npcs[rid] = (race, female, gear)
            elif rtype in (b'LEVC', b'LEVI'):
                rid = None; members = []
                for sname, sdata in subrecords(rec):
                    if sname == b'NAME': rid = cstr(sdata)
                    elif sname in (b'CNAM', b'INAM'): members.append(cstr(sdata))
                if rid: lev[rid] = members
            elif rtype == b'CELL':
                region = None; grid = None; interior = False; cellname = None
                ids = []
                last = None
                for sname, sdata in subrecords(rec):
                    # Cell header DATA only; refs carry their own DATA subrecords.
                    if sname == b'DATA' and len(sdata) == 12 and grid is None and last is None:
                        flags, gx, gy = struct.unpack_from('<Iii', sdata)
                        interior = bool(flags & 1); grid = (gx, gy)
                    elif sname == b'RGNN': region = cstr(sdata)
                    elif sname == b'FRMR': last = 'frmr'
                    elif sname == b'NAME' and last == 'frmr':
                        ids.append(cstr(sdata)); last = None
                    elif sname == b'NAME' and cellname is None:
                        cellname = cstr(sdata)
                if interior:
                    cell_models[('__interior__', cellname or str(len(cell_models)))].update(ids)
                else:
                    cell_models[(region or '__wilderness__', grid)].update(ids)
    raw_cell_ids = {k: set(v) for k, v in cell_models.items()}
    # NPCs behave like lists: expand to worn gear + race body parts.
    race_parts = collections.defaultdict(list)
    for rid in models:
        if rid.startswith('b_n_') and '1st' not in rid and '_hair' not in rid and '_head' not in rid:
            race_parts[rid[4:].split('_')[0].strip()].append(rid)
    carriers = collections.Counter()
    for nid, (race, female, gear) in npcs.items():
        carriers.update(set(gear))
    for nid, npc in npcs.items():
        race, female, gear = npc
        marker = '_f_' if female else '_m_'
        parts = [rid for rid in race_parts.get(race, ()) if marker in rid]
        # Head/hair (first two) always render; carried items only when
        # common enough to be a worn uniform rather than vendor stock.
        common = [g for g in gear[:2]] + [g for g in gear[2:] if carriers[g] >= 3]
        worn = [pt for g in common for pt in wornparts.get(g, ()) if '1st' not in pt]
        lev[nid] = common + parts + worn
    # Expand leveled lists to their spawnable members (nested lists too).
    for _ in range(4):
        changed = False
        for lid, members in lev.items():
            ext = [m for mid in members if mid in lev for m in lev[mid]]
            if any(m not in members for m in ext):
                lev[lid] = list(set(members) | set(ext)); changed = True
        if not changed: break
    for key, ids in cell_models.items():
        extra = {m for rid in ids if rid in lev for m in lev[rid]}
        ids.update(extra)
    return models, cell_models, raw_cell_ids

def main():
    esms = sys.argv[1:-1]; outdir = sys.argv[-1]
    models, cell_models, raw_cell_ids = scan(esms)
    # Object->cell index: first placement wins (matches runtime scan order).
    with open(outdir + '/object_index.txt', 'w') as f:
        seen = set()
        n = 0
        for (region, key), ids in raw_cell_ids.items():
            for rid in sorted(ids):
                if not rid or rid in seen: continue
                seen.add(rid)
                if region == '__interior__':
                    f.write(f"{rid}\tI {key}\n")
                elif isinstance(key, tuple):
                    f.write(f"{rid}\tE {key[0]} {key[1]}\n")
                n += 1
        print(f"object index: {n} ids")
    region_freq = collections.defaultdict(collections.Counter)
    global_freq = collections.Counter()
    region_count = collections.defaultdict(set)
    for (region, _key), ids in cell_models.items():
        seen = set()
        for rid in ids:
            m = models.get(rid)
            if m and m not in seen:
                seen.add(m)
                region_freq[region][m] += 1
                global_freq[m] += 1
                region_count[m].add(region)
    # flat file: global top-1000 (exterior+interior mixed)
    with open(outdir + '/commonwarm.txt', 'w') as f:
        for m, c in global_freq.most_common(1000):
            f.write(f"{c} {m}\n")
    # region packages: general = in >=4 regions; per-region top-100 minus general
    general = {m for m, regs in region_count.items() if len(regs) >= 4}
    general_ranked = [(global_freq[m], m) for m in general]
    general_ranked.sort(reverse=True)
    # Skeletons: every actor needs these before anything else renders.
    for anim in ('meshes/base_animkna.nif', 'meshes/base_anim_female.nif', 'meshes/base_anim.nif'):
        general_ranked.insert(0, (9999, anim))
        general.add(anim)
    # Cluster regions into asset-biomes by model-set overlap: sibling
    # regions (the three ash zones etc.) share one package, so border
    # walks between them need no swap at all.
    names = [r for r in region_freq if r != '__interior__']
    ENV = lambda m: not m.startswith(('meshes/b/', 'meshes/a/', 'meshes/c/'))
    rsets = {r: set([m for m, c in region_freq[r].most_common() if ENV(m)][:200]) for r in names}
    parent = {r: r for r in names}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for i, a in enumerate(names):
        for b in names[i+1:]:
            inter = len(rsets[a] & rsets[b])
            union = len(rsets[a] | rsets[b])
            j = inter / union if union else 0
            if j >= 0.45:
                parent[find(a)] = find(b)
            if j >= 0.2:
                print(f"  overlap {a} ~ {b}: {j:.2f}")
    clusters = collections.defaultdict(list)
    for r in names:
        clusters[find(r)].append(r)
    cluster_pkg = {}
    for root, members in clusters.items():
        merged = collections.Counter()
        for r in members:
            merged.update(region_freq[r])
        cluster_pkg[root] = merged
    print(f"biome clusters: {len(clusters)} from {len(names)} regions:")
    for root, members in clusters.items():
        if len(members) > 1:
            print(f"  {members}")
    general_written = set(m for c, m in general_ranked[:300])
    region_written = collections.defaultdict(set)
    with open(outdir + '/warm_regions.txt', 'w') as f:
        f.write("[general]\n")
        for c, m in general_ranked[:300]:
            f.write(f"{c} {m}\n")
        for region in sorted(region_freq):
            if region == '__interior__':
                continue
            f.write(f"[region:{region}]\n")
            n = 0
            written = region_written[region]
            cluster_counts = cluster_pkg[find(region)]
            for m, c in cluster_counts.most_common():
                if m in general: continue
                f.write(f"{c} {m}\n")
                written.add(m)
                n += 1
                if n >= 250: break
            # Architecture tier: complex x/ models are few-but-brutal cold.
            a = 0
            for m, c in cluster_counts.most_common():
                if m in general or m in written: continue
                if not m.startswith('meshes/x/'): continue
                f.write(f"{c} {m}\n")
                written.add(m)
                a += 1
                if a >= 150: break
        # Hotspots: per-cell remainder beyond general + its region package.
        nhot = 0
        for (region, key), ids in sorted(cell_models.items(), key=lambda kv: str(kv[0])):
            if region == '__interior__' or not isinstance(key, tuple): continue
            cellset = set(models[r] for r in ids if r in models)
            rest = cellset - general_written - region_written[region]
            if len(rest) < 12: continue
            f.write(f"[hotspot:{key[0]},{key[1]}]\n")
            for m in sorted(rest, key=lambda m: -global_freq[m])[:80]:
                f.write(f"{global_freq[m]} {m}\n")
            nhot += 1
        print(f"hotspots: {nhot}")
    print(f"models={len(models)} cells={len(cell_models)} regions={len(region_freq)-1}")
    print(f"global top: {global_freq.most_common(8)}")
    print(f"general set: {len(general_ranked[:150])} models")
main()
