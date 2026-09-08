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
              color: '#334155'
            }
          },
          axisLine: {
            roundCap: false,
            lineStyle: {
              width: 8,
              color: [
                [0.5, '#dc2626'],
                [0.75, '#d97706'],
                [0.9, '#0284c7'],
                [1, '#059669']
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
            fontWeight: '600',
            color: '#000000'
          },
          detail: {
            valueAnimation: true,
            fontSize: 30,
            fontWeight: '700',
            offsetCenter: [0, '-10%'],
            formatter: '{value}',
            color: '#000000'
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
        backgroundColor: '#ffffff',
        borderColor: '#e2e8f0',
        textStyle: { color: '#0f172a', fontSize: 12 },
        extraCssText: 'box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);'
      },
      legend: {
        data: ['RTT 延迟 (ms)', 'Wi-Fi 信号 (dBm)', '综合得分'],
        textStyle: { color: '#64748b' },
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
        axisLine: { lineStyle: { color: '#cbd5e1' } },
        axisLabel: { color: '#64748b', fontSize: 11 }
      },
      yAxis: [
        {
          type: 'value',
          name: 'ms / 分数',
          axisLine: { lineStyle: { color: '#cbd5e1' } },
          splitLine: { lineStyle: { color: '#f1f5f9' } },
          axisLabel: { color: '#64748b' }
        },
        {
          type: 'value',
          name: 'dBm',
          min: -100,
          max: 0,
          position: 'right',
          axisLine: { lineStyle: { color: '#cbd5e1' } },
          splitLine: { show: false },
          axisLabel: { color: '#64748b' }
        }
      ],
      series: [
        {
          name: 'RTT 延迟 (ms)',
          type: 'line',
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#0284c7' },
          areaStyle: {
            color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
              { offset: 0, color: 'rgba(2, 132, 199, 0.15)' },
              { offset: 1, color: 'rgba(2, 132, 199, 0.01)' }
            ])
          },
          data: []
        },
        {
          name: 'Wi-Fi 信号 (dBm)',
          type: 'line',
          yAxisIndex: 1,
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#059669' },
          data: []
        },
        {
          name: '综合得分',
          type: 'line',
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: '#d97706', type: 'dashed' },
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
