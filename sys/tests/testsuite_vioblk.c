#include "testsuite_vioblk.h"
#include "device.h"
#include "error.h"
#include "console.h"
#include "string.h"   
#include "uio.h"      

#define CDEVINST 0
#define CDEVNAME "vioblk"
//#define CDEVNAME "vioblk"   // vioblk_attach()

static int test_open_and_capacity(struct storage *sto);
static int test_rw_verify(struct storage *sto);

void run_testsuite_vioblk() {

    struct storage *sto = find_storage(CDEVNAME, CDEVINST);  // returns void* to storage
    if (!sto) {
        kprintf("[vioblk] ERROR: device not found\n");
        return;
    }

    int rc = storage_open(sto);                                // open the device
    if (rc != 0) {
        kprintf("[vioblk] ERROR: storage_open -> %s\n", error_name(rc));
        return;
    }

    rc = test_open_and_capacity(sto);
    if (rc != 0) {
        kprintf("[vioblk] FAIL: capacity test -> %s\n", error_name(rc));
        return;
    }

    rc = test_rw_verify(sto);
    if (rc != 0) {
        kprintf("[vioblk] FAIL: rw verify -> %s\n", error_name(rc));
        goto out_close;
    }

    kprintf("[vioblk] ALL TESTS PASSED\n");

out_close:
    storage_close(sto);
}

static int test_open_and_capacity(struct storage *sto) {
    unsigned long long cap = 0ULL;
    int rc = storage_cntl(sto, FCNTL_GETEND, &cap);            // returns capacity in BYTES via arg
    if (rc != 0) return rc;

    unsigned int blksz = storage_blksz(sto);
    if (blksz == 0) return -EINVAL;

    kprintf("[vioblk] capacity = %llu bytes, blksz = %u bytes\n", cap, blksz);


    return 0;
}

static int test_rw_verify(struct storage *sto) {
    unsigned int blksz = storage_blksz(sto);

    // Buffers
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];

    // Fill pattern
    for (unsigned i = 0; i < 512; i++) {
        wbuf[i] = (uint8_t)(i);
    }
    memset(rbuf, 0x00, 512);

    // Write to LBA 0 (pos=0). NOTE: this overwrites the first sectors.
    long wrote = storage_store(sto, 0ULL, wbuf, 512);
    
    if (wrote < 0) 
        return (int)wrote;
    if ((unsigned long)wrote != 512) 
        return -EIO;

    // Read back
    long readn = storage_fetch(sto, /*pos*/0ULL, rbuf, 512);

    if (readn < 0) 
        return (int)readn;
    if ((unsigned long)readn != 512) return -EIO;

    // Verify
    if (memcmp(wbuf, rbuf, 512) != 0) {
        kprintf("[vioblk] MISMATCH after readback!\n");
        return -EIO;
    }

    kprintf("[vioblk] rw verify OK\n");
    return 0;
}
