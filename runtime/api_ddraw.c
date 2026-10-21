/* DirectDraw 方法实现（COM 虚表）。像素/Blt 等核心见 ddraw_core.inc。 */
#include "ddraw_core.inc"

#define METHOD(n) static void n(void)
#define HR(v, n) HOST_RET((uint32_t)(v), (n))
#define DDTRACE(...) do { if (g_trace_api) host_log(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- 通用 */
static int guid_eq(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, uint8_t b0, uint8_t b1) {
    return rt_rd32(g_mem, p) == d1 && rt_rd16(g_mem, p + 4) == d2 && rt_rd16(g_mem, p + 6) == d3 && g_mem[p + 8] == b0 && g_mem[p + 9] == b1;
}

static void write_pixfmt(Surf *s, uint32_t p) {
    memset(GP(p), 0, 32);
    rt_wr32(g_mem, p, 32);
    if (s->bpp == 8) rt_wr32(g_mem, p + 4, 0x60);        /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    else rt_wr32(g_mem, p + 4, 0x40);                     /* DDPF_RGB */
    rt_wr32(g_mem, p + 12, (uint32_t)s->bpp);
    rt_wr32(g_mem, p + 16, s->rmask); rt_wr32(g_mem, p + 20, s->gmask); rt_wr32(g_mem, p + 24, s->bmask);
}

static void fill_desc(Surf *s, uint32_t d, int with_ptr, uint32_t ptr) {
    memset(GP(d), 0, 108);
    rt_wr32(g_mem, d, 108);
    uint32_t fl = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    if (with_ptr) fl |= DDSD_LPSURFACE;
    rt_wr32(g_mem, d + 4, fl);
    rt_wr32(g_mem, d + 8, (uint32_t)s->h);
    rt_wr32(g_mem, d + 12, (uint32_t)s->w);
    rt_wr32(g_mem, d + 16, (uint32_t)s->pitch);
    if (with_ptr) rt_wr32(g_mem, d + 36, ptr);
    write_pixfmt(s, d + 72);
    rt_wr32(g_mem, d + 104, s->caps);
}

