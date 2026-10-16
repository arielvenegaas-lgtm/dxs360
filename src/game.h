/* game.h: "Aurora Bricks" - a 2D Breakout-style game for Xbox 360.
 * Pure logic + drawing into the GfxContext software framebuffer.
 *
 * This translation unit is shared verbatim by both the D3D9 (XDK) build and the
 * libXenon (bare-metal) build, so it only ever touches the Gfx_* / PadState API.
 */
#pragma once

#include "gfx.h"
#include "input.h"

#define BRICK_COLS 10
#define BRICK_ROWS 5

#define MAX_BALLS     8
#define MAX_POWERS    8
#define MAX_PARTICLES 192
#define MAX_SHOTS     8

typedef enum {
    GAME_READY = 0,
    GAME_PLAYING,
    GAME_LEVELCLEAR,
    GAME_GAMEOVER,
    GAME_WIN
} GameState;

/* Brick kinds. */
enum {
    BRICK_NORMAL = 0,  /* 1 hit                          */
    BRICK_STRONG,      /* 2 hits                         */
    BRICK_TOUGH,       /* 3 hits                         */
    BRICK_SOLID,       /* indestructible (not required)  */
    BRICK_BOMB         /* 1 hit; explodes its neighbours */
};

/* Power-up kinds. */
enum {
    PW_WIDE = 0,   /* wider paddle (timed)        */
    PW_MULTI,      /* split into extra balls      */
    PW_SLOW,       /* slow the balls (timed)      */
    PW_LASER,      /* fire shots (timed)          */
    PW_STICKY,     /* catch the ball (timed)      */
    PW_LIFE,       /* +1 life (instant)           */
    PW_SHRINK,     /* BAD: narrower paddle        */
    PW_FAST,       /* BAD: faster balls           */
    PW_COUNT
};

typedef struct {
    int alive;
    int hp;
    int maxhp;
    int kind;
    int power;      /* carries a guaranteed power-up */
} Brick;

typedef struct {
    int   active;
    float x, y, vx, vy, r;
    int   stuck;    /* held on the paddle by the sticky power */
} Ball;

typedef struct {
    int   active;
    float x, y, vy;
    int   kind;
} Power;

typedef struct {
    int      active;
    float    x, y, vx, vy;
    float    life, maxlife;
    uint32_t color;
} Particle;

typedef struct {
    int   active;
    float x, y;
} Shot;

typedef struct {
    uint32_t width, height;

    GameState state;
    float     state_timer;
    int       paused;

    float     paddle_x, paddle_w, paddle_h, paddle_y;
    float     paddle_w_eff;   /* current width incl. wide/shrink powers */

    Ball      balls[MAX_BALLS];
    int       ball_count;

    Brick     bricks[BRICK_ROWS * BRICK_COLS];
    float     brick_x0, brick_y0, brick_w, brick_h, brick_gap;
    int       bricks_left;    /* destructible bricks still standing */

    Power     powers[MAX_POWERS];
    Particle  particles[MAX_PARTICLES];
    Shot      shots[MAX_SHOTS];

    /* power-up effect timers, seconds remaining */
    float     pw_wide_t, pw_slow_t, pw_laser_t, pw_sticky_t, pw_shrink_t, pw_fast_t;
    float     shot_cd;

    int       score;
    int       lives;
    int       level;
    int       combo;          /* bricks broken since the last paddle touch */
    int       best_combo;

    uint32_t  rng;

    const char *level_name;

    /* last measured frames/sec, filled in by the platform layer (0 = hide) */
    float     fps;

    /* tuning, scaled to resolution */
    float     paddle_speed;
    float     ball_speed;
    float     ball_r;
    float     field_x0, field_x1, field_y0, field_y1;
    int       text_scale;
} Game;

void Game_Init(Game *g, uint32_t width, uint32_t height);
void Game_Reset(Game *g);
void Game_Update(Game *g, const PadState *pad, float dt);
void Game_Draw(Game *g, GfxContext *gfx);
