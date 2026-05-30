/*
 * connect4.c -- Connect Four game (freestanding, ring 3).
 * ========================================================
 *
 * Classic 7-column x 6-row Connect Four.
 * Modes: vs AI (minimax + alpha-beta, depth 6) or 2-player hot-seat.
 * Gravity drop animation.  Win detection (horiz/vert/diag).  SFX.
 *
 * Window: 560 x 600
 *   Top HUD:    560 x 40   (mode indicator, turn/status)
 *   Board area: 560 x 480  (7 cols x 6 rows, each cell 76x76, 8px margin)
 *   Bottom bar: 560 x 80   (instructions)
 *
 * Controls:
 *   Mouse move     -- hover column (drop preview)
 *   Left click     -- drop disc in hovered column
 *   N              -- new game
 *   M or TAB       -- toggle mode (vs AI / 2-player)
 *   ESC            -- quit
 *
 * Build:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/connect4/connect4.c -o /tmp/c4.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/game/game.c -o /tmp/game.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/c4.o /tmp/game.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/connect4.elf
 *   objdump -d /tmp/connect4.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [C4] starting
 *   [C4] player 1 wins / player 2 wins / AI wins / draw
 */

#include "../../lib/game/game.h"

/* =========================================================================
 * Serial output (SYS_WRITE, no libc)
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
static unsigned long _slen(const char *s) { unsigned long n=0; while(s[n])n++; return n; }
static void _print(const char *m) { _sc3(SYS_WRITE, 1, (long)m, (long)_slen(m)); }

/* =========================================================================
 * Layout constants
 * ========================================================================= */
#define WIN_W        560
#define WIN_H        600

#define HUD_H         40
#define BOT_H         80

#define BOARD_X        8          /* left margin for board                 */
#define BOARD_Y       HUD_H       /* board starts below HUD                */
#define BOARD_W      (WIN_W - 16) /* 544 px for 7 columns                  */
#define BOARD_H      (WIN_H - HUD_H - BOT_H)  /* 480 px for 6 rows        */

#define COLS          7
#define ROWS          6

/* Cell size derived from available area */
#define CELL_W       (BOARD_W / COLS)   /* 77 px */
#define CELL_H       (BOARD_H / ROWS)   /* 80 px */

/* Disc radius: a bit smaller than cell to leave a visible gap */
#define DISC_R       30

/* =========================================================================
 * Colors (ARGB32)
 * ========================================================================= */
#define COL_BG          0xFF1A1A2E
#define COL_HUD_BG      0xFF16213E
#define COL_BOT_BG      0xFF0F0F1E
#define COL_BOARD       0xFF1565C0   /* bright blue board                  */
#define COL_BOARD_DARK  0xFF0D47A1   /* darker blue for cell borders       */
#define COL_EMPTY       0xFF0D1B3E   /* dark hole (empty disc slot)        */
#define COL_P1          0xFFE53935   /* player 1: red                      */
#define COL_P2          0xFFFFD600   /* player 2 / AI: yellow              */
#define COL_P1_DARK     0xFF7B1FA2   /* darker red for disc shading        */
#define COL_P2_DARK     0xFFFF8F00   /* darker yellow for disc shading     */
#define COL_WIN_RING    0xFF00E676   /* bright green highlight on win line */
#define COL_PREVIEW     0x60FF4444   /* semi-transparent drop preview      */
#define COL_WHITE       0xFFFFFFFF
#define COL_GRAY        0xFF9E9E9E
#define COL_YELLOW_TXT  0xFFFFD600
#define COL_RED_TXT     0xFFEF5350
#define COL_GREEN_TXT   0xFF00E676
#define COL_CYAN        0xFF00BCD4

/* =========================================================================
 * Game state
 * ========================================================================= */
#define PIECE_NONE  0
#define PIECE_P1    1   /* red   */
#define PIECE_P2    2   /* yellow */

typedef enum {
    STATE_MENU,        /* initial mode selection screen */
    STATE_PLAYING,
    STATE_ANIM,        /* disc falling animation        */
    STATE_WIN,
    STATE_DRAW
} game_state_t;

typedef enum {
    MODE_VS_AI,
    MODE_2PLAYER
} game_mode_t;

