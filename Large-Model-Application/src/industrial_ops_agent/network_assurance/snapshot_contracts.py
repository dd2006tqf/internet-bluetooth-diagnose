"""端侧事故静帧（单张图片）的上行契约。

## 为什么不是扩 ``video_contracts.py`` 的 codec 枚举

``video_contracts.py`` 把 ``codec`` 钉死为 ``Literal["H264"]``，注释写明这是
刻意限制：*"放开之前必须先把整段校验与浏览器播放路径打通，否则「收进来了
但谁也放不了」比「拒绝」更糟。"*

静帧不是那个东西。拿 ``VideoClipManifest`` 承载一张 JPEG 会同时破坏两处语义：

| ``VideoClipManifest`` 字段 | 片段里的含义 | 一张静帧里是什么 |
| --- | --- | --- |
| ``fps`` | 码流帧率 | **无意义**（43KB 常量填什么都行） |
| ``expected_chunks`` | 分片总数 | 恒 1，但仍要走两段式 |
| ``started_at_ms`` / ``ended_at_ms`` | 片段覆盖的时间窗口 | 时刻，不是窗口 |
| ``declared_content_sha256`` | 整段码流摘要 | 单张图摘要 |

后三个还只是别扭，``fps`` 那一行才是真问题：一个恒被填成 25 的字段会被
下游当成事实读，而它描述的东西不存在。

所以另开契约，语义只有一条：**某个设备在某时刻拍到了一张图，这是它。**

## 为什么单张不分片

实测 IMX219 @1640x1232、quality=70 的 JPEG 是 118~128 KB（真实场景），
最坏情况（满屏合成纹理）也才 624 KB，都在 ``MAX_SNAPSHOT_BYTES`` 之内。
分片是为几十 MB 的片段准备的机制，为一张 118 KB 的图引入清单 + 分片两段式，
只是把两个失败点换成一个失败点。

上限仍按 ``MAX_VIDEO_CHUNK_BYTES`` 对齐：它和片段分片共享同一个 1 MB 原始体
上限（base64 膨胀 33% 后仍要装得下），不是随手取的大数。

## 为什么保留 ``content_sha256`` 三元组

与分片端点同一套顺序：**先解 base64、再算摘要、最后比对声明值**。
先比长度的话，一个只改了 padding 的报文就能让两者都"看起来对"。
"""

from __future__ import annotations

from typing import Literal, Self

from pydantic import Field, model_validator

from industrial_ops_agent.network_assurance.video_contracts import (
    MAX_VIDEO_CHUNK_B64_CHARS,
    MAX_VIDEO_CHUNK_BYTES,
)
from industrial_ops_agent.network_assurance.wireless_contracts import _ClosedModel

#: 信封标识。与 ``network.edge.video-clips.v1`` 分域：它是「一段码流」，
#: 这里是「一张图」。两者共用 base64 分片载荷的写法，但清单/分片两段式
#: 与单张一次性的生命周期完全不同，混在一个 schema_version 里会让消费方
#: 必须先看 codec 才能决定怎么解析。
SNAPSHOT_UPLINK_SCHEMA_VERSION = "network.edge.snapshot-images.v1"

#: 本版本只接受 JPEG。
#:
#: 与 ``VIDEO_CODEC_H264`` 同一个理由：浏览器能直接显示 JPEG，不需要任何
#: 解码器协商。将来加 HEVC/AV1 静帧之前，必须先确认播放路径通。
SNAPSHOT_CODEC_JPEG = "JPEG"

#: 单张图片裸字节上限（base64 前）。与片段分片共用同一上限——它们都受
#: edge 端点 1 MB 原始体约束，base64 之后还要留 JSON 外壳的余量。
MAX_SNAPSHOT_BYTES = MAX_VIDEO_CHUNK_BYTES

#: base64 编码后的上限，供契约自检（不是服务端限制）。
MAX_SNAPSHOT_B64_CHARS = MAX_VIDEO_CHUNK_B64_CHARS


class SnapshotImage(_ClosedModel):
    """一张待上传的事故静帧。

    ``image_id`` 是板端生成的幂等键，必须跨重试稳定。它与 ``clip_id``
    同形（同一个 `validate_boundary_identifier` 约束）：都要直接成为对象
    存储键的一段，放开空格/斜杠/控制字符会让键的解析要么炸在存储层，
    要么在租户前缀之外拼出第二个路径段。
    """

    schema_version: Literal["network.edge.snapshot-images.v1"] = (
        SNAPSHOT_UPLINK_SCHEMA_VERSION
    )

    #: 板端生成的图片标识。跨重试稳定——它就是幂等键。
    image_id: str = Field(
        min_length=1, max_length=128, pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"
    )
    device_id: str = Field(min_length=1, max_length=128)
    site_id: str = Field(default="", max_length=128)
    gateway_id: str = Field(default="", max_length=128)

    #: 关联的区域事故（若该图由事故触发）。空串表示「没有关联事故」，
    #: 而不是「关联到一个空事故」——后者会让按事故检索的查询串味。
    incident_id: str = Field(default="", max_length=128)

    codec: Literal["JPEG"] = SNAPSHOT_CODEC_JPEG
    width: int = Field(gt=0, le=8192)
    height: int = Field(gt=0, le=8192)

    #: 拍摄时刻（板端墙钟毫秒）。是**时刻**不是窗口：一张图没有时长。
    captured_at_ms: int = Field(ge=0)

    #: 解码后的字节数。与 base64 解码结果比对，防止 pad 截断。
    size_bytes: int = Field(ge=0, le=MAX_SNAPSHOT_BYTES)

    #: 解码后字节的 sha256（十六进制小写）。
    content_sha256: str = Field(min_length=64, max_length=64)

    payload_b64: str = Field(min_length=1, max_length=MAX_SNAPSHOT_B64_CHARS)

    @model_validator(mode="after")
    def _enforce_shape(self) -> Self:
        # 空载荷不是「一张空白图」，是「板端没拍到还发了请求」。让它在契约层
        # 就失败，比在对象存储里留下一个 0 字节对象好——后者会被枚举当成
        # 一张真图，而任何查看器都打不开它。
        if self.size_bytes == 0:
            raise ValueError("a snapshot image cannot carry zero bytes")
        return self


class SnapshotImageResult(_ClosedModel):
    """静帧投递的回执。"""

    image_id: str
    accepted: bool
    duplicate: bool
    size_bytes: int