static void make_mode_desc(uint32_t d, int w, int h, int bpp) {
    memset(GP(d), 0, 108);
    rt_wr32(g_mem, d, 108);
    rt_wr32(g_mem, d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
    rt_wr32(g_mem, d + 8, (uint32_t)h); rt_wr32(g_mem, d + 12, (uint32_t)w);
    rt_wr32(g_mem, d + 16, (uint32_t)(w * bpp / 8));
    Surf tmp; memset(&tmp, 0, sizeof tmp); set_masks(&tmp, bpp);
    write_pixfmt(&tmp, d + 72);
}

/* ---------------------------------------------------------------- IDirectDraw */
METHOD(dd_QueryInterface) {
    uint32_t riid = ARG(1), out = ARG(2);
    if (guid_eq(riid, 0x6C14DB80, 0xA733, 0x11CE, 0xA5, 0x21)) {            /* IDirectDraw */
        uint32_t o = new_obj(vt_dd, 0); rt_wr32(g_mem, o + 8, 1); rt_wr32(g_mem, out, o); dd.refs++;
        HR(DD_OK, 3);
    } else if (guid_eq(riid, 0xB3A6F3E0, 0x2B43, 0x11CF, 0xA2, 0xDE)) {     /* IDirectDraw2 */
        extern uint32_t g_vt_dd2;
        uint32_t o = new_obj(g_vt_dd2, 0); rt_wr32(g_mem, o + 8, 2); rt_wr32(g_mem, out, o); dd.refs++;
        HR(DD_OK, 3);
    } else {
        DDTRACE("[ddraw] IDirectDraw::QueryInterface 不支持的 IID %08X", rt_rd32(g_mem, riid));
        rt_wr32(g_mem, out, 0);
        HR(E_NOINTERFACE, 3);
    }
}
uint32_t g_vt_dd2;
METHOD(dd_AddRef) { dd.refs++; HR(dd.refs, 1); }
METHOD(dd_Release) { if (dd.refs > 0) dd.refs--; HR(dd.refs, 1); }
METHOD(dd_Compact) { HR(DD_OK, 1); }

METHOD(dd_CreateClipper) {
    for (int i = 0; i < MAX_CLIPPERS; i++) {
        if (!clippers[i].used) {
            clippers[i].used = 1; clippers[i].refs = 1;
            clippers[i].guest = new_obj(vt_clip, i);
            rt_wr32(g_mem, ARG(2), clippers[i].guest);
            HR(DD_OK, 4);
            return;
        }
    }
    HR(DDERR_GENERIC, 4);
}

METHOD(dd_CreatePalette) {
    uint32_t flags = ARG(1), entries = ARG(2), out = ARG(3);
    for (int i = 0; i < MAX_PALS; i++) {
        if (!pals[i].used) {
            Pal *p = &pals[i];
            memset(p, 0, sizeof *p);
            p->used = 1; p->refs = 1;
            p->n = (flags & 4) ? 256 : (flags & 2) ? 16 : (flags & 1) ? 4 : (flags & 8) ? 2 : 256;
            memcpy(p->c, gdi_system_palette(), sizeof p->c);
            if (entries) for (int k = 0; k < p->n; k++) {
                const uint8_t *e = g_mem + entries + k * 4;
                p->c[k] = (uint32_t)(e[0] << 16 | e[1] << 8 | e[2]);
            }
            p->guest = new_obj(vt_pal, i);
            rt_wr32(g_mem, out, p->guest);
            HR(DD_OK, 5);
            return;
        }
    }
    HR(DDERR_GENERIC, 5);
}

static void make_back_buffers(Surf *p, int count) {
    int prev = surf_idx(p);
    for (int k = 0; k < count; k++) {
        int b = surf_alloc(p->w, p->h, p->bpp, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY);
        if (b < 0) return;
        surfs[prev].attached_back = b;
        surfs[b].attached_front = prev;
        prev = b;
    }
}

METHOD(dd_CreateSurface) {
    uint32_t desc = ARG(1), out = ARG(2);
    uint32_t flags = rt_rd32(g_mem, desc + 4);
    uint32_t caps = (flags & DDSD_CAPS) ? rt_rd32(g_mem, desc + 104) : 0;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int w = ddraw_display_width(), h = ddraw_display_height(), bpp = ddraw_display_bpp();
        int idx = surf_alloc(w, h, bpp, DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY | DDSCAPS_FRONTBUFFER);
        if (idx < 0) { HR(DDERR_GENERIC, 4); return; }
        Surf *s = &surfs[idx];
        s->is_primary = 1;
        if ((flags & DDSD_BACKBUFFERCOUNT) && rt_rd32(g_mem, desc + 20) > 0) {
            s->caps |= DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            make_back_buffers(s, (int)rt_rd32(g_mem, desc + 20));
        }
        primary_idx = idx;
        ensure_window();
        update_window_target();
        DDTRACE("[ddraw] 创建主表面 #%d obj=%08X %dx%dx%d 后备缓冲=%d(#%d)", idx, s->guest, w, h, bpp, s->attached_back >= 0, s->attached_back);
        rt_wr32(g_mem, out, s->guest);
        HR(DD_OK, 4);
        return;
    }
    int w = (flags & DDSD_WIDTH) ? (int)rt_rd32(g_mem, desc + 12) : 0;
    int h = (flags & DDSD_HEIGHT) ? (int)rt_rd32(g_mem, desc + 8) : 0;
    int bpp = ddraw_display_bpp();
    if (flags & DDSD_PIXELFORMAT) {
        uint32_t pf = desc + 72;
        uint32_t pff = rt_rd32(g_mem, pf + 4);
        if (pff & 0x20) bpp = 8;
        else if (pff & 0x40) bpp = (int)rt_rd32(g_mem, pf + 12);
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { DDTRACE("[ddraw] CreateSurface 非法尺寸 %dx%d", w, h); HR(DDERR_INVALIDPARAMS, 4); return; }
    int idx = surf_alloc(w, h, bpp, caps | ((caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY)) ? 0 : DDSCAPS_VIDEOMEMORY));
    if (idx < 0) { HR(DDERR_GENERIC, 4); return; }
    if ((flags & DDSD_PIXELFORMAT) && bpp == 16) {
        uint32_t pf = desc + 72;
        uint32_t rm = rt_rd32(g_mem, pf + 16);
        if (rm == 0x7C00) { surfs[idx].rmask = 0x7C00; surfs[idx].gmask = 0x03E0; surfs[idx].bmask = 0x001F; }
    }
    DDTRACE("[ddraw] 创建离屏表面 #%d %dx%dx%d caps=%08X", idx, w, h, bpp, caps);
    rt_wr32(g_mem, out, surfs[idx].guest);
    HR(DD_OK, 4);
}

METHOD(dd_EnumDisplayModes) {
    uint32_t cb = ARG(4), ctx = ARG(3);
    static const int modes[][2] = {{320, 200}, {320, 240}, {640, 400}, {640, 480}, {800, 600}, {1024, 768}};
    static const int depths[] = {8, 16, 32};
    uint32_t d = gm_calloc(128);
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++)
        for (size_t k = 0; k < sizeof depths / sizeof depths[0]; k++) {
            make_mode_desc(d, modes[m][0], modes[m][1], depths[k]);
            uint32_t args[2] = { d, ctx };
            uint32_t r = rt_guest_call(cb, 2, args);
            if (!r) { gm_free(d); HR(DD_OK, 5); return; }
        }
    gm_free(d);
    HR(DD_OK, 5);
}

