/*
 * solitaire.c -- Klondike Solitaire (freestanding, ring 3).
 * ==========================================================
 *
 * Full Klondike Solitaire on the custom x86_64 OS game framework.
 *
 * Layout (800 x 600 window):
 *   - Felt-green table background
 *   - 7 tableau columns  (T0..T6)
 *   - 4 foundation piles (F0..F3, top-right area, one per suit)
 *   - 1 stock pile       (top-left)
 *   - 1 waste pile       (next to stock)
 *
 * Rules:
 *   - Standard Klondike: draw 1 from stock
 *   - Tableau: descending rank, alternating color (red/black)
 *   - Foundation: ascending rank (A..K), same suit
 *   - Empty tableau column accepts any King (or empty stack)
 *   - Empty foundation accepts only Ace
 *   - Move single face-up cards or face-up runs
 *   - Auto-flip: when a face-down card is the top of a tableau column,
 *     it is automatically flipped face-up
 *   - Win: all 52 cards on foundations
 *
 * Controls:
 *   Mouse left-click / drag:
 *     - Click stock    -> flip top card(s) to waste
 *     - Click waste    -> pick up top waste card
 *     - Click tableau  -> pick up face-up card (and any run below it)
 *     - Click foundation -> pick up top card (to move back to tableau)
 *     - Release over valid target -> place card(s)
 *     - Double-click face-up card -> auto-move to foundation if legal
 *   N key -> New game
 *   U key -> Undo last move (1 level)
 *   ESC   -> Exit
 *
 * Build:
 *   FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *          -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2"
 *   gcc $FLAGS -c userspace/apps/solitaire/solitaire.c -o /tmp/sol.o
 *   gcc $FLAGS -c userspace/lib/game/game.c             -o /tmp/game.o
 *   gcc $FLAGS -c userspace/lib/wl/wl_client.c          -o /tmp/wlc.o
 *   gcc $FLAGS -c userspace/lib/font/bitfont.c          -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *      -T userspace/userspace.ld \
 *      /tmp/sol.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *      -o /tmp/solitaire.elf
 *   objdump -d /tmp/solitaire.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [SOL] starting
 *   [SOL] win
 *   [SOL] new game
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Serial output (SYS_WRITE, no libc).
 * ========================================================================= */
static inline long _sol_sc3(long n, long a, long b, long c)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return r;
}
static void sol_print(const char *s)
{
    u64 len = 0;
    while (s[len]) len++;
    _sol_sc3(SYS_WRITE, 1, (long)s, (long)len);
}

/* =========================================================================
 * Window / layout constants.
 * ========================================================================= */
#define WIN_W         800
#define WIN_H         600

/* Card dimensions */
#define CARD_W        64
#define CARD_H        88
#define CARD_RADIUS    5

/* Margins & spacing */
#define MARGIN_X      14
#define MARGIN_TOP    14
#define COL_GAP       10          /* horizontal gap between columns */
#define FACE_DOWN_DY  16          /* vertical offset for face-down stacked card */
#define FACE_UP_DY    20          /* vertical offset for face-up stacked card */

/* Tableau columns: 7, starting at x = MARGIN_X, spaced CARD_W + COL_GAP */
#define TAB_COLS      7
#define TAB_Y         (MARGIN_TOP + CARD_H + 18)   /* y start of tableau rows */

/* Foundation: 4 piles, right side */
#define FOUND_X0      (WIN_W - MARGIN_X - 4*CARD_W - 3*COL_GAP)
#define FOUND_Y       MARGIN_TOP

/* Stock and waste: top left */
#define STOCK_X       MARGIN_X
#define STOCK_Y       MARGIN_TOP
#define WASTE_X       (MARGIN_X + CARD_W + COL_GAP)
#define WASTE_Y       MARGIN_TOP

/* =========================================================================
 * Colours.
 * ========================================================================= */
