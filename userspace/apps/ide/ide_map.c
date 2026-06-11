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
#include "ide_marks.h"   /* IDE-FORGE-0: ISOLATE "LOCKED" header chip */

/* ---- card geometry (pixels) at 100% zoom ---- */
#define MAP_HEADER_H   (ROW_H + 2)   /* panel title strip                  */
#define MAP_CARD_W     214           /* central node width                 */
#define MAP_CARDHDR_H  22            /* central card header band           */
#define MAP_PORT_H     17            /* one port row inside central card   */
#define MAP_SAT_W      154           /* satellite card width               */
#define MAP_SATHDR_H   6             /* satellite accent header band       */
#define MAP_SAT_H      46            /* satellite card height (2-line: label + type/ports) -- VIZ1-PARITY-0 */
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

/* VIZ1-PARITY-0: square connector stud where a wire meets a card border (the
 * LEGO "plug"). Drawn in the edge pass so the card later overpaints its inner
 * half -> the stud pokes cleanly out of the border. */
static void map_stud(Canvas* cv, int x, int y, uint32_t col)
{
    gfx_fill(cv, x - 3, y - 3, 6, 6, col);
    gfx_stroke(cv, x - 3, y - 3, 6, 6, TH_BG);
}

/* VIZ1-PARITY-0: declared type of a global by name ("" if unknown). */
static const char* map_global_type(Model* m, const char* name)
{
    int i;
    if (!m || !name || !name[0]) return "";
    for (i = 0; i < m->nglobals && i < M_MAXGLOBALS; i++)
        if (map_streq(m->globals[i].name, name)) return m->globals[i].type;
    return "";
}

/* VIZ1-PARITY-0: port count of a function by name (-1 = not in the model,
 * i.e. an external call). */
static int map_func_nports(Model* m, const char* name)
{
    int i;
    if (!m || !name || !name[0]) return -1;
    for (i = 0; i < m->nfuncs && i < M_MAXFUNCS; i++)
        if (map_streq(m->funcs[i].name, name)) return m->funcs[i].nports;
    return -1;
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
    char     sub[56];         /* VIZ1-PARITY-0: second line (Type/Ports) */
    char     fname[M_NAME];   /* for MK_CALL: the called function name   */
} MapSat;

/* Upper bound: must accommodate both the per-function dependency view (reads +
 * writes + calls + absent ports + inbound callers) AND the file overview (all
 * includes + macros + records + globals + protos + funcs). We take the larger. */
