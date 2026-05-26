/* linux_app 入口骨架：旧协议链路已删除，等待按 anyMSG 新架构重建。 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"

static void print_usage(const char *program)
{
    printf("Usage: %s [--config PATH] [--status-dir DIR] [--disable-status]\n", program);
    printf("  --config PATH      INI config path, default linux_app/config/device_config.ini if present\n");
    printf("  --status-dir DIR   status snapshot directory, default /run/put/status\n");
    printf("  --disable-status   do not write status snapshot files\n");
}

static int find_config_arg(int argc, char **argv, const char **out_path, bool *out_explicit)
{
    *out_path = APP_CONFIG_DEFAULT_PATH;
    *out_explicit = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0) {
            if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --config\n");
                return -1;
            }
            *out_path = argv[++i];
            *out_explicit = true;
        }
    }

    return 0;
}

static int apply_cli_overrides(int argc, char **argv, app_config_t *config)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        }

        if (strcmp(argv[i], "--config") == 0) {
            ++i;
            continue;
        }

        if (strcmp(argv[i], "--status-dir") == 0) {
            if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --status-dir\n");
                return -1;
            }
            (void)snprintf(config->status_dir, sizeof(config->status_dir), "%s", argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--disable-status") == 0) {
            config->status_enabled = false;
            continue;
        }

        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    app_config_t config;
    const char *config_path;
    bool config_explicit;
    int cli_rc;
    unified_error_t err;

    if (find_config_arg(argc, argv, &config_path, &config_explicit) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    app_config_set_defaults(&config);
    if (config_explicit || (access(config_path, R_OK) == 0)) {
        err = app_config_load_file(&config, config_path);
        if (err != UNIFIED_OK) {
            fprintf(stderr, "failed to load config: %s\n", config_path);
            return EXIT_FAILURE;
        }
    }

    cli_rc = apply_cli_overrides(argc, argv, &config);
    if (cli_rc > 0) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (cli_rc < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (app_config_validate(&config) != UNIFIED_OK) {
        fprintf(stderr, "invalid linux_app config\n");
        return EXIT_FAILURE;
    }

    printf("linux_app skeleton: legacy unified_frame pipeline removed; waiting for anyMSG rebuild.\n");
    return EXIT_SUCCESS;
}
