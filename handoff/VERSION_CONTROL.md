# 完整工程版本管理规范

## 目标

每次公开更新必须形成一个完整的 Git 工程快照。远程推送可能只传输增量对象，但一个
提交或版本标签在逻辑上包含当时全部受控文件；检出该标签后不能依赖其他分支上的文件。

## 每次更新必须包含

1. 源代码、CMake/交叉编译配置和相关脚本。
2. 与源码匹配的模型、元数据、标签图和板端发布二进制。
3. 发布二进制的 `release/rv1126/SHA256SUMS`。
4. README、测试流程、实测状态和 `CHANGELOG.md`。
5. 数据选择清单、依赖版本和可重复生成未入库资源的命令。

以下内容不提交：Python 虚拟环境、缓存、CMake 中间目录、完整 Rockchip SDK、临时
日志以及可通过清单重新生成的 COCO 图片/标签。它们不是运行发布包或阅读源码所必需，
排除规则保存在 `.gitignore`。

## 发布步骤

```bash
cd "$HOME/桌面/rv1126_object_training"

# 1. 修改 VERSION 和 CHANGELOG，并完成源码、文档、模型及发布包更新。
# 2. 运行主机测试和 ARM 交叉编译，然后暂存本版本的全部文件。
git add -A
bash scripts/verify_checkout.sh
git diff --check

# 3. 创建完整工程提交。
git diff --cached --stat
git commit -m "说明本次完整更新"

# 4. 确认提交后工作树干净，再建立带注释的版本标签。
git status --short
git tag -a v1.2.1 -m "RV1126 object detection v1.2.1"

# 5. 同步两个远程仓库的分支和标签。
git push origin main
git push origin v1.2.1
git push gitee main
git push gitee v1.2.1
```

后续版本按变更范围递增，例如：

- 兼容性破坏或平台重构：`2.0.0`
- 新功能且保持兼容：`1.1.0`
- 修复、参数或文档更新：`1.0.1`

## 获取或对比完整版本

```bash
git clone git@github.com:wangwqn/rv1126.git
cd rv1126
git checkout v1.2.1
bash scripts/verify_checkout.sh

git tag --list
git log --oneline --graph --decorate --all
git diff --stat v1.2.0..v1.2.1
git diff v1.2.0..v1.2.1
```

若需要两个版本同时存在，不在当前目录来回切换，使用独立工作树：

```bash
git worktree add ../rv1126-v1.2.1 v1.2.1
```
