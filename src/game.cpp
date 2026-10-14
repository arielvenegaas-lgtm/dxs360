/* game.cpp: "Aurora Bricks" - Breakout-style 2D game logic and drawing.
 *
 * Shared by the D3D9 (XDK) and libXenon builds; only uses the Gfx_* / PadState
 * API. Everything here is a plain software framebuffer + blit.
 */
#include "game.h"

#include <math.h>
#include <string.h>

/* 0xAARRGGBB */
#define COL_BG       0xFF0C0C14u
#define COL_BORDER   0xFF3A3A50u
#define COL_PADDLE   0xFF3FB6FFu
#define COL_PADDLE2  0xFF9FE0FFu
#define COL_BALL     0xFFFFFFFFu
#define COL_TEXT     0xFFF0F0F0u
#define COL_DIM      0xFF9098A0u
#define COL_ACCENT   0xFFFFD040u
#define COL_GOOD     0xFF60E060u
#define COL_BAD      0xFFE05050u
#define COL_SOLID    0xFF4A505Cu
#define COL_TOUGH    0xFFC0C8D0u
#define COL_BOMB     0xFFFF7020u

static const uint32_t BRICK_COLORS[BRICK_ROWS] = {
    0xFFE04040u, /* red    */
    0xFFE08830u, /* orange */
    0xFFE0D040u, /* yellow */
    0xFF50C050u, /* green  */
    0xFF5090E0u  /* blue   */
};

static float Clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- tiny integer-to-string helpers (avoid relying on printf) --- */
static int AppendStr(char *buf, int len, int cap, const char *s) {
    while (*s && len < cap - 1) buf[len++] = *s++;
    buf[len] = 0;
    return len;
}

static int AppendInt(char *buf, int len, int cap, int value) {
    char tmp[12];
    int n = 0;
    if (value < 0) { if (len < cap - 1) buf[len++] = '-'; value = -value; }
    if (value == 0) tmp[n++] = '0';
    while (value > 0 && n < 11) { tmp[n++] = (char)('0' + (value % 10)); value /= 10; }
    while (n > 0 && len < cap - 1) buf[len++] = tmp[--n];
    buf[len] = 0;
    return len;
}

static int TextW(const char *s, int scale) {
    return (int)strlen(s) * (6 * scale);
}

/* --- deterministic-ish RNG (so no controller still demos nicely) --- */
static uint32_t RandNext(Game *g) {
    g->rng = g->rng * 1664525u + 1013904223u;
    return g->rng;
}
static float RandUnit(Game *g) { return (float)(RandNext(g) >> 8) * (1.0f / 16777216.0f); }
static float RandRange(Game *g, float a, float b) { return a + (b - a) * RandUnit(g); }

