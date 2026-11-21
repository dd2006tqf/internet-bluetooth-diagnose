"""对象内容的完整性判据：与存储介质无关。

为什么单独一个模块
==================

「取回的内容是不是当初那份」这个判断，与对象存在磁盘、内存还是 S3 上
**毫无关系**。把它写在 ``local_disk.py`` 里会造成两个后果：

1. 路由层为了做下载校验，得 import 一个叫 "local_disk" 的模块——而生产环境
   用的是 ``MinioTenantObjectStore``，那个 import 在语义上就是错的；
2. 将来换存储实现时，会有人以为校验也要跟着换一遍。它不该换：判据是判据,
   存储是存储。

所以判据单独住在这里，任何实现都可以用。
"""

from __future__ import annotations

import hashlib


class DigestMismatchError(RuntimeError):
    """取回的对象与声明的摘要不一致。

    这不是「读取失败」——读取成功了，只是内容不是我们以为的那一份。
    把它与「对象不存在」混为一谈会让「证据被篡改」看起来像「证据还没到」，
    而两者要采取的处置完全相反：
    前者要立刻查是谁改的，后者只需要再等一会儿。
    """


def verify_retrieved_digest(content: bytes, expected_sha256: str) -> None:
    """取回校验：重算 sha256 并比对。

    这是留证链路唯一能证明「存进去的东西可以原样取回来」的手段。
    单独抽成函数而不是塞进路由，因为它是**判据**——判据必须能用一行
    命令独立验证，也能被单测直接钉死。
    """

    actual = hashlib.sha256(content).hexdigest()
    if actual != expected_sha256:
        raise DigestMismatchError(
            f"retrieved object digest {actual[:16]} != expected {expected_sha256[:16]}"
        )
