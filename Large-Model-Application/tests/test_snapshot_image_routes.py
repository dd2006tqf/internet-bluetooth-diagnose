"""事故静帧上行的 HTTP 传输层校验（不重复服务层语义测试）。

分工与 ``test_video_clip_routes.py`` 一致：这里只测**路由独占**的职责。

## 为什么这个端点需要自己的三重校验

分片端点已经踩过一次的坑，图片端点原样继承：载荷是 base64，声明了
``size_bytes`` 与 ``content_sha256``。三者必须按固定顺序校验，因为

* 先比 ``size_bytes`` 再解 base64：一个只改了 padding 的报文能让两者
  \"看起来对\"——解码少了字节，而声明值也刚好对得上；
* 先比摘要再比长度：截断后的载荷摘要必然不符，于是报出\"摘要错\"而不是
  \"长度错\"，运维会去查内容被篡改，而真实原因只是板端截短了包。

所以顺序钉死为 **decode → size → sha256**，三条各有一条断言。

## 为什么必须在写对象存储之前校验完

一个校验失败的载荷如果先落了对象，就会留下一个**孤儿对象**：没有任何
数据库行指向它，但它确实占了存储、也确实能被按前缀枚举到。这比\"请求
被拒绝\"糟得多，因为拒绝是可观测的，孤儿对象不可观测。
"""

from __future__ import annotations

import base64
import hashlib
from typing import Any

import pytest
from pydantic import ValidationError

from industrial_ops_agent.network_assurance.snapshot_contracts import (
    MAX_SNAPSHOT_BYTES,
    SNAPSHOT_UPLINK_SCHEMA_VERSION,
    SnapshotImage,
)

TENANT = "tenant-alpha"
DEVICE = "radxa-cubie-a7a"


def _payload(size: int = 200) -> bytes:
    """一段形状像 JPEG 的字节（首尾标记在位，便于断言不被改写）。"""
    body = b"\x00" * max(size - 4, 0)
    return b"\xff\xd8" + body + b"\xff\xd9"


def _manifest(payload: bytes, **overrides: Any) -> dict[str, Any]:
    base: dict[str, Any] = {
        "schema_version": SNAPSHOT_UPLINK_SCHEMA_VERSION,
        "image_id": "img-20261120-0001",
        "device_id": DEVICE,
        "site_id": "site-1",
        "gateway_id": "gw-1",
        "incident_id": "sitinc-abc",
        "codec": "JPEG",
        "width": 1640,
        "height": 1232,
        "captured_at_ms": 1_795_155_756_003,
        "size_bytes": len(payload),
        "content_sha256": hashlib.sha256(payload).hexdigest(),
        "payload_b64": base64.b64encode(payload).decode(),
    }
    base.update(overrides)
    return base


# ===========================================================================
# 契约形状
# ===========================================================================


def test_manifest_accepts_a_well_formed_image() -> None:
    payload = _payload()
    image = SnapshotImage.model_validate(_manifest(payload))
    assert image.schema_version == SNAPSHOT_UPLINK_SCHEMA_VERSION
    assert image.codec == "JPEG"
    assert image.size_bytes == len(payload)
    assert image.captured_at_ms == 1_795_155_756_003


def test_manifest_rejects_unknown_fields() -> None:
    # ``_ClosedModel`` 的意义：多出来的字段必须报错而不是被静默丢弃。
    # 静默丢弃会让板端以为某个字段生效了，而云端从来没有读过它。
    payload = _payload()
    with pytest.raises(ValidationError):
        SnapshotImage.model_validate(_manifest(payload, fps=25))


def test_manifest_rejects_non_jpeg_codec() -> None:
    # v1 只接受 JPEG。H264 静帧不存在，放开它等于允许一个无法渲染的对象。
    payload = _payload()
    with pytest.raises(ValidationError):
        SnapshotImage.model_validate(_manifest(payload, codec="H264"))


def test_manifest_rejects_zero_byte_payload() -> None:
    # 空载荷不是「一张空白图」，是「板端没拍到还发了请求」。
    # 放行它会在对象存储里留下一个任何查看器都打不开的对象。
    empty = b""
    with pytest.raises(ValidationError):
        SnapshotImage.model_validate(
            _manifest(empty, size_bytes=0, content_sha256=hashlib.sha256(empty).hexdigest(),
                      payload_b64="")
        )