static uint32_t Lighten(uint32_t c, int amt) {
    int r = (int)((c >> 16) & 0xFF) + amt;
    int g = (int)((c >> 8) & 0xFF) + amt;
    int b = (int)(c & 0xFF) + amt;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t Darken(uint32_t c, int amt) {
    int r = (int)((c >> 16) & 0xFF) - amt;
    int g = (int)((c >> 8) & 0xFF) - amt;
    int b = (int)(c & 0xFF) - amt;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ======================================================================
 * Levels
 * ====================================================================== */
typedef struct {
    const char *name;
    const char *rows[BRICK_ROWS];
    float       speed;   /* ball-speed multiplier for this level */
} LevelDef;

/* Legend:  ' ' empty   '.' normal(1hp)   'o' strong(2hp)   'X' tough(3hp)
 *          '#' solid (indestructible)     '*' bomb (1hp, blows up neighbours) */
static const LevelDef LEVELS[] = {
    { "FIRST STEPS", {
        "          ",
        "          ",
        "oooooooooo",
        "oooooooooo",
        "oooooooooo",
    }, 1.00f },
    { "CHECKERBOARD", {
        ".o.o.o.o.o",
        "o.o.o.o.o.",
        ".o.o.o.o.o",
        "o.o.o.o.o.",
        ".o.o.o.o.o",
    }, 1.03f },
    { "PYRAMID", {
        "    oo    ",
        "   oooo   ",
        "  oooooo  ",
        " oooooooo ",
        "oooooooooo",
    }, 1.06f },
    { "FORTRESS", {
        "##########",
        "#        #",
        "# XXXXXX #",
        "#        #",
        "#o o  o o#",
    }, 1.09f },
    { "FIREWORKS", {
        "          ",
        "  *    *  ",
        " oooooooo ",
        "  oooooo  ",
        "   oooo   ",
    }, 1.12f },
    { "BOWTIE", {
        "          ",
        "    oo    ",
        "   oooo   ",
        "  oo  oo  ",
        " oo    oo ",
    }, 1.15f },
    { "TUNNELS", {
        "XX  XX  XX",
        "          ",
        "oo      oo",
        "          ",
        "XX  XX  XX",
    }, 1.18f },
    { "DIAMOND", {
        "    XX    ",
        "   oooo   ",
        "  oo  oo  ",
        "   oooo   ",
        "    oo    ",
    }, 1.21f },
    { "GUARDIANS", {
        "          ",
        " #      # ",
        "  o.oo.o  ",
        " #      # ",
        "          ",
    }, 1.24f },
    { "FINAL GAUNTLET", {
        "X*X    X*X",
        "          ",
        "oooooooooo",
        "          ",
        "X*X    X*X",
    }, 1.28f },
};
#define LEVEL_COUNT ((int)(sizeof(LEVELS) / sizeof(LEVELS[0])))

static void Layout(Game *g) {
    float W = (float)g->width;
    float H = (float)g->height;

    g->field_x0 = W * 0.04f;
    g->field_x1 = W - W * 0.04f;
    g->field_y0 = H * 0.06f;
    g->field_y1 = H - H * 0.03f;

    g->paddle_w = W * 0.16f;
    g->paddle_w_eff = g->paddle_w;
    g->paddle_h = H * 0.025f;
    if (g->paddle_h < 6.0f) g->paddle_h = 6.0f;
    g->paddle_y = g->field_y1 - g->paddle_h - H * 0.01f;

    g->ball_r = W * 0.006f;
    if (g->ball_r < 3.0f) g->ball_r = 3.0f;

    g->paddle_speed = W * 1.10f;
    g->ball_speed   = W * 0.55f;

    float usable = g->field_x1 - g->field_x0;
    g->brick_gap = W * 0.004f;
    if (g->brick_gap < 2.0f) g->brick_gap = 2.0f;
    g->brick_w = usable / (float)BRICK_COLS - g->brick_gap;
    g->brick_h = H * 0.030f;
    g->brick_x0 = g->field_x0;
    g->brick_y0 = g->field_y0 + H * 0.06f;

    g->text_scale = (int)(W / 640.0f);
    if (g->text_scale < 2) g->text_scale = 2;
}

/* ======================================================================
 * Transient entities
 * ====================================================================== */
static void ClearTransients(Game *g) {
    memset(g->powers, 0, sizeof(g->powers));
    memset(g->particles, 0, sizeof(g->particles));
    memset(g->shots, 0, sizeof(g->shots));
    g->pw_wide_t = g->pw_slow_t = g->pw_laser_t = 0.0f;
    g->pw_sticky_t = g->pw_shrink_t = g->pw_fast_t = 0.0f;
    g->shot_cd = 0.0f;
    g->combo = 0;
}

static void SpawnBurst(Game *g, float x, float y, uint32_t color) {
    int made = 0;
    float spd = g->height * 0.55f;
    for (int i = 0; i < MAX_PARTICLES && made < 7; ++i) {
        Particle *p = &g->particles[i];
        if (p->active) continue;
        float a = RandRange(g, 0.0f, 6.2831853f);
        float s = RandRange(g, spd * 0.25f, spd);
        p->active = 1;
        p->x = x; p->y = y;
        p->vx = cosf(a) * s;
        p->vy = sinf(a) * s - spd * 0.2f;
        p->maxlife = RandRange(g, 0.35f, 0.75f);
        p->life = p->maxlife;
        p->color = color;
        made++;
    }
}

/* ======================================================================
 * Power-ups
 * ====================================================================== */
static uint32_t PowerColor(int kind) {
    switch (kind) {
    case PW_WIDE:   return 0xFF40D060u;
    case PW_MULTI:  return 0xFF40C0E0u;
    case PW_SLOW:   return 0xFF6070F0u;
    case PW_LASER:  return 0xFFE06040u;
    case PW_STICKY: return 0xFFE0C040u;
    case PW_LIFE:   return 0xFFE060A0u;
    case PW_SHRINK: return 0xFFA06060u;
    case PW_FAST:   return 0xFFC08040u;
    default:        return 0xFFA0A0A0u;
    }
}

static char PowerLetter(int kind) {
    switch (kind) {
    case PW_WIDE:   return 'W';
    case PW_MULTI:  return 'M';
    case PW_SLOW:   return 'S';
    case PW_LASER:  return 'L';
    case PW_STICKY: return 'C';
    case PW_LIFE:   return '+';
    case PW_SHRINK: return 'X';
    case PW_FAST:   return 'F';
    default:        return '?';
    }
}

static void SpawnPower(Game *g, int kind, float x, float y) {
    for (int i = 0; i < MAX_POWERS; ++i) {
        Power *p = &g->powers[i];
        if (p->active) continue;
        p->active = 1;
        p->x = x;
        p->y = y;
        p->vy = g->height * 0.30f;
        p->kind = kind;
        return;
    }
}

static void SplitBalls(Game *g) {
    int existing = g->ball_count;
    for (int i = 0; i < existing && g->ball_count < MAX_BALLS; ++i) {
        Ball *b = &g->balls[i];
        if (!b->active || b->stuck) continue;
        for (int k = 0; k < 2 && g->ball_count < MAX_BALLS; ++k) {
            float ang = (k == 0 ? 30.0f : -30.0f) * 0.01745329f;
            float ca = cosf(ang), sa = sinf(ang);
            float vx = b->vx * ca - b->vy * sa;
            float vy = b->vx * sa + b->vy * ca;
            for (int j = 0; j < MAX_BALLS; ++j) {
                Ball *nb = &g->balls[j];
                if (nb->active) continue;
                nb->active = 1;
                nb->stuck = 0;
                nb->r = g->ball_r;
                nb->x = b->x;
                nb->y = b->y;
                nb->vx = vx;
                nb->vy = vy;
                g->ball_count++;
                break;
            }
        }
    }
}

static void ApplyPower(Game *g, int kind) {
    switch (kind) {
    case PW_WIDE:   g->pw_wide_t = 16.0f;   g->pw_shrink_t = 0.0f; break;
    case PW_MULTI:  SplitBalls(g); break;
    case PW_SLOW:   g->pw_slow_t = 12.0f;   g->pw_fast_t = 0.0f;   break;
    case PW_LASER:  g->pw_laser_t = 16.0f;  break;
    case PW_STICKY: g->pw_sticky_t = 16.0f; break;
    case PW_LIFE:   g->lives++;  break;
    case PW_SHRINK: g->pw_shrink_t = 12.0f; g->pw_wide_t = 0.0f;   break;
    case PW_FAST:   g->pw_fast_t = 10.0f;   g->pw_slow_t = 0.0f;   break;
    }
    g->score += 50;
}

/* ======================================================================
 * Balls
 * ====================================================================== */
static void AttachBallToPaddle(Game *g) {
    memset(g->balls, 0, sizeof(g->balls));
    g->ball_count = 1;
    Ball *b = &g->balls[0];
    b->active = 1;
    b->stuck = 1;
    b->r = g->ball_r;
    b->x = g->paddle_x + g->paddle_w_eff * 0.5f;
    b->y = g->paddle_y - b->r - 1.0f;
    b->vx = b->vy = 0.0f;
}

static float BallSpeed(const Ball *b) {
    return sqrtf(b->vx * b->vx + b->vy * b->vy);
}

/* Keep minimum vertical/horizontal components so balls never crawl perfectly
 * sideways or bounce forever in a dead-vertical loop. */
static void ClampBallAngle(Game *g, Ball *b) {
    float speed = BallSpeed(b);
    if (speed <= 0.0f) return;
    float minvy = speed * 0.22f;
    float minvx = speed * 0.15f;
    int changed = 0;
    if (fabsf(b->vy) < minvy) {
        b->vy = (b->vy < 0.0f ? -1.0f : 1.0f) * minvy;
        changed = 1;
    }
    if (fabsf(b->vx) < minvx) {
        float sx;
        if (b->vx > 0.0f) sx = 1.0f;
        else if (b->vx < 0.0f) sx = -1.0f;
        else sx = (RandUnit(g) < 0.5f) ? -1.0f : 1.0f;
        b->vx = sx * minvx;
        changed = 1;
    }
    if (changed) {
        float s2 = sqrtf(b->vx * b->vx + b->vy * b->vy);
        if (s2 > 0.0f) {
            float k = speed / s2;
            b->vx *= k;
            b->vy *= k;
        }
    }
}

static uint32_t BrickBaseColor(int rIdx, int kind) {
    switch (kind) {
    case BRICK_SOLID:  return COL_SOLID;
    case BRICK_TOUGH:  return COL_TOUGH;
    case BRICK_BOMB:   return COL_BOMB;
    case BRICK_STRONG: return Lighten(BRICK_COLORS[rIdx], 0x28);
    default:           return BRICK_COLORS[rIdx];
    }
}

/* Kill a brick (already resolved as destroyed), with bomb chain + scoring. */
static void BreakBrick(Game *g, int idx, int depth) {
    Brick *b = &g->bricks[idx];
    if (!b->alive || b->kind == BRICK_SOLID) return;

    int rIdx = idx / BRICK_COLS;
    int cIdx = idx % BRICK_COLS;
    float cx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap) + g->brick_w * 0.5f;
    float cy = g->brick_y0 + rIdx * (g->brick_h + g->brick_gap) + g->brick_h * 0.5f;
    uint32_t col = BrickBaseColor(rIdx, b->kind);

    SpawnBurst(g, cx, cy, col);

    b->alive = 0;
    g->bricks_left--;

    if (depth == 0) {
        g->combo++;
        if (g->combo > g->best_combo) g->best_combo = g->combo;
    }
    int mult = g->combo > 0 ? g->combo : 1;
    if (mult > 10) mult = 10;
    g->score += 25 * g->level * mult;

    /* power drops: guaranteed carriers, plus a small random chance */
    if (b->power) {
        SpawnPower(g, (int)(RandNext(g) % PW_COUNT), cx, cy);
    } else if (RandUnit(g) < 0.12f) {
        SpawnPower(g, (int)(RandNext(g) % PW_COUNT), cx, cy);
    }

    /* bombs blow up their 8 neighbours (chain, depth-limited) */
    if (b->kind == BRICK_BOMB && depth < 4) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                int nr = rIdx + dy, nc = cIdx + dx;
                if (nr < 0 || nr >= BRICK_ROWS || nc < 0 || nc >= BRICK_COLS) continue;
                int ni = nr * BRICK_COLS + nc;
                if (g->bricks[ni].alive && g->bricks[ni].kind != BRICK_SOLID)
                    BreakBrick(g, ni, depth + 1);
            }
        }
    }
}

