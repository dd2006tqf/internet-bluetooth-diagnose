/**
 * app.js
 * 前端核心交互逻辑、WebSocket 连接与 REST API 轮询
 */

let ws = null;
let currentIface = 'wlan0';
let currentHistoryLimit = 60;

document.addEventListener('DOMContentLoaded', () => {
  initCharts();
  initWebSocket();
  initIfaceSelector();
  fetchAllData();
  fetchBackendLogs();

  // 周期全量轮询（保底：每 5 秒刷新静态组件与表格）
  setInterval(fetchPeriodicData, 5000);

  // 历史走势图静默自刷新（每 30 秒自动按当前时间跨度拉取最新数据，推动曲线平滑步进）
  setInterval(() => {
    fetchHistory(currentHistoryLimit, true);
  }, 30000);

  // 绑定 AI 诊断按钮
  document.getElementById('btn-ai-diagnose').addEventListener('click', triggerAiDiagnosis);
});

// ============================================================================
// 网卡下拉选择与动态切换
// ============================================================================
async function initIfaceSelector() {
  const selector = document.getElementById('iface-select');
  if (!selector) return;

  selector.addEventListener('change', (e) => {
    const newIface = e.target.value;
    if (!newIface || newIface === currentIface) return;
    currentIface = newIface;

    appendConsoleLog({
      time: new Date().toTimeString().split(' ')[0],
      level: 'INFO',
      module: 'IFACE',
      message: `User switched target interface to: '${currentIface}'`
    });

    // 切换网卡后，重置历史走势图并以新网卡拉取
    fetchHistory(60);
  });

  // 初次加载拉取可用网络接口列表
  try {
    const res = await fetch('/api/interfaces');
    const json = await res.json();
    if (json.success && json.interfaces && json.interfaces.length > 0) {
      updateIfaceOptions(json.interfaces);
    }
  } catch (e) {
    console.error('Init iface list failed', e);
  }
}

function updateIfaceOptions(interfaces) {
  const selector = document.getElementById('iface-select');
  if (!selector || !interfaces) return;

  // 检查是否列表完全一致，避免频繁重置选中状态
  const existing = Array.from(selector.options).map(o => o.value);
  const isSame = interfaces.length === existing.length && interfaces.every(val => existing.includes(val));
  if (isSame) {
    if (interfaces.includes(currentIface) && selector.value !== currentIface) {
      selector.value = currentIface;
    }
    return;
  }

  selector.innerHTML = '';
  interfaces.forEach(iface => {
    const opt = document.createElement('option');
    opt.value = iface;
    let label = iface;
    if (iface.startsWith('wl')) label += ' (无线 Wi-Fi)';
    else if (iface.startsWith('eth') || iface.startsWith('en')) label += ' (有线 Ethernet)';
    else if (iface === 'lo') label += ' (回环 Loopback)';
    opt.innerText = label;
    selector.appendChild(opt);
  });

  if (interfaces.includes(currentIface)) {
    selector.value = currentIface;
  } else if (interfaces.length > 0) {
    selector.value = interfaces[0];
    currentIface = interfaces[0];
  }
}

// ============================================================================
// WebSocket 实时推送与重连
// ============================================================================
function initWebSocket() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = `${protocol}//${window.location.host}/ws/live`;

  const dot = document.getElementById('ws-indicator');
  const txt = document.getElementById('ws-status-text');

  ws = new WebSocket(wsUrl);

  ws.onopen = () => {
    dot.className = 'status-dot';
    txt.innerText = '实时长连接 (Active)';
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'METRICS_UPDATE') {
        if (msg.health) updateHealthUI(msg.health);
        if (msg.conflict) updateConflictUI(msg.conflict);
      } else if (msg.type === 'LOG_ENTRY') {
        appendConsoleLog(msg.entry);
      }
    } catch (e) {
      console.error('WS Parse Error', e);
    }
  };

  ws.onclose = () => {
    dot.className = 'status-dot offline';
    txt.innerText = '离线重连中...';
    setTimeout(initWebSocket, 3000);
  };

  ws.onerror = () => {
    ws.close();
  };
}

// ============================================================================
// UI 更新渲染函数
// ============================================================================
function updateHealthUI(data) {
  if (!data) return;

  // 1. 顶部网卡
  if (data.interface) {
    const selector = document.getElementById('iface-select');
    if (selector && !selector.value) {
      currentIface = data.interface;
      selector.value = data.interface;
    }
  }

  // 2. 表盘与等级标签
  const score = data.overall_score !== undefined ? data.overall_score : (data.quality_score || 0);
  updateGauge(score);

  const levelTag = document.getElementById('badge-quality-level');
  const level = data.overall_quality || 'FAIR';
  levelTag.innerText = level;
  levelTag.className = `monitor-tag ${level === 'EXCELLENT' ? 'running' : level === 'POOR' ? 'failed' : 'running'}`;

  // 3. KPI 卡片
  const rtt = data.rtt_ms !== undefined ? data.rtt_ms : '--';
  document.getElementById('kpi-rtt').innerHTML = `${rtt} <span style="font-size: 14px;">ms</span>`;

  const jitter = data.jitter_ms !== undefined ? Math.round(data.jitter_ms) : '--';
  document.getElementById('kpi-jitter').innerHTML = `${jitter} <span style="font-size: 14px;">ms</span>`;

  const rssi = (data.rssi_dbm !== undefined && data.rssi_dbm > -1000) ? data.rssi_dbm : '--';
  document.getElementById('kpi-rssi').innerHTML = `${rssi} <span style="font-size: 14px;">dBm</span>`;
  if (data.rssi_source) {
    document.getElementById('kpi-rssi-source').innerText = `来源: ${data.rssi_source}`;
  }

  const loss = (data.tcp_loss_rate !== null && data.tcp_loss_rate !== undefined) ? data.tcp_loss_rate.toFixed(2) : '0.00';
  document.getElementById('kpi-tcp-loss').innerHTML = `${loss} <span style="font-size: 14px;">%</span>`;

  // 4. 告警横幅
  const alertBox = document.getElementById('alert-box');
  const alertText = document.getElementById('alert-text');
  if (data.issues && data.issues.length > 0) {
    alertBox.style.display = 'flex';
    alertText.innerText = `智能异常检测: ${data.issues.join(' | ')}`;
    alertBox.className = 'alert-banner danger';
  } else {
    alertBox.style.display = 'none';
  }
}