#define MAP_MAXSAT_FUNC  (M_MAXREFS + M_MAXREFS + M_MAXCALLS + M_MAXPORTS + M_MAXFUNCS)
#define MAP_MAXSAT_OV    (M_MAXINCLUDES + M_MAXMACROS + M_MAXRECORDS + M_MAXGLOBALS + M_MAXPROTOS + M_MAXFUNCS)
#define MAP_MAXSAT       (MAP_MAXSAT_FUNC > MAP_MAXSAT_OV ? MAP_MAXSAT_FUNC : MAP_MAXSAT_OV)

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
    s->sub[0]   = '\0';
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

    /* Title: "SEMANTIC LEGO MAP - <focusname> [zoom%]" or "FILE OVERVIEW". */
    buf[0] = '\0';
    map_cat(buf, "SEMANTIC LEGO MAP", sizeof(buf));
    if (focus >= 0 && focus < m->nfuncs && m->funcs[focus].name[0]) {
        map_cat(buf, " - ", sizeof(buf));
        map_cat(buf, m->funcs[focus].name, sizeof(buf));
    } else {
        map_cat(buf, " - FILE OVERVIEW", sizeof(buf));
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

    /* IDE-FORGE-0 ISOLATE: a "LOCKED" chip (right-aligned in the header) when the
     * focused function is isolated -- the map is pinned and the caret no longer
     * re-centers it (see ide_sel_from_caret). */
    if (focus >= 0 && focus < m->nfuncs) {
        SymMark* fmk = marks_find(m->funcs[focus].name);
        if (fmk && fmk->isolate) {
            const char* lk = "LOCKED";
            int cw = gfx_textw(lk) + 2 * PAD;
            int lx = r.x + r.w - PAD - cw;
            int ly = r.y + 2;
            if (lx > r.x + PAD) {
                gfx_round(cv, lx, ly, cw, MAP_HEADER_H - 4, 4, TH_PANEL2);
                gfx_text_clip(cv, lx + PAD, r.y + (MAP_HEADER_H - GFX_FH) / 2,
                              lk, TH_YELLOW, lx + PAD, cw - 2 * PAD);
            }
        }
    }

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

    /* ---- FILE OVERVIEW: all code elements as colored LEGO tiles ---- */
    if (focus < 0 || focus >= m->nfuncs) {
        /* Total element count for the overview */
        int total = m->nincludes + m->nmacros + m->nrecords +
                    m->nglobals + m->nfuncs + m->nprotos;
        if (total == 0) {
            const char* hint = "No code elements found";
            int tw = gfx_textw(hint);
            gfx_text_clip(cv, body.x + (body.w - tw) / 2,
                          body.y + (body.h - GFX_FH) / 2,
                          hint, TH_TEXT_DIM, body.x, body.w);
            return;
        }

        /* Tile geometry */
        #define OV_TILE_W   180
        #define OV_TILE_H   28
        #define OV_GAP_X    10
        #define OV_GAP_Y    6
        #define OV_GLYPH_W  24     /* space for the small type icon */
        #define OV_HDRBAND  5      /* accent header band height */

        int cols = (body.w - 2 * PAD + OV_GAP_X) / (OV_TILE_W + OV_GAP_X);
        if (cols < 1) cols = 1;

        int tile_idx = 0;
        /* MAP-STABLE-0: overview tiles form a fixed grid -- tile_idx walks the
         * model in fixed section order (includes->macros->records->globals->
         * protos->funcs), each section in source-array order, so a given
         * element keeps the SAME grid cell while the file (and window width)
         * are unchanged. scroll_y reuses map_oy, which ide_set_focus() zeroes
         * on every focus transition, so the overview always opens at the top
         * row and never inherits a focused-view pan. (Globals carry no source
         * line, so a cross-section line sort is intentionally not attempted.) */
        int scroll_y = a->map_oy;

        /* Helper macro to place a tile and record it in the satellite table.
         * kind = MK_CALL for function tiles (navigable), MK_READ otherwise. */
        #define OV_TILE(accent, label, fname_str, mk) do {                  \
            int col = tile_idx % cols;                                      \
            int row = tile_idx / cols;                                      \
            int tx = body.x + PAD + col * (OV_TILE_W + OV_GAP_X);          \
            int ty = body.y + PAD + row * (OV_TILE_H + OV_GAP_Y) + scroll_y;\
            Rect sc; sc.x = tx; sc.y = ty; sc.w = OV_TILE_W; sc.h = OV_TILE_H;\
            if (ty + OV_TILE_H > body.y && ty < body.y + body.h) {         \
                map_card(cv, tx, ty, OV_TILE_W, OV_TILE_H, OV_HDRBAND,    \
                         TH_PANEL2, (accent));                             \
                if (tile_idx == a->map_selected) {                        \
                    gfx_stroke(cv, tx,   ty,   OV_TILE_W,   OV_TILE_H,   TH_BLUE);\
                    gfx_stroke(cv, tx-1, ty-1, OV_TILE_W+2, OV_TILE_H+2, TH_BLUE);\
                }                                                          \
                int lx = tx + PAD + OV_GLYPH_W;                           \
                int ly = ty + OV_HDRBAND + (OV_TILE_H - OV_HDRBAND - GFX_FH) / 2;\
                gfx_text_clip(cv, lx, ly, (label), TH_TEXT,               \
                              lx, OV_TILE_W - PAD - OV_GLYPH_W - PAD);   \
                /* small type icon */                                       \
                gfx_text_clip(cv, tx + PAD, ly, (mk == MK_CALL ? "{}" :   \
                    (accent) == TH_GREEN ? "S" :                           \
                    (accent) == TH_YELLOW ? "=" :                          \
                    (accent) == TH_PURPLE ? "#" :                          \
                    (accent) == TH_TEXT_DIM ? ">" :                        \
                    (accent) == TH_CYAN ? "p" : "?"),                     \
                    (accent), tx + PAD, OV_GLYPH_W);                      \
            }                                                               \
            map_sat_push(sc, (mk), (accent), 0, 0, 0, (label), (fname_str));\
            tile_idx++;                                                     \
        } while(0)

        /* == SECTION: Includes (gray) == */
        for (i = 0; i < m->nincludes && i < M_MAXINCLUDES; i++) {
            OV_TILE(TH_TEXT_DIM, m->includes[i].path, "", MK_READ);
        }

        /* == SECTION: Macros (purple) == */
        for (i = 0; i < m->nmacros && i < M_MAXMACROS; i++) {
            OV_TILE(TH_PURPLE, m->macros[i].name, "", MK_READ);
        }

        /* == SECTION: Records/typedefs (green) == */
        for (i = 0; i < m->nrecords && i < M_MAXRECORDS; i++) {
            buf[0] = '\0';
            map_cat(buf, m->records[i].kind_tag, sizeof(buf));
            map_cat(buf, " ", sizeof(buf));
            map_cat(buf, m->records[i].name, sizeof(buf));
            OV_TILE(TH_GREEN, buf, "", MK_READ);
        }

        /* == SECTION: Globals (yellow) == */
        for (i = 0; i < m->nglobals && i < M_MAXGLOBALS; i++) {
            buf[0] = '\0';
            if (m->globals[i].type[0]) {
                map_cat(buf, m->globals[i].type, sizeof(buf));
                map_cat(buf, " ", sizeof(buf));
            }
            map_cat(buf, m->globals[i].name, sizeof(buf));
            OV_TILE(TH_YELLOW, buf, "", MK_READ);
        }

        /* == SECTION: Function prototypes (blue, dim) == */
        for (i = 0; i < m->nprotos && i < M_MAXPROTOS; i++) {
            buf[0] = '\0';
            map_cat(buf, m->protos[i].name, sizeof(buf));
            map_cat(buf, "()", sizeof(buf));
            OV_TILE(TH_CYAN, buf, "", MK_READ);
        }

        /* == SECTION: Function definitions (blue, clickable) == */
        for (i = 0; i < m->nfuncs && i < M_MAXFUNCS; i++) {
            buf[0] = '\0';
            map_cat(buf, m->funcs[i].name, sizeof(buf));
            map_cat(buf, "()", sizeof(buf));
            OV_TILE(TH_BLUE, buf, m->funcs[i].name, MK_CALL);
        }

        #undef OV_TILE

        /* footer legend */
        {
            const char* hint = "gray #include  purple #define  green struct/type  yellow global  cyan proto  blue func (click)";
            gfx_text_clip(cv, body.x + PAD, body.y + body.h - GFX_FH - 2,
                          hint, TH_TEXT_FAINT, body.x + PAD, body.w - 2 * PAD);
        }
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
               + nports * port_h + GFX_FH + PAD * 3;   /* + IDA-style close hint (VIZ1-PARITY-0) */
    if (card_h < cardhdr_h + ROW_H + GFX_FH + PAD * 3)
        card_h = cardhdr_h + ROW_H + GFX_FH + PAD * 3;

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
     *
     * MAP-STABLE-0 ORDER CONTRACT: within every column, satellites follow
     * their model-array index order (f->reads / m->funcs / f->writes /
     * f->calls / f->ports). That order is PARSE ORDER = SOURCE ORDER (the
     * parser appends first-occurrence-deduped; ide_semantic builds ports in
     * a fixed order). It is NOT a hash and never depends on focus/visit
     * history, so a given function always lays out identically frame-to-
     * frame and visit-to-visit (aphantasia spatial-consistency law). Do not
     * introduce a sort on a mutable key here.
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
            /* wire reaches the satellite at its RIGHT edge. Stash the BARE global
             * name in fname so a click can navigate by dependency (jump to a
             * function that WRITES this global -- the producer). */
            { MapSat* s = map_sat_push(sc, MK_READ, TH_CYAN, card_left, ay, 0, buf, f->reads[i]);
              const char* ty = map_global_type(m, f->reads[i]);     /* VIZ1-PARITY-0 */
              if (s && ty[0]) { map_cpy(s->sub, "Type: ", sizeof(s->sub));
                                map_cat(s->sub, ty, sizeof(s->sub)); } }
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
            { MapSat* s = map_sat_push(sc, MK_CALL, TH_BLUE, card_left, hdr_anchor, 0, buf, g->name);
              if (s) { char nb[12]; int nn = ide_itoa(g->nports, nb); nb[nn] = 0;  /* VIZ1-PARITY-0 */
                       map_cpy(s->sub, "Ports (", sizeof(s->sub));
                       map_cat(s->sub, nb, sizeof(s->sub));
                       map_cat(s->sub, ")", sizeof(s->sub)); } }
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
            /* Stash the BARE global name in fname so a click navigates by
             * dependency (jump to a function that READS this global -- a consumer). */
            MapSat* s = map_sat_push(sc, MK_WRITE, TH_GREEN, card_right, wy,
                                     0, buf, f->writes[i]);
            if (s) s->warn = multi;
            { const char* ty = map_global_type(m, f->writes[i]);    /* VIZ1-PARITY-0 */
              if (s && ty[0]) { map_cpy(s->sub, "Type: ", sizeof(s->sub));
                                map_cat(s->sub, ty, sizeof(s->sub)); } }
            sy += sat_h + sat_vgap;
        }

        for (i = 0; i < ncalls; i++) {
            Rect sc;
            sc.x = right_x; sc.y = sy; sc.w = sat_w; sc.h = sat_h;
            buf[0] = '\0';
            map_cat(buf, f->calls[i][0] ? f->calls[i] : "fn", sizeof(buf));
            map_cat(buf, "()", sizeof(buf));
            { MapSat* s = map_sat_push(sc, MK_CALL, TH_YELLOW, card_right, cy_anchor,
                         0, buf, f->calls[i]);
              int np = map_func_nports(m, f->calls[i]);             /* VIZ1-PARITY-0 */
              if (s) {
                  if (np >= 0) { char nb[12]; int nn = ide_itoa(np, nb); nb[nn] = 0;
                                 map_cpy(s->sub, "Ports (", sizeof(s->sub));
                                 map_cat(s->sub, nb, sizeof(s->sub));
                                 map_cat(s->sub, ")", sizeof(s->sub)); }
                  else map_cpy(s->sub, "(extern)", sizeof(s->sub));
              } }
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
        /* VIZ1-PARITY-0: square connector studs at both wire endpoints; the
         * cards (passes 2b/2c) overpaint the inner half so each stud pokes
         * out of the border -- the mockup's LEGO plug look. */
        map_stud(cv, s->ax, s->ay, s->accent);
        map_stud(cv, sx,    sy,    s->accent);
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
    /* IDE-FORGE-0 node detail: a "N ln" size chip right-aligned in the header
     * (Func.line_end - line_start + 1). The signature clips to the room left. */
    int hy = cy + (cardhdr_h - GFX_FH) / 2;
    {
        int lines = f->line_end - f->line_start + 1;
        if (lines < 1) lines = 1;
        char lnbuf[16]; int p = 0;
        { char nb[12]; int nn = ide_itoa(lines, nb); for (int j = 0; j < nn && p < 11; j++) lnbuf[p++] = nb[j]; }
        lnbuf[p++] = ' '; lnbuf[p++] = 'l'; lnbuf[p++] = 'n'; lnbuf[p] = 0;
        int lnw = gfx_textw(lnbuf);
        int sig_clip = card_w - 2 * PAD - lnw - PAD;
        if (sig_clip < GFX_FW) sig_clip = card_w - 2 * PAD;
        gfx_text_clip(cv, cx + PAD, hy, buf, TH_BG, cx + PAD, sig_clip);
        int lx = cx + card_w - PAD - lnw;
        if (lx > cx + PAD + sig_clip)
            gfx_text_clip(cv, lx, hy, lnbuf, TH_BG, lx, lnw);
    }

    /* "Ports (N)" sub-heading. */
    int sub_y = cy + cardhdr_h + PAD;
    buf[0] = '\0';
    map_cat(buf, "Ports (", sizeof(buf));
    { char nb[12]; ide_itoa(nports, nb); map_cat(buf, nb, sizeof(buf)); }
    map_cat(buf, ")", sizeof(buf));
    gfx_text_clip(cv, cx + PAD, sub_y, buf, TH_TEXT_DIM,
                  cx + PAD, card_w - 2 * PAD);

    /* IDE-FORGE-0 node detail: "R<r> W<w> C<c>" fan chip + a red holes badge
     * ("!<n>" absent ports), right-aligned on the Ports sub-heading line. */
    {
        int absent_n = 0;
        for (i = 0; i < nports; i++)
            if (f->ports[i].status == PS_ABSENT) absent_n++;
        char rb[40]; int p = 0;
        rb[p++] = 'R'; { char nb[12]; int nn = ide_itoa(f->nreads,  nb); for (int j = 0; j < nn && p < 36; j++) rb[p++] = nb[j]; }
        rb[p++] = ' '; rb[p++] = 'W'; { char nb[12]; int nn = ide_itoa(f->nwrites, nb); for (int j = 0; j < nn && p < 36; j++) rb[p++] = nb[j]; }
        rb[p++] = ' '; rb[p++] = 'C'; { char nb[12]; int nn = ide_itoa(f->ncalls,  nb); for (int j = 0; j < nn && p < 36; j++) rb[p++] = nb[j]; }
        rb[p] = 0;
        int rw = gfx_textw(rb);
        char hb[16]; hb[0] = 0;
        int hw = 0;
        if (absent_n > 0) {
            int q = 0; hb[q++] = '!';
            { char nb[12]; int nn = ide_itoa(absent_n, nb); for (int j = 0; j < nn && q < 14; j++) hb[q++] = nb[j]; }
            hb[q] = 0;
            hw = gfx_textw(hb) + GFX_FW;
        }
        int rx = cx + card_w - PAD - rw - hw;
        int used = gfx_textw(buf);                 /* "Ports (N)" width */
        if (rx > cx + PAD + used + GFX_FW) {
            gfx_text_clip(cv, rx, sub_y, rb, TH_TEXT_FAINT, rx, rw);
            if (absent_n > 0)
                gfx_text_clip(cv, rx + rw + GFX_FW, sub_y, hb, TH_RED,
                              rx + rw + GFX_FW, gfx_textw(hb));
        }
    }

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

    /* VIZ1-PARITY-0: IDA-style close hint along the card's bottom edge. */
    gfx_text_clip(cv, cx + PAD, cy + card_h - GFX_FH - PAD,
                  "CLICK TO CLOSE (IDA STYLE)", TH_TEXT_FAINT,
                  cx + PAD, card_w - 2 * PAD);

    /* ----------------------------------------------------------------------
     * PASS 2c -- draw every satellite card on top.
     * -------------------------------------------------------------------- */
    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        MapSat* s   = &map_sats[i];
        Rect    sc  = s->r;
        int     ty  = sc.y + sathdr_h + (sc.h - sathdr_h - GFX_FH) / 2;

        /* SELECTION glow: a larger accent rect drawn BEHIND the card so a cyan
         * border shows once the card paints on top -- visual feedback that the
         * map is interactive (click or keyboard-select a node). */
        if (i == a->map_selected)
            gfx_round(cv, sc.x - 3, sc.y - 3, sc.w + 6, sc.h + 6, 6, TH_CYAN);

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

        /* solid card with accent header band.
         * VIZ1-PARITY-0: two-line layout -- primary label top-aligned, the
         * sub line (Type: <t> / Ports (N) / (extern)) dimmed below it. */
        map_card(cv, sc.x, sc.y, sc.w, sc.h, sathdr_h, TH_PANEL2, s->accent);
        ty = sc.y + sathdr_h + 3;
        map_dot(cv, sc.x + PAD + 3, ty + GFX_FH / 2, s->accent);
        gfx_text_clip(cv, sc.x + PAD + 12, ty, s->label, TH_TEXT,
                      sc.x + PAD, sc.w - 2 * PAD - 8);
        if (s->sub[0])
            gfx_text_clip(cv, sc.x + PAD + 12, ty + GFX_FH + 1, s->sub,
                          TH_TEXT_DIM, sc.x + PAD, sc.w - 2 * PAD - 8);

        if (s->warn)   /* multi-writer warning marker (orange) in the corner */
            map_dot(cv, sc.x + sc.w - PAD - 2, sc.y + sathdr_h + PAD,
                    TH_ORANGE);
    }

    /* ---- footer legend: colored swatch chips (VIZ1-PARITY-0) ---- */
    {
        static const uint32_t lc[4] = { TH_CYAN, TH_GREEN, TH_YELLOW, TH_RED };
        static const char* const lt[4] = { "read", "write", "call", "absent" };
        int lx = body.x + PAD;
        int ly = body.y + body.h - GFX_FH - 2;
        int k, sw = GFX_FH - 4;
        for (k = 0; k < 4; k++) {
            gfx_fill  (cv, lx, ly + 2, sw, sw, lc[k]);
            gfx_stroke(cv, lx, ly + 2, sw, sw, TH_BORDER_LT);
            lx += sw + 5;
            gfx_text_clip(cv, lx, ly, lt[k], TH_TEXT_DIM, body.x + PAD, body.w - 2 * PAD);
            lx += gfx_textw(lt[k]) + 14;
        }
    }
}

