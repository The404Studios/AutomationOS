/*
 * ide_map.c -- VIZ-1 "SEMANTIC LEGO MAP" panel.
 *
 * Renders the focused function (a->model.funcs[a->focus_func]) as a CENTRAL
 * node card with one row per Port, surrounded by SATELLITE cards for the
 * globals it reads (cyan) / writes (green), the functions it calls (yellow),
 * and dashed-red cards for ABSENT required ports. Colored edges run from the
 * relevant port row on the central card to each satellite; absent edges are
 * dashed red and end in a small arrowhead. Layout is a fixed grid (left =
 * inputs/reads, right = writes/calls, far-right = absent), panned by
 * a->map_ox / a->map_oy.
 *
 * Each card is drawn with a soft offset drop-shadow (a translucent rounded
 * rect via gfx_blend) and a coloured header band (a second rounded rect of the
 * accent colour across the card top) so the graph reads as physical "blocks".
 *
 * Render is two-pass: we first lay every satellite into the file-static
 * map_sats[] table (geometry + accent + label + central anchor), then draw all
 * edges, THEN draw every card on top -- so wires sit cleanly behind the cards.
 * The same table is read back by panel_map_click() for hit-testing.
 *
 * Freestanding: no libc, no malloc, no stdio. All helpers are file-static and
 * prefixed map_. Every loop is bounded by the M_MAX* caps; nothing here can
 * crash on a function with zero ports / reads / writes / calls. All drawing is
 * clipped to the supplied Rect r (gfx_* already clip to the canvas).
 */
#include "ide.h"
#include "ide_theme.h"

/* ---- card geometry (pixels) at 100% zoom ---- */
#define MAP_HEADER_H   (ROW_H + 2)   /* panel title strip                  */
#define MAP_CARD_W     214           /* central node width                 */
#define MAP_CARDHDR_H  22            /* central card header band           */
#define MAP_PORT_H     17            /* one port row inside central card   */
#define MAP_SAT_W      154           /* satellite card width               */
#define MAP_SATHDR_H   6             /* satellite accent header band       */
#define MAP_SAT_H      32            /* satellite card height (1-line)     */
#define MAP_SAT_H2     46            /* satellite card height (2-line)     */
#define MAP_SAT_VGAP   12            /* vertical gap between satellites    */
#define MAP_COL_GAP    44            /* gap from central card to a column  */
#define MAP_RADIUS     7             /* rounded-corner radius              */
#define MAP_SHADOW     0x55000000u   /* translucent drop-shadow colour     */
#define MAP_SHADOW_OFF 3             /* shadow offset (px, down-right)     */

/* Apply zoom scaling to a dimension (zoom is percent, e.g. 100 = 100%). */
static inline int map_scale(int val, int zoom) {
    return (val * zoom) / 100;
}

/* ---------------------------------------------------------------------------
 * Tiny freestanding string helpers (no libc). All bounded + NUL terminating.
 * ------------------------------------------------------------------------- */

static int map_strlen(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n] && n < 1024) n++;
    return n;
}