function updateConflictUI(conflict) {
  if (!conflict) return;

  const bandVal = document.getElementById('val-wifi-band');
  const tagStatus = document.getElementById('tag-band-status');
  const descVal = document.getElementById('val-conflict-desc');

  const band = conflict.wifi_band || '2.4GHz';
  bandVal.innerText = band;

  if (conflict.detected) {
    tagStatus.innerText = '检测到射频冲突';
    tagStatus.className = 'monitor-tag failed';
    descVal.innerText = `置信度: ${Math.round(conflict.confidence)}% (Wi-Fi 跌落 ${conflict.wifi_rssi_drop}dBm)`;
  } else {
    if (band === '5GHz' || band === '6GHz') {
      tagStatus.innerText = '频段物理隔离';
      tagStatus.className = 'monitor-tag running';
      descVal.innerText = '当前信道与 2.4GHz 蓝牙正交，免受互调微波干扰';
    } else {
      tagStatus.innerText = '无同频冲突';
      tagStatus.className = 'monitor-tag running';
      descVal.innerText = 'Wi-Fi 与蓝牙 RSSI 独立平稳';
    }
  }
}

// ============================================================================
// 数据请求拉取（并发请求 + 防重入锁，防网络抖动导致的请求积压与乱序覆盖）
// ============================================================================
const fetchFlags = {
  health: false,
  coexistence: false,
  monitors: false,
  ebpf: false,
  bluetooth: false,
  history: false
};

function fetchAllData() {
  fetchHealth();
  fetchMonitors();
  fetchEbpfHealth();
  fetchBluetooth();
  fetchCoexistence();
  fetchHistory(60);
}

function fetchPeriodicData() {
  fetchMonitors();
  fetchEbpfHealth();
  fetchBluetooth();
}

async function fetchHealth() {
  if (fetchFlags.health) return;
  fetchFlags.health = true;
  try {
    const res = await fetch('/api/health');
    const json = await res.json();
    if (json.success && json.data) {
      updateHealthUI(json.data);
    }
  } catch (e) {
    console.error('Fetch health failed', e);
  } finally {
    fetchFlags.health = false;
  }
}

async function fetchCoexistence() {
  if (fetchFlags.coexistence) return;
  fetchFlags.coexistence = true;
  try {
    const res = await fetch('/api/coexistence');
    const json = await res.json();
    if (json.success && json.data) {
      updateConflictUI(json.data);
    }
  } catch (e) {
    console.error('Fetch coexistence failed', e);
  } finally {
    fetchFlags.coexistence = false;
  }
}

async function fetchMonitors() {
  if (fetchFlags.monitors) return;
  fetchFlags.monitors = true;
  try {
    const res = await fetch('/api/monitors');
    const json = await res.json();
    if (json.success && json.monitors) {
      const container = document.getElementById('monitors-list');
      container.innerHTML = '';
      let runningCount = 0;

      json.monitors.forEach(m => {
        const isRunning = m.state.toLowerCase() === 'running';
        if (isRunning) runningCount++;

        const div = document.createElement('div');
        div.className = 'monitor-item';
        div.onclick = () => openMonitorModal(m);
        div.innerHTML = `
          <div class="monitor-name" title="${m.name}">${m.name}</div>
          <span class="monitor-tag ${isRunning ? 'running' : 'failed'}">${m.state}</span>
        `;
        container.appendChild(div);
      });

      document.getElementById('monitors-count').innerText = `${runningCount}/${json.monitors.length} 运行中`;
    }
  } catch (e) {
    console.error('Fetch monitors failed', e);
  } finally {
    fetchFlags.monitors = false;
  }
}

// 监控器卡片弹窗控制逻辑
let selectedMonitor = null;

