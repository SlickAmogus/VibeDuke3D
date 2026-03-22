/*
 * procgen.c — Procedural map generator for Duke Nukem 3D (Xbox port)
 *
 * Generates a chain of rectangular rooms connected by corridors,
 * populated with enemies and pickups.  Directly fills the engine's
 * sector[], wall[], sprite[] arrays — no .map file needed.
 */

#include "duke3d.h"
#include "names.h"
#include "procgen.h"
#include <stdlib.h>

/* --- Constants --- */
#define PG_MIN_ROOMS     8
#define PG_MAX_ROOMS     12
#define PG_ROOM_MIN      4096   /* minimum room dimension (Build units) */
#define PG_ROOM_MAX      8192   /* maximum room dimension */
#define PG_DOOR_WIDTH    1536   /* doorway opening width */
#define PG_HALL_LEN      2048   /* connector hallway length */
#define PG_FLOOR_Z       0      /* floor height */
#define PG_CEIL_Z        (-(128<<8))  /* ceiling height (-32768) */

/* Texture tile numbers */
#define PG_WALL_PIC      293    /* W_TECHWALL1 */
#define PG_FLOOR_PIC     183    /* SLIME tile */
#define PG_CEIL_PIC      126    /* generic ceiling */

/* Enemy / pickup / weapon / decoration picnums */
#define PG_ENEMIES_COUNT 2
static const short pg_enemies[PG_ENEMIES_COUNT] = { LIZTROOP, PIGCOP };

#define PG_PICKUPS_COUNT 6
static const short pg_pickups[PG_PICKUPS_COUNT] = {
    COLA, SIXPAK, FIRSTAID, SHOTGUNAMMO, RPGAMMO, FREEZEAMMO
};

#define PG_WEAPONS_COUNT 5
static const short pg_weapons[PG_WEAPONS_COUNT] = {
    CHAINGUNSPRITE, RPGSPRITE, SHOTGUNSPRITE, FREEZESPRITE, SHRINKERSPRITE
};

#define PG_DECOR_COUNT 5
static const short pg_decor[PG_DECOR_COUNT] = {
    WATERFOUNTAIN, RUBBERCAN, CANWITHSOMETHING, EXPLODINGBARREL, HYDRENT
};

/* --- Room data --- */
typedef struct {
    int x0, y0, x1, y1;     /* bounding box */
    int cx, cy;              /* center */
    int sect_idx;            /* sector index */
    int door_y0, door_y1;    /* doorway Y range (east/west doors) */
    int door_x0, door_x1;   /* doorway X range (north/south doors) */
} pgroom_t;

/* --- Barrier tracking (room-lock system) --- */
#define PG_MAX_BARRIERS PG_MAX_ROOMS
typedef struct {
    int barrier_sprite;  /* sprite index of blocking BIGFORCE, or -1 */
    int room_idx;        /* room index that must be cleared */
    int active;
} pgbarrier_t;

static pgbarrier_t pg_barriers[PG_MAX_BARRIERS];
static int pg_num_barriers = 0;
static pgroom_t pg_rooms_saved[PG_MAX_ROOMS];
static int pg_num_rooms_saved = 0;
static int pg_active = 0;       /* 1 when a procgen level is loaded */
static int pg_tick_count = 0;   /* ticks since level start */
static int pg_current_room = 1; /* which room's enemies are currently alive */
static int pg_enemies_spawned = 0; /* have enemies for current room been spawned? */

/* Enemy data per room — stored during generation, activated room-by-room */
#define PG_MAX_ROOM_ENEMIES 8
typedef struct {
    int count;
    struct {
        int x, y;
        short picnum;
        short sprite_idx;  /* engine sprite index (set after insertsprite) */
    } e[PG_MAX_ROOM_ENEMIES];
} pg_room_enemies_t;
static pg_room_enemies_t pg_room_enemies[PG_MAX_ROOMS];

/* --- Globals for construction --- */
static int pg_nsect, pg_nwall, pg_nsprite;

/* --- Helpers --- */
static int pg_rand_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (rand() % (hi - lo));
}