static void StepBall(Game *g, Ball *b, float sdt, float speedMul) {
    b->x += b->vx * speedMul * sdt;
    b->y += b->vy * speedMul * sdt;

    float r = b->r;

    if (b->x - r < g->field_x0) { b->x = g->field_x0 + r; b->vx = -b->vx; }
    if (b->x + r > g->field_x1) { b->x = g->field_x1 - r; b->vx = -b->vx; }
    if (b->y - r < g->field_y0) { b->y = g->field_y0 + r; b->vy = -b->vy; }

    /* paddle */
    if (b->vy > 0.0f && b->y + r >= g->paddle_y &&
        b->y - r <= g->paddle_y + g->paddle_h &&
        b->x + r >= g->paddle_x &&
        b->x - r <= g->paddle_x + g->paddle_w_eff) {
        b->y = g->paddle_y - r;
        g->combo = 0;
        if (g->pw_sticky_t > 0.0f) {
            b->stuck = 1;
            b->vx = b->vy = 0.0f;
            return;
        }
        float center = g->paddle_x + g->paddle_w_eff * 0.5f;
        float rel = Clampf((b->x - center) / (g->paddle_w_eff * 0.5f), -1.0f, 1.0f);
        float speed = g->ball_speed;
        b->vx = rel * speed * 0.85f + RandRange(g, -0.04f, 0.04f) * speed;
        float vy2 = speed * speed - b->vx * b->vx;
        b->vy = -(vy2 > 0.0f ? sqrtf(vy2) : speed * 0.5f);
        ClampBallAngle(g, b);
    }

    /* bricks */
    for (int i = 0; i < BRICK_ROWS * BRICK_COLS; ++i) {
        Brick *br = &g->bricks[i];
        if (!br->alive) continue;
        int rIdx = i / BRICK_COLS;
        int cIdx = i % BRICK_COLS;
        float bx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap);
        float by = g->brick_y0 + rIdx * (g->brick_h + g->brick_gap);
        float bw = g->brick_w, bh = g->brick_h;

        float nx = Clampf(b->x, bx, bx + bw);
        float ny = Clampf(b->y, by, by + bh);
        float dx = b->x - nx, dy = b->y - ny;
        if (dx * dx + dy * dy <= r * r) {
            float overlap_x = (b->x < bx + bw * 0.5f)
                              ? (b->x + r) - bx : (bx + bw) - (b->x - r);
            float overlap_y = (b->y < by + bh * 0.5f)
                              ? (b->y + r) - by : (by + bh) - (b->y - r);
            if (overlap_x < overlap_y) b->vx = -b->vx;
            else                       b->vy = -b->vy;
            ClampBallAngle(g, b);

            if (br->kind == BRICK_SOLID) break; /* can't be broken */

            br->hp--;
            if (br->hp <= 0) {
                BreakBrick(g, i, 0);
            } else {
                g->combo++;
                if (g->combo > g->best_combo) g->best_combo = g->combo;
                g->score += 5 * g->level;
                SpawnBurst(g, b->x, b->y, COL_DIM);
            }
            break; /* one brick per substep */
        }
    }
}