function openMonitorModal(monitor) {
  selectedMonitor = monitor;
  document.getElementById('ctrl-modal-name').innerText = monitor.name;

  appendConsoleLog({
    time: new Date().toTimeString().split(' ')[0],
    level: 'INFO',
    module: 'UI',
    message: `User inspected monitor card '${monitor.name}' (State: ${monitor.state})`
  });

  const stateTag = document.getElementById('ctrl-modal-state');
  const isRunning = monitor.state.toLowerCase() === 'running';
  stateTag.innerText = monitor.state.toUpperCase();
  stateTag.className = `monitor-tag ${isRunning ? 'running' : 'failed'}`;

  const btnStart = document.getElementById('btn-modal-start');
  const btnStop = document.getElementById('btn-modal-stop');
  const btnRestart = document.getElementById('btn-modal-restart');

  // 根据当前运行状态智能置灰或高亮
  if (isRunning) {
    btnStart.style.display = 'none';
    btnStop.style.display = 'inline-flex';
    btnRestart.style.display = 'inline-flex';
  } else {
    btnStart.style.display = 'inline-flex';
    btnStop.style.display = 'none';
    btnRestart.style.display = 'none';
  }

  btnRestart.onclick = () => executeMonitorAction(monitor.name, 'restart');
  btnStart.onclick = () => executeMonitorAction(monitor.name, 'enable');
  btnStop.onclick = () => executeMonitorAction(monitor.name, 'disable');

  // 加载该监控器特有的配置参数
  loadMonitorConfig(monitor.name);

  document.getElementById('monitor-control-modal').classList.add('show');
}

function closeMonitorModal() {
  document.getElementById('monitor-control-modal').classList.remove('show');
}

// 缓存当前正在编辑的配置参数
let currentMonitorConfig = null;

async function loadMonitorConfig(name) {
  const container = document.getElementById('config-form-container');
  const statusMsg = document.getElementById('config-status-msg');
  if (!container) return;

  container.innerHTML = `<div style="color: var(--text-dim); font-size: 12px; text-align: center; padding: 10px;">读取 '${name}' 参数中...</div>`;
  if (statusMsg) {
    statusMsg.className = 'config-status-msg';
    statusMsg.innerText = '';
  }

  try {
    const res = await fetch(`/api/monitors/${name}/config`);
    const json = await res.json();
    if (!json.success || !json.config) {
      container.innerHTML = `<div style="color: var(--color-danger); font-size: 12px; padding: 8px;">加载配置失败: ${json.error || json.detail || '未知错误'}</div>`;
      return;
    }

    currentMonitorConfig = json.config;
    renderConfigForm(name, json.config);
  } catch (e) {
    container.innerHTML = `<div style="color: var(--color-danger); font-size: 12px; padding: 8px;">网络异常: ${e}</div>`;
  }
}

function reloadMonitorConfig() {
  if (selectedMonitor) {
    loadMonitorConfig(selectedMonitor.name);
  }
}

function renderConfigForm(name, config) {
  const container = document.getElementById('config-form-container');
  container.innerHTML = '';

  const entries = Object.entries(config);
  if (entries.length === 0) {
    container.innerHTML = `<div style="color: var(--text-dim); font-size: 12px; padding: 6px;">该监控器暂无可调整的配置项</div>`;
    return;
  }

  entries.forEach(([field, val]) => {
    // 忽略内部对象或无须手动在表单调谐的固定项
    if (typeof val === 'object' && val !== null) return;

    const row = document.createElement('div');
    row.className = 'config-field-row';

    let hint = '';
    let placeholder = '';
    if (field.includes('interval')) {
      hint = '如: 2s, 5000ms';
      placeholder = '采样周期';
    } else if (field.includes('timeout')) {
      hint = '如: 800ms, 1s';
      placeholder = '探测超时';
    } else if (field === 'target') {
      hint = 'IPv4 地址';
      placeholder = '223.5.5.5';
    } else if (field === 'window_size' || field === 'window') {
      hint = '样本数 (2~1000)';
      placeholder = '30';
    } else if (field === 'bpf_obj') {
      hint = 'ELF 对象路径';
    }

    const fieldId = `cfg-input-${field}`;
    row.innerHTML = `
      <div class="config-field-label">
        <label for="${fieldId}"><strong>${field}</strong></label>
        <span class="field-hint">${hint}</span>
      </div>
      <input type="text" id="${fieldId}" class="config-input" data-field="${field}" value="${val !== undefined && val !== null ? val : ''}" placeholder="${placeholder}">
    `;
    container.appendChild(row);
  });
}

