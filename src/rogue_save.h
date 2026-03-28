#ifndef __rogue_save_h__
#define __rogue_save_h__

typedef struct {
    int gauntlet_completions;
    /* Future: gold, unlocks, persistent upgrades */
} rogue_data_t;

extern rogue_data_t rogue_data;

int rogue_load(void);   /* Returns 0 on success */
int rogue_save(void);   /* Returns 0 on success */

#endif
