#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that reads from the first command line argument file and writes its contents to STDOUT.

void main(int argc, char* argv[]) {
    if (argc < 2) {
        _write(CONSOLEOUT, "Usage: cat <filename>\n", 22);
        return;
    }

    char* filename = argv[1];
    int fd = _open(-1, filename);
    if (fd < 0) {
        _write(CONSOLEOUT, "Error: Unable to open file.\n", 28);
        return;
    }

    char buffer[1024];
    long bytes_read;
    do {
        bytes_read = _read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            _write(CONSOLEOUT, "Error: Unable to read file.\n", 28);
            _close(fd);
            return;
        }
        _write(STDOUT, buffer, bytes_read);
    } while (bytes_read > 0);

    // End of file reached

    _close(fd);
}