#define COL_FELT      0xFF1A6B2A    /* rich dark green felt */
#define COL_FELT_DARK 0xFF155720    /* darker felt for shadows */
#define COL_CARD_BG   0xFFFFFFFF    /* card face background */
#define COL_CARD_BACK 0xFF1A3FA0    /* card back pattern blue */
#define COL_CARD_BACK2 0xFF102880   /* card back darker */
#define COL_BORDER    0xFF333333
#define COL_RED       0xFFCC1111    /* hearts / diamonds */
#define COL_BLACK     0xFF111111    /* clubs / spades */
#define COL_SLOT      0xFF0F4A1C    /* empty slot outline */
#define COL_SLOT_FILL 0xFF145C21    /* empty slot fill */
#define COL_SHADOW    0x55000000    /* card drop shadow (semi-transparent) */
#define COL_DRAG_TINT 0xCCFFFFFF    /* dragged card tint */
#define COL_HIGHLIGHT 0xFFFFD700    /* gold highlight for valid drop target */
#define COL_HUD_BG    0xFF0D3D14
#define COL_HUD_TEXT  0xFFCCFFCC
#define COL_WIN_BG    0xFF0A2E0F
#define COL_WIN_GOLD  0xFFFFD700
#define COL_BTN_BG    0xFF2A7A35
#define COL_BTN_TEXT  0xFFFFFFFF

/* =========================================================================
 * Card model.
 * ========================================================================= */

/* Suits: 0=Spades(B) 1=Hearts(R) 2=Diamonds(R) 3=Clubs(B) */
#define SUIT_SPADES   0
#define SUIT_HEARTS   1
#define SUIT_DIAMONDS 2
#define SUIT_CLUBS    3

/* Ranks 1..13: Ace=1, Jack=11, Queen=12, King=13 */

typedef struct {
    u8 rank;       /* 1..13 */
    u8 suit;       /* 0..3  */
    u8 face_up;    /* 1=face-up, 0=face-down */
    u8 _pad;
} Card;

/* A card value of 0 means "no card" (empty slot) */
#define NO_CARD 0xFF

static inline int card_is_red(Card c)  { return c.suit == SUIT_HEARTS || c.suit == SUIT_DIAMONDS; }
static inline int card_is_black(Card c){ return c.suit == SUIT_SPADES  || c.suit == SUIT_CLUBS;   }

/* =========================================================================
 * Pile structures.
 * ========================================================================= */
#define MAX_STACK     52

typedef struct {
    Card  cards[MAX_STACK];
    int   count;
} Pile;

static inline void pile_clear(Pile *p)        { p->count = 0; }
static inline int  pile_empty(const Pile *p)  { return p->count == 0; }
static inline Card pile_top(const Pile *p)    { return p->cards[p->count - 1]; }

static inline void pile_push(Pile *p, Card c)
{
    if (p->count < MAX_STACK)
        p->cards[p->count++] = c;
}

static inline Card pile_pop(Pile *p)
{
    Card c = {0,0,0,0};
    if (p->count > 0) c = p->cards[--p->count];
    return c;
}

/* =========================================================================
 * Game state.
 * ========================================================================= */
static Pile stock;
static Pile waste;
static Pile found[4];      /* foundations, one per suit */
static Pile tab[TAB_COLS]; /* tableau columns */

/* Drag state */
static int   drag_active;       /* 1 if dragging */
static Card  drag_cards[13];    /* cards being dragged (run) */
static int   drag_count;        /* number of cards in drag */
static int   drag_mx, drag_my;  /* current mouse position during drag */
static int   drag_ox, drag_oy;  /* offset from card top-left to mouse */

/* Source of drag (for undo / put-back) */
#define SRC_STOCK   0
#define SRC_WASTE   1
#define SRC_FOUND   2   /* +suit index */
#define SRC_TAB     6   /* +col index */

static int   drag_src;
static int   drag_src_idx;      /* col or suit index */
static int   drag_src_card_idx; /* card index within pile */

/* Undo state (single level) */
typedef struct {
    Pile stock, waste, found[4], tab[TAB_COLS];
    int  valid;
} UndoState;
static UndoState undo_state;

/* Win state */
static int   game_won;
static int   win_anim;          /* frame counter for win animation */

/* Move count */
static int   moves;

/* Double-click detection */
static u64   last_click_time;
static int   last_click_x, last_click_y;
#define DBLCLICK_MS 400

/* Mouse state from last frame (for edge detection) */
static int prev_btn;

/* =========================================================================
 * Save / restore undo state.
 * ========================================================================= */
static void save_undo(void)
{
    undo_state.stock = stock;
    undo_state.waste = waste;
    for (int i = 0; i < 4; i++) undo_state.found[i] = found[i];
    for (int i = 0; i < TAB_COLS; i++) undo_state.tab[i] = tab[i];
    undo_state.valid = 1;
}

static void apply_undo(void)
{
    if (!undo_state.valid) return;
    stock = undo_state.stock;
    waste = undo_state.waste;
    for (int i = 0; i < 4; i++) found[i] = undo_state.found[i];
    for (int i = 0; i < TAB_COLS; i++) tab[i] = undo_state.tab[i];
    undo_state.valid = 0;
}

