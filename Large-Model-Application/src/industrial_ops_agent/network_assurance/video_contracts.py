"""板端 → 云端 视频留证片段上行契约（``network.edge.video-clips.v1``）。

## 为什么是「清单 + 分片」而不是一个大报文

现有 edge 上行（``/edge/telemetry``、``/edge/wireless-events``）都受
``MAX_TELEMETRY_BODY_BYTES``（1 MB）约束，而事件留证片段是几十 MB 量级
（1640x1232 @ 2000 kbps，异常前后 90 s ≈ 22 MB）。把 1 MB 上限抬高到
几十 MB 会同时抬掉**所有** edge 端点的内存与解析代价——一次畸形请求就能
让一个 worker 占住几十 MB。所以片段拆成：

1. ``POST /edge/video-clips`` —— 签名清单（JSON），登记一个片段；
2. ``POST /edge/video-clips/{clip_id}/chunks/{index}`` —— 逐个签名分片（JSON + base64）。

## 为什么分片载荷用 base64 而不是裸二进制

裸 ``application/octet-stream`` 能让验签直接覆盖原始字节，看起来更省事，
但它会造出**第二套**鉴权约定：现有 ``_read_verified_body`` 靠报文里的
``device_id`` 把签名绑定到设备，而二进制报文里没有 JSON 可解析，设备身份
就只能改从 header 取——于是同一套 Ed25519 信任锚在两类端点上出现两种绑定
方式，审计时无法用一条规则解释全部上行。

统一走 JSON 的代价是 base64 的 33% 体积膨胀（22 MB → 约 29 MB 上行）。
留证是**事件触发**的偶发行为，不是持续推流，这个代价换一套统一的鉴权语义
是划算的；如果将来真要做持续推流，那条通路的瓶颈在协议选择（WebRTC/RTP），
与这里的留证通道无关。

## 幂等

- 清单：``clip_id`` 主键，重复投递是**常态**（板端游标在确认送达前不前移），
  同内容重放计 duplicate，内容冲突则拒绝——证据不可变，不能静默改写。
- 分片：``(tenant, clip_id, chunk_index)`` 唯一。同内容重放计 duplicate，
  **不同内容同索引一律拒绝**：那意味着板端编号错乱，接受它就等于让证据
  悄悄拼错。

## 整体完整性

每个分片自带 ``sha256``，服务端逐片校验（廉价，能在入库前挡住传输损坏）。
整段片段的 ``declared_content_sha256`` 只做**声明留存**，不在请求路径上
回读全部对象去校验——那是一次几十 MB 的同步 IO，会把一个偶发上行变成
长事务。整段校验应由离线 worker 做，本模块不假装已经做过。
"""

from __future__ import annotations

from typing import Literal, Self

from pydantic import Field, model_validator

from industrial_ops_agent.network_assurance.wireless_contracts import _ClosedModel

#: 信封标识。与 ``network.edge.telemetry.v1`` / ``network.edge.wireless-events.v1``
#: 分域：这里只有留证片段，不含任何网络事实或无线事实。
VIDEO_CLIP_UPLINK_SCHEMA_VERSION = "network.edge.video-clips.v1"

#: 单片裸字节上限（base64 前）。640 KiB → base64 约 873 KiB，加上 JSON 外壳
#: 仍在 1 MB 原始体上限之内。留出余量是刻意的：清单字段（clip_id、sha256、
#: 时间戳）也要占几十字节。
MAX_VIDEO_CHUNK_BYTES = 640 * 1024

#: 单片 base64 编码后的上限，供契约自检（不是服务端限制，服务端限制是原始体）。
MAX_VIDEO_CHUNK_B64_CHARS = (MAX_VIDEO_CHUNK_BYTES + 2) // 3 * 4

#: 单片段裸字节上限。防止一个畸形清单声明一个 100 GB 的片段，
#: 让后续分片把存储打满。
MAX_VIDEO_CLIP_BYTES = 256 * 1024 * 1024

#: 单片段分片数上限。与字节上限互为兜底：0 字节分片也能凑数。
MAX_VIDEO_CLIP_CHUNKS = 512

#: 片段时长上限（毫秒）。留证窗口是「异常前后几十秒」，不是 7x24 录像。
MAX_VIDEO_CLIP_DURATION_MS = 10 * 60 * 1000

#: v1 只接受一种编码。放开之前必须先把整段校验与浏览器播放路径打通，
#: 否则「收进来了但谁也放不了」比「拒绝」更糟。
VIDEO_CODEC_H264 = "H264"


