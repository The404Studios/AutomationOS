/*
 * chess.c -- Full chess game with AI opponent (freestanding, ring 3).
 * ====================================================================
 *
 * 560x620 window "Chess".
 *   - 8x8 board with alternating square colours (light/dark).
 *   - All pieces drawn with g_circle / g_fill_rect / g_text primitives.
 *   - Full legal move rules: pawn (two-step, en passant, promotion to Q),
 *     knight, bishop, rook, queen, king, castling (kingside + queenside).
 *   - Check detection; moves that leave own king in check are filtered out.
 *   - Checkmate + stalemate detection.
 *   - Click a piece to highlight legal squares; click destination to move.
 *   - Player = White, AI = Black.
 *   - AI: minimax with alpha-beta pruning, depth 3, material + PST eval.
 *   - Status bar: turn, check/checkmate/stalemate, captured pieces.
 *   - N key: new game.  ESC: exit.
 *   - g_beep on move/capture/check.
 *
 * Build (from repo root, WSL Arch):
 *   FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *          -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2"
 *   gcc $FLAGS -c userspace/apps/chess/chess.c      -o /tmp/chess.o
 *   gcc $FLAGS -c userspace/lib/game/game.c          -o /tmp/game.o
 *   gcc $FLAGS -c userspace/lib/wl/wl_client.c       -o /tmp/wlc.o
 *   gcc $FLAGS -c userspace/lib/font/bitfont.c        -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/chess.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/chess.elf
 *   objdump -d /tmp/chess.elf | grep fs:0x28   # MUST be empty
 */

#include "../../lib/game/game.h"

/* AOS syscall numbers (game.h only exposes SYS_WRITE). These match
 * kernel/include/syscall.h -- NOTE: SYS_EXIT=0, and syscall 1 is SYS_FORK,
 * so an exit MUST use rax=0, never rax=1. */
#ifndef SYS_EXIT
#define SYS_EXIT 0
#endif

/* =========================================================================
 * Serial output helper (no libc).
 * ========================================================================= */
static inline long _sc3(long n, long a, long b, long c)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return r;
}
static void chess_print(const char *s)
{
    u64 len = 0;
    while (s[len]) len++;
    _sc3(SYS_WRITE, 1, (long)s, (long)len);
}

/* Terminate the process cleanly. AOS SYS_EXIT == 0 (rax=1 would FORK!). */
static void chess_exit(int code)
{
    _sc3(SYS_EXIT, (long)code, 0, 0);
    for (;;) { /* unreachable; guard against return */ }
}

/* =========================================================================
 * Window / board layout.
 * ========================================================================= */
#define WIN_W        560
#define WIN_H        620

/* Board: 8x8 squares, each SQ_SZ pixels, with a border offset. */
#define SQ_SZ        60
#define BOARD_X      28    /* left edge of board */
#define BOARD_Y      28    /* top edge of board  */
#define BOARD_W      (SQ_SZ * 8)
#define BOARD_H      (SQ_SZ * 8)

/* Status bar below board */
#define STATUS_Y     (BOARD_Y + BOARD_H + 8)
#define STATUS_H     (WIN_H - STATUS_Y - 4)

/* =========================================================================
 * Piece / colour constants.
 * ========================================================================= */
#define EMPTY  0

/* Piece types (1-6) */
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

/* Colour flags packed into the piece byte:
 *   bits 0-2 = piece type (0=empty, 1-6)
 *   bit  3   = colour (0=white, 1=black)
 */
#define WHITE  0
#define BLACK  8

#define PIECE(b)  ((b) & 7)
#define COLOUR(b) ((b) & 8)
#define IS_WHITE(b) (((b) != EMPTY) && (COLOUR(b) == WHITE))
#define IS_BLACK(b) (((b) != EMPTY) && (COLOUR(b) == BLACK))

/* =========================================================================
 * Game state.
 * ========================================================================= */
typedef struct {
    /* board[rank][file], rank 0 = row 8 (black's back rank), rank 7 = row 1 (white's back rank) */
    u8 board[8][8];

    /* Castling rights */
    int w_castle_k;   /* white can castle kingside  */
    int w_castle_q;   /* white can castle queenside */
    int b_castle_k;
    int b_castle_q;

    /* En passant target square (-1 if none); encodes as rank*8+file */
    int ep_sq;

    /* Whose turn: WHITE or BLACK */
    int turn;

    /* Check / game-over flags */
    int in_check;
    int checkmate;
    int stalemate;

    /* Captured pieces (material counts for display) */
    int white_captured[7];  /* [piece_type] = count */
    int black_captured[7];

    /* UI selection */
    int sel_rank;    /* currently selected square (-1 = none) */
    int sel_file;

    /* Legal move list for selected piece */
    /* Encoded as (to_rank << 3) | to_file  -- up to 64 entries */
    u8  legal[64];
    int legal_count;

    /* AI thinking */
    int ai_thinking;
    u64 ai_start_tick;

    /* Game-over overlay shown */
    int game_over_shown;

    /* Move count (half-moves / ply) */
    int ply;
} chess_t;

static chess_t gs;

/* =========================================================================
 * Move encoding for move generation / minimax.
 *
 * We pack a move into a 32-bit integer:
 *   bits  0-2: from_file
 *   bits  3-5: from_rank
 *   bits  6-8: to_file
 *   bits 9-11: to_rank
 *   bits 12-14: promotion piece type (0 = none, QUEEN=5 for auto-promo)
 *   bit  15: castling flag (informational)
 *   bit  16: en-passant flag (informational)
 * ========================================================================= */
#define MOVE_FROM_FILE(m)  ((m)        & 7)
#define MOVE_FROM_RANK(m)  (((m) >> 3) & 7)
#define MOVE_TO_FILE(m)    (((m) >> 6) & 7)
#define MOVE_TO_RANK(m)    (((m) >> 9) & 7)
#define MOVE_PROMO(m)      (((m) >> 12) & 7)
#define MOVE_IS_CASTLE(m)  (((m) >> 15) & 1)
#define MOVE_IS_EP(m)      (((m) >> 16) & 1)

static inline int make_move(int fr, int ff, int tr, int tf,
                             int promo, int castle, int ep)
{
    return (fr & 7) << 3 | (ff & 7) |
           (tr & 7) << 9 | (tf & 7) << 6 |
           (promo & 7) << 12 |
           (castle ? (1 << 15) : 0) |
           (ep    ? (1 << 16) : 0);
}

/* =========================================================================
 * Board helpers.
 * ========================================================================= */
static inline int in_bounds(int r, int f)
{
    return (unsigned)r < 8 && (unsigned)f < 8;
}