/* Board: board[row][col], row 0 = bottom, row 5 = top */
static i8 board[ROWS][COLS];

/* Win line: 4 cells (row, col pairs) */
static int win_cells[4][2];
static int win_found;

/* Current state */
static game_state_t g_state;
static game_mode_t  g_mode;
static int          g_turn;      /* PIECE_P1 or PIECE_P2                   */
static int          g_hover_col; /* column mouse is hovering over (-1=none) */

/* Drop animation */
static int  anim_col;
static int  anim_row;     /* target row                                    */
static int  anim_piece;
static int  anim_y_fp;    /* current Y in 16.16 fixed-point (pixels*65536) */
static int  anim_vy_fp;   /* velocity in 16.16 fp                         */
static int  anim_target_y;/* pixel Y of destination disc center            */
static int  anim_src_y;   /* pixel Y of source (top of board)              */

/* Win flash timer */
static u64  win_flash_start;

/* =========================================================================
 * Board helpers
 * ========================================================================= */
static void board_reset(void)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            board[r][c] = PIECE_NONE;
    win_found = 0;
}

/* Cell center pixel coordinates (row 0 = bottom of board). */
static int cell_cx(int col)
{
    return BOARD_X + col * CELL_W + CELL_W / 2;
}
static int cell_cy(int row)
{
    /* row 0 is the bottom logical row → highest pixel Y */
    int visual_row = (ROWS - 1) - row;
    return BOARD_Y + visual_row * CELL_H + CELL_H / 2;
}

/* Return the lowest empty row in a column, or -1 if full. */
static int drop_row(int col)
{
    for (int r = 0; r < ROWS; r++) {
        if (board[r][col] == PIECE_NONE)
            return r;
    }
    return -1;
}

/* Check if the board is completely full. */
static int board_full(void)
{
    for (int c = 0; c < COLS; c++)
        if (board[ROWS-1][c] == PIECE_NONE)
            return 0;
    return 1;
}

/* =========================================================================
 * Win detection
 * ========================================================================= */
