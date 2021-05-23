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
                printf("config usage: [info|default|dd|"
                "scanner blank/verbose/itvl/period/duration|"
                "advertiser blank/verbose/duration/max_events/itvl_min/itvl_max]\n");
            } else {
                printf("config usage: [info|default|"
                "scanner blank/verbose/itvl/period/duration|"
                "advertiser blank/verbose/duration/max_events/itvl_min/itvl_max]\n");
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

        bool none = true;
        /* scanner toggle or activate verbose */
        if (memcmp(argv[2], "scanner", 7) == 0) {
                if (argc == 4) { /* verbose */
                    if (memcmp(argv[3], "verbose", 7) == 0) {
                        config->scanner_verbose = !config->scanner_verbose;
                        if (config->scanner_verbose) {
                            printf("Config: set scanner verbose\n");
                        } else { printf("Config: set scanner not verbose\n"); }
                        none = false;
                        return 0;
                    }
                }

                bool changed = false;
                if (argc == 5) /* itvl, window, period, duration */
                {
                    if (memcmp(argv[3], "itvl", 4) == 0) {
                        config->scanner_itvl = atoi(argv[4]);
                        changed = true;
                        none = false;
                    }
                    if (memcmp(argv[3], "window", 6) == 0) {
                        config->scanner_window = atoi(argv[4]);
                        changed = true;
                        none = false;
                    }
                    if (memcmp(argv[3], "period", 6) == 0) {
                        config->scanner_period = atoi(argv[4]);
                        changed = true;
                        none = false;
                    }
                    if (memcmp(argv[3], "duration", 8) == 0) {
                        config->scanner_duration = atoi(argv[4]);
                        changed = true;
                        none = false;
                    }
                }

                if (none) { /* toggle */
                    config->scanner_enable = !config->scanner_enable;
                    if (config->scanner_enable) {
                        printf("Config: set scanner enabled\n");
                    } else { printf("Config: set scanner disabled\n"); }
                    changed = true;
                }
                if (changed) {
                        jelling_restart_scanner();
                }
                return 0;

        }

        /* advertiser toggle or activate verbose */
        none = true;
        if (memcmp(argv[2], "advertiser", 10) == 0) {
            if (argc == 4) { /* verbose */
                if (memcmp(argv[3], "verbose", 7) == 0) {
                    config->advertiser_verbose = !config->advertiser_verbose;
                    if (config->advertiser_verbose) {
                        printf("Config: set advertiser verbose\n");
                    } else { printf("Config: set advertiser not verbose\n"); }
                    return 0;
                }
            }

            bool changed = false;
            if (argc == 5) { /* duration, max events, itvl min, itvl max */
                if (memcmp(argv[3], "duration", 8) == 0) {
                    config->advertiser_duration = atoi(argv[4]);
                    changed = true;
                    none = false;
                }
                if (memcmp(argv[3], "itvl_min", 8) == 0) {
                    config->advertiser_itvl_min = atoi(argv[4]);
                    changed = true;
                    none = false;
                }
                if (memcmp(argv[3], "itvl_max", 8) == 0) {
                    config->advertiser_itvl_max = atoi(argv[4]);
                    changed = true;
                    none = false;
                }
                if (memcmp(argv[3], "max_events", 10) == 0) {
                    config->advertiser_max_events = atoi(argv[4]);
                    changed = true;
                    none = false;
                }
            }

            if (none) { /* toggle */
                config->advertiser_enable = !config->advertiser_enable;
                if (config->advertiser_enable) {
                    printf("Config: set advertising enabled\n");
                } else { printf("Config: set advertising disabled\n"); }
                changed = true;
            }
            if (changed) {
                jelling_restart_advertiser();
            }
            return 0;
        }
        /* else */
        printf("config usage: [info|scanner blank/verbose|advertiser blank/verbose|"
            "default]\n");
    }

    if (memcmp(argv[1], "scanner", 7) == 0) {
        _cmd_only_scanner();
    }
    return 0;
}
