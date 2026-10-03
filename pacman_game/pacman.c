/*
 * Pac-Man for Flipper Zero
 * Controls: D-pad = move, OK = pause (or restart after game over), Back = exit
 *
 * Screen layout: 32x14 tile maze (4px tiles = 128x56) + 8px HUD row.
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <string.h>

#define MAZE_W 32
#define MAZE_H 14
#define HALF_W 16
#define TILE 4
#define NUM_GHOSTS 4
#define START_LIVES 3
#define PAC_MS 125

/* Left half of the maze (16 chars each); mirrored at load time.
 * '#' wall, '.' dot, 'o' power pellet, ' ' empty */
static const char* const MAZE_HALF[MAZE_H] = {
    "################",
    "#o..............",
    "#.###.####.####.",
    "#.###.####.####.",
    "#...............",
    "#.###.#.####### ", /* gap = ghost house door */
    " .....#.#       ", /* col 0 open = warp tunnel */
    "#.###.#.#       ",
    "#.###.#.########",
    "#...............",
    "#.###.####.####.",
    "#.###.####.####.",
    "#o..............",
    "################",
};

typedef enum {
    StatusReady,
    StatusPlaying,
    StatusDying,
    StatusLevelClear,
    StatusGameOver,
} Status;

typedef enum {
    GhostInHouse,
    GhostActive,
} GhostState;

typedef struct {
    int8_t x, y, dx, dy, want_dx, want_dy;
    bool mouth;
} Pac;

typedef struct {
    int8_t x, y, dx, dy;
    GhostState state;
    bool scared;
    uint32_t release_at;
} Ghost;

typedef struct {
    char tiles[MAZE_H][MAZE_W];
    Pac pac;
    Ghost ghosts[NUM_GHOSTS];
    Status status;
    bool paused;
    bool scatter;
    uint32_t score;
    uint32_t high_score;
    uint8_t lives;
    uint8_t level;
    uint16_t dots_left;
    uint8_t combo;
    uint32_t clock; /* game clock in ms, only advances when not paused */
    uint32_t state_until;
    uint32_t next_pac;
    uint32_t next_ghost;
    uint32_t scared_until;
    uint32_t phase_start;
} Game;

typedef struct {
    Game game;
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
} App;

static const int8_t DX[4] = {0, -1, 0, 1};
static const int8_t DY[4] = {-1, 0, 1, 0};
static const int8_t CORNER_X[NUM_GHOSTS] = {30, 1, 30, 1};
static const int8_t CORNER_Y[NUM_GHOSTS] = {1, 1, 12, 12};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int wrap_x(int x) {
    if(x < 0) return MAZE_W - 1;
    if(x >= MAZE_W) return 0;
    return x;
}

static bool in_house(int x, int y) {
    return y >= 5 && y <= 7 && x >= 9 && x <= 22;
}

/* Walkable for Pac-Man and for roaming ghosts (house is off limits). */
static bool tile_open(Game* g, int x, int y) {
    if(y < 0 || y >= MAZE_H) return false;
    x = wrap_x(x);
    if(g->tiles[y][x] == '#') return false;
    if(in_house(x, y)) return false;
    return true;
}

static uint32_t ghost_ms(Game* g, bool scared) {
    if(scared) return 220;
    int ms = 165 - (int)g->level * 8;
    if(ms < 105) ms = 105;
    return (uint32_t)ms;
}

/* ------------------------------------------------------------------ */
/* Game setup                                                          */
/* ------------------------------------------------------------------ */

static void load_maze(Game* g) {
    for(int y = 0; y < MAZE_H; y++) {
        int len = (int)strlen(MAZE_HALF[y]);
        for(int x = 0; x < HALF_W; x++) {
            char c = (x < len) ? MAZE_HALF[y][x] : '#';
            g->tiles[y][x] = c;
            g->tiles[y][MAZE_W - 1 - x] = c;
        }
    }
    g->tiles[9][15] = ' '; /* Pac-Man start tile */
    g->dots_left = 0;
    for(int y = 0; y < MAZE_H; y++)
        for(int x = 0; x < MAZE_W; x++)
            if(g->tiles[y][x] == '.' || g->tiles[y][x] == 'o') g->dots_left++;
}

