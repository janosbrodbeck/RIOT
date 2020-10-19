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

int _nimble_jelling_handler(int argc, char **argv)
{
    if ((argc == 1) || _ishelp(argv[1])) {
        printf("usage: %s [help|info|start|stop]\n", argv[0]);
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
    return 0;
}