/* =========================================================================
 * Deck shuffle & deal.
 * ========================================================================= */
static void new_game(void)
{
    /* Reset everything */
    pile_clear(&stock);
    pile_clear(&waste);
    for (int i = 0; i < 4; i++) pile_clear(&found[i]);
    for (int i = 0; i < TAB_COLS; i++) pile_clear(&tab[i]);

    game_won = 0;
    win_anim = 0;
    moves    = 0;
    drag_active = 0;
    undo_state.valid = 0;
    last_click_time = 0;
    prev_btn = 0;

    /* Build ordered deck */
    Card deck[52];
    int n = 0;
    for (int s = 0; s < 4; s++) {
        for (int r = 1; r <= 13; r++) {
            deck[n].rank   = (u8)r;
            deck[n].suit   = (u8)s;
            deck[n].face_up = 0;
            deck[n]._pad   = 0;
            n++;
        }
    }

    /* Fisher-Yates shuffle using framework PRNG */
    for (int i = 51; i > 0; i--) {
        int j = g_rand_range(i + 1);
        Card tmp = deck[i];
        deck[i]  = deck[j];
        deck[j]  = tmp;
    }

    /* Deal tableau: col i gets i+1 cards; only top card is face-up */
    int idx = 0;
    for (int col = 0; col < TAB_COLS; col++) {
        for (int row = 0; row <= col; row++) {
            Card c = deck[idx++];
            c.face_up = (row == col) ? 1 : 0;
            pile_push(&tab[col], c);
        }
    }

    /* Remaining 24 cards go face-down into stock */
    for (; idx < 52; idx++) {
        Card c = deck[idx];
        c.face_up = 0;
        pile_push(&stock, c);
    }

    sol_print("[SOL] new game\n");
}

/* =========================================================================
 * Auto-flip top face-down tableau card.
 * ========================================================================= */
static void auto_flip(void)
{
    for (int col = 0; col < TAB_COLS; col++) {
        if (!pile_empty(&tab[col])) {
            Card *top = &tab[col].cards[tab[col].count - 1];
            if (!top->face_up) top->face_up = 1;
        }
    }
}

/* =========================================================================
 * Legal move checks.
 * ========================================================================= */

/* Can card c be placed on top of tableau pile? */
static int tab_can_place(Card c, const Pile *p)
{
    if (pile_empty(p)) {
        /* Only King on empty column */
        return c.rank == 13;
    }
    Card top = pile_top(p);
    if (!top.face_up) return 0;
    /* Descending rank, alternating color */
    return (c.rank == top.rank - 1) &&
           (card_is_red(c) != card_is_red(top));
}

/* Can card c be placed on foundation f? */
static int found_can_place(Card c, int f)
{
    if (pile_empty(&found[f])) {
        return c.rank == 1;   /* Ace starts foundation */
    }
    Card top = pile_top(&found[f]);
    return (c.suit == top.suit) && (c.rank == top.rank + 1);
}

/* Try to auto-move card c to any foundation; return foundation index or -1 */
static int find_foundation_for(Card c)
{
    for (int f = 0; f < 4; f++) {
        if (found_can_place(c, f)) return f;
    }
    return -1;
}

/* =========================================================================
 * Win detection.
 * ========================================================================= */
static int check_win(void)
{
    for (int f = 0; f < 4; f++) {
        if (found[f].count != 13) return 0;
    }
    return 1;
}

/* =========================================================================
 * Column X position (left edge of card).
 * ========================================================================= */
static inline int col_x(int col)
{
    return MARGIN_X + col * (CARD_W + COL_GAP);
}

/* Y position of card at index `idx` within tableau column `col`. */
static int tab_card_y(int col, int idx)
{
    int y = TAB_Y;
    for (int i = 0; i < idx; i++) {
        y += tab[col].cards[i].face_up ? FACE_UP_DY : FACE_DOWN_DY;
    }
    return y;
}

/* Foundation pile positions */
static inline int found_x(int f) { return FOUND_X0 + f * (CARD_W + COL_GAP); }
static inline int found_y(int f) { return FOUND_Y; }

/* =========================================================================
 * Hit-test helpers: return 1 if (mx,my) is inside rect.
 * ========================================================================= */
static inline int hit(int mx, int my, int x, int y, int w, int h)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* =========================================================================
 * Drawing helpers.
 * ========================================================================= */

/* Suit symbol characters using bitfont:
 * We use ASCII letters since the bitfont is 8x16 text-only.
 * S=Spades, H=Hearts, D=Diamonds, C=Clubs
 * We represent suits with single-char abbreviations in the suit color.
 */
