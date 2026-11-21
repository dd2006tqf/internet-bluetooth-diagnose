"use client";

import { Alert, Button, Card, Empty, Image, Space, Spin, Tag, Typography } from "antd";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { apiClient } from "@/lib/api/network";
import type { SnapshotImageItem } from "@/lib/api/network";

const { Text } = Typography;

function formatBytes(value: number): string {
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
}

/**
 * 事故静帧查看器。
 *
 * ## 为什么图要一张张按需取，而不是一次性全拉
 *
 * 列表接口刻意**不含载荷**（那是契约：摘要只读事实），所以每张图都是一次
 * 独立请求。一次事故可能几十张图，全拉会同时开几十个 fetch、几十个 blob URL
 * 常驻内存——一个 1640×1232 的 JPEG 解码后是 8 MB 位图，十张就是 80 MB。
 * 所以这里只在缩略图进入可视区时才取字节。
 *
 * ## 为什么必须自己 revoke blob URL
 *
 * `URL.createObjectURL` 创建的对象**不会被 GC 回收**，只要还有引用就一直
 * 占着内存，直到页面卸载或显式 `revokeObjectURL`。翻十次页就泄漏几百 MB，
 * 而现象是「用久了浏览器变卡」——没人会想到是这几十张图。所以：
 *
 * - 组件卸载时 revoke 全部；
 * - 列表变化（换事故筛选）时 revoke 上一批；
 * - **加载失败不 revoke**：失败时根本没有 URL。
 *
 * ## 为什么取回失败要显式说出来
 *
 * `GET .../content` 的 500 语义是「已登记的证据取不回来」——它不是「还没
 * 上传完」（那张图根本不会有行），也不是「图太大了」。这是整条留证链路里
 * 唯一会承认「证据丢了」的地方。把它渲染成一个破图标，就等于让这个信号
 * 消失。所以失败时给一句人话，并保留重试。
 */
