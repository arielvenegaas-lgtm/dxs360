/* theme.h: colour + metric palette for the Eden360 frontend.
 *
 * Colours are 0xAARRGGBB (the renderer's native layout). The palette is a
 * dark theme inspired by Eden's "default_dark" Qt stylesheet: a deep slate
 * background with a bright sky-blue accent. All values here are our own.
 */
#pragma once

#include <stdint.h>

#define COL_BG        0xFF19232Du /* page background        */
#define COL_BG_2      0xFF0F1922u /* deeper background (edge) */
#define COL_PANEL     0xFF232629u /* cards / bars            */
#define COL_PANEL_2   0xFF2A2929u /* raised cards            */
#define COL_LINE      0xFF32414Bu /* separators / borders    */
#define COL_TEXT      0xFFEFF0F1u /* primary text            */
#define COL_TEXT_DIM  0xFF9AA3ABu /* secondary text          */
#define COL_MUTED     0xFF76797Cu /* hints                   */
#define COL_ACCENT    0xFF3DAEE9u /* Eden sky blue           */
#define COL_ACCENT_D  0xFF148CD2u /* pressed / highlight     */
#define COL_ACCENT_DD 0xFF1464A0u /* selection bar           */
#define COL_GOOD      0xFF40DD40u
#define COL_WARN      0xFFE85C00u
#define COL_BAD       0xFFFF5464u
#define COL_TILE      0xFF2B3A46u /* placeholder art base    */
#define COL_BLACK     0xFF000000u
#define COL_WHITE     0xFFFFFFFFu