static inline int enemy(int piece, int my_colour)
{
    return piece != EMPTY && COLOUR(piece) != my_colour;
}

/* =========================================================================
 * Board initialisation.
 * ========================================================================= */
static void board_reset(chess_t *g)
{
    /* Clear */
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++)
            g->board[r][f] = EMPTY;

    /* Black pieces (rank 0 = row 8) */
    g->board[0][0] = BLACK | ROOK;
    g->board[0][1] = BLACK | KNIGHT;
    g->board[0][2] = BLACK | BISHOP;
    g->board[0][3] = BLACK | QUEEN;
    g->board[0][4] = BLACK | KING;
    g->board[0][5] = BLACK | BISHOP;
    g->board[0][6] = BLACK | KNIGHT;
    g->board[0][7] = BLACK | ROOK;
    for (int f = 0; f < 8; f++)
        g->board[1][f] = BLACK | PAWN;

    /* White pieces (rank 7 = row 1) */
    g->board[7][0] = WHITE | ROOK;
    g->board[7][1] = WHITE | KNIGHT;
    g->board[7][2] = WHITE | BISHOP;
    g->board[7][3] = WHITE | QUEEN;
    g->board[7][4] = WHITE | KING;
    g->board[7][5] = WHITE | BISHOP;
    g->board[7][6] = WHITE | KNIGHT;
    g->board[7][7] = WHITE | ROOK;
    for (int f = 0; f < 8; f++)
        g->board[6][f] = WHITE | PAWN;

    g->w_castle_k = 1;
    g->w_castle_q = 1;
    g->b_castle_k = 1;
    g->b_castle_q = 1;
    g->ep_sq      = -1;
    g->turn       = WHITE;
    g->in_check   = 0;
    g->checkmate  = 0;
    g->stalemate  = 0;
    g->sel_rank   = -1;
    g->sel_file   = -1;
    g->legal_count= 0;
    g->ai_thinking= 0;
    g->game_over_shown = 0;
    g->ply        = 0;

    for (int i = 0; i < 7; i++) {
        g->white_captured[i] = 0;
        g->black_captured[i] = 0;
    }
}

/* =========================================================================
 * Attack detection.
 *
 * is_attacked(board, rank, file, by_colour):
 *   Returns 1 if the square (rank, file) is attacked by any piece of
 *   by_colour.
 * ========================================================================= */
static int is_attacked(u8 b[8][8], int r, int f, int by_colour)
{
    /* Pawns */
    if (by_colour == WHITE) {
        /* White pawns attack upward (decreasing rank) */
        if (in_bounds(r+1, f-1) && b[r+1][f-1] == (WHITE|PAWN)) return 1;
        if (in_bounds(r+1, f+1) && b[r+1][f+1] == (WHITE|PAWN)) return 1;
    } else {
        /* Black pawns attack downward (increasing rank) */
        if (in_bounds(r-1, f-1) && b[r-1][f-1] == (BLACK|PAWN)) return 1;
        if (in_bounds(r-1, f+1) && b[r-1][f+1] == (BLACK|PAWN)) return 1;
    }

    /* Knights */
    static const int knd[8][2] = {
        {-2,-1},{-2,1},{-1,-2},{-1,2},
        { 1,-2},{ 1,2},{ 2,-1},{ 2,1}
    };
    u8 kn_piece = (u8)(by_colour | KNIGHT);
    for (int i = 0; i < 8; i++) {
        int nr = r + knd[i][0], nf = f + knd[i][1];
        if (in_bounds(nr, nf) && b[nr][nf] == kn_piece) return 1;
    }

    /* Sliding: rook / queen (orthogonal) */
    static const int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int d = 0; d < 4; d++) {
        int nr = r + orth[d][0], nf = f + orth[d][1];
        while (in_bounds(nr, nf)) {
            u8 sq = b[nr][nf];
            if (sq != EMPTY) {
                if (COLOUR(sq) == (u8)by_colour &&
                    (PIECE(sq) == ROOK || PIECE(sq) == QUEEN)) return 1;
                break;
            }
            nr += orth[d][0]; nf += orth[d][1];
        }
    }

    /* Sliding: bishop / queen (diagonal) */
    static const int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int d = 0; d < 4; d++) {
        int nr = r + diag[d][0], nf = f + diag[d][1];
        while (in_bounds(nr, nf)) {
            u8 sq = b[nr][nf];
            if (sq != EMPTY) {
                if (COLOUR(sq) == (u8)by_colour &&
                    (PIECE(sq) == BISHOP || PIECE(sq) == QUEEN)) return 1;
                break;
            }
            nr += diag[d][0]; nf += diag[d][1];
        }
    }

    /* King */
    u8 kg_piece = (u8)(by_colour | KING);
    for (int dr = -1; dr <= 1; dr++)
        for (int df = -1; df <= 1; df++) {
            if (dr == 0 && df == 0) continue;
            int nr = r + dr, nf = f + df;
            if (in_bounds(nr, nf) && b[nr][nf] == kg_piece) return 1;
        }

    return 0;
}

/* Find king of given colour; returns rank in *kr, file in *kf. */
static void find_king(u8 b[8][8], int colour, int *kr, int *kf)
{
    u8 kp = (u8)(colour | KING);
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++)
            if (b[r][f] == kp) { *kr = r; *kf = f; return; }
    *kr = -1; *kf = -1;
}

/* =========================================================================
 * Move generation (pseudo-legal + legal filtering).
 * ========================================================================= */

/* Move buffer -- sufficient for any position */
#define MAX_MOVES 256

typedef struct {
    int mv[MAX_MOVES];
    int count;
} movelist_t;

static void add_move(movelist_t *ml, int m)
{
    if (ml->count < MAX_MOVES)
        ml->mv[ml->count++] = m;
}

