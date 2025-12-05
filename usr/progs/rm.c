#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that deletes the specified files from the filesystem.

void main(int argc, char* argv[]) {
    if (argc < 2) {
        _write(CONSOLEOUT, "Usage: rm <filename1> <filename2> ...\n", 38);
        return;
    }

    for (int i = 1; i < argc; i++) {
        char* filename = argv[i];
        int result = _fsdelete(filename);
        if (result < 0) {
            _write(CONSOLEOUT, "Error: Unable to delete file.\n", 30);
        }
    }
}
