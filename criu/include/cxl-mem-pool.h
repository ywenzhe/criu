#ifndef __CXL_MEM_POOL_H__
#define __CXL_MEM_POOL_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

/*
 * CXL Memory Pool Management
 *
 * This module manages a memory pool backed by CXL memory accessed via
 * a DAX (Direct Access) device. It provides atomic allocation and
 * read/write operations for checkpoint/restore operations.
 */

struct cxl_mem_pool {
	void *base_addr;		/* mmap() base address of DAX device */
	size_t total_size;		/* Total size of memory pool (bytes) */
	atomic_uint_fast64_t write_offset; /* Current write offset (atomic) */
	int dax_fd;			/* File descriptor for DAX device */
	bool initialized;		/* Whether pool is initialized */
	char *dev_path;			/* Path to DAX device */
};

/* Global CXL memory pool instance */
extern struct cxl_mem_pool cxl_pool;

/*
 * Initialize CXL memory pool
 *
 * @dax_dev_path: Path to DAX device (e.g., "/dev/dax0.0")
 * @pool_size: Size of memory pool to map (bytes, must be page-aligned)
 *
 * Returns: 0 on success, -1 on error
 */
int cxl_mem_pool_init(const char *dax_dev_path, size_t pool_size);

/*
 * Cleanup and unmap CXL memory pool
 */
void cxl_mem_pool_fini(void);

/*
 * Atomically allocate space in CXL memory pool
 *
 * @bytes: Number of bytes to allocate (will be aligned to PAGE_SIZE)
 *
 * Returns: Offset in pool on success, (uint64_t)-1 on exhaustion
 */
uint64_t cxl_mem_pool_alloc(size_t bytes);

/*
 * Write data to CXL memory pool at given offset
 *
 * @offset: Offset in pool (must be within allocated range)
 * @data: Data to write
 * @size: Size of data in bytes
 *
 * Returns: 0 on success, -1 on error
 */
int cxl_mem_pool_write(uint64_t offset, const void *data, size_t size);

/*
 * Read data from CXL memory pool at given offset
 *
 * @offset: Offset in pool
 * @buffer: Buffer to read into
 * @size: Size to read in bytes
 *
 * Returns: 0 on success, -1 on error
 */
int cxl_mem_pool_read(uint64_t offset, void *buffer, size_t size);

/*
 * Reset write offset (for pre-dump scenarios)
 */
void cxl_mem_pool_reset(void);

/*
 * Get current pool usage statistics
 *
 * @used_bytes: Output - bytes allocated
 * @total_bytes: Output - total pool size
 */
void cxl_mem_pool_get_stats(uint64_t *used_bytes, uint64_t *total_bytes);

#endif /* __CXL_MEM_POOL_H__ */