static int check_win_for(int piece)
{
    /* directions: (dr, dc) pairs for right, up, diag-up-right, diag-up-left */
    int dirs[4][2] = { {0,1}, {1,0}, {1,1}, {1,-1} };

    for (int dr = 0; dr < 4; dr++) {
        int drow = dirs[dr][0], dcol = dirs[dr][1];
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                if (board[r][c] != piece) continue;
                /* Try to extend 4 in this direction */
                int ok = 1;
                for (int k = 1; k < 4; k++) {
                    int nr = r + drow*k, nc = c + dcol*k;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS ||
                        board[nr][nc] != piece) {
                        ok = 0; break;
                    }
                }
                if (ok) {
                    /* Record the 4 winning cells */
                    for (int k = 0; k < 4; k++) {
                        win_cells[k][0] = r + drow*k;
                        win_cells[k][1] = c + dcol*k;
                    }
                    win_found = 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* =========================================================================
 * AI: minimax with alpha-beta pruning
 * ========================================================================= */
#define AI_PIECE   PIECE_P2
#define HUM_PIECE  PIECE_P1
#define AI_DEPTH   6

/* Count how many 'piece' are in a 4-window, with no opponent blocking. */
static int score_window(int a, int b, int c_, int d, int piece)
{
    int cnt = 0, empty = 0;
    int opp = (piece == PIECE_P1) ? PIECE_P2 : PIECE_P1;
    int cells[4] = { a, b, c_, d };
    for (int i = 0; i < 4; i++) {
        if (cells[i] == piece) cnt++;
        else if (cells[i] == PIECE_NONE) empty++;
    }
    /* If opponent is in there, this window is blocked */
    int opp_cnt = 0;
    for (int i = 0; i < 4; i++) if (cells[i] == opp) opp_cnt++;
    if (opp_cnt > 0) return 0;

    if (cnt == 4) return 1000000;
    if (cnt == 3 && empty == 1) return 50;
    if (cnt == 2 && empty == 2) return 3;
    return 0;
}

/* Static heuristic evaluation of board from AI perspective. */
static int eval_board(void)
{
    int score = 0;

    /* Center column preference (col 3): higher weight */
    for (int r = 0; r < ROWS; r++) {
        if (board[r][3] == AI_PIECE)  score += 6;
        if (board[r][3] == HUM_PIECE) score -= 6;
        /* Adjacent to center (cols 2,4) */
        if (board[r][2] == AI_PIECE || board[r][4] == AI_PIECE)  score += 3;
        if (board[r][2] == HUM_PIECE || board[r][4] == HUM_PIECE) score -= 3;
    }

    /* Score all horizontal windows */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c <= COLS-4; c++) {
            score += score_window(board[r][c],   board[r][c+1],
                                   board[r][c+2], board[r][c+3], AI_PIECE);
            score -= score_window(board[r][c],   board[r][c+1],
                                   board[r][c+2], board[r][c+3], HUM_PIECE);
        }
    }

    /* Score all vertical windows */
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r <= ROWS-4; r++) {
            score += score_window(board[r][c],   board[r+1][c],
                                   board[r+2][c], board[r+3][c], AI_PIECE);
            score -= score_window(board[r][c],   board[r+1][c],
                                   board[r+2][c], board[r+3][c], HUM_PIECE);
        }
    }

    /* Score all diagonal (up-right) windows */
    for (int r = 0; r <= ROWS-4; r++) {
        for (int c = 0; c <= COLS-4; c++) {
            score += score_window(board[r][c],     board[r+1][c+1],
                                   board[r+2][c+2], board[r+3][c+3], AI_PIECE);
            score -= score_window(board[r][c],     board[r+1][c+1],
                                   board[r+2][c+2], board[r+3][c+3], HUM_PIECE);
        }
    }

    /* Score all diagonal (up-left) windows */
    for (int r = 0; r <= ROWS-4; r++) {
        for (int c = COLS-1; c >= 3; c--) {
            score += score_window(board[r][c],     board[r+1][c-1],
                                   board[r+2][c-2], board[r+3][c-3], AI_PIECE);
            score -= score_window(board[r][c],     board[r+1][c-1],
                                   board[r+2][c-2], board[r+3][c-3], HUM_PIECE);
        }
    }

    return score;
}

/* Check terminal state: returns +big if AI wins, -big if human wins, 0 otherwise. */
static int terminal_score(void)
{
    /* Quick scan for 4-in-a-row */
    int dirs[4][2] = { {0,1}, {1,0}, {1,1}, {1,-1} };
    for (int dr = 0; dr < 4; dr++) {
        int drow = dirs[dr][0], dcol = dirs[dr][1];
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                int p = board[r][c];
                if (p == PIECE_NONE) continue;
                int ok = 1;
                for (int k = 1; k < 4; k++) {
                    int nr = r + drow*k, nc = c + dcol*k;
                    if (nr<0||nr>=ROWS||nc<0||nc>=COLS||board[nr][nc]!=p)
                    { ok=0; break; }
                }
                if (ok) {
                    return (p == AI_PIECE) ? 2000000 : -2000000;
                }
            }
        }
    }
    return 0;
}

/* Column preference order: center outward */
static const int COL_ORDER[COLS] = { 3, 2, 4, 1, 5, 0, 6 };

/* minimax with alpha-beta pruning.
 * maximizing=1: AI's turn; maximizing=0: human's turn. */
static int minimax(int depth, int alpha, int beta, int maximizing)
{
    int t = terminal_score();
    if (t != 0) {
        /* Scale by remaining depth so shallower wins are preferred */
        return t > 0 ? t + depth : t - depth;
    }
    if (depth == 0) return eval_board();
    if (board_full()) return 0;

    if (maximizing) {
        int best = -3000000;
        for (int ci = 0; ci < COLS; ci++) {
            int c = COL_ORDER[ci];
            int r = drop_row(c);
            if (r < 0) continue;
            board[r][c] = AI_PIECE;
            int val = minimax(depth - 1, alpha, beta, 0);
            board[r][c] = PIECE_NONE;
            if (val > best) best = val;
            if (best > alpha) alpha = best;
            if (alpha >= beta) break; /* prune */
        }
        return best;
    } else {
        int best = 3000000;
        for (int ci = 0; ci < COLS; ci++) {
            int c = COL_ORDER[ci];
            int r = drop_row(c);
            if (r < 0) continue;
            board[r][c] = HUM_PIECE;
            int val = minimax(depth - 1, alpha, beta, 1);
            board[r][c] = PIECE_NONE;
            if (val < best) best = val;
            if (best < beta) beta = best;
            if (alpha >= beta) break; /* prune */
        }
        return best;
    }
}