METHOD(dd_FlipToGDISurface) { HR(DD_OK, 1); }

METHOD(dd_GetCaps) {
    uint32_t drv = ARG(1), hel = ARG(2);
    for (int k = 0; k < 2; k++) {
        uint32_t p = k ? hel : drv;
        if (!p) continue;
        uint32_t sz = rt_rd32(g_mem, p);
        if (sz < 64 || sz > 1024) sz = 316;
        memset(GP(p), 0, sz);
        rt_wr32(g_mem, p, sz);
        if (k == 0) {
            rt_wr32(g_mem, p + 4, 0x40u | 0x400000u | 0x8000000u | 0x4000000u | 0x80000000u | 0x20000000u);   /* BLT COLORKEY PALETTE BLTCOLORFILL CANBLTSYSMEM */
            rt_wr32(g_mem, p + 12, 0x8u | 0x2u);                  /* CKEYCAPS: SRCBLT DESTBLT */
            rt_wr32(g_mem, p + 24, 0x4u);                          /* PALCAPS 8BIT */
            rt_wr32(g_mem, p + 60, 64u * 1024 * 1024);            /* VidMemTotal */
            rt_wr32(g_mem, p + 64, 60u * 1024 * 1024);            /* VidMemFree */
        } else {
            rt_wr32(g_mem, p + 4, 0x40u | 0x400000u | 0x8000000u | 0x4000000u);
        }
    }
    HR(DD_OK, 3);
}

METHOD(dd_GetDisplayMode) {
    make_mode_desc(ARG(1), ddraw_display_width(), ddraw_display_height(), ddraw_display_bpp());
    HR(DD_OK, 2);
}
METHOD(dd_GetFourCCCodes) { if (ARG(1)) rt_wr32(g_mem, ARG(1), 0); HR(DD_OK, 3); }
METHOD(dd_GetGDISurface) {
    if (primary_idx < 0) { HR(DDERR_NOTFOUND, 2); return; }
    surfs[primary_idx].refs++;
    rt_wr32(g_mem, ARG(1), surfs[primary_idx].guest);
    HR(DD_OK, 2);
}
METHOD(dd_GetMonitorFrequency) { rt_wr32(g_mem, ARG(1), 60); HR(DD_OK, 2); }
METHOD(dd_GetScanLine) { rt_wr32(g_mem, ARG(1), 0); HR(DD_OK, 2); }
METHOD(dd_GetVerticalBlankStatus) { rt_wr32(g_mem, ARG(1), 1); HR(DD_OK, 2); }
METHOD(dd_Initialize) { HR(DD_OK, 2); }
METHOD(dd_RestoreDisplayMode) { HR(DD_OK, 1); }
METHOD(dd_SetCooperativeLevel) {
    dd.hwnd = ARG(1); dd.coop = ARG(2);
    DDTRACE("[ddraw] SetCooperativeLevel hwnd=%08X flags=%08X", dd.hwnd, dd.coop);
    HR(DD_OK, 3);
}
static void set_mode(int w, int h, int bpp) {
    dd.w = w; dd.h = h; dd.bpp = bpp; dd.in_mode = 1;
    DDTRACE("[ddraw] SetDisplayMode %dx%dx%d", w, h, bpp);
    ensure_window();
}
METHOD(dd_SetDisplayMode1) { set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3)); HR(DD_OK, 4); }
METHOD(dd_SetDisplayMode2) { set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3)); HR(DD_OK, 6); }
METHOD(dd_WaitForVerticalBlank) {
    /* 以约 60Hz 节拍阻塞，同时刷新画面 */
    static uint64_t next;
    uint64_t now = plat_ticks_ms();
    if (dirty) ddraw_present();
    if (next > now && next - now < 40) plat_sleep_ms((uint32_t)(next - now));
    next = plat_ticks_ms() + 16;
    HR(DD_OK, 3);
}
METHOD(dd_GetAvailableVidMem) {
    if (ARG(2)) rt_wr32(g_mem, ARG(2), 64u * 1024 * 1024);
    if (ARG(3)) rt_wr32(g_mem, ARG(3), 60u * 1024 * 1024);
    HR(DD_OK, 4);
}

