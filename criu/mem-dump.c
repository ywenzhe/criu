#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <google/protobuf-c/protobuf-c.h>

#include "mem-dump.h"
#include "log.h"
#include "xmalloc.h"
#include "protobuf.h"
#include "image-desc.h"

/* 全局内存 dump 管理器 */
struct mem_dump_manager *mem_dump_mgr = NULL;

/* 初始化内存 dump 管理器 */
int mem_dump_init(void)
{
	if (mem_dump_mgr) {
		pr_warn("Memory dump manager already initialized\n");
		return 0;
	}

	mem_dump_mgr = xzalloc(sizeof(*mem_dump_mgr));
	if (!mem_dump_mgr) {
		pr_perror("Failed to allocate memory dump manager");
		return -1;
	}

	INIT_LIST_HEAD(&mem_dump_mgr->buffers);
	mem_dump_mgr->total_size = 0;
	mem_dump_mgr->nr_buffers = 0;

	pr_info("Memory dump manager initialized\n");
	return 0;
}

/* 清理内存 dump 管理器 */
void mem_dump_fini(void)
{
	struct mem_dump_buffer *buf, *tmp;

	if (!mem_dump_mgr)
		return;

	pr_info("Cleaning up memory dump manager (%d buffers, %zu bytes)\n",
		mem_dump_mgr->nr_buffers, mem_dump_mgr->total_size);

	list_for_each_entry_safe(buf, tmp, &mem_dump_mgr->buffers, list) {
		list_del(&buf->list);
		xfree(buf->data);
		xfree(buf);
	}

	xfree(mem_dump_mgr);
	mem_dump_mgr = NULL;
}

/* 创建新的内存 dump 缓冲区 */
static struct mem_dump_buffer *alloc_mem_dump_buffer(pid_t pid, int type)
{
	struct mem_dump_buffer *buf;

	buf = xzalloc(sizeof(*buf));
	if (!buf) {
		pr_perror("Failed to allocate mem_dump_buffer");
		return NULL;
	}

	buf->pid = pid;
	buf->type = type;
	buf->size = 0;
	buf->capacity = 0;
	buf->data = NULL;
	INIT_LIST_HEAD(&buf->list);

	return buf;
}

/* 扩展缓冲区容量 */
static int expand_buffer(struct mem_dump_buffer *buf, size_t new_size)
{
	void *new_data;
	size_t new_capacity;

	if (new_size <= buf->capacity)
		return 0;

	/* 按 2 的幂次增长，或直接增长到所需大小 */
	new_capacity = buf->capacity ? buf->capacity : 4096;
	while (new_capacity < new_size)
		new_capacity *= 2;

	new_data = xrealloc(buf->data, new_capacity);
	if (!new_data) {
		pr_perror("Failed to expand buffer from %zu to %zu bytes",
			  buf->capacity, new_capacity);
		return -1;
	}

	buf->data = new_data;
	buf->capacity = new_capacity;

	return 0;
}

/*
 * 在内存中写入 protobuf 数据
 * 这个函数替代了原来的 pb_write_one() + img_from_set() 的组合
 */
int mem_dump_write_pb(pid_t pid, int type, void *obj, int pb_type)
{
	struct mem_dump_buffer *buf;
	ProtobufCMessage *msg = obj;
	size_t packed_size;
	void *packed_data;
	int ret = -1;

	if (!mem_dump_mgr) {
		pr_err("Memory dump manager not initialized\n");
		return -1;
	}

	/* 计算序列化后的大小 */
	packed_size = protobuf_c_message_get_packed_size(msg);
	if (packed_size == 0) {
		pr_err("Invalid protobuf message (size = 0)\n");
		return -1;
	}

	pr_debug("Dumping to memory: pid=%d type=%d pb_type=%d size=%zu\n",
		 pid, type, pb_type, packed_size);

	/* 创建新的缓冲区 */
	buf = alloc_mem_dump_buffer(pid, type);
	if (!buf)
		return -1;

	/* 分配足够的空间存储序列化数据 */
	if (expand_buffer(buf, packed_size) < 0)
		goto err;

	/* 序列化 protobuf 消息到内存 */
	packed_data = buf->data;
	packed_size = protobuf_c_message_pack(msg, packed_data);
	if (packed_size == 0) {
		pr_err("Failed to pack protobuf message\n");
		goto err;
	}

	buf->size = packed_size;

	/* 添加到管理器的链表中 */
	list_add_tail(&buf->list, &mem_dump_mgr->buffers);
	mem_dump_mgr->nr_buffers++;
	mem_dump_mgr->total_size += packed_size;

	pr_info("Memory dump: pid=%d type=%d size=%zu (total: %d buffers, %zu bytes)\n",
		pid, type, packed_size, mem_dump_mgr->nr_buffers, mem_dump_mgr->total_size);

	ret = 0;
	goto out;

err:
	if (buf) {
		xfree(buf->data);
		xfree(buf);
	}
out:
	return ret;
}

