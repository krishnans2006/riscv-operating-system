#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that lists all files in the specified “directory” ("c" for ktfs, "dev" for devices, or
// "/" for root) to STDOUT, one per line.

void main(int argc, char* argv[]) {
    if (argc < 2) {
        dprintf(CONSOLEOUT, "Usage: ls <directory>\n");
        return;
    }

    char* directory = argv[1];
    
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
        dprintf(STDOUT, "c\n");
        dprintf(STDOUT, "dev\n");
        return;
    }

    int fd = _open(-1, dir_name);
    if (fd < 0) {
        dprintf(CONSOLEOUT, "Error: Unable to open directory.\n");
        return;
    }

    char buffer[15];
    long bytes_read;
    while (1) {
        bytes_read = _read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            dprintf(CONSOLEOUT, "Error: Unable to read directory.\n");
            _close(fd);
            return;
        }
        if (bytes_read == 0) {
            // End of listing
            break;
        }
        dprintf(STDOUT, "%s\n", buffer);
    }

    _close(fd);

    return;
}