/* Generate pseudo-legal moves for one piece at (r,f) */
static void gen_piece_moves(u8 b[8][8], int r, int f,
                             int colour, int ep_sq, movelist_t *ml,
                             int ck, int cq)  /* castling rights */
{
    u8 pc = b[r][f];
    int type = PIECE(pc);
    int opp  = (colour == WHITE) ? BLACK : WHITE;

    switch (type) {
    case PAWN: {
        int dir = (colour == WHITE) ? -1 : 1;  /* white moves up (rank-), black moves down (rank+) */
        int start_rank = (colour == WHITE) ? 6 : 1;
        int promo_rank = (colour == WHITE) ? 0 : 7;

        /* One step forward */
        int nr = r + dir;
        if (in_bounds(nr, f) && b[nr][f] == EMPTY) {
            if (nr == promo_rank)
                add_move(ml, make_move(r, f, nr, f, QUEEN, 0, 0));
            else {
                add_move(ml, make_move(r, f, nr, f, 0, 0, 0));
                /* Two steps from start */
                if (r == start_rank && b[nr + dir][f] == EMPTY)
                    add_move(ml, make_move(r, f, nr + dir, f, 0, 0, 0));
            }
        }

        /* Captures (diagonal) */
        for (int df = -1; df <= 1; df += 2) {
            int nf2 = f + df;
            if (!in_bounds(nr, nf2)) continue;
            /* Normal capture */
            if (b[nr][nf2] != EMPTY && COLOUR(b[nr][nf2]) == (u8)opp) {
                if (nr == promo_rank)
                    add_move(ml, make_move(r, f, nr, nf2, QUEEN, 0, 0));
                else
                    add_move(ml, make_move(r, f, nr, nf2, 0, 0, 0));
            }
            /* En passant */
            if (ep_sq >= 0) {
                int ep_r = ep_sq >> 3, ep_f = ep_sq & 7;
                if (nr == ep_r && nf2 == ep_f)
                    add_move(ml, make_move(r, f, nr, nf2, 0, 0, 1));
            }
        }
        break;
    }

    case KNIGHT: {
        static const int nd[8][2] = {
            {-2,-1},{-2,1},{-1,-2},{-1,2},
            { 1,-2},{ 1,2},{ 2,-1},{ 2,1}
        };
        for (int i = 0; i < 8; i++) {
            int nr2 = r + nd[i][0], nf2 = f + nd[i][1];
            if (in_bounds(nr2, nf2) &&
                (b[nr2][nf2] == EMPTY || COLOUR(b[nr2][nf2]) == (u8)opp))
                add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
        }
        break;
    }

    case BISHOP: {
        static const int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (int d = 0; d < 4; d++) {
            int nr2 = r + diag[d][0], nf2 = f + diag[d][1];
            while (in_bounds(nr2, nf2)) {
                if (b[nr2][nf2] == EMPTY)
                    add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                else {
                    if (COLOUR(b[nr2][nf2]) == (u8)opp)
                        add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                    break;
                }
                nr2 += diag[d][0]; nf2 += diag[d][1];
            }
        }
        break;
    }

    case ROOK: {
        static const int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (int d = 0; d < 4; d++) {
            int nr2 = r + orth[d][0], nf2 = f + orth[d][1];
            while (in_bounds(nr2, nf2)) {
                if (b[nr2][nf2] == EMPTY)
                    add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                else {
                    if (COLOUR(b[nr2][nf2]) == (u8)opp)
                        add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                    break;
                }
                nr2 += orth[d][0]; nf2 += orth[d][1];
            }
        }
        break;
    }

    case QUEEN: {
        static const int all8[8][2] = {
            {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
        };
        for (int d = 0; d < 8; d++) {
            int nr2 = r + all8[d][0], nf2 = f + all8[d][1];
            while (in_bounds(nr2, nf2)) {
                if (b[nr2][nf2] == EMPTY)
                    add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                else {
                    if (COLOUR(b[nr2][nf2]) == (u8)opp)
                        add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
                    break;
                }
                nr2 += all8[d][0]; nf2 += all8[d][1];
            }
        }
        break;
    }

    case KING: {
        static const int kd[8][2] = {
            {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
        };
        for (int i = 0; i < 8; i++) {
            int nr2 = r + kd[i][0], nf2 = f + kd[i][1];
            if (in_bounds(nr2, nf2) &&
                (b[nr2][nf2] == EMPTY || COLOUR(b[nr2][nf2]) == (u8)opp))
                add_move(ml, make_move(r, f, nr2, nf2, 0, 0, 0));
        }
        /* Castling */
        if (ck) {
            /* Kingside: squares between king and rook must be empty */
            /* White: king at 7,4; rook at 7,7 -> squares 7,5 and 7,6 */
            /* Black: king at 0,4; rook at 0,7 -> squares 0,5 and 0,6 */
            int cr = (colour == WHITE) ? 7 : 0;
            if (b[cr][5] == EMPTY && b[cr][6] == EMPTY &&
                !is_attacked(b, cr, 4, opp) &&
                !is_attacked(b, cr, 5, opp) &&
                !is_attacked(b, cr, 6, opp))
                add_move(ml, make_move(r, f, cr, 6, 0, 1, 0));
        }
        if (cq) {
            int cr = (colour == WHITE) ? 7 : 0;
            if (b[cr][3] == EMPTY && b[cr][2] == EMPTY && b[cr][1] == EMPTY &&
                !is_attacked(b, cr, 4, opp) &&
                !is_attacked(b, cr, 3, opp) &&
                !is_attacked(b, cr, 2, opp))
                add_move(ml, make_move(r, f, cr, 2, 0, 1, 0));
        }
        break;
    }
    } /* switch */
}

/* Apply a move to a board copy, return captured piece type (0 if none) */
static int apply_move_copy(u8 src[8][8], u8 dst[8][8], int mv)
{
    /* Copy board */
    for (int r2 = 0; r2 < 8; r2++)
        for (int f2 = 0; f2 < 8; f2++)
            dst[r2][f2] = src[r2][f2];

    int fr = MOVE_FROM_RANK(mv), ff = MOVE_FROM_FILE(mv);
    int tr = MOVE_TO_RANK(mv),   tf = MOVE_TO_FILE(mv);
    int promo  = MOVE_PROMO(mv);
    int castle = MOVE_IS_CASTLE(mv);
    int ep     = MOVE_IS_EP(mv);

    u8 pc = dst[fr][ff];
    int colour = COLOUR(pc);
    int captured = PIECE(dst[tr][tf]);

    dst[fr][ff] = EMPTY;

    if (promo)
        dst[tr][tf] = (u8)(colour | promo);
    else
        dst[tr][tf] = pc;

    /* En passant capture: remove the captured pawn */
    if (ep) {
        int dir = (colour == WHITE) ? 1 : -1;
        dst[tr + dir][tf] = EMPTY;
        captured = PAWN;
    }

    /* Castling: move the rook */
    if (castle) {
        int cr = (colour == WHITE) ? 7 : 0;
        if (tf == 6) {
            /* Kingside */
            dst[cr][5] = dst[cr][7];
            dst[cr][7] = EMPTY;
        } else {
            /* Queenside */
            dst[cr][3] = dst[cr][0];
            dst[cr][0] = EMPTY;
        }
    }

    return captured;
}

/* Check if a move leaves own king in check (used for legal filtering) */
static int move_leaves_in_check(u8 b[8][8], int mv, int colour)
{
    u8 tmp[8][8];
    apply_move_copy(b, tmp, mv);
    int kr, kf;
    find_king(tmp, colour, &kr, &kf);
    if (kr < 0) return 1;  /* should not happen */
    int opp = (colour == WHITE) ? BLACK : WHITE;
    return is_attacked(tmp, kr, kf, opp);
}

/* Generate all fully legal moves for the given colour */
static void gen_legal_moves(u8 b[8][8], int colour, int ep_sq,
                             int ck, int cq, movelist_t *out)
{
    movelist_t pseudo = {.count = 0};
    for (int r2 = 0; r2 < 8; r2++)
        for (int f2 = 0; f2 < 8; f2++) {
            u8 sq = b[r2][f2];
            if (sq == EMPTY || COLOUR(sq) != (u8)colour) continue;
            gen_piece_moves(b, r2, f2, colour, ep_sq, &pseudo, ck, cq);
        }
    out->count = 0;
    for (int i = 0; i < pseudo.count; i++) {
        int mv = pseudo.mv[i];
        if (!move_leaves_in_check(b, mv, colour))
            add_move(out, mv);
    }
}

/* =========================================================================
 * Apply move to the real game state.
 * ========================================================================= */
static void state_apply_move(chess_t *g, int mv)
{
    int fr = MOVE_FROM_RANK(mv), ff = MOVE_FROM_FILE(mv);
    int tr = MOVE_TO_RANK(mv),   tf = MOVE_TO_FILE(mv);
    int promo  = MOVE_PROMO(mv);
    int castle = MOVE_IS_CASTLE(mv);
    int ep     = MOVE_IS_EP(mv);

    u8 pc = g->board[fr][ff];
    int colour = COLOUR(pc);
    int type   = PIECE(pc);

    int captured_type = PIECE(g->board[tr][tf]);

    /* Remove moving piece */
    g->board[fr][ff] = EMPTY;

    /* Place piece (with promotion) */
    if (promo)
        g->board[tr][tf] = (u8)(colour | promo);
    else
        g->board[tr][tf] = pc;

    /* En passant capture */
    if (ep) {
        int dir = (colour == WHITE) ? 1 : -1;
        g->board[tr + dir][tf] = EMPTY;
        captured_type = PAWN;
    }

    /* Castling: move rook */
    if (castle) {
        int cr = (colour == WHITE) ? 7 : 0;
        if (tf == 6) {
            g->board[cr][5] = g->board[cr][7];
            g->board[cr][7] = EMPTY;
        } else {
            g->board[cr][3] = g->board[cr][0];
            g->board[cr][0] = EMPTY;
        }
    }

    /* Update captured arrays */
    if (captured_type) {
        if (colour == WHITE)
            g->white_captured[captured_type]++;
        else
            g->black_captured[captured_type]++;
    }

    /* Update castling rights */
    if (type == KING) {
        if (colour == WHITE) { g->w_castle_k = 0; g->w_castle_q = 0; }
        else                 { g->b_castle_k = 0; g->b_castle_q = 0; }
    }
    if (type == ROOK) {
        if (colour == WHITE) {
            if (fr == 7 && ff == 0) g->w_castle_q = 0;
            if (fr == 7 && ff == 7) g->w_castle_k = 0;
        } else {
            if (fr == 0 && ff == 0) g->b_castle_q = 0;
            if (fr == 0 && ff == 7) g->b_castle_k = 0;
        }
    }

    /* Update en passant square */
    if (type == PAWN && g_abs(tr - fr) == 2)
        g->ep_sq = ((fr + tr) / 2) * 8 + ff;
    else
        g->ep_sq = -1;

    /* Switch turn */
    g->turn = (colour == WHITE) ? BLACK : WHITE;
    g->ply++;
}

/* =========================================================================
 * Check / checkmate / stalemate detection.
 * ========================================================================= */
static void update_game_status(chess_t *g)
{
    int colour = g->turn;
    int opp    = (colour == WHITE) ? BLACK : WHITE;

    /* Find king */
    int kr, kf;
    find_king(g->board, colour, &kr, &kf);

    g->in_check = (kr >= 0) ? is_attacked(g->board, kr, kf, opp) : 0;

    /* Generate legal moves */
    int ck = (colour == WHITE) ? g->w_castle_k : g->b_castle_k;
    int cq = (colour == WHITE) ? g->w_castle_q : g->b_castle_q;
    movelist_t ml = {.count = 0};
    gen_legal_moves(g->board, colour, g->ep_sq, ck, cq, &ml);

    if (ml.count == 0) {
        if (g->in_check)
            g->checkmate = 1;
        else
            g->stalemate = 1;
    } else {
        g->checkmate = 0;
        g->stalemate = 0;
    }
}

/* =========================================================================
 * AI: minimax with alpha-beta pruning.
 * ========================================================================= */

/* Piece values (centipawns) */
static const int PIECE_VAL[7] = {0, 100, 320, 330, 500, 900, 20000};

/* Piece-square tables (for white; flip rank for black).
 * Values in centipawns, from top of board (rank 0) to bottom (rank 7).
 * Adjusted for our board orientation where rank 7 = white's back rank.
 */
static const i8 PST_PAWN[8][8] = {
    {  0,  0,  0,  0,  0,  0,  0,  0},
    { 50, 50, 50, 50, 50, 50, 50, 50},
    { 10, 10, 20, 30, 30, 20, 10, 10},
    {  5,  5, 10, 25, 25, 10,  5,  5},
    {  0,  0,  0, 20, 20,  0,  0,  0},
    {  5, -5,-10,  0,  0,-10, -5,  5},
    {  5, 10, 10,-20,-20, 10, 10,  5},
    {  0,  0,  0,  0,  0,  0,  0,  0}
};

static const i8 PST_KNIGHT[8][8] = {
    {-50,-40,-30,-30,-30,-30,-40,-50},
    {-40,-20,  0,  0,  0,  0,-20,-40},
    {-30,  0, 10, 15, 15, 10,  0,-30},
    {-30,  5, 15, 20, 20, 15,  5,-30},
    {-30,  0, 15, 20, 20, 15,  0,-30},
    {-30,  5, 10, 15, 15, 10,  5,-30},
    {-40,-20,  0,  5,  5,  0,-20,-40},
    {-50,-40,-30,-30,-30,-30,-40,-50}
};

static const i8 PST_BISHOP[8][8] = {
    {-20,-10,-10,-10,-10,-10,-10,-20},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-10,  0,  5, 10, 10,  5,  0,-10},
    {-10,  5,  5, 10, 10,  5,  5,-10},
    {-10,  0, 10, 10, 10, 10,  0,-10},
    {-10, 10, 10, 10, 10, 10, 10,-10},
    {-10,  5,  0,  0,  0,  0,  5,-10},
    {-20,-10,-10,-10,-10,-10,-10,-20}
};

static const i8 PST_ROOK[8][8] = {
    {  0,  0,  0,  0,  0,  0,  0,  0},
    {  5, 10, 10, 10, 10, 10, 10,  5},
    { -5,  0,  0,  0,  0,  0,  0, -5},
    { -5,  0,  0,  0,  0,  0,  0, -5},
    { -5,  0,  0,  0,  0,  0,  0, -5},
    { -5,  0,  0,  0,  0,  0,  0, -5},
    { -5,  0,  0,  0,  0,  0,  0, -5},
    {  0,  0,  0,  5,  5,  0,  0,  0}
};

static const i8 PST_QUEEN[8][8] = {
    {-20,-10,-10, -5, -5,-10,-10,-20},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-10,  0,  5,  5,  5,  5,  0,-10},
    { -5,  0,  5,  5,  5,  5,  0, -5},
    {  0,  0,  5,  5,  5,  5,  0, -5},
    {-10,  5,  5,  5,  5,  5,  0,-10},
    {-10,  0,  5,  0,  0,  0,  0,-10},
    {-20,-10,-10, -5, -5,-10,-10,-20}
};

