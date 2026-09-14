#include "file_list.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char *TAG = "FILE_LIST";

static char **paths = NULL;
static size_t count = 0;
static size_t capacity = 0;
static size_t current = 0;

static const char *exts[] = {".mp3", ".wav", ".flac", ".aac", NULL};

static bool is_audio_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {0};
    size_t i = 0;
    for (const char *p = dot; *p && i < 7; p++, i++) {
        ext[i] = tolower((unsigned char)*p);
    }
    for (size_t j = 0; exts[j]; j++) {
        if (strcmp(ext, exts[j]) == 0) return true;
    }
    return false;
}

static bool ensure_capacity(size_t needed)
{
    if (capacity >= needed) return true;
    size_t new_cap = capacity == 0 ? 16 : capacity * 2;
    while (new_cap < needed) new_cap *= 2;
    char **new_paths = heap_caps_realloc(paths, new_cap * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!new_paths) {
        ESP_LOGE(TAG, "realloc failed (%zu entries)", new_cap);
        return false;
    }
    paths = new_paths;
    capacity = new_cap;
    return true;
}

static void add_path(const char *dir, const char *name)
{
    if (!ensure_capacity(count + 1)) return;
    char full[MAX_FILE_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    paths[count] = heap_caps_malloc(strlen(full) + 1, MALLOC_CAP_SPIRAM);
    if (paths[count]) {
        strcpy(paths[count], full);
        count++;
    }
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open dir: %s", dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[MAX_FILE_PATH_LEN + 4];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                scan_dir(full);
            } else if (S_ISREG(st.st_mode) && is_audio_file(ent->d_name)) {
                add_path(dir, ent->d_name);
            }
        }
    }
    closedir(d);
}

esp_err_t file_list_scan(const char *base_path)
{
    file_list_free();
    scan_dir(base_path);

    size_t tmp = 0;
    for (size_t i = 0; i < count; i++) {
        if (paths[i]) paths[tmp++] = paths[i];
    }
    count = tmp;

    qsort(paths, count, sizeof(char *), (int (*)(const void *, const void *))strcmp);

    ESP_LOGI(TAG, "scanned %zu audio files from %s", count, base_path);
    return count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t file_list_count(void) { return count; }

const char *file_list_get(size_t index)
{
    if (index >= count) return NULL;
    return paths[index];
}

void file_list_next(void)
{
    if (count == 0) return;
    current = (current + 1) % count;
}

void file_list_prev(void)
{
    if (count == 0) return;
    current = (current - 1 + count) % count;
}

void file_list_set_current(size_t index)
{
    if (count == 0 || index >= count) return;
    current = index;
}

const char *file_list_current(void)
{
    if (count == 0) return NULL;
    return paths[current];
}

size_t file_list_current_index(void) { return current; }

void file_list_free(void)
{
    for (size_t i = 0; i < count; i++) {
        if (paths[i]) heap_caps_free(paths[i]);
    }
    heap_caps_free(paths);
    paths = NULL;
    count = 0;
    capacity = 0;
    current = 0;
}
