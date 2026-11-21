# 顺序类不变量不能用「最终状态」检验：两种删除顺序收敛到同一个结果

> 发现于：2026-11-21
> 位置：`Large-Model-Application/src/industrial_ops_agent/network_assurance/snapshot_retention.py`
> 影响：一条名字写着「钉住删除顺序」的用例，在顺序被改反之后依然全绿

## 现象

静帧回收要同时删两样东西：库里的行，和对象存储里的字节。写入顺序是
「先对象、后行」，删除必须是它的反序「**先删行、后删对象**」：

| 顺序 | 崩在中间剩下什么 | 后果 |
| --- | --- | --- |
| 先删行、再删对象（正确） | 对象在、行没了 | 孤儿对象，可回收，不说谎 |
| 先删对象、再删行（错误） | 行在、对象没了 | **谎报行**：声称证据已送达，取回 500 |

我为此写了一条用例，名字就叫
`test_object_delete_failure_leaves_no_row_but_counts_orphan`，注释里写着
「这条钉住删除顺序」。

然后我做了一次实验：把实现临时改成「先删对象」，跑一遍。

**29 条用例全绿，包括那一条。**

## 成因

两种顺序的最终状态是**完全一样的**：

```
先删行、再删对象：  删行成功 → 删对象抛错（被吞） → 行没了、对象在
先删对象、再删行：  删对象抛错（被吞） → 删行成功      → 行没了、对象在
```

因为 `_delete_objects` 里有一句

```python
except Exception:  # noqa: BLE001 - 单个对象失败不该中断整轮回收
    failures += 1
```

对象删除失败被吞掉并计数——这是**正确的**行为（不能让一个坏对象挡住整轮
回收），但它同时把顺序信息抹平了。用例断言的 `deleted == 1` 与
`_count(db) == 0` 在两种顺序下都成立。

所以那条用例实际验的是「**行最终会被删掉**」，不是顺序。测试名与它的
注释都在撒谎，而这不会报错——它只是安静地通过。

这是「判据在成功和失败时输出相同」的又一个变体，但比之前几例更隐蔽：
前面几例是**两个不同来源**得出相同结果，这一例是**同一条代码路径在两种
实现下**得出相同结果。差别在于，前者靠换数据源能戳穿，后者只能靠**换实现**。

## 修法

唯一能区分顺序的是**中途失败**：让第一步就炸，然后看第二步有没有被动过。

```python
def test_row_delete_failure_leaves_object_untouched(db, store):
    object_key = _seed(db, store, "snap-old", age_days=30)

    service = SnapshotRetentionService(db)
    original = service._delete_rows

    def _explode(context, image_ids):
        raise OperationalError("simulated crash in row deletion", None, Exception())

    service._delete_rows = _explode
    try:
        with pytest.raises(OperationalError):
            _run(service, store, retention_days=7, apply=True)
    finally:
        service._delete_rows = original

    # 行还在（删行本身就失败了），这是预期内的。
    assert _count(db) == 1
    # 关键断言：对象必须还在。若实现先删对象，这里会是 False，
    # 而行还在——一个指向缺失对象的谎报行。
    assert _object_exists(store, object_key)
```

验法是**把实现改反一次，看它变不变红**。改反后这条用例确实变红，而原来
那 28 条仍然是绿的——这才证明它真的在测顺序。

## 边界

- 这条用例 monkeypatch 了私有方法 `_delete_rows`。它测的是「两步之间的
  顺序」，代价是与内部结构耦合。如果将来把删除拆成三个步骤，它需要跟着改。
  这是可以接受的：**顺序不变量本来就不存在于公开接口上**，不注入失败就没有
  别的观测点。
- 它只覆盖「第一步抛异常」。进程在两步之间被 `kill -9`、或在两步之间的
  IO 上挂死，它都覆盖不到——那些要靠真实断电演练，不是单测能给的。
- 反过来也不成立：这条用例绿，只说明「删行失败时对象没被动」。它**不**证明
  「对象删失败时行仍会被删」，也**不**证明「删行真的提交了」。三条要用例。

## 教训

**「最终状态正确」不等于「过程正确」。凡是顺序类不变量，判据必须落在
中途失败上；只在终点比对，两种实现和三种实现都一样通过。**

再往上一层：写完一条声称验证某个性质的用例之后，问一句
**「我把这个性质弄反，它会变红吗？」** 如果不会，那它验的是别的东西。
这个问题比我当时写的任何注释都更值钱——注释说它钉住顺序，实验说它没有。