/* ---------------------------------------------------------------------------
 * panel_map_click -- click handling.
 *
 * Outside r: not ours (return 0). Inside r: always consume (return 1). If the
 * click landed on a CALL satellite whose function exists in the model, refocus
 * to it via ide_set_focus().
 * ------------------------------------------------------------------------- */

/* Follow a satellite node (shared by click + keyboard activate):
 *   CALL        -> refocus the called function.
 *   READ/WRITE  -> jump to a producer/consumer of that global.
 *   ABSENT      -> no-op (quick-fix menu TODO). */
static void map_sat_follow(Ide* a, MapSat* s)
{
    int j;
    switch (s->kind) {
    case MK_CALL:
        if (s->fname[0]) {
            int any = -1;
            for (j = 0; j < a->model.nfuncs && j < M_MAXFUNCS; j++) {
                if (!map_streq(a->model.funcs[j].name, s->fname)) continue;
                if (map_streq(a->model.funcs[j].file, a->model.cur_file)) {
                    /* IDE-SYNC-0 S2: a map follow also lands the editor
                     * caret on the function (prev_focus kept inside). */
                    ide_sel_jump(a, j, PANE_MAP);
                    return;
                }
                /* sibling-file match: remember the first, but keep scanning
                 * so a same-file definition wins a static-name collision. */
                if (any < 0) any = j;
            }
            /* IDE-XFILE-0b: the callee lives in a SIBLING file -- open it
             * and jump there (the model rebuilds; inputs copied inside). */
            if (any >= 0)
                ide_sel_jump_xfile(a, a->model.funcs[any].name,
                                      a->model.funcs[any].file);
        }
        break;
    case MK_READ:
    case MK_WRITE:
        /* Navigate by dependency. s->fname holds the BARE global name.
         *   READ  port -> jump to a function that WRITES it (the producer).
         *   WRITE port -> jump to a function that READS it (a consumer).
         * Skip the currently-focused function so the move is always visible. */
        if (s->fname[0]) {
            /* IDE-XFILE-0b: pass 0 prefers a same-file producer/consumer
             * (no file switch); pass 1 falls back to a sibling file via
             * the open-then-jump path. */
            int pass;
            for (pass = 0; pass < 2; pass++) {
                for (j = 0; j < a->model.nfuncs && j < M_MAXFUNCS; j++) {
                    Func* g = &a->model.funcs[j];
                    int k, nref, hit = 0;
                    int same = map_streq(g->file, a->model.cur_file);
                    if (j == a->focus_func) continue;
                    if (pass == 0 ? !same : same) continue;
                    if (s->kind == MK_READ) {
                        nref = g->nwrites; if (nref > M_MAXREFS) nref = M_MAXREFS;
                        for (k = 0; k < nref; k++)
                            if (g->writes[k][0] && map_streq(g->writes[k], s->fname)) { hit = 1; break; }
                    } else {
                        nref = g->nreads;  if (nref > M_MAXREFS) nref = M_MAXREFS;
                        for (k = 0; k < nref; k++)
                            if (g->reads[k][0] && map_streq(g->reads[k], s->fname)) { hit = 1; break; }
                    }
                    if (!hit) continue;
                    if (same) ide_sel_jump(a, j, PANE_MAP);            /* S2 */
                    else      ide_sel_jump_xfile(a, g->name, g->file); /* 0b */
                    return;
                }
            }
        }
        break;
    case MK_ABSENT:
    default:
        break;
    }
}

