#include "shell.h"
#include "string.h"
#include "syscall.h"

#define BUFSIZE 1024
#define MAXARGS 8

char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\n':
			case '\r':
			case '\0':
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

int parse(char* buf, char* argv[MAXARGS + 1]) {
	int argc = 0;

	char *head;
	head = buf;
	
	while(*head) { // find each argv
		// skip spaces and newlines
		while(*head == ' ' || *head == '\n' || *head == '\r') head++;
		if (*head == '\0') break;

		switch(*head) {
			// end of buf
			case '\0':
				break;

			default:	
				// regular argument to store
				argv[argc++] = head;
				head = find_terminator(head);
				if (*head) {
					*head = '\0';
					head++;
				}
		}
	}

	// reach the '\0' at the end

	argv[argc] = NULL;  // null-terminate the current argv
	return argc; 		// returns the number of args
}

// A program that reads items from STDIN, separated by spaces or newlines, and executes the command
// specified in its arguments with those items as additional arguments

void main(int argc, char *argv[]) {
    char buf[BUFSIZE];
    int argc_in;                   // arg count
	char* argv_in[MAXARGS + 1];    // array of string args
	int argc_new;
	char* argv_new[MAXARGS + 1];

	// (1) read STDIN
    long bytes_read = _read(STDIN, buf, BUFSIZE - 1);
    if (bytes_read < 0) {
        dprintf(CONSOLEOUT, "Failed to read STDIN (Error Code: %d)\n", bytes_read);
        return;
    }
	buf[bytes_read] = '\0';

	// get args from STDIN
    argc_in = parse(buf, argv_in);

	// (2) set up new argv and argc
	// shift all arg indices forward to pop argv[0] = xargs
	argc_new = argc - 1;
	for (size_t i = 0; i < argc_new; i++) {
		argv_new[i] = argv[i+1];
	}
	// append each arg from STDIN to the end of the args from the command line
	for (size_t i = 0; i < argc_in; i++) {
		argv_new[argc_new + i] = argv_in[i];
	}
	argv_new[argc_new + argc_in] = NULL;

	argc_new += argc_in;

	// skip if no args
	if (argc_new == 0) return;

	// (3) get the path of the program
	char path[BUFSIZE];
	if (strchr(argv_new[0], '/') == NULL) {
		// prepend c/
		snprintf(path, BUFSIZE, "c/%s", argv_new[0]);
	} else {
		strncpy(path, argv_new[0], BUFSIZE);
	}

	// (4) open the program file
	int fd = _open(-1, path);
	if (fd < 0) {
		// failed to open program file
		dprintf(CONSOLEOUT, "Failed to find %s (Error Code: %d)\r", path, fd);
		return;
	}

	// (5) just exec, don't need to fork
	int result = _exec(fd, argc_new, argv_new);
	// unsuccessful exec
	dprintf(CONSOLEOUT, "Failed to exec %s (Error Code: %d)\r", path, result);
	_exit();
	

    return;
}
