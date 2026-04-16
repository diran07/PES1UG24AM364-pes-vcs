// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
void index_status(const Index *idx) {
    // Staged changes: compare index vs HEAD commit
    printf("Staged changes:\n");
    if (idx->count == 0) {
        printf("    (nothing to show)\n");
    } else {
        for (size_t i = 0; i < idx->count; i++) {
            printf("    new file:   %s\n", idx->entries[i].path);
        }
    }

    // Unstaged changes: compare working dir vs index
    printf("\nUnstaged changes:\n");
    int unstaged = 0;
    for (size_t i = 0; i < idx->count; i++) {
        struct stat st;
        if (stat(idx->entries[i].path, &st) != 0) {
            printf("    deleted:    %s\n", idx->entries[i].path);
            unstaged++;
        } else if ((time_t)st.st_mtime != idx->entries[i].mtime ||
                   (size_t)st.st_size  != idx->entries[i].size) {
            printf("    modified:   %s\n", idx->entries[i].path);
            unstaged++;
        }
    }
    if (!unstaged) printf("    (nothing to show)\n");

    printf("\nUntracked files:\n");
    // (Basic version: skip for now or list files not in index)
    printf("    (nothing to show)\n");
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *idx) {
    idx->entries = NULL;
    idx->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // Not an error — empty index

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;

        IndexEntry e = {0};
        char hash_hex[HASH_HEX_SIZE + 1];
        char path[512];
        long mtime;
        size_t size;

        if (sscanf(line, "%o %64s %ld %zu %511s",
                   &e.mode, hash_hex, &mtime, &size, path) != 5) continue;

        hex_to_hash(hash_hex, &e.id);
        e.mtime = (time_t)mtime;
        e.size  = size;
        e.path  = strdup(path);

        idx->entries = realloc(idx->entries, (idx->count + 1) * sizeof(IndexEntry));
        idx->entries[idx->count++] = e;
    }
    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
static int path_cmp(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}

int index_save(const Index *idx) {
    // Sort entries by path
    IndexEntry *sorted = malloc(idx->count * sizeof(IndexEntry));
    memcpy(sorted, idx->entries, idx->count * sizeof(IndexEntry));
    qsort(sorted, idx->count, sizeof(IndexEntry), path_cmp);

    // Write to temp file
    char tmp[] = ".pes/index.tmp";
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(sorted); return -1; }

    FILE *f = fdopen(fd, "w");
    for (size_t i = 0; i < idx->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted[i].id, hex);
        fprintf(f, "%o %s %ld %zu %s\n",
                sorted[i].mode, hex,
                (long)sorted[i].mtime, sorted[i].size,
                sorted[i].path);
    }
    fflush(f);
    fsync(fd);
    fclose(f);
    free(sorted);

    return rename(tmp, INDEX_FILE);
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *idx, const char *path) {
    // Read the file from working directory
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    // Write blob to object store
    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data); return -1;
    }
    free(data);

    // Get file metadata
    struct stat st;
    stat(path, &st);

    // Check if entry already exists in index; update or add
    IndexEntry *existing = index_find(idx, path);
    if (existing) {
        existing->id    = id;
        existing->mtime = st.st_mtime;
        existing->size  = size;
        existing->mode  = 0100644;
    } else {
        idx->entries = realloc(idx->entries, (idx->count + 1) * sizeof(IndexEntry));
        IndexEntry *e = &idx->entries[idx->count++];
        e->path  = strdup(path);
        e->id    = id;
        e->mode  = 0100644;
        e->mtime = st.st_mtime;
        e->size  = size;
    }
    return 0;
}