/* ---------------------------------------------------------------- IDirectDrawSurface */
METHOD(sf_QueryInterface) {
    Surf *s = surf_of(ARG(0));
    uint32_t riid = ARG(1), out = ARG(2);
    if (s && (guid_eq(riid, 0x6C14DB81, 0xA733, 0x11CE, 0xA5, 0x21) || guid_eq(riid, 0x57805885, 0x6EEC, 0x11CF, 0x94, 0x41) ||
              guid_eq(riid, 0xDA044E00, 0x69B2, 0x11D0, 0xA1, 0xD5))) {
        s->refs++;
        rt_wr32(g_mem, out, s->guest);
        HR(DD_OK, 3);
        return;
    }
    DDTRACE("[ddraw] Surface::QueryInterface 不支持的 IID %08X", rt_rd32(g_mem, riid));
    rt_wr32(g_mem, out, 0);
    HR(E_NOINTERFACE, 3);
}
METHOD(sf_AddRef) { Surf *s = surf_of(ARG(0)); if (s) s->refs++; HR(s ? s->refs : 0, 1); }
METHOD(sf_Release) {
    Surf *s = surf_of(ARG(0));
    if (!s) { HR(0, 1); return; }
    if (--s->refs <= 0) {
        int idx = surf_idx(s);
        if (idx == primary_idx) { primary_idx = -1; update_window_target(); }
        if (s->attached_front >= 0 && surfs[s->attached_front].used) surfs[s->attached_front].attached_back = -1;
        if (s->attached_back >= 0 && surfs[s->attached_back].used) { surfs[s->attached_back].attached_front = -1; if (--surfs[s->attached_back].refs <= 0) { gm_free(surfs[s->attached_back].pixels); gm_free(surfs[s->attached_back].guest); surfs[s->attached_back].used = 0; } }
        gm_free(s->pixels);
        gm_free(s->guest);
        s->used = 0;
        HR(0, 1);
        return;
    }
    HR(s->refs, 1);
}
METHOD(sf_AddAttachedSurface) {
    Surf *s = surf_of(ARG(0)), *o = surf_of(ARG(1));
    if (s && o) { s->attached_back = surf_idx(o); o->attached_front = surf_idx(s); o->refs++; }
    HR(DD_OK, 2);
}

static void clip_src_ck(Surf *s, uint32_t flags, uint32_t fx, int *use, uint32_t *lo, uint32_t *hi) {
    *use = 0;
    if (!s) return;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { *use = 1; *lo = rt_rd32(g_mem, fx + 92); *hi = rt_rd32(g_mem, fx + 96); }
    else if ((flags & DDBLT_KEYSRC) && s->has_src_ck) { *use = 1; *lo = s->src_ck_lo; *hi = s->src_ck_hi; }
}

METHOD(sf_Blt) {
    /* Blt(this, lpDestRect, lpSrcSurface, lpSrcRect, dwFlags, lpBltFx) */
    Surf *d = surf_of(ARG(0)), *s = surf_of(ARG(2));
    uint32_t flags = ARG(4), fx = ARG(5);
    if (!d) { HR(DDERR_INVALIDPARAMS, 6); return; }
    Rc dr, sr;
    read_rect(ARG(1), &dr, d->w, d->h);
    if (flags & DDBLT_COLORFILL) {
        do_blt(d, dr, NULL, dr, flags, 0, 0, fx ? rt_rd32(g_mem, fx + 80) : 0, 1, 0, 0, 0, 0, 0, 0);
    } else if (s) {
        read_rect(ARG(3), &sr, s->w, s->h);
        int use_sck = 0, use_dck = 0;
        uint32_t slo = 0, shi = 0, dlo = 0, dhi = 0;
        clip_src_ck(s, flags, fx, &use_sck, &slo, &shi);
        if ((flags & DDBLT_KEYDEST) && d->has_dst_ck) { use_dck = 1; dlo = d->dst_ck_lo; dhi = d->dst_ck_hi; }
        int mlr = 0, mud = 0;
        if ((flags & DDBLT_DDFX) && fx) { uint32_t fxf = rt_rd32(g_mem, fx + 4); mlr = (fxf & 2) != 0; mud = (fxf & 4) != 0; }
        do_blt(d, dr, s, sr, flags, mlr, mud, 0, 0, use_sck, use_dck, slo, shi, dlo, dhi);
    }
    if (d->is_primary) { dirty = 1; }
    HR(DD_OK, 6);
}

METHOD(sf_BltFast) {
    Surf *d = surf_of(ARG(0)), *s = surf_of(ARG(3));
    int x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    uint32_t trans = ARG(5);
    if (!d || !s) { HR(DDERR_INVALIDPARAMS, 6); return; }
    Rc sr; read_rect(ARG(4), &sr, s->w, s->h);
    Rc dr = { x, y, x + (sr.r - sr.l), y + (sr.b - sr.t) };
    int use_sck = (trans & 1) && s->has_src_ck, use_dck = (trans & 2) && d->has_dst_ck;
    do_blt(d, dr, s, sr, 0, 0, 0, 0, 0, use_sck, use_dck, s->src_ck_lo, s->src_ck_hi, d->dst_ck_lo, d->dst_ck_hi);
    if (d->is_primary) dirty = 1;
    HR(DD_OK, 6);
}