/* Local name compare (so we don't depend on link order of ide_streq). */
static int map_streq(const char* a, const char* b)
{
    int i = 0;
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* Bounded copy; returns chars written (excluding NUL). */
static int map_cpy(char* d, const char* s, int cap)
{
    int i = 0;
    if (!d || cap <= 0) return 0;
    if (s) { for (; i < cap - 1 && s[i]; i++) d[i] = s[i]; }
    d[i] = '\0';
    return i;
}

/* Append s onto d (cap bytes incl NUL) starting at current length. */
static void map_cat(char* d, const char* s, int cap)
{
    int i, j = 0;
    if (!d || cap <= 0) return;
    i = map_strlen(d);
    if (i >= cap - 1) { d[cap - 1] = '\0'; return; }
    if (s) { for (; i < cap - 1 && s[j]; i++, j++) d[i] = s[j]; }
    d[i] = '\0';
}

/*
 * Format a 0..100 "fit" score as a "0.NN" string (e.g. 92 -> "0.92",
 * 100 -> "1.00", 5 -> "0.05"). Writes into out (>= 6 bytes). Returns length.
 */
static int map_fit_str(int fit, char* out)
{
    int whole, frac, n = 0;
    if (fit < 0)   fit = 0;
    if (fit > 100) fit = 100;
    whole = fit / 100;          /* 0 or 1 */
    frac  = fit % 100;          /* 0..99  */
    out[n++] = (char)('0' + whole);
    out[n++] = '.';
    out[n++] = (char)('0' + (frac / 10));
    out[n++] = (char)('0' + (frac % 10));
    out[n]   = '\0';
    return n;
}

/* ---------------------------------------------------------------------------
 * Drawing helpers.
 * ------------------------------------------------------------------------- */

/* Filled disc-ish dot (small filled rounded square) of colour col. */
static void map_dot(Canvas* cv, int cx, int cy, uint32_t col)
{
    gfx_round(cv, cx - 3, cy - 3, 7, 7, 3, col);
}

/* Soft drop shadow: a translucent rounded rect offset down-and-right. Drawn
 * before the card so the card paints over the top-left of the shadow. */
static void map_shadow(Canvas* cv, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    gfx_round(cv, x + MAP_SHADOW_OFF, y + MAP_SHADOW_OFF, w, h,
              MAP_RADIUS, MAP_SHADOW);
}

/*
 * A solid rounded card: drop-shadow, body fill, a coloured header band across
 * the top (a second rounded rect of accent colour, clipped to the band height
 * by overpainting the lower portion with the body fill), and a 1px border.
 * Caller draws the body contents afterwards.
 */
static void map_card(Canvas* cv, int x, int y, int w, int h, int hdr_h,
                     uint32_t fill, uint32_t accent)
{
    if (w <= 0 || h <= 0) return;
    map_shadow(cv, x, y, w, h);
    /* accent header band: rounded rect of accent across the whole card... */
    gfx_round(cv, x, y, w, h, MAP_RADIUS, accent);
    /* ...then carve the body back out below the header band. The body uses a
     * smaller radius offset so the lower corners stay rounded. */
    if (h - hdr_h > 0)
        gfx_round(cv, x, y + hdr_h, w, h - hdr_h, MAP_RADIUS, fill);
    /* hairline divider under the band + outer border */
    gfx_hline(cv, x, y + hdr_h, w, accent);
    gfx_stroke(cv, x, y, w, h, accent);
}

/* A dashed-border card (no header band): body fill + dashed accent rectangle.
 * Used for ABSENT-port satellites. */
static void map_card_dashed(Canvas* cv, int x, int y, int w, int h,
                            uint32_t fill, uint32_t accent)
{
    if (w <= 0 || h <= 0) return;
    map_shadow(cv, x, y, w, h);
    gfx_round(cv, x, y, w, h, MAP_RADIUS, fill);
    gfx_dashed(cv, x,     y,     x + w, y,     accent, 4);
    gfx_dashed(cv, x,     y + h, x + w, y + h, accent, 4);
    gfx_dashed(cv, x,     y,     x,     y + h, accent, 4);
    gfx_dashed(cv, x + w, y,     x + w, y + h, accent, 4);
}

/* A small filled arrowhead at (sx,sy) pointing horizontally toward the card.
 * `from_left` = the wire arrives from the left, so the head points right. */
static void map_arrow(Canvas* cv, int sx, int sy, int from_left, uint32_t col)
{
    int d = from_left ? -6 : 6;     /* tail x-offset of the two whiskers */
    gfx_line(cv, sx, sy, sx + d, sy - 4, col);
    gfx_line(cv, sx, sy, sx + d, sy + 4, col);
    gfx_line(cv, sx, sy, sx + d, sy,     col);
}

/*
 * Edge from a port anchor on the central card (px,py) to the left/right edge
 * mid-point of a satellite card. We draw a short horizontal stub out of the
 * port, then a straight line to the satellite anchor -- cheap but reads as an
 * orthogonal-ish wire. `dashed` selects gfx_dashed (absent edges). An arrowhead
 * is drawn at the satellite end.
 */
static void map_edge(Canvas* cv, int px, int py, int sx, int sy,
                     uint32_t col, int dashed)
{
    int going_right = (sx >= px);
    int stub = going_right ? 12 : -12;   /* stub points toward the satellite */
    int mx   = px + stub;
    if (dashed) {
        gfx_dashed(cv, px, py, mx, py, col, 4);
        gfx_dashed(cv, mx, py, sx, sy, col, 4);
    } else {
        gfx_line(cv, px, py, mx, py, col);
        gfx_line(cv, mx, py, sx, sy, col);
    }
    /* arrowhead at the satellite end. The wire reaches the satellite from the
     * left when the satellite sits to the right of the central card. */
    map_arrow(cv, sx, sy, going_right, col);
}

/* ---------------------------------------------------------------------------
 * Satellite placement table.
 *
 * We record each satellite's screen rect, kind, accent colour, label and the
 * central-card anchor (ax,ay) + the wire's dashed flag, so the render can do a
 * clean edges-then-cards two-pass and panel_map_click() can hit-test without
 * re-deriving the layout. A satellite is one of:
 *   read global, write global, called function, or an absent port.
 * Only "call" satellites are clickable (to refocus that function).
 *
 * The table is a file-static scratch buffer rebuilt every panel_map() call;
 * panel_map_click() runs right after a render in the same frame, so reading it
 * back is safe and avoids duplicating the layout math.
 * ------------------------------------------------------------------------- */

typedef enum {
    MK_READ = 0, MK_WRITE, MK_CALL, MK_ABSENT
} MapKind;

typedef struct {
    Rect     r;
    MapKind  kind;
    uint32_t accent;          /* card accent / edge colour               */
    int      ax, ay;          /* central-card edge anchor for the wire   */
    int      dashed;          /* 1 = dashed edge (absent)                */
    int      warn;            /* 1 = draw orange multi-writer warning dot */
    int      fit;             /* MK_ABSENT: fit-if-added score 0..100    */
    char     label[80];       /* primary card label                      */
    char     fname[M_NAME];   /* for MK_CALL: the called function name   */
} MapSat;

/* Upper bound: reads + writes + calls + absent ports + inbound callers, all
 * M-capped. The + M_MAXFUNCS reserves room for the FAR-LEFT caller column so a
 * heavily-called function's callers are never silently dropped by map_sat_push. */
#define MAP_MAXSAT (M_MAXREFS + M_MAXREFS + M_MAXCALLS + M_MAXPORTS + M_MAXFUNCS)

static MapSat map_sats[MAP_MAXSAT];
static int    map_nsats;
static int    map_have_layout;   /* 1 once panel_map() has populated map_sats */

static void map_sat_reset(void)
{
    map_nsats = 0;
}

static MapSat* map_sat_push(Rect rc, MapKind kind, uint32_t accent,
                            int ax, int ay, int dashed, const char* label,
                            const char* fname)
{
    MapSat* s;
    if (map_nsats >= MAP_MAXSAT) return 0;
    s = &map_sats[map_nsats++];
    s->r      = rc;
    s->kind   = kind;
    s->accent = accent;
    s->ax     = ax;
    s->ay     = ay;
    s->dashed = dashed;
    s->warn   = 0;
    s->fit    = 0;
    s->label[0] = '\0';
    s->fname[0] = '\0';
    if (label) map_cpy(s->label, label, (int)sizeof(s->label));
    if (fname) map_cpy(s->fname, fname, M_NAME);
    return s;
}

/* ---------------------------------------------------------------------------
 * panel_map -- main render.
 * ------------------------------------------------------------------------- */

void panel_map(Ide* a, Canvas* cv, Rect r)
{
    Model* m;
    Func*  f;
    int    focus;
    char   buf[96];
    char   fitbuf[8];
    int    i;
    int    zoom;

    if (!a || !cv || r.w <= 0 || r.h <= 0) return;

    /* ---- backdrop + header strip ---- */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_BG);
    gfx_fill(cv, r.x, r.y, r.w, MAP_HEADER_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + MAP_HEADER_H - 1, r.w, TH_BORDER);

    m     = &a->model;
    focus = a->focus_func;
    zoom  = a->map_zoom;
    if (zoom < 1)   zoom = 1;        /* clamp scale range 0.01 (1%) .. 1.00 (100%) */
    if (zoom > 100) zoom = 100;

    /* Title: "SEMANTIC LEGO MAP - <focusname> [zoom%]". */
    buf[0] = '\0';
    map_cat(buf, "SEMANTIC LEGO MAP", sizeof(buf));
    if (focus >= 0 && focus < m->nfuncs && m->funcs[focus].name[0]) {
        map_cat(buf, " - ", sizeof(buf));
        map_cat(buf, m->funcs[focus].name, sizeof(buf));
    }
    /* Show zoom level in header */
    if (zoom != 100) {
        char zbuf[16];
        map_cat(buf, " [", sizeof(buf));
        ide_itoa(zoom, zbuf);
        map_cat(buf, zbuf, sizeof(buf));
        map_cat(buf, "%]", sizeof(buf));
    }
    gfx_text_clip(cv, r.x + PAD, r.y + (MAP_HEADER_H - GFX_FH) / 2,
                  buf, TH_TEXT_DIM, r.x + PAD, r.w - 2 * PAD);

    /* Body region below the header strip. */
    Rect body;
    body.x = r.x;
    body.y = r.y + MAP_HEADER_H;
    body.w = r.w;
    body.h = r.h - MAP_HEADER_H;
    if (body.h <= 0) return;

    /* Reset the satellite hit table for this frame. */
    map_sat_reset();
    map_have_layout = 1;

    /* ---- no focus: centered hint ---- */
    if (focus < 0 || focus >= m->nfuncs) {
        const char* hint = "Select a function";
        int tw = gfx_textw(hint);
        gfx_text_clip(cv, body.x + (body.w - tw) / 2,
                      body.y + (body.h - GFX_FH) / 2,
                      hint, TH_TEXT_DIM, body.x, body.w);
        return;
    }

    f = &m->funcs[focus];

    /* ----------------------------------------------------------------------
     * CENTRAL NODE CARD geometry. Positioned somewhat left-of-centre so the
     * right column (writes/calls) and absent column have room, then shifted
     * by the pan offset (map_ox/oy). All dimensions scaled by zoom.
     * -------------------------------------------------------------------- */
    int nports = f->nports;
    if (nports < 0) nports = 0;
    if (nports > M_MAXPORTS) nports = M_MAXPORTS;

    /* Apply zoom to all card dimensions */
    int card_w      = map_scale(MAP_CARD_W, zoom);
    int cardhdr_h   = map_scale(MAP_CARDHDR_H, zoom);
    int port_h      = map_scale(MAP_PORT_H, zoom);
    int sat_w       = map_scale(MAP_SAT_W, zoom);
    int sat_h       = map_scale(MAP_SAT_H, zoom);
    int sat_h2      = map_scale(MAP_SAT_H2, zoom);
    int sat_vgap    = map_scale(MAP_SAT_VGAP, zoom);
    int col_gap     = map_scale(MAP_COL_GAP, zoom);
    int sathdr_h    = map_scale(MAP_SATHDR_H, zoom);
    int shadow_off  = map_scale(MAP_SHADOW_OFF, zoom);
    int radius      = map_scale(MAP_RADIUS, zoom);

    int card_h = cardhdr_h + ROW_H /* "Ports (N)" line */
               + nports * port_h + PAD * 2;
    if (card_h < cardhdr_h + ROW_H + PAD * 2)
        card_h = cardhdr_h + ROW_H + PAD * 2;

    int cx = body.x + (body.w - card_w) / 2 - 30 + a->map_ox;
    int cy = body.y + (body.h - card_h) / 2 + a->map_oy;
    /* keep the card at least partially on-screen */
    if (cx < body.x + 4) cx = body.x + 4;
    if (cy < body.y + 4) cy = body.y + 4;

    int card_left  = cx;
    int card_right = cx + card_w;

    /* Port-row anchors (mid-row y), keyed by port index. Computed up-front so
     * satellite edges can attach precisely before the card is drawn. */
    int inner_y = cy + cardhdr_h + PAD + ROW_H;   /* first port-row top y */
    int port_y[M_MAXPORTS];
    for (i = 0; i < nports; i++)
        port_y[i] = inner_y + i * port_h + port_h / 2;

    /* Anchor for a satellite of a given port type: first matching central port
     * row, else the card header midline as a fallback. */
    int hdr_anchor = cy + cardhdr_h + ROW_H / 2;

    /* ----------------------------------------------------------------------
     * PASS 1 -- lay out every satellite into map_sats[] (no drawing yet).
     *   LEFT column   : read globals (cyan)
     *   RIGHT column  : write globals (green) then called functions (yellow)
     *   FAR-RIGHT col : absent ports (dashed red)
     * -------------------------------------------------------------------- */
    int left_x  = card_left - col_gap - sat_w;
    int right_x = card_right + col_gap;
    int far_x   = right_x + sat_w + col_gap;

    /* ---- LEFT: read globals (cyan, "g (read)") ---- */
    {
        int nreads = f->nreads;
        if (nreads < 0) nreads = 0;
        if (nreads > M_MAXREFS) nreads = M_MAXREFS;

        int ay = hdr_anchor;
        int j;
        for (j = 0; j < nports; j++)
            if (f->ports[j].type == PORT_STATE_READ) { ay = port_y[j]; break; }

        int sy = cy;
        for (i = 0; i < nreads; i++) {
            Rect sc;
            sc.x = left_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h;
            buf[0] = '\0';
            map_cat(buf, f->reads[i][0] ? f->reads[i] : "g", sizeof(buf));
            map_cat(buf, " (read)", sizeof(buf));
            /* wire reaches the satellite at its RIGHT edge */
            map_sat_push(sc, MK_READ, TH_CYAN, card_left, ay, 0, buf, 0);
            sy += sat_h + sat_vgap;
        }
    }

    /* ---- FAR-LEFT: inbound CALLERS (blue) -- every function in the model that
     * CALLS the focused one. This makes the graph bidirectional: previously the
     * map only showed "what I call"; now it also shows "who calls me", so a leaf
     * handler (no outbound calls) is still reachable/visible from its callers.
     * Reuses MK_CALL so the existing click-to-navigate path jumps to the caller. */
    {
        int caller_x = left_x - col_gap - sat_w;
        int sy = cy;
        int j;
        for (j = 0; j < m->nfuncs && j < M_MAXFUNCS; j++) {
            Func* g;
            int   c, ncalls, calls_focus = 0;
            if (j == focus) continue;                 /* self shown centrally */
            g = &m->funcs[j];
            if (!g->name[0]) continue;
            ncalls = g->ncalls;
            if (ncalls < 0) ncalls = 0;
            if (ncalls > M_MAXCALLS) ncalls = M_MAXCALLS;
            for (c = 0; c < ncalls; c++)
                if (g->calls[c][0] && f->name[0] &&
                    map_streq(g->calls[c], f->name)) { calls_focus = 1; break; }
            if (!calls_focus) continue;

            Rect sc;
            sc.x = caller_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h;
            buf[0] = '\0';
            map_cat(buf, g->name, sizeof(buf));
            map_cat(buf, "() ->", sizeof(buf));       /* "-> " calls into us */
            /* wire reaches the central card's LEFT edge at the header line */
            map_sat_push(sc, MK_CALL, TH_BLUE, card_left, hdr_anchor, 0, buf, g->name);
            sy += sat_h + sat_vgap;
        }
    }

    /* ---- RIGHT: write globals (green) then calls (yellow) ---- */
    {
        int nwrites = f->nwrites;
        int ncalls  = f->ncalls;
        if (nwrites < 0) nwrites = 0;
        if (nwrites > M_MAXREFS) nwrites = M_MAXREFS;
        if (ncalls  < 0) ncalls  = 0;
        if (ncalls  > M_MAXCALLS) ncalls = M_MAXCALLS;

        int wy = hdr_anchor, cy_anchor = hdr_anchor;
        int j;
        for (j = 0; j < nports; j++)
            if (f->ports[j].type == PORT_STATE_WRITE) { wy = port_y[j]; break; }
        for (j = 0; j < nports; j++)
            if (f->ports[j].type == PORT_CONTROL) { cy_anchor = port_y[j]; break; }

        int sy = cy;
        for (i = 0; i < nwrites; i++) {
            /* warn if this written global has more than one writer program-wide */
            int multi = 0;
            int g;
            for (g = 0; g < m->nglobals && g < M_MAXGLOBALS; g++)
                if (m->globals[g].name[0] && f->writes[i][0] &&
                    map_streq(m->globals[g].name, f->writes[i])) {
                    multi = (m->globals[g].nwriters > 1); break;
                }

            Rect sc;
            sc.x = right_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h;
            buf[0] = '\0';
            map_cat(buf, f->writes[i][0] ? f->writes[i] : "g", sizeof(buf));
            map_cat(buf, " (write)", sizeof(buf));
            MapSat* s = map_sat_push(sc, MK_WRITE, TH_GREEN, card_right, wy,
                                     0, buf, 0);
            if (s) s->warn = multi;
            sy += sat_h + sat_vgap;
        }

        for (i = 0; i < ncalls; i++) {
            Rect sc;
            sc.x = right_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h;
            buf[0] = '\0';
            map_cat(buf, f->calls[i][0] ? f->calls[i] : "fn", sizeof(buf));
            map_cat(buf, "()", sizeof(buf));
            map_sat_push(sc, MK_CALL, TH_YELLOW, card_right, cy_anchor,
                         0, buf, f->calls[i]);
            sy += sat_h + sat_vgap;
        }
    }

    /* ---- FAR-RIGHT: absent ports (dashed red, two-line card) ---- */
    {
        int sy = cy;
        for (i = 0; i < nports; i++) {
            Port* p = &f->ports[i];
            if (p->status != PS_ABSENT) continue;

            Rect sc;
            sc.x = far_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h2;
            MapSat* s = map_sat_push(sc, MK_ABSENT, TH_RED, card_right,
                                     port_y[i], 1,
                                     p->name[0] ? p->name : "gate", 0);
            if (s) s->fit = p->fit;
            sy += sat_h2 + sat_vgap;
        }
    }

    /* ----------------------------------------------------------------------
     * PASS 2a -- draw all edges first, so wires sit behind every card.
     * -------------------------------------------------------------------- */
    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        MapSat* s = &map_sats[i];
        int sat_left = (s->ax <= s->r.x);   /* wire goes left->right? */
        int sx = sat_left ? s->r.x : s->r.x + s->r.w;  /* satellite anchor x */
        int sy = s->r.y + s->r.h / 2;
        map_edge(cv, s->ax, s->ay, sx, sy, s->accent, s->dashed);
    }

    /* ----------------------------------------------------------------------
     * PASS 2b -- draw the central card on top of the wires.
     * -------------------------------------------------------------------- */
    map_card(cv, cx, cy, card_w, card_h, cardhdr_h, TH_PANEL, TH_PURPLE);

    /* Header = function signature: "ret name(p0, p1, ...)" (abbreviated). */
    buf[0] = '\0';
    if (f->ret[0]) { map_cat(buf, f->ret, sizeof(buf)); map_cat(buf, " ", sizeof(buf)); }
    map_cat(buf, f->name[0] ? f->name : "fn", sizeof(buf));
    map_cat(buf, "(", sizeof(buf));
    {
        int np = f->nparams;
        if (np < 0) np = 0;
        if (np > M_MAXPARAMS) np = M_MAXPARAMS;
        for (i = 0; i < np; i++) {
            const char* pn = f->params[i].type[0] ? f->params[i].type
                                                  : f->params[i].name;
            if (i) map_cat(buf, ",", sizeof(buf));
            map_cat(buf, pn ? pn : "?", sizeof(buf));
            /* keep the signature short */
            if (map_strlen(buf) > 40) { map_cat(buf, "..", sizeof(buf)); break; }
        }
    }
    map_cat(buf, ")", sizeof(buf));
    gfx_text_clip(cv, cx + PAD, cy + (cardhdr_h - GFX_FH) / 2,
                  buf, TH_BG, cx + PAD, card_w - 2 * PAD);

    /* "Ports (N)" sub-heading. */
    int sub_y = cy + cardhdr_h + PAD;
    buf[0] = '\0';
    map_cat(buf, "Ports (", sizeof(buf));
    { char nb[12]; ide_itoa(nports, nb); map_cat(buf, nb, sizeof(buf)); }
    map_cat(buf, ")", sizeof(buf));
    gfx_text_clip(cv, cx + PAD, sub_y, buf, TH_TEXT_DIM,
                  cx + PAD, card_w - 2 * PAD);

    /*
     * Port rows: a colored dot (th_port_color), the port name, and the fit as
     * "0.NN" right-aligned. Absent ports are drawn in red with "req 0.NN".
     */
    for (i = 0; i < nports; i++) {
        Port* p = &f->ports[i];
        int ry  = inner_y + i * port_h;
        int mid = port_y[i];
        int is_absent = (p->status == PS_ABSENT);
        uint32_t pc = th_port_color((int)p->type);
        int ty = ry + (port_h - GFX_FH) / 2;

        /* dot at the row's leading edge */
        map_dot(cv, cx + PAD + 3, mid, is_absent ? TH_RED : pc);

        /* port name (left) */
        gfx_text_clip(cv, cx + PAD + 12, ty,
                      p->name[0] ? p->name : "port",
                      is_absent ? TH_RED : TH_TEXT,
                      cx + PAD, card_w - 2 * PAD - 48);

        /* fit score, right-aligned within the card */
        fitbuf[0] = '\0';
        if (is_absent) {
            char tmp[8];
            map_cpy(fitbuf, "req ", sizeof(fitbuf));
            map_fit_str(p->fit, tmp);
            map_cat(fitbuf, tmp, sizeof(fitbuf));
        } else {
            map_fit_str(p->fit, fitbuf);
        }
        {
            int fw = gfx_textw(fitbuf);
            int fx = cx + card_w - PAD - fw;
            gfx_text_clip(cv, fx, ty, fitbuf,
                          is_absent ? TH_RED : TH_TEXT_DIM,
                          cx + PAD, card_w - 2 * PAD);
        }
    }

    /* ----------------------------------------------------------------------
     * PASS 2c -- draw every satellite card on top.
     * -------------------------------------------------------------------- */
    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        MapSat* s   = &map_sats[i];
        Rect    sc  = s->r;
        int     ty  = sc.y + sathdr_h + (sc.h - sathdr_h - GFX_FH) / 2;

        if (s->kind == MK_ABSENT) {
            /* dashed-red two-line card: "(ABSENT)" then "fit 0.NN" */
            map_card_dashed(cv, sc.x, sc.y, sc.w, sc.h, TH_PANEL2, s->accent);
            map_dot(cv, sc.x + PAD + 3, sc.y + 5 + GFX_FH / 2, s->accent);

            buf[0] = '\0';
            map_cat(buf, s->label, sizeof(buf));
            map_cat(buf, " (ABSENT)", sizeof(buf));
            gfx_text_clip(cv, sc.x + PAD + 12, sc.y + 5, buf, TH_RED,
                          sc.x + PAD, sc.w - 2 * PAD);

            buf[0] = '\0';
            map_cat(buf, "fit ", sizeof(buf));
            { char tmp[8]; map_fit_str(s->fit, tmp); map_cat(buf, tmp, sizeof(buf)); }
            gfx_text_clip(cv, sc.x + PAD + 12, sc.y + 5 + GFX_FH + 2, buf,
                          TH_TEXT_DIM, sc.x + PAD, sc.w - 2 * PAD);
            continue;
        }

        /* solid card with accent header band */
        map_card(cv, sc.x, sc.y, sc.w, sc.h, sathdr_h, TH_PANEL2, s->accent);
        map_dot(cv, sc.x + PAD + 3, ty + GFX_FH / 2, s->accent);
        gfx_text_clip(cv, sc.x + PAD + 12, ty, s->label, TH_TEXT,
                      sc.x + PAD, sc.w - 2 * PAD - 8);

        if (s->warn)   /* multi-writer warning marker (orange) in the corner */
            map_dot(cv, sc.x + sc.w - PAD - 2, sc.y + sathdr_h + PAD,
                    TH_ORANGE);
    }

    /* ---- footer legend (faint) ---- */
    {
        const char* hint = "cyan read  green write  yellow call  red absent";
        gfx_text_clip(cv, body.x + PAD, body.y + body.h - GFX_FH - 2,
                      hint, TH_TEXT_FAINT, body.x + PAD, body.w - 2 * PAD);
    }
}

