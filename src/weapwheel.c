/*
 * weapwheel.c — Weapon wheel UI for Duke Nukem 3D (Xbox port)
 *
 * Hold Black or White to open a radial weapon selector.
 * Use the left stick to highlight a weapon, release to equip.
 * Short taps still cycle weapons normally.
 */

#include "duke3d.h"
#include "names.h"
#include "weapwheel.h"
#include <math.h>

/* --- Configuration --- */
#define WW_HOLD_FRAMES    20   /* frames before tap becomes wheel-hold */
#define WW_RADIUS         56   /* circle radius in 320x200 virtual pixels */
#define WW_CENTER_X       160  /* center X (320x200 space) */
#define WW_CENTER_Y       100  /* center Y (320x200 space) */
#define WW_SCALE_NORMAL   24576  /* 0.375x (37.5% of 65536) */
#define WW_SCALE_HIGHLIGHT 36864 /* 0.5625x (56.25% of 65536) */
#define WW_STICK_DEADZONE 8000 /* left stick deadzone for selection */

/* 10 weapons on the wheel (excluding kick and special weapons) */
#define WW_NUM_WEAPONS 10

static const struct {
    int weapon_num;    /* internal weapon index (duke3d.h) */
    short sprite_tile; /* display sprite picnum */
    const char *name;
} ww_weapons[WW_NUM_WEAPONS] = {
    { PISTOL_WEAPON,     FIRSTGUNSPRITE,   "PISTOL"      },  /* 0: top */
    { SHOTGUN_WEAPON,    SHOTGUNSPRITE,    "SHOTGUN"     },  /* 1 */
    { CHAINGUN_WEAPON,   CHAINGUNSPRITE,   "CHAINGUN"    },  /* 2 */
    { RPG_WEAPON,        RPGSPRITE,        "RPG"         },  /* 3 */
    { HANDBOMB_WEAPON,   HEAVYHBOMB,       "PIPEBOMB"    },  /* 4: bottom */
    { SHRINKER_WEAPON,   SHRINKERSPRITE,   "SHRINKER"    },  /* 5 */
    { DEVISTATOR_WEAPON, DEVISTATORSPRITE, "DEVASTATOR"  },  /* 6 */
    { TRIPBOMB_WEAPON,   TRIPBOMBSPRITE,   "TRIPMINE"    },  /* 7 */
    { FREEZE_WEAPON,     FREEZESPRITE,     "FREEZER"     },  /* 8 */
    { GROW_WEAPON,       GROWSPRITEICON,   "EXPANDER"    },  /* 9 */
};

/* Precomputed sin/cos for 10 positions around a circle.
 * Position 0 = top (270 degrees), going clockwise. */
static int ww_pos_x[WW_NUM_WEAPONS];
static int ww_pos_y[WW_NUM_WEAPONS];
static int ww_positions_init = 0;

/* --- State --- */
static int ww_open = 0;           /* 1 = wheel is displayed */
static int ww_hold_count = 0;     /* frames the trigger button has been held */
static int ww_selected = -1;      /* currently highlighted weapon slot (-1 = none) */
static int ww_trigger_btn = 0;    /* which button opened the wheel (0=none) */
static int ww_confirmed = 0;      /* set on the frame the wheel closes */

static void ww_init_positions(void)
{
    int i;
    for (i = 0; i < WW_NUM_WEAPONS; i++) {
        /* Start at top (270 deg = -90 deg), go clockwise */
        double angle = (-90.0 + i * 36.0) * 3.14159265 / 180.0;
        ww_pos_x[i] = WW_CENTER_X + (int)(WW_RADIUS * cos(angle));
        ww_pos_y[i] = WW_CENTER_Y + (int)(WW_RADIUS * sin(angle));
    }
    ww_positions_init = 1;
}

int weapwheel_active(void)
{
    return ww_open;
}