/* ======================================================================
 * Level loading
 * ====================================================================== */
static void LoadLevel(Game *g) {
    int idx = g->level - 1;
    if (idx < 0) idx = 0;
    if (idx >= LEVEL_COUNT) idx = LEVEL_COUNT - 1;
    const LevelDef *def = &LEVELS[idx];

    g->level_name = def->name;
    g->bricks_left = 0;

    for (int r = 0; r < BRICK_ROWS; ++r) {
        for (int c = 0; c < BRICK_COLS; ++c) {
            Brick *b = &g->bricks[r * BRICK_COLS + c];
            char ch = def->rows[r][c];
            b->alive = 0;
            b->hp = 0;
            b->maxhp = 0;
            b->kind = BRICK_NORMAL;
            b->power = 0;

            int hp = 0, kind = BRICK_NORMAL;
            switch (ch) {
            case '.': hp = 1; kind = BRICK_NORMAL; break;
            case 'o': hp = 2; kind = BRICK_STRONG; break;
            case 'X': hp = 3; kind = BRICK_TOUGH;  break;
            case '#': hp = 1; kind = BRICK_SOLID;  break;
            case '*': hp = 1; kind = BRICK_BOMB;   break;
            default:  continue;
            }
            b->alive = 1;
            b->hp = hp;
            b->maxhp = hp;
            b->kind = kind;
            if (kind != BRICK_SOLID) g->bricks_left++;
        }
    }

    /* Guarantee a few power-up carriers per level. */
    int carriers = 2 + g->level / 3;
    if (carriers > 5) carriers = 5;
    for (int n = 0; n < carriers; ++n) {
        for (int tries = 0; tries < 40; ++tries) {
            int i = (int)(RandNext(g) % (BRICK_ROWS * BRICK_COLS));
            Brick *b = &g->bricks[i];
            if (b->alive && b->kind != BRICK_SOLID && !b->power) {
                b->power = 1;
                break;
            }
        }
    }

    ClearTransients(g);

    g->paddle_w_eff = g->paddle_w;
    g->paddle_x = (g->field_x0 + g->field_x1) * 0.5f - g->paddle_w_eff * 0.5f;
    g->ball_speed = (float)g->width * 0.55f * def->speed;
    AttachBallToPaddle(g);

    g->state = GAME_READY;
    g->state_timer = 0.0f;
    g->paused = 0;
}

void Game_Init(Game *g, uint32_t width, uint32_t height) {
    memset(g, 0, sizeof(*g));
    g->width = width;
    g->height = height;
    g->rng = 0x1234567u;
    Layout(g);
    g->score = 0;
    g->lives = 3;
    g->level = 1;
    g->best_combo = 0;
    LoadLevel(g);
}

void Game_Reset(Game *g) {
    g->score = 0;
    g->lives = 3;
    g->level = 1;
    g->best_combo = 0;
    LoadLevel(g);
}

/* ======================================================================
 * Update
 * ====================================================================== */
static void FireShots(Game *g) {
    int made = 0;
    for (int i = 0; i < MAX_SHOTS && made < 2; ++i) {
        Shot *s = &g->shots[i];
        if (s->active) continue;
        s->active = 1;
        s->x = (made == 0) ? g->paddle_x + 3.0f
                           : g->paddle_x + g->paddle_w_eff - 3.0f;
        s->y = g->paddle_y - 4.0f;
        made++;
    }
    g->shot_cd = 0.22f;
}

