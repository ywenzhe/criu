#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "cxl-mem-pool.h"
#include "log.h"
#include "common/compiler.h"
#include "common/page.h"
#include "util.h"

/* Global CXL memory pool instance */
struct cxl_mem_pool cxl_pool = {
	.base_addr = NULL,
	.total_size = 0,
	.write_offset = ATOMIC_VAR_INIT(0),
	.dax_fd = -1,
	.initialized = false,
	.dev_path = NULL,
};

int cxl_mem_pool_init(const char *dax_dev_path, size_t pool_size)
{
	struct stat st;
	void *addr;

	if (cxl_pool.initialized) {
		pr_warn("CXL memory pool already initialized\n");
		return 0;
	}

	if (!dax_dev_path || pool_size == 0) {
		pr_err("Invalid parameters: dax_dev_path=%s, pool_size=%zu\n", dax_dev_path, pool_size);
		return -1;
	}

	/* Ensure pool_size is page-aligned */
	if (pool_size % PAGE_SIZE != 0) {
		pr_warn("pool_size %zu is not page-aligned, rounding up\n", pool_size);
		pool_size = ALIGN(pool_size, PAGE_SIZE);
	}

	pr_info("Initializing CXL memory pool: device=%s, size=%zu bytes (%zu MB)\n", dax_dev_path, pool_size,
		pool_size / (1024 * 1024));

	/* Open DAX device */
	cxl_pool.dax_fd = open(dax_dev_path, O_RDWR);
	if (cxl_pool.dax_fd < 0) {
		pr_perror("Failed to open DAX device %s", dax_dev_path);
		pr_err("Ensure the device exists and you have sufficient permissions (root/CAP_SYS_ADMIN)\n");
		return -1;
	}

	/* Verify DAX device size */
	if (fstat(cxl_pool.dax_fd, &st) < 0) {
		pr_perror("Failed to stat DAX device %s", dax_dev_path);
		goto err_close_fd;
	}

	if (S_ISCHR(st.st_mode)) {
		/* Character device - try to get size via ioctl or assume sufficient */
		pr_debug("DAX device %s is a character device\n", dax_dev_path);
	} else if (st.st_size > 0 && (size_t)st.st_size < pool_size) {
		pr_err("DAX device %s size (%lld bytes) is smaller than requested pool_size (%zu bytes)\n",
		       dax_dev_path, (long long)st.st_size, pool_size);
		goto err_close_fd;
	}

	/* Memory map the DAX device */
	addr = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, cxl_pool.dax_fd, 0);
	if (addr == MAP_FAILED) {
		pr_perror("Failed to mmap DAX device %s (size=%zu)", dax_dev_path, pool_size);
		pr_err("Common causes:\n");
		pr_err("  - Insufficient permissions (need root or CAP_SYS_ADMIN)\n");
		pr_err("  - DAX device size mismatch\n");
		pr_err("  - Memory address space exhaustion\n");
		goto err_close_fd;
	}

	cxl_pool.base_addr = addr;
	cxl_pool.total_size = pool_size;
	cxl_pool.dev_path = xstrdup(dax_dev_path);
	atomic_init(&cxl_pool.write_offset, 0);
	cxl_pool.initialized = true;

	pr_info("CXL memory pool initialized successfully at %p\n", addr);
	return 0;

err_close_fd:
	close(cxl_pool.dax_fd);
	cxl_pool.dax_fd = -1;
	return -1;
}

void cxl_mem_pool_fini(void)
{
	if (!cxl_pool.initialized)
		return;

	pr_info("Cleaning up CXL memory pool\n");

	if (cxl_pool.base_addr != NULL) {
		if (munmap(cxl_pool.base_addr, cxl_pool.total_size) < 0)
			pr_perror("Failed to munmap CXL memory pool");
		cxl_pool.base_addr = NULL;
	}

	if (cxl_pool.dax_fd >= 0) {
		close(cxl_pool.dax_fd);
		cxl_pool.dax_fd = -1;
	}

	if (cxl_pool.dev_path) {
		xfree(cxl_pool.dev_path);
		cxl_pool.dev_path = NULL;
	}

	cxl_pool.total_size = 0;
	atomic_store(&cxl_pool.write_offset, 0);
	cxl_pool.initialized = false;
}