static void reset_positions(Game* g) {
    static const int8_t sx[NUM_GHOSTS] = {14, 17, 13, 18};

    g->pac.x = 15;
    g->pac.y = 9;
    g->pac.dx = -1;
    g->pac.dy = 0;
    g->pac.want_dx = -1;
    g->pac.want_dy = 0;
    g->pac.mouth = false;

    for(int i = 0; i < NUM_GHOSTS; i++) {
        Ghost* gh = &g->ghosts[i];
        gh->x = sx[i];
        gh->y = 7;
        gh->dx = 0;
        gh->dy = -1;
        gh->state = GhostInHouse;
        gh->scared = false;
        gh->release_at = g->clock + 800 + (uint32_t)i * 2500;
    }

    g->scared_until = 0;
    g->scatter = true;
    g->phase_start = g->clock;
    g->combo = 0;
    g->status = StatusReady;
    g->state_until = g->clock + 1800;
}

static void new_game(Game* g) {
    g->score = 0;
    g->lives = START_LIVES;
    g->level = 1;
    g->paused = false;
    load_maze(g);
    reset_positions(g);
}

/* ------------------------------------------------------------------ */
/* Game logic                                                          */
/* ------------------------------------------------------------------ */

static void power_up(Game* g) {
    int dur = 7000 - (int)g->level * 600;
    if(dur < 2500) dur = 2500;
    g->scared_until = g->clock + (uint32_t)dur;
    g->combo = 0;
    for(int i = 0; i < NUM_GHOSTS; i++) {
        Ghost* gh = &g->ghosts[i];
        if(gh->state == GhostActive) {
            gh->scared = true;
            gh->dx = -gh->dx;
            gh->dy = -gh->dy;
        }
    }
}

static void pac_step(Game* g) {
    Pac* p = &g->pac;

    if((p->want_dx || p->want_dy) && tile_open(g, p->x + p->want_dx, p->y + p->want_dy)) {
        p->dx = p->want_dx;
        p->dy = p->want_dy;
    }

    if((p->dx || p->dy) && tile_open(g, p->x + p->dx, p->y + p->dy)) {
        p->x = (int8_t)wrap_x(p->x + p->dx);
        p->y = (int8_t)(p->y + p->dy);
        p->mouth = !p->mouth;

        char* t = &g->tiles[p->y][p->x];
        if(*t == '.') {
            g->score += 10;
            g->dots_left--;
            *t = ' ';
        } else if(*t == 'o') {
            g->score += 50;
            g->dots_left--;
            *t = ' ';
            power_up(g);
        }

        if(g->dots_left == 0) {
            g->status = StatusLevelClear;
            g->state_until = g->clock + 2200;
        }
    }
}

static void ghost_target(Game* g, int i, int* tx, int* ty) {
    Pac* p = &g->pac;

    if(g->scatter) {
        *tx = CORNER_X[i];
        *ty = CORNER_Y[i];
        return;
    }

    switch(i) {
    case 0: /* chaser */
        *tx = p->x;
        *ty = p->y;
        break;
    case 1: /* ambusher: aims ahead of Pac-Man */
        *tx = p->x + p->dx * 4;
        *ty = p->y + p->dy * 4;
        break;
    case 2: { /* flanker: mirrors the chaser around a point ahead of Pac-Man */
        Ghost* b = &g->ghosts[0];
        int ax = p->x + p->dx * 2;
        int ay = p->y + p->dy * 2;
        *tx = 2 * ax - b->x;
        *ty = 2 * ay - b->y;
        break;
    }
    default: { /* shy: chases when far, retreats to corner when close */
        int dx = g->ghosts[3].x - p->x;
        int dy = g->ghosts[3].y - p->y;
        if(dx * dx + dy * dy > 64) {
            *tx = p->x;
            *ty = p->y;
        } else {
            *tx = CORNER_X[3];
            *ty = CORNER_Y[3];
        }
        break;
    }
    }
}