METHOD(sf_DeleteAttachedSurface) { HR(DD_OK, 3); }
METHOD(sf_EnumAttachedSurfaces) {
    Surf *s = surf_of(ARG(0));
    uint32_t ctx = ARG(1), cb = ARG(2);
    if (s && s->attached_back >= 0) {
        Surf *b = &surfs[s->attached_back];
        uint32_t d = gm_calloc(128);
        fill_desc(b, d, 0, 0);
        b->refs++;
        uint32_t args[3] = { b->guest, d, ctx };
        rt_guest_call(cb, 3, args);
        gm_free(d);
    }
    HR(DD_OK, 3);
}

METHOD(sf_Flip) {
    Surf *s = surf_of(ARG(0));
    if (!s) { HR(DDERR_INVALIDPARAMS, 3); return; }
    if (s->attached_back >= 0) {
        Surf *b = &surfs[s->attached_back];
        uint32_t t = s->pixels; s->pixels = b->pixels; b->pixels = t;
        update_window_target();
    }
    DDTRACE("[ddraw] Flip #%d primary=%d back=%d", surf_idx(s), s->is_primary, s->attached_back);
    if (s->is_primary) { dirty = 1; ddraw_present_if_dirty(); }
    HR(DD_OK, 3);
}

METHOD(sf_GetAttachedSurface) {
    Surf *s = surf_of(ARG(0));
    uint32_t capsp = ARG(1), out = ARG(2);
    uint32_t caps = rt_rd32(g_mem, capsp);
    if (s && (caps & DDSCAPS_BACKBUFFER) && s->attached_back >= 0) {
        Surf *b = &surfs[s->attached_back];
        b->refs++;
        rt_wr32(g_mem, out, b->guest);
        HR(DD_OK, 3);
        return;
    }
    DDTRACE("[ddraw] GetAttachedSurface 未找到 caps=%08X", caps);
    rt_wr32(g_mem, out, 0);
    HR(DDERR_NOTFOUND, 3);
}

METHOD(sf_OK2) { HR(DD_OK, 2); }
METHOD(sf_OK1) { HR(DD_OK, 1); }
METHOD(sf_OK3) { HR(DD_OK, 3); }
METHOD(sf_GetCaps) { Surf *s = surf_of(ARG(0)); if (s) rt_wr32(g_mem, ARG(1), s->caps); HR(DD_OK, 2); }
METHOD(sf_GetClipper) { HR(DDERR_NOTFOUND, 2); }
METHOD(sf_SetClipper) { HR(DD_OK, 2); }

METHOD(sf_GetColorKey) {
    Surf *s = surf_of(ARG(0));
    uint32_t flags = ARG(1), out = ARG(2);
    if (!s) { HR(DDERR_INVALIDPARAMS, 3); return; }
    if ((flags & DDCKEY_SRCBLT) && s->has_src_ck) { rt_wr32(g_mem, out, s->src_ck_lo); rt_wr32(g_mem, out + 4, s->src_ck_hi); HR(DD_OK, 3); return; }
    if ((flags & DDCKEY_DESTBLT) && s->has_dst_ck) { rt_wr32(g_mem, out, s->dst_ck_lo); rt_wr32(g_mem, out + 4, s->dst_ck_hi); HR(DD_OK, 3); return; }
    HR(0x8876016Au, 3);   /* DDERR_NOCOLORKEY */
}
METHOD(sf_SetColorKey) {
    Surf *s = surf_of(ARG(0));
    uint32_t flags = ARG(1), ck = ARG(2);
    if (!s) { HR(DDERR_INVALIDPARAMS, 3); return; }
    uint32_t lo = ck ? rt_rd32(g_mem, ck) : 0, hi = ck ? rt_rd32(g_mem, ck + 4) : 0;
    if (flags & DDCKEY_SRCBLT) { s->has_src_ck = ck != 0; s->src_ck_lo = lo; s->src_ck_hi = hi; }
    if (flags & DDCKEY_DESTBLT) { s->has_dst_ck = ck != 0; s->dst_ck_lo = lo; s->dst_ck_hi = hi; }
    HR(DD_OK, 3);
}

METHOD(sf_GetDC) {
    Surf *s = surf_of(ARG(0));
    if (!s) { HR(DDERR_INVALIDPARAMS, 2); return; }
    GdiTarget t;
    memset(&t, 0, sizeof t);
    t.bits = g_mem + s->pixels; t.w = s->w; t.h = s->h; t.pitch = s->pitch; t.bpp = s->bpp;
    t.rmask = s->rmask; t.gmask = s->gmask; t.bmask = s->bmask;
    t.pal = s->bpp == 8 ? surf_palette_rgb(s) : NULL;
    s->dc = gdi_new_dc(&t);
    rt_wr32(g_mem, ARG(1), s->dc);
    HR(DD_OK, 2);
}
METHOD(sf_ReleaseDC) {
    Surf *s = surf_of(ARG(0));
    gdi_free_dc(ARG(1));
    if (s) { s->dc = 0; if (s->is_primary) dirty = 1; }
    HR(DD_OK, 2);
}

