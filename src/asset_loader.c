// asset_loader.c — .wasc ZIP archive reading and manifest parsing

#include "cart_host.h"

#include "../deps/miniz.h"
#include "../deps/cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ─── Archive open/close ────────────────────────────────────────────────────

static int read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(size);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    fclose(f);
    *out = data;
    *out_len = (size_t)size;
    return 0;
}

int wc_archive_open(wc_host_t* host, const char* path) {
    size_t len = strlen(path);
    bool is_wasc = (len > 5 && strcmp(path + len - 5, ".wasc") == 0);

    if (!is_wasc) {
        // Bare .wasm file
        if (read_file(path, &host->wasm_bytes, &host->wasm_bytes_len) != 0) return -1;

        // Default manifest
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
        return 0;
    }

    // .wasc ZIP archive
    uint8_t* file_data;
    size_t file_size;
    if (read_file(path, &file_data, &file_size) != 0) return -1;

    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_mem(zip, file_data, file_size, 0)) {
        fprintf(stderr, "wasmcart: failed to open ZIP: %s\n", path);
        free(file_data);
        free(zip);
        return -1;
    }
    host->archive = zip;

    // Read manifest.json
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) {
            wc_parse_manifest(host, json, json_size);
            free(json);
        }
    } else {
        // No manifest — use defaults
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
    }

    // Read cart.wasm (or manifest entry)
    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) {
        fprintf(stderr, "wasmcart: entry '%s' not found in archive\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        free(file_data);
        host->archive = NULL;
        return -1;
    }

    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    if (!host->wasm_bytes) {
        fprintf(stderr, "wasmcart: failed to extract %s\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        free(file_data);
        host->archive = NULL;
        return -1;
    }

    // Note: we keep file_data alive because mz_zip_reader_init_mem doesn't copy.
    // The zip reader references it. We'll free it in wc_archive_close.
    // Store it somewhere... for now leak it (TODO: store in host struct).

    return 0;
}

int wc_archive_open_memory(wc_host_t* host, const uint8_t* data, size_t len) {
    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_mem(zip, data, len, 0)) {
        free(zip);
        return -1;
    }
    host->archive = zip;

    // Same flow as above for manifest + wasm extraction
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) { wc_parse_manifest(host, json, json_size); free(json); }
    }

    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) return -1;
    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    return host->wasm_bytes ? 0 : -1;
}

void wc_archive_close(wc_host_t* host) {
    if (host->archive) {
        mz_zip_reader_end((mz_zip_archive*)host->archive);
        free(host->archive);
        host->archive = NULL;
    }
    if (host->wasm_bytes) {
        free(host->wasm_bytes);
        host->wasm_bytes = NULL;
        host->wasm_bytes_len = 0;
    }
}

// ─── Asset loading ─────────────────────────────────────────────────────────

int32_t wc_archive_asset_size(wc_host_t* host, const char* path) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    // Try with and without "assets/" prefix
    int idx = mz_zip_reader_locate_file(zip, path, NULL, 0);
    if (idx < 0) {
        char prefixed[512];
        snprintf(prefixed, sizeof(prefixed), "assets/%s", path);
        idx = mz_zip_reader_locate_file(zip, prefixed, NULL, 0);
    }
    if (idx < 0) return -1;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;
    return (int32_t)stat.m_uncomp_size;
}

int32_t wc_archive_load_asset(wc_host_t* host, const char* path, uint8_t* dest, uint32_t max_size) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    int idx = mz_zip_reader_locate_file(zip, path, NULL, 0);
    if (idx < 0) {
        char prefixed[512];
        snprintf(prefixed, sizeof(prefixed), "assets/%s", path);
        idx = mz_zip_reader_locate_file(zip, prefixed, NULL, 0);
    }
    if (idx < 0) {
        static int _miss = 0;
        if (_miss < 3) { fprintf(stderr, "wasmcart: asset not found: %s\n", path); _miss++; }
        return -1;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;

    uint32_t read_size = (uint32_t)stat.m_uncomp_size;
    if (read_size > max_size) {
        fprintf(stderr, "wasmcart: asset %s: size %u > max %u, truncating\n", path, read_size, max_size);
        read_size = max_size;
    }

    if (!mz_zip_reader_extract_to_mem(zip, idx, dest, read_size, 0)) {
        fprintf(stderr, "wasmcart: asset %s: extract failed\n", path);
        return -1;
    }
    static int _load = 0;
    if (_load < 5) { fprintf(stderr, "wasmcart: loaded asset %s (%u bytes, max_size=%u, uncomp=%u)\n", path, read_size, max_size, (uint32_t)stat.m_uncomp_size); _load++; }
    return (int32_t)read_size;
}

// ─── Manifest parsing ──────────────────────────────────────────────────────

int wc_parse_manifest(wc_host_t* host, const char* json, size_t len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        fprintf(stderr, "wasmcart: failed to parse manifest.json\n");
        return -1;
    }

    cJSON* item;

    item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(item))
        strncpy(host->manifest.name, item->valuestring, sizeof(host->manifest.name) - 1);

    item = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(item))
        strncpy(host->manifest.version, item->valuestring, sizeof(host->manifest.version) - 1);

    item = cJSON_GetObjectItem(root, "abi");
    host->manifest.abi = cJSON_IsNumber(item) ? item->valueint : 2;

    item = cJSON_GetObjectItem(root, "entry");
    if (cJSON_IsString(item))
        strncpy(host->manifest.entry, item->valuestring, sizeof(host->manifest.entry) - 1);
    else
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);

    item = cJSON_GetObjectItem(root, "players");
    host->manifest.players = cJSON_IsNumber(item) ? item->valueint : 1;

    item = cJSON_GetObjectItem(root, "pointer");
    host->manifest.pointer = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "keyboard");
    host->manifest.keyboard = cJSON_IsTrue(item);

    cJSON* net = cJSON_GetObjectItem(root, "net");
    if (cJSON_IsObject(net)) {
        cJSON* ws = cJSON_GetObjectItem(net, "websocket");
        if (cJSON_IsArray(ws)) {
            host->manifest.websocket = true;
            host->manifest.ws_domain_count = 0;
            cJSON* domain;
            cJSON_ArrayForEach(domain, ws) {
                if (cJSON_IsString(domain) && host->manifest.ws_domain_count < 8) {
                    strncpy(host->manifest.ws_domains[host->manifest.ws_domain_count],
                            domain->valuestring, 255);
                    host->manifest.ws_domain_count++;
                }
            }
        }
        item = cJSON_GetObjectItem(net, "data-channel");
        host->manifest.data_channel = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);
    return 0;
}