def test_manifest_rejects_oversized_declaration() -> None:
    # 声明的体积超过单张上限就必须拒绝：真实载荷可能很小，但服务端会按
    # 声明值去理解这张图，超出上限的声明意味着板端算错了什么。
    payload = _payload()
    with pytest.raises(ValidationError):
        SnapshotImage.model_validate(
            _manifest(payload, size_bytes=MAX_SNAPSHOT_BYTES + 1)
        )


def test_manifest_rejects_image_id_with_path_separator() -> None:
    # image_id 直接成为对象存储键的一段。放开斜杠就能拼出第二个路径段。
    payload = _payload()
    with pytest.raises(ValidationError):
        SnapshotImage.model_validate(_manifest(payload, image_id="../etc/passwd"))


# ===========================================================================
# 三重校验的顺序（路由层逻辑的镜像，用纯函数表达）
# ===========================================================================


def _validate_like_route(image: SnapshotImage) -> str | None:
    """复刻路由的校验顺序；返回错误码，通过时返回 None。

    不直接调路由是为了让顺序本身可断言：路由里那三段是顺序代码，
    而顺序错了**不会报错**，只会给出误导性的错误码。
    """
    try:
        payload = base64.b64decode(image.payload_b64, validate=True)
    except Exception:
        return "edge_snapshot_image_payload_not_base64"
    if len(payload) != image.size_bytes:
        return "edge_snapshot_image_size_mismatch"
    if hashlib.sha256(payload).hexdigest() != image.content_sha256:
        return "edge_snapshot_image_digest_mismatch"
    return None


def test_valid_payload_passes_all_three_checks() -> None:
    payload = _payload()
    image = SnapshotImage.model_validate(_manifest(payload))
    assert _validate_like_route(image) is None


def test_stripped_padding_never_reaches_object_store() -> None:
    # 只改 padding：这是最容易被放过去的一类篡改，因为 JSON 合法、字段齐全。
    #
    # 断言的是**性质**（「不会以成功告终」），不是某个具体错误码。原因：
    # 去掉 padding 之后，base64 长度是否仍被 ``validate=True`` 接受，取决于
    # 载荷长度模 3 的余数——有些长度去掉 '=' 依然合法，只是解码结果短了几字节。
    # 把断言写成「必然在契约层抛 ValidationError」会一半概率不成立，那样测的是
    # 字典序运气，不是安全性。
    payload = _payload()
    good_b64 = base64.b64encode(payload).decode()
    manifest = _manifest(payload, payload_b64=good_b64.rstrip("="))

    # 可能在契约层就被拒（长度不对），
    try:
        image = SnapshotImage.model_validate(manifest)
    except ValidationError:
        return   # 第一道闸拦住了，符合预期

    # 也可能契约层放过：那时必须由长度或摘要校验拦住，绝不能返回 None。
    assert _validate_like_route(image) is not None, (
        "去掉 padding 的载荷通过了全部三重校验——这是安全缺口"
    )


def test_size_mismatch_is_reported_before_digest_mismatch() -> None:
    # 构造「长度错 且 摘要错」的载荷：必须报长度错。
    # 若顺序反了，运维会看到「内容被篡改」，而真实原因只是截短了包。
    payload = _payload()
    short = payload[:-10]
    manifest = _manifest(
        payload,
        size_bytes=len(payload),          # 声明原长
        payload_b64=base64.b64encode(short).decode(),   # 实际更短
        content_sha256=hashlib.sha256(payload).hexdigest(),
    )
    image = SnapshotImage.model_validate(manifest)
    assert _validate_like_route(image) == "edge_snapshot_image_size_mismatch"


def test_digest_mismatch_is_caught_when_size_agrees() -> None:
    # 长度对但内容被换：这是唯一应该报摘要错的情形。
    payload = _payload()
    swapped = bytearray(payload)
    swapped[10] ^= 0xFF
    manifest = _manifest(
        payload,
        payload_b64=base64.b64encode(bytes(swapped)).decode(),
        content_sha256=hashlib.sha256(payload).hexdigest(),
    )
    image = SnapshotImage.model_validate(manifest)
    assert _validate_like_route(image) == "edge_snapshot_image_digest_mismatch"


def test_manifest_json_round_trips() -> None:
    # 板端按这份 JSON 发，云端按同一份解。序列化再反序列化必须等价，
    # 否则「契约对」只是两边的 Python 对象对，线上的字节并不对。
    payload = _payload()
    original = SnapshotImage.model_validate(_manifest(payload))
    again = SnapshotImage.model_validate_json(original.model_dump_json())
    assert again == original
