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

/* Debounce: track LB/RB button state from last frame for weapon prev/next */
static int p2_lb_prev = 0;
static int p2_rb_prev = 0;

void xbox_splitscreen_init(void)
{
    if (joy2 != NULL) return;

    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    /* Controller index 1 = port 2 on original Xbox (4-port breakaway) */
    if (SDL_NumJoysticks() > 1) {
        joy2 = SDL_JoystickOpen(1);
        if (joy2)
            buildprintf("SS_JOY: controller 2 opened ok\n");
        else
            buildprintf("SS_JOY: SDL_JoystickOpen(1) failed (NumJoysticks=%d)\n",
                SDL_NumJoysticks());
    } else {
        buildprintf("SS_JOY: only %d joystick(s) found, no controller 2\n",
            SDL_NumJoysticks());
    }
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

    /* Detect controller disconnect/reconnect (e.g. breakaway cable pull).
     * SDL_JoystickGetAttached() returns false when the device is gone.
     * Close the stale handle and attempt to reopen immediately. */
    if (!SDL_JoystickGetAttached(joy2)) {
        buildprintf("SS_JOY: controller 2 disconnected — attempting reopen\n");
        SDL_JoystickClose(joy2);
        joy2 = NULL;
        p2_lb_prev = 0;
        p2_rb_prev = 0;
        xbox_splitscreen_init();
        if (joy2 == NULL) return;
    }

    /* State is already fresh from the last SDL_PumpEvents in handleevents().
     * Do NOT call SDL_JoystickUpdate() here — it pushes SDL_JOYAXISMOTION
     * events into the queue that would bleed into joyaxis[] via sdlayer2.c
     * even with the which==0 filter (race between update and handleevents). */

    /* --- Axes ----------------------------------------------------------- */
    /* nxdk SDL2 raw joystick axis layout (verified from sdlayer2.c xbox_axis_map):
     *   Axis 0 = Left  stick X  (left=-32768, right=32767)
     *   Axis 1 = Left  stick Y  (up=-32768,   down=32767)
     *   Axis 2 = Left  trigger  (0..32767)
     *   Axis 3 = Right stick X
     *   Axis 4 = Right stick Y
     *   Axis 5 = Right trigger  (0..32767)
     * Note: axes 3-5 differ from the old (wrong) assumption of 4/5/3. */
    int lx = SDL_JoystickGetAxis(joy2, 0);
    int ly = SDL_JoystickGetAxis(joy2, 1);
    int rx = SDL_JoystickGetAxis(joy2, 3); /* right stick X (was 4, wrong) */
    int ry = SDL_JoystickGetAxis(joy2, 4); /* right stick Y (was 5, wrong) */
    int lt = SDL_JoystickGetAxis(joy2, 2); /* left  trigger (unchanged)    */
    int rt = SDL_JoystickGetAxis(joy2, 5); /* right trigger (was 3, wrong) */

    /* Apply deadzone */
    if (lx > -DEADZONE && lx < DEADZONE) lx = 0;
    if (ly > -DEADZONE && ly < DEADZONE) ly = 0;
    if (rx > -DEADZONE && rx < DEADZONE) rx = 0;
    if (ry > -DEADZONE && ry < DEADZONE) ry = 0;

    /* fvel/svel: store raw local-space values (-128..127).
     * World-space conversion (momx/momy) happens in faketimerhandler() in game.c
     * using ps[1].ang, matching what the CONTROL system does for player 1. */
    /* nxdk raw joystick convention: pushing forward = ly negative, left = lx negative.
     * World-space conversion in faketimerhandler (via mulscale9/sintable) requires
     * positive fwd for forward motion and positive str for right-strafe.
     * The strafe sintable term at ang=0 is negative, so str sign is flipped relative
     * to "push left = move left" — negate lx too so left-push → left-move. */
    inp->fvel = (short)((-ly) >> 8);   /* forward/back: push forward → positive fvel */
    inp->svel = (short)((-lx) >> 8);   /* strafe: negate because world-space formula inverts */
    /* avel/horz: >>10 gives -31..31 range matching P1's feel.
     * >>9 (-63..63) was too fast; >>10 is closer to the CONTROL system's default. */
    inp->avel = (signed char)(rx >> 10);
    inp->horz = (signed char)((-ry) >> 10); /* push up = negative ry = look up */

    /* --- Buttons -------------------------------------------------------- */
    /* Raw SDL joystick button order on nxdk for Xbox OG controller:
     *   0=A  1=B  2=X  3=Y  4=LB(White)  5=RB(Black)  6=Back  7=Start
     *   8=LeftStick  9=RightStick
     * This matches P1's sdlayer2.c xbox_btn_map[] raw ordering.
     *
     * Bit layout (player.c loc.bits):
     *   0=Jump  1=Crouch  2=Fire  5=Run  16=MedKit  20=Inv_Left
     *   22=QuickKick  25=Jetpack  27=Inv_Right  29=Open  30=Inventory  31=Escape
     *   8-11=weapon select (0=none, 11=prev, 12=next)
     *
     * P1 _functio.h Xbox defaults:
     *   A=Jump  B=Crouch  X=Open  Y=Inventory
     *   LB=Prev_Weapon  RB=Next_Weapon
     *   LT=Run  RT=Fire
     *   D-up=Jetpack  D-down=MedKit  D-left=Inv_Left  D-right=Inv_Right
     *   LS=QuickKick  Start=Menu */
    unsigned int bits = 0;

    /* Face buttons */
    if (SDL_JoystickGetButton(joy2, 0)) bits |= (1<<0);   /* A → Jump       */
    if (SDL_JoystickGetButton(joy2, 1)) bits |= (1<<1);   /* B → Crouch     */
    if (SDL_JoystickGetButton(joy2, 2)) bits |= (1<<29);  /* X → Open/Use   */
    if (SDL_JoystickGetButton(joy2, 3)) bits |= (1<<30);  /* Y → Inventory  */
    if (SDL_JoystickGetButton(joy2, 8)) bits |= (1<<22);  /* LS → QuickKick */
    if (SDL_JoystickGetButton(joy2, 7)) bits |= (1<<31);  /* Start → Escape */

    /* Triggers: RT=Fire, LT=Run (matches P1's digital axis defaults) */
    if (rt > 4000) bits |= (1<<2);  /* RT → Fire */
    if (lt > 4000) bits |= (1<<5);  /* LT → Run  */

    /* D-pad: Jetpack / MedKit / Inventory — held state, matches P1 D-pad defaults */
    int hat = SDL_JoystickGetHat(joy2, 0);
    if (hat & SDL_HAT_UP)    bits |= (1<<25);  /* D-up    → Jetpack        */
    if (hat & SDL_HAT_DOWN)  bits |= (1<<16);  /* D-down  → MedKit         */
    if (hat & SDL_HAT_LEFT)  bits |= (1<<20);  /* D-left  → Inventory_Left */
    if (hat & SDL_HAT_RIGHT) bits |= (1<<27);  /* D-right → Inventory_Right*/

    /* LB/RB: Previous/Next weapon with edge detection.
     * Encode into bits 8-11 for one frame on press (0 = no change). */
    int lb_now = SDL_JoystickGetButton(joy2, 4);
    int rb_now = SDL_JoystickGetButton(joy2, 5);
    int weapon_j = 0;
    if (lb_now && !p2_lb_prev) weapon_j = 11; /* Previous_Weapon */
    if (rb_now && !p2_rb_prev) weapon_j = 12; /* Next_Weapon */
    p2_lb_prev = lb_now;
    p2_rb_prev = rb_now;

    bits |= ((unsigned int)(weapon_j & 0xF)) << 8;

    inp->bits = bits;
}

/* Vibrate controller 2 independently from controller 1.
 * Called from player.c when player 2 takes damage or dies. */
void xbox_splitscreen_rumble(int low_freq, int high_freq, int duration_ms)
{
    if (joy2 == NULL) return;
    SDL_JoystickRumble(joy2, (Uint16)low_freq, (Uint16)high_freq, (Uint32)duration_ms);
}
