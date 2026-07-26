#include "pairings.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(PICO_INTERCOM_TARGET)
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#define PICO_INTERCOM_HAS_FLASH_STORAGE 1
#else
#define PICO_INTERCOM_HAS_FLASH_STORAGE 0
#endif

#if PICO_INTERCOM_HAS_FLASH_STORAGE
#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif

#define PAIRING_FLASH_MAGIC 0x50495043u
#define PAIRING_FLASH_VERSION 1u
#define PAIRING_FLASH_IMAGE_SIZE 768U
#define PAIRING_FLASH_IMAGE_HEADER_BYTES (sizeof(uint32_t) + sizeof(uint32_t) + \
                                          sizeof(uint32_t) + sizeof(uint32_t) + \
                                          sizeof(pairing_t) * PAIRING_MAX_COUNT)
#define PAIRING_FLASH_REGION_SIZE FLASH_SECTOR_SIZE
#define PAIRING_FLASH_REGION_OFFSET (PICO_FLASH_SIZE_BYTES - PAIRING_FLASH_REGION_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t checksum;
    pairing_t pairings[PAIRING_MAX_COUNT];
    uint8_t padding[PAIRING_FLASH_IMAGE_SIZE - PAIRING_FLASH_IMAGE_HEADER_BYTES];
} pairing_flash_image_t;
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
    while (length > 0U && (value[length - 1U] == '\n' || value[length - 1U] == '\r')) {
        value[length - 1U] = '\0';
        length--;
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

#if PICO_INTERCOM_HAS_FLASH_STORAGE
static uint32_t pairing_store_checksum(const pairing_flash_image_t *image) {
    uint32_t checksum = 0x811C9DC5u;
    const uint8_t *bytes = (const uint8_t *)image;
    const size_t checksum_offset = offsetof(pairing_flash_image_t, checksum);

    for (size_t index = 0; index < sizeof(*image); ++index) {
        if (index >= checksum_offset && index < checksum_offset + sizeof(image->checksum)) {
            continue;
        }

        checksum ^= bytes[index];
        checksum *= 16777619u;
    }

    return checksum;
}

static bool pairing_store_flash_image_is_valid(const pairing_flash_image_t *image) {
    if (image == NULL) {
        return false;
    }

    if (image->magic != PAIRING_FLASH_MAGIC || image->version != PAIRING_FLASH_VERSION) {
        return false;
    }

    if (image->count > PAIRING_MAX_COUNT) {
        return false;
    }

    return image->checksum == pairing_store_checksum(image);
}

static bool pairing_store_flash_read_image(pairing_flash_image_t *image) {
    if (image == NULL) {
        return false;
    }

    const uint8_t *flash_address = (const uint8_t *)(XIP_BASE + PAIRING_FLASH_REGION_OFFSET);
    memcpy(image, flash_address, sizeof(*image));
    return pairing_store_flash_image_is_valid(image);
}

static bool pairing_store_flash_write_image(const pairing_flash_image_t *image) {
    if (image == NULL) {
        return false;
    }

    uint32_t interrupts = save_and_disable_interrupts();
    const int erase_status = flash_range_erase(PAIRING_FLASH_REGION_OFFSET, PAIRING_FLASH_REGION_SIZE);
    const int program_status = flash_range_program(PAIRING_FLASH_REGION_OFFSET,
                                                   (const uint8_t *)image, sizeof(*image));
    restore_interrupts(interrupts);

    if (erase_status != 0 || program_status != 0) {
        fprintf(stderr, "WARNING: flash pairing write failed (erase=%d program=%d)\n",
                erase_status, program_status);
        return false;
    }

    pairing_flash_image_t verified = {0};
    if (!pairing_store_flash_read_image(&verified)) {
        return false;
    }

    return memcmp(&verified, image, sizeof(*image)) == 0;
}

static bool pairing_store_flash_load(const pairing_store_t *store, pairing_t *pairings,
                                     size_t *count) {
    (void)store;
    pairing_flash_image_t image = {0};
    if (!pairing_store_flash_read_image(&image)) {
        if (count != NULL) {
            *count = 0U;
        }
        return true;
    }

    const size_t loaded_count = image.count;
    if (count != NULL) {
        *count = loaded_count;
    }

    if (pairings != NULL) {
        memset(pairings, 0, sizeof(pairings[0]) * PAIRING_MAX_COUNT);
        for (size_t index = 0; index < loaded_count; ++index) {
            pairings[index] = image.pairings[index];
        }
    }

    return true;
}

static bool pairing_store_flash_save(const pairing_store_t *store, const pairing_t *pairing) {
    (void)store;
    if (pairing == NULL) {
        return false;
    }

    pairing_flash_image_t image = {0};
    bool image_loaded = pairing_store_flash_read_image(&image);
    size_t count = 0U;

    if (image_loaded) {
        count = image.count;
    }

    bool found = false;
    for (size_t index = 0; index < count; ++index) {
        if (image.pairings[index].peer_id == pairing->peer_id) {
            image.pairings[index] = *pairing;
            found = true;
            break;
        }
    }

    if (!found && count < PAIRING_MAX_COUNT) {
        image.pairings[count++] = *pairing;
    }

    image.magic = PAIRING_FLASH_MAGIC;
    image.version = PAIRING_FLASH_VERSION;
    image.count = count;
    image.checksum = 0U;
    image.checksum = pairing_store_checksum(&image);
    return pairing_store_flash_write_image(&image);
}

static bool pairing_store_flash_clear(const pairing_store_t *store) {
    (void)store;
    pairing_flash_image_t image = {0};
    return pairing_store_flash_write_image(&image);
}
#endif

bool pairing_store_init(pairing_store_t *store, const char *path) {
    if (store == NULL) {
        return false;
    }

    memset(store, 0, sizeof(*store));

    const char *store_path = path;
    if (store_path == NULL || store_path[0] == '\0') {
        store_path = "pairings.txt";
    }

    int required_length = snprintf(store->path, sizeof(store->path), "%s", store_path);
    if (required_length < 0 || (size_t)required_length >= sizeof(store->path)) {
        store->path[0] = '\0';
        store->initialized = false;
        return false;
    }

    store->initialized = true;
    return true;
}

bool pairing_store_save(pairing_store_t *store, const pairing_t *pairing) {
    if (store == NULL || !store->initialized || pairing == NULL) {
        return false;
    }

#if PICO_INTERCOM_HAS_FLASH_STORAGE
    return pairing_store_flash_save(store, pairing);
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

    if (pairing_store_read_handle(handle, existing, &count) &&
        pairing_store_is_duplicate(existing, count, pairing->peer_id)) {
        fclose(handle);
        return true;
    }

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

#if PICO_INTERCOM_HAS_FLASH_STORAGE
    return pairing_store_flash_load(store, pairings, count);
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

    if (!pairing_store_read_handle(handle, pairings, count)) {
        fclose(handle);
        return false;
    }

    fclose(handle);
    return true;
#endif
}

bool pairing_store_clear(pairing_store_t *store) {
    if (store == NULL || !store->initialized) {
        return false;
    }

#if PICO_INTERCOM_HAS_FLASH_STORAGE
    return pairing_store_flash_clear(store);
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
