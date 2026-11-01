#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>

int main(int argc, char *argv[]) {
    char exe_path[PATH_MAX];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        return 1;
    }

    char real_exe[PATH_MAX];
    if (!realpath(exe_path, real_exe)) {
        strncpy(real_exe, exe_path, sizeof(real_exe) - 1);
        real_exe[sizeof(real_exe) - 1] = '\0';
    }

    char *dir = dirname(real_exe);
    char shim_path[PATH_MAX];
    snprintf(shim_path, sizeof(shim_path), "%s/libds4_shim.dylib", dir);

    const char *game_bin = "/Applications/Devotion.app/Contents/MacOS/Devotion";

    /* Redirect stdout/stderr to /tmp/ds4_shim_launch.log when launched from Finder/Dock */
    freopen("/tmp/ds4_shim_launch.log", "w", stdout);
    freopen("/tmp/ds4_shim_launch.log", "a", stderr);

    setenv("DYLD_INSERT_LIBRARIES", shim_path, 1);

    /* Strip macOS Finder -psn_* argument if present, and set argv[0] to game_bin */
    char **new_argv = (char **)calloc((size_t)argc + 2, sizeof(char *));
    if (!new_argv) return 1;

    new_argv[0] = (char *)game_bin;
    int out_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-psn_", 5) == 0) {
            continue;
        }
        new_argv[out_idx++] = argv[i];
    }
    new_argv[out_idx] = NULL;

    execv(game_bin, new_argv);
    perror("execv failed");
    return 1;
}
