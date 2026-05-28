#include "status_collector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ethernet_adapter.h"
#include "shared_memory_ipc.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static put_shm_region_t g_region;

static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

static size_t make_anymsg(uint8_t *buffer)
{
    anymsg_header_t *header;

    memset(buffer, 0, PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, ANYMSG_HEADER_SIZE);
    header->retries = 1u;
    header->destination_cid[0] = 0x20u;
    header->source_cid[0] = 0x40u;
    write_le16(header->payload_length, 0u);
    header->type = ANYMSG_TYPE_RAW_CAN;
    return ANYMSG_HEADER_SIZE;
}

static char *read_file(const char *path)
{
    FILE *fp;
    long len;
    char *buffer;

    fp = fopen(path, "r");
    if (fp == NULL) {
        return NULL;
    }
    if ((fseek(fp, 0, SEEK_END) != 0) || ((len = ftell(fp)) < 0) || (fseek(fp, 0, SEEK_SET) != 0)) {
        (void)fclose(fp);
        return NULL;
    }
    buffer = (char *)calloc((size_t)len + 1u, 1u);
    if (buffer == NULL) {
        (void)fclose(fp);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)len, fp) != (size_t)len) {
        free(buffer);
        (void)fclose(fp);
        return NULL;
    }
    (void)fclose(fp);
    return buffer;
}

static int path_join(char *out, size_t out_size, const char *dir, const char *file)
{
    return (snprintf(out, out_size, "%s/%s", dir, file) < (int)out_size) ? 0 : -1;
}

static int test_status_snapshots_include_modules_and_ipc(void)
{
    char dir[128];
    char path[192];
    char *modules_json;
    char *ipc_json;
    char *route_json;
    status_collector_t collector;
    linux_shm_ipc_t ipc;
    linux_shm_ipc_stats_t stats;
    ethernet_rx_context_t ctx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    size_t frame_len;

    (void)snprintf(dir, sizeof(dir), "/tmp/put_status_collector_test_%ld", (long)getpid());
    (void)mkdir(dir, 0755);

    status_collector_init(&collector, dir, true);
    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        status_collector_configure_module(&collector,
                                          (status_module_id_t)i,
                                          false,
                                          false,
                                          status_module_display_protocol((status_module_id_t)i),
                                          "planned");
    }
    status_collector_configure_module(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      true,
                                      true,
                                      "Ethernet UDP raw",
                                      "test");
    status_collector_mark_running(&collector, STATUS_MODULE_ETHERNET);

    linux_shm_ipc_init(&ipc);
    CHECK(linux_shm_ipc_format_region(&ipc, &g_region, 77u, 0u, NULL) == UNIFIED_OK);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ipc = &ipc;
    ctx.collector = &collector;
    ctx.linux_epoch = 77u;
    frame_len = make_anymsg(frame);
    CHECK(ethernet_adapter_handle_datagram(&ctx, frame, frame_len) == UNIFIED_OK);

    linux_shm_ipc_get_stats(&ipc, &stats);
    status_collector_update_ipc_stats(&collector, &stats, false, 0u);
    CHECK(status_collector_write_all(&collector) == 0);

    CHECK(path_join(path, sizeof(path), dir, "modules.json") == 0);
    modules_json = read_file(path);
    CHECK(modules_json != NULL);
    CHECK(strstr(modules_json, "\"name\":\"ethernet\"") != NULL);
    CHECK(strstr(modules_json, "\"name\":\"can\"") != NULL);
    CHECK(strstr(modules_json, "\"name\":\"wifi\"") != NULL);
    CHECK(strstr(modules_json, "\"name\":\"bluetooth\"") != NULL);
    CHECK(strstr(modules_json, "\"name\":\"4g\"") != NULL);
    CHECK(strstr(modules_json, "\"name\":\"rs485\"") != NULL);
    CHECK(strstr(modules_json, "\"rx_bytes\":40") != NULL);
    CHECK(strstr(modules_json, "\"rx_frames\":1") != NULL);
    free(modules_json);

    CHECK(path_join(path, sizeof(path), dir, "ipc_status.json") == 0);
    ipc_json = read_file(path);
    CHECK(ipc_json != NULL);
    CHECK(strstr(ipc_json, "\"frame_pool\"") != NULL);
    CHECK(strstr(ipc_json, "\"rx_rings\"") != NULL);
    CHECK(strstr(ipc_json, "\"interface\":\"ethernet\"") != NULL);
    CHECK(strstr(ipc_json, "\"pending_bitmap\"") != NULL);
    CHECK(strstr(ipc_json, "\"used\":1") != NULL);
    free(ipc_json);

    CHECK(path_join(path, sizeof(path), dir, "route_status.json") == 0);
    route_json = read_file(path);
    CHECK(route_json != NULL);
    CHECK(strstr(route_json, "\"state\":\"unknown\"") != NULL);
    CHECK(strstr(route_json, "\"priority_queues\":[]") != NULL);
    free(route_json);

    (void)path_join(path, sizeof(path), dir, "modules.json");
    (void)remove(path);
    (void)path_join(path, sizeof(path), dir, "ipc_status.json");
    (void)remove(path);
    (void)path_join(path, sizeof(path), dir, "route_status.json");
    (void)remove(path);
    (void)path_join(path, sizeof(path), dir, "events.jsonl");
    (void)remove(path);
    (void)rmdir(dir);
    status_collector_destroy(&collector);
    return 0;
}

int main(void)
{
    if (test_status_snapshots_include_modules_and_ipc() != 0) {
        return 1;
    }
    puts("status_collector_test: OK");
    return 0;
}