static void ghost_step(Game* g, int i) {
    Ghost* gh = &g->ghosts[i];

    if(gh->state == GhostInHouse) {
        if(g->clock < gh->release_at) return;
        /* Scripted exit: slide to the door columns, then go up. */
        if(gh->y > 4) {
            if(gh->x < 15)
                gh->x++;
            else if(gh->x > 16)
                gh->x--;
            else
                gh->y--;
        }
        if(gh->y <= 4) {
            gh->state = GhostActive;
            gh->dx = (i & 1) ? 1 : -1;
            gh->dy = 0;
        }
        return;
    }

    int cand[4];
    int n = 0;
    bool moving = (gh->dx != 0 || gh->dy != 0);
    for(int d = 0; d < 4; d++) {
        if(moving && DX[d] == -gh->dx && DY[d] == -gh->dy) continue; /* no U-turns */
        if(!tile_open(g, gh->x + DX[d], gh->y + DY[d])) continue;
        cand[n++] = d;
    }

    int pick = -1;
    if(n == 0) {
        for(int d = 0; d < 4; d++)
            if(DX[d] == -gh->dx && DY[d] == -gh->dy) pick = d;
        if(pick < 0 || !tile_open(g, gh->x + DX[pick], gh->y + DY[pick])) return;
    } else if(gh->scared) {
        pick = cand[furi_hal_random_get() % (uint32_t)n];
    } else {
        int tx, ty;
        ghost_target(g, i, &tx, &ty);
        int best = 0x7fffffff;
        for(int k = 0; k < n; k++) {
            int d = cand[k];
            int ex = gh->x + DX[d] - tx;
            int ey = gh->y + DY[d] - ty;
            int dist = ex * ex + ey * ey;
            if(dist < best) {
                best = dist;
                pick = d;
            }
        }
    }

    gh->dx = DX[pick];
    gh->dy = DY[pick];
    gh->x = (int8_t)wrap_x(gh->x + gh->dx);
    gh->y = (int8_t)(gh->y + gh->dy);
}

static void check_collisions(Game* g) {
    for(int i = 0; i < NUM_GHOSTS; i++) {
        Ghost* gh = &g->ghosts[i];
        if(gh->state != GhostActive) continue;
        if(gh->x != g->pac.x || gh->y != g->pac.y) continue;

        if(gh->scared) {
            g->score += 200u << (g->combo > 3 ? 3 : g->combo);
            g->combo++;
            gh->scared = false;
            gh->state = GhostInHouse;
            gh->x = (i & 1) ? 17 : 14;
            gh->y = 7;
            gh->release_at = g->clock + 2000;
        } else {
            g->status = StatusDying;
            g->state_until = g->clock + 1400;
            return;
        }
    }
}