void Game_Update(Game *g, const PadState *pad, float dt) {
    if (dt > 0.1f) dt = 0.1f;

    int connected = (pad && pad->connected);
    uint16_t btn = pad ? pad->buttons : 0;
    uint16_t prs = pad ? pad->pressed : 0;
    int launch = (prs & (PAD_A | PAD_B)) != 0;

    if (prs & PAD_START) g->paused = !g->paused;

    /* control axis, or an autopilot demo when no pad is connected */
    float axis = pad ? pad->lx : 0.0f;
    if (btn & PAD_LEFT)  axis -= 1.0f;
    if (btn & PAD_RIGHT) axis += 1.0f;
    int autoLaunch = 0;
    if (!connected) {
        /* attract-mode demo: keep the ball alive and aim at a live brick */
        float ballx = (g->field_x0 + g->field_x1) * 0.5f;
        float besty = -1e9f;
        if (g->state == GAME_PLAYING) {
            for (int i = 0; i < MAX_BALLS; ++i) {
                Ball *b = &g->balls[i];
                if (b->active && b->y > besty) { besty = b->y; ballx = b->x; }
            }
        }
        float halfw = g->paddle_w_eff * 0.5f;
        float rel = 0.0f;
        if (g->state == GAME_PLAYING) {
            int best = -1; float bestd = 1e18f;
            for (int i = 0; i < BRICK_ROWS * BRICK_COLS; ++i) {
                if (!g->bricks[i].alive) continue;
                int rIdx = i / BRICK_COLS, cIdx = i % BRICK_COLS;
                float bx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap) + g->brick_w * 0.5f;
                float by = g->brick_y0 + rIdx * (g->brick_h + g->brick_gap) + g->brick_h * 0.5f;
                float dx = bx - ballx, dy = by - besty;
                float d = dx * dx * 1.6f + dy * dy * 0.35f;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (best >= 0) {
                int cIdx = best % BRICK_COLS;
                float tx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap) + g->brick_w * 0.5f;
                rel = Clampf((tx - ballx) / (g->width * 0.5f), -0.7f, 0.7f);
            }
        }
        float want_center = ballx - rel * halfw;
        float center = g->paddle_x + halfw;
        axis = Clampf((want_center - center) / (halfw * 0.7f), -1.0f, 1.0f);
        if (g->state == GAME_READY || g->state == GAME_GAMEOVER || g->state == GAME_WIN)
            autoLaunch = 1;
    }
    axis = Clampf(axis, -1.0f, 1.0f);

    /* effect timers */
    if (!g->paused && g->state != GAME_GAMEOVER && g->state != GAME_WIN) {
        if (g->pw_wide_t   > 0.0f) g->pw_wide_t   -= dt;
        if (g->pw_slow_t   > 0.0f) g->pw_slow_t   -= dt;
        if (g->pw_laser_t  > 0.0f) g->pw_laser_t  -= dt;
        if (g->pw_sticky_t > 0.0f) g->pw_sticky_t -= dt;
        if (g->pw_shrink_t > 0.0f) g->pw_shrink_t -= dt;
        if (g->pw_fast_t   > 0.0f) g->pw_fast_t   -= dt;
        if (g->shot_cd     > 0.0f) g->shot_cd     -= dt;
    }

    /* effective paddle width (wide/shrink), preserving the paddle centre */
    {
        float baseW = g->paddle_w;
        float effW = baseW;
        if (g->pw_wide_t   > 0.0f) effW *= 1.7f;
        if (g->pw_shrink_t > 0.0f) effW *= 0.6f;
        float maxW = (g->field_x1 - g->field_x0) * 0.6f;
        if (effW > maxW) effW = maxW;
        if (effW < baseW * 0.5f) effW = baseW * 0.5f;
        if (effW != g->paddle_w_eff) {
            float cxp = g->paddle_x + g->paddle_w_eff * 0.5f;
            g->paddle_w_eff = effW;
            g->paddle_x = cxp - effW * 0.5f;
        }
    }

    if (!g->paused && g->state != GAME_GAMEOVER && g->state != GAME_WIN) {
        g->paddle_x += axis * g->paddle_speed * dt;
        g->paddle_x = Clampf(g->paddle_x, g->field_x0, g->field_x1 - g->paddle_w_eff);
    }

    float speedMul = 1.0f;
    if (g->pw_slow_t > 0.0f) speedMul = 0.62f;
    else if (g->pw_fast_t > 0.0f) speedMul = 1.5f;

    switch (g->state) {
    case GAME_READY:
        for (int i = 0; i < MAX_BALLS; ++i)
            if (g->balls[i].active && g->balls[i].stuck) {
                g->balls[i].x = g->paddle_x + g->paddle_w_eff * 0.5f;
                g->balls[i].y = g->paddle_y - g->balls[i].r - 1.0f;
            }
        if (launch || autoLaunch) {
            for (int i = 0; i < MAX_BALLS; ++i) {
                Ball *b = &g->balls[i];
                if (!b->active) continue;
                b->stuck = 0;
                float speed = g->ball_speed;
                b->vx = speed * 0.45f;
                float vy2 = speed * speed - b->vx * b->vx;
                b->vy = -(vy2 > 0.0f ? sqrtf(vy2) : speed * 0.8f);
            }
            g->state = GAME_PLAYING;
        }
        break;

    case GAME_PLAYING: {
        if (g->paused) break;

        /* stuck balls ride the paddle; launch releases them all */
        int anyStuck = 0;
        for (int i = 0; i < MAX_BALLS; ++i) {
            Ball *b = &g->balls[i];
            if (b->active && b->stuck) {
                anyStuck = 1;
                b->x = g->paddle_x + g->paddle_w_eff * 0.5f;
                b->y = g->paddle_y - b->r - 1.0f;
            }
        }
        if (anyStuck && (launch || autoLaunch)) {
            for (int i = 0; i < MAX_BALLS; ++i) {
                Ball *b = &g->balls[i];
                if (!b->active || !b->stuck) continue;
                b->stuck = 0;
                float speed = g->ball_speed;
                b->vx = speed * (RandUnit(g) < 0.5f ? -0.45f : 0.45f);
                float vy2 = speed * speed - b->vx * b->vx;
                b->vy = -(vy2 > 0.0f ? sqrtf(vy2) : speed * 0.8f);
            }
        }

        /* laser */
        if ((g->pw_laser_t > 0.0f) && g->shot_cd <= 0.0f && (btn & PAD_A))
            FireShots(g);

        /* move balls (substepped so fast balls can't tunnel) */
        for (int i = 0; i < MAX_BALLS; ++i) {
            Ball *b = &g->balls[i];
            if (!b->active || b->stuck) continue;
            float speed = BallSpeed(b) * speedMul;
            float max_step = b->r * 0.5f;
            if (max_step < 1.0f) max_step = 1.0f;
            int steps = (int)(speed * dt / max_step) + 1;
            if (steps > 16) steps = 16;
            float sdt = dt / (float)steps;
            for (int s = 0; s < steps; ++s) StepBall(g, b, sdt, speedMul);
            if (b->y - b->r > g->field_y1) b->active = 0;
        }

        /* recount balls */
        g->ball_count = 0;
        for (int i = 0; i < MAX_BALLS; ++i) if (g->balls[i].active) g->ball_count++;

        /* falling power-ups */
        for (int i = 0; i < MAX_POWERS; ++i) {
            Power *p = &g->powers[i];
            if (!p->active) continue;
            p->y += p->vy * dt;
            float half = g->height * 0.014f;
            if (p->y + half >= g->paddle_y && p->y - half <= g->paddle_y + g->paddle_h &&
                p->x >= g->paddle_x - half && p->x <= g->paddle_x + g->paddle_w_eff + half) {
                ApplyPower(g, p->kind);
                p->active = 0;
            } else if (p->y - half > g->field_y1) {
                p->active = 0;
            }
        }

        /* laser shots */
        float shot_speed = g->height * 1.4f;
        for (int i = 0; i < MAX_SHOTS; ++i) {
            Shot *s = &g->shots[i];
            if (!s->active) continue;
            s->y -= shot_speed * dt;
            if (s->y < g->field_y0) { s->active = 0; continue; }
            for (int j = 0; j < BRICK_ROWS * BRICK_COLS; ++j) {
                Brick *br = &g->bricks[j];
                if (!br->alive) continue;
                int rIdx = j / BRICK_COLS, cIdx = j % BRICK_COLS;
                float bx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap);
                float by = g->brick_y0 + rIdx * (g->brick_h + g->brick_gap);
                if (s->x >= bx && s->x <= bx + g->brick_w &&
                    s->y >= by && s->y <= by + g->brick_h) {
                    s->active = 0;
                    if (br->kind != BRICK_SOLID) {
                        br->hp--;
                        if (br->hp <= 0) BreakBrick(g, j, 0);
                        else SpawnBurst(g, s->x, s->y, COL_DIM);
                    }
                    break;
                }
            }
        }

        /* life loss only when every ball is gone */
        if (g->ball_count == 0) {
            g->lives--;
            if (g->lives <= 0) {
                g->state = GAME_GAMEOVER;
                g->state_timer = 0.0f;
            } else {
                AttachBallToPaddle(g);
                g->state = GAME_READY;
            }
        }

        if (g->state == GAME_PLAYING && g->bricks_left <= 0) {
            g->state = GAME_LEVELCLEAR;
            g->state_timer = 0.0f;
        }
        break;
    }

    case GAME_LEVELCLEAR:
        g->state_timer += dt;
        if (launch || autoLaunch || g->state_timer > 2.4f) {
            if (g->level >= LEVEL_COUNT) {
                g->state = GAME_WIN;
                g->state_timer = 0.0f;
            } else {
                g->level++;
                LoadLevel(g);
            }
        }
        break;

    case GAME_GAMEOVER:
        g->state_timer += dt;
        if (launch || (autoLaunch && g->state_timer > 3.0f)) Game_Reset(g);
        break;

    case GAME_WIN:
        g->state_timer += dt;
        if (launch || (autoLaunch && g->state_timer > 3.0f)) Game_Reset(g);
        break;
    }

    /* particles always settle, even between states */
    if (!g->paused) {
        float grav = g->height * 1.7f;
        for (int i = 0; i < MAX_PARTICLES; ++i) {
            Particle *p = &g->particles[i];
            if (!p->active) continue;
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            p->vy += grav * dt;
            p->life -= dt;
            if (p->life <= 0.0f || p->x < -4.0f || p->x > (float)g->width + 4.0f ||
                p->y > (float)g->height + 4.0f) p->active = 0;
        }
    }
}

