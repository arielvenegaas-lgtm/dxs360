/* game.h: "Aurora Bricks" - a 2D Breakout-style game for Xbox 360.
 * Pure logic + drawing into the GfxContext software framebuffer.
 */
#pragma once

#include "gfx.h"
#include "input.h"

#define BRICK_COLS 10
#define BRICK_ROWS 5

typedef enum {
    GAME_READY = 0,
    GAME_PLAYING,
    GAME_LEVELCLEAR,
    GAME_GAMEOVER
} GameState;

typedef struct {
    int   alive;
    int   hp;
} Brick;

typedef struct {
    uint32_t  width, height;

    GameState state;
    float     state_timer;
    int       paused;

    float     paddle_x, paddle_w, paddle_h, paddle_y;
    float     ball_x, ball_y, ball_vx, ball_vy, ball_r;
    int       ball_attached;

    Brick     bricks[BRICK_ROWS * BRICK_COLS];
    float     brick_x0, brick_y0, brick_w, brick_h, brick_gap;
    int       bricks_left;

    int       score;
    int       lives;
    int       level;

    /* last measured frames/sec, filled in by the platform layer (0 = hide) */
    float     fps;

    /* tuning, scaled to resolution */
    float     paddle_speed;
    float     ball_speed;
    float     field_x0, field_x1, field_y0, field_y1;
    int       text_scale;
} Game;

void Game_Init(Game *g, uint32_t width, uint32_t height);
void Game_Reset(Game *g);
void Game_Update(Game *g, const PadState *pad, float dt);
void Game_Draw(Game *g, GfxContext *gfx);
