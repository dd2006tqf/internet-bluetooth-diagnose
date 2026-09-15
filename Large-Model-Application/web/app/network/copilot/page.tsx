"use client";

import { Button, Card, Input, List, Tag, Typography } from "antd";
import { useState } from "react";

import { AppShell } from "@/components/AppShell";
import { apiClient } from "@/lib/api/network";
import type { CopilotAnswer } from "@/lib/api/network";

/**
 * Network Copilot panel.
 *
 * Renders the structured diagnosis verbatim — the causal guardrail
 * (guardrails/network_causal.py) has already rejected any report that
 * contradicts the snapshot, so the UI does not need a second opinion layer.
 * When the model path is unavailable the answer falls back to the
 * deterministic chain derived from the snapshot itself (model_used=false).
 */

type Turn = {
  role: "user" | "assistant";
  text: string;
  answer?: CopilotAnswer;
};

export default function NetworkCopilotPage() {
  const [turns, setTurns] = useState<Turn[]>([]);
  const [input, setInput] = useState("");
  const [sending, setSending] = useState(false);

  const send = async () => {
    const question = input.trim();
    if (!question || sending) return;
    setInput("");
    setTurns((prev) => [...prev, { role: "user", text: question }]);
    setSending(true);
    try {
      const body = await apiClient.copilot(question);
      setTurns((prev) => [
        ...prev,
        { role: "assistant", text: body.answer, answer: body },
      ]);
    } catch {
      setTurns((prev) => [
        ...prev,
        { role: "assistant", text: "无法连接诊断服务，请稍后再试。" },
      ]);
    } finally {
      setSending(false);
    }
  };

  return (
    <AppShell>
      <div className="page-stack">
        <div>
          <Typography.Title level={2}>网络排障助手</Typography.Title>
          <Typography.Text type="secondary">
            依据 WeakNet 不可变快照做因果解释，输出受物理因果护栏约束
          </Typography.Text>
        </div>
        <Card>
          <List
            dataSource={turns}
            locale={{ emptyText: "描述你的问题，例如“车间 A 区网关为什么在 14:10 变红？”" }}
            renderItem={(turn) => (
              <List.Item>
                <div style={{ width: "100%" }}>
                  <Typography.Paragraph
                    strong={turn.role === "user"}
                    style={{ marginBottom: 0, whiteSpace: "pre-wrap" }}
                  >
                    {turn.role === "user" ? "你： " : "助手： "}
                    {turn.text}
                  </Typography.Paragraph>
                  {turn.answer ? (
                    <div style={{ marginTop: 8 }}>
                      <Tag color={turn.answer.model_used ? "blue" : "default"}>
                        {turn.answer.model_used ? "模型解释（已过护栏）" : "确定性因果链"}
                      </Tag>
                      <Tag>{turn.answer.asset_id}</Tag>
                    </div>
                  ) : null}
                </div>
              </List.Item>
            )}
          />
        </Card>
        <Card>
          <Input.Search
            placeholder="向排障助手提问…"
            value={input}
            onChange={(event) => setInput(event.target.value)}
            onSearch={send}
            enterButton={<Button type="primary" loading={sending}>发送</Button>}
          />
        </Card>
      </div>
    </AppShell>
  );
}