static const i8 PST_KING_MG[8][8] = {
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-20,-30,-30,-40,-40,-30,-30,-20},
    {-10,-20,-20,-20,-20,-20,-20,-10},
    { 20, 20,  0,  0,  0,  0, 20, 20},
    { 20, 30, 10,  0,  0, 10, 30, 20}
};

static int pst_value(int type, int r, int f, int colour)
{
    /* For white: board rank 7 is their back rank; PST row 7 = king side
     * For black: flip the rank (row 0 becomes row 7 for PST lookup) */
    int pr = (colour == WHITE) ? r : (7 - r);
    switch (type) {
    case PAWN:   return PST_PAWN[pr][f];
    case KNIGHT: return PST_KNIGHT[pr][f];
    case BISHOP: return PST_BISHOP[pr][f];
    case ROOK:   return PST_ROOK[pr][f];
    case QUEEN:  return PST_QUEEN[pr][f];
    case KING:   return PST_KING_MG[pr][f];
    }
    return 0;
}

/* Static evaluation: positive = good for white */
static int evaluate(u8 b[8][8])
{
    int score = 0;
    for (int r2 = 0; r2 < 8; r2++) {
        for (int f2 = 0; f2 < 8; f2++) {
            u8 sq = b[r2][f2];
            if (sq == EMPTY) continue;
            int type   = PIECE(sq);
            int colour = COLOUR(sq);
            int val    = PIECE_VAL[type] + pst_value(type, r2, f2, colour);
            if (colour == WHITE) score += val;
            else                 score -= val;
        }
    }
    return score;
}