METHOD(sf_GetPalette) {
    Surf *s = surf_of(ARG(0));
    if (s && s->pal >= 0 && pals[s->pal].used) { pals[s->pal].refs++; rt_wr32(g_mem, ARG(1), pals[s->pal].guest); HR(DD_OK, 2); return; }
    rt_wr32(g_mem, ARG(1), 0);
    HR(DDERR_NOPALETTEATTACHED, 2);
}
METHOD(sf_SetPalette) {
    Surf *s = surf_of(ARG(0));
    Pal *p = pal_of(ARG(1));
    if (s) {
        s->pal = p ? (int)(p - pals) : -1;
        if (s->is_primary) { dirty = 1; update_window_target(); }
    }
    HR(DD_OK, 2);
}
METHOD(sf_GetPixelFormat) { Surf *s = surf_of(ARG(0)); if (s) write_pixfmt(s, ARG(1)); HR(DD_OK, 2); }
METHOD(sf_GetSurfaceDesc) { Surf *s = surf_of(ARG(0)); if (s) fill_desc(s, ARG(1), 0, 0); HR(DD_OK, 2); }

METHOD(sf_Lock) {
    Surf *s = surf_of(ARG(0));
    uint32_t rect = ARG(1), desc = ARG(2);
    if (!s) { HR(DDERR_INVALIDPARAMS, 5); return; }
    uint32_t ptr = s->pixels;
    if (rect) {
        int l = (int32_t)rt_rd32(g_mem, rect), t = (int32_t)rt_rd32(g_mem, rect + 4);
        ptr += (uint32_t)(t * s->pitch + l * (s->bpp / 8));
    }
    fill_desc(s, desc, 1, ptr);
    s->locked = 1;
    HR(DD_OK, 5);
}
METHOD(sf_Unlock) {
    Surf *s = surf_of(ARG(0));
    if (s) {
        s->locked = 0; if (s->is_primary) dirty = 1;
        if (g_trace_api) {
            size_t nz = 0, n = (size_t)s->pitch * s->h;
            for (size_t i = 0; i < n; i++) nz += g_mem[s->pixels + i] != 0;
            host_log("[ddraw] Unlock #%d %dx%d 非零字节=%zu/%zu", surf_idx(s), s->w, s->h, nz, n);
        }
    }
    HR(DD_OK, 2);
}
METHOD(sf_IsLost) { HR(DD_OK, 1); }
METHOD(sf_Restore) { HR(DD_OK, 1); }

/* ---------------------------------------------------------------- IDirectDrawPalette / Clipper */
METHOD(pl_QueryInterface) { rt_wr32(g_mem, ARG(2), 0); HR(E_NOINTERFACE, 3); }
METHOD(pl_AddRef) { Pal *p = pal_of(ARG(0)); if (p) p->refs++; HR(p ? p->refs : 0, 1); }
METHOD(pl_Release) {
    Pal *p = pal_of(ARG(0));
    if (!p) { HR(0, 1); return; }
    if (--p->refs <= 0) {
        for (int i = 0; i < MAX_SURFS; i++) if (surfs[i].used && surfs[i].pal == (int)(p - pals)) surfs[i].pal = -1;
        gm_free(p->guest);
        p->used = 0;
        HR(0, 1);
        return;
    }
    HR(p->refs, 1);
}
METHOD(pl_GetCaps) { rt_wr32(g_mem, ARG(1), 4); HR(DD_OK, 2); }
METHOD(pl_GetEntries) {
    Pal *p = pal_of(ARG(0));
    uint32_t start = ARG(2), count = ARG(3), out = ARG(4);
    if (!p) { HR(DDERR_INVALIDPARAMS, 5); return; }
    for (uint32_t i = 0; i < count && start + i < 256; i++) {
        uint32_t c = p->c[start + i];
        g_mem[out + i * 4] = (uint8_t)(c >> 16); g_mem[out + i * 4 + 1] = (uint8_t)(c >> 8); g_mem[out + i * 4 + 2] = (uint8_t)c; g_mem[out + i * 4 + 3] = 0;
    }
    HR(DD_OK, 5);
}
METHOD(pl_SetEntries) {
    Pal *p = pal_of(ARG(0));
    uint32_t start = ARG(2), count = ARG(3), in = ARG(4);
    if (!p) { HR(DDERR_INVALIDPARAMS, 5); return; }
    for (uint32_t i = 0; i < count && start + i < 256; i++) {
        const uint8_t *e = g_mem + in + i * 4;
        p->c[start + i] = (uint32_t)(e[0] << 16 | e[1] << 8 | e[2]);
    }
    if (primary_idx >= 0 && surfs[primary_idx].pal == (int)(p - pals)) dirty = 1;
    HR(DD_OK, 5);
}
METHOD(pl_Initialize) { HR(DD_OK, 4); }

