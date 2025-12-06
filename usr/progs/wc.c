#include "shell.h"
#include "string.h"
#include "syscall.h"

// A program that reads from the first command line argument file and outputs the number of lines,
// words, and bytes in the file to STDOUT, separated by tabs. If no file is specified, it should
// read from STDIN.

void main(int argc, char* argv[]) {
    int fd;

    if (argc < 2) {
        fd = STDIN;
    } else {
        char* filename = argv[1];
        fd = _open(-1, filename);
        if (fd < 0) {
            dprintf(CONSOLEOUT, "Error: Unable to open file.\n");
            return;
        }
    }

    char buffer;
    long bytes_read;
    unsigned long line_count = 0;
    unsigned long word_count = 0;
    unsigned long byte_count = 0;  // Technically unnecessary (GETEND), but oh well
    int in_word = 0;

    while (1) {
        bytes_read = _read(fd, &buffer, 1);
        if (bytes_read < 0) {
            dprintf(CONSOLEOUT, "Error: Unable to read file.\n");
            _close(fd);
            return;
        }
        if (bytes_read == 0) {
            break; // End of file
        }

        byte_count++;

        if (buffer == '\n') {
            line_count++;
        }

        if (buffer == ' ' || buffer == '\n' || buffer == '\t') {
            if (in_word) {
                in_word = 0;
                word_count++;
            }
        } else {
            in_word = 1;
        }
    }

    // If we ended while still in a word, count it
    if (in_word) {
        word_count++;
    }

    _close(fd);

    // Output results
    // Note: max file size is 16844800 bytes, so max string length is 8 + 1 + 8 + 1 + 8 + 1 = 27
    dprintf(STDOUT, "%lu\t%lu\t%lu\n", line_count, word_count, byte_count);

    return;
}
