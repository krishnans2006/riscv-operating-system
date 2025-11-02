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

    retval = test_many_small_reads();
    test_output = (retval == 0) ? "test_many_small_reads passed!" : "test_many_small_reads failed!";
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
    kprintf("Contents of hello: ");
    for (long i = 0; i < bytes_read; i++) {
        kprintf("%c", buffer[i]);
    }
    kprintf("...\n");

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
    kprintf("Contents of trek: ");
    for (long i = 0; i < bytes_read; i++) {
        kprintf("%c", buffer[i]);
    }
    kprintf("...\n");

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
    kprintf("Contents of test.txt: ");
    for (long i = 0; i < bytes_read; i++) {
        kprintf("%c", buffer[i]);
    }
    kprintf("...\n");

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

    // Print first 2 bytes
    kprintf("First 2 bytes of hello: %c%c\n", buffer[0], buffer[1]);

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

    // Print next 2 bytes
    kprintf("Next 2 bytes of hello: %c%c\n", buffer[0], buffer[1]);
    
    // Verify continuation of ELF header
    assert(buffer[0] == 'L');
    assert(buffer[1] == 'F');
    
    uio_close(uio);

    return 0;
}

int test_many_small_reads() {
    // Open 'c:\test.txt' file
    struct uio* uio;
    int result = open_file("c", "test.txt", &uio);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    // Read a small (but changing) number of bytes repeatedly and verify contents
    char buffer[16];
    int total_bytes_read = 0;

    kprintf("Reading test.txt in many small reads:\n");

    for (int size = 1; size <= 16; size++) {
        kprintf("  ");
        for (int iter = 0; iter < 10; iter++) {
            long bytes_read = uio_read(uio, buffer, size);
            if (bytes_read < 0) {
                kprintf("uio_read failed: %s\n", error_name(bytes_read));
                uio_close(uio);
                return -1;
            }
            assert(bytes_read == size);

            // Print read contents
            for (long i = 0; i < bytes_read; i++) {
                kprintf("%c", buffer[i]);
            }
            kprintf(" ");

            // Verify contents
            for (long i = 0; i < bytes_read; i++) {
                char expected_char = '0' + ((total_bytes_read + i) % 10);
                assert(buffer[i] == expected_char);
            }

            total_bytes_read += bytes_read;
        }
        kprintf("\n");
    }

    assert(total_bytes_read == 10 * 16 * (16 + 1) / 2);  // Sum of 1 to 16, times 10

    uio_close(uio);
    return 0;
}