export function SnapshotPanel({ assetId, incidentId }: { assetId: string; incidentId?: string }) {
  const [items, setItems] = useState<SnapshotImageItem[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [urls, setUrls] = useState<Record<string, string>>({});
  const [failed, setFailed] = useState<Record<string, string>>({});

  // 存一份最新 urls 的镜像，供卸载时清理用。
  // 直接用 state 会在 effect 闭包里读到挂载时的旧值（空对象），于是最后一批
  // URL 永远漏掉——泄漏恰好发生在最常发生的那条路径上（离开页面）。
  const urlsRef = useRef<Record<string, string>>({});

  const revokeAll = useCallback((map: Record<string, string>) => {
    for (const url of Object.values(map)) URL.revokeObjectURL(url);
  }, []);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError(null);

    apiClient
      .listAssetSnapshotImages(assetId, { incidentId })
      .then((rows) => {
        if (cancelled) return;
        setItems(rows);
      })
      .catch((err) => {
        if (cancelled) return;
        // 列表失败与「没有静帧」必须分开：前者是接口/权限问题，后者是事实。
        // 混成一个空列表会让一次 500 看起来像「这台设备从没拍过照片」。
        setError(err?.message ?? String(err));
        setItems([]);
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });

    return () => {
      cancelled = true;
    };
  }, [assetId, incidentId]);

  // 列表换了一批 → 上一批 blob URL 全部作废，必须 revoke。
  useEffect(() => {
    return () => {
      revokeAll(urlsRef.current);
      urlsRef.current = {};
      setUrls({});
      setFailed({});
    };
  }, [assetId, incidentId, revokeAll]);

  const loadImage = useCallback(
    async (imageId: string) => {
      if (urlsRef.current[imageId]) return;
      try {
        const url = await apiClient.getSnapshotImageObjectUrl(assetId, imageId);
        urlsRef.current = { ...urlsRef.current, [imageId]: url };
        setUrls(urlsRef.current);
      } catch (err: any) {
        setFailed((prev) => ({
          ...prev,
          [imageId]: err?.message ?? String(err),
        }));
      }
    },
    [assetId],
  );

  const retry = useCallback(
    (imageId: string) => {
      setFailed((prev) => {
        const next = { ...prev };
        delete next[imageId];
        return next;
      });
      void loadImage(imageId);
    },
    [loadImage],
  );

  const ordered = useMemo(
    // 列表接口已按拍摄时刻倒序返回；这里不再重排，避免前端与后端的排序口径
    // 各说一套。真要改顺序时改的是契约，不是这里。
    () => items,
    [items],
  );

  if (loading) {
    return (
      <Card size="small" title="事故静帧留证">
        <Spin />
      </Card>
    );
  }

  if (error) {
    return (
      <Card size="small" title="事故静帧留证">
        <Alert
          type="error"
          showIcon
          message="静帧列表加载失败"
          description={`${error}（这是接口或权限问题，不代表这台设备没有静帧）`}
        />
      </Card>
    );
  }

  if (ordered.length === 0) {
    return (
      <Card size="small" title="事故静帧留证">
        <Empty
          image={Empty.PRESENTED_IMAGE_SIMPLE}
          description={
            incidentId
              ? "这次事故没有留下静帧（可能拍摄时刻不在事故窗口内）"
              : "这台网关还没有上传过静帧"
          }
        />
      </Card>
    );
  }

  return (
    <Card
      size="small"
      title={`事故静帧留证（${ordered.length} 张）`}
      extra={
        <Text type="secondary" style={{ fontSize: 12 }}>
          逐张按需取回，取回时服务端会重算 sha256 与库中记录比对
        </Text>
      }
    >
      <Space wrap size={12}>
        {ordered.map((row) => {
          const url = urls[row.image_id];
          const err = failed[row.image_id];
          return (
            <Card
              key={row.image_id}
              size="small"
              style={{ width: 260 }}
              cover={
                url ? (
                  <Image
                    src={url}
                    alt={`事故静帧 ${row.image_id}`}
                    width={258}
                    style={{ objectFit: "cover" }}
                  />
                ) : err ? (
                  <div
                    style={{
                      height: 180,
                      display: "flex",
                      flexDirection: "column",
                      alignItems: "center",
                      justifyContent: "center",
                      gap: 8,
                      padding: 12,
                    }}
                  >
                    <Tag color="red">取回失败</Tag>
                    <Text type="secondary" style={{ fontSize: 12, textAlign: "center" }}>
                      已登记的证据取不回来：{err}
                    </Text>
                    <Button size="small" onClick={() => retry(row.image_id)}>
                      重试
                    </Button>
                  </div>
                ) : (
                  <div
                    style={{
                      height: 180,
                      display: "flex",
                      alignItems: "center",
                      justifyContent: "center",
                    }}
                  >
                    <Button size="small" onClick={() => void loadImage(row.image_id)}>
                      加载图片
                    </Button>
                  </div>
                )
              }
            >
              <Space direction="vertical" size={2} style={{ width: "100%" }}>
                {/* 拍摄时刻来自板端自述，收到时刻来自云端。两者不同是正常的：
                    板子没有 RTC，墙钟可能偏。同时显示才看得出这一点。 */}
                <Text style={{ fontSize: 12 }}>
                  拍摄 {new Date(row.captured_at_ms).toLocaleString()}
                </Text>
                <Text type="secondary" style={{ fontSize: 12 }}>
                  收到 {new Date(row.received_at).toLocaleString()}
                </Text>
                <Text type="secondary" style={{ fontSize: 12 }}>
                  {row.width}×{row.height} · {formatBytes(row.size_bytes)} · {row.codec}
                </Text>
                {row.incident_id ? (
                  <Text code style={{ fontSize: 11 }}>
                    {row.incident_id}
                  </Text>
                ) : (
                  <Text type="secondary" style={{ fontSize: 11 }}>
                    未关联事故
                  </Text>
                )}
              </Space>
            </Card>
          );
        })}
      </Space>
    </Card>
  );
}
