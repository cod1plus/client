#!/usr/bin/env python3
"""
radar.py - CS-style top-down minimap of a CoD1 MP map, straight from the compiled .bsp collision.

    python radar.py <pk3> <bsp path in pk3> <name> [--res 4] [--size 1024] [--out DIR]

Method (no textures, no lighting - the brushes the player collides with):
  * every player-blocking brush (contents SOLID 0x1 or PLAYERCLIP 0x10000; triggers / water out) of the
    world and of the non-trigger brush models is intersected with a vertical line per grid column
    (CoD1 v59 brush = 6 axial sides storing min/max as floats + extra planes, inside = n.p < d);
  * per column the solid intervals are merged; the gaps between them are "free intervals"
    (floor = top of the solid below, ceiling = bottom of the solid above); a floor with >= 56 u of
    headroom is a place a player can stand (crouched at least);
  * standable places of neighbouring columns are linked when the floors differ by <= 40 u (stairs,
    slopes, jumpable crates) and the body fits through (shared headroom >= 56 u); the components that
    contain a spawn point are the playable area - roofs, skybox tops and sealed rooms drop out;
  * the image shows, per column, the LOWEST playable floor (ground level of buildings), shaded by
    height, its outline in white, and the bomb sites A / B.
Outputs <name>.png (RGBA, north = +Y up) and <name>.json (world rectangle of the image).
"""
import argparse, json, math, os, re, struct, sys, zipfile
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HEADROOM = 56.0
STEP = 40.0


