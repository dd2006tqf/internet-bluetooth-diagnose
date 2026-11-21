# 内存对象存储让「写入顺序」和「取回校验」两条约束同时变成不可检验

> 发现于：2026-11-21
> 位置：`scripts/rehearsal_server.py`（装配点）/ `object_store/memory.py`
> 影响：整套留证链路的两条核心不变量，在最常用的排练环境里从来没有被执行过

## 现象

静帧上行链路「端到端跑通了」：板端拍图、签名、上传，云端落库，字段与板端
事实逐字一致，摘要也一致。所有这些都通过了真机验证。

但下面两个问题都无法回答：

1. 「写入顺序：先写对象存储、再写数据库」——如果反过来会怎样？
2. 「取回对象，重算 sha256，与库里的 `content_sha256` 比对」——能过吗？

不是「没测」，是**测不了**。

```python
# scripts/rehearsal_server.py（修改前）
app.state.object_store = InMemoryTenantObjectStore()
```

```python
# object_store/memory.py
async def put(self, context, key, content) -> None:
    key.assert_owned_by(context)
    self._objects[key.value] = content      # 字典赋值，不会失败
```

## 成因

两条约束各被内存实现的一个重要性质抹掉了：

| 约束 | 内存实现里的实际情况 | 后果 |
| --- | --- | --- |
| 先写对象、再写库（对象失败就不写库） | `put()` 几乎不可能失败 | 那个 503 分支从未被走到 |
| 取回 → 重算 sha256 → 比对 | 进程重启对象即丢 | 取回这一步根本做不到 |

更麻烦的是**这两条都不会报错**。测试全绿、端到端跑通、日志一切正常——因为
内存实现确实完成了它承诺的事。它只是不承诺那两件真正重要的事。

这是「假装没有」的反面：它连假装都不需要，因为没人问过它。

## 修法

三个改动，缺一不可：

1. `object_store/local_disk.py`：`LocalDiskTenantObjectStore`，真落盘、跨重启存活。
2. `object_store/integrity.py`：`verify_retrieved_digest()` 独立成模块。判据与
   存储介质无关——把它写在 `local_disk.py` 里会让生产环境（用 MinIO）的路由
   不得不 import 一个叫 "local_disk" 的东西，那是语义错误。
3. `GET /assets/{asset_id}/snapshot-images/{image_id}/content`：下载时**真的**
   重算摘要并与行里的 `content_sha256` 比对，不符则 500。

`rehearsal_server.py` 用环境变量切换，默认行为不变：

```python
object_store_dir = os.environ.get("REHEARSAL_OBJECT_STORE_DIR", "")
if object_store_dir:
    app.state.object_store = LocalDiskTenantObjectStore(object_store_dir)
else:
    app.state.object_store = InMemoryTenantObjectStore()
```

落盘实现本身还有两个坑，都是「不做就会留下看起来像证据的垃圾」：

- 写入必须 `mkstemp(dir=path.parent)` + `fsync` + `os.replace`。临时文件放在
  别的目录会让 `os.replace` 跨文件系统、退化成复制+删除、不再原子；少了
  `fsync`，ext4 delayed allocation 会留下**名字在、内容空**的文件。
- 路径校验必须三层：租户边界 → 逐段拒绝 `..`/`.`/空段/反斜杠 → `resolve()`
  之后 `is_relative_to(root)`。第三层不是冗余：前两层挡不住「根目录内某个
  中间目录其实是软链接」，而那种情况下前两层全部失效。

## 边界

补上的证据有明确范围：

- 验证在 `TestClient` + SQLite + 真目录里完成，**真机 + 真 Postgres + 落盘
  实现**的组合仍未跑。
- **503 分支（对象写失败 → 不写库）仍未被端到端走到**。落盘实现让这个分支
  变得可触发（磁盘满、权限、路径攻击），但还没有一条用例把它钉死。
- 静帧没有保留/配额策略。落盘会让磁盘持续增长，这也是它做成开关而非默认的
  原因之一。

## 教训

**测试替身会自动抹掉它无法表达的失败模式，而且抹得无声无息。**

内存实现不是「简化版」，它是「只实现成功路径」的版本。凡是「某个操作会失败
时应该怎么办」的约束，在它下面永远处于未检验状态——而这类约束恰恰是留证、
审计、幂等这些场景的全部价值所在。

判断一个替身是否掩盖了什么，问一句：**它有没有可能失败？** 如果不会，
那所有以失败为前提的分支都是死代码。
