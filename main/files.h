#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FILES_MAX 48
#define FILES_NAME_MAX 64

typedef struct {
    char name[FILES_NAME_MAX];
    uint64_t size;
    bool is_dir;
} file_entry_t;

void files_init(void);
void files_request_scan(void);
uint32_t files_generation(void);
int files_snapshot(file_entry_t *out, int max, int *dir_count);
