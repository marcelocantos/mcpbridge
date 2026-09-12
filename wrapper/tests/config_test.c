/* Copyright 2026 Marcelo Cantos
 * SPDX-License-Identifier: Apache-2.0 */

/* Shared config-validation corpus, wrapper side.
 *
 * The wrapper (src/config.c) and the daemon (daemon/internal/config)
 * are two independent implementations of one schema. Nothing in the
 * build used to force them to agree, and they had already drifted:
 * the daemon accepted `http://[::1]/mcp`, the wrapper refused to
 * start on it.
 *
 * ../testdata/config-validation/cases.json is the single verdict
 * table. This test and daemon/internal/config/corpus_test.go both
 * read it. A case added there without fixing both validators turns
 * the build red — that is the point. */

#include "../src/config.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Corpus lives at the repo root; the wrapper's tests run with cwd
 * set to wrapper/, so one level up. */
#define CORPUS_DIR "../testdata/config-validation"

static int fail_count = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            fail_count++;                                                  \
        }                                                                  \
    } while (0)

/* read_file slurps path into a NUL-terminated heap buffer, or returns
 * NULL. Caller frees. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int main(void) {
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/cases.json", CORPUS_DIR);
    char *manifest_text = read_file(manifest_path);
    if (manifest_text == NULL) {
        fprintf(stderr, "FAIL: cannot read corpus manifest %s\n", manifest_path);
        return 1;
    }
    cJSON *manifest = cJSON_Parse(manifest_text);
    free(manifest_text);
    if (manifest == NULL) {
        fprintf(stderr, "FAIL: corpus manifest is not valid JSON\n");
        return 1;
    }
    cJSON *cases = cJSON_GetObjectItemCaseSensitive(manifest, "cases");
    if (!cJSON_IsArray(cases) || cJSON_GetArraySize(cases) == 0) {
        fprintf(stderr, "FAIL: corpus manifest has no cases\n");
        cJSON_Delete(manifest);
        return 1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cases) {
        cJSON *file = cJSON_GetObjectItemCaseSensitive(item, "file");
        cJSON *valid = cJSON_GetObjectItemCaseSensitive(item, "valid");
        if (!cJSON_IsString(file) || !cJSON_IsBool(valid)) {
            fprintf(stderr, "FAIL: malformed corpus case entry\n");
            fail_count++;
            continue;
        }
        char config_path[512];
        snprintf(config_path, sizeof(config_path), "%s/configs/%s",
                 CORPUS_DIR, file->valuestring);

        /* config_load writes its own diagnostics to stderr on the
         * reject cases; that noise is expected and harmless. */
        struct config *c = config_load(config_path);
        int accepted = (c != NULL);
        config_free(c);

        int want = cJSON_IsTrue(valid);
        if (accepted != want) {
            fprintf(stderr,
                    "FAIL %s: corpus says %s, wrapper %s\n",
                    file->valuestring,
                    want ? "valid" : "invalid",
                    accepted ? "accepted" : "rejected");
            fail_count++;
        }
    }
    cJSON_Delete(manifest);

    if (fail_count > 0) {
        fprintf(stderr, "config_test: %d failure(s)\n", fail_count);
        return 1;
    }
    printf("config_test: ok\n");
    return 0;
}