static void pg_init_sector(int s, int wallptr, int wallnum,
                           int floorz, int ceilz)
{
    memset(&sector[s], 0, sizeof(sectortype));
    sector[s].wallptr = wallptr;
    sector[s].wallnum = wallnum;
    sector[s].floorz = floorz;
    sector[s].ceilingz = ceilz;
    sector[s].floorpicnum = PG_FLOOR_PIC;
    sector[s].ceilingpicnum = PG_CEIL_PIC;
    sector[s].visibility = 32;
    sector[s].floorshade = 1;
    sector[s].ceilingshade = 1;
    sector[s].lotag = 0;
    sector[s].hitag = 0;
    sector[s].extra = -1;
}

static void pg_init_wall(int w, int x, int y, int point2,
                         int nextsector, int nextwall)
{
    memset(&wall[w], 0, sizeof(walltype));
    wall[w].x = x;
    wall[w].y = y;
    wall[w].point2 = point2;
    wall[w].nextsector = nextsector;
    wall[w].nextwall = nextwall;
    wall[w].picnum = PG_WALL_PIC;
    wall[w].overpicnum = 0;
    wall[w].cstat = 0;
    wall[w].shade = 0;
    wall[w].pal = 0;
    wall[w].xrepeat = 8;
    wall[w].yrepeat = 8;
    wall[w].lotag = 0;
    wall[w].hitag = 0;
    wall[w].extra = -1;
}

static int pg_add_sprite(int x, int y, int z, short picnum,
                          short sectnum, short ang)
{
    int i = pg_nsprite++;
    memset(&sprite[i], 0, sizeof(spritetype));
    sprite[i].x = x;
    sprite[i].y = y;
    sprite[i].z = z;
    sprite[i].picnum = picnum;
    sprite[i].sectnum = sectnum;
    sprite[i].statnum = 0;
    sprite[i].ang = ang;
    sprite[i].owner = i;
    sprite[i].xrepeat = 0;   /* spawn() will set these */
    sprite[i].yrepeat = 0;
    sprite[i].clipdist = 0;
    sprite[i].cstat = 0;
    sprite[i].shade = 0;
    sprite[i].pal = 0;
    sprite[i].xvel = 0;
    sprite[i].yvel = 0;
    sprite[i].zvel = 0;
    sprite[i].lotag = 0;
    sprite[i].hitag = 0;
    sprite[i].extra = -1;
    return i;
}

/*
 * Build a rectangular room sector.  Returns the wall index after
 * the last wall written.
 *
 * Walls go clockwise: NW→NE→SE→SW
 *   Wall 0: (x0,y0) → (x1,y0)   north
 *   Wall 1: (x1,y0) → (x1,y1)   east
 *   Wall 2: (x1,y1) → (x0,y1)   south
 *   Wall 3: (x0,y1) → (x0,y0)   west
 *
 * If a wall has a doorway, it gets split into 3 segments.
 * door_side: 0=none, 1=east, 2=south, 3=west, 4=north
 */