/* ======================================================================
 * Drawing
 * ====================================================================== */
static void DrawBrick(Game *g, GfxContext *gfx, int i) {
    Brick *b = &g->bricks[i];
    int rIdx = i / BRICK_COLS, cIdx = i % BRICK_COLS;
    int x = (int)(g->brick_x0 + cIdx * (g->brick_w + g->brick_gap));
    int y = (int)(g->brick_y0 + rIdx * (g->brick_h + g->brick_gap));
    int w = (int)g->brick_w, h = (int)g->brick_h;

    uint32_t base = BrickBaseColor(rIdx, b->kind);
    if (b->maxhp > 1 && b->hp < b->maxhp) base = Darken(base, 0x38);

    Gfx_FillRect(gfx, x, y, w, h, base);
    Gfx_FillRect(gfx, x, y, w, 2, Lighten(base, 0x40));
    Gfx_FillRect(gfx, x, y + h - 2, w, 2, Darken(base, 0x40));

    if (b->kind == BRICK_SOLID) {
        Gfx_FillRect(gfx, x + w / 2 - 1, y + 2, 2, h - 4, Darken(base, 0x20));
        Gfx_FillRect(gfx, x + 2, y + h / 2 - 1, w - 4, 2, Darken(base, 0x20));
    } else if (b->kind == BRICK_BOMB) {
        Gfx_FillRect(gfx, x + w / 2 - 2, y + h / 2 - 2, 4, 4, 0xFF301010u);
    } else if (b->power) {
        Gfx_FillRect(gfx, x + w / 2 - 2, y + h / 2 - 2, 4, 4, COL_ACCENT);
    }
}

