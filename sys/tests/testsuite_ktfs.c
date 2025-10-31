#include "testsuite_ktfs.h"
#include "error.h"
#include "console.h"
#include "filesys.h"
#include "fsimpl.h"

void run_testsuite_ktfs(const char* name) {
    kprintf("Running ktfs tests...\n");

    // Run tests
    int retval = -EINVAL;
    char * test_output;

    retval = test1();
    test_output = (retval == 0) ? "test1 passed!" : "test1 failed!"; 
    kprintf("%s\n", test_output);
}

int test1() {
    // Open root directory
    struct uio* uio;
    int result = open_file("c", "\\hello", &uio);
    if (result != 0) {
        return result;
    }

    // Read contents
    char buffer[64];
    long bytes_read = uio_read(uio, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        uio_close(uio);
        return (int)bytes_read;
    }

    kprintf("Read %ld bytes: %.*s\n", bytes_read, (int)bytes_read, buffer);
    return 0;
}
