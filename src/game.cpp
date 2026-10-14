/* game.cpp: "Aurora Bricks" - Breakout-style 2D game logic and drawing. */
#include "game.h"

#include <math.h>
#include <string.h>

/* 0xAARRGGBB */
#define COL_BG       0xFF101018u
#define COL_BORDER   0xFF3A3A50u
#define COL_PADDLE   0xFF3FB6FFu
#define COL_BALL     0xFFFFFFFFu
#define COL_TEXT     0xFFF0F0F0u
#define COL_DIM      0xFF9098A0u
#define COL_ACCENT   0xFFFFD040u

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

static void Layout(Game *g) {
    float W = (float)g->width;
    float H = (float)g->height;

    g->field_x0 = W * 0.04f;
    g->field_x1 = W - W * 0.04f;
    g->field_y0 = H * 0.06f;
    g->field_y1 = H - H * 0.03f;

    g->paddle_w = W * 0.16f;
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
    g->brick_y0 = g->field_y0 + H * 0.05f;

    g->text_scale = (int)(W / 640.0f);
    if (g->text_scale < 2) g->text_scale = 2;
}

static void BallAttach(Game *g) {
    g->ball_attached = 1;
    g->ball_vx = 0.0f;
    g->ball_vy = 0.0f;
    g->ball_x = g->paddle_x + g->paddle_w * 0.5f;
    g->ball_y = g->paddle_y - g->ball_r - 2.0f;
}

static void LoadLevel(Game *g) {
    g->bricks_left = 0;
    for (int r = 0; r < BRICK_ROWS; ++r) {
        for (int c = 0; c < BRICK_COLS; ++c) {
            Brick *b = &g->bricks[r * BRICK_COLS + c];
            b->alive = 1;
            b->hp = (r < 2) ? 2 : 1; /* tougher top rows */
            g->bricks_left++;
        }
    }
    g->paddle_x = (g->field_x0 + g->field_x1) * 0.5f - g->paddle_w * 0.5f;
    BallAttach(g);
    g->ball_speed = (float)g->width * (0.55f + 0.03f * (g->level - 1));
    g->state = GAME_READY;
    g->state_timer = 0.0f;
    g->paused = 0;
}

void Game_Init(Game *g, uint32_t width, uint32_t height) {
    memset(g, 0, sizeof(*g));
    g->width = width;
    g->height = height;
    Layout(g);
    g->score = 0;
    g->lives = 3;
    g->level = 1;
    LoadLevel(g);
}

void Game_Reset(Game *g) {
    g->score = 0;
    g->lives = 3;
    g->level = 1;
    LoadLevel(g);
}

static void StepBall(Game *g, float dt) {
    g->ball_x += g->ball_vx * dt;
    g->ball_y += g->ball_vy * dt;

    float r = g->ball_r;

    /* side walls */
    if (g->ball_x - r < g->field_x0) { g->ball_x = g->field_x0 + r; g->ball_vx = -g->ball_vx; }
    if (g->ball_x + r > g->field_x1) { g->ball_x = g->field_x1 - r; g->ball_vx = -g->ball_vx; }
    if (g->ball_y - r < g->field_y0) { g->ball_y = g->field_y0 + r; g->ball_vy = -g->ball_vy; }

    /* paddle */
    if (g->ball_vy > 0.0f && g->ball_y + r >= g->paddle_y &&
        g->ball_y - r <= g->paddle_y + g->paddle_h &&
        g->ball_x + r >= g->paddle_x &&
        g->ball_x - r <= g->paddle_x + g->paddle_w) {
        g->ball_y = g->paddle_y - r;
        float center = g->paddle_x + g->paddle_w * 0.5f;
        float rel = Clampf((g->ball_x - center) / (g->paddle_w * 0.5f), -1.0f, 1.0f);
        float speed = g->ball_speed;
        g->ball_vx = rel * speed * 0.85f;
        float vy2 = speed * speed - g->ball_vx * g->ball_vx;
        g->ball_vy = -(vy2 > 0.0f ? sqrtf(vy2) : speed * 0.5f);
    }

    /* bricks */
    for (int i = 0; i < BRICK_ROWS * BRICK_COLS; ++i) {
        Brick *b = &g->bricks[i];
        if (!b->alive) continue;
        int rIdx = i / BRICK_COLS;
        int cIdx = i % BRICK_COLS;
        float bx = g->brick_x0 + cIdx * (g->brick_w + g->brick_gap);
        float by = g->brick_y0 + rIdx * (g->brick_h + g->brick_gap);
        float bw = g->brick_w;
        float bh = g->brick_h;

        /* circle-vs-AABB (nearest point) */
        float nx = Clampf(g->ball_x, bx, bx + bw);
        float ny = Clampf(g->ball_y, by, by + bh);
        float dx = g->ball_x - nx;
        float dy = g->ball_y - ny;
        if (dx * dx + dy * dy <= r * r) {
            /* resolve along the shallower penetration axis */
            float overlap_x = (g->ball_x < bx + bw * 0.5f)
                              ? (g->ball_x + r) - bx : (bx + bw) - (g->ball_x - r);
            float overlap_y = (g->ball_y < by + bh * 0.5f)
                              ? (g->ball_y + r) - by : (by + bh) - (g->ball_y - r);
            if (overlap_x < overlap_y) g->ball_vx = -g->ball_vx;
            else                       g->ball_vy = -g->ball_vy;

            b->hp--;
            if (b->hp <= 0) {
                b->alive = 0;
                g->bricks_left--;
                g->score += 10 * g->level;
            }
            break; /* one brick per substep */
        }
    }
}