/* Node counter to cap search budget */
static int ai_nodes;
#define AI_NODE_LIMIT  80000
#define AI_DEPTH       3       /* root search depth (plies) */

/* Minimax with alpha-beta (negamax variant) */
/* Returns score relative to the player to move (positive = good). */
static int minimax(u8 b[8][8], int depth, int alpha, int beta,
                   int colour, int ep_sq, int ck, int cq)
{
    if (ai_nodes >= AI_NODE_LIMIT || depth == 0)
        return (colour == WHITE) ? evaluate(b) : -evaluate(b);

    ai_nodes++;

    movelist_t ml = {.count = 0};
    gen_legal_moves(b, colour, ep_sq, ck, cq, &ml);

    if (ml.count == 0) {
        /* Checkmate or stalemate */
        int opp = (colour == WHITE) ? BLACK : WHITE;
        int kr, kf;
        find_king(b, colour, &kr, &kf);
        if (kr >= 0 && is_attacked(b, kr, kf, opp))
            return -20000 + depth;  /* checkmate (prefer faster mates) */
        return 0;                   /* stalemate */
    }

    int best = -30000;
    for (int i = 0; i < ml.count; i++) {
        int mv = ml.mv[i];
        int fr2 = MOVE_FROM_RANK(mv), ff2 = MOVE_FROM_FILE(mv);
        int tr2 = MOVE_TO_RANK(mv);
        u8 pc2 = b[fr2][ff2];
        int mv_type = PIECE(pc2);

        /* Compute next state parameters */
        int next_ck = ck, next_cq = cq;
        int next_ep = -1;

        if (mv_type == KING) { next_ck = 0; next_cq = 0; }
        if (mv_type == ROOK) {
            if (colour == WHITE) {
                if (fr2 == 7 && ff2 == 0) next_cq = 0;
                if (fr2 == 7 && ff2 == 7) next_ck = 0;
            } else {
                if (fr2 == 0 && ff2 == 0) next_cq = 0;
                if (fr2 == 0 && ff2 == 7) next_ck = 0;
            }
        }
        if (mv_type == PAWN && g_abs(tr2 - fr2) == 2)
            next_ep = ((fr2 + tr2) / 2) * 8 + ff2;

        u8 tmp[8][8];
        apply_move_copy(b, tmp, mv);

        /* Get opponent castling rights (we only track current side's rights here;
         * for full correctness pass both sides -- but this simplified version
         * passes the same ck/cq for the opponent, which is acceptable for depth 3) */
        int opp_colour = (colour == WHITE) ? BLACK : WHITE;
        int opp_ck = (opp_colour == WHITE) ? 1 : next_ck;
        int opp_cq = (opp_colour == WHITE) ? 1 : next_cq;

        int score = -minimax(tmp, depth - 1, -beta, -alpha,
                             opp_colour, next_ep, opp_ck, opp_cq);

        if (score > best) best = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    return best;
}

/* Find the best move for the given colour */
static int ai_best_move(chess_t *g)
{
    int colour = g->turn;
    int ck = (colour == WHITE) ? g->w_castle_k : g->b_castle_k;
    int cq = (colour == WHITE) ? g->w_castle_q : g->b_castle_q;

    movelist_t ml = {.count = 0};
    gen_legal_moves(g->board, colour, g->ep_sq, ck, cq, &ml);
    if (ml.count == 0) return -1;

    ai_nodes = 0;
    int best_score = -30000;
    int best_mv    = ml.mv[0];

    /* Shuffle move order slightly via a simple deterministic perturbation
     * (avoids repetitive play without random dependency) */
    for (int i = 0; i < ml.count; i++) {
        /* Swap with a "random" position based on move bits */
        int j = (i + (ml.mv[i] & 0x1F)) % ml.count;
        int tmp2 = ml.mv[i]; ml.mv[i] = ml.mv[j]; ml.mv[j] = tmp2;
    }

    int best_alpha = -30000;
    for (int i = 0; i < ml.count; i++) {
        int mv = ml.mv[i];
        int fr2 = MOVE_FROM_RANK(mv), ff2 = MOVE_FROM_FILE(mv);
        int tr2 = MOVE_TO_RANK(mv);
        u8 pc2 = g->board[fr2][ff2];
        int mv_type = PIECE(pc2);

        int next_ep = -1;
        if (mv_type == PAWN && g_abs(tr2 - fr2) == 2)
            next_ep = ((fr2 + tr2) / 2) * 8 + ff2;

        u8 tmp_b[8][8];
        apply_move_copy(g->board, tmp_b, mv);

        /* The recursive search switches to the opponent, so it tracks the
         * opponent's (unchanged) castling rights. The mover's updated rights
         * only matter two plies deeper, which this depth does not exploit. */
        int opp_colour = (colour == WHITE) ? BLACK : WHITE;
        int opp_ck = (opp_colour == WHITE) ? g->w_castle_k : g->b_castle_k;
        int opp_cq = (opp_colour == WHITE) ? g->w_castle_q : g->b_castle_q;

        int score = -minimax(tmp_b, AI_DEPTH - 1, -30000, -best_alpha,
                             opp_colour, next_ep, opp_ck, opp_cq);

        if (score > best_score) {
            best_score = score;
            best_mv    = mv;
            if (score > best_alpha) best_alpha = score;
        }

        if (ai_nodes >= AI_NODE_LIMIT) break;
    }

    return best_mv;
}

/* =========================================================================
 * Colour palette.
 * ========================================================================= */
#define COL_BG         0xFF1A1A2E   /* deep navy background */
#define COL_BOARD_DARK 0xFF769656   /* classic dark square green */
#define COL_BOARD_LITE 0xFFEEEED2   /* classic light square cream */
#define COL_BORDER     0xFF4A3728   /* dark wood border */
#define COL_SELECT     0xFFFFFF00   /* yellow selection highlight */
#define COL_LEGAL      0x8800CC00   /* semi-transparent green dot */
#define COL_LEGAL_CAP  0x88FF4400   /* orange dot for legal capture */
#define COL_CHECK      0xCCFF2222   /* red check highlight */

#define COL_W_PIECE    0xFFF0D9B5   /* white piece fill */
#define COL_W_OUTLINE  0xFF8B6914   /* white piece outline */
#define COL_B_PIECE    0xFF404040   /* black piece fill */
#define COL_B_OUTLINE  0xFF000000   /* black piece outline */

#define COL_STATUS_BG  0xFF0D0D1A
#define COL_STATUS_FG  0xFFDDDDFF
#define COL_WHITE_CAP  0xFFE0E0E0
#define COL_BLACK_CAP  0xFF666666

/* =========================================================================
 * Drawing helpers.
 * ========================================================================= */

/* Convert board rank/file to pixel coordinates (top-left of square). */
static inline int sq_px(int f) { return BOARD_X + f * SQ_SZ; }
static inline int sq_py(int r) { return BOARD_Y + r * SQ_SZ; }
static inline int sq_cx(int f) { return sq_px(f) + SQ_SZ / 2; }
static inline int sq_cy(int r) { return sq_py(r) + SQ_SZ / 2; }

/* Draw a chess piece using primitives + a letter glyph */
static void draw_piece(game_t *gctx, int cx, int cy, int type, int colour)
{
    u32 fill    = (colour == WHITE) ? COL_W_PIECE   : COL_B_PIECE;
    u32 outline = (colour == WHITE) ? COL_W_OUTLINE : COL_B_OUTLINE;
    u32 letter_col = (colour == WHITE) ? 0xFF2A1A00 : 0xFFDDDDDD;

    int r_outer = SQ_SZ / 2 - 4;   /* outer circle radius */

    switch (type) {
    case PAWN:
        /* Small circle body */
        g_circle(gctx, cx, cy + 4, SQ_SZ/2 - 8, fill);
        g_circle_outline(gctx, cx, cy + 4, SQ_SZ/2 - 8, outline);
        /* Head */
        g_circle(gctx, cx, cy - 6, SQ_SZ/4 - 2, fill);
        g_circle_outline(gctx, cx, cy - 6, SQ_SZ/4 - 2, outline);
        /* Base bar */
        g_fill_rect(gctx, cx - r_outer + 4, cy + 12, (r_outer - 4) * 2, 5, fill);
        g_rect(gctx,      cx - r_outer + 4, cy + 12, (r_outer - 4) * 2, 5, outline);
        break;

    case KNIGHT:
        /* Body */
        g_circle(gctx, cx, cy + 2, r_outer - 2, fill);
        g_circle_outline(gctx, cx, cy + 2, r_outer - 2, outline);
        /* "N" glyph for distinction */
        g_text_center(gctx, cx, cy + 2, "N", letter_col);
        break;

    case BISHOP:
        /* Tall oval-ish shape: stacked circles */
        g_circle(gctx, cx, cy + 8,  r_outer - 4, fill);
        g_circle_outline(gctx, cx, cy + 8, r_outer - 4, outline);
        g_circle(gctx, cx, cy - 2,  r_outer - 8, fill);
        g_circle_outline(gctx, cx, cy - 2, r_outer - 8, outline);
        /* Top dot */
        g_circle(gctx, cx, cy - 13, 3, outline);
        g_text_center(gctx, cx, cy + 4, "B", letter_col);
        break;

    case ROOK:
        /* Rectangle body */
        g_fill_rect(gctx, cx - r_outer + 4, cy - r_outer + 4,
                    (r_outer - 4) * 2, (r_outer - 4) * 2 + 4, fill);
        g_rect(gctx,      cx - r_outer + 4, cy - r_outer + 4,
               (r_outer - 4) * 2, (r_outer - 4) * 2 + 4, outline);
        /* Battlements */
        for (int bi = -1; bi <= 1; bi++) {
            g_fill_rect(gctx, cx + bi * 8 - 4, cy - r_outer + 1, 8, 6, fill);
            g_rect(gctx,      cx + bi * 8 - 4, cy - r_outer + 1, 8, 6, outline);
        }
        g_text_center(gctx, cx, cy + 2, "R", letter_col);
        break;

    case QUEEN:
        /* Large circle */
        g_circle(gctx, cx, cy, r_outer - 2, fill);
        g_circle_outline(gctx, cx, cy, r_outer - 2, outline);
        /* Crown points */
        for (int qi = -2; qi <= 2; qi++) {
            int qx = cx + qi * 7;
            int qy = cy - r_outer + 2 - (((qi & 1) == 0) ? 4 : 0);
            g_circle(gctx, qx, qy, 3, outline);
        }
        g_text_center(gctx, cx, cy, "Q", letter_col);
        break;

    case KING:
        /* Large body */
        g_circle(gctx, cx, cy + 2, r_outer - 2, fill);
        g_circle_outline(gctx, cx, cy + 2, r_outer - 2, outline);
        /* Cross on top */
        g_fill_rect(gctx, cx - 2, cy - r_outer - 2, 4, 10, outline);
        g_fill_rect(gctx, cx - 6, cy - r_outer + 1, 12, 4, outline);
        g_text_center(gctx, cx, cy + 2, "K", letter_col);
        break;
    }
}

/* =========================================================================
 * String helpers (no libc).
 * ========================================================================= */
static int chess_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void chess_strcat(char *dst, const char *src)
{
    int n = chess_strlen(dst);
    int i = 0;
    while (src[i]) dst[n++] = src[i++];
    dst[n] = 0;
}

static void chess_strcpy(char *dst, const char *src)
{
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* =========================================================================
 * Build captured pieces string.
 * ========================================================================= */
static const char *piece_glyph(int type)
{
    switch(type) {
    case PAWN:   return "P";
    case KNIGHT: return "N";
    case BISHOP: return "B";
    case ROOK:   return "R";
    case QUEEN:  return "Q";
    case KING:   return "K";
    }
    return "?";
}

static void build_captured_str(int captured[7], char *out, int max_len)
{
    out[0] = 0;
    int total = 0;
    for (int t = 1; t <= 6; t++) {
        for (int c = 0; c < captured[t] && total < max_len - 2; c++, total++) {
            chess_strcat(out, piece_glyph(t));
        }
    }
}

/* =========================================================================
 * UI: compute legal moves for selected piece.
 * ========================================================================= */
static void compute_legal_for_sel(chess_t *g)
{
    g->legal_count = 0;
    if (g->sel_rank < 0) return;

    int colour = g->turn;
    int ck = (colour == WHITE) ? g->w_castle_k : g->b_castle_k;
    int cq = (colour == WHITE) ? g->w_castle_q : g->b_castle_q;

    movelist_t ml = {.count = 0};
    gen_piece_moves(g->board, g->sel_rank, g->sel_file, colour,
                    g->ep_sq, &ml, ck, cq);

    /* Filter: only legal moves from this square */
    for (int i = 0; i < ml.count; i++) {
        int mv = ml.mv[i];
        if (!move_leaves_in_check(g->board, mv, colour)) {
            int tr2 = MOVE_TO_RANK(mv), tf2 = MOVE_TO_FILE(mv);
            /* Store as rank<<3|file */
            g->legal[g->legal_count++] = (u8)((tr2 << 3) | tf2);
        }
    }
}

static int is_legal_dest(chess_t *g, int r, int f)
{
    u8 enc = (u8)((r << 3) | f);
    for (int i = 0; i < g->legal_count; i++)
        if (g->legal[i] == enc) return 1;
    return 0;
}

/* Find the move from sel to (tr, tf) in the full move list */
static int find_move_to(chess_t *g, int tr, int tf)
{
    int colour = g->turn;
    int ck = (colour == WHITE) ? g->w_castle_k : g->b_castle_k;
    int cq = (colour == WHITE) ? g->w_castle_q : g->b_castle_q;
    movelist_t ml = {.count = 0};
    gen_piece_moves(g->board, g->sel_rank, g->sel_file, colour,
                    g->ep_sq, &ml, ck, cq);
    for (int i = 0; i < ml.count; i++) {
        int mv = ml.mv[i];
        if (MOVE_TO_RANK(mv) == tr && MOVE_TO_FILE(mv) == tf &&
            !move_leaves_in_check(g->board, mv, colour))
            return mv;
    }
    return -1;
}

/* =========================================================================
 * Render.
 * ========================================================================= */
static void render(game_t *gctx, chess_t *g)
{
    /* Background */
    g_clear(gctx, COL_BG);

    /* Board border (rounded rectangle) */
    g_rounded_rect(gctx, BOARD_X - 6, BOARD_Y - 6,
                   BOARD_W + 12, BOARD_H + 12, 6, COL_BORDER);

    /* Rank/file labels */
    static const char *files_str = "abcdefgh";
    static const char *ranks_str[8] = {"8","7","6","5","4","3","2","1"};
    for (int f = 0; f < 8; f++) {
        char fs[2] = {files_str[f], 0};
        g_text(gctx, sq_px(f) + SQ_SZ/2 - 3, BOARD_Y - 20, fs, 0xFFAAAAAA);
    }
    for (int r = 0; r < 8; r++) {
        g_text(gctx, BOARD_X - 18, sq_py(r) + SQ_SZ/2 - 8, ranks_str[r], 0xFFAAAAAA);
    }

    /* Squares */
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int px = sq_px(f), py = sq_py(r);
            u32 sq_col = ((r + f) & 1) ? COL_BOARD_DARK : COL_BOARD_LITE;

            /* Check highlight */
            if (g->in_check && g->board[r][f] == (u8)(g->turn | KING))
                sq_col = COL_CHECK;

            /* Selection highlight */
            if (r == g->sel_rank && f == g->sel_file)
                sq_col = COL_SELECT;

            g_fill_rect(gctx, px, py, SQ_SZ, SQ_SZ, sq_col);

            /* Legal move indicators */
            if (is_legal_dest(g, r, f)) {
                u8 target = g->board[r][f];
                u32 dot_col = (target != EMPTY) ? COL_LEGAL_CAP : COL_LEGAL;
                g_circle(gctx, sq_cx(f), sq_cy(r), SQ_SZ/6, dot_col);
            }
        }
    }

    /* Pieces */
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            u8 sq = g->board[r][f];
            if (sq == EMPTY) continue;
            draw_piece(gctx, sq_cx(f), sq_cy(r), PIECE(sq), COLOUR(sq));
        }
    }

    /* ---- Status bar ---- */
    g_fill_rect(gctx, 0, STATUS_Y - 4, WIN_W, WIN_H - STATUS_Y + 4, COL_STATUS_BG);

    /* Turn / game state */
    char status[128];
    status[0] = 0;

    if (g->checkmate) {
        if (g->turn == WHITE)
            chess_strcpy(status, "CHECKMATE - Black wins!");
        else
            chess_strcpy(status, "CHECKMATE - White wins!");
    } else if (g->stalemate) {
        chess_strcpy(status, "STALEMATE - Draw");
    } else if (g->in_check) {
        if (g->turn == WHITE)
            chess_strcpy(status, "White to move - CHECK!");
        else
            chess_strcpy(status, "Black to move - CHECK!");
    } else if (g->ai_thinking) {
        chess_strcpy(status, "AI thinking...");
    } else {
        if (g->turn == WHITE)
            chess_strcpy(status, "White to move");
        else
            chess_strcpy(status, "Black to move");
    }

    g_text(gctx, 8, STATUS_Y + 2, status, COL_STATUS_FG);

    /* Captured pieces */
    char cap_w[64] = "W captured: ";
    char cap_b[64] = "B captured: ";
    char tmp_buf[32]; tmp_buf[0] = 0;
    build_captured_str(g->white_captured, tmp_buf, 28);
    chess_strcat(cap_w, tmp_buf);
    tmp_buf[0] = 0;
    build_captured_str(g->black_captured, tmp_buf, 28);
    chess_strcat(cap_b, tmp_buf);

    g_text(gctx, 8,      STATUS_Y + 20, cap_w, COL_WHITE_CAP);
    g_text(gctx, 8,      STATUS_Y + 38, cap_b, COL_BLACK_CAP);

    /* N=New game hint */
    g_text(gctx, WIN_W - 110, STATUS_Y + 2, "N=New game", 0xFF888888);

    /* Game-over overlay */
    if ((g->checkmate || g->stalemate) && g->game_over_shown) {
        g_rounded_rect(gctx, WIN_W/2 - 110, WIN_H/2 - 40,
                       220, 80, 10, 0xCC000000);
        if (g->checkmate) {
            if (g->turn == WHITE)
                g_text_center(gctx, WIN_W/2, WIN_H/2 - 18,
                              "CHECKMATE", 0xFFFF4444);
            else
                g_text_center(gctx, WIN_W/2, WIN_H/2 - 18,
                              "CHECKMATE", 0xFF44FF44);
            if (g->turn == WHITE)
                g_text_center(gctx, WIN_W/2, WIN_H/2 + 2,
                              "Black wins!", 0xFFFFFFFF);
            else
                g_text_center(gctx, WIN_W/2, WIN_H/2 + 2,
                              "White wins!", 0xFFFFFFFF);
        } else {
            g_text_center(gctx, WIN_W/2, WIN_H/2 - 10,
                          "STALEMATE - Draw", 0xFFFFFF44);
        }
        g_text_center(gctx, WIN_W/2, WIN_H/2 + 20,
                      "Press N for new game", 0xFFAAAAAA);
    }
}