async function submitMonitorConfig() {
  if (!selectedMonitor) return;
  const name = selectedMonitor.name;
  const container = document.getElementById('config-form-container');
  const statusMsg = document.getElementById('config-status-msg');
  const btnApply = document.getElementById('btn-apply-config');

  const inputs = container.querySelectorAll('input.config-input');
  if (!inputs.length) return;

  const params = {};
  inputs.forEach(input => {
    const field = input.getAttribute('data-field');
    const val = input.value.trim();
    params[field] = val;
  });

  statusMsg.className = 'config-status-msg loading';
  statusMsg.innerText = '正在提交并热生效参数...';
  btnApply.disabled = true;

  try {
    const res = await fetch(`/api/monitors/${name}/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ params })
    });
    const json = await res.json();
    if (res.ok && json.success) {
      statusMsg.className = 'config-status-msg success';
      statusMsg.innerText = `✅ 参数已热生效 (${new Date().toTimeString().split(' ')[0]})`;
      appendConsoleLog({
        time: new Date().toTimeString().split(' ')[0],
        level: 'SUCCESS',
        module: 'CONFIG',
        message: `Monitor '${name}' hot-tuned successfully: ` + JSON.stringify(params)
      });
      // 刷新配置确保同步
      setTimeout(() => loadMonitorConfig(name), 600);
    } else {
      const err = json.detail || json.error || '更新失败';
      statusMsg.className = 'config-status-msg error';
      statusMsg.innerText = `❌ ${err}`;
      appendConsoleLog({
        time: new Date().toTimeString().split(' ')[0],
        level: 'ERROR',
        module: 'CONFIG',
        message: `Failed to tune params for '${name}': ${err}`
      });
    }
  } catch (e) {
    statusMsg.className = 'config-status-msg error';
    statusMsg.innerText = `❌ 请求异常: ${e}`;
  } finally {
    btnApply.disabled = false;
  }
}

async function saveConfigOverrides() {
  const statusMsg = document.getElementById('config-status-msg');
  const btnSave = document.getElementById('btn-save-config');
  if (statusMsg) {
    statusMsg.className = 'config-status-msg loading';
    statusMsg.innerText = '正在将覆盖参数固化至磁盘...';
  }
  btnSave.disabled = true;

  try {
    const res = await fetch('/api/monitors/save', { method: 'POST' });
    const json = await res.json();
    if (res.ok && json.success) {
      if (statusMsg) {
        statusMsg.className = 'config-status-msg success';
        statusMsg.innerText = `💾 配置已固化持久化落盘`;
      }
      appendConsoleLog({
        time: new Date().toTimeString().split(' ')[0],
        level: 'SUCCESS',
        module: 'CONFIG',
        message: `Monitor overrides successfully saved to disk`
      });
    } else {
      const err = json.detail || json.error || '固化失败';
      if (statusMsg) {
        statusMsg.className = 'config-status-msg error';
        statusMsg.innerText = `❌ 固化失败: ${err}`;
      }
    }
  } catch (e) {
    if (statusMsg) {
      statusMsg.className = 'config-status-msg error';
      statusMsg.innerText = `❌ 请求异常: ${e}`;
    }
  } finally {
    btnSave.disabled = false;
  }
}

async function executeMonitorAction(name, action) {
  const actionNames = {
    restart: '重启',
    enable: '启动',
    disable: '停止'
  };
  const label = actionNames[action] || action;
  appendConsoleLog({
    time: new Date().toTimeString().split(' ')[0],
    level: 'INFO',
    module: 'USER',
    message: `User clicked [${label}] on monitor '${name}'`
  });

  try {
    const res = await fetch(`/api/monitors/${name}/${action}`, { method: 'POST' });
    const json = await res.json();
    if (json.success) {
      appendConsoleLog({
        time: new Date().toTimeString().split(' ')[0],
        level: 'SUCCESS',
        module: 'D-BUS',
        message: `Monitor '${name}' action [${label}] completed successfully`
      });
      closeMonitorModal();
      fetchMonitors();
    } else {
      const err = json.detail || json.error || '依赖限制或未知错误';
      appendConsoleLog({
        time: new Date().toTimeString().split(' ')[0],
        level: 'ERROR',
        module: 'D-BUS',
        message: `Failed to [${label}] monitor '${name}': ${err}`
      });
      alert(`${label}失败: ${err}`);
    }
  } catch (e) {
    appendConsoleLog({
      time: new Date().toTimeString().split(' ')[0],
      level: 'ERROR',
      module: 'HTTP',
      message: `Request error during [${label}] on '${name}': ${e}`
    });
    alert(`${label}请求异常: ` + e);
  }
}

async function fetchEbpfHealth() {
  if (fetchFlags.ebpf) return;
  fetchFlags.ebpf = true;
  try {
    const res = await fetch('/api/ebpf/health');
    const json = await res.json();
    if (json.success && json.data && json.data.monitors) {
      const tbody = document.getElementById('ebpf-table-body');
      tbody.innerHTML = '';
      json.data.monitors.forEach(m => {
        let displayTime = '';
        const us = Number(m.average_read_time_us);
        if (isNaN(us) || us > 60000000 || us < 0) {
          displayTime = '< 1 μs';
        } else if (us >= 1000) {
          displayTime = (us / 1000).toFixed(2) + ' ms';
        } else {
          displayTime = Math.round(us) + ' μs';
        }

        const tr = document.createElement('tr');
        tr.className = 'ebpf-row-clickable';
        tr.title = `点击查看 ${m.name} 内核深度诊断数据`;
        tr.onclick = () => openEbpfModal(m);
        tr.innerHTML = `
          <td><strong>${m.name}</strong> <span style="font-size: 10px; color: var(--color-cyan);">🔍</span></td>
          <td><span class="monitor-tag running">${m.attached_probes} 探针</span></td>
          <td>${m.samples}</td>
          <td><code style="color: var(--color-cyan);">${displayTime}</code></td>
        `;
        tbody.appendChild(tr);
      });
    }
  } catch (e) {
    console.error('Fetch ebpf failed', e);
  } finally {
    fetchFlags.ebpf = false;
  }
}

async function fetchBluetooth() {
  if (fetchFlags.bluetooth) return;
  fetchFlags.bluetooth = true;
  try {
    const res = await fetch('/api/bluetooth/devices');
    const json = await res.json();
    if (json.success && Array.isArray(json.devices)) {
      const tbody = document.getElementById('bt-table-body');
      tbody.innerHTML = '';

      // 严格按信号强度 (RSSI) 降序排序（强信号 -50dBm 优先排在最前）
      const sortedDevices = [...json.devices].sort((a, b) => (b.rssi ?? -100) - (a.rssi ?? -100));

      document.getElementById('bt-count').innerText = `已发现 ${sortedDevices.length} 台设备 (按信号降序)`;

      sortedDevices.forEach(dev => {
        const isStrong = dev.rssi >= -65;
        const isFair = dev.rssi >= -80;
        const color = isStrong ? '#059669' : (isFair ? '#d97706' : '#dc2626');
        const isNamed = dev.name && dev.name !== '未知设备' && !dev.name.includes(dev.mac);

        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>
            <div style="font-weight: 600; color: ${isNamed ? 'var(--text-main)' : 'var(--text-muted)'};">
              ${dev.name || '未知设备'}
            </div>
            <div style="font-size: 10px; color: var(--text-dim); font-family: monospace;">${dev.mac}</div>
          </td>
          <td>
            <span style="color: ${color}; font-weight: 700; font-family: ui-monospace, monospace;">${dev.rssi} dBm</span>
          </td>
          <td><span style="font-size: 11px; color: var(--text-muted);">${dev.type}</span></td>
          <td>${dev.connected ? '<span class="monitor-tag running">已连接</span>' : '<span class="monitor-tag stopped">就绪</span>'}</td>
        `;
        tbody.appendChild(tr);
      });
    }
  } catch (e) {
    console.error('Fetch bluetooth failed', e);
  } finally {
    fetchFlags.bluetooth = false;
  }
}

