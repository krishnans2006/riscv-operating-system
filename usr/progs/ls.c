#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that lists all files in the specified “directory” ("c" for ktfs, "dev" for devices, or
// "/" for root) to STDOUT, one per line.

void main(int argc, char* argv[]) {
    if (argc < 2) {
        _write(CONSOLEOUT, "Usage: ls <directory>\n", 22);
        return;
    }

    const char* directory = argv[1];
    
    // Ignore leading and trailing slashes
    int start_index = 0;
    int end_index = strlen(directory) - 1;
    if (directory[start_index] == '/') {
        start_index++;
    }
    if (directory[end_index] == '/') {
        end_index--;
    }
    char dir_name[16];
    int dir_len = end_index - start_index + 1;
    strncpy(dir_name, &directory[start_index], dir_len);
    dir_name[dir_len] = '\0';

    // "" = list mountpoints
    if (strcmp(dir_name, "") == 0) {
        _write(STDOUT, "c\n", 2);
        _write(STDOUT, "dev\n", 4);
        return;
    }

    int fd = _open(-1, dir_name);
    if (fd < 0) {
        _write(CONSOLEOUT, "Error: Unable to open directory.\n", 33);
        return;
    }

    char buffer[15];
    long bytes_read;
    while (1) {
        bytes_read = _read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            _write(CONSOLEOUT, "Error: Unable to read directory.\n", 33);
            _close(fd);
            return;
        }
        if (bytes_read == 0) {
            // End of listing
            break;
        }
        _write(STDOUT, buffer, bytes_read);
        _write(STDOUT, "\n", 1);
    }

    _close(fd);
}