/* =========================================================================
 * Handle a click on square (r, f) during player's turn.
 * ========================================================================= */
static void handle_click(chess_t *g, int r, int f)
{
    if (g->turn != WHITE) return;
    if (g->checkmate || g->stalemate) return;

    u8 sq = g->board[r][f];

    /* If we have a selection and clicked a legal destination */
    if (g->sel_rank >= 0 && is_legal_dest(g, r, f)) {
        int mv = find_move_to(g, r, f);
        if (mv >= 0) {
            int captured = PIECE(g->board[r][f]);
            if (MOVE_IS_EP(mv)) captured = PAWN;

            state_apply_move(g, mv);

            /* SFX */
            if (captured)
                g_beep(880, 80);   /* capture: higher pitch */
            else
                g_beep(440, 60);   /* normal move */

            g->sel_rank   = -1;
            g->sel_file   = -1;
            g->legal_count = 0;

            update_game_status(g);

            if (g->in_check)
                g_beep(660, 120);

            if (g->checkmate || g->stalemate) {
                g->game_over_shown = 1;
                g_beep(220, 400);
                return;
            }

            /* Trigger AI */
            g->ai_thinking  = 1;
            g->ai_start_tick = game_ticks();
        }
        return;
    }

    /* Select a white piece */
    if (sq != EMPTY && IS_WHITE(sq)) {
        g->sel_rank = r;
        g->sel_file = f;
        compute_legal_for_sel(g);
    } else {
        /* Deselect */
        g->sel_rank   = -1;
        g->sel_file   = -1;
        g->legal_count = 0;
    }
}

