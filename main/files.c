#include "files.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "app_state.h"
#include "board.h"
#include "sd_card.h"

static const char *TAG = "files";
static file_entry_t s_entries[FILES_MAX];
static int s_count;
static int s_dirs;
static uint32_t s_generation;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static int cmp_entry(const void *a, const void *b)
{
    const file_entry_t *ea = a;
    const file_entry_t *eb = b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void scan_once(void)
{
    app_state_t st;
    state_snapshot(&st);
    if (!st.fs_mounted || st.host_active) return;

    static file_entry_t tmp[FILES_MAX];
    int n = 0;
    int dirs = 0;

    sd_card_lock();
    state_snapshot(&st);
    if (!st.fs_mounted || st.host_active) {
        sd_card_unlock();
        return;
    }
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        sd_card_unlock();
        ESP_LOGW(TAG, "opendir failed");
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < FILES_MAX) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strncasecmp(de->d_name, "SYSTEM~", 7) == 0) continue;
        if (strcasecmp(de->d_name, "System Volume Information") == 0) continue;
        if (strcasecmp(de->d_name, "$RECYCLE.BIN") == 0) continue;
        if (strlen(de->d_name) > 200) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%.200s", SD_MOUNT_POINT, de->d_name);
        struct stat stbuf;
        if (stat(path, &stbuf) != 0) continue;

        file_entry_t *e = &tmp[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%.63s", de->d_name);
        e->is_dir = S_ISDIR(stbuf.st_mode);
        e->size = e->is_dir ? 0 : (uint64_t)stbuf.st_size;
        if (e->is_dir) dirs++;
        n++;
    }
    closedir(dir);
    sd_card_unlock();

    qsort(tmp, n, sizeof(tmp[0]), cmp_entry);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_entries, tmp, sizeof(tmp[0]) * n);
    s_count = n;
    s_dirs = dirs;
    s_generation++;
    xSemaphoreGive(s_lock);

    state_set_counts(n - dirs, dirs);
    ESP_LOGI(TAG, "scanned %d entries (%d dirs)", n, dirs);
}

static void files_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2500));
        scan_once();
    }
}

void files_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(files_task, "files", 4096, NULL, 3, &s_task);
}

void files_request_scan(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

uint32_t files_generation(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_generation;
    xSemaphoreGive(s_lock);
    return v;
}

int files_snapshot(file_entry_t *out, int max, int *dir_count)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_entries, sizeof(out[0]) * n);
    if (dir_count) *dir_count = s_dirs;
    xSemaphoreGive(s_lock);
    return n;
}
