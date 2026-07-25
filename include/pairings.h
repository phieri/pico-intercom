#ifndef PAIRINGS_H
#define PAIRINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIRING_MAX_NAME_LEN 64U
#define PAIRING_MAX_COUNT 8U
#define PAIRING_LINE_BUFFER_LEN 128U
#define PAIRING_STORE_PATH_LEN 128U

typedef struct {
    uint8_t peer_id;
    char name[PAIRING_MAX_NAME_LEN];
} pairing_t;

typedef struct {
    char path[PAIRING_STORE_PATH_LEN];
    bool initialized;
} pairing_store_t;

bool pairing_store_init(pairing_store_t *store, const char *path);
bool pairing_store_save(pairing_store_t *store, const pairing_t *pairing);
bool pairing_store_load(pairing_store_t *store, pairing_t *pairings, size_t *count);
bool pairing_store_clear(pairing_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
