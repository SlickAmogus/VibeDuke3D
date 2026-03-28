/* xbox_splitscreen.c — Couch co-op splitscreen support for jfduke3d-xbox
 *
 * Reads Xbox controller port 2 via SDL joystick and maps it to the Duke3D
 * input struct so faketimerhandler() can inject it as player 2's input.
 *
 * Layout:
 *   Left stick  → fvel / svel (move forward/strafe)
 *   Right stick → avel / horz (turn / look up-down)
 *   Deadzone    → 8000 (out of 32767)
 *
 * Button mapping (Xbox controller):
 *   A            → Jump        (bit 0)
 *   B            → Open/Use    (bit 29)
 *   X            → Fire        (bit 2)
 *   Y            → Inventory   (bit 30)
 *   LB           → Quick kick  (bit 22)
 *   RB           → Run/Sprint  (bit 5)
 *   LT (axis)    → Crouch      (bit 1)
 *   RT (axis)    → Fire        (bit 2, alternate)
 *   Start        → Escape/Menu (bit 31)
 *   Back         → Map toggle  (bit 18, center_view reused)
 *   D-pad up     → Look up     (bit 13)
 *   D-pad down   → Look down   (bit 14)
 *   D-pad left   → Prev weapon (navigated via weapon bits cycling)
 *   D-pad right  → Next weapon (same)
 */

#include "SDL.h"
#include <string.h>

/* Forward-declared types from duke3d.h — avoid full include to prevent
 * circular dependency; we only need the input struct layout. */
typedef struct {
    signed char  avel;   /* turn velocity */
    signed char  horz;   /* look up/down  */
    short        fvel;   /* forward vel   */
    short        svel;   /* strafe vel    */
    unsigned int bits;   /* button bitfield */
} xbox_split_input_t;

#define DEADZONE 8000

/* The SDL joystick handle for controller port 2 (index 1).
 * Opened lazily on first call to xbox_splitscreen_init(). */
static SDL_Joystick *joy2 = NULL;

/* Current weapon slot for player 2 (1-based, matches Duke3D weapon numbering) */
static int p2_weapon = 1;
/* Debounce: remember d-pad hat state from last frame */
static int p2_hat_prev = SDL_HAT_CENTERED;

void xbox_splitscreen_init(void)
{
    if (joy2 != NULL) return;

    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    /* Controller index 1 = port 2 on original Xbox (4-port breakaway) */
    if (SDL_NumJoysticks() > 1)
        joy2 = SDL_JoystickOpen(1);
}

void xbox_splitscreen_shutdown(void)
{
    if (joy2) {
        SDL_JoystickClose(joy2);
        joy2 = NULL;
    }
}

/* Read controller 2 and fill *inp with the Duke3D input values.
 * Called from faketimerhandler() in game.c when ud.splitscreen != 0. */
void xbox_splitscreen_getinput(void *inp_vp)
{
    xbox_split_input_t *inp = (xbox_split_input_t *)inp_vp;
    memset(inp, 0, sizeof(*inp));

    if (joy2 == NULL) {
        xbox_splitscreen_init();
        if (joy2 == NULL) return;
    }

    SDL_JoystickUpdate();

    /* --- Axes ----------------------------------------------------------- */
    /* SDL Xbox layout (nxdk SDL2 port):
     *   Axis 0 = Left  stick X  (left=-32768, right=32767)
     *   Axis 1 = Left  stick Y  (up=-32768,   down=32767)
     *   Axis 2 = Left  trigger  (0..32767)
     *   Axis 3 = Right trigger  (0..32767)
     *   Axis 4 = Right stick X
     *   Axis 5 = Right stick Y
     */
    int lx = SDL_JoystickGetAxis(joy2, 0);
    int ly = SDL_JoystickGetAxis(joy2, 1);
    int rx = SDL_JoystickGetAxis(joy2, 4);
    int ry = SDL_JoystickGetAxis(joy2, 5);
    int lt = SDL_JoystickGetAxis(joy2, 2); /* 0..32767 */
    int rt = SDL_JoystickGetAxis(joy2, 3); /* 0..32767 */

    /* Apply deadzone */
    if (lx > -DEADZONE && lx < DEADZONE) lx = 0;
    if (ly > -DEADZONE && ly < DEADZONE) ly = 0;
    if (rx > -DEADZONE && rx < DEADZONE) rx = 0;
    if (ry > -DEADZONE && ry < DEADZONE) ry = 0;

    /* fvel: forward/back — left stick Y, inverted (push forward = negative Y) */
    inp->fvel = (short)((-ly) >> 8);   /* ~-128..127 */
    /* svel: strafe — left stick X */
    inp->svel = (short)(lx >> 8);
    /* avel: turn — right stick X */
    inp->avel = (signed char)(rx >> 9);  /* ~-64..63 */
    /* horz: look up/down — right stick Y */
    inp->horz = (signed char)((-ry) >> 9);

    /* --- Buttons -------------------------------------------------------- */
    /* SDL maps Xbox face buttons: 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start
     *                             8=LeftStick 9=RightStick               */
    unsigned int bits = 0;

    if (SDL_JoystickGetButton(joy2, 0)) bits |= (1<<0);   /* A → Jump    */
    if (SDL_JoystickGetButton(joy2, 2)) bits |= (1<<2);   /* X → Fire    */
    if (SDL_JoystickGetButton(joy2, 1)) bits |= (1<<29);  /* B → Open    */
    if (SDL_JoystickGetButton(joy2, 3)) bits |= (1<<30);  /* Y → Inventory */
    if (SDL_JoystickGetButton(joy2, 4)) bits |= (1<<22);  /* LB → QuickKick */
    if (SDL_JoystickGetButton(joy2, 5)) bits |= (1<<5);   /* RB → Run    */
    if (SDL_JoystickGetButton(joy2, 7)) bits |= (1<<31);  /* Start → Menu/Esc */
    if (SDL_JoystickGetButton(joy2, 6)) bits |= (1<<18);  /* Back → center view (map-like) */

    /* RT → Fire (analog trigger threshold) */
    if (rt > 4000) bits |= (1<<2);
    /* LT → Crouch */
    if (lt > 4000) bits |= (1<<1);

    /* D-pad (hat 0) — weapon cycling with debounce */
    int hat = SDL_JoystickGetHat(joy2, 0);

    if ((hat & SDL_HAT_UP)   && !(p2_hat_prev & SDL_HAT_UP))   bits |= (1<<13); /* look up */
    if ((hat & SDL_HAT_DOWN) && !(p2_hat_prev & SDL_HAT_DOWN)) bits |= (1<<14); /* look down */

    if ((hat & SDL_HAT_RIGHT) && !(p2_hat_prev & SDL_HAT_RIGHT)) {
        p2_weapon++;
        if (p2_weapon > 10) p2_weapon = 1;
    }
    if ((hat & SDL_HAT_LEFT) && !(p2_hat_prev & SDL_HAT_LEFT)) {
        p2_weapon--;
        if (p2_weapon < 1) p2_weapon = 10;
    }
    p2_hat_prev = hat;

    /* Encode weapon number into bits 8-11 */
    bits |= ((unsigned int)(p2_weapon & 0xF)) << 8;

    inp->bits = bits;
}
