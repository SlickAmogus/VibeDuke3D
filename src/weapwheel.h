#ifndef __weapwheel_h__
#define __weapwheel_h__

/* Call each frame from displayrest() to draw the wheel overlay. */
void weapwheel_draw(void);

/* Call from input handling to check/update wheel state.
 * Returns 1 if the wheel is active (suppress other input). */
int weapwheel_active(void);

/* Call each tick from processinput or the game loop. */
void weapwheel_update(void);

#endif