/* 获取指定类型和 PID 的内存 dump 数据 */
struct mem_dump_buffer *mem_dump_get(pid_t pid, int type)
{
	struct mem_dump_buffer *buf;

	if (!mem_dump_mgr)
		return NULL;

	list_for_each_entry(buf, &mem_dump_mgr->buffers, list) {
		if (buf->pid == pid && buf->type == type)
			return buf;
	}

	return NULL;
}

/* 打印内存 dump 统计信息 */
void mem_dump_show_stats(void)
{
	struct mem_dump_buffer *buf;
	int count_by_type[CR_FD_MAX] = {0};
	size_t size_by_type[CR_FD_MAX] = {0};

	if (!mem_dump_mgr) {
		pr_info("Memory dump manager not initialized\n");
		return;
	}

	pr_info("\n");
	pr_info("========================================\n");
	pr_info("Memory Dump Statistics\n");
	pr_info("========================================\n");
	pr_info("Total buffers: %d\n", mem_dump_mgr->nr_buffers);
	pr_info("Total size: %zu bytes (%.2f MB)\n",
		mem_dump_mgr->total_size,
		mem_dump_mgr->total_size / (1024.0 * 1024.0));

	/* 按类型统计 */
	list_for_each_entry(buf, &mem_dump_mgr->buffers, list) {
		if (buf->type >= 0 && buf->type < CR_FD_MAX) {
			count_by_type[buf->type]++;
			size_by_type[buf->type] += buf->size;
		}
	}

	pr_info("\nBreakdown by type:\n");
	for (int i = 0; i < CR_FD_MAX; i++) {
		if (count_by_type[i] > 0) {
			pr_info("  Type %d: %d buffers, %zu bytes\n",
				i, count_by_type[i], size_by_type[i]);
		}
	}
	pr_info("========================================\n");
}

/* 将内存 dump 数据导出到文件（用于调试或持久化） */
int mem_dump_export_to_files(const char *dir)
{
	struct mem_dump_buffer *buf;
	char filepath[PATH_MAX];
	FILE *fp;
	int count = 0;

	if (!mem_dump_mgr) {
		pr_err("Memory dump manager not initialized\n");
		return -1;
	}

	pr_info("Exporting memory dumps to directory: %s\n", dir);

	list_for_each_entry(buf, &mem_dump_mgr->buffers, list) {
		snprintf(filepath, sizeof(filepath), "%s/memdump_%d_%d_%d.bin",
			 dir, buf->pid, buf->type, count);

		fp = fopen(filepath, "wb");
		if (!fp) {
			pr_perror("Failed to create file %s", filepath);
			return -1;
		}

		if (fwrite(buf->data, 1, buf->size, fp) != buf->size) {
			pr_perror("Failed to write data to %s", filepath);
			fclose(fp);
			return -1;
		}

		fclose(fp);
		count++;
		pr_debug("Exported: %s (%zu bytes)\n", filepath, buf->size);
	}

	pr_info("Successfully exported %d memory dumps\n", count);
	return 0;
}

/* 可选：从内存 dump 数据恢复（用于后续的 restore 操作） */
int mem_dump_restore_from_memory(void)
{
	/* TODO: 实现从内存恢复的逻辑 */
	pr_info("Memory dump restore not yet implemented\n");
	return 0;
}

