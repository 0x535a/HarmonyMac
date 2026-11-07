/* Static temporary fixture helper: bounded logs and explicit child teardown. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "stop")) {
        for (int n = 2; n < argc; ++n) {
            char *end;
            long pid = strtol(argv[n], &end, 10);
            if (*end || pid <= 1) return 2;
            kill((pid_t)pid, SIGKILL);
        }
        return 0;
    }
    if (strcmp(argv[1], "dump")) return 2;
    FILE *file = fopen(argv[2], "r");
    if (!file) { perror(argv[2]); return 1; }
    char buffer[4096];
    size_t count, left = 65536;
    while (left && (count = fread(buffer, 1, left < sizeof(buffer) ? left : sizeof(buffer), file))) {
        fwrite(buffer, 1, count, stdout); left -= count;
    }
    fclose(file); return 0;
}
