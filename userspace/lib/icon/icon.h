/**
 * icon.h  --  Procedural icon library for the desktop shell
 *
 * Draws crisp, recognisable app/file icons entirely from primitives into a
 * caller-supplied ARGB32 pixel buffer (0xAARRGGBB layout, same as the rest
 * of the userspace graphics stack).
 *
 * All functions are pure C11, freestanding-safe:
 *   -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone
 * No libc, no libm, no syscalls.  Integer / fixed-point arithmetic only.
 *
 * Coordinate convention
 * ---------------------
 * Every public function takes:
 *   uint32_t *px      -- base pointer of the pixel buffer
 *   int       stride  -- row stride in PIXELS (not bytes)
 *   int       x, y   -- top-left corner of the icon cell
 *   int       size    -- icon cell side length in pixels (works best 16-128)
 *
 * Pixel format: 0xAARRGGBB  (alpha in bits 31-24).
 *
 * Dependencies
 * ------------
 * The glyph-text path in icon_rounded_tile() uses bitfont.h from the same
 * tree.  Callers that do not invoke icon_rounded_tile() do not need bitfont.
 */

#ifndef ICON_H
#define ICON_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Generic rounded-tile icon
 * -------------------------------------------------------------------------
 * Draws a rounded-rectangle background with a subtle top-to-bottom vertical
 * gradient (bg lightened ~12 % at top, bg darkened ~12 % at bottom), then
 * overlays up to two characters of `glyph` text centered inside the tile.
 *
 * bg   -- base fill colour  (0xAARRGGBB)
 * fg   -- glyph text colour (0xAARRGGBB)
 * glyph -- NUL-terminated string; only the first 1-2 printable chars used.
 *          Pass NULL or "" to draw the tile without text.
 */
void icon_rounded_tile(uint32_t *px, int stride, int x, int y, int size,
                       uint32_t bg, uint32_t fg, const char *glyph);

/* -------------------------------------------------------------------------
 * Themed single-purpose icons
 * -------------------------------------------------------------------------
 * Each draws a recognisable symbol inside a rounded tile of the given size.
 * `accent` is the dominant hue used for the icon art; the tile background
 * is automatically derived as a darker, slightly desaturated shade of the
 * accent so the symbol pops.
 *
 * All icons look clean at 32 px and good at 16-128 px.
 */

/** Terminal: >_ prompt in a dark shell background */
void icon_terminal(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent);

/** Folder: classic manila-folder silhouette */
void icon_folder(uint32_t *px, int stride, int x, int y, int size,
                 uint32_t accent);

/** Text file: white page with horizontal text-line rules and a dog-ear */
void icon_file_text(uint32_t *px, int stride, int x, int y, int size,
                    uint32_t accent);

/** Paint: artist's brush over a small colour-swatch palette */
void icon_paint(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent);

/** Calculator: grid of square buttons with an = row */
void icon_calc(uint32_t *px, int stride, int x, int y, int size,
               uint32_t accent);

/** Clock: round face with hour and minute hands */
void icon_clock(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent);

/** Game: simplified D-pad + two action buttons */
void icon_game(uint32_t *px, int stride, int x, int y, int size,
               uint32_t accent);

/** Settings: 8-tooth gear silhouette */
void icon_settings(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent);

/** Chart: three ascending bar-chart columns */
void icon_chart(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent);

/** Music: an eighth-note (filled note-head + stem + flag) */
void icon_music(uint32_t *px, int stride, int x, int y, int size,
                uint32_t accent);

/** Calendar: monthly-grid view with a coloured header bar */
void icon_calendar(uint32_t *px, int stride, int x, int y, int size,
                   uint32_t accent);

/** IDE: dark tile with a small node-graph (connected boxes + link lines)
 *  and a "</>" code mark -- reads as "code map / blueprint". */
void icon_ide(uint32_t *px, int stride, int x, int y, int size,
              uint32_t accent);

/* -------------------------------------------------------------------------
 * Drop shadow helper
 * -------------------------------------------------------------------------
 * Paints a soft rectangular shadow beneath an icon cell.  Call BEFORE the
 * icon itself so the icon renders on top.
 *
 * opacity -- 0-255 alpha for the darkest shadow pixel
 * spread  -- shadow radius in pixels (try 3-6)
 */
void icon_draw_shadow(uint32_t *px, int stride, int x, int y, int size,
                      int spread, uint8_t opacity);

/* -------------------------------------------------------------------------
 * Accent colour helper
 * -------------------------------------------------------------------------
 * Hashes `name` to one of ~16 pleasant, distinctive accent colours so that
 * generic app tiles look varied without requiring explicit colours.
 * Returns a fully-opaque 0xFFRRGGBB value.
 */
uint32_t icon_accent_for_name(const char *name);

/* -------------------------------------------------------------------------
 * High-level dispatch
 * -------------------------------------------------------------------------
 * Maps well-known app names (case-insensitive prefix match) to the themed
 * icon functions above, and falls back to icon_rounded_tile() with the
 * first 1-2 initials of `app_name` for unknown applications.
 *
 * Known mappings
 * --------------
 *   "terminal" / "term" / "shell" / "bash" / "zsh"  -> icon_terminal
 *   "folder"   / "files" / "fm"   / "explorer"       -> icon_folder
 *   "text"     / "editor"/ "nano" / "vim" / "gedit"  -> icon_file_text
 *   "paint"    / "draw"  / "gimp" / "krita"          -> icon_paint
 *   "calc"     / "calculator" / "bc"                 -> icon_calc
 *   "clock"    / "time"  / "watch" / "alarm"         -> icon_clock
 *   "game"     / "games" / "play"                    -> icon_game
 *   "settings" / "prefs" / "config" / "system"       -> icon_settings
 *   "chart"    / "graph" / "plot"  / "stats"         -> icon_chart
 *   "music"    / "audio" / "sound" / "synth" / "mpd" -> icon_music
 *   "calendar" / "cal"   / "dates" / "dateapp"       -> icon_calendar
 *   "ide"      / "code"  / "blueprint" / "devtool"   -> icon_ide
 *   "sysmon"   / "taskman" / "monitor" / "stats"     -> icon_chart
 *   (everything else)                                -> icon_rounded_tile
 */
void icon_for_app(uint32_t *px, int stride, int x, int y, int size,
                  const char *app_name);

#endif /* ICON_H */