static const char *suit_char[4] = { "S", "H", "D", "C" };
static const char *rank_str[14] = {
    "", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"
};

static u32 suit_color(int suit)
{
    return (suit == SUIT_HEARTS || suit == SUIT_DIAMONDS) ? COL_RED : COL_BLACK;
}

/* Draw a single card at (x,y). face_up=1 shows face, 0 shows back. */
static void draw_card(game_t *g, int x, int y, Card c, int face_up, int highlight)
{
    /* Shadow */
    g_fill_rect(g, x + 3, y + 3, CARD_W, CARD_H, 0x44000000);

    if (face_up) {
        /* Card face */
        g_rounded_rect(g, x, y, CARD_W, CARD_H, CARD_RADIUS, COL_CARD_BG);
        /* Border */
        u32 border = highlight ? COL_HIGHLIGHT : COL_BORDER;
        /* Draw rounded border outline (approximate with rect outline) */
        g_rect(g, x, y, CARD_W, CARD_H, border);
        g_rect(g, x+1, y+1, CARD_W-2, CARD_H-2, border);

        u32 col = suit_color(c.suit);

        /* Top-left rank + suit */
        const char *rs = rank_str[c.rank];
        g_text(g, x + 4, y + 3,  rs, col);
        g_text(g, x + 4, y + 13, suit_char[c.suit], col);

        /* Bottom-right rank + suit (rotated 180 -- we just mirror top-left) */
        /* Compute text widths (8px per char) */
        int rlen = 0; while (rs[rlen]) rlen++;
        g_text(g, x + CARD_W - 4 - rlen * 8, y + CARD_H - 28, rs, col);
        g_text(g, x + CARD_W - 12,            y + CARD_H - 18, suit_char[c.suit], col);

        /* Center: large suit symbol */
        /* Draw a larger center decoration using filled shapes */
        int cx = x + CARD_W / 2;
        int cy = y + CARD_H / 2;

        if (c.suit == SUIT_HEARTS) {
            /* Heart: two circles + triangle */
            g_circle(g, cx - 7, cy - 4, 7, col);
            g_circle(g, cx + 7, cy - 4, 7, col);
            /* Triangle below */
            for (int i = 0; i <= 14; i++) {
                g_fill_rect(g, cx - (14 - i), cy - 1 + i, (14 - i) * 2, 1, col);
            }
        } else if (c.suit == SUIT_DIAMONDS) {
            /* Diamond: rotated square */
            for (int i = 0; i <= 11; i++) {
                int hw = (i <= 11/2) ? i : (11 - i);
                g_fill_rect(g, cx - hw, cy - 11 + i*2, hw*2, 2, col);
            }
        } else if (c.suit == SUIT_SPADES) {
            /* Spade: inverted heart + stem */
            g_circle(g, cx - 6, cy + 2, 6, col);
            g_circle(g, cx + 6, cy + 2, 6, col);
            /* Inverted triangle on top */
            for (int i = 0; i <= 11; i++) {
                g_fill_rect(g, cx - i, cy - 11 + i, i*2, 1, col);
            }
            /* Stem */
            g_fill_rect(g, cx - 3, cy + 8, 6, 7, col);
            g_fill_rect(g, cx - 7, cy + 14, 14, 3, col);
        } else {
            /* Clubs: three circles + stem */
            g_circle(g, cx,     cy - 4,  6, col);
            g_circle(g, cx - 7, cy + 4,  6, col);
            g_circle(g, cx + 7, cy + 4,  6, col);
            g_fill_rect(g, cx - 3, cy + 8, 6, 7, col);
            g_fill_rect(g, cx - 6, cy + 14, 12, 3, col);
        }

    } else {
        /* Card back */
        g_rounded_rect(g, x, y, CARD_W, CARD_H, CARD_RADIUS, COL_CARD_BACK);
        g_rect(g, x, y, CARD_W, CARD_H, COL_BORDER);
        /* Decorative back pattern: grid of small rectangles */
        for (int py = y + 6; py < y + CARD_H - 6; py += 7) {
            for (int px = x + 5; px < x + CARD_W - 5; px += 7) {
                g_fill_rect(g, px, py, 5, 5, COL_CARD_BACK2);
            }
        }
        /* Inner border */
        g_rect(g, x + 4, y + 4, CARD_W - 8, CARD_H - 8, COL_CARD_BACK2);
    }
}