static void game_update(Game* g, uint32_t dt) {
    g->clock += dt;

    switch(g->status) {
    case StatusReady:
        if(g->clock >= g->state_until) {
            g->status = StatusPlaying;
            g->next_pac = g->clock;
            g->next_ghost = g->clock;
        }
        break;

    case StatusPlaying: {
        if(g->scared_until && g->clock >= g->scared_until) {
            g->scared_until = 0;
            g->combo = 0;
            for(int i = 0; i < NUM_GHOSTS; i++) g->ghosts[i].scared = false;
        }

        uint32_t phase_len = g->scatter ? 6000 : 18000;
        if(g->clock - g->phase_start >= phase_len) {
            g->scatter = !g->scatter;
            g->phase_start = g->clock;
            for(int i = 0; i < NUM_GHOSTS; i++) {
                Ghost* gh = &g->ghosts[i];
                if(gh->state == GhostActive && !gh->scared) {
                    gh->dx = -gh->dx;
                    gh->dy = -gh->dy;
                }
            }
        }

        if(g->clock >= g->next_pac) {
            g->next_pac = g->clock + PAC_MS;
            pac_step(g);
            if(g->status == StatusPlaying) check_collisions(g);
            if(g->status != StatusPlaying) break;
        }

        if(g->clock >= g->next_ghost) {
            bool any_scared = g->scared_until != 0;
            g->next_ghost = g->clock + ghost_ms(g, any_scared);
            for(int i = 0; i < NUM_GHOSTS; i++) ghost_step(g, i);
            check_collisions(g);
        }
        break;
    }

    case StatusDying:
        if(g->clock >= g->state_until) {
            if(g->lives > 0) g->lives--;
            if(g->lives == 0) {
                if(g->score > g->high_score) g->high_score = g->score;
                g->status = StatusGameOver;
            } else {
                reset_positions(g);
            }
        }
        break;

    case StatusLevelClear:
        if(g->clock >= g->state_until) {
            g->level++;
            load_maze(g);
            reset_positions(g);
        }
        break;

    case StatusGameOver:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static bool is_wall(Game* g, int x, int y) {
    if(x < 0 || x >= MAZE_W || y < 0 || y >= MAZE_H) return true;
    return g->tiles[y][x] == '#';
}

static void draw_round(Canvas* c, int px, int py) {
    canvas_draw_box(c, px + 1, py, 2, 4);
    canvas_draw_box(c, px, py + 1, 4, 2);
}

static void draw_pac(Canvas* c, Pac* p, bool visible) {
    if(!visible) return;
    int px = p->x * TILE, py = p->y * TILE;
    canvas_set_color(c, ColorBlack);
    draw_round(c, px, py);
    if(p->mouth) {
        canvas_set_color(c, ColorWhite);
        if(p->dx != 0) {
            int fx = p->dx > 0 ? px + 3 : px;
            int ix = p->dx > 0 ? px + 2 : px + 1;
            canvas_draw_dot(c, fx, py + 1);
            canvas_draw_dot(c, fx, py + 2);
            canvas_draw_dot(c, ix, py + 1);
        } else if(p->dy != 0) {
            int fy = p->dy > 0 ? py + 3 : py;
            int iy = p->dy > 0 ? py + 2 : py + 1;
            canvas_draw_dot(c, px + 1, fy);
            canvas_draw_dot(c, px + 2, fy);
            canvas_draw_dot(c, px + 1, iy);
        }
        canvas_set_color(c, ColorBlack);
    }
}

static void draw_ghost(Canvas* c, Ghost* gh, bool flash) {
    int px = gh->x * TILE, py = gh->y * TILE;
    canvas_set_color(c, ColorBlack);

    if(gh->scared && !flash) {
        canvas_draw_frame(c, px, py, 4, 4);
        return;
    }

    canvas_draw_box(c, px + 1, py, 2, 1);
    canvas_draw_box(c, px, py + 1, 4, 2);
    canvas_draw_dot(c, px, py + 3);
    canvas_draw_dot(c, px + 2, py + 3);

    if(!gh->scared) {
        canvas_set_color(c, ColorWhite);
        canvas_draw_dot(c, px + 1, py + 1);
        canvas_draw_dot(c, px + 2, py + 1);
        canvas_set_color(c, ColorBlack);
    }
}

static void draw_banner(Canvas* c, int cx, int cy, int w, const char* text) {
    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, cx - w / 2, cy - 5, w, 10);
    canvas_set_color(c, ColorBlack);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, cx, cy, AlignCenter, AlignCenter, text);
}

