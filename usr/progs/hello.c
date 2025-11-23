#ifndef UMODE // cp1
    #include "uio.h"
    void main(struct uio * uio) {
        uio_printf(uio, "Hello, world!\n");
    }
#endif

#ifdef UMODE // cp2&3
    #include "string.h"
    #include "syscall.h"   // <-- make sure this exists and has the wrappers
    #include "error.h"     // optional, if you want error_name()

    void main(void) {
        int rc, fd;
        char buf[64];
        char *path = "c/hello_test.txt";     // "c" = KTFS mountpoint
        char *msg  = "Hello from file syscall!\n";

        printf("hello: starting syscall file test\n");

        // 1) Create a file on KTFS
        rc = _fscreate(path);
        if (rc < 0) {
            printf("fscreate(%s) failed: %d\n", path, rc);
            // if error_name() is available:
            // printf("fscreate failed: %s\n", error_name(rc));
        } else {
            printf("fscreate(%s) OK\n", path);
        }

        // 2) Open the file for writing (fd = -1 asks kernel to choose a free fd)
        fd = _open(-1, path);
        if (fd < 0) {
            printf("open(%s) failed: %d\n", path, fd);
            return;
        }
        printf("open(%s) -> fd=%d\n", path, fd);

        // 3) Write a message into the file
        rc = (int)_write(fd, msg, strlen(msg));
        printf("write(fd=%d) -> %d bytes\n", fd, rc);

        // 4) Close the file
        rc = _close(fd);
        printf("close(fd=%d) -> %d\n", fd, rc);

        // 5) Reopen for reading
        fd = _open(-1, path);
        if (fd < 0) {
            printf("reopen(%s) failed: %d\n", path, fd);
            return;
        }
        printf("reopen(%s) -> fd=%d\n", path, fd);

        // 6) Read back the content
        memset(buf, 0, sizeof buf);
        rc = (int)_read(fd, buf, sizeof(buf) - 1);
        printf("read(fd=%d) -> %d bytes\n", fd, rc);
        printf("file contents: \"%s\"\n", buf);

        // 7) Close again
        rc = _close(fd);
        printf("close(fd=%d) -> %d\n", fd, rc);

        // 8) Optionally delete the file
        rc = _fsdelete(path);
        printf("fsdelete(%s) -> %d\n", path, rc);

        printf("hello: syscall file test done\n");
    }
#endif