/* Draw an empty card slot (dashed outline). */
static void draw_slot(game_t *g, int x, int y, int show_suit, int suit)
{
    g_rounded_rect(g, x, y, CARD_W, CARD_H, CARD_RADIUS, COL_SLOT_FILL);
    g_rect(g, x, y, CARD_W, CARD_H, COL_SLOT);
    g_rect(g, x+1, y+1, CARD_W-2, CARD_H-2, COL_SLOT);
    if (show_suit) {
        /* Show expected suit letter faintly */
        u32 col = (suit == SUIT_HEARTS || suit == SUIT_DIAMONDS) ?
                  0xFF4A8858 : 0xFF3A7748;
        g_text_center(g, x + CARD_W/2, y + CARD_H/2, suit_char[suit], col);
        /* Also show A for Ace hint */
        g_text_center(g, x + CARD_W/2, y + CARD_H/2 - 12, "A", col);
    }
}

/* =========================================================================
 * Drag: pick up cards from a pile.
 * ========================================================================= */

/* Begin dragging a run from tab[col] starting at card index `from`. */
static void drag_from_tab(int col, int from, int mx, int my)
{
    int cx = col_x(col);
    int cy = tab_card_y(col, from);

    drag_count = 0;
    for (int i = from; i < tab[col].count; i++) {
        drag_cards[drag_count++] = tab[col].cards[i];
    }
    /* Remove them from the tableau */
    tab[col].count = from;

    drag_active   = 1;
    drag_src      = SRC_TAB + col;
    drag_src_idx  = col;
    drag_src_card_idx = from;
    drag_mx = mx;
    drag_my = my;
    drag_ox = mx - cx;
    drag_oy = my - cy;
}

static void drag_from_waste(int mx, int my)
{
    if (pile_empty(&waste)) return;
    drag_cards[0] = pile_pop(&waste);
    drag_count  = 1;
    drag_active = 1;
    drag_src    = SRC_WASTE;
    drag_src_idx = 0;
    drag_mx = mx; drag_my = my;
    drag_ox = mx - WASTE_X;
    drag_oy = my - WASTE_Y;
}

static void drag_from_found(int f, int mx, int my)
{
    if (pile_empty(&found[f])) return;
    drag_cards[0] = pile_pop(&found[f]);
    drag_count  = 1;
    drag_active = 1;
    drag_src    = SRC_FOUND + f;
    drag_src_idx = f;
    drag_mx = mx; drag_my = my;
    drag_ox = mx - found_x(f);
    drag_oy = my - found_y(f);
}

/* Cancel drag: return cards to source. */
static void drag_cancel(void)
{
    if (!drag_active) return;
    int src = drag_src;
    if (src == SRC_WASTE) {
        /* Push back onto waste */
        pile_push(&waste, drag_cards[0]);
    } else if (src >= SRC_FOUND && src < SRC_TAB) {
        int f = src - SRC_FOUND;
        pile_push(&found[f], drag_cards[0]);
    } else {
        /* Tableau: push all back */
        int col = src - SRC_TAB;
        for (int i = 0; i < drag_count; i++) {
            pile_push(&tab[col], drag_cards[i]);
        }
    }
    drag_active = 0;
}

/* =========================================================================
 * Drop: attempt to place dragged cards onto a target.
 * Returns 1 on success.
 * ========================================================================= */
static int drag_drop(int mx, int my)
{
    if (!drag_active) return 0;

    Card top = drag_cards[0];  /* bottom card of the run (lowest rank) */

    /* Try foundations (only single-card drags) */
    if (drag_count == 1) {
        for (int f = 0; f < 4; f++) {
            int fx = found_x(f), fy = found_y(f);
            if (hit(mx, my, fx, fy, CARD_W, CARD_H)) {
                if (found_can_place(top, f)) {
                    save_undo();
                    pile_push(&found[f], top);
                    drag_active = 0;
                    auto_flip();
                    moves++;
                    g_beep(880, 40);
                    return 1;
                }
            }
        }
    }

    /* Try tableau columns */
    for (int col = 0; col < TAB_COLS; col++) {
        int tx = col_x(col);
        /* Compute y range of this column */
        int ty, th;
        if (pile_empty(&tab[col])) {
            ty = TAB_Y;
            th = CARD_H;
        } else {
            ty = TAB_Y;
            /* Last card position */
            int last_y = tab_card_y(col, tab[col].count - 1);
            th = last_y - TAB_Y + CARD_H;
        }

        if (hit(mx, my, tx, ty, CARD_W + 4, th + FACE_UP_DY)) {
            if (tab_can_place(top, &tab[col])) {
                save_undo();
                for (int i = 0; i < drag_count; i++) {
                    pile_push(&tab[col], drag_cards[i]);
                }
                drag_active = 0;
                auto_flip();
                moves++;
                g_beep(660, 30);
                return 1;
            }
        }
    }

    return 0;
}

