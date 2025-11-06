#ifndef __CR_MEM_DUMP_H__
#define __CR_MEM_DUMP_H__

#include "common/list.h"
#include "images/mm.pb-c.h"

/*
 * 内存 dump 缓冲区结构
 * 用于在内存中存储序列化后的 protobuf 数据，而不是写入文件
 */
struct mem_dump_buffer {
	void *data;           /* 序列化后的 protobuf 数据 */
	size_t size;          /* 数据大小 */
	size_t capacity;      /* 缓冲区容量 */
	int type;             /* 数据类型（对应原来的 CR_FD_* 类型） */
	pid_t pid;            /* 关联的进程 PID */
	struct list_head list; /* 链表节点 */
};

/*
 * 内存 dump 管理器
 * 管理所有在内存中的 dump 数据
 */
struct mem_dump_manager {
	struct list_head buffers;  /* 所有缓冲区的链表 */
	size_t total_size;         /* 总数据大小 */
	int nr_buffers;            /* 缓冲区数量 */
};

/* 全局内存 dump 管理器 */
extern struct mem_dump_manager *mem_dump_mgr;

/* 初始化内存 dump 管理器 */
extern int mem_dump_init(void);

/* 清理内存 dump 管理器 */
extern void mem_dump_fini(void);

/* 在内存中写入 protobuf 数据 */
extern int mem_dump_write_pb(pid_t pid, int type, void *obj, int pb_type);

/* 获取指定类型和 PID 的内存 dump 数据 */
extern struct mem_dump_buffer *mem_dump_get(pid_t pid, int type);

/* 遍历所有内存 dump 缓冲区 */
#define for_each_mem_dump_buffer(buf) \
	list_for_each_entry(buf, &mem_dump_mgr->buffers, list)

/* 打印内存 dump 统计信息 */
extern void mem_dump_show_stats(void);

/* 可选：将内存 dump 数据导出到文件（用于调试或持久化） */
extern int mem_dump_export_to_files(const char *dir);

/* 可选：从内存 dump 数据恢复（用于后续的 restore 操作） */
extern int mem_dump_restore_from_memory(void);

#endif /* __CR_MEM_DUMP_H__ */