static int pg_build_room(pgroom_t *rm, int s, int wbase,
                          int door_east, int door_west,
                          int door_south, int door_north)
{
    int w = wbase;
    int x0 = rm->x0, y0 = rm->y0, x1 = rm->x1, y1 = rm->y1;
    int first_wall = w;

    /* North wall */
    if (door_north) {
        pg_init_wall(w, x0, y0, w+1, -1, -1); w++;              /* NW → door left */
        pg_init_wall(w, rm->door_x0, y0, w+1, -1, -1); w++;     /* door left → door right (portal, linked later) */
        pg_init_wall(w, rm->door_x1, y0, w+1, -1, -1); w++;     /* door right → NE */
    } else {
        pg_init_wall(w, x0, y0, w+1, -1, -1); w++;              /* NW → NE */
    }

    /* East wall */
    if (door_east) {
        pg_init_wall(w, x1, y0, w+1, -1, -1); w++;              /* NE → door top */
        pg_init_wall(w, x1, rm->door_y0, w+1, -1, -1); w++;     /* door top → door bot (portal) */
        pg_init_wall(w, x1, rm->door_y1, w+1, -1, -1); w++;     /* door bot → SE */
    } else {
        pg_init_wall(w, x1, y0, w+1, -1, -1); w++;              /* NE → SE */
    }

    /* South wall */
    if (door_south) {
        pg_init_wall(w, x1, y1, w+1, -1, -1); w++;              /* SE → door right */
        pg_init_wall(w, rm->door_x1, y1, w+1, -1, -1); w++;     /* door right → door left (portal) */
        pg_init_wall(w, rm->door_x0, y1, w+1, -1, -1); w++;     /* door left → SW */
    } else {
        pg_init_wall(w, x1, y1, w+1, -1, -1); w++;              /* SE → SW */
    }

    /* West wall */
    if (door_west) {
        pg_init_wall(w, x0, y1, w+1, -1, -1); w++;              /* SW → door bot */
        pg_init_wall(w, x0, rm->door_y1, w+1, -1, -1); w++;     /* door bot → door top (portal) */
        pg_init_wall(w, x0, rm->door_y0, w+1, -1, -1); w++;     /* door top → NW */
    } else {
        pg_init_wall(w, x0, y1, w+1, -1, -1); w++;              /* SW → NW */
    }

    /* Close the loop: last wall's point2 → first wall */
    wall[w-1].point2 = first_wall;

    int nwalls = w - wbase;
    rm->sect_idx = s;
    pg_init_sector(s, wbase, nwalls, PG_FLOOR_Z, PG_CEIL_Z);

    return w;
}

/*
 * Build a connector hallway between two rooms.
 * dir: 0 = east-west (horizontal), 1 = north-south (vertical)
 */
static int pg_build_connector(int s, int wbase,
                               int x0, int y0, int x1, int y1)
{
    int w = wbase;
    /* 4 walls, clockwise: N→E→S→W */
    pg_init_wall(w, x0, y0, w+1, -1, -1); w++;
    pg_init_wall(w, x1, y0, w+1, -1, -1); w++;
    pg_init_wall(w, x1, y1, w+1, -1, -1); w++;
    pg_init_wall(w, x0, y1, wbase, -1, -1); w++;

    pg_init_sector(s, wbase, 4, PG_FLOOR_Z, PG_CEIL_Z);
    return w;
}

/*
 * Link two portal walls (bidirectional).
 * wall_a in sector_a ↔ wall_b in sector_b.
 */
static void pg_link_portal(int wall_a, int sect_a, int wall_b, int sect_b)
{
    wall[wall_a].nextsector = sect_b;
    wall[wall_a].nextwall = wall_b;
    wall[wall_b].nextsector = sect_a;
    wall[wall_b].nextwall = wall_a;
}

/*
 * Find the wall index within a room's sector that starts at (x,y).
 */
