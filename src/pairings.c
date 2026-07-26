#include "pairings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET) && defined(__has_include)
#if __has_include("littlefs/lfs.h")
#define PICO_INTERCOM_HAS_LITTLEFS 1
#endif
#endif

#ifndef PICO_INTERCOM_HAS_LITTLEFS
#define PICO_INTERCOM_HAS_LITTLEFS 0
#endif

#if PICO_INTERCOM_HAS_LITTLEFS
#include "littlefs/lfs.h"
#endif

static bool pairing_store_is_duplicate(const pairing_t *pairings, size_t count,
                                       uint8_t peer_id) {
    for (size_t index = 0; index < count; ++index) {
        if (pairings[index].peer_id == peer_id) {
            return true;
        }
    }

    return false;
}

static void pairing_store_trim_newline(char *value) {
    if (value == NULL) {
        return;
    }

    size_t length = strlen(value);
    if (length > 0U && value[length - 1U] == '\n') {
        value[length - 1U] = '\0';
    }
}

static bool pairing_store_copy_name(pairing_t *pairing, const char *name) {
    if (pairing == NULL || name == NULL) {
        return false;
    }

    int written = snprintf(pairing->name, sizeof(pairing->name), "%s", name);
    return written >= 0 && (size_t)written < sizeof(pairing->name);
}

#if !defined(PICO_INTERCOM_TARGET)
static bool pairing_store_read_handle(FILE *handle, pairing_t *pairings, size_t *count) {
    if (handle == NULL || pairings == NULL || count == NULL) {
        return false;
    }

    size_t loaded = 0U;
    char line[PAIRING_LINE_BUFFER_LEN];
    while (loaded < PAIRING_MAX_COUNT && fgets(line, sizeof(line), handle) != NULL) {
        char *separator = strchr(line, ',');
        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        unsigned int peer_id = 0U;
        if (sscanf(line, "%u", &peer_id) != 1 || peer_id > UINT8_MAX) {
            continue;
        }

        char *name = separator + 1U;
        pairing_store_trim_newline(name);

        memset(&pairings[loaded], 0, sizeof(pairings[loaded]));
        pairings[loaded].peer_id = (uint8_t)peer_id;
        if (!pairing_store_copy_name(&pairings[loaded], name)) {
            continue;
        }
        loaded++;
    }

    *count = loaded;
    return true;
}
#endif

#if PICO_INTERCOM_HAS_LITTLEFS
static bool pairing_store_littlefs_save(const pairing_store_t *store, const pairing_t *pairing) {
    (void)store;
    (void)pairing;
    return false;
}

static bool pairing_store_littlefs_load(const pairing_store_t *store, pairing_t *pairings,
                                        size_t *count) {
    (void)store;
    (void)pairings;
    (void)count;
    return false;
}

static bool pairing_store_littlefs_clear(const pairing_store_t *store) {
    (void)store;
    return false;
}
#endif

bool pairing_store_init(pairing_store_t *store, const char *path) {
    if (store == NULL) {
        return false;
    }

    memset(store, 0, sizeof(*store));
    if (path == NULL || path[0] == '\0') {
        snprintf(store->path, sizeof(store->path), "pairings.txt");
    } else {
        snprintf(store->path, sizeof(store->path), "%s", path);
    }

    store->initialized = true;
    return true;
}

bool pairing_store_save(pairing_store_t *store, const pairing_t *pairing) {
    if (store == NULL || !store->initialized || pairing == NULL) {
        return false;
    }

#if PICO_INTERCOM_HAS_LITTLEFS
    if (pairing_store_littlefs_save(store, pairing)) {
        return true;
    }
#endif

#if defined(PICO_INTERCOM_TARGET)
    (void)store;
    (void)pairing;
    return true;
#else
    FILE *handle = fopen(store->path, "a+");
    if (handle == NULL) {
        return false;
    }

    pairing_t existing[PAIRING_MAX_COUNT] = {{0}};
    size_t count = 0U;
    if (fseek(handle, 0, SEEK_SET) != 0) {
        fclose(handle);
        return false;
    }

#if !defined(PICO_INTERCOM_TARGET)
    if (pairing_store_read_handle(handle, existing, &count) &&
        pairing_store_is_duplicate(existing, count, pairing->peer_id)) {
        fclose(handle);
        return true;
    }
#endif

    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return false;
    }

    if (fprintf(handle, "%u,%s\n", (unsigned)pairing->peer_id, pairing->name) < 0) {
        fclose(handle);
        return false;
    }

    fclose(handle);
    return true;
#endif
}

bool pairing_store_load(pairing_store_t *store, pairing_t *pairings, size_t *count) {
    if (store == NULL || !store->initialized || pairings == NULL || count == NULL) {
        return false;
    }

#if PICO_INTERCOM_HAS_LITTLEFS
    if (pairing_store_littlefs_load(store, pairings, count)) {
        return true;
    }
#endif

#if defined(PICO_INTERCOM_TARGET)
    if (pairings != NULL) {
        memset(pairings, 0, sizeof(pairings[0]) * PAIRING_MAX_COUNT);
    }
    *count = 0U;
    return true;
#else
    FILE *handle = fopen(store->path, "r");
    if (handle == NULL) {
        *count = 0U;
        return errno == ENOENT;
    }

#if !defined(PICO_INTERCOM_TARGET)
    if (!pairing_store_read_handle(handle, pairings, count)) {
        fclose(handle);
        return false;
    }
#endif

    fclose(handle);
    return true;
#endif
}

bool pairing_store_clear(pairing_store_t *store) {
    if (store == NULL || !store->initialized) {
        return false;
    }

#if PICO_INTERCOM_HAS_LITTLEFS
    if (pairing_store_littlefs_clear(store)) {
        return true;
    }
#endif

#if defined(PICO_INTERCOM_TARGET)
    (void)store;
    return true;
#else
    if (remove(store->path) != 0) {
        return errno == ENOENT;
    }

    return true;
#endif
}
