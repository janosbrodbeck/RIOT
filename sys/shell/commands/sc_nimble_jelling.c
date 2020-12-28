#include <stdio.h>
#include <stdlib.h>

#include "fmt.h"
#include "xtimer.h"
#include "nimble_riot.h"
#include "jelling.h"

static void _cmd_info(void)
{
    jelling_print_info();
}

static void _cmd_start(void)
{
    jelling_start();
}

static void _cmd_stop(void)
{
    jelling_stop();
}

static int _ishelp(char *argv)
{
    return memcmp(argv, "help", 4) == 0;
}

static void _cmd_only_scanner(void) {
    printf("Configured Jelling as BLE scanner\n");
    jelling_config_t *config = jelling_get_config();
    config->advertiser_enable = false;
    config->scanner_verbose = true;
}

int _nimble_jelling_handler(int argc, char **argv)
{
    if ((argc == 1) || _ishelp(argv[1])) {
        printf("usage: %s [help|info|start|stop|config|scanner]\n", argv[0]);
        return 0;
    }
    if (memcmp(argv[1], "info", 4) == 0) {
        _cmd_info();
    }
    if (memcmp(argv[1], "start", 4) == 0) {
        _cmd_start();
    }
    if (memcmp(argv[1], "stop", 4) == 0) {
        _cmd_stop();
    }
    if (memcmp(argv[1], "config", 6) == 0) {
        if (argc == 2) {
            if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
                printf("config usage: [info|default|icmp|dd|filter {ADDR}/clear|scanner blank/verbose|"
                "advertiser blank/verbose]\n");
            } else {
                printf("config usage: [info|default|icmp|filter {ADDR}/clear|scanner blank/verbose|"
                "advertiser blank/verbose]\n");
            }
            return 0;
        }

        if (memcmp(argv[2], "info", 4) == 0) {
            jelling_print_config();
            return 0;
        }

        if (memcmp(argv[2], "default", 7) == 0) {
            jelling_load_default_config();
            printf("Config: default values loaded\n");
            return 0;
        }

        jelling_config_t *config = jelling_get_config();
        if (IS_ACTIVE(JELLING_DUPLICATE_DETECTION_FEATURE_ENABLE)) {
            /* toggle duplicate detection */
            if (memcmp(argv[2], "dd", 2) == 0) {
                config->duplicate_detection_enable = !config->duplicate_detection_enable;
                if (config->duplicate_detection_enable) {
                    printf("Config: set duplicate detection enabled\n");
                } else { printf("Config: set duplicate detection disabled\n"); }
                return 0;
            }
        }

        /* toggle icmp */
        if (memcmp(argv[2], "icmp", 4) == 0) {
            config->advertiser_block_icmp = !config->advertiser_block_icmp;
            if (config->advertiser_block_icmp) {
                printf("Config: set ICMP packets blocked\n");
            } else { printf("Config: set ICMP packets not blocked\n"); }
            return 0;
        }

        /* scanner toggle or activate verbose */
        if (memcmp(argv[2], "scanner", 7) == 0) {
                if (argc == 4) { /* verbose */
                    if (memcmp(argv[3], "verbose", 7) == 0) {
                        config->scanner_verbose = !config->scanner_verbose;
                        if (config->scanner_verbose) {
                            printf("Config: set scanner verbose\n");
                        } else { printf("Config: set scanner not verbose\n"); }
                        return 0;
                    }
                } else { /* toggle */
                    config->scanner_enable = !config->scanner_enable;
                    if (config->scanner_enable) {
                        printf("Config: set scanner enabled\n");
                    } else { printf("Config: set scanner disabled\n"); }
                    return 0;
                }

        }

        /* advertiser toggle or activate verbose */
        if (memcmp(argv[2], "advertiser", 10) == 0) {
            if (argc == 4) { /* verbose */
                if (memcmp(argv[3], "verbose", 7) == 0) {
                    config->advertiser_verbose = !config->advertiser_verbose;
                    if (config->advertiser_verbose) {
                        printf("Config: set advertiser verbose\n");
                    } else { printf("Config: set advertiser not verbose\n"); }
                    return 0;
                }
            } else { /* toggle */
                config->advertiser_enable = !config->advertiser_enable;
                if (config->advertiser_enable) {
                    printf("Config: set advertising enabled\n");
                } else { printf("Config: set advertising disabled\n"); }
                return 0;
            }
        }

        if (argc == 4) {
            if (memcmp(argv[2], "filter", 6) == 0) {
                if (memcmp(argv[3], "clear", 5) == 0) {
                    jelling_filter_clear();
                    printf("Config: filter list cleared\n");
                    return 0;
                }
                /* add new address */
                if (jelling_filter_add(argv[3]) == 0) {
                    printf("Config: new address filter applied\n");
                } else {
                    printf("Config: could not apply address filter\n");
                }
                return 0;
            }
        }

        /* else */
        printf("config usage: [info|scanner blank/verbose|advertiser blank/verbose|"
            "icmp|filter {}|default]\n");
    }

    if (memcmp(argv[1], "scanner", 7) == 0) {
        _cmd_only_scanner();
    }
    return 0;
}