int panel_map_click(Ide* a, Rect r, int mx, int my)
{
    int i;
    if (!a) return 0;
    if (!rect_hit(r, mx, my)) return 0;   /* outside our panel: pass through */

    if (!map_have_layout) return 1;       /* nothing laid out yet, but consume */

    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        MapSat* s = &map_sats[i];
        if (!rect_hit(s->r, mx, my)) continue;
        a->map_selected = i;              /* select (highlight) the clicked node */
        map_sat_follow(a, s);             /* then follow it (may refocus)        */
        return 1;                         /* clicked a satellite: consume        */
    }
    return 1;       /* clicked empty map space: consume */
}

/* Keyboard navigation across map nodes (uses last frame's laid-out geometry).
 * dir: 0=up 1=down 2=left 3=right. Moves a->map_selected to the nearest node in
 * that direction; with nothing selected yet, selects the first node. */
void map_nav(Ide* a, int dir)
{
    int i, cx, cy, best = -1;
    long bestd = 0;
    if (!a || !map_have_layout || map_nsats <= 0) return;
    if (a->map_selected < 0 || a->map_selected >= map_nsats) { a->map_selected = 0; return; }
    cx = map_sats[a->map_selected].r.x + map_sats[a->map_selected].r.w / 2;
    cy = map_sats[a->map_selected].r.y + map_sats[a->map_selected].r.h / 2;
    for (i = 0; i < map_nsats && i < MAP_MAXSAT; i++) {
        int ix, iy, dx, dy, adx, ady, ok = 0;
        if (i == a->map_selected) continue;
        ix = map_sats[i].r.x + map_sats[i].r.w / 2;
        iy = map_sats[i].r.y + map_sats[i].r.h / 2;
        dx = ix - cx; dy = iy - cy;
        adx = dx < 0 ? -dx : dx; ady = dy < 0 ? -dy : dy;
        switch (dir) {
        case 0: ok = (dy < 0 && ady >= adx); break;   /* up    */
        case 1: ok = (dy > 0 && ady >= adx); break;   /* down  */
        case 2: ok = (dx < 0 && adx >= ady); break;   /* left  */
        case 3: ok = (dx > 0 && adx >= ady); break;   /* right */
        }
        if (ok) {
            long d = (long)dx * dx + (long)dy * dy;
            if (best < 0 || d < bestd) { best = i; bestd = d; }
        }
    }
    if (best >= 0) a->map_selected = best;
}

/* Activate (follow) the keyboard-selected map node -- the Enter key analogue of
 * clicking it. */
void map_activate(Ide* a)
{
    if (!a || !map_have_layout) return;
    if (a->map_selected < 0 || a->map_selected >= map_nsats) return;
    map_sat_follow(a, &map_sats[a->map_selected]);
}
