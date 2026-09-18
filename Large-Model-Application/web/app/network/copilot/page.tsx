"use client";

import {
  Alert,
  Button,
  Card,
  Drawer,
  Form,
  Input,
  InputNumber,
  List,
  Space,
  Tag,
  Typography,
  message,
} from "antd";
import { useEffect, useState } from "react";

import { AppShell } from "@/components/AppShell";
import { apiClient } from "@/lib/api/network";
import type { CopilotAnswer } from "@/lib/api/network";

type Turn = {
  role: "user" | "assistant";
  text: string;
  answer?: CopilotAnswer;
};

export default function NetworkCopilotPage() {
  const [turns, setTurns] = useState<Turn[]>([]);
  const [input, setInput] = useState("");
  const [sending, setSending] = useState(false);

  // 配置抽屉状态
  const [drawerOpen, setDrawerOpen] = useState(false);
  const [configLoading, setConfigLoading] = useState(false);
  const [savingConfig, setSavingConfig] = useState(false);
  const [testingConfig, setTestingConfig] = useState(false);
  const [testResult, setTestResult] = useState<{
    ok: boolean;
    message: string;
    latency_ms?: number;
  } | null>(null);

  const [form] = Form.useForm();

  // 加载当前大模型配置
  const loadConfig = async () => {
    setConfigLoading(true);
    try {
      const cfg = await apiClient.getCopilotConfig();
      form.setFieldsValue({
        upstream_url: cfg.upstream_url,
        model_name: cfg.model_name,
        timeout_seconds: cfg.timeout_seconds,
        api_key: "", // 保持输入框空白（可填新 key）
      });
    } catch (e) {
      console.error("加载大模型配置失败", e);
    } finally {
      setConfigLoading(false);
    }
  };

  const handleOpenDrawer = () => {
    setDrawerOpen(true);
    setTestResult(null);
    loadConfig();
  };

  // 测试连接
  const handleTestConnection = async () => {
    try {
      const values = await form.validateFields(["upstream_url", "model_name", "api_key"]);
      setTestingConfig(true);
      setTestResult(null);
      const res = await apiClient.testCopilotConfig({
        upstream_url: values.upstream_url,
        model_name: values.model_name,
        api_key: values.api_key || undefined,
      });
      setTestResult(res);
      if (res.ok) {
        message.success(res.message);
      } else {
        message.error(res.message);
      }
    } catch {
      // 表单校验未通过
    } finally {
      setTestingConfig(false);
    }
  };

  // 保存配置
  const handleSaveConfig = async () => {
    try {
      const values = await form.validateFields();
      setSavingConfig(true);
      await apiClient.updateCopilotConfig({
        upstream_url: values.upstream_url,
        model_name: values.model_name,
        api_key: values.api_key || undefined,
        timeout_seconds: values.timeout_seconds,
      });
      message.success("大模型配置已热更新并即刻生效！");
      setDrawerOpen(false);
    } catch (e: any) {
      message.error(`保存失败: ${e.message || e}`);
    } finally {
      setSavingConfig(false);
    }
  };

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
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
          <div>
            <Typography.Title level={2} style={{ marginBottom: 4 }}>
              网络排障助手 (Copilot)
            </Typography.Title>
            <Typography.Text type="secondary">
              依据 WeakNet 不可变快照做因果解释，输出受物理因果护栏严格约束
            </Typography.Text>
          </div>
          <Button onClick={handleOpenDrawer}>
            大模型热配置
          </Button>
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
                        {turn.answer.model_used ? "大模型解释（已过因果护栏）" : "确定性因果链（离线）"}
                      </Tag>
                      <Tag>{turn.answer.asset_id}</Tag>
                      {turn.answer.primary_issue && (
                        <Tag color="orange">{turn.answer.primary_issue}</Tag>
                      )}
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

        {/* 大模型配置抽屉 */}
        <Drawer
          title="大模型推理网关热配置"
          width={480}
          open={drawerOpen}
          onClose={() => setDrawerOpen(false)}
          extra={
            <Space>
              <Button onClick={() => setDrawerOpen(false)}>取消</Button>
              <Button type="primary" loading={savingConfig} onClick={handleSaveConfig}>
                保存并生效
              </Button>
            </Space>
          }
        >
          <Form form={form} layout="vertical" disabled={configLoading}>
            <Form.Item
              name="upstream_url"
              label="中转站 / Base URL"
              rules={[{ required: true, message: "请输入大模型接口基础路径" }]}
              extra="兼容 OpenAI 协议的地址，例如 https://vectide.cn/v1 或 http://ollama:11434/v1"
            >
              <Input placeholder="https://vectide.cn/v1" />
            </Form.Item>

            <Form.Item
              name="model_name"
              label="模型名称 (Model)"
              rules={[{ required: true, message: "请输入模型名称" }]}
              extra="例如 deepseek-v4-pro-0813, qwen-2.5-72b-instruct 等"
            >
              <Input placeholder="deepseek-v4-pro-0813" />
            </Form.Item>

            <Form.Item
              name="api_key"
              label="API Key"
              extra="留空表示保持当前已配置的 Key 不变；输入新 Key 会即刻更新覆盖"
            >
              <Input.Password placeholder="sk-..." />
            </Form.Item>

            <Form.Item
              name="timeout_seconds"
              label="请求超时时间 (秒)"
              initialValue={90}
              rules={[{ required: true, message: "请输入超时时间" }]}
            >
              <InputNumber min={5} max={180} style={{ width: "100%" }} />
            </Form.Item>

            <div style={{ marginBottom: 16 }}>
              <Button
                loading={testingConfig}
                onClick={handleTestConnection}
              >
                测试连通性
              </Button>
            </div>

            {testResult && (
              <Alert
                type={testResult.ok ? "success" : "error"}
                message={testResult.message}
                showIcon
              />
            )}
          </Form>
        </Drawer>
      </div>
    </AppShell>
  );
}