uint64_t cxl_mem_pool_alloc(size_t bytes)
{
	uint64_t old_offset, new_offset;

	if (!cxl_pool.initialized) {
		pr_err("CXL memory pool not initialized\n");
		return (uint64_t)-1;
	}

	/* Align to page size for safety */
	if (bytes % PAGE_SIZE != 0) {
		pr_debug("Aligning allocation size from %zu to %zu bytes\n", bytes, ALIGN(bytes, PAGE_SIZE));
		bytes = ALIGN(bytes, PAGE_SIZE);
	}

	/* Atomically allocate space */
	old_offset = atomic_fetch_add(&cxl_pool.write_offset, bytes);
	new_offset = old_offset + bytes;

	if (new_offset > cxl_pool.total_size) {
		pr_err("CXL memory pool exhausted: requested %zu bytes, offset %llu exceeds pool size %zu\n", bytes,
		       (unsigned long long)new_offset, cxl_pool.total_size);
		return (uint64_t)-1;
	}

	pr_debug("Allocated %zu bytes at offset %llu (pool usage: %.2f%%)\n", bytes, (unsigned long long)old_offset,
		 (double)new_offset / cxl_pool.total_size * 100.0);

	return old_offset;
}

int cxl_mem_pool_write(uint64_t offset, const void *data, size_t size)
{
	if (!cxl_pool.initialized) {
		pr_err("CXL memory pool not initialized\n");
		return -1;
	}

	if (offset + size > cxl_pool.total_size) {
		pr_err("Write out of bounds: offset %llu + size %zu exceeds pool size %zu\n",
		       (unsigned long long)offset, size, cxl_pool.total_size);
		return -1;
	}

	/* Direct memcpy to DAX-mapped memory */
	memcpy((char *)cxl_pool.base_addr + offset, data, size);

	pr_debug("Wrote %zu bytes to CXL offset %llu\n", size, (unsigned long long)offset);
	return 0;
}

int cxl_mem_pool_read(uint64_t offset, void *buffer, size_t size)
{
	if (!cxl_pool.initialized) {
		pr_err("CXL memory pool not initialized\n");
		return -1;
	}

	if (offset + size > cxl_pool.total_size) {
		pr_err("Read out of bounds: offset %llu + size %zu exceeds pool size %zu\n",
		       (unsigned long long)offset, size, cxl_pool.total_size);
		return -1;
	}

	/* Direct memcpy from DAX-mapped memory */
	memcpy(buffer, (char *)cxl_pool.base_addr + offset, size);

	pr_debug("Read %zu bytes from CXL offset %llu\n", size, (unsigned long long)offset);
	return 0;
}

void cxl_mem_pool_reset(void)
{
	if (!cxl_pool.initialized) {
		pr_warn("Attempted to reset uninitialized CXL memory pool\n");
		return;
	}

	atomic_store(&cxl_pool.write_offset, 0);
	pr_info("CXL memory pool write offset reset to 0\n");
}

void cxl_mem_pool_get_stats(uint64_t *used_bytes, uint64_t *total_bytes)
{
	if (!cxl_pool.initialized) {
		if (used_bytes)
			*used_bytes = 0;
		if (total_bytes)
			*total_bytes = 0;
		return;
	}

	if (used_bytes)
		*used_bytes = atomic_load(&cxl_pool.write_offset);
	
	if (total_bytes)
		*total_bytes = cxl_pool.total_size;
}

void *cxl_mem_pool_get_addr(uint64_t offset)
{
       if (!cxl_pool.initialized)
               return NULL;

       if (offset >= cxl_pool.total_size)
               return NULL;

       return (char *)cxl_pool.base_addr + offset;
}