/**
 * app.js
 * 前端核心交互逻辑、WebSocket 连接与 REST API 轮询
 */

let ws = null;
let currentIface = 'wlan0';

document.addEventListener('DOMContentLoaded', () => {
  initCharts();
  initWebSocket();
  initIfaceSelector();
  fetchAllData();
  fetchBackendLogs();

  // 周期全量轮询（保底：每 5 秒刷新静态组件与表格）
  setInterval(fetchPeriodicData, 5000);

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
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td><strong>${m.name}</strong></td>
          <td><span class="monitor-tag running">${m.attached_probes} 探针</span></td>
          <td>${m.samples}</td>
          <td><code style="color: var(--color-cyan);">${m.average_read_time_us} μs</code></td>
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
    if (json.success && json.devices) {
      const tbody = document.getElementById('bt-table-body');
      tbody.innerHTML = '';
      document.getElementById('bt-count').innerText = `已发现 ${json.devices.length} 台设备`;

      json.devices.forEach(dev => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>
            <div style="font-weight: 600;">${dev.name || '未知设备'}</div>
            <div style="font-size: 11px; color: var(--text-dim); font-family: monospace;">${dev.mac}</div>
          </td>
          <td><span style="color: ${dev.rssi > -70 ? '#10b981' : dev.rssi > -85 ? '#f59e0b' : '#ef4444'}; font-weight: 600;">${dev.rssi} dBm</span></td>
          <td>${dev.type}</td>
          <td>${dev.connected ? '<span class="monitor-tag running">连接</span>' : '<span class="monitor-tag stopped">就绪</span>'}</td>
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

async function fetchHistory(limit = 60) {
  if (fetchFlags.history) return;
  fetchFlags.history = true;
  try {
    const res = await fetch(`/api/history?iface=${currentIface}&limit=${limit}`);
    const json = await res.json();
    if (json.success && json.data) {
      updateHistoryChart(json.data);
    }
  } catch (e) {
    console.error('Fetch history failed', e);
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
// 实时操作与系统控制台日志 (Log Console)
// ============================================================================
function appendConsoleLog(entry) {
  const consoleDom = document.getElementById('log-console');
  if (!consoleDom || !entry) return;

  const div = document.createElement('div');
  div.className = `log-row log-level-${entry.level || 'INFO'}`;
  div.innerHTML = `
    <span class="log-time">[${entry.time || '--:--:--'}]</span>
    <span class="log-mod">[${entry.module || 'SYS'}]</span>
    <span class="log-msg">${entry.message}</span>
  `;
  consoleDom.appendChild(div);

  // 超过 150 条清理最老一条，并始终滚到底部
  if (consoleDom.children.length > 150) {
    consoleDom.removeChild(consoleDom.firstChild);
  }
  consoleDom.scrollTop = consoleDom.scrollHeight;
}

async function fetchBackendLogs() {
  try {
    const res = await fetch('/api/logs');
    const json = await res.json();
    if (json.success && json.logs) {
      const consoleDom = document.getElementById('log-console');
      consoleDom.innerHTML = '';
      json.logs.forEach(appendConsoleLog);
    }
  } catch (e) {
    console.error('Fetch logs failed', e);
  }
}

function clearLocalLogs() {
  const consoleDom = document.getElementById('log-console');
  if (consoleDom) consoleDom.innerHTML = '';
}