void weapwheel_update(void)
{
    extern int joyaxis[], joyb;
    /* Read raw SDL joystick button bitmask — immune to CONTROL_ClearButton.
     * Bit 9 = White/LeftShoulder (Prev), Bit 10 = Black/RightShoulder (Next) */
    int btn_held = (joyb & ((1<<9) | (1<<10))) != 0;
    struct player_struct *p = &ps[myconnectindex];

    if (!ww_positions_init) ww_init_positions();

    if (btn_held) {
        if (ww_hold_count == 0) {
            /* First frame of press — record which button */
            ww_trigger_btn = (joyb & (1<<10)) ? 1 : 2;  /* 1=next(Black), 2=prev(White) */
        }
        ww_hold_count++;
        if (!ww_open && ww_hold_count >= WW_HOLD_FRAMES) {
            /* Held long enough — open the wheel */
            ww_open = 1;
            ww_selected = -1;
            ww_confirmed = 0;
            /* Freeze game world in single player */
            if (ud.multimode < 2)
                ud.pause_on = 1;
        }

        if (ww_open) {
            /* Read left stick for selection */
            int sx = joyaxis[0];
            int sy = joyaxis[1];
            int mag = (int)sqrt((double)sx*sx + (double)sy*sy);

            if (mag > WW_STICK_DEADZONE) {
                /* Compute angle from stick, map to 0-9 */
                double ang = atan2((double)sy, (double)sx) * 180.0 / 3.14159265;
                /* Shift so top (270/-90 deg) = slot 0 */
                ang += 90.0;
                if (ang < 0) ang += 360.0;
                if (ang >= 360.0) ang -= 360.0;
                ww_selected = ((int)(ang / 36.0)) % WW_NUM_WEAPONS;
            }
        }
    } else {
        /* Button released */
        if (ww_open) {
            /* Wheel was open — equip selected weapon */
            if (ww_selected >= 0 && ww_selected < WW_NUM_WEAPONS) {
                int w = ww_weapons[ww_selected].weapon_num;
                if (p->gotweapon[w] && (p->ammo_amount[w] > 0 || w == KNEE_WEAPON)) {
                    addweapon(p, w);
                }
            }
            ww_open = 0;
            ww_selected = -1;
            ww_confirmed = 1;
            /* Unfreeze game world */
            if (ud.multimode < 2)
                ud.pause_on = 0;
        } else if (ww_hold_count > 0 && ww_hold_count < WW_HOLD_FRAMES) {
            /* Short tap on release — do the weapon cycle now.
             * We suppressed the cycle during the hold, so handle it here. */
            extern int joyb;
            /* Determine which button was tapped (check last known state) */
            if (ww_hold_count > 0) {
                /* Cycle to next or previous based on which button was used.
                 * Since both are released now, use ww_trigger_btn if set,
                 * otherwise default to next. */
                int k = p->curr_weapon;
                int dir = (ww_trigger_btn == 2) ? -1 : 1;
                int tries = 10;
                do {
                    k += dir;
                    if (k < 0) k = 9;
                    if (k > 9) k = 0;
                    if (p->gotweapon[k] && p->ammo_amount[k] > 0) {
                        addweapon(p, k);
                        break;
                    }
                } while (--tries > 0);
            }
        }
        ww_hold_count = 0;
    }
}

void weapwheel_draw(void)
{
    int i;
    struct player_struct *p;
    short pal;

    if (!ww_open) return;
    if (!ww_positions_init) ww_init_positions();

    p = &ps[screenpeek];

    /* Fullscreen black translucent overlay using BLANK tile */
    {
        rotatesprite(0, 0, 65536, 0, 0,
                     0, 0, 1+8+16+64, 0, 0, xdim-1, ydim-1);
    }

    /* Transparent NUKEBUTTON backdrop behind the weapon circle */
    {
        /* Scale to encompass the weapon circle (radius 56 + sprite size).
         * NUKEBUTTON is small, so scale it up significantly. */
        int bg_scale = 65536 + 32768;  /* ~1.5x zoom */
        rotatesprite(WW_CENTER_X<<16, WW_CENTER_Y<<16, bg_scale, 0,
                     NUKEBUTTON, 16, 0, 2+8+1, 0, 0, xdim-1, ydim-1);
    }

    /* Draw each weapon in a circle */
    for (i = 0; i < WW_NUM_WEAPONS; i++) {
        int w = ww_weapons[i].weapon_num;
        int has_weapon = p->gotweapon[w] && (p->ammo_amount[w] > 0 || w == KNEE_WEAPON);
        int highlighted = (i == ww_selected);
        int scale = highlighted ? WW_SCALE_HIGHLIGHT : WW_SCALE_NORMAL;
        signed char shade;

        if (!has_weapon) {
            shade = 30;  /* dark/grayed out */
            pal = 0;
        } else if (highlighted) {
            shade = -16;  /* bright highlight */
            pal = 0;
        } else {
            shade = 8;   /* normal */
            pal = 0;
        }

        /* Current weapon gets a distinct palette */
        if (w == p->curr_weapon && has_weapon)
            pal = 6;  /* yellowish tint for current weapon */

        rotatesprite(ww_pos_x[i]<<16, ww_pos_y[i]<<16, scale, 0,
                     ww_weapons[i].sprite_tile, shade, pal,
                     2+8, 0, 0, xdim-1, ydim-1);

        /* Draw weapon name under the highlighted weapon */
        if (highlighted && has_weapon) {
            minitext(ww_pos_x[i]-12, ww_pos_y[i]+16,
                     ww_weapons[i].name, 0, 2+8+16);
        }
    }

    /* Center text */
    minitext(WW_CENTER_X - 24, WW_CENTER_Y - 4, "SELECT WEAPON", 0, 2+8+16);
}