/* Return the best column for the AI to play. */
static int ai_best_col(void)
{
    int best_col = -1;
    int best_val = -3000000;

    /* Check if AI can win immediately */
    for (int ci = 0; ci < COLS; ci++) {
        int c = COL_ORDER[ci];
        int r = drop_row(c);
        if (r < 0) continue;
        board[r][c] = AI_PIECE;
        int t = terminal_score();
        board[r][c] = PIECE_NONE;
        if (t > 0) return c; /* instant win */
    }

    /* Check if we must block human's immediate win */
    for (int ci = 0; ci < COLS; ci++) {
        int c = COL_ORDER[ci];
        int r = drop_row(c);
        if (r < 0) continue;
        board[r][c] = HUM_PIECE;
        int t = terminal_score();
        board[r][c] = PIECE_NONE;
        if (t < 0) {
            /* Human wins here -- we must block */
            best_col = c;
            best_val = 1999999;
        }
    }
    if (best_val >= 1999999) return best_col;

    /* Full minimax search */
    for (int ci = 0; ci < COLS; ci++) {
        int c = COL_ORDER[ci];
        int r = drop_row(c);
        if (r < 0) continue;
        board[r][c] = AI_PIECE;
        int val = minimax(AI_DEPTH - 1, -3000000, 3000000, 0);
        board[r][c] = PIECE_NONE;
        if (val > best_val) {
            best_val = val;
            best_col = c;
        }
    }
    if (best_col < 0) {
        /* Fallback: any valid column */
        for (int c = 0; c < COLS; c++) {
            if (drop_row(c) >= 0) { best_col = c; break; }
        }
    }
    return best_col;
}

/* =========================================================================
 * Drop animation helpers
 * ========================================================================= */
#define ANIM_GRAVITY_FP  (3 * 65536)  /* acceleration per frame (FP 16.16) */
#define ANIM_V0_FP       (1 * 65536)  /* initial downward velocity          */

static void start_drop_anim(int col, int row, int piece)
{
    anim_col    = col;
    anim_row    = row;
    anim_piece  = piece;
    anim_src_y  = BOARD_Y + CELL_H / 2;   /* top of board area, disc center */
    anim_target_y = cell_cy(row);
    anim_y_fp   = anim_src_y * 65536;
    anim_vy_fp  = ANIM_V0_FP;
    g_state     = STATE_ANIM;
}

