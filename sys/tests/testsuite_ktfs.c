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

    retval = test_open();
    test_output = (retval == 0) ? "test_open passed!" : "test_open failed!";
    kprintf("%s\n", test_output);

    retval = test_partial_read();
    test_output = (retval == 0) ? "test_partial_read passed!" : "test_partial_read failed!";
    kprintf("%s\n", test_output);
}

int test_open() {
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

    // Open 'c:\test.txt' file
    // Contents of this file: '01234567890123456789...'
    result = open_file("c", "test.txt", &uio);
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

    // Make sure contents are correct
    for (long i = 0; i < bytes_read; i++) {
        assert(buffer[i] == '0' + (i % 10));
    }

    uio_close(uio);
    
    return 0;
}

int test_partial_read() {
    // Open 'c:\hello' file
    struct uio* uio;
    int result = open_file("c", "hello", &uio);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    // Read first 2 bytes
    char buffer[2];
    long bytes_read = uio_read(uio, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("uio_read failed: %s\n", error_name(bytes_read));
        uio_close(uio);
        return -1;
    }
    assert(bytes_read == 2);

    // Verify start of ELF header
    assert(buffer[0] == 0x7F);
    assert(buffer[1] == 'E');

    // Read next 2 bytes
    bytes_read = uio_read(uio, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("uio_read failed: %s\n", error_name(bytes_read));
        uio_close(uio);
        return -1;
    }
    assert(bytes_read == 2);
    
    // Verify continuation of ELF header
    assert(buffer[0] == 'L');
    assert(buffer[1] == 'F');
    
    uio_close(uio);
    return 0;
}