async function fetchHistory(limit = 60, silent = false) {
  currentHistoryLimit = limit;
  if (fetchFlags.history) return;
  fetchFlags.history = true;
  try {
    const res = await fetch(`/api/history?iface=${currentIface}&limit=${limit}`);
    const json = await res.json();
    if (json.success && json.data) {
      updateHistoryChart(json.data);
    }
  } catch (e) {
    if (!silent) console.error('Fetch history failed', e);
  } finally {
    fetchFlags.history = false;
  }
}

// ============================================================================
// AI 一键诊断交互
// ============================================================================
async function triggerAiDiagnosis() {
  const modal = document.getElementById('ai-modal');
  const body = document.getElementById('ai-modal-body');
  modal.classList.add('show');
  body.innerHTML = `
    <div style="text-align: center; padding: 40px; color: var(--text-muted);">
      <div style="font-size: 24px; margin-bottom: 12px; animation: spin 1s linear infinite;">⏳</div>
      正在调用 WeakNet 本地专家知识库分析多维度时序指标...
    </div>
  `;

  try {
    const res = await fetch('/api/ai/diagnose', { method: 'POST' });
    const json = await res.json();
    if (json.success && json.report) {
      const rep = json.report;
      let html = `
        <div style="display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border-color); padding-bottom: 12px;">
          <div>
            <div style="font-size: 12px; color: var(--text-dim);">诊断状态</div>
            <strong style="color: ${rep.severity === 'CRITICAL' ? '#ef4444' : rep.severity === 'WARNING' ? '#f59e0b' : '#10b981'}; font-size: 16px;">${rep.severity}</strong>
          </div>
          <div>
            <div style="font-size: 12px; color: var(--text-dim);">分析评分</div>
            <strong style="font-size: 18px; color: var(--color-cyan);">${Math.round(rep.overall_score)} / 100</strong>
          </div>
          <div>
            <div style="font-size: 12px; color: var(--text-dim);">知识库引擎</div>
            <span class="brand-badge">${rep.knowledge_base_connected ? 'RAG 知识库就绪' : '内置规则引擎'}</span>
          </div>
        </div>

        <div>
          <div style="font-weight: 600; font-size: 14px; margin-bottom: 10px;">🔍 根因分析项 (${rep.findings_count} 项)</div>
          <div style="display: flex; flex-direction: column; gap: 10px;">
      `;

      rep.findings.forEach(f => {
        html += `
          <div class="finding-card ${f.level}">
            <div style="font-weight: 600; font-size: 13px; margin-bottom: 4px;">${f.dim}</div>
            <div style="color: var(--text-main); font-size: 12px; margin-bottom: 4px;">${f.desc}</div>
            <div style="color: var(--text-muted); font-size: 11px;"><strong>根因推定:</strong> ${f.cause}</div>
          </div>
        `;
      });

      html += `
          </div>
        </div>

        <div>
          <div style="font-weight: 600; font-size: 14px; margin-bottom: 8px;">💡 专家处置与调优建议</div>
          <ul style="padding-left: 20px; font-size: 13px; color: var(--text-muted); line-height: 1.8;">
      `;

      rep.recommendations.forEach(r => {
        html += `<li>${r}</li>`;
      });

      html += `
          </ul>
        </div>
      `;
      body.innerHTML = html;
    } else {
      body.innerHTML = `<div style="color: #ef4444;">诊断报告生成失败</div>`;
    }
  } catch (e) {
    body.innerHTML = `<div style="color: #ef4444;">网络请求出错: ${e}</div>`;
  }
}

function closeAiModal() {
  document.getElementById('ai-modal').classList.remove('show');
}

// ============================================================================
// eBPF 探针深度指标下钻模态框
// ============================================================================
let currentViewingProbe = null;

