#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that outputs its command line arguments to STDOUT, separated by spaces, followed by a
// newline. Since you are not required to implement any special shell features (like quotes or
// escape characters), you can assume that each argument is separated by a single space.

void main(int argc, char* argv[]) {
    // Ignore argv[0] (the program name)
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            _write(STDOUT, " ", 1);
        }
        _write(STDOUT, argv[i], strlen(argv[i]));
    }
    _write(STDOUT, "\n", 1);
}
