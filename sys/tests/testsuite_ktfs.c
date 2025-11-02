#include "testsuite_ktfs.h"
#include "console.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "misc.h"
#include "uio.h"
#include <string.h>

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
    // Open 'c:\hello' file
    struct uio* uio;
    int result = open_file("c", "hello", &uio);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    // Read contents
    char buffer[64];
    long bytes_read = uio_read(uio, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("uio_read failed: %s\n", error_name(bytes_read));
        uio_close(uio);
        return -1;
    }

    // Print contents
    for (long i = 0; i < bytes_read; i++) {
        kprintf("%c", buffer[i]);
    }
    kprintf("\n");

    // Make sure it's an ELF file
    assert(strncmp(&buffer[1], "ELF", 3) == 0);

    uio_close(uio);

    // Open 'c:\trek' file
    result = open_file("c", "trek", &uio);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    // Read contents
    bytes_read = uio_read(uio, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("uio_read failed: %s\n", error_name(bytes_read));
        uio_close(uio);
        return -1;
    }

    // Print contents
    for (long i = 0; i < bytes_read; i++) {
        kprintf("%c", buffer[i]);
    }
    kprintf("\n");

    // Make sure it's an ELF file
    assert(strncmp(&buffer[1], "ELF", 3) == 0);

    uio_close(uio);
    
    return 0;
}
