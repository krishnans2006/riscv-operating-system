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

    kprintf("Run test_open:\n");
    retval = test_open();
    kprintf("%s\n\n", (retval == 0) ? "Pass!" : "Fail!");
    
    kprintf("Run test_partial_read:\n");
    retval = test_partial_read();
    kprintf("%s\n\n", (retval == 0) ? "Pass!" : "Fail!");

    kprintf("Run test_many_small_reads:\n");
    retval = test_many_small_reads();
    kprintf("%s\n\n", (retval == 0) ? "Pass!" : "Fail!");
    
    kprintf("Run test_multiblock_reads:\n");
    retval = test_multiblock_reads();
    kprintf("%s\n\n", (retval == 0) ? "Pass!" : "Fail!");
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

int test_multiblock_reads() {
    // Open 'c:\test.txt' file
    struct uio* uio;
    int result = open_file("c", "test.txt", &uio);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    // Read 602 bytes (one block is 512 bytes) many times
    char buffer[602];
    int total_bytes_read = 0;
    
    kprintf("Reading 602 bytes at a time from test.txt:\n");

    for (int iter = 0; iter < 7; iter++) {
        long bytes_read = uio_read(uio, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            kprintf("uio_read failed: %s\n", error_name(bytes_read));
            uio_close(uio);
            return -1;
        }
        assert(bytes_read == sizeof(buffer));

        // Print read contents
        kprintf("  From byte %d to %d: ", total_bytes_read, total_bytes_read + bytes_read - 1);
        for (long i = 0; i < bytes_read; i++) {
            kprintf("%c", buffer[i]);
        }
        kprintf("\n");

        // Verify contents
        for (long i = 0; i < bytes_read; i++) {
            char expected_char = '0' + ((total_bytes_read + i) % 10);
            assert(buffer[i] == expected_char);
        }

        total_bytes_read += bytes_read;
    };

    // Read 1602 bytes (> 3 blocks) a few times
    char buffer2[1602];

    kprintf("\nReading 1602 bytes at a time from test.txt:\n");

    for (int iter = 0; iter < 4; iter++) {
        long bytes_read = uio_read(uio, buffer2, sizeof(buffer2));
        if (bytes_read < 0) {
            kprintf("uio_read failed: %s\n", error_name(bytes_read));
            uio_close(uio);
            return -1;
        }
        assert(bytes_read == sizeof(buffer2));

        // Print read contents
        kprintf("  From byte %d to %d: ", total_bytes_read, total_bytes_read + bytes_read - 1);
        for (long i = 0; i < bytes_read; i++) {
            kprintf("%c", buffer2[i]);
        }
        kprintf("\n");
        
        // Verify contents
        for (long i = 0; i < bytes_read; i++) {
            char expected_char = '0' + ((total_bytes_read + i) % 10);
            assert(buffer2[i] == expected_char);
        }

        total_bytes_read += bytes_read;
    };

    uio_close(uio);
    return 0;
}