/* ---------------------------------------------------------------------------
 * panel_map_click -- click handling.
 *
 * Outside r: not ours (return 0). Inside r: always consume (return 1). If the
 * click landed on a CALL satellite whose function exists in the model, refocus
 * to it via ide_set_focus().
 * ------------------------------------------------------------------------- */

int panel_map_click(Ide* a, Rect r, int mx, int my)
{
    int i, j;
    if (!a) return 0;
    if (!rect_hit(r, mx, my)) return 0;   /* outside our panel: pass through */

    if (!map_have_layout) return 1;       /* nothing laid out yet, but consume */

    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        MapSat* s = &map_sats[i];
        if (!rect_hit(s->r, mx, my)) continue;

        /* Handle different satellite types */
        switch (s->kind) {
        case MK_CALL:
            if (s->fname[0]) {
                /* find the called function in the model and refocus to it */
                for (j = 0; j < a->model.nfuncs && j < M_MAXFUNCS; j++) {
                    if (map_streq(a->model.funcs[j].name, s->fname)) {
                        a->prev_focus = a->focus_func;   /* remember for Backspace = back */
                        ide_set_focus(a, j);
                        return 1;
                    }
                }
            }
            break;

        case MK_READ:
        case MK_WRITE:
            /* Future: jump to global definition or show all readers/writers
             * For now, we could search for the global declaration in the code.
             * This is a placeholder for revolutionary "navigate by dependency" feature.
             * The label format is "varname (read)" or "varname (write)"
             * Extract varname and search for its definition. */
            /* TODO: Implement global definition lookup */
            break;

        case MK_ABSENT:
            /* Clicked an absent port - could show why it's missing or offer to add it
             * This is revolutionary "auto-fix architecture violations" */
            /* TODO: Show absent port details or quick-fix menu */
            break;
        }
        return 1;   /* clicked a satellite: consume */
    }
    return 1;       /* clicked empty map space: consume */
}
