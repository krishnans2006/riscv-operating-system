#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

// helper function for parser
char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\t':
			case '\0':
			case FIN:
			case FOUT:
			case PIPE:
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

int parse(char* buf, char* argv[][MAXARGS + 1], int* argc, char** input_file, char** output_file) {
	// FIXME
	// feel free to change this function however you see fit

	int pn = 0; // program number, highest level index into argv and argc

	argc[0] = 0;
	*input_file = NULL;
    *output_file = NULL;

	char *head;
	head = buf;
	
	while(*head) { // find each argv
		// skip spaces
		while(*head == ' ' || *head == '\t') head++;
		if (*head == '\0') break;

		switch(*head) {
			// end of buf
			case '\0':
				break;

			// > output to file
			case FOUT:
				head++; 										// step over >
				while(*head == ' ' || *head == '\t') head++; 	// step over spaces to reach the start of the output file
				*output_file = head;							// set the output file
				head = find_terminator(head);					// get to the end of the output file name
				if (*head) {
					*head = '\0';
					head++;
				}
				continue;

			// < input to file
			case FIN:
				head++;											// step over <
				while(*head == ' ' || *head == '\t') head++; 	// step over spaces to reach  the start of the input file
				*input_file = head;								// set the input file
				head = find_terminator(head);					// get to the end of the input file name
				if (*head) {
					*head = '\0';
					head++;
				}
				continue;

			// | pipe
			case PIPE:
				head++;						// step over |
				argv[pn][argc[pn]] = NULL;	// null-terminate the current argv
				pn++;						// increment program number
				*(argc + pn) = 0;
				continue;

			default:	
				// regular argument to store
				argv[pn][argc[pn]++] = head;
				head = find_terminator(head);
				if (*head) {
					*head = '\0';
					head++;
				}
		}
	}

	// reach the '\0' at the end

	argv[pn][argc[pn]] = NULL; // null-terminate the current argv
	return pn + 1; 			   // returns the number of program commands
}

int main()
{
    char buf[BUFSIZE];
	int argc[MAXARGS]; // array of ints, each int is arg count for one of a separate piped program
	char* argv[MAXARGS][MAXARGS + 1]; // array of array of string args, each array of string args corresponds to a separate piped program
	char* input_file;
	char* output_file;
	int wfd, rfd, rfd_next;
	int result; // for storing error codes


  	_open(CONSOLEOUT, "dev/uart1");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		printf("LUMON OS> ");
		getsn(buf, BUFSIZE - 1);

		if (0 == strcmp(buf, "exit"))
		_exit();

		// FIXME
		// (1) Call parse and terminate each argv with NULL
		int num = parse(buf, argv, argc, &input_file, &output_file);

		// skip if one of the programs has no args or has > 8 args
		int no_args = 0;
		for (int pn = 0; pn < num; pn++) {
			if (argc[pn] == 0) { no_args = 1; break; }
			if (argc[pn] > MAXARGS) { no_args = 2; break; }
		}
		if (no_args == 1) {
			continue;
		} else if (no_args == 2) {
			dprintf(CONSOLEOUT, "Error: Program has > %d arguments\n", MAXARGS);
			continue;
		}


		// (2) loop over each piped program
		wfd = -1; rfd = -1; rfd_next = -1; // reset pipe ptrs

		for (int pn = 0; pn < num; pn++) {
			// (a) get the path of the program
			char path[BUFSIZE];
			if (strchr(argv[pn][0], '/') == NULL) {
				// prepend c/
				snprintf(path, BUFSIZE, "c/%s", argv[pn][0]);
			} else {
				strncpy(path, argv[pn][0], BUFSIZE);
			}

			// (b) open the program file
			int fd = _open(-1, path);
			if (fd < 0) {
				// failed to open program file
				dprintf(CONSOLEOUT, "Failed to find %s (Error Code: %d)\n", path, fd);
				break;
			}

			// (c-i) get the read-pipe from the previous program, reset other pipes
			rfd = rfd_next;
			rfd_next = -1;
			wfd = -1;

			// (c-ii) create outputing pipe, which happens when it's not the last program in the pipeline
			if (pn < num - 1) {
				result = _pipe(&wfd, &rfd_next);
				if (result < 0) {
					dprintf(CONSOLEOUT, "Failed to create pipe (Error Code: %d)\n", result);
					break;
				}
			}

			// (d) fork and exec
			int pid = _fork();
			if (pid == 0) {
				// CHILD process
				// (i) input redirection
				if (input_file != NULL && pn == 0) {
					_close(STDIN);
					result = _open(STDIN, input_file);
					if (result < 0) {
						dprintf(CONSOLEOUT, "Failed to open %s (Error Code: %d)\n", input_file, result);
						_exit();
					}
				}
				// (ii) output redirection
				if (output_file != NULL && pn == num - 1) {
					_fscreate(output_file);
					_close(STDOUT);
					result = _open(STDOUT, output_file);
					if (result < 0) {
						dprintf(CONSOLEOUT, "Failed to open %s (Error Code: %d)\n", output_file, result);
						_exit();
					}
				}
				// (iii) connect output pipe, which happens when it's not the last program
				if (pn < num - 1) {
					_close(STDOUT);
					_uiodup(wfd, STDOUT);
				}
				// (iv) connect input pipe, which happens when it's not the first program
				if (pn > 0) {
					_close(STDIN);
					_uiodup(rfd, STDIN);
				}

				// (v) close pipe descriptor files
				if (wfd >= 0) _close(wfd);
				if (rfd >= 0) _close(rfd);
				if (rfd_next >= 0) _close(rfd_next);

				// (vi) finally exec
				result = _exec(fd, argc[pn], argv[pn]);
				// unsuccessful exec
				dprintf(CONSOLEOUT, "Failed to exec %s (Error Code: %d)\n", path, result);
				_exit();
			}

			// close file descriptors
			_close(fd);
			if (pn < num - 1) {
				_close(wfd);
			}
			if (pn > 0) {
				_close(rfd);
			}
			// wait for children process
			_wait(pid);
		}
	}
}