/* =========================================================================
 * Entry point.
 * ========================================================================= */
void _start(void)
{
    chess_print("[CHESS] starting\n");

    game_t *gctx = game_open(WIN_W, WIN_H, "Chess");
    if (!gctx) {
        chess_print("[CHESS] game_open failed\n");
        chess_exit(1);
    }

    board_reset(&gs);
    update_game_status(&gs);

    int prev_btn = 0;

    while (game_frame_begin(gctx)) {
        /* ---- Input ---- */
        if (game_key_pressed(gctx, KEY_ESC)) break;

        if (game_key_pressed(gctx, KEY_N)) {
            chess_print("[CHESS] new game\n");
            board_reset(&gs);
            update_game_status(&gs);
            prev_btn = 0;
        }

        /* Mouse click handling (rising edge on MOUSE_LEFT) */
        int mx, my, mbtn;
        game_mouse(gctx, &mx, &my, &mbtn);

        int clicked = (mbtn & MOUSE_LEFT) && !(prev_btn & MOUSE_LEFT);
        prev_btn = mbtn;

        if (clicked && !gs.ai_thinking &&
            !gs.checkmate && !gs.stalemate) {
            /* Convert pixel -> board square */
            int bx = mx - BOARD_X;
            int by = my - BOARD_Y;
            if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
                int f = bx / SQ_SZ;
                int r = by / SQ_SZ;
                if ((unsigned)r < 8 && (unsigned)f < 8)
                    handle_click(&gs, r, f);
            }
        }

        /* ---- AI move ---- */
        if (gs.ai_thinking && gs.turn == BLACK &&
            !gs.checkmate && !gs.stalemate) {
            /* Run AI (synchronously -- acceptable for depth 3) */
            int mv = ai_best_move(&gs);
            gs.ai_thinking = 0;

            if (mv >= 0) {
                int captured = PIECE(gs.board[MOVE_TO_RANK(mv)][MOVE_TO_FILE(mv)]);
                if (MOVE_IS_EP(mv)) captured = PAWN;

                state_apply_move(&gs, mv);

                if (captured)
                    g_beep(660, 80);
                else
                    g_beep(330, 60);

                update_game_status(&gs);

                if (gs.in_check)
                    g_beep(880, 120);

                if (gs.checkmate || gs.stalemate) {
                    gs.game_over_shown = 1;
                    g_beep(220, 400);
                }
            }
        }

        /* ---- Render ---- */
        render(gctx, &gs);
        game_present(gctx);
        game_sync(gctx);
    }

    chess_print("[CHESS] exit\n");
    chess_exit(0);
}