/* =========================================================================
 * Auto-move a single card to foundation.
 * ========================================================================= */
static int auto_to_foundation(Card c, int src, int src_col)
{
    int f = find_foundation_for(c);
    if (f < 0) return 0;

    save_undo();

    /* Remove from source */
    if (src == SRC_WASTE) {
        pile_pop(&waste);
    } else if (src >= SRC_FOUND && src < SRC_TAB) {
        pile_pop(&found[src - SRC_FOUND]);
    } else {
        pile_pop(&tab[src_col]);
    }

    pile_push(&found[f], c);
    auto_flip();
    moves++;
    g_beep(1047, 50);
    return 1;
}

/* =========================================================================
 * Stock click: flip one card to waste (cycle through when empty).
 * ========================================================================= */
static void stock_click(void)
{
    if (pile_empty(&stock)) {
        /* Recycle waste back to stock */
        if (pile_empty(&waste)) return;
        save_undo();
        while (!pile_empty(&waste)) {
            Card c = pile_pop(&waste);
            c.face_up = 0;
            pile_push(&stock, c);
        }
        moves++;
        g_beep(440, 30);
    } else {
        save_undo();
        Card c = pile_pop(&stock);
        c.face_up = 1;
        pile_push(&waste, c);
        moves++;
        g_beep(523, 25);
    }
}

/* =========================================================================
 * Input processing.
 * ========================================================================= */
static void process_input(game_t *g)
{
    int mx, my, btn;
    game_mouse(g, &mx, &my, &btn);

    int click  = (btn & MOUSE_LEFT) && !(prev_btn & MOUSE_LEFT);  /* press edge */
    int release = !(btn & MOUSE_LEFT) && (prev_btn & MOUSE_LEFT); /* release edge */

    /* Update drag position */
    if (drag_active) {
        drag_mx = mx;
        drag_my = my;
    }

    if (click && !drag_active && !game_won) {
        u64 now = game_ticks();
        int dbl = (int)(now - last_click_time) < DBLCLICK_MS &&
                  g_abs(mx - last_click_x) < 20 &&
                  g_abs(my - last_click_y) < 20;
        last_click_time = now;
        last_click_x = mx;
        last_click_y = my;

        /* --- Stock click --- */
        if (hit(mx, my, STOCK_X, STOCK_Y, CARD_W, CARD_H)) {
            stock_click();
            goto done_click;
        }

        /* --- Waste click: pick up top card --- */
        if (!pile_empty(&waste) && hit(mx, my, WASTE_X, WASTE_Y, CARD_W, CARD_H)) {
            Card top = pile_top(&waste);
            if (dbl) {
                auto_to_foundation(top, SRC_WASTE, 0);
            } else {
                drag_from_waste(mx, my);
            }
            goto done_click;
        }

        /* --- Foundation click: pick up card to move back --- */
        for (int f = 0; f < 4; f++) {
            if (!pile_empty(&found[f]) &&
                hit(mx, my, found_x(f), found_y(f), CARD_W, CARD_H)) {
                if (dbl) {
                    /* double-click on foundation: no useful action */
                } else {
                    drag_from_found(f, mx, my);
                }
                goto done_click;
            }
        }

        /* --- Tableau click --- */
        for (int col = 0; col < TAB_COLS; col++) {
            int tx = col_x(col);
            /* Hit-test each card from top to bottom */
            for (int i = tab[col].count - 1; i >= 0; i--) {
                int cy_card = tab_card_y(col, i);
                /* Only test the visible portion of each card */
                int visible_h = (i == tab[col].count - 1) ? CARD_H :
                    (tab[col].cards[i].face_up ? FACE_UP_DY : FACE_DOWN_DY);
                if (hit(mx, my, tx, cy_card, CARD_W, visible_h)) {
                    Card c = tab[col].cards[i];
                    if (!c.face_up) goto done_click; /* can't pick face-down */
                    if (dbl && i == tab[col].count - 1) {
                        /* Double-click top card: try auto-move to foundation */
                        auto_to_foundation(c, SRC_TAB + col, col);
                    } else {
                        /* Drag from this card downward */
                        drag_from_tab(col, i, mx, my);
                    }
                    goto done_click;
                }
            }
        }
        done_click:;
    }

    if (release) {
        if (drag_active) {
            if (!drag_drop(mx, my)) {
                drag_cancel();
                g_beep(220, 30);
            }
        }
    }

    prev_btn = btn;
}