void Game_Update(Game *g, const PadState *pad, float dt) {
    if (dt > 0.1f) dt = 0.1f; /* clamp after hitches */

    int connected = (pad && pad->connected);
    uint16_t btn = pad ? pad->buttons : 0;
    uint16_t prs = pad ? pad->pressed : 0;

    if (prs & PAD_START) g->paused = !g->paused;

    /* control axis: real stick/dpad, or an autopilot demo when no pad is
     * connected (so the title still shows gameplay with no controller). */
    float axis = pad ? pad->lx : 0.0f;
    if (btn & PAD_LEFT)  axis -= 1.0f;
    if (btn & PAD_RIGHT) axis += 1.0f;
    int autoLaunch = 0;
    if (!connected) {
        float target = (g->state == GAME_PLAYING && !g->ball_attached)
                       ? g->ball_x
                       : (g->field_x0 + g->field_x1) * 0.5f;
        float center = g->paddle_x + g->paddle_w * 0.5f;
        axis = Clampf((target - center) / (g->paddle_w * 0.7f), -1.0f, 1.0f);
        if (g->state == GAME_READY || g->state == GAME_GAMEOVER) autoLaunch = 1;
    }
    axis = Clampf(axis, -1.0f, 1.0f);

    if (!g->paused && g->state != GAME_GAMEOVER) {
        g->paddle_x += axis * g->paddle_speed * dt;
        g->paddle_x = Clampf(g->paddle_x, g->field_x0, g->field_x1 - g->paddle_w);
    }

    switch (g->state) {
    case GAME_READY:
        BallAttach(g);
        if ((prs & PAD_A) || autoLaunch) {
            g->ball_attached = 0;
            float speed = g->ball_speed;
            g->ball_vx = speed * 0.45f;
            float vy2 = speed * speed - g->ball_vx * g->ball_vx;
            g->ball_vy = -(vy2 > 0.0f ? sqrtf(vy2) : speed * 0.8f);
            g->state = GAME_PLAYING;
        }
        break;

    case GAME_PLAYING: {
        if (g->paused) break;
        if (g->ball_attached) { BallAttach(g); break; }
        /* substep so fast balls cannot tunnel through bricks */
        float speed = sqrtf(g->ball_vx * g->ball_vx + g->ball_vy * g->ball_vy);
        float max_step = g->ball_r * 0.5f;
        if (max_step < 1.0f) max_step = 1.0f;
        int steps = (int)(speed * dt / max_step) + 1;
        if (steps > 16) steps = 16;
        float sdt = dt / (float)steps;
        for (int s = 0; s < steps; ++s) {
            if (g->ball_vy > 0.0f && g->ball_y - g->ball_r > g->field_y1) break;
            StepBall(g, sdt);
        }
        if (g->ball_y - g->ball_r > g->field_y1) {
            g->lives--;
            if (g->lives <= 0) {
                g->state = GAME_GAMEOVER;
                g->state_timer = 0.0f;
            } else {
                g->state = GAME_READY;
            }
        }
        if (g->bricks_left <= 0) {
            g->state = GAME_LEVELCLEAR;
            g->state_timer = 0.0f;
        }
        break;
    }

    case GAME_LEVELCLEAR:
        g->state_timer += dt;
        if (prs & PAD_A || g->state_timer > 2.0f) {
            g->level++;
            LoadLevel(g);
        }
        break;

    case GAME_GAMEOVER:
        g->state_timer += dt;
        if ((prs & PAD_A) || (autoLaunch && g->state_timer > 3.0f)) Game_Reset(g);
        break;
    }
}

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

static void DrawBrickColor(Game *g, GfxContext *gfx, int i, uint32_t base) {
    int rIdx = i / BRICK_COLS;
    int cIdx = i % BRICK_COLS;
    int x = (int)(g->brick_x0 + cIdx * (g->brick_w + g->brick_gap));
    int y = (int)(g->brick_y0 + rIdx * (g->brick_h + g->brick_gap));
    int w = (int)g->brick_w;
    int h = (int)g->brick_h;

    Gfx_FillRect(gfx, x, y, w, h, base);
    /* simple bevel: lighter top edge, darker bottom edge */
    Gfx_FillRect(gfx, x, y, w, 2, Lighten(base, 0x40));
    Gfx_FillRect(gfx, x, y + h - 2, w, 2, Darken(base, 0x40));
}