/* Advance animation one frame; returns 1 when done. */
static int advance_anim(void)
{
    anim_vy_fp += ANIM_GRAVITY_FP;
    anim_y_fp  += anim_vy_fp;

    int cy = anim_y_fp / 65536;

    if (cy >= anim_target_y) {
        /* Snap to target and finalize */
        board[anim_row][anim_col] = (i8)anim_piece;
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Column under mouse
 * ========================================================================= */
static int col_from_mouse_x(int mx)
{
    if (mx < BOARD_X || mx >= BOARD_X + BOARD_W) return -1;
    int c = (mx - BOARD_X) / CELL_W;
    if (c < 0) c = 0;
    if (c >= COLS) c = COLS - 1;
    return c;
}

/* =========================================================================
 * Rendering
 * ========================================================================= */
static void draw_disc(game_t *g, int cx, int cy, int piece, int r)
{
    if (piece == PIECE_P1) {
        g_circle(g, cx, cy, r,     COL_P1);
        g_circle(g, cx-4, cy-4, r/4, 0xFFFF8A80); /* highlight */
    } else if (piece == PIECE_P2) {
        g_circle(g, cx, cy, r,     COL_P2);
        g_circle(g, cx-4, cy-4, r/4, 0xFFFFF9C4); /* highlight */
    }
}

static int is_win_cell(int row, int col)
{
    if (!win_found) return 0;
    for (int k = 0; k < 4; k++)
        if (win_cells[k][0] == row && win_cells[k][1] == col)
            return 1;
    return 0;
}

static void render_frame(game_t *g)
{
    /* -- Background -- */
    g_clear(g, COL_BG);

    /* -- HUD strip -- */
    g_fill_rect(g, 0, 0, WIN_W, HUD_H, COL_HUD_BG);
    g_fill_rect(g, 0, HUD_H-2, WIN_W, 2, COL_BOARD_DARK);

    /* Title */
    g_text(g, 10, 12, "CONNECT FOUR", COL_CYAN);

    /* Mode label */
    if (g_mode == MODE_VS_AI)
        g_text(g, WIN_W - 170, 12, "vs AI (M=2P)", COL_GRAY);
    else
        g_text(g, WIN_W - 180, 12, "2-PLAYER (M=AI)", COL_GRAY);

    /* -- Board background -- */
    g_fill_rect(g, BOARD_X - 4, BOARD_Y, BOARD_W + 8, BOARD_H, COL_BOARD);

    /* Draw each cell slot */
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            int cx = cell_cx(col);
            int cy = cell_cy(row);
            int piece = board[row][col];

            /* Draw cell background hole */
            g_circle(g, cx, cy, DISC_R + 2, COL_BOARD_DARK);
            g_circle(g, cx, cy, DISC_R,     COL_EMPTY);

            /* Draw disc if placed */
            if (piece != PIECE_NONE) {
                /* Win flash: alternate green/normal on win cells */
                if (is_win_cell(row, col)) {
                    u64 now = game_ticks();
                    u64 elapsed = now - win_flash_start;
                    int flash = (int)((elapsed / 250) % 2);
                    if (flash) {
                        g_circle(g, cx, cy, DISC_R, COL_WIN_RING);
                        g_circle(g, cx, cy, DISC_R - 4,
                                 (piece == PIECE_P1) ? COL_P1 : COL_P2);
                    } else {
                        draw_disc(g, cx, cy, piece, DISC_R);
                    }
                } else {
                    draw_disc(g, cx, cy, piece, DISC_R);
                }
            }
        }
    }

    /* -- Drop preview: shadow disc in hovered column -- */
    if (g_state == STATE_PLAYING && g_hover_col >= 0) {
        int r = drop_row(g_hover_col);
        if (r >= 0) {
            /* Draw a faint disc at top of the hovered column */
            int cx = cell_cx(g_hover_col);
            int cy = BOARD_Y - DISC_R - 4;
            u32 col = (g_turn == PIECE_P1) ? COL_P1 : COL_P2;
            /* Slightly transparent version by blending with bg */
            g_circle(g, cx, cy, DISC_R, col);
            g_circle(g, cx, cy, DISC_R - 3, COL_BG);
            g_circle(g, cx, cy, DISC_R - 6, col);
            /* Downward arrow indicator */
            g_fill_rect(g, cx - 2, cy + DISC_R + 2, 5, 8, col);
            /* Arrow head */
            g_fill_rect(g, cx - 5, cy + DISC_R + 8, 11, 2, col);
            g_fill_rect(g, cx - 3, cy + DISC_R + 10, 7, 2, col);
            g_fill_rect(g, cx - 1, cy + DISC_R + 12, 3, 2, col);
        }
    }

    /* -- Falling animation disc -- */
    if (g_state == STATE_ANIM) {
        int cx = cell_cx(anim_col);
        int cy = anim_y_fp / 65536;
        draw_disc(g, cx, cy, anim_piece, DISC_R);
    }

    /* -- Turn indicator / status messages -- */
    if (g_state == STATE_PLAYING || g_state == STATE_ANIM) {
        /* Turn badge at bottom of HUD */
        const char *whose = "";
        u32 tcol = COL_WHITE;
        if (g_turn == PIECE_P1) {
            whose = "RED";
            tcol  = COL_RED_TXT;
        } else {
            if (g_mode == MODE_VS_AI) {
                whose = "YELLOW (AI)";
            } else {
                whose = "YELLOW";
            }
            tcol = COL_YELLOW_TXT;
        }
        /* Center the turn string */
        g_text_center(g, WIN_W / 2, HUD_H / 2, whose, tcol);
    } else if (g_state == STATE_WIN) {
        const char *msg = "";
        u32 tcol = COL_WHITE;
        /* Who won? Check win_cells[0] piece */
        int wp = board[ win_cells[0][0] ][ win_cells[0][1] ];
        if (wp == PIECE_P1) {
            msg  = (g_mode == MODE_2PLAYER) ? "RED WINS!" : "YOU WIN!";
            tcol = COL_RED_TXT;
        } else {
            msg  = (g_mode == MODE_VS_AI) ? "AI WINS!" : "YELLOW WINS!";
            tcol = COL_YELLOW_TXT;
        }
        g_text_center(g, WIN_W / 2, HUD_H / 2, msg, tcol);
    } else if (g_state == STATE_DRAW) {
        g_text_center(g, WIN_W / 2, HUD_H / 2, "DRAW!", COL_GRAY);
    } else if (g_state == STATE_MENU) {
        g_text_center(g, WIN_W / 2, HUD_H / 2, "SELECT MODE (M=Toggle, Enter=Start)", COL_CYAN);
    }

    /* -- Bottom bar -- */
    g_fill_rect(g, 0, WIN_H - BOT_H, WIN_W, BOT_H, COL_BOT_BG);
    g_fill_rect(g, 0, WIN_H - BOT_H, WIN_W, 2, COL_BOARD_DARK);

    if (g_state == STATE_MENU) {
        /* Mode selection display */
        const char *m1 = "Mode:";
        const char *m2 = (g_mode == MODE_VS_AI) ? "[VS AI]" : " VS AI ";
        const char *m3 = (g_mode == MODE_2PLAYER) ? "[2-PLAYER]" : " 2-PLAYER ";
        g_text_center(g, WIN_W/2, WIN_H - BOT_H + 20, "Choose mode, then press ENTER or click to start", COL_WHITE);
        g_text(g, 150, WIN_H - BOT_H + 44, m1, COL_GRAY);
        u32 c2 = (g_mode == MODE_VS_AI)    ? COL_GREEN_TXT : COL_GRAY;
        u32 c3 = (g_mode == MODE_2PLAYER)  ? COL_GREEN_TXT : COL_GRAY;
        g_text(g, 210, WIN_H - BOT_H + 44, m2, c2);
        g_text(g, 300, WIN_H - BOT_H + 44, m3, c3);
        g_text_center(g, WIN_W/2, WIN_H - BOT_H + 64, "M = toggle mode", COL_GRAY);
    } else {
        g_text(g, 10, WIN_H - BOT_H + 14, "Click column to drop.  N=New  M=Mode  ESC=Quit", COL_GRAY);
        /* Player legend */
        g_circle(g, 30,  WIN_H - BOT_H + 52, 12, COL_P1);
        g_text(g, 47, WIN_H - BOT_H + 46,
               (g_mode == MODE_2PLAYER) ? "P1 (Red)" : "You (Red)", COL_RED_TXT);
        g_circle(g, 200, WIN_H - BOT_H + 52, 12, COL_P2);
        g_text(g, 217, WIN_H - BOT_H + 46,
               (g_mode == MODE_2PLAYER) ? "P2 (Yellow)" : "AI (Yellow)", COL_YELLOW_TXT);

        if (g_state == STATE_WIN || g_state == STATE_DRAW) {
            g_text_center(g, WIN_W/2, WIN_H - BOT_H + 64, "N = new game", COL_GREEN_TXT);
        }
    }
}

