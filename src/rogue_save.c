/*
 * rogue_save.c — Roguelite persistent save data
 *
 * Simple XOR-obfuscated binary file storing gauntlet stats.
 * Saved to the Xbox writable directory alongside duke3d.cfg.
 */

#include "duke3d.h"
#include "rogue_save.h"
#include <stdio.h>
#include <string.h>

rogue_data_t rogue_data = { 0 };

#define ROGUE_XOR_KEY  0xA5
#define ROGUE_MAGIC    "RGS1"
#define ROGUE_MAGIC_LEN 4

static void rogue_xor(unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++)
        buf[i] ^= ROGUE_XOR_KEY;
}

static void rogue_build_path(char *out, int maxlen)
{
#ifdef _XBOX
    extern const char *xbox_get_writedir(void);
    Bsnprintf(out, maxlen, "%srogue.sav", xbox_get_writedir());
#else
    Bsnprintf(out, maxlen, "rogue.sav");
#endif
}

int rogue_load(void)
{
    char path[256];
    FILE *f;
    unsigned char buf[ROGUE_MAGIC_LEN + sizeof(rogue_data_t)];

    rogue_build_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) {
        /* No save file — start fresh */
        memset(&rogue_data, 0, sizeof(rogue_data));
        return -1;
    }

    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        fclose(f);
        memset(&rogue_data, 0, sizeof(rogue_data));
        return -1;
    }
    fclose(f);

    /* De-obfuscate */
    rogue_xor(buf, sizeof(buf));

    /* Validate magic */
    if (memcmp(buf, ROGUE_MAGIC, ROGUE_MAGIC_LEN) != 0) {
        memset(&rogue_data, 0, sizeof(rogue_data));
        return -1;
    }

    memcpy(&rogue_data, buf + ROGUE_MAGIC_LEN, sizeof(rogue_data_t));
    return 0;
}

int rogue_save(void)
{
    char path[256];
    FILE *f;
    unsigned char buf[ROGUE_MAGIC_LEN + sizeof(rogue_data_t)];

    rogue_build_path(path, sizeof(path));

    /* Build buffer: magic + data */
    memcpy(buf, ROGUE_MAGIC, ROGUE_MAGIC_LEN);
    memcpy(buf + ROGUE_MAGIC_LEN, &rogue_data, sizeof(rogue_data_t));

    /* Obfuscate */
    rogue_xor(buf, sizeof(buf));

    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
    return 0;
}