static void DrawBall(GfxContext *gfx, Ball *b) {
    int cx = (int)b->x, cy = (int)b->y, r = (int)b->r;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
            if (x * x + y * y <= r * r)
                Gfx_FillRect(gfx, cx + x, cy + y, 1, 1, COL_BALL);
    if (r >= 3)
        Gfx_FillRect(gfx, cx - r / 2, cy - r / 2, 2, 2, 0xFFD0F0FFu);
}

static void DrawPower(Game *g, GfxContext *gfx, Power *p) {
    int s = (int)(g->height * 0.028f);
    if (s < 8) s = 8;
    int x = (int)(p->x - s * 0.5f);
    int y = (int)(p->y - s * 0.5f);
    uint32_t c = PowerColor(p->kind);
    Gfx_FillRect(gfx, x, y, s, s, c);
    Gfx_FillRect(gfx, x, y, s, 2, Lighten(c, 0x40));
    Gfx_FillRect(gfx, x, y + s - 2, s, 2, Darken(c, 0x40));
    char letter[2];
    letter[0] = PowerLetter(p->kind);
    letter[1] = 0;
    Gfx_DrawText(gfx, x + s / 2 - 3, y + s / 2 - 3, 1, 0xFF000000u, letter);
}

static void DrawStars(Game *g, GfxContext *gfx) {
    uint32_t W = g->width, H = g->height;
    for (int i = 0; i < 64; ++i) {
        uint32_t hv = (uint32_t)i * 2654435761u + 0x9E3779B9u;
        hv ^= hv >> 15;
        uint32_t x = ((hv & 0xFFFFu) * W) >> 16;
        uint32_t y = (((hv >> 16) & 0xFFFFu) * H) >> 16;
        uint32_t tone = (hv >> 8) & 3u;
        uint32_t c = (tone == 0) ? 0xFF2A2A38u
                   : (tone == 1) ? 0xFF454556u
                                 : 0xFF606076u;
        int sz = (tone == 3) ? 2 : 1;
        Gfx_FillRect(gfx, (int)x, (int)y, sz, sz, c);
    }
}

static void DrawHUD(Game *g, GfxContext *gfx) {
    int ts = g->text_scale;
    char buf[64];
    int n;

    /* SCORE */
    n = 0;
    n = AppendStr(buf, n, sizeof(buf), "SCORE ");
    n = AppendInt(buf, n, sizeof(buf), g->score);
    Gfx_DrawText(gfx, (int)g->field_x0, 4, ts, COL_TEXT, buf);

    /* LEVEL */
    n = 0;
    n = AppendStr(buf, n, sizeof(buf), "LEVEL ");
    n = AppendInt(buf, n, sizeof(buf), g->level);
    n = AppendStr(buf, n, sizeof(buf), "/");
    n = AppendInt(buf, n, sizeof(buf), LEVEL_COUNT);
    int lw = TextW(buf, ts);
    Gfx_DrawText(gfx, (int)g->field_x1 - lw, 4, ts, COL_ACCENT, buf);

    /* LIVES as little paddles (or a count if many) */
    {
        int maxicons = 6;
        int iw = 4 * ts, ih = 2 * ts, gap = 3;
        int cx = (int)(g->width * 0.5f);
        if (g->lives <= maxicons) {
            int total = g->lives * (iw + gap);
            int sx = cx - total / 2;
            for (int i = 0; i < g->lives; ++i)
                Gfx_FillRect(gfx, sx + i * (iw + gap), 4, iw, ih, COL_PADDLE);
        } else {
            n = 0;
            n = AppendStr(buf, n, sizeof(buf), "LIVES x");
            n = AppendInt(buf, n, sizeof(buf), g->lives);
            int w = TextW(buf, ts);
            Gfx_DrawText(gfx, cx - w / 2, 4, ts, COL_PADDLE, buf);
        }
    }

    /* level name + combo (second row) */
    {
        int y2 = 4 + 9 * ts;
        int w = TextW(g->level_name, ts);
        Gfx_DrawText(gfx, (int)(g->width * 0.5f) - w / 2, y2, ts, COL_DIM, g->level_name);

        if (g->combo > 1) {
            n = 0;
            n = AppendStr(buf, n, sizeof(buf), "COMBO x");
            n = AppendInt(buf, n, sizeof(buf), g->combo > 10 ? 10 : g->combo);
            int cw = TextW(buf, ts);
            Gfx_DrawText(gfx, (int)(g->field_x1) - cw, y2, ts, COL_ACCENT, buf);
        }
    }

    /* active power-up icons, bottom strip */
    {
        int icon = 6 * ts;
        int gap = 6;
        int count = 0;
        int kinds[6];
        if (g->pw_wide_t   > 0.0f) kinds[count++] = PW_WIDE;
        if (g->pw_slow_t   > 0.0f) kinds[count++] = PW_SLOW;
        if (g->pw_laser_t  > 0.0f) kinds[count++] = PW_LASER;
        if (g->pw_sticky_t > 0.0f) kinds[count++] = PW_STICKY;
        if (g->pw_shrink_t > 0.0f) kinds[count++] = PW_SHRINK;
        if (g->pw_fast_t   > 0.0f) kinds[count++] = PW_FAST;
        int total = count * (icon + gap);
        int sx = (int)(g->width * 0.5f) - total / 2;
        for (int i = 0; i < count; ++i) {
            int x = sx + i * (icon + gap);
            uint32_t c = PowerColor(kinds[i]);
            Gfx_FillRect(gfx, x, (int)g->field_y1 + 4, icon, icon, c);
            char letter[2];
            letter[0] = PowerLetter(kinds[i]);
            letter[1] = 0;
            Gfx_DrawText(gfx, x + icon / 2 - 3, (int)g->field_y1 + 4 + icon / 2 - 3,
                         1, 0xFF000000u, letter);
        }
    }

    /* FPS counter, top-right of the play area */
    if (g->fps > 0.0f) {
        n = 0;
        n = AppendStr(buf, n, sizeof(buf), "FPS ");
        n = AppendInt(buf, n, sizeof(buf), (int)(g->fps + 0.5f));
        int w = TextW(buf, ts);
        Gfx_DrawText(gfx, (int)g->width - (int)(g->width * 0.02f) - w,
                     (int)(g->field_y0 * 0.25f), ts, COL_ACCENT, buf);
    }
}