/* =========================================================================
 * New game
 * ========================================================================= */
static void new_game(void)
{
    board_reset();
    g_turn  = PIECE_P1;
    g_state = STATE_PLAYING;
    win_flash_start = 0;
}

/* =========================================================================
 * Place a disc (human or AI) -- starts animation
 * ========================================================================= */
static void place_disc(int col)
{
    if (col < 0 || col >= COLS) return;
    int r = drop_row(col);
    if (r < 0) return; /* column full */
    /* Don't place on board yet -- animation will do it */
    int piece = g_turn;
    g_beep(880, 40);
    start_drop_anim(col, r, piece);
}

/* Called when animation finishes: disc is now on board. */
static void on_disc_placed(void)
{
    int row = anim_row, col = anim_col, piece = anim_piece;

    /* Check win */
    if (check_win_for(piece)) {
        g_state = STATE_WIN;
        win_flash_start = game_ticks();
        g_beep(1047, 80);
        g_beep(1319, 80);
        g_beep(1568, 120);
        /* Serial output */
        if (g_mode == MODE_VS_AI) {
            if (piece == HUM_PIECE) _print("[C4] player wins\n");
            else                    _print("[C4] AI wins\n");
        } else {
            if (piece == PIECE_P1) _print("[C4] player 1 wins\n");
            else                   _print("[C4] player 2 wins\n");
        }
        return;
    }

    /* Check draw */
    if (board_full()) {
        g_state = STATE_DRAW;
        g_beep(440, 200);
        _print("[C4] draw\n");
        return;
    }

    /* Switch turns */
    g_turn = (piece == PIECE_P1) ? PIECE_P2 : PIECE_P1;
    g_state = STATE_PLAYING;

    (void)row; (void)col; /* suppress warnings */
}

