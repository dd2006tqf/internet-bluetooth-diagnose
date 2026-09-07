/**
 * charts.js
 * ECharts 图表初始化与更新管理
 */

let gaugeChart = null;
let historyChart = null;

function initCharts() {
  // 1. 初始化仪表盘
  const gaugeDom = document.getElementById('chart-gauge');
  if (gaugeDom) {
    gaugeChart = echarts.init(gaugeDom);
    const gaugeOption = {
      series: [
        {
          type: 'gauge',
          startAngle: 180,
          endAngle: 0,
          min: 0,
          max: 100,
          splitNumber: 4,
          itemStyle: {
            color: '#3b82f6'
          },
          progress: {
            show: true,
            roundCap: false,
            width: 8
          },
          pointer: {
            icon: 'path://M12.8,0.7l12,40.1H0.7L12.8,0.7z',
            length: '10%',
            width: 8,
            offsetCenter: [0, '-45%'],
            itemStyle: {
              color: '#e5e7eb'
            }
          },
          axisLine: {
            roundCap: false,
            lineStyle: {
              width: 8,
              color: [
                [0.5, 'rgba(239, 68, 68, 0.6)'],
                [0.75, 'rgba(245, 158, 11, 0.6)'],
                [0.9, 'rgba(59, 130, 246, 0.7)'],
                [1, 'rgba(16, 185, 129, 0.8)']
              ]
            }
          },
          axisTick: { show: false },
          splitLine: { show: false },
          axisLabel: {
            distance: 14,
            color: '#64748b',
            fontSize: 10
          },
          title: {
            show: true,
            offsetCenter: [0, '20%'],
            fontSize: 12,
            color: '#94a3b8'
          },
          detail: {
            valueAnimation: true,
            fontSize: 26,
            fontWeight: '600',
            offsetCenter: [0, '-10%'],
            formatter: '{value}',
            color: '#f3f4f6'
          },
          data: [
            {
              value: 0,
              name: '质量综合得分'
            }
          ]
        }
      ]
    };
    gaugeChart.setOption(gaugeOption);
  }

  // 2. 初始化时序走势折线图
  const historyDom = document.getElementById('chart-history');
  if (historyDom) {
    historyChart = echarts.init(historyDom);
    const historyOption = {
      backgroundColor: 'transparent',
      tooltip: {
        trigger: 'axis',
        backgroundColor: '#1f2937',
        borderColor: 'rgba(255,255,255,0.1)',
        textStyle: { color: '#f3f4f6' }
      },
      legend: {
        data: ['RTT 延迟 (ms)', 'Wi-Fi 信号 (dBm)', '综合得分'],
        textStyle: { color: '#9ca3af' },
        right: 10
      },
      grid: {
        top: 40,
        left: 50,
        right: 40,
        bottom: 30
      },
      xAxis: {
        type: 'category',
        boundaryGap: false,
        data: [],
        axisLine: { lineStyle: { color: '#374151' } },
        axisLabel: { color: '#9ca3af', fontSize: 11 }
      },
      yAxis: [
        {
          type: 'value',
          name: 'ms / 分数',
          axisLine: { lineStyle: { color: '#374151' } },
          splitLine: { lineStyle: { color: 'rgba(255,255,255,0.05)' } },
          axisLabel: { color: '#9ca3af' }
        },
        {
          type: 'value',
          name: 'dBm',
          min: -100,
          max: 0,
          position: 'right',
          axisLine: { lineStyle: { color: '#374151' } },
          splitLine: { show: false },
          axisLabel: { color: '#9ca3af' }
        }
      ],
      series: [
        {
          name: 'RTT 延迟 (ms)',
          type: 'line',
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#3b82f6' },
          data: []
        },
        {
          name: 'Wi-Fi 信号 (dBm)',
          type: 'line',
          yAxisIndex: 1,
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#10b981' },
          data: []
        },
        {
          name: '综合得分',
          type: 'line',
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#f59e0b', type: 'dashed' },
          data: []
        }
      ]
    };
    historyChart.setOption(historyOption);
  }

  // 窗口自适应
  window.addEventListener('resize', () => {
    if (gaugeChart) gaugeChart.resize();
    if (historyChart) historyChart.resize();
  });
}

function updateGauge(score) {
  if (!gaugeChart) return;
  gaugeChart.setOption({
    series: [
      {
        data: [{ value: Math.round(score), name: '质量综合得分' }]
      }
    ]
  });
}

function updateHistoryChart(historyData) {
  if (!historyChart || !historyData) return;
  const times = [];
  const rtts = [];
  const rssis = [];
  const scores = [];

  historyData.forEach(item => {
    // 截取时间 HH:MM:SS
    const t = item.ts ? item.ts.split('T')[1] || item.ts : '';
    times.push(t);
    rtts.push(item.rtt_ms >= 0 ? item.rtt_ms : null);
    rssis.push(item.rssi_dbm > -1000 ? item.rssi_dbm : null);
    scores.push(item.score !== null ? item.score : null);
  });

  historyChart.setOption({
    xAxis: { data: times },
    series: [
      { name: 'RTT 延迟 (ms)', data: rtts },
      { name: 'Wi-Fi 信号 (dBm)', data: rssis },
      { name: '综合得分', data: scores }
    ]
  });
}