void Game_Draw(Game *g, GfxContext *gfx) {
    Gfx_Clear(gfx, COL_BG);
    DrawStars(g, gfx);

    /* play-field border */
    int bx0 = (int)g->field_x0, bx1 = (int)g->field_x1;
    int by0 = (int)g->field_y0, by1 = (int)g->field_y1;
    Gfx_FillRect(gfx, bx0 - 2, by0 - 2, (bx1 - bx0) + 4, 3, COL_BORDER);
    Gfx_FillRect(gfx, bx0 - 2, by1, (bx1 - bx0) + 4, 3, COL_BORDER);
    Gfx_FillRect(gfx, bx0 - 2, by0, 3, (by1 - by0), COL_BORDER);
    Gfx_FillRect(gfx, bx1, by0, 3, (by1 - by0), COL_BORDER);

    for (int i = 0; i < BRICK_ROWS * BRICK_COLS; ++i)
        if (g->bricks[i].alive) DrawBrick(g, gfx, i);

    for (int i = 0; i < MAX_POWERS; ++i)
        if (g->powers[i].active) DrawPower(g, gfx, &g->powers[i]);

    /* shots (laser bolts) */
    for (int i = 0; i < MAX_SHOTS; ++i) {
        Shot *s = &g->shots[i];
        if (!s->active) continue;
        Gfx_FillRect(gfx, (int)s->x - 1, (int)s->y - 6, 3, 7, COL_BAD);
        Gfx_FillRect(gfx, (int)s->x - 1, (int)s->y - 6, 3, 2, 0xFFFFF0A0u);
    }

    /* paddle (with a highlight stripe) */
    Gfx_FillRect(gfx, (int)g->paddle_x, (int)g->paddle_y,
                 (int)g->paddle_w_eff, (int)g->paddle_h, COL_PADDLE);
    Gfx_FillRect(gfx, (int)g->paddle_x, (int)g->paddle_y,
                 (int)g->paddle_w_eff, 2, COL_PADDLE2);

    for (int i = 0; i < MAX_BALLS; ++i)
        if (g->balls[i].active) DrawBall(gfx, &g->balls[i]);

    /* particles */
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        Particle *p = &g->particles[i];
        if (!p->active) continue;
        if (p->x < 0.0f || p->x > (float)g->width - 1.0f ||
            p->y < 0.0f || p->y > (float)g->height - 1.0f) continue;
        float f = p->life / p->maxlife;
        int sz = (f > 0.5f) ? 3 : (f > 0.25f ? 2 : 1);
        Gfx_FillRect(gfx, (int)p->x, (int)p->y, sz, sz, p->color);
    }

    DrawHUD(g, gfx);

    /* overlays */
    int cx = (int)(g->width * 0.5f);
    int cy = (int)(g->height * 0.5f);
    int ts = g->text_scale;

    if (g->state == GAME_READY) {
        const char *msg = "PRESS A TO LAUNCH";
        Gfx_DrawText(gfx, cx - TextW(msg, ts) / 2, cy, ts, COL_DIM, msg);
    } else if (g->state == GAME_LEVELCLEAR) {
        const char *msg = "LEVEL CLEARED!";
        Gfx_DrawText(gfx, cx - TextW(msg, ts) / 2, cy, ts, COL_ACCENT, msg);
    } else if (g->state == GAME_GAMEOVER) {
        const char *m1 = "GAME OVER";
        const char *m2 = "PRESS A TO RESTART";
        char buf[48];
        int n = 0;
        n = AppendStr(buf, n, sizeof(buf), "BEST COMBO x");
        n = AppendInt(buf, n, sizeof(buf), g->best_combo);
        Gfx_DrawText(gfx, cx - TextW(m1, ts) / 2, cy - 6 * ts, ts, COL_ACCENT, m1);
        Gfx_DrawText(gfx, cx - TextW(buf, ts) / 2, cy + 3 * ts, ts, COL_TEXT, buf);
        Gfx_DrawText(gfx, cx - TextW(m2, ts) / 2, cy + 12 * ts, ts, COL_DIM, m2);
    } else if (g->state == GAME_WIN) {
        const char *m1 = "YOU WIN!";
        const char *m2 = "ALL LEVELS CLEARED";
        const char *m3 = "PRESS A TO PLAY AGAIN";
        Gfx_DrawText(gfx, cx - TextW(m1, ts) / 2, cy - 12 * ts, ts, COL_GOOD, m1);
        Gfx_DrawText(gfx, cx - TextW(m2, ts) / 2, cy - 3 * ts, ts, COL_ACCENT, m2);
        Gfx_DrawText(gfx, cx - TextW(m3, ts) / 2, cy + 6 * ts, ts, COL_TEXT, m3);
    }

    if (g->paused) {
        const char *msg = "PAUSED";
        Gfx_DrawText(gfx, cx - TextW(msg, ts) / 2, cy - 14 * ts, ts, COL_TEXT, msg);
    }
}