class VideoClipManifest(_ClosedModel):
    """签名信封：一个待上传的留证片段的自述。

    ``expected_chunks`` 与 ``total_bytes`` 都由板端声明。服务端**不**
    用它们去预分配任何东西，只用它们判断「收齐了没有」——预分配会把一个
    可伪造的数字直接变成内存压力。
    """

    schema_version: Literal["network.edge.video-clips.v1"] = (
        VIDEO_CLIP_UPLINK_SCHEMA_VERSION
    )

    #: 板端生成的片段标识。必须跨重试稳定——它就是幂等键。
    #:
    #: 取值域与 ``validate_boundary_identifier`` 一致，不是巧合：这个值会
    #: 直接成为对象存储键的一段。若允许 ``..`` 之外的空格、斜杠、控制字符，
    #: 键的解析要么炸在存储层（500），要么在租户前缀之外拼出第二个路径段。
    #: 在契约层拦截，板端拿到的是明确的 422 而不是一个看不懂的后端错误。
    clip_id: str = Field(
        min_length=1, max_length=128, pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"
    )
    device_id: str = Field(min_length=1, max_length=128)
    site_id: str = Field(default="", max_length=128)
    gateway_id: str = Field(default="", max_length=128)

    #: 关联的区域事故（若该片段由事故触发）。空串表示「没有关联事故」，
    #: 而不是「关联到一个空事故」——后者会让按事故检索的查询串味。
    incident_id: str = Field(default="", max_length=128)

    codec: Literal["H264"] = VIDEO_CODEC_H264
    width: int = Field(gt=0, le=8192)
    height: int = Field(gt=0, le=8192)
    fps: int = Field(default=25, ge=1, le=120)

    started_at_ms: int = Field(ge=0)
    ended_at_ms: int = Field(ge=0)

    total_bytes: int = Field(ge=0, le=MAX_VIDEO_CLIP_BYTES)
    expected_chunks: int = Field(ge=0, le=MAX_VIDEO_CLIP_CHUNKS)

    #: 整段码流的 sha256（十六进制小写）。声明留存，见模块 docstring。
    declared_content_sha256: str = Field(default="", max_length=64)

    @model_validator(mode="after")
    def _enforce_shape(self) -> Self:
        if self.ended_at_ms < self.started_at_ms:
            raise ValueError("clip ends before it starts")
        if self.ended_at_ms - self.started_at_ms > MAX_VIDEO_CLIP_DURATION_MS:
            raise ValueError("clip duration exceeds the evidence window limit")
        if self.expected_chunks == 0 and self.total_bytes != 0:
            raise ValueError("a chunkless clip cannot carry bytes")
        if self.declared_content_sha256 and len(self.declared_content_sha256) != 64:
            raise ValueError("declared_content_sha256 is not a sha256 hex digest")
        return self

    @property
    def duration_ms(self) -> int:
        return self.ended_at_ms - self.started_at_ms


class VideoClipChunk(_ClosedModel):
    """签名信封：片段中的一个分片。

    ``payload_b64`` 是码流原始字节的标准 base64（带 padding）。校验顺序是
    **先解 base64、再算 sha256、最后比对声明值**——先比对长度的话，一个
    只改了 base64 padding 的报文就能让两者都「看起来对」。
    """

    schema_version: Literal["network.edge.video-clips.v1"] = (
        VIDEO_CLIP_UPLINK_SCHEMA_VERSION
    )

    device_id: str = Field(min_length=1, max_length=128)
    clip_id: str = Field(
        min_length=1, max_length=128, pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"
    )
    chunk_index: int = Field(ge=0, lt=MAX_VIDEO_CLIP_CHUNKS)

    #: 该分片解码后的字节数。与 base64 解码结果比对，防止 pad 截断。
    size_bytes: int = Field(ge=0, le=MAX_VIDEO_CHUNK_BYTES)

    #: 该分片解码后字节的 sha256。
    content_sha256: str = Field(min_length=64, max_length=64)

    payload_b64: str = Field(max_length=MAX_VIDEO_CHUNK_B64_CHARS)


class VideoClipManifestResult(_ClosedModel):
    """清单投递的回执。板端据此决定「还需要补哪些分片」。"""

    clip_id: str
    accepted: bool
    duplicate: bool
    expected_chunks: int
    received_chunks: int
    complete: bool


class VideoClipChunkResult(_ClosedModel):
    """分片投递的回执。``complete`` 为真表示该片段所有分片已到齐。"""

    clip_id: str
    chunk_index: int
    accepted: bool
    duplicate: bool
    received_chunks: int
    expected_chunks: int
    complete: bool


class VideoClipItem(_ClosedModel):
    """运营端读到的片段摘要（只读事实，不含载荷）。"""

    clip_id: str
    asset_id: str
    site_id: str
    gateway_id: str
    incident_id: str
    codec: str
    width: int
    height: int
    fps: int
    started_at_ms: int
    ended_at_ms: int
    total_bytes: int
    expected_chunks: int
    received_chunks: int
    state: str
    declared_content_sha256: str
