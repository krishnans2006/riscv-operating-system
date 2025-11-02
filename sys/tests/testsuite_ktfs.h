#ifndef _TESTSUITE_KTFS_H_
#define _TESTSUITE_KTFS_H_

// Add more test prototypes here
// Add args if you want
void run_testsuite_ktfs(const char* name);
int test_open(void);
int test_partial_read(void);
int test_many_small_reads(void);
int test_multiblock_reads(void);

#endif // _TESTSUITE_KTFS_H_