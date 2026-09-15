"use client";

import { Alert, Button, Card, List, Space, Tag, Typography } from "antd";
import { useEffect, useRef, useState } from "react";

import {
  type AgUiTransportEvent,
  connectAgUiEventStream,
} from "@/lib/api/client";

import { readAgUiEvents } from "@/lib/ag-ui-stream";

interface AgUiEventView {
  id: string;
  type: string;
  data: Record<string, unknown>;
}

const TERMINAL_EVENTS = new Set(["RUN_FINISHED", "RUN_ERROR"]);

export function AgentEventTimeline({
  agentRunId,
  transportEvents,
  onTerminal,
  onInterrupt,
}: {
  agentRunId: string;
  transportEvents?: readonly AgUiTransportEvent[];
  onTerminal?: () => void;
  onInterrupt?: (interruptId: string | undefined) => void;
}) {
  const [events, setEvents] = useState<AgUiEventView[]>([]);
  const [error, setError] = useState<string>();
  const [connecting, setConnecting] = useState(false);
  const [reconnectNonce, setReconnectNonce] = useState(0);
  const cursor = useRef<string | undefined>(undefined);
  const terminalNotified = useRef(false);
  const usesPostTransport = transportEvents !== undefined;

  useEffect(() => {
    cursor.current = undefined;
    terminalNotified.current = false;
    setEvents([]);
    setError(undefined);
    onInterrupt?.(undefined);
  }, [agentRunId, onInterrupt]);

  useEffect(() => {
    if (transportEvents === undefined) return;
    const projected = transportEvents.map((event, index) => ({
      id: event.id ?? `transport-${index + 1}`,
      type: event.type,
      data: event.data,
    }));
    setEvents(projected);
    const terminal = [...projected]
      .reverse()
      .find((event) => TERMINAL_EVENTS.has(event.type));
    setConnecting(terminal === undefined);
    if (terminal !== undefined && !terminalNotified.current) {
      terminalNotified.current = true;
      onInterrupt?.(inputInterruptId(terminal.data));
      onTerminal?.();
    }
  }, [onInterrupt, onTerminal, transportEvents]);

  useEffect(() => {
    if (usesPostTransport) return;
    let cancelled = false;
    const controller = new AbortController();

    async function consume() {
      let retry = 0;
      setConnecting(true);
      while (!cancelled) {
        try {
          const connectedAt = performance.now();
          let sawEvent = false;
          const response = await connectAgUiEventStream(
            agentRunId,
            cursor.current,
            globalThis.fetch,
            controller.signal,
          );
          if (!response.body) throw new Error("事件流响应没有可读正文");
          setError(undefined);
          for await (const event of readAgUiEvents(response.body)) {
            if (cancelled) break;
            if (!event.id) continue;
            const parsed: AgUiEventView = { ...event, id: event.id };
            sawEvent = true;
            cursor.current = parsed.id;
            setEvents((current) => {
              if (current.some((item) => item.id === parsed.id)) return current;
              return [...current, parsed];
            });
            if (TERMINAL_EVENTS.has(parsed.type) && !terminalNotified.current) {
              terminalNotified.current = true;
              onInterrupt?.(inputInterruptId(parsed.data));
              onTerminal?.();
            } else if (
              parsed.type === "CUSTOM"
              && parsed.data.name === "industrial.approval.required"
            ) {
              onTerminal?.();
            }
          }
          if (cancelled || terminalNotified.current) return;
          if (sawEvent || performance.now() - connectedAt >= 1_000) retry = 0;
          throw new Error("Agent 事件流在终态前结束");
        } catch (caught) {
          if (cancelled || controller.signal.aborted) return;
          if (retry >= 3) {
            setError(caught instanceof Error ? caught.message : "事件流意外中断");
            return;
          }
          retry += 1;
          setError(`事件流中断，正在进行第 ${retry}/3 次自动续传。`);
          await abortableDelay(500 * 2 ** (retry - 1), controller.signal);
        }
      }
    }

    void consume().finally(() => {
      if (!cancelled) setConnecting(false);
    });
    return () => {
      cancelled = true;
      controller.abort();
    };
  }, [agentRunId, onInterrupt, onTerminal, reconnectNonce, usesPostTransport]);

  return (
    <Card
      title="Agent 实时轨迹"
      extra={usesPostTransport ? (
        <Tag color="processing">AG-UI POST 实时传输</Tag>
      ) : (
        <Button
          loading={connecting}
          onClick={() => {
            setError(undefined);
            setReconnectNonce((value) => value + 1);
          }}
        >
          {cursor.current ? `从事件 ${cursor.current} 续传` : "重新连接"}
        </Button>
      )}
    >
      {error ? (
        <Alert
          type="warning"
          showIcon
          message="事件流已中断"
          description="点击续传会通过 Last-Event-ID 从最后确认的服务端序号恢复，不会重放已显示事件。"
        />
      ) : null}
      <List
        locale={{ emptyText: connecting ? "正在等待服务端事件" : "暂无可见事件" }}
        dataSource={events}
        renderItem={(item) => (
          <List.Item>
            <List.Item.Meta
              title={<><Tag>{item.id}</Tag>{eventLabel(item)}</>}
              description={
                item.type === "CUSTOM"
                  && item.data.name === "industrial.tool.completed" ? (
                  <ToolFactTrace data={asRecord(item.data.value) ?? {}} />
                ) : item.type === "TEXT_MESSAGE_CONTENT" ? (
                  <Typography.Paragraph>{textValue(item.data.delta)}</Typography.Paragraph>
                ) : item.type === "MESSAGES_SNAPSHOT" ? (
                  <Typography.Paragraph>{snapshotMessage(item.data)}</Typography.Paragraph>
                ) : (
                  <Typography.Text code>{JSON.stringify(item.data)}</Typography.Text>
                )
              }
            />
          </List.Item>
        )}
      />
    </Card>
  );
}