async function openEbpfModal(probe) {
  currentViewingProbe = probe;
  const name = probe.name;
  document.getElementById('ebpf-modal-title').innerText = name;
  const body = document.getElementById('ebpf-modal-body');
  body.innerHTML = `
    <div style="text-align: center; padding: 24px; color: var(--text-muted);">
      正在拉取 <strong>${name}</strong> 的内核 BPF Map 深度采集指标...
    </div>
  `;
  document.getElementById('ebpf-detail-modal').classList.add('show');

  appendConsoleLog({
    time: new Date().toTimeString().split(' ')[0],
    level: 'INFO',
    module: 'EBPF',
    message: `User inspected eBPF probe detail for '${name}'`
  });

  await fetchAndRenderEbpfDetail(name, probe);
}

function closeEbpfModal() {
  document.getElementById('ebpf-detail-modal').classList.remove('show');
}

async function fetchAndRenderEbpfDetail(name, probe) {
  const body = document.getElementById('ebpf-modal-body');
  if (!body) return;

  // 基础探针元数据卡片
  let baseHeaderHtml = `
    <div class="ebpf-stat-grid" style="margin-bottom: 12px;">
      <div class="ebpf-stat-box">
        <span class="ebpf-stat-label">运行状态</span>
        <span class="ebpf-stat-val" style="color: #059669; font-size: 15px;">${probe.state ? probe.state.toUpperCase() : 'ATTACHED'}</span>
      </div>
      <div class="ebpf-stat-box">
        <span class="ebpf-stat-label">挂载探针点 (Probes)</span>
        <span class="ebpf-stat-val" style="color: var(--color-cyan);">${probe.attached_probes} 个</span>
      </div>
      <div class="ebpf-stat-box">
        <span class="ebpf-stat-label">历史采样总计</span>
        <span class="ebpf-stat-val">${probe.samples} 次</span>
      </div>
    </div>
  `;

  try {
    if (name === 'SkbDropMonitor') {
      const res = await fetch('/api/ebpf/skb-drop');
      const json = await res.json();
      const dropData = json.data || {};
      const totalDrops = dropData.total_drops || 0;
      const reasons = dropData.top_reasons || [];

      let detailHtml = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">📦 内核网络层 / Socket 丢包统计 (tracepoint/skb/kfree_skb)</div>
          <div class="alert-banner ${totalDrops > 0 ? 'danger' : ''}" style="margin-bottom: 10px;">
            ${totalDrops > 0 ? `⚠️ 内核累计捕获到 <strong>${totalDrops}</strong> 个 Socket/skb 异常丢包事件` : `✅ 当前内核网络栈 Socket 丢包计数为 0，协议链路完全平稳`}
          </div>
      `;

      if (reasons.length > 0) {
        detailHtml += `
          <table style="margin-top: 6px;">
            <thead>
              <tr>
                <th>丢包根本归因 (Drop Reason)</th>
                <th>触发协议</th>
                <th>丢包发生计数</th>
              </tr>
            </thead>
            <tbody>
        `;
        reasons.forEach(r => {
          detailHtml += `
            <tr>
              <td><strong style="color: #dc2626;">${r.reason || 'UNKNOWN'}</strong></td>
              <td><code>${r.protocol || 'IP'}</code></td>
              <td><strong>${r.count}</strong></td>
            </tr>
          `;
        });
        detailHtml += `</tbody></table>`;
      }
      detailHtml += `</div>`;
      body.innerHTML = detailHtml;

    } else if (name === 'DnsMonitor') {
      const res = await fetch('/api/ebpf/dns');
      const json = await res.json();
      const raw = json.data || '';
      // 解析 形如 "totalQueries:0|totalResponses:0|totalTimeouts:0|totalErrors:0|avgLatencyMs:0|maxLatencyMs:0|timeoutRate:0.000000"
      const kv = {};
      raw.split('|').forEach(part => {
        const [k, v] = part.split(':');
        if (k && v !== undefined) kv[k.trim()] = v.trim();
      });

      body.innerHTML = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">🌐 DNS 解析吞吐与时延感知 (kprobe/udp_recvmsg)</div>
          <div class="ebpf-stat-grid" style="margin-top: 6px;">
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">DNS 请求总量</span>
              <span class="ebpf-stat-val">${kv.totalQueries || 0}</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">响应返回数</span>
              <span class="ebpf-stat-val">${kv.totalResponses || 0}</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">超时丢包数</span>
              <span class="ebpf-stat-val" style="color: ${Number(kv.totalTimeouts) > 0 ? '#dc2626' : 'inherit'};">${kv.totalTimeouts || 0}</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">平均解析延迟</span>
              <span class="ebpf-stat-val" style="color: var(--color-cyan);">${kv.avgLatencyMs || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">峰值延迟</span>
              <span class="ebpf-stat-val">${kv.maxLatencyMs || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">超时发生率</span>
              <span class="ebpf-stat-val">${(Number(kv.timeoutRate || 0) * 100).toFixed(2)} %</span>
            </div>
          </div>
        </div>
      `;

    } else if (name === 'WifiPacketLossMonitor') {
      const res = await fetch('/api/ebpf/wifi-loss');
      const json = await res.json();
      const raw = json.data || '';
      // 解析 "ifindex:1 rxPkts:101 txPkts:101 txDrops:0 txLossRate:0.000000%|..."
      const ifaceRows = raw.split('|').filter(r => r.trim().length > 0);

      let ifaceHtml = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">📶 Wi-Fi 链路层收发与硬件丢包率 (mac80211/cfg80211 驱动探针)</div>
          <table style="margin-top: 8px;">
            <thead>
              <tr>
                <th>网络接口 Index</th>
                <th>接收包数 (RX)</th>
                <th>发送包数 (TX)</th>
                <th>底层丢包 (Drops)</th>
                <th>链路丢包率</th>
              </tr>
            </thead>
            <tbody>
      `;
      ifaceRows.forEach(row => {
        const parts = row.split(/\s+/);
        const obj = {};
        parts.forEach(p => {
          const [k, v] = p.split(':');
          if (k && v !== undefined) obj[k] = v;
        });
        const dropNum = Number(obj.txDrops || 0);
        ifaceHtml += `
          <tr>
            <td><strong>Interface #${obj.ifindex || '-'}</strong></td>
            <td>${obj.rxPkts || 0}</td>
            <td>${obj.txPkts || 0}</td>
            <td><span style="color: ${dropNum > 0 ? '#dc2626' : '#059669'}; font-weight: 600;">${obj.txDrops || 0}</span></td>
            <td><code style="color: var(--color-cyan);">${obj.txLossRate || '0%'}</code></td>
          </tr>
        `;
      });
      ifaceHtml += `</tbody></table></div>`;
      body.innerHTML = ifaceHtml;

    } else if (name === 'HttpLatencyMonitor') {
      const res = await fetch('/api/ebpf/http-latency');
      const json = await res.json();
      const raw = json.data || '';
      // 解析 "totalTxns:32|p50Ms:1616|p95Ms:1616|p99Ms:1616|maxMs:1616|analysis:主要应用慢..."
      const kv = {};
      raw.split('|').forEach(part => {
        const [k, v] = part.split(':');
        if (k && v !== undefined) kv[k.trim()] = v.trim();
      });

      body.innerHTML = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">⚡ HTTP 请求首字节延迟 (TTFB 分位数，kretprobe/tcp_recvmsg)</div>
          <div class="ebpf-stat-grid" style="margin-top: 6px;">
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">跟踪 HTTP 事务总数</span>
              <span class="ebpf-stat-val">${kv.totalTxns || 0}</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">P50 中位延迟</span>
              <span class="ebpf-stat-val">${kv.p50Ms || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">P95 尾部延迟</span>
              <span class="ebpf-stat-val" style="color: var(--color-cyan);">${kv.p95Ms || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">P99 极值延迟</span>
              <span class="ebpf-stat-val" style="color: ${Number(kv.p99Ms) > 500 ? '#dc2626' : '#059669'};">${kv.p99Ms || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">历史最大延迟</span>
              <span class="ebpf-stat-val">${kv.maxMs || 0} ms</span>
            </div>
            <div class="ebpf-stat-box">
              <span class="ebpf-stat-label">专家归因定性</span>
              <span class="ebpf-stat-val" style="font-size: 13px; color: ${kv.analysis && kv.analysis.includes('慢') ? '#d97706' : '#059669'};">${kv.analysis || '正常'}</span>
            </div>
          </div>
        </div>
      `;

    } else if (name === 'ProcessNetProfiler') {
      const res = await fetch('/api/ebpf/profiling');
      const json = await res.json();
      const raw = json.data || '';

      // 解析 Top Bandwidth 与 Top Retransmit
      const sections = raw.split('===');
      let bwList = [];
      let retransList = [];

      sections.forEach(sec => {
        if (sec.includes('Top Bandwidth')) {
          bwList = sec.split('|').filter(line => line.includes('pid:'));
        } else if (sec.includes('Top Retransmit')) {
          retransList = sec.split('|').filter(line => line.includes('pid:'));
        }
      });

      const parseProcLine = (line) => {
        const parts = line.trim().split(/\s+/);
        const obj = {};
        parts.forEach(p => {
          const [k, v] = p.split(':');
          if (k && v !== undefined) obj[k] = v;
        });
        return obj;
      };

      let profHtml = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">📊 进程级实时网络带宽画像 (Top Bandwidth)</div>
          <table style="margin-top: 6px;">
            <thead>
              <tr>
                <th>PID</th>
                <th>进程名 (Process Comm)</th>
                <th>发送字节 (TX Bytes)</th>
                <th>数据包 (Packets)</th>
                <th>重传数</th>
              </tr>
            </thead>
            <tbody>
      `;
      bwList.forEach(line => {
        const p = parseProcLine(line);
        profHtml += `
          <tr>
            <td><code>${p.pid || '-'}</code></td>
            <td><strong>${p.comm || '-'}</strong></td>
            <td><span style="color: var(--color-cyan); font-weight: 600;">${p.txBytes || 0} B</span></td>
            <td>${p.txPackets || 0}</td>
            <td><span style="color: ${Number(p.retrans) > 0 ? '#dc2626' : '#059669'};">${p.retrans || 0}</span></td>
          </tr>
        `;
      });
      profHtml += `</tbody></table></div>`;

      if (retransList.length > 0) {
        profHtml += `
          <div class="ebpf-section-block" style="margin-top: 14px;">
            <div class="ebpf-section-title">🔄 进程级异常 TCP 重传画像 (Top Retransmit)</div>
            <table style="margin-top: 6px;">
              <thead>
                <tr>
                  <th>PID</th>
                  <th>进程名 (Comm)</th>
                  <th>重传次数 (Retrans)</th>
                  <th>传输字节</th>
                </tr>
              </thead>
              <tbody>
        `;
        retransList.forEach(line => {
          const p = parseProcLine(line);
          profHtml += `
            <tr>
              <td><code>${p.pid || '-'}</code></td>
              <td><strong>${p.comm || '-'}</strong></td>
              <td><span style="color: ${Number(p.retrans) > 0 ? '#dc2626' : '#059669'}; font-weight: 700;">${p.retrans || 0}</span></td>
              <td>${p.txBytes || 0} B</td>
            </tr>
          `;
        });
        profHtml += `</tbody></table></div>`;
      }

      body.innerHTML = profHtml;

    } else {
      // 其它探针（如 TcpRetransMonitor, TcpConnMonitor, BtAudioAnalyzer）
      body.innerHTML = `
        ${baseHeaderHtml}
        <div class="ebpf-section-block">
          <div class="ebpf-section-title">📌 探针运行详情快照</div>
          <div style="background: #f8fafc; border: 1px solid var(--border-color); border-radius: var(--radius-sm); padding: 12px; font-size: 12px; line-height: 1.8;">
            <div><strong>探针唯一标识:</strong> <code>${probe.name}</code></div>
            <div><strong>探针挂载状态:</strong> <span class="monitor-tag running">${probe.status || 'Active & Attached'}</span></div>
            <div><strong>单次读取均耗时:</strong> <code>${Number(probe.average_read_time_us || 0)} μs</code></div>
            <div><strong>累计读取总耗时:</strong> <code>${Number(probe.total_read_time_us || 0)} μs</code></div>
            <div><strong>最近错误描述:</strong> <span style="color: var(--text-dim);">${probe.last_error || '无异常 (Error Free)'}</span></div>
          </div>
        </div>
      `;
    }
  } catch (e) {
    body.innerHTML = `
      ${baseHeaderHtml}
      <div style="color: #dc2626; padding: 14px; font-size: 13px;">拉取内核指标失败: ${e}</div>
    `;
  }
}

// ============================================================================
// 实时操作与系统控制台日志 (Log Console)
// ============================================================================
const localLogStore = [];
let currentLogFilter = 'ALL';

function appendConsoleLog(entry) {
  if (!entry) return;
  localLogStore.push(entry);
  if (localLogStore.length > 300) {
    localLogStore.shift();
  }

  // 判断是否符合当前选中的过滤器
  if (matchesLogFilter(entry, currentLogFilter)) {
    renderSingleLogRow(entry);
  }
}

function matchesLogFilter(entry, filter) {
  if (filter === 'ALL') return true;
  if (filter === 'ERRORS') {
    const lvl = (entry.level || '').toUpperCase();
    return lvl === 'ERROR' || lvl === 'WARNING' || lvl === 'WARN';
  }
  if (filter === 'CONFIG') {
    return (entry.module || '').toUpperCase() === 'CONFIG';
  }
  if (filter === 'MONITOR') {
    return (entry.module || '').toUpperCase() === 'MONITOR' || (entry.module || '').toUpperCase() === 'D-BUS';
  }
  return true;
}

function renderSingleLogRow(entry) {
  const consoleDom = document.getElementById('log-console');
  if (!consoleDom) return;

  const div = document.createElement('div');
  div.className = `log-row log-level-${entry.level || 'INFO'}`;
  div.innerHTML = `
    <span class="log-time">[${entry.time || '--:--:--'}]</span>
    <span class="log-mod">[${entry.module || 'SYS'}]</span>
    <span class="log-msg">${entry.message}</span>
  `;
  consoleDom.appendChild(div);

  // 控制台可视区域保留上限
  if (consoleDom.children.length > 200) {
    consoleDom.removeChild(consoleDom.firstChild);
  }
  consoleDom.scrollTop = consoleDom.scrollHeight;
}

function applyLogFilter() {
  const select = document.getElementById('log-filter-select');
  if (select) {
    currentLogFilter = select.value;
  }
  const consoleDom = document.getElementById('log-console');
  if (!consoleDom) return;

  consoleDom.innerHTML = '';
  localLogStore.forEach(entry => {
    if (matchesLogFilter(entry, currentLogFilter)) {
      renderSingleLogRow(entry);
    }
  });
}

function exportConsoleLogs() {
  if (!localLogStore.length) {
    alert('当前没有可导出的控制台审计日志');
    return;
  }

  const lines = localLogStore.map(e => `[${e.time || ''}] [${e.level || 'INFO'}] [${e.module || 'SYS'}] ${e.message}`);
  const header = `=======================================================\n`
               + ` WeakNet Network Diagnostics Audit Log Export\n`
               + ` Export Time: ${new Date().toISOString()}\n`
               + ` Target Interface: ${currentIface}\n`
               + ` Total Entries: ${localLogStore.length}\n`
               + `=======================================================\n\n`;

  const blob = new Blob([header + lines.join('\n')], { type: 'text/plain;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
  a.href = url;
  a.download = `weaknet_audit_${ts}.log`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

async function fetchBackendLogs() {
  try {
    const res = await fetch('/api/logs');
    const json = await res.json();
    if (json.success && Array.isArray(json.logs)) {
      localLogStore.length = 0;
      json.logs.forEach(e => localLogStore.push(e));
      applyLogFilter();
    }
  } catch (e) {
    console.error('Fetch logs failed', e);
  }
}

function clearLocalLogs() {
  localLogStore.length = 0;
  const consoleDom = document.getElementById('log-console');
  if (consoleDom) consoleDom.innerHTML = '';
}
