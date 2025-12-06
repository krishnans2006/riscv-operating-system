#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that creates empty files with the specified names in the filesystem.

void main(int argc, char* argv[]) {
    if (argc < 2) {
        dprintf(CONSOLEOUT, "Usage: touch <filename1> <filename2> ...\n", 41);
        return;
    }

    for (int i = 1; i < argc; i++) {
        char* filename = argv[i];
        int result = _fscreate(filename);
        if (result < 0) {
            dprintf(CONSOLEOUT, "Error: Unable to create a file.\n", 32);
        }
    }
}