def load_bsp(pk3, path):
    # a .bsp file as is, or a file inside a pk3
    d = open(pk3, 'rb').read() if pk3.lower().endswith('.bsp') else zipfile.ZipFile(pk3).read(path)
    assert d[:4] == b'IBSP' and struct.unpack_from('<i', d, 4)[0] == 59, 'not a CoD1 v59 bsp'
    L = [struct.unpack_from('<ii', d, 8 + i * 8) for i in range(31)]   # (length, offset)
    def lump(i): ln, off = L[i]; return d[off:off + ln]
    m = lump(0)
    mats = []
    for i in range(len(m) // 72):
        nm = m[i * 72:i * 72 + 64].split(b'\0')[0].decode('latin1')
        sf, cf = struct.unpack_from('<ii', m, i * 72 + 64)
        mats.append((nm, sf, cf))
    planes = np.frombuffer(lump(2), dtype='<f4').reshape(-1, 4)
    sides_raw = lump(3)
    sides_f = np.frombuffer(sides_raw, dtype='<f4').reshape(-1, 2)[:, 0]
    sides_i = np.frombuffer(sides_raw, dtype='<i4').reshape(-1, 2)[:, 0]
    brushes = np.frombuffer(lump(4), dtype='<u2').reshape(-1, 2)
    models = [struct.unpack_from('<6f6i', lump(27), i * 48) for i in range(len(lump(27)) // 48)]
    ents = []
    for b in re.findall(r'\{([^}]*)\}', lump(29).decode('latin1')):
        ents.append(dict(re.findall(r'"([^"]*)" "([^"]*)"', b)))
    # patch collision (lump 24, 16 bytes: short material, short type, short a, short b, int i1, int i2)
    #   type 1 = triangle soup (terrain): a verts from i1, b indices (u16, vert-relative) from i2
    #   type 0 = curve patch: a x b control grid (a fastest) from i2
    cv = np.frombuffer(lump(25), dtype='<f4').reshape(-1, 3).astype(np.float64)
    ci = np.frombuffer(lump(26), dtype='<u2')
    pc = lump(24)
    tris = []
    for i in range(len(pc) // 16):
        sh, typ, a, b, i1, i2 = struct.unpack_from('<hhhhii', pc, i * 16)
        if not (0 <= sh < len(mats)) or not blocking(mats[sh]): continue
        if typ == 1:
            v = cv[i1:i1 + a]; idx = ci[i2:i2 + b].astype(np.int64).reshape(-1, 3)
            tris.append(v[idx])
        elif typ == 0 and a >= 3 and b >= 3:
            g = cv[i2:i2 + a * b].reshape(b, a, 3)
            tris.append(tess_patch(g))
    tris = np.concatenate(tris) if tris else np.zeros((0, 3, 3))
    return mats, planes, sides_f, sides_i, brushes, models, ents, tris


def tess_patch(g, n=6):
    """biquadratic bezier patch (3x3 blocks sharing edges) -> triangles"""
    out = []
    t = np.linspace(0, 1, n + 1)
    B = np.stack([(1 - t) ** 2, 2 * t * (1 - t), t ** 2], 1)          # (n+1, 3)
    for r in range(0, g.shape[0] - 2, 2):
        for c in range(0, g.shape[1] - 2, 2):
            P = g[r:r + 3, c:c + 3]                                     # (3,3,3)
            S = np.einsum('ui,vj,ijk->uvk', B, B, P)                    # (n+1, n+1, 3)
            a = S[:-1, :-1]; b = S[1:, :-1]; cc = S[1:, 1:]; d = S[:-1, 1:]
            out.append(np.stack([a, b, cc], 2).reshape(-1, 3, 3))
            out.append(np.stack([a, cc, d], 2).reshape(-1, 3, 3))
    return np.concatenate(out) if out else np.zeros((0, 3, 3))


def blocking(mat):
    nm, sf, cf = mat
    n = nm.lower()
    if 'trigger' in n or 'water' in n or 'hint' in n or 'skip' in n:
        return False
    return bool(cf & 0x1) or bool(cf & 0x10000)


def brush_list(mats, planes, sides_f, sides_i, brushes, models, ents, danger=False):
    """player-blocking brushes (danger=False) or the volumes that kill (minefield / trigger_hurt).
    A brush model's brushes are relative to its entity's origin."""
    first_side = np.concatenate([[0], np.cumsum(brushes[:, 0].astype(np.int64))])
    use = {} if danger else {0: np.zeros(3)}
    for e in ents:
        mdl = e.get('model', '')
        if not mdl.startswith('*'): continue
        cls = e.get('classname', ''); tn = e.get('targetname', '')
        is_danger = cls == 'trigger_hurt' or tn == 'minefield'
        if danger != is_danger or (not danger and cls.startswith('trigger')): continue
        use[int(mdl[1:])] = np.array([float(v) for v in e.get('origin', '0 0 0').split()])
    out = []
    for mi in sorted(use):
        if mi >= len(models): continue
        o = use[mi]
        fb, nb = models[mi][10], models[mi][11]
        for b in range(fb, fb + nb):
            ns, mat = int(brushes[b, 0]), int(brushes[b, 1])
            if ns < 6 or (not danger and not blocking(mats[mat])): continue
            s0 = int(first_side[b])
            box = sides_f[s0:s0 + 6].astype(np.float64) + np.repeat(o, 2)   # minx maxx miny maxy minz maxz
            extra = planes[sides_i[s0 + 6:s0 + ns]].astype(np.float64) if ns > 6 else np.zeros((0, 4))
            if len(extra) and o.any():
                extra = extra.copy(); extra[:, 3] += extra[:, :3] @ o
            out.append((box, extra))
    return out


def brush_columns(blist, x0, y0, nx, ny, res, zlo, zhi):
    """per brush and grid column touched: the z interval the brush occupies. CONSERVATIVE: a column
    counts when the brush overlaps its cell square (not only its centre), so a 1-2 u thick clip wall
    seals the grid instead of slipping between cell centres."""
    cols, zs0, zs1, nzs = [], [], [], []
    h = res / 2.0
    xs = x0 + (np.arange(nx) + 0.5) * res
    ys = y0 + (np.arange(ny) + 0.5) * res
    for box, extra in blist:
        if box[5] < zlo or box[4] > zhi: continue
        ix0 = max(0, int(math.ceil((box[0] - h - x0) / res - 0.5))); ix1 = min(nx - 1, int(math.floor((box[1] + h - x0) / res - 0.5)))
        iy0 = max(0, int(math.ceil((box[2] - h - y0) / res - 0.5))); iy1 = min(ny - 1, int(math.floor((box[3] + h - y0) / res - 0.5)))
        if ix0 > ix1 or iy0 > iy1: continue
        X, Y = np.meshgrid(xs[ix0:ix1 + 1], ys[iy0:iy1 + 1], indexing='ij')
        z0 = np.full(X.shape, box[4]); z1 = np.full(X.shape, box[5]); ok = np.ones(X.shape, bool)
        tnz = np.ones(X.shape)                                    # normal z of the top face
        for pnx, pny, pnz, pd in extra:
            s_ = pd - pnx * X - pny * Y + h * (abs(pnx) + abs(pny))
            if pnz > 1e-6:
                cand = s_ / pnz; upd = cand < z1
                z1 = np.where(upd, cand, z1); tnz = np.where(upd, pnz / max(1e-9, math.sqrt(pnx * pnx + pny * pny + pnz * pnz)), tnz)
            elif pnz < -1e-6: z0 = np.maximum(z0, s_ / pnz)
            else: ok &= s_ > 0
        ok &= z1 > z0 + 0.01
        if not ok.any(): continue
        IX, IY = np.meshgrid(np.arange(ix0, ix1 + 1), np.arange(iy0, iy1 + 1), indexing='ij')
        cols.append((IY * nx + IX)[ok]); zs0.append(np.clip(z0[ok], zlo, zhi)); zs1.append(np.clip(z1[ok], zlo, zhi))
        nzs.append(tnz[ok])
    return cols, zs0, zs1, nzs


def spawns_and_sites(ents):
    sp, sites = [], []
    for e in ents:
        c = e.get('classname', '')
        if ('spawn' in c and c.startswith('mp_')) or c == 'info_player_start':
            o = [float(v) for v in e.get('origin', '0 0 0').split()]
            sp.append(o)
    for e in ents:
        tn = e.get('targetname', '')
        if e.get('classname', '') in ('trigger_multiple', 'trigger_use', 'trigger_radius') and (tn.lower().startswith('bombzone') or
                                                             'bombzone' in e.get('script_gameobjectname', '')):
            lab = tn[-1].upper() if tn[-1:].upper() in ('A', 'B') else e.get('script_label', '?')
            sites.append((lab, e.get('model', ''), [float(v) for v in e.get('origin', '0 0 0').split()]))
    return np.array(sp), sites


def column_nodes(blist, tris, x0, y0, nx, ny, res, zlo, zhi):
    """free intervals (floor, ceiling) of every column with >= HEADROOM, as flat arrays"""
    cols, zs0, zs1, nzs = brush_columns(blist, x0, y0, nx, ny, res, zlo, zhi)
    # collision triangles: sampled every res/2, per column the z range the triangle covers
    #   (a flat one = a thin slab to stand on, a steep one = a wall slab)
    step = res / 2.0
    for A, Bv, C in tris:
        e1 = Bv - A; e2 = C - A
        nu = max(1, int(math.ceil(np.hypot(e1[0], e1[1]) / step)) if abs(e1[2]) < 1e9 else 1)
        nv = max(1, int(math.ceil(np.hypot(e2[0], e2[1]) / step)))
        nu = max(nu, int(math.ceil(abs(e1[2]) / step / 4)) or 1); nv = max(nv, int(math.ceil(abs(e2[2]) / step / 4)) or 1)
        if nu * nv > 4_000_000: continue
        u, v = np.meshgrid(np.arange(nu + 1) / nu, np.arange(nv + 1) / nv, indexing='ij')
        m = (u + v) <= 1.0 + 1e-9
        u = u[m]; v = v[m]
        P = A + u[:, None] * e1 + v[:, None] * e2
        ix = np.floor((P[:, 0] - x0) / res).astype(np.int64); iy = np.floor((P[:, 1] - y0) / res).astype(np.int64)
        ok = (ix >= 0) & (ix < nx) & (iy >= 0) & (iy < ny)
        if not ok.any(): continue
        c = iy[ok] * nx + ix[ok]; z = P[ok, 2]
        uc, inv = np.unique(c, return_inverse=True)
        zmin = np.full(len(uc), np.inf); zmax = np.full(len(uc), -np.inf)
        np.minimum.at(zmin, inv, z); np.maximum.at(zmax, inv, z)
        cols.append(uc); zs0.append(np.clip(zmin - 2.0, zlo, zhi)); zs1.append(np.clip(zmax, zlo, zhi))
        nn = np.cross(e1, e2); nzs.append(np.full(len(uc), abs(nn[2]) / max(1e-9, np.linalg.norm(nn))))
    col = np.concatenate(cols); a = np.concatenate(zs0); b = np.concatenate(zs1); nz = np.concatenate(nzs)
    o = np.lexsort((a, col)); col, a, b, nz = col[o], a[o], b[o], nz[o]
    SH = 20000.0; BIG = 100000.0
    key = col * BIG + (b + SH)
    run = np.maximum.accumulate(key) - col * BIG - SH            # running max of z1 within the column
    prev = np.concatenate([[-1e9], run[:-1]])
    newcol = np.concatenate([[True], col[1:] != col[:-1]])
    start = newcol | (a > prev + 0.5)
    idx = np.flatnonzero(start)
    mcol = col[idx]; mz0 = a[idx]; mz1 = np.maximum.reduceat(b, idx)
    gid = np.cumsum(start) - 1                     # the face that makes each merged top: its slope
    topnz = np.ones(len(idx)); it = b >= mz1[gid] - 1e-6
    topnz[gid[it]] = nz[it]
    # free gap between merged solid i and i+1 of the same column
    same = mcol[1:] == mcol[:-1]
    f = mz1[:-1][same]; t = mz0[1:][same]; c = mcol[:-1][same]; fn = topnz[:-1][same]
    # above the highest solid of a column: open up to zhi (the sky brushes sit above zhi)
    last = np.concatenate([~same, [True]])
    f = np.concatenate([f, mz1[last]]); t = np.concatenate([t, np.full(last.sum(), zhi)]); c = np.concatenate([c, mcol[last]])
    fn = np.concatenate([fn, topnz[last]])
    keep = ((t - f) >= HEADROOM) & (fn >= 0.7)      # PM: standable only on a slope <= 45 deg
    return c[keep], f[keep], t[keep]


def components(nc, f, t, nx, ny, K=6):
    """label the standable places, linked across neighbouring columns"""
    order = np.lexsort((f, nc)); nc, f, t = nc[order], f[order], t[order]
    first = np.searchsorted(nc, np.arange(nx * ny))
    count = np.searchsorted(nc, np.arange(nx * ny), side='right') - first
    N = len(nc)
    slot = np.arange(N) - first[nc]
    keepk = slot < K
    F = np.full((nx * ny, K), np.nan, np.float32); T = np.full((nx * ny, K), np.nan, np.float32)
    I = np.full((nx * ny, K), -1, np.int32)
    F[nc[keepk], slot[keepk]] = f[keepk]; T[nc[keepk], slot[keepk]] = t[keepk]; I[nc[keepk], slot[keepk]] = np.flatnonzero(keepk)
    F = F.reshape(ny, nx, K); T = T.reshape(ny, nx, K); I = I.reshape(ny, nx, K)
    ea, eb = [], []
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        ys = slice(0, ny - dy); yd = slice(dy, ny)
        if dx >= 0: xs = slice(0, nx - dx); xd = slice(dx, nx)
        else: xs = slice(-dx, nx); xd = slice(0, nx + dx)
        Fa, Ta, Ia = F[ys, xs], T[ys, xs], I[ys, xs]
        Fb, Tb, Ib = F[yd, xd], T[yd, xd], I[yd, xd]
        for i in range(K):
            if np.all(Ia[..., i] < 0): continue
            for j in range(K):
                if np.all(Ib[..., j] < 0): continue
                with np.errstate(invalid='ignore'):
                    m = (np.abs(Fa[..., i] - Fb[..., j]) <= STEP) & \
                        ((np.minimum(Ta[..., i], Tb[..., j]) - np.maximum(Fa[..., i], Fb[..., j])) >= HEADROOM)
                m &= (Ia[..., i] >= 0) & (Ib[..., j] >= 0)
                if m.any():
                    ea.append(Ia[..., i][m]); eb.append(Ib[..., j][m])
    ea = np.concatenate(ea) if ea else np.zeros(0, np.int64)
    eb = np.concatenate(eb) if eb else np.zeros(0, np.int64)
    # union-find by hooking roots to the smaller root + full pointer jumping (O(log N) rounds)
    lab = np.arange(N)
    ea = ea.astype(np.int64); eb = eb.astype(np.int64)
    while True:
        lu = lab[ea]; lv = lab[eb]
        m = lu != lv
        if not m.any(): break
        ea, eb, lu, lv = ea[m], eb[m], lu[m], lv[m]
        np.minimum.at(lab, np.maximum(lu, lv), np.minimum(lu, lv))
        while True:
            nl = lab[lab]
            if np.array_equal(nl, lab): break
            lab = nl
    return nc, f, t, lab


def grid_labels(mask):
    """4-connected components of a 2D bool mask -> (labels array, -1 outside)"""
    h, w = mask.shape
    idx = np.full(mask.shape, -1, np.int64); n = int(mask.sum()); idx[mask] = np.arange(n)
    ea = np.concatenate([idx[:, :-1][mask[:, :-1] & mask[:, 1:]], idx[:-1, :][mask[:-1, :] & mask[1:, :]]])
    eb = np.concatenate([idx[:, 1:][mask[:, :-1] & mask[:, 1:]], idx[1:, :][mask[:-1, :] & mask[1:, :]]])
    lab = np.arange(n)
    while True:
        lu = lab[ea]; lv = lab[eb]; m = lu != lv
        if not m.any(): break
        ea, eb, lu, lv = ea[m], eb[m], lu[m], lv[m]
        np.minimum.at(lab, np.maximum(lu, lv), np.minimum(lu, lv))
        while True:
            nl = lab[lab]
            if np.array_equal(nl, lab): break
            lab = nl
    out = np.full(mask.shape, -1, np.int64); out[mask] = lab
    return out


def despeckle(mask, min_hole, min_island):
    """fill non-playable specks smaller than min_hole cells, drop playable islands < min_island"""
    lab = grid_labels(~mask)
    u, cnt = np.unique(lab[lab >= 0], return_counts=True)
    small = u[cnt < min_hole]
    mask = mask | np.isin(lab, small)
    lab = grid_labels(mask)
    u, cnt = np.unique(lab[lab >= 0], return_counts=True)
    return mask & ~np.isin(lab, u[cnt < min_island])


def render(name, nc, f, t, lab, seeds_lab, nx, ny, x0, y0, res, sites, models, size, outdir, clip=None,
           upp=None, edge_units=None, labels=True):
    play = np.isin(lab, seeds_lab)
    pc, pf = nc[play], f[play]
    low = np.full(nx * ny, np.nan)
    np.fmin.at(low, pc, pf)                       # lowest playable floor per column
    low = low.reshape(ny, nx)
    mask = ~np.isnan(low)
    if clip is not None:                                   # optional: only around the spawns / sites
        cx_ = x0 + (np.arange(nx) + 0.5) * res; cy_ = y0 + (np.arange(ny) + 0.5) * res
        inside = (cx_[None, :] >= clip[0]) & (cx_[None, :] <= clip[2]) & (cy_[:, None] >= clip[1]) & (cy_[:, None] <= clip[3])
        mask &= inside
    # specks: < 24 cells of 4 u (a 20 u rubble bit) filled, playable islands < 300 cells dropped
    filled = despeckle(mask, int(24 * (4.0 / res) ** 2), int(300 * (4.0 / res) ** 2))
    gap = filled & ~mask
    if gap.any():                                          # filled specks take their neighbours' floor
        from PIL import ImageFilter as _F
        low = np.where(gap, np.nan, low)
        for _ in range(6):
            pad_ = np.pad(low, 1, constant_values=np.nan)
            nb = np.nanmin(np.stack([pad_[:-2, 1:-1], pad_[2:, 1:-1], pad_[1:-1, :-2], pad_[1:-1, 2:]]), 0)
            low = np.where(filled & np.isnan(low), nb, low)
    mask = filled & ~np.isnan(low)
    # crop: playable bounds + margin, square
    yy, xx = np.nonzero(mask)
    cx0, cx1, cy0, cy1 = xx.min(), xx.max(), yy.min(), yy.max()
    side = max(cx1 - cx0, cy1 - cy0) + 1
    side = int(side * 1.06) + 2
    mx = (cx0 + cx1) // 2 - side // 2; my = (cy0 + cy1) // 2 - side // 2
    pad = np.zeros((side, side), bool); H = np.full((side, side), np.nan)
    sy0, sx0 = max(0, my), max(0, mx)
    sy1, sx1 = min(ny, my + side), min(nx, mx + side)
    pad[sy0 - my:sy1 - my, sx0 - mx:sx1 - mx] = mask[sy0:sy1, sx0:sx1]
    H[sy0 - my:sy1 - my, sx0 - mx:sx1 - mx] = low[sy0:sy1, sx0:sx1]
    # world rectangle of the image
    wx0 = x0 + mx * res; wy0 = y0 + my * res; wside = side * res
    if upp:                                      # a fixed density (the zoomed in-game minimap)
        size = int(min(size, max(512, math.ceil(wside / upp))))
    # height shade
    hv = H[pad]
    lo, hi = np.percentile(hv, 2), np.percentile(hv, 98)
    # tone = height above the surrounding ground (box mean over ~256 u): terrain stays flat,
    # crates / platforms / raised walkways come out lighter
    def boxsum(a, r):
        c = np.cumsum(np.cumsum(np.pad(a, ((1, 0), (1, 0))), 0), 1)
        n_ = a.shape[0]; m_ = a.shape[1]
        y0_ = np.clip(np.arange(n_) - r, 0, n_); y1_ = np.clip(np.arange(n_) + r + 1, 0, n_)
        x0_ = np.clip(np.arange(m_) - r, 0, m_); x1_ = np.clip(np.arange(m_) + r + 1, 0, m_)
        return (c[y1_][:, x1_] - c[y0_][:, x1_] - c[y1_][:, x0_] + c[y0_][:, x0_])
    r_ = max(4, int(256 / res))
    Hz = np.nan_to_num(H, nan=0.0); W = pad.astype(np.float64)
    ref = boxsum(Hz * W, r_) / np.maximum(boxsum(W, r_), 1)
    g = np.clip((Hz - ref - 20.0) / 60.0, 0, 1) * pad
    # supersampled layers, then area-downsample to `size`
    S = size * 2
    def up(a, mode=Image.NEAREST):
        return np.asarray(Image.fromarray(a).resize((S, S), mode))
    m8 = (pad * 255).astype(np.uint8)
    msk = up(m8, Image.BILINEAR) > 127                                  # smoothed outline
    gry = up((g * 255).astype(np.uint8), Image.BILINEAR).astype(np.float32) / 255.0
    edge = msk & ~(np.asarray(Image.fromarray((msk * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(3))) > 0)
    ek = 5
    if edge_units:                               # outline width in world units, not pixels
        ek = max(3, int(round(edge_units / (wside / S))) | 1)
    edge = np.asarray(Image.fromarray((edge * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(ek))) > 0
    # non-playable areas enclosed by the playable ones (buildings, blocks): dark, like a CS radar
    outside = Image.fromarray(np.where(msk, 255, 0).astype(np.uint8)).copy()
    ImageDraw.floodfill(outside, (0, 0), 128)
    hole = np.asarray(outside) == 0
    rgba = np.zeros((S, S, 4), np.float32)
    base = 36 + gry * 58
    rgba[..., 0] = np.where(msk, base, 0); rgba[..., 1] = np.where(msk, base, 0); rgba[..., 2] = np.where(msk, base + 4, 0)
    rgba[..., 3] = np.where(msk, 225, 0)
    rgba[hole] = (12, 12, 14, 200)
    rgba[edge] = (240, 240, 240, 255)
    img = Image.fromarray(np.flipud(rgba).clip(0, 255).astype(np.uint8), 'RGBA')   # +Y up
    img = img.resize((size, size), Image.LANCZOS)
    # bomb sites
    dr = ImageDraw.Draw(img)
    try: font = ImageFont.truetype('arialbd.ttf', int(size * 0.034))
    except Exception: font = ImageFont.load_default()
    placed = []
    for lab_, mdl, org in sites:
        px_, py_ = org[0], org[1]                     # brush model bounds are relative to its origin
        if mdl.startswith('*') and int(mdl[1:]) < len(models):
            mm = models[int(mdl[1:])]
            px_ += (mm[0] + mm[3]) / 2; py_ += (mm[1] + mm[4]) / 2
        u = (px_ - wx0) / wside * size; v = (1 - (py_ - wy0) / wside) * size
        r = size * 0.026
        if labels:
            dr.ellipse([u - r - 2, v - r - 2, u + r + 2, v + r + 2], fill=(20, 20, 20, 255))
            dr.ellipse([u - r, v - r, u + r, v + r], fill=(222, 196, 140, 255), outline=(255, 255, 255, 255), width=2)
            dr.text((u, v + 1), lab_, fill=(25, 25, 25, 255), font=font, anchor='mm')
        placed.append({'site': lab_, 'x': round(px_, 1), 'y': round(py_, 1)})
    os.makedirs(outdir, exist_ok=True)
    img.save(os.path.join(outdir, name + '.png'))
    meta = {'map': name, 'size': size, 'world_x0': wx0, 'world_y0': wy0, 'world_size': wside,
            'units_per_pixel': wside / size, 'north_up': '+Y', 'floor_shade_range': [float(lo), float(hi)],
            'sites': placed}
    json.dump(meta, open(os.path.join(outdir, name + '.json'), 'w'), indent=1)
    return meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pk3'); ap.add_argument('bsp'); ap.add_argument('name')
    ap.add_argument('--res', type=float, default=4.0); ap.add_argument('--size', type=int, default=1024)
    ap.add_argument('--out', default='out')
    ap.add_argument('--spawn-margin', type=float, default=0, help='keep only what lies within the spawns/sites box + this margin')
    ap.add_argument('--upp', type=float, default=0, help='world units per pixel (the size becomes area / upp, at most --size)')
    ap.add_argument('--edge-units', type=float, default=0, help='outline width in world units (default: 5 supersampled pixels)')
    ap.add_argument('--no-sites', action='store_true', help='bomb sites in the json only, not drawn (the client draws them upright)')
    a = ap.parse_args()
    mats, planes, sf, si, brushes, models, ents, tris = load_bsp(a.pk3, a.bsp)
    bl = brush_list(mats, planes, sf, si, brushes, models, ents)
    sp, sites = spawns_and_sites(ents)
    w = models[0]
    pad = 2048
    x0 = max(w[0], sp[:, 0].min() - pad); x1 = min(w[3], sp[:, 0].max() + pad)
    y0 = max(w[1], sp[:, 1].min() - pad); y1 = min(w[4], sp[:, 1].max() + pad)
    zlo = max(w[2], sp[:, 2].min() - 1200); zhi = min(w[5], sp[:, 2].max() + 1600)
    nx = int((x1 - x0) / a.res) + 1; ny = int((y1 - y0) / a.res) + 1
    print(f'{a.name}: {len(bl)} blocking brushes, grid {nx}x{ny} @ {a.res}u, z {zlo:.0f}..{zhi:.0f}, '
          f'{len(sp)} spawns, {len(tris)} collision triangles, sites {[s[0] for s in sites]}', flush=True)
    nc, f, t = column_nodes(bl, tris, x0, y0, nx, ny, a.res, zlo, zhi)
    dz = brush_list(mats, planes, sf, si, brushes, models, ents, danger=True)
    if dz:
        dc, d0, d1, _ = brush_columns(dz, x0, y0, nx, ny, a.res, zlo, zhi)
        dc = np.concatenate(dc); d0 = np.concatenate(d0); d1 = np.concatenate(d1)
        o = np.argsort(dc); dc, d0, d1 = dc[o], d0[o], d1[o]
        lo_ = np.searchsorted(dc, nc); hi_ = np.searchsorted(dc, nc, side='right')
        kill = np.zeros(len(nc), bool)
        for k in np.flatnonzero(hi_ > lo_):
            kill[k] = np.any((f[k] >= d0[lo_[k]:hi_[k]] - 64) & (f[k] <= d1[lo_[k]:hi_[k]]))
        nc, f, t = nc[~kill], f[~kill], t[~kill]
        print(f'  {len(dz)} minefield / hurt brushes: {kill.sum()} standable places removed', flush=True)
    nc, f, t, lab = components(nc, f, t, nx, ny)
    # spawn -> the standable place it stands on
    seeds = []
    for o in sp:
        ix = int((o[0] - x0) / a.res); iy = int((o[1] - y0) / a.res)
        if not (0 <= ix < nx and 0 <= iy < ny): continue
        c = iy * nx + ix
        lo_, hi_ = np.searchsorted(nc, c), np.searchsorted(nc, c, side='right')
        best = -1
        for k in range(lo_, hi_):
            if f[k] <= o[2] + 24 and t[k] > o[2] and (best < 0 or f[k] > f[best]): best = k
        if best >= 0: seeds.append(lab[best])
    seeds = np.unique(seeds)
    print(f'  {len(nc)} standable places, {len(np.unique(lab))} components, {len(seeds)} hold spawns', flush=True)
    clip = None
    if a.spawn_margin > 0:
        pts = [p_[:2] for p_ in sp] + [s_[2][:2] for s_ in sites]
        pts = np.array(pts); mg = a.spawn_margin
        clip = (pts[:, 0].min() - mg, pts[:, 1].min() - mg, pts[:, 0].max() + mg, pts[:, 1].max() + mg)
    meta = render(a.name, nc, f, t, lab, seeds, nx, ny, x0, y0, a.res, sites, models, a.size, a.out, clip,
                  a.upp or None, a.edge_units or None, not a.no_sites)
    print('  ->', os.path.join(a.out, a.name + '.png'), json.dumps({k: meta[k] for k in ('world_x0', 'world_y0', 'world_size')}))


if __name__ == '__main__':
    main()
