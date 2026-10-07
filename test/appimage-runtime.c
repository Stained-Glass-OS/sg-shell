/* A stand-in AppImage runtime for test/appimage-check.sh: an ELF program
 * that, like the real type 2 runtime with APPIMAGE_EXTRACT_AND_RUN=1,
 * unpacks the SquashFS appended to it (unsquashfs -o: just past its own
 * section headers) and runs its AppRun with the arguments it was given.
 * It notes whether it was asked to extract (no FUSE) in $APPDIR's AppRun
 * environment (SG_GATE_EXTRACT). Never installed; the gate builds it.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include <elf.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char self[PATH_MAX], dir[] = "/tmp/sg-gate-appimage.XXXXXX", root[PATH_MAX + 32], off[32], apprun[PATH_MAX + 64];
    Elf64_Ehdr eh;
    FILE *f;
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    pid_t pid;
    int st;
    char **args;
    if (n <= 0) return 127;
    self[n] = 0;
    if (!(f = fopen(self, "rb")) || fread(&eh, sizeof(eh), 1, f) != 1) return 127;
    fclose(f);
    snprintf(off, sizeof(off), "%llu", (unsigned long long)(eh.e_shoff + (unsigned long long)eh.e_shentsize * eh.e_shnum));
    if (!mkdtemp(dir)) return 127;
    snprintf(root, sizeof(root), "%s/root", dir);
    if (!(pid = fork())) {
        execlp("unsquashfs", "unsquashfs", "-q", "-n", "-o", off, "-d", root, self, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) return 127;
    snprintf(apprun, sizeof(apprun), "%s/AppRun", root);
    setenv("APPDIR", root, 1);
    setenv("APPIMAGE", self, 1);
    setenv("SG_GATE_EXTRACT", getenv("APPIMAGE_EXTRACT_AND_RUN") ? "1" : "0", 1);
    args = calloc(argc + 1, sizeof(*args));
    args[0] = apprun;
    memcpy(args + 1, argv + 1, (argc - 1) * sizeof(*args));
    execv(apprun, args);
    return 127;
}
