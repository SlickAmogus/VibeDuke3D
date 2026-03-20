#ifndef __procgen_h__
#define __procgen_h__

/* Generate a procedural level by populating sector[], wall[], sprite[].
 * Returns 0 on success. Outputs player start position. */
int procgen_generate_level(int *posx, int *posy, int *posz,
                           short *ang, short *cursectnum);

/* Call each game tick — removes room barriers when all enemies are dead. */
void procgen_tick(void);

#endif