METHOD(cl_QueryInterface) { rt_wr32(g_mem, ARG(2), 0); HR(E_NOINTERFACE, 3); }
static Clipper *clip_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t i = rt_rd32(g_mem, obj + 4);
    return (i < MAX_CLIPPERS && clippers[i].used) ? &clippers[i] : NULL;
}
METHOD(cl_AddRef) { Clipper *c = clip_of(ARG(0)); if (c) c->refs++; HR(c ? c->refs : 0, 1); }
METHOD(cl_Release) {
    Clipper *c = clip_of(ARG(0));
    if (!c) { HR(0, 1); return; }
    if (--c->refs <= 0) { gm_free(c->guest); c->used = 0; HR(0, 1); return; }
    HR(c->refs, 1);
}
METHOD(cl_Ok3) { HR(DD_OK, 3); }
METHOD(cl_Ok4) { HR(DD_OK, 4); }
METHOD(cl_Ok2) { HR(DD_OK, 2); }

/* ---------------------------------------------------------------- 虚表与入口 */
typedef struct { const char *name; RtHostFn fn; } VM;

static uint32_t make_vtbl(const VM *m, int n) {
    uint32_t v = gm_calloc((uint32_t)n * 4);
    for (int i = 0; i < n; i++) rt_wr32(g_mem, v + (uint32_t)i * 4, rt_register_thunk(m[i].name, m[i].fn));
    return v;
}