const TOOL_LABELS: Record<string, string> = {
  "asset.get": "设备主数据",
  "warranty.get": "合同与保修",
  "parts.availability": "备件库存",
  "schedule.availability": "工程师排班",
  "work_orders.history": "历史工单",
};

const DOMAIN_LABELS: Record<string, string> = {
  asset: "设备",
  service_contract: "合同",
  inventory: "库存",
  schedule: "排班",
  work_order: "工单",
};

function ToolFactTrace({ data }: { data: Record<string, unknown> }) {
  const payload = asRecord(data.payload) ?? data;
  const authority = asRecord(payload.authority);
  const fieldSources = asRecord(authority?.field_sources);
  const toolId = textValue(payload.tool_id);
  const domain = textValue(authority?.domain);
  const fieldCount = fieldSources ? Object.keys(fieldSources).length : 0;

  return (
    <Space direction="vertical" size={4}>
      <Space wrap>
        <Tag color="blue">{TOOL_LABELS[toolId] ?? toolId}</Tag>
        {domain ? <Tag color="geekblue">权威域：{DOMAIN_LABELS[domain] ?? domain}</Tag> : null}
        <Typography.Text>来源：{textValue(payload.source)}</Typography.Text>
        <Typography.Text type="secondary">
          数据时点：{formatTimestamp(textValue(payload.as_of))}
        </Typography.Text>
      </Space>
      <Typography.Text type="secondary">
        来源记录：<Typography.Text code copyable>{textValue(payload.source_record_id)}</Typography.Text>
        {authority ? ` · 所有者 ${textValue(authority.owner)} · ${fieldCount} 个权威字段` : ""}
      </Typography.Text>
    </Space>
  );
}

function asRecord(value: unknown): Record<string, unknown> | undefined {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : undefined;
}

function textValue(value: unknown): string {
  return typeof value === "string" ? value : "—";
}

function formatTimestamp(value: string): string {
  const timestamp = new Date(value);
  return Number.isNaN(timestamp.getTime())
    ? value
    : timestamp.toLocaleString("zh-CN", { hour12: false });
}

function eventLabel(event: AgUiEventView): string {
  if (event.type === "CUSTOM") return textValue(event.data.name);
  if (event.type === "STEP_STARTED") {
    return `步骤开始：${textValue(event.data.stepName)}`;
  }
  if (event.type === "STEP_FINISHED") {
    return `步骤完成：${textValue(event.data.stepName)}`;
  }
  if (event.type === "RUN_STARTED") return "Agent 运行开始";
  if (event.type === "RUN_FINISHED") return "Agent 运行结束";
  if (event.type === "RUN_ERROR") return "Agent 运行失败";
  if (event.type === "STATE_SNAPSHOT") return "Agent 状态快照";
  if (event.type === "MESSAGES_SNAPSHOT") return "Agent 消息快照";
  if (event.type === "TEXT_MESSAGE_START") return "诊断结论开始";
  if (event.type === "TEXT_MESSAGE_CONTENT") return "诊断结论";
  if (event.type === "TEXT_MESSAGE_END") return "诊断结论结束";
  return event.type;
}

function snapshotMessage(data: Record<string, unknown>): string {
  if (!Array.isArray(data.messages)) return "—";
  for (const value of data.messages) {
    const message = asRecord(value);
    if (typeof message?.content === "string") return message.content;
  }
  return "—";
}

function inputInterruptId(data: Record<string, unknown>): string | undefined {
  const outcome = asRecord(data.outcome);
  if (outcome?.type !== "interrupt" || !Array.isArray(outcome.interrupts)) return undefined;
  for (const value of outcome.interrupts) {
    const interrupt = asRecord(value);
    if (interrupt?.reason === "input_required" && typeof interrupt.id === "string") {
      return interrupt.id;
    }
  }
  return undefined;
}

async function abortableDelay(milliseconds: number, signal: AbortSignal): Promise<void> {
  await new Promise<void>((resolve) => {
    const timeout = window.setTimeout(resolve, milliseconds);
    signal.addEventListener("abort", () => {
      window.clearTimeout(timeout);
      resolve();
    }, { once: true });
  });
}