/* =========================================================================
 * Drawing: full scene.
 * ========================================================================= */

/* Draw a "New Game" button in the HUD */
static void draw_button(game_t *g, int x, int y, int w, int h, const char *label)
{
    g_rounded_rect(g, x, y, w, h, 4, COL_BTN_BG);
    g_rect(g, x, y, w, h, 0xFF4AAA55);
    g_text_center(g, x + w/2, y + h/2, label, COL_BTN_TEXT);
}

static void draw_scene(game_t *g)
{
    /* Background */
    g_clear(g, COL_FELT);

    /* Subtle felt texture: darker horizontal lines */
    for (int y = 0; y < WIN_H; y += 4) {
        g_fill_rect(g, 0, y, WIN_W, 1, 0x0A000000);
    }

    /* ---- HUD bar ---- */
    g_fill_rect(g, 0, 0, WIN_W, MARGIN_TOP - 2, COL_HUD_BG);

    /* Moves counter */
    g_text(g, WIN_W - 120, 1, "MOVES:", COL_HUD_TEXT);
    g_draw_int(g, WIN_W - 50, 1, moves, COL_HUD_TEXT);

    /* Buttons */
    draw_button(g, WIN_W/2 - 40, 1, 80, MARGIN_TOP - 3, "N=New");

    /* Undo hint */
    g_text(g, 2, 1, "U=Undo  ESC=Quit", COL_HUD_TEXT);

    /* ---- Stock ---- */
    if (pile_empty(&stock)) {
        draw_slot(g, STOCK_X, STOCK_Y, 0, 0);
        /* Recycling arrow hint */
        g_text_center(g, STOCK_X + CARD_W/2, STOCK_Y + CARD_H/2, ">>", 0xFF4AAA55);
    } else {
        /* Show count of remaining stock cards (face-down) */
        Card dummy = { 0, 0, 0, 0 };
        draw_card(g, STOCK_X, STOCK_Y, dummy, 0, 0);
        /* Small count overlay */
        g_fill_rect(g, STOCK_X + CARD_W - 20, STOCK_Y + 2, 18, 14, 0xAA000000);
        g_draw_int(g, STOCK_X + CARD_W - 18, STOCK_Y + 3, stock.count, 0xFFFFFFFF);
    }

    /* ---- Waste ---- */
    if (pile_empty(&waste)) {
        draw_slot(g, WASTE_X, WASTE_Y, 0, 0);
    } else {
        /* Show up to 3 waste cards fanned left-to-right; topmost drawn last */
        int show = waste.count < 3 ? waste.count : 3;
        for (int i = show - 1; i >= 0; i--) {
            int off = (show - 1 - i) * 16;
            Card c = waste.cards[waste.count - 1 - i];
            draw_card(g, WASTE_X + off, WASTE_Y, c, 1, 0);
        }
    }

    /* ---- Foundations ---- */
    for (int f = 0; f < 4; f++) {
        int fx = found_x(f), fy = found_y(f);
        if (pile_empty(&found[f])) {
            draw_slot(g, fx, fy, 1, f);
        } else {
            Card top = pile_top(&found[f]);
            draw_card(g, fx, fy, top, 1, 0);
            /* Count overlay */
            g_fill_rect(g, fx + CARD_W - 20, fy + 2, 18, 14, 0xAA000000);
            g_draw_int(g, fx + CARD_W - 18, fy + 3, found[f].count, 0xFFFFFFFF);
        }
    }

    /* ---- Tableau ---- */
    for (int col = 0; col < TAB_COLS; col++) {
        int tx = col_x(col);
        if (pile_empty(&tab[col])) {
            draw_slot(g, tx, TAB_Y, 0, 0);
        } else {
            for (int i = 0; i < tab[col].count; i++) {
                int cy = tab_card_y(col, i);
                /* Clip cards that go too far down */
                if (cy > WIN_H - 20) break;
                /* Skip dragged cards */
                if (drag_active && drag_src == SRC_TAB + col &&
                    i >= drag_src_card_idx)
                    continue;
                draw_card(g, tx, cy, tab[col].cards[i], tab[col].cards[i].face_up, 0);
            }
        }
    }

    /* ---- Dragged cards ---- */
    if (drag_active) {
        int dx = drag_mx - drag_ox;
        int dy = drag_my - drag_oy;
        for (int i = 0; i < drag_count; i++) {
            int cy = dy + i * FACE_UP_DY;
            /* Slight shadow offset to show lift */
            g_fill_rect(g, dx + 5, cy + 5, CARD_W, CARD_H, 0x55000000);
            draw_card(g, dx, cy, drag_cards[i], drag_cards[i].face_up, 0);
        }
        /* Highlight potential drop targets */
        /* Foundation highlight */
        if (drag_count == 1) {
            for (int f = 0; f < 4; f++) {
                if (found_can_place(drag_cards[0], f)) {
                    int fx = found_x(f), fy = found_y(f);
                    g_rect(g, fx - 2, fy - 2, CARD_W + 4, CARD_H + 4, COL_HIGHLIGHT);
                    g_rect(g, fx - 3, fy - 3, CARD_W + 6, CARD_H + 6, COL_HIGHLIGHT);
                }
            }
        }
        /* Tableau highlight */
        for (int col = 0; col < TAB_COLS; col++) {
            if (tab_can_place(drag_cards[0], &tab[col])) {
                int tx = col_x(col);
                int ty = pile_empty(&tab[col]) ? TAB_Y :
                         tab_card_y(col, tab[col].count - 1);
                g_rect(g, tx - 2, ty - 2, CARD_W + 4, CARD_H + 4, COL_HIGHLIGHT);
            }
        }
    }

    /* ---- Win overlay ---- */
    if (game_won) {
        /* Dim the table */
        g_fill_rect(g, 0, 0, WIN_W, WIN_H, 0xAA000000);

        int bx = WIN_W/2 - 160, by = WIN_H/2 - 80;
        g_rounded_rect(g, bx, by, 320, 160, 12, COL_WIN_BG);
        g_rect(g, bx, by, 320, 160, COL_WIN_GOLD);
        g_rect(g, bx+1, by+1, 318, 158, COL_WIN_GOLD);

        g_text_center(g, WIN_W/2, WIN_H/2 - 44, "YOU WIN!", COL_WIN_GOLD);

        /* Animated stars using circles */
        int frame = win_anim;
        for (int i = 0; i < 8; i++) {
            int angle = (frame * 3 + i * 32) & 255;
            int rx = g_cos(angle) * 120 / 65536;
            int ry = g_sin(angle) * 50  / 65536;
            u32 col = (i & 1) ? COL_WIN_GOLD : 0xFFFFFFAA;
            g_circle(g, WIN_W/2 + rx, WIN_H/2 + ry, 5, col);
        }

        /* Move count */
        g_text_center(g, WIN_W/2, WIN_H/2 - 10, "Moves:", 0xFFFFFFFF);
        g_draw_int(g, WIN_W/2 + 40, WIN_H/2 - 10, moves, 0xFFFFFF88);

        g_text_center(g, WIN_W/2, WIN_H/2 + 30, "Press N for new game", 0xFFCCCCCC);
        g_text_center(g, WIN_W/2, WIN_H/2 + 50, "Congratulations!", 0xFFFFD700);
    }
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    sol_print("[SOL] starting\n");

    game_t *g = game_open(WIN_W, WIN_H, "Klondike Solitaire");
    if (!g) {
        sol_print("[SOL] game_open failed\n");
        /* Syscall exit */
        asm volatile("mov $60, %%rax; xor %%rdi, %%rdi; syscall" ::: "rax", "rdi");
    }

    /* Seed RNG from ticks */
    u64 t = game_ticks();
    g_srand((u32)(t ^ (t >> 32)));

    new_game();

    while (game_frame_begin(g)) {

        /* Key input */
        if (game_key_pressed(g, KEY_ESC)) break;

        if (game_key_pressed(g, KEY_N)) {
            u64 ts = game_ticks();
            g_srand((u32)(ts ^ (ts >> 17)));
            new_game();
        }

        if (game_key_pressed(g, KEY_U)) {
            apply_undo();
            drag_active = 0;
        }

        /* Mouse input */
        process_input(g);

        /* Win check */
        if (!game_won && check_win()) {
            game_won = 1;
            win_anim = 0;
            sol_print("[SOL] win\n");
            g_beep(1047, 100);
            g_beep(1319, 100);
            g_beep(1568, 200);
        }
        if (game_won) win_anim++;

        /* Render */
        draw_scene(g);

        game_present(g);
        game_sync(g);
    }

    sol_print("[SOL] exit\n");
    asm volatile("mov $60, %%rax; xor %%rdi, %%rdi; syscall" ::: "rax", "rdi");
}