/* =========================================================================
 * _start entry point
 * ========================================================================= */
void _start(void)
{
    _print("[C4] starting\n");

    game_t *g = game_open(WIN_W, WIN_H, "Connect Four");
    if (!g) {
        _print("[C4] game_open FAILED\n");
        for (;;) {}
    }

    /* Initial state */
    g_mode      = MODE_VS_AI;
    g_state     = STATE_MENU;
    g_hover_col = -1;
    board_reset();
    g_turn = PIECE_P1;

    int prev_btn = 0;

    while (game_frame_begin(g)) {
        int mx, my, btn;
        game_mouse(g, &mx, &my, &btn);

        /* Compute hover column (always track) */
        if (my >= BOARD_Y && my < BOARD_Y + BOARD_H)
            g_hover_col = col_from_mouse_x(mx);
        else
            g_hover_col = -1;

        int clicked = (btn & MOUSE_LEFT) && !(prev_btn & MOUSE_LEFT);

        /* -- Global keys -- */
        if (game_key_pressed(g, KEY_ESC)) break;

        if (game_key_pressed(g, KEY_N) && g_state != STATE_ANIM) {
            new_game();
            goto render;
        }

        /* Toggle mode key: M or TAB */
        if ((game_key_pressed(g, KEY_M) || game_key_pressed(g, KEY_TAB))
            && g_state != STATE_ANIM) {
            g_mode = (g_mode == MODE_VS_AI) ? MODE_2PLAYER : MODE_VS_AI;
            if (g_state != STATE_MENU) new_game();
            goto render;
        }

        /* -- State machine -- */
        switch (g_state) {

        case STATE_MENU:
            if (game_key_pressed(g, KEY_ENTER) || game_key_pressed(g, KEY_SPACE)
                || clicked) {
                new_game();
            }
            break;

        case STATE_PLAYING:
            /* Human turn input */
            if (g_mode == MODE_2PLAYER ||
                (g_mode == MODE_VS_AI && g_turn == HUM_PIECE)) {

                if (clicked && g_hover_col >= 0) {
                    place_disc(g_hover_col);
                }

                /* Keyboard: number keys 1-7 select column */
                for (int k = 0; k < COLS; k++) {
                    if (game_key_pressed(g, KEY_1 + k)) {
                        place_disc(k);
                        break;
                    }
                }
            }

            /* AI turn: trigger immediately */
            if (g_mode == MODE_VS_AI && g_turn == AI_PIECE
                && g_state == STATE_PLAYING) {
                int col = ai_best_col();
                if (col >= 0) place_disc(col);
            }
            break;

        case STATE_ANIM:
            if (advance_anim()) {
                on_disc_placed();
            }
            break;

        case STATE_WIN:
        case STATE_DRAW:
            /* N handled above globally */
            if (game_key_pressed(g, KEY_ENTER) || game_key_pressed(g, KEY_SPACE)) {
                new_game();
            }
            break;
        }

render:
        render_frame(g);
        game_present(g);
        game_sync(g);

        prev_btn = btn;
    }
}