static void build_vtables(void) {
    static const VM ddv[] = {
        {"IDirectDraw::QueryInterface", dd_QueryInterface}, {"IDirectDraw::AddRef", dd_AddRef}, {"IDirectDraw::Release", dd_Release},
        {"IDirectDraw::Compact", dd_Compact}, {"IDirectDraw::CreateClipper", dd_CreateClipper}, {"IDirectDraw::CreatePalette", dd_CreatePalette},
        {"IDirectDraw::CreateSurface", dd_CreateSurface}, {"IDirectDraw::DuplicateSurface", NULL}, {"IDirectDraw::EnumDisplayModes", dd_EnumDisplayModes},
        {"IDirectDraw::EnumSurfaces", NULL}, {"IDirectDraw::FlipToGDISurface", dd_FlipToGDISurface}, {"IDirectDraw::GetCaps", dd_GetCaps},
        {"IDirectDraw::GetDisplayMode", dd_GetDisplayMode}, {"IDirectDraw::GetFourCCCodes", dd_GetFourCCCodes}, {"IDirectDraw::GetGDISurface", dd_GetGDISurface},
        {"IDirectDraw::GetMonitorFrequency", dd_GetMonitorFrequency}, {"IDirectDraw::GetScanLine", dd_GetScanLine},
        {"IDirectDraw::GetVerticalBlankStatus", dd_GetVerticalBlankStatus}, {"IDirectDraw::Initialize", dd_Initialize},
        {"IDirectDraw::RestoreDisplayMode", dd_RestoreDisplayMode}, {"IDirectDraw::SetCooperativeLevel", dd_SetCooperativeLevel},
        {"IDirectDraw::SetDisplayMode", dd_SetDisplayMode1}, {"IDirectDraw::WaitForVerticalBlank", dd_WaitForVerticalBlank},
    };
    vt_dd = make_vtbl(ddv, (int)(sizeof ddv / sizeof ddv[0]));
    VM dd2v[24];
    memcpy(dd2v, ddv, sizeof ddv);
    dd2v[21].name = "IDirectDraw2::SetDisplayMode"; dd2v[21].fn = dd_SetDisplayMode2;
    dd2v[23].name = "IDirectDraw2::GetAvailableVidMem"; dd2v[23].fn = dd_GetAvailableVidMem;
    /* 索引 22 为 WaitForVerticalBlank，23 为 GetAvailableVidMem */
    dd2v[22] = ddv[22];
    g_vt_dd2 = make_vtbl(dd2v, 24);

    static const VM sfv[] = {
        {"IDirectDrawSurface::QueryInterface", sf_QueryInterface}, {"IDirectDrawSurface::AddRef", sf_AddRef}, {"IDirectDrawSurface::Release", sf_Release},
        {"IDirectDrawSurface::AddAttachedSurface", sf_AddAttachedSurface}, {"IDirectDrawSurface::AddOverlayDirtyRect", NULL},
        {"IDirectDrawSurface::Blt", sf_Blt}, {"IDirectDrawSurface::BltBatch", NULL}, {"IDirectDrawSurface::BltFast", sf_BltFast},
        {"IDirectDrawSurface::DeleteAttachedSurface", sf_DeleteAttachedSurface}, {"IDirectDrawSurface::EnumAttachedSurfaces", sf_EnumAttachedSurfaces},
        {"IDirectDrawSurface::EnumOverlayZOrders", NULL}, {"IDirectDrawSurface::Flip", sf_Flip}, {"IDirectDrawSurface::GetAttachedSurface", sf_GetAttachedSurface},
        {"IDirectDrawSurface::GetBltStatus", sf_OK2}, {"IDirectDrawSurface::GetCaps", sf_GetCaps}, {"IDirectDrawSurface::GetClipper", sf_GetClipper},
        {"IDirectDrawSurface::GetColorKey", sf_GetColorKey}, {"IDirectDrawSurface::GetDC", sf_GetDC}, {"IDirectDrawSurface::GetFlipStatus", sf_OK2},
        {"IDirectDrawSurface::GetOverlayPosition", NULL}, {"IDirectDrawSurface::GetPalette", sf_GetPalette}, {"IDirectDrawSurface::GetPixelFormat", sf_GetPixelFormat},
        {"IDirectDrawSurface::GetSurfaceDesc", sf_GetSurfaceDesc}, {"IDirectDrawSurface::Initialize", NULL}, {"IDirectDrawSurface::IsLost", sf_IsLost},
        {"IDirectDrawSurface::Lock", sf_Lock}, {"IDirectDrawSurface::ReleaseDC", sf_ReleaseDC}, {"IDirectDrawSurface::Restore", sf_Restore},
        {"IDirectDrawSurface::SetClipper", sf_SetClipper}, {"IDirectDrawSurface::SetColorKey", sf_SetColorKey}, {"IDirectDrawSurface::SetOverlayPosition", NULL},
        {"IDirectDrawSurface::SetPalette", sf_SetPalette}, {"IDirectDrawSurface::Unlock", sf_Unlock}, {"IDirectDrawSurface::UpdateOverlay", NULL},
        {"IDirectDrawSurface::UpdateOverlayDisplay", NULL}, {"IDirectDrawSurface::UpdateOverlayZOrder", NULL},
        {"IDirectDrawSurface2::GetDDInterface", NULL}, {"IDirectDrawSurface2::PageLock", sf_OK2}, {"IDirectDrawSurface2::PageUnlock", sf_OK2},
        {"IDirectDrawSurface3::SetSurfaceDesc", NULL},
    };
    vt_surf = make_vtbl(sfv, (int)(sizeof sfv / sizeof sfv[0]));

    static const VM plv[] = {
        {"IDirectDrawPalette::QueryInterface", pl_QueryInterface}, {"IDirectDrawPalette::AddRef", pl_AddRef}, {"IDirectDrawPalette::Release", pl_Release},
        {"IDirectDrawPalette::GetCaps", pl_GetCaps}, {"IDirectDrawPalette::GetEntries", pl_GetEntries}, {"IDirectDrawPalette::Initialize", pl_Initialize},
        {"IDirectDrawPalette::SetEntries", pl_SetEntries},
    };
    vt_pal = make_vtbl(plv, (int)(sizeof plv / sizeof plv[0]));

    static const VM clv[] = {
        {"IDirectDrawClipper::QueryInterface", cl_QueryInterface}, {"IDirectDrawClipper::AddRef", cl_AddRef}, {"IDirectDrawClipper::Release", cl_Release},
        {"IDirectDrawClipper::GetClipList", NULL}, {"IDirectDrawClipper::GetHWnd", NULL}, {"IDirectDrawClipper::Initialize", cl_Ok3},
        {"IDirectDrawClipper::IsClipListChanged", cl_Ok3}, {"IDirectDrawClipper::SetClipList", cl_Ok3}, {"IDirectDrawClipper::SetHWnd", cl_Ok3},
    };
    vt_clip = make_vtbl(clv, (int)(sizeof clv / sizeof clv[0]));
}

API_FN(DirectDrawCreate) {
    if (!vt_dd) {
        memcpy(default_pal, gdi_system_palette(), sizeof default_pal);
        build_vtables();
    }
    if (!dd.guest) dd.guest = new_obj(vt_dd, 0);
    dd.refs++;
    rt_wr32(g_mem, ARG(1), dd.guest);
    DDTRACE("[ddraw] DirectDrawCreate");
    HOST_RET(DD_OK, 3);
}

void api_register_ddraw(void) {
    static const ApiEntry t[] = { API_ENTRY(DirectDrawCreate) };
    api_register(t, 1);
}
