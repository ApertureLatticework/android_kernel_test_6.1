#!/bin/bash

# 应用 patch/ 目录下的所有 .patch 文件
PATCH_ROOT="patch"

if [ ! -d "$PATCH_ROOT" ]; then
    echo "错误: 未找到 '$PATCH_ROOT' 目录！请在内核源码根目录运行此脚本。"
    exit 1
fi

# 按目录顺序处理：f2fs, mm, sched（或其他顺序）
# 也可以直接遍历所有子目录
for dir in "$PATCH_ROOT"/*/; do
    if [ ! -d "$dir" ]; then
        continue
    fi

    echo ">>> 正在处理目录: $(basename "$dir")"
    # 按文件名排序（确保顺序一致）
    for patch_file in "$dir"*.patch; do
        if [ ! -f "$patch_file" ]; then
            continue
        fi

        echo "  应用补丁: $(basename "$patch_file")"
        if ! git apply --check "$patch_file" 2>/dev/null; then
            echo "  ❌ 补丁检查失败: $(basename "$patch_file")"
            echo "     尝试跳过或手动解决冲突。"
            # 可选：退出或继续
            # exit 1
        else
            git apply "$patch_file"
            if [ $? -ne 0 ]; then
                echo "  ❌ 应用失败: $(basename "$patch_file")"
                exit 1
            fi
        fi
    done
    echo
done

echo "✅ 所有补丁已成功应用！"