static int pg_find_wall(int sect, int x, int y)
{
    int base = sector[sect].wallptr;
    int num = sector[sect].wallnum;
    for (int i = 0; i < num; i++) {
        if (wall[base+i].x == x && wall[base+i].y == y)
            return base + i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */

int procgen_generate_level(int *posx, int *posy, int *posz,
                           short *ang, short *cursectnum)
{
    int i;
    int num_rooms = PG_MIN_ROOMS + (rand() % (PG_MAX_ROOMS - PG_MIN_ROOMS + 1));
    pgroom_t rooms[PG_MAX_ROOMS];
    int conn_sect[PG_MAX_ROOMS]; /* sector index of connector between room i and i+1 */

    /* Reset engine state */
    initspritelists();
    clearbuf(&show2dsector[0], (int)((MAXSECTORS+3)>>5), 0L);
    clearbuf(&show2dsprite[0], (int)((MAXSPRITES+3)>>5), 0L);
    clearbuf(&show2dwall[0], (int)((MAXWALLS+3)>>5), 0L);

    pg_nsect = 0;
    pg_nwall = 0;
    pg_nsprite = 0;

    srand(totalclock ^ (totalclock << 16));

    /* --- Phase 1: Decide room sizes and positions --- */
    int cur_x = 0, cur_y = 0;
    for (i = 0; i < num_rooms; i++) {
        int rw = PG_ROOM_MIN + (rand() % (PG_ROOM_MAX - PG_ROOM_MIN));
        int rh = PG_ROOM_MIN + (rand() % (PG_ROOM_MAX - PG_ROOM_MIN));

        rooms[i].x0 = cur_x;
        rooms[i].y0 = cur_y;
        rooms[i].x1 = cur_x + rw;
        rooms[i].y1 = cur_y + rh;
        rooms[i].cx = cur_x + rw / 2;
        rooms[i].cy = cur_y + rh / 2;

        /* Compute doorway positions (centered on wall) */
        int dy_center = cur_y + rh / 2;
        rooms[i].door_y0 = dy_center - PG_DOOR_WIDTH / 2;
        rooms[i].door_y1 = dy_center + PG_DOOR_WIDTH / 2;
        int dx_center = cur_x + rw / 2;
        rooms[i].door_x0 = dx_center - PG_DOOR_WIDTH / 2;
        rooms[i].door_x1 = dx_center + PG_DOOR_WIDTH / 2;

        /* Position next room */
        if (i < num_rooms - 1) {
            int next_w = PG_ROOM_MIN + (rand() % (PG_ROOM_MAX - PG_ROOM_MIN));
            int next_h = PG_ROOM_MIN + (rand() % (PG_ROOM_MAX - PG_ROOM_MIN));
            if (i % 2 == 0) {
                /* Connect east: next room is to the right */
                cur_x = rooms[i].x1 + PG_HALL_LEN;
                cur_y = rooms[i].cy - next_h / 2;
            } else {
                /* Connect south: next room is below */
                cur_y = rooms[i].y1 + PG_HALL_LEN;
                cur_x = rooms[i].cx - next_w / 2;
            }
        }
    }

    /* --- Phase 2: Build room sectors and walls --- */
    for (i = 0; i < num_rooms; i++) {
        int door_east = 0, door_west = 0, door_south = 0, door_north = 0;

        /* Determine which sides have doors */
        if (i < num_rooms - 1) {
            if (i % 2 == 0) door_east = 1;
            else door_south = 1;
        }
        if (i > 0) {
            if ((i - 1) % 2 == 0) door_west = 1;
            else door_north = 1;
        }

        /* For west/north doors, use the CONNECTOR's doorway coords
         * (which match the previous room's door position) */
        if (door_west) {
            int prev = i - 1;
            rooms[i].door_y0 = rooms[prev].door_y0;
            rooms[i].door_y1 = rooms[prev].door_y1;
        }
        if (door_north) {
            int prev = i - 1;
            rooms[i].door_x0 = rooms[prev].door_x0;
            rooms[i].door_x1 = rooms[prev].door_x1;
        }

        pg_nwall = pg_build_room(&rooms[i], pg_nsect, pg_nwall,
                                  door_east, door_west,
                                  door_south, door_north);
        pg_nsect++;

        /* Build connector hallway to next room */
        if (i < num_rooms - 1) {
            int cs = pg_nsect;
            conn_sect[i] = cs;

            if (i % 2 == 0) {
                /* East-west connector */
                int hx0 = rooms[i].x1;
                int hx1 = rooms[i].x1 + PG_HALL_LEN;
                int hy0 = rooms[i].door_y0;
                int hy1 = rooms[i].door_y1;
                pg_nwall = pg_build_connector(cs, pg_nwall, hx0, hy0, hx1, hy1);
            } else {
                /* North-south connector */
                int hx0 = rooms[i].door_x0;
                int hx1 = rooms[i].door_x1;
                int hy0 = rooms[i].y1;
                int hy1 = rooms[i].y1 + PG_HALL_LEN;
                pg_nwall = pg_build_connector(cs, pg_nwall, hx0, hy0, hx1, hy1);
            }
            pg_nsect++;
        }
    }

    numsectors = pg_nsect;
    numwalls = pg_nwall;

    /* --- Phase 3: Link portal walls --- */
    for (i = 0; i < num_rooms - 1; i++) {
        int rs = rooms[i].sect_idx;
        int cs = conn_sect[i];
        int ns = rooms[i + 1].sect_idx;

        if (i % 2 == 0) {
            /* East connector: room→connector west wall, connector→room east wall */
            /* Room's east doorway wall starts at (x1, door_y0) */
            int rw = pg_find_wall(rs, rooms[i].x1, rooms[i].door_y0);
            /* Connector's west wall starts at (x1, door_y1) → goes to (x1, door_y0) */
            int cw_west = pg_find_wall(cs, rooms[i].x1, rooms[i].door_y1);
            if (rw >= 0 && cw_west >= 0)
                pg_link_portal(rw, rs, cw_west, cs);

            /* Connector's east wall starts at (x1+HALL, door_y0) */
            int hx1 = rooms[i].x1 + PG_HALL_LEN;
            int cw_east = pg_find_wall(cs, hx1, rooms[i].door_y0);
            /* Next room's west doorway starts at (next.x0, door_y1) */
            int nw = pg_find_wall(ns, rooms[i+1].x0, rooms[i].door_y1);
            if (cw_east >= 0 && nw >= 0)
                pg_link_portal(cw_east, cs, nw, ns);
        } else {
            /* South connector: room→connector north wall, connector→room south wall */
            /* Room's south doorway wall starts at (door_x1, y1) */
            int rw = pg_find_wall(rs, rooms[i].door_x1, rooms[i].y1);
            /* Connector's north wall starts at (door_x0, y1) */
            int cw_north = pg_find_wall(cs, rooms[i].door_x0, rooms[i].y1);
            if (rw >= 0 && cw_north >= 0)
                pg_link_portal(rw, rs, cw_north, cs);

            /* Connector's south wall starts at (door_x1, y1+HALL) */
            int hy1 = rooms[i].y1 + PG_HALL_LEN;
            int cw_south = pg_find_wall(cs, rooms[i].door_x1, hy1);
            /* Next room's north doorway starts at (door_x0, next.y0) */
            int nw = pg_find_wall(ns, rooms[i].door_x0, rooms[i+1].y0);
            if (cw_south >= 0 && nw >= 0)
                pg_link_portal(cw_south, cs, nw, ns);
        }
    }

    /* --- Phase 4: Place sprites --- */

    /* Player start in room 0 */
    pg_add_sprite(rooms[0].cx, rooms[0].cy, PG_FLOOR_Z,
                  APLAYER, rooms[0].sect_idx, 1536);

    /* Room 1: place enemies during generation (prelevel/spawn initializes them).
     * Rooms 2+: only STORE enemy data — they'll be created at runtime when
     * the room activates, so they don't exist, make sounds, or fight until then. */
    memset(pg_room_enemies, 0, sizeof(pg_room_enemies));
    for (i = 1; i < num_rooms; i++) {
        int n_enemies = 2 + (rand() % 4);  /* 2-5 */
        if (n_enemies > PG_MAX_ROOM_ENEMIES) n_enemies = PG_MAX_ROOM_ENEMIES;
        pg_room_enemies[i].count = n_enemies;
        for (int e = 0; e < n_enemies; e++) {
            pg_room_enemies[i].e[e].x = pg_rand_range(rooms[i].x0 + 512, rooms[i].x1 - 512);
            pg_room_enemies[i].e[e].y = pg_rand_range(rooms[i].y0 + 512, rooms[i].y1 - 512);
            pg_room_enemies[i].e[e].picnum = pg_enemies[rand() % PG_ENEMIES_COUNT];
            pg_room_enemies[i].e[e].sprite_idx = -1;  /* not yet created */
        }
    }

    /* Only room 1 enemies are placed as actual sprites */
    for (i = 0; i < pg_room_enemies[1].count; i++) {
        int si = pg_add_sprite(pg_room_enemies[1].e[i].x, pg_room_enemies[1].e[i].y,
                      PG_FLOOR_Z, pg_room_enemies[1].e[i].picnum,
                      rooms[1].sect_idx, rand() & 2047);
        pg_room_enemies[1].e[i].sprite_idx = si;
    }

    /* Pickups scattered in most rooms */
    for (i = 0; i < num_rooms; i++) {
        int n_items = 2 + (rand() % 3);  /* 2-4 per room */
        for (int p = 0; p < n_items; p++) {
            int px = pg_rand_range(rooms[i].x0 + 512, rooms[i].x1 - 512);
            int py = pg_rand_range(rooms[i].y0 + 512, rooms[i].y1 - 512);
            short ppic = pg_pickups[rand() % PG_PICKUPS_COUNT];
            pg_add_sprite(px, py, PG_FLOOR_Z, ppic,
                          rooms[i].sect_idx, 0);
        }
    }

    /* Weapons: one random weapon per room (starting from room 1) */
    for (i = 1; i < num_rooms; i++) {
        int wx = pg_rand_range(rooms[i].x0 + 1024, rooms[i].x1 - 1024);
        int wy = pg_rand_range(rooms[i].y0 + 1024, rooms[i].y1 - 1024);
        short wpic = pg_weapons[rand() % PG_WEAPONS_COUNT];
        pg_add_sprite(wx, wy, PG_FLOOR_Z, wpic,
                      rooms[i].sect_idx, 0);
    }

    /* Decorations: 1-3 per room in corners and edges */
    for (i = 0; i < num_rooms; i++) {
        int n_decor = 1 + (rand() % 3);
        for (int d = 0; d < n_decor; d++) {
            /* Place near walls (within 768 units of edges) */
            int dx, dy;
            int edge = rand() % 4;
            switch (edge) {
                case 0: /* north wall */
                    dx = pg_rand_range(rooms[i].x0 + 512, rooms[i].x1 - 512);
                    dy = rooms[i].y0 + 384;
                    break;
                case 1: /* south wall */
                    dx = pg_rand_range(rooms[i].x0 + 512, rooms[i].x1 - 512);
                    dy = rooms[i].y1 - 384;
                    break;
                case 2: /* east wall */
                    dx = rooms[i].x1 - 384;
                    dy = pg_rand_range(rooms[i].y0 + 512, rooms[i].y1 - 512);
                    break;
                default: /* west wall */
                    dx = rooms[i].x0 + 384;
                    dy = pg_rand_range(rooms[i].y0 + 512, rooms[i].y1 - 512);
                    break;
            }
            short dpic = pg_decor[rand() % PG_DECOR_COUNT];
            int di = pg_add_sprite(dx, dy, PG_FLOOR_Z, dpic,
                          rooms[i].sect_idx, rand() & 2047);
            /* Decorations need explicit repeat values — spawn() doesn't
             * always set them for non-enemy sprites. */
            sprite[di].xrepeat = 32;
            sprite[di].yrepeat = 32;
        }
    }

    /* Barriers: close each connector's ceiling to the floor.
     * Room 0→1 connector stays OPEN (spawn room has no enemies).
     * Rooms 1→2, 2→3, etc. are crushed shut until cleared. */
    pg_num_barriers = 0;
    for (i = 0; i < num_rooms - 1; i++) {
        int cs = conn_sect[i];
        if (i == 0) {
            /* First connector (room 0→1) stays open */
            pg_barriers[pg_num_barriers].barrier_sprite = cs;
            pg_barriers[pg_num_barriers].room_idx = 1;
            pg_barriers[pg_num_barriers].active = 0;  /* already open */
        } else {
            sector[cs].ceilingz = sector[cs].floorz;  /* crushed shut */
            pg_barriers[pg_num_barriers].barrier_sprite = cs;
            pg_barriers[pg_num_barriers].room_idx = i + 1;
            pg_barriers[pg_num_barriers].active = 1;
        }
        pg_num_barriers++;
    }

    /* Save room data for tick checks */
    pg_num_rooms_saved = num_rooms;
    for (i = 0; i < num_rooms; i++)
        pg_rooms_saved[i] = rooms[i];
    pg_active = 1;
    pg_tick_count = 0;
    pg_current_room = 1;
    pg_enemies_spawned = 1;  /* room 1 enemies already spawned above */

    /* Exit nuke button in last room — centered, on the wall */
    {
        int last = num_rooms - 1;
        /* Place on the far wall of the last room so the player walks toward it */
        int nx = rooms[last].cx;
        int ny = rooms[last].y0 + 384;  /* near north wall */
        int si = pg_add_sprite(nx, ny, PG_FLOOR_Z - (40 << 8),
                               NUKEBUTTON, rooms[last].sect_idx, 1536);
        sprite[si].lotag = 65535;  /* non-zero so neartag finds it */
        sprite[si].cstat = 16;    /* wall-aligned sprite */
        sprite[si].xrepeat = 32;
        sprite[si].yrepeat = 32;
    }

    /* Register all sprites in engine linked lists */
    for (i = 0; i < pg_nsprite; i++)
        insertsprite(sprite[i].sectnum, sprite[i].statnum);

    /* Set player start */
    *posx = rooms[0].cx;
    *posy = rooms[0].cy;
    *posz = PG_FLOOR_Z;
    *ang = 1536;
    updatesector(*posx, *posy, cursectnum);

#if USE_POLYMOST && USE_OPENGL
    memset(spriteext, 0, sizeof(spriteext));
#endif

    return 0;
}

/* Create enemies for a room at runtime using insertsprite + spawn.
 * Enemies don't exist until this is called — no sounds, no AI, nothing. */
static void pg_activate_room_enemies(int room)
{
    int e;
    if (room < 1 || room >= pg_num_rooms_saved) return;

    int sect = pg_rooms_saved[room].sect_idx;
    int floorz = sector[sect].floorz;

    for (e = 0; e < pg_room_enemies[room].count; e++) {
        int si = insertsprite(sect, 0);
        if (si < 0) continue;

        sprite[si].x = pg_room_enemies[room].e[e].x;
        sprite[si].y = pg_room_enemies[room].e[e].y;
        sprite[si].z = floorz;
        sprite[si].picnum = pg_room_enemies[room].e[e].picnum;
        sprite[si].cstat = 0;
        sprite[si].shade = 0;
        sprite[si].pal = 0;
        sprite[si].clipdist = 0;
        sprite[si].xrepeat = 0;
        sprite[si].yrepeat = 0;
        sprite[si].xoffset = 0;
        sprite[si].yoffset = 0;
        sprite[si].ang = rand() & 2047;
        sprite[si].owner = si;
        sprite[si].xvel = 0;
        sprite[si].yvel = 0;
        sprite[si].zvel = 0;
        sprite[si].lotag = 0;
        sprite[si].hitag = 0;
        sprite[si].extra = -1;

        /* spawn() fully initializes: health, AI, xrepeat/yrepeat, statnum→1 */
        spawn(-1, si);
        pg_room_enemies[room].e[e].sprite_idx = si;

        /* Teleport sound at spawn position */
        spritesound(TELEPORTER, si);
    }
}

/* No-op: rooms 2+ have no sprites placed during generation */
void procgen_post_prelevel(void) { }

/* --- Barrier tick: sequential room-by-room progression --- */
void procgen_tick(void)
{
    int i;
    if (!pg_active) return;

    pg_tick_count++;
    /* Grace period after level start or room spawn */
    if (pg_tick_count < 90) return;

    /* Check if current room is cleared (no actors in statnum 1
     * within the room's bounding box) */
    if (pg_current_room >= pg_num_rooms_saved) return;

    /* Check if all tracked enemies for this room are dead.
     * Enemies are in statnum 2 (actors) with health in sprite[si].extra.
     * Dead enemies have extra <= 0 or leave statnum 2. */
    int alive = 0;
    for (i = 0; i < pg_room_enemies[pg_current_room].count; i++) {
        int si = pg_room_enemies[pg_current_room].e[i].sprite_idx;
        if (si >= 0 && sprite[si].statnum >= 1 && sprite[si].statnum <= 2
            && sprite[si].extra > 0)
            alive++;
    }

    if (alive == 0) {
        /* Room cleared — open the barrier AFTER this room (to the next room) */
        int b = pg_current_room;  /* barrier i = connector between room i and room i+1 */
        if (b >= 0 && b < pg_num_barriers && pg_barriers[b].active) {
            int cs = pg_barriers[b].barrier_sprite;
            if (sector[cs].ceilingz >= sector[cs].floorz) {
                sector[cs].ceilingz = PG_CEIL_Z;
                sound(ELEVATOR_ON);
            }
            pg_barriers[b].active = 0;
        }

        /* Advance to next room and create its enemies */
        pg_current_room++;
        if (pg_current_room < pg_num_rooms_saved) {
            pg_activate_room_enemies(pg_current_room);
            pg_tick_count = 0;  /* reset grace period for new room */
        }
    }
}