static void render_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
    Game* g = &app->game;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    bool hide_walls = (g->status == StatusLevelClear) && ((g->clock / 200) & 1);

    /* Maze */
    for(int y = 0; y < MAZE_H; y++) {
        for(int x = 0; x < MAZE_W; x++) {
            char t = g->tiles[y][x];
            int px = x * TILE, py = y * TILE;
            if(t == '#') {
                if(hide_walls) continue;
                if(!is_wall(g, x, y - 1)) canvas_draw_line(canvas, px, py, px + 3, py);
                if(!is_wall(g, x, y + 1)) canvas_draw_line(canvas, px, py + 3, px + 3, py + 3);
                if(!is_wall(g, x - 1, y)) canvas_draw_line(canvas, px, py, px, py + 3);
                if(!is_wall(g, x + 1, y)) canvas_draw_line(canvas, px + 3, py, px + 3, py + 3);
            } else if(t == '.') {
                canvas_draw_dot(canvas, px + 1, py + 1);
            } else if(t == 'o') {
                if((g->clock / 250) & 1) draw_round(canvas, px, py);
            }
        }
    }

    /* Ghosts */
    bool flash = g->scared_until && (g->scared_until - g->clock < 1800) &&
                 (((g->clock / 150) & 1) != 0);
    if(g->status != StatusLevelClear) {
        for(int i = 0; i < NUM_GHOSTS; i++) draw_ghost(canvas, &g->ghosts[i], flash);
    }

    /* Pac-Man (blinks while dying) */
    bool pac_visible = (g->status != StatusDying) || (((g->clock / 120) & 1) != 0);
    draw_pac(canvas, &g->pac, pac_visible);

    /* HUD */
    char buf[24];
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->score);
    canvas_draw_str(canvas, 1, 63, buf);
    snprintf(buf, sizeof(buf), "LV %u", g->level);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, buf);
    for(int i = 0; i < g->lives; i++) {
        draw_round(canvas, 122 - i * 6, 59);
    }

    /* Overlays */
    if(g->status == StatusReady) {
        draw_banner(canvas, 64, 28, 40, "READY!");
    } else if(g->paused) {
        draw_banner(canvas, 64, 28, 44, "PAUSED");
    } else if(g->status == StatusGameOver) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 22, 12, 84, 34);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_frame(canvas, 22, 12, 84, 34);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 21, AlignCenter, AlignCenter, "GAME OVER");
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            buf,
            sizeof(buf),
            "%lu  Hi %lu",
            (unsigned long)g->score,
            (unsigned long)g->high_score);
        canvas_draw_str_aligned(canvas, 64, 31, AlignCenter, AlignCenter, buf);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "OK: play again");
    }

    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t pacman_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    new_game(&app->game);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app->queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message_block(notifications, &sequence_display_backlight_enforce_on);

    bool running = true;
    uint32_t last = furi_get_tick();

    while(running) {
        InputEvent event;
        FuriStatus st = furi_message_queue_get(app->queue, &event, 20);

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        Game* g = &app->game;

        uint32_t now = furi_get_tick();
        uint32_t dt = now - last;
        last = now;

        if(st == FuriStatusOk && (event.type == InputTypePress || event.type == InputTypeRepeat)) {
            switch(event.key) {
            case InputKeyUp:
                g->pac.want_dx = 0;
                g->pac.want_dy = -1;
                break;
            case InputKeyDown:
                g->pac.want_dx = 0;
                g->pac.want_dy = 1;
                break;
            case InputKeyLeft:
                g->pac.want_dx = -1;
                g->pac.want_dy = 0;
                break;
            case InputKeyRight:
                g->pac.want_dx = 1;
                g->pac.want_dy = 0;
                break;
            case InputKeyOk:
                if(g->status == StatusGameOver) {
                    new_game(g);
                } else {
                    g->paused = !g->paused;
                }
                break;
            case InputKeyBack:
                running = false;
                break;
            default:
                break;
            }
        }

        if(!g->paused) game_update(g, dt);

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    notification_message_block(notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
