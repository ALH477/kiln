/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host packer: build a StreamDB from files on disk, using the upstream C lib. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "streamdb.h"

static int put_file(StreamDB *db, const char *key, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return -1; }
    fclose(f);
    StreamDBStatus s = streamdb_insert(db, (const unsigned char*)key, strlen(key), buf, n);
    free(buf);
    if (s != STREAMDB_OK) { fprintf(stderr, "insert %s failed: %d\n", key, s); return -1; }
    printf("  packed %-28s %ld bytes\n", key, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || (argc % 2) != 0) {
        fprintf(stderr, "usage: pack <out.streamdb> <key> <file> [<key> <file>...]\n");
        return 2;
    }
    remove(argv[1]);
    StreamDB *db = streamdb_init(argv[1], 0);
    if (!db) { fprintf(stderr, "streamdb_init failed\n"); return 1; }
    for (int i = 2; i + 1 < argc; i += 2)
        if (put_file(db, argv[i], argv[i+1]) != 0) return 1;
    if (streamdb_flush(db) != STREAMDB_OK) { fprintf(stderr, "flush failed\n"); return 1; }
    streamdb_free(db);
    printf("wrote %s\n", argv[1]);
    return 0;
}
