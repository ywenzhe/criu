#!/bin/bash
set -e

echo "=== CXL Memory Pool C/R Test ==="

# 准备测试程序
cat > /tmp/test_loop.c << 'EOF'
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

int main() {
    int count = 0;
    pid_t pid = getpid();

    printf("Started with PID %d\n", pid);
    fflush(stdout);

    while (1) {
        count++;
        sleep(2);
    }
    return 0;
}
EOF

gcc -o /tmp/test_loop /tmp/test_loop.c

# 创建checkpoint目录
CKPT_DIR=/tmp/criu_cxl_test
rm -rf $CKPT_DIR
mkdir -p $CKPT_DIR

# 启动测试进程（使用setsid成为session leader）
echo "=== Starting test process ==="
setsid /tmp/test_loop </dev/null >/dev/null 2>&1 &
PID=$!
echo "Process PID: $PID"

# 等待进程稳定
sleep 3

# Dump with CXL
echo "=== Dumping process with CXL memory ==="
sudo ./criu/criu dump -t $PID \
    --images-dir $CKPT_DIR \
    --cxl-mem \
    --cxl-dax=/tmp/cxl_test/cxl_mem.dat \
    --cxl-pool-size=512M \
    -v4 \
    --log-file=$CKPT_DIR/dump.log

if [ $? -ne 0 ]; then
    echo "DUMP FAILED"
    exit 1
fi

echo "=== Dump successful ==="
sleep 2

# Restore with CXL
echo "=== Restoring process from CXL memory ==="
sudo ./criu/criu restore \
    --images-dir $CKPT_DIR \
    --cxl-mem \
    --cxl-dax=/tmp/cxl_test/cxl_mem.dat \
    --cxl-pool-size=512M \
    -v4 \
    --log-file=$CKPT_DIR/restore.log

if [ $? -ne 0 ]; then
    echo "RESTORE FAILED - Check logs:"
    echo "  Dump:    $CKPT_DIR/dump.log"
    echo "  Restore: $CKPT_DIR/restore.log"
    exit 1
fi

echo "=== Restore successful ==="

# 检查恢复的进程
sleep 2
NEW_PID=$(pgrep test_loop)
if [ -n "$NEW_PID" ]; then
    echo "=== SUCCESS: Process restored with PID $NEW_PID ==="
    ps aux | grep test_loop | grep -v grep
    sudo kill $NEW_PID
else
    echo "=== ERROR: Restored process not found ==="
    exit 1
fi

echo "=== Test completed successfully ==="
