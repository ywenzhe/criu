#!/bin/bash
set -e

echo "=== Simple CXL Memory Pool Test ==="

# 创建简单的C程序
cat > /tmp/simple_test.c << 'EOF'
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main() {
    // 分配一些内存确保有页面需要checkpoint
    char *buffer = malloc(64 * 1024);  // 64KB
    memset(buffer, 'A', 64 * 1024);

    // 循环计数
    int count = 0;
    while (1) {
        count++;
        sleep(1);
        // 每10秒写一次，避免过多输出
        if (count % 10 == 0) {
            printf("Count: %d\n", count);
            fflush(stdout);
        }
    }
    return 0;
}
EOF

gcc -o /tmp/simple_test /tmp/simple_test.c

CKPT_DIR=/tmp/criu_cxl_simple
rm -rf $CKPT_DIR
mkdir -p $CKPT_DIR

# 方法1：使用--leave-running测试（setsid使其成为session leader）
echo ""
echo "=== Test 1: Dump with --leave-running ==="
setsid /tmp/simple_test </dev/null >/dev/null 2>&1 &
PID=$!
echo "Process PID: $PID"
sleep 2

echo "Dumping (process continues running)..."
sudo ./criu/criu dump -t $PID \
    --images-dir $CKPT_DIR \
    --leave-running \
    --cxl-mem \
    --cxl-dax=/tmp/cxl_test/cxl_mem.dat \
    --cxl-pool-size=512M \
    -vvv \
    --log-file=$CKPT_DIR/dump.log

if [ $? -eq 0 ]; then
    echo "✓ Dump successful (process still running)"

    # 检查pagemap中是否有CXL offset
    echo ""
    echo "Checking pagemap for CXL offsets..."
    sudo crit decode -i $CKPT_DIR/pagemap-$PID.img | grep -A 3 "cxl_pool_offset" | head -20 || true

    # 杀死原进程
    sudo kill $PID
    sleep 1

    echo ""
    echo "=== Test 2: Restore from CXL memory ==="
    sudo ./criu/criu restore \
        --images-dir $CKPT_DIR \
        --cxl-mem \
        --cxl-dax=/tmp/cxl_test/cxl_mem.dat \
        --cxl-pool-size=512M \
        -d \
        -vvv \
        --log-file=$CKPT_DIR/restore.log

    if [ $? -eq 0 ]; then
        echo "✓ Restore successful"
        sleep 2

        # 查找恢复的进程
        NEW_PID=$(pgrep simple_test)
        if [ -n "$NEW_PID" ]; then
            echo "✓ Process restored with PID: $NEW_PID"
            ps aux | grep simple_test | grep -v grep
            sudo kill $NEW_PID
            echo ""
            echo "=== ✓✓✓ ALL TESTS PASSED ✓✓✓ ==="
        else
            echo "✗ Restored process not found"
            exit 1
        fi
    else
        echo "✗ Restore failed"
        echo "Check restore log: $CKPT_DIR/restore.log"
        exit 1
    fi
else
    echo "✗ Dump failed"
    echo "Check dump log: $CKPT_DIR/dump.log"
    sudo kill $PID 2>/dev/null || true
    exit 1
fi