void Game_Draw(Game *g, GfxContext *gfx) {
    Gfx_Clear(gfx, COL_BG);

    /* play-field border */
    int bx0 = (int)g->field_x0, bx1 = (int)g->field_x1;
    int by0 = (int)g->field_y0, by1 = (int)g->field_y1;
    Gfx_FillRect(gfx, bx0 - 2, by0 - 2, (bx1 - bx0) + 4, 3, COL_BORDER);
    Gfx_FillRect(gfx, bx0 - 2, by1, (bx1 - bx0) + 4, 3, COL_BORDER);
    Gfx_FillRect(gfx, bx0 - 2, by0, 3, (by1 - by0), COL_BORDER);
    Gfx_FillRect(gfx, bx1, by0, 3, (by1 - by0), COL_BORDER);

    /* bricks */
    for (int i = 0; i < BRICK_ROWS * BRICK_COLS; ++i) {
        if (!g->bricks[i].alive) continue;
        DrawBrickColor(g, gfx, i, BRICK_COLORS[i / BRICK_COLS]);
    }

    /* paddle */
    Gfx_FillRect(gfx, (int)g->paddle_x, (int)g->paddle_y,
                 (int)g->paddle_w, (int)g->paddle_h, COL_PADDLE);

    /* ball */
    {
        int cx = (int)g->ball_x, cy = (int)g->ball_y, r = (int)g->ball_r;
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (x * x + y * y <= r * r)
                    Gfx_FillRect(gfx, cx + x, cy + y, 1, 1, COL_BALL);
    }

    /* HUD */
    {
        char buf[64];
        int n = 0;
        n = AppendStr(buf, n, sizeof(buf), "SCORE ");
        n = AppendInt(buf, n, sizeof(buf), g->score);
        Gfx_DrawText(gfx, (int)g->field_x0, (int)(g->field_y0 * 0.25f),
                     g->text_scale, COL_TEXT, buf);

        n = 0;
        n = AppendStr(buf, n, sizeof(buf), "LIVES ");
        n = AppendInt(buf, n, sizeof(buf), g->lives);
        Gfx_DrawText(gfx, (int)(g->width * 0.42f), (int)(g->field_y0 * 0.25f),
                     g->text_scale, COL_TEXT, buf);

        n = 0;
        n = AppendStr(buf, n, sizeof(buf), "LEVEL ");
        n = AppendInt(buf, n, sizeof(buf), g->level);
        Gfx_DrawText(gfx, (int)(g->width * 0.72f), (int)(g->field_y0 * 0.25f),
                     g->text_scale, COL_ACCENT, buf);

        /* FPS counter, top-right corner (value supplied by the platform). */
        if (g->fps > 0.0f) {
            int ts = g->text_scale < 1 ? 1 : g->text_scale;
            n = 0;
            n = AppendStr(buf, n, sizeof(buf), "FPS ");
            n = AppendInt(buf, n, sizeof(buf), (int)(g->fps + 0.5f));
            int wtext = (int)strlen(buf) * (6 * ts);
            int margin = (int)(g->width * 0.02f);
            Gfx_DrawText(gfx, (int)g->width - margin - wtext,
                         (int)(g->field_y0 * 0.25f), ts, COL_ACCENT, buf);
        }
    }

    /* overlays */
    int cx = (int)(g->width * 0.5f);
    int cy = (int)(g->height * 0.5f);
    int ts = g->text_scale;

    if (g->state == GAME_READY) {
        const char *msg = "PRESS A TO LAUNCH";
        int w = (int)strlen(msg) * (6 * ts);
        Gfx_DrawText(gfx, cx - w / 2, cy, ts, COL_DIM, msg);
    } else if (g->state == GAME_LEVELCLEAR) {
        const char *msg = "LEVEL CLEARED";
        int w = (int)strlen(msg) * (6 * ts);
        Gfx_DrawText(gfx, cx - w / 2, cy, ts, COL_ACCENT, msg);
    } else if (g->state == GAME_GAMEOVER) {
        const char *m1 = "GAME OVER";
        const char *m2 = "PRESS A TO RESTART";
        int w1 = (int)strlen(m1) * (6 * ts);
        int w2 = (int)strlen(m2) * (6 * ts);
        Gfx_DrawText(gfx, cx - w1 / 2, cy - 5 * ts, ts, COL_ACCENT, m1);
        Gfx_DrawText(gfx, cx - w2 / 2, cy + 4 * ts, ts, COL_TEXT, m2);
    }

    if (g->paused) {
        const char *msg = "PAUSED";
        int w = (int)strlen(msg) * (6 * ts);
        Gfx_DrawText(gfx, cx - w / 2, cy - 14 * ts, ts, COL_TEXT, msg);
    }
}
