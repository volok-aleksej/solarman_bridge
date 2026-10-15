'use strict';

// All API URLs are relative (no leading slash) so the page keeps working
// when a reverse proxy mounts this service under a path prefix.
const API = {
  live:    'api/live',
  info:    'api/info',
  history: 'api/history',
  daily:   'api/daily',
  command:   'api/command',
  inverters: 'api/inverters',
};

const LIVE_POLL_MS = 1000;
// How long the page waits for a queued command's outcome.
const CMD_WAIT_MS  = 30000;
// Below this a flow line is drawn idle: sensors read a few watts at rest.
const FLOW_MIN_W   = 20;

// Tabs under the Realtime diagram, one table set per node.
const RT_TABS = [
  { id: 'batt', label: 'Battery' },
  { id: 'pv',   label: 'Solar' },
  { id: 'grid', label: 'Grid' },
  { id: 'load', label: 'Load' },
];
const TABS = ['realtime', 'history', 'energy', 'info'];

// ── Chart.js helpers ──────────────────────────────────────────────────────────
// Chart instances live outside the Alpine component on purpose: Alpine's deep
// reactive proxy recurses forever on Chart.js' self-referential objects.

const AXIS_FONT = { family: 'monospace', size: 11, weight: 'bold' };
const AXIS_CLR  = '#1f2328';
const GRID_CLR  = '#eaeef2';

const C = {
  pv:   '#d4a72c',
  load: '#8250df',
  grid: '#cf222e',
  batt: '#1a7f37',
  l1:   '#bc4c00',
  l2:   '#0969da',
  l3:   '#1b7c83',
  sell: '#2da44e',
  dis:  '#0550ae',
};

let CHARTS       = null;
let ENERGY_CHART = null;
let ENERGY_PIES  = {};     // id -> Chart
let ENERGY_ROWS  = [];     // daily_energy row or null, one per day of the month

// Daily bar series of each Energy tab: [label, daily_energy field, color].
const ENERGY_BARS = {
  batt: [['Charged', 'batt_charge_wh', C.batt], ['Discharged', 'batt_discharge_wh', C.dis]],
  pv:   [['Generated', 'pv_wh', C.pv]],
  grid: [['Bought', 'grid_buy_wh', C.grid], ['Sold', 'grid_sell_wh', C.sell]],
  load: [['Consumed', 'load_wh', C.load]],
};

// Where one day's energy came from and went to, in Wh, from the daily
// counters alone. Solar is sold first (only solar is assumed to be sold),
// then charges the battery, and the rest feeds the load. The load takes
// solar first, then the battery; the grid covers what is left.
function splitDay(e) {
  const pvToGrid   = Math.min(e.grid_sell_wh, e.pv_wh);
  const pvToBatt   = Math.min(e.batt_charge_wh, e.pv_wh - pvToGrid);
  const pvToLoad   = e.pv_wh - pvToGrid - pvToBatt;
  const loadFromPv = Math.min(e.load_wh, pvToLoad);
  const battToLoad = Math.min(e.batt_discharge_wh, e.load_wh - loadFromPv);
  return {
    pvToLoad, pvToBatt, pvToGrid,
    loadFromPv, battToLoad,
    gridToLoad: e.load_wh - loadFromPv - battToLoad,
  };
}

function fmtAxisTime(ms, spanMs) {
  const d = new Date(ms);
  const p = n => String(n).padStart(2, '0');
  if (spanMs <= 6 * 3600e3)
    return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  if (spanMs <= 48 * 3600e3)
    return p(d.getHours()) + ':' + p(d.getMinutes());
  return p(d.getDate()) + '.' + p(d.getMonth() + 1) + ' ' +
         p(d.getHours()) + ':' + p(d.getMinutes());
}

function makeLineChart(id, title, yfmt) {
  const cv = document.getElementById(id);
  if (!cv || !window.Chart) return null;
  return new window.Chart(cv, {
    type: 'line',
    data: { datasets: [] },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      parsing: false,
      normalized: true,
      interaction: { mode: 'index', intersect: false },
      elements: {
        point: { radius: 0, hitRadius: 6 },
        line:  { borderWidth: 1.5, tension: 0.2 },
      },
      plugins: {
        title:  { display: true, text: title, align: 'start',
                  color: AXIS_CLR, font: AXIS_FONT },
        legend: { display: true,
                  labels: { boxWidth: 12, boxHeight: 3,
                            color: AXIS_CLR, font: AXIS_FONT } },
        tooltip: {
          titleFont: AXIS_FONT, bodyFont: AXIS_FONT,
          callbacks: {
            title: items => new Date(items[0].parsed.x).toLocaleString(),
            label: ctx   => ctx.dataset.label + ': ' + yfmt(ctx.parsed.y),
          },
        },
      },
      scales: {
        x: {
          type: 'linear',
          bounds: 'data',
          ticks: {
            color: AXIS_CLR, font: AXIS_FONT,
            maxRotation: 0, autoSkip: true, maxTicksLimit: 8,
            callback(value, _i, ticks) {
              const span = ticks.length
                ? ticks[ticks.length - 1].value - ticks[0].value : 0;
              return fmtAxisTime(value, span);
            },
          },
          grid: { color: GRID_CLR },
        },
        y: {
          ticks: { color: AXIS_CLR, font: AXIS_FONT, callback: v => yfmt(v) },
          grid:  { color: GRID_CLR },
        },
      },
    },
  });
}

// Labels of the lines the user has hidden by clicking the legend. Chart.js
// keeps that per dataset object, so it is carried over by label whenever
// the datasets are rebuilt.
function hiddenLabels(chart) {
  return new Set(chart.data.datasets
    .filter((_, i) => !chart.isDatasetVisible(i))
    .map(d => d.label));
}

// series = [{ label, color, fn }]; fn maps a sample to its y value.
function setSeries(chart, data, series) {
  if (!chart) return;
  const hidden = hiddenLabels(chart);
  chart.data.datasets = series.map(s => ({
    label:           s.label,
    borderColor:     s.color,
    backgroundColor: s.color,
    hidden:          hidden.has(s.label),
    data:            data.map(d => ({ x: d.ts, y: s.fn(d) })),
  }));
  chart.update('none');
}

// ── Alpine component ──────────────────────────────────────────────────────────

function inverter() {
  return {
    tab:          'realtime',
    live:         {},
    info:         null,
    status:       'connecting…',
    linkStatus:   'connecting…',
    histHours:    24,
    histStatus:   '',
    energyYear:   new Date().getFullYear(),
    energyMonth:  new Date().getMonth() + 1,
    energyStatus: '',
    energyTotals: [],
    energyPies:   [],
    energyTab:    'batt',
    rtTab:        'batt',
    RT_TABS,
    cmdBusy:      false,
    cmdStatus:    '',
    pw:           { open: false, cmd: null, value: '', error: '', busy: false },

    inverters:      [],   // from api/inverters
    inverter:       '',   // selected name; '' until the list is loaded
    energyInverter: '',   // Energy may also show 'all'

    get s() { return this.live.sample || null; },

    // Query parameter for the selected inverter.
    invQ(name) {
      const n = name === undefined ? this.inverter : name;
      return n ? 'inverter=' + encodeURIComponent(n) : '';
    },

    selectInverter(name) {
      if (name === 'all') { this.energyInverter = 'all'; this.loadEnergy(); return; }
      this.inverter       = name;
      this.energyInverter = name;
      this.live      = {};
      this.info      = null;
      this.cmdStatus = '';
      this.loadInfo();
      this.openTab(this.tab);
    },

    async loadInverters() {
      try {
        const r = await fetch(API.inverters);
        if (!r.ok) return;
        this.inverters = (await r.json()).inverters;
        if (!this.inverter && this.inverters.length) {
          this.inverter       = this.inverters[0].name;
          this.energyInverter = this.inverter;
        }
      } catch (_) {}
    },

    openTab(t) {
      this.tab = t;
      if (t === 'history') this.loadHistory();
      if (t === 'energy')  this.loadEnergy();
      if (t === 'info')    this.loadInfo();
    },

    init() {
      // The open tab lives in the URL hash so it survives reloads/bookmarks.
      const fromHash = () => {
        const t = location.hash.slice(1);
        if (TABS.includes(t)) this.openTab(t);
      };
      fromHash();
      window.addEventListener('hashchange', fromHash);
      this.$watch('tab', t => { if (location.hash !== '#' + t) location.hash = t; });

      this.loadInverters().then(() => {
        this.loadInfo();   // the centre button needs to know if control is on
        this.openTab(this.tab);
      });
      this._startLivePolling();
      this._histLastLoad = 0;
      // Refresh the open History tab at roughly the resolution it shows.
      setInterval(() => {
        if (this.tab !== 'history') return;
        const intervalMs = Math.max(10000, this.histHours * 6000);
        if (Date.now() - this._histLastLoad >= intervalMs) this.loadHistory();
      }, 1000);
    },

    // ── formatting ───────────────────────────────────────────────────────────

    kw(w) {
      if (w == null) return '—';
      return Math.abs(w) < 1000 ? w + ' W' : (w / 1000).toFixed(2) + ' kW';
    },
    watts(w)  { return w == null ? '—' : Math.round(w) + ' W'; },
    amps(ma)  { return ma == null ? '—' : (ma / 1000).toFixed(1) + ' A'; },
    kwh(wh)   { return wh == null ? '—' : (wh / 1000).toFixed(1) + ' kWh'; },
    ageText() {
      const a = this.live.age_ms;
      if (a == null || a < 0) return '?';
      if (a < 120e3) return Math.round(a / 1000) + ' s';
      if (a < 7200e3) return Math.round(a / 60e3) + ' min';
      return (a / 3600e3).toFixed(1) + ' h';
    },

    // ── realtime ─────────────────────────────────────────────────────────────

    // The inverter has no load current register: sum of P/U over phases.
    loadCurrentMa() {
      const s = this.s;
      let ma = 0;
      for (const p of [1, 2, 3]) {
        const mv = s['load_l' + p + '_voltage_mv'];
        if (mv > 0) ma += Math.abs(s['load_l' + p + '_power_w']) / mv * 1e6;
      }
      return ma;
    },

    // Direction of a flow line, each drawn from its node towards the centre:
    // 'in' means power flows into the inverter, 'out' away from it, '' none.
    flowDir(node) {
      const s = this.s;
      const w = { pv:   s.pv1_power_w + s.pv2_power_w,
                  batt: s.batt_power_w,
                  grid: s.grid_power_w,
                  load: -s.load_power_w }[node];
      return Math.abs(w) < FLOW_MIN_W ? '' : w > 0 ? 'in' : 'out';
    },

    // Tables of the selected Realtime tab: [{head: [...], rows: [[...]]}].
    rtTables() {
      const s = this.s;
      const V  = mv => (mv / 1000).toFixed(1);
      const A  = ma => (ma / 1000).toFixed(2);
      const kWh = wh => (wh / 1000).toFixed(1);
      const C  = dc => (dc / 10).toFixed(1);
      const phases = [1, 2, 3];
      switch (this.rtTab) {
        case 'batt': return [
          { head: ['Voltage, V', 'Current, A', 'Power, W', 'SoC, %', 'Temperature, °C'],
            rows: [[(s.batt_voltage_mv / 1000).toFixed(2), A(s.batt_current_ma),
                    s.batt_power_w, s.batt_soc_pct, C(s.batt_temp_dc)]] },
          { head: ['Energy, kWh', 'Charged', 'Discharged'],
            rows: [['Today', kWh(s.day_batt_charge_wh),   kWh(s.day_batt_discharge_wh)],
                   ['Total', kWh(s.total_batt_charge_wh), kWh(s.total_batt_discharge_wh)]] },
        ];
        case 'pv': return [
          { head: ['String', 'Voltage, V', 'Current, A', 'Power, W'],
            rows: [1, 2].map(n => ['PV' + n, V(s['pv' + n + '_voltage_mv']),
                                   A(s['pv' + n + '_current_ma']), s['pv' + n + '_power_w']]) },
          { head: ['Energy, kWh', 'Generated'],
            rows: [['Today', kWh(s.day_pv_wh)], ['Total', kWh(s.total_pv_wh)]] },
        ];
        case 'grid': return [
          // The inverter reports one grid frequency for all phases.
          { head: ['Phase', 'Voltage, V', 'Current, A', 'Power, W', 'Frequency, Hz'],
            rows: phases.map(p => ['L' + p, V(s['grid_l' + p + '_voltage_mv']),
                                   A(s['grid_l' + p + '_current_ma']), s['grid_l' + p + '_power_w'],
                                   (s.grid_freq_chz / 100).toFixed(2)]) },
          { head: ['Energy, kWh', 'Bought', 'Sold'],
            rows: [['Today', kWh(s.day_grid_buy_wh),   kWh(s.day_grid_sell_wh)],
                   ['Total', kWh(s.total_grid_buy_wh), kWh(s.total_grid_sell_wh)]] },
        ];
        case 'load': return [
          // No load current register: P / U per phase.
          { head: ['Phase', 'Voltage, V', 'Current ≈, A', 'Power, W', 'Inverter output, W'],
            rows: phases.map(p => {
              const mv = s['load_l' + p + '_voltage_mv'], w = s['load_l' + p + '_power_w'];
              return ['L' + p, V(mv), mv > 0 ? A(Math.abs(w) / mv * 1e6) : '—', w,
                      s['inv_l' + p + '_power_w']];
            }) },
          { head: ['Energy, kWh', 'Consumed'],
            rows: [['Today', kWh(s.day_load_wh)], ['Total', kWh(s.total_load_wh)]] },
          { head: ['Inverter', 'DC side, °C', 'AC side, °C'],
            rows: [['Temperature', C(s.dc_temp_dc), C(s.ac_temp_dc)]] },
        ];
      }
      return [];
    },

    canControl() {
      return !!(this.info && this.info.control.enabled) && !this.cmdBusy;
    },

    // The centre button switches the inverter, when control is enabled.
    toggleInverter() {
      if (!this.canControl()) return;
      const name = this.live.running ? 'power_off' : 'power_on';
      const c = this.info.control.commands.find(c => c.name === name);
      if (c) this.runCommand(c);
    },


    _startLivePolling() {
      let busy = false;
      const poll = async () => {
        if (busy) return;   // a slow answer must not stack requests up
        busy = true;
        const name = this.inverter;
        try {
          const r = await fetch(`${API.live}?${this.invQ()}`);
          if (!r.ok) throw new Error(r.status);
          const j = await r.json();
          if (name === this.inverter) {   // not switched meanwhile
            this.live = j;
            this.linkStatus = j.online ? ''
              : (j.connected ? 'stale data' : 'logger offline');
          }
        } catch (e) {
          this.linkStatus = 'error: ' + e.message;
        }
        busy = false;
      };
      poll();
      setInterval(poll, LIVE_POLL_MS);

      // The header clock ticks on its own, on the second boundary, so it
      // never skips or repeats a second because of request timing.
      const tick = () => {
        this.status = this.linkStatus || new Date().toLocaleTimeString();
        setTimeout(tick, 1000 - Date.now() % 1000 + 5);
      };
      tick();
    },

    // ── history ──────────────────────────────────────────────────────────────

    _initCharts() {
      const fW = v => (v / 1000).toFixed(2) + ' kW';
      const fP = v => v.toFixed(0) + '%';
      const fV = v => v.toFixed(1) + ' V';
      CHARTS = {
        p: makeLineChart('cp', 'Power (grid + import / battery + discharge)', fW),
        s: makeLineChart('cs', 'Battery SoC', fP),
        b: makeLineChart('cb', 'Battery voltage', v => v.toFixed(2) + ' V'),
        g: makeLineChart('cg', 'Grid voltage', fV),
      };
    },

    async loadHistory() {
      this.histStatus = 'loading…';
      this._histLastLoad = Date.now();
      try {
        const t1 = Math.floor(Date.now() / 1000);
        const t0 = t1 - this.histHours * 3600;
        const r = await fetch(`${API.history}?${this.invQ()}&time_start=${t0}&time_end=${t1}&count=600`);
        if (!r.ok) throw new Error(r.status);
        const data = (await r.json()).data;

        // Charts are created on the first visible History render: Chart.js
        // cannot size itself inside a display:none container.
        await this.$nextTick();
        if (!CHARTS) this._initCharts();
        Object.values(CHARTS).forEach(c => c && c.resize());

        setSeries(CHARTS.p, data, [
          { label: 'PV',      color: C.pv,   fn: d => d.pv1_power_w + d.pv2_power_w },
          { label: 'Load',    color: C.load, fn: d => d.load_power_w },
          { label: 'Grid',    color: C.grid, fn: d => d.grid_power_w },
          { label: 'Battery', color: C.batt, fn: d => d.batt_power_w },
        ]);
        setSeries(CHARTS.s, data, [
          { label: 'SoC', color: C.batt, fn: d => d.batt_soc_pct },
        ]);
        setSeries(CHARTS.b, data, [
          { label: 'Voltage', color: C.l2, fn: d => d.batt_voltage_mv / 1000 },
        ]);
        setSeries(CHARTS.g, data, [1, 2, 3].map(p => ({
          label: 'L' + p, color: C['l' + p],
          fn: d => d['grid_l' + p + '_voltage_mv'] / 1000,
        })));

        this.histStatus = data.length
          ? `${data.length} pts · ${new Date().toLocaleTimeString()}`
          : 'no data';
      } catch (e) {
        this.histStatus = 'error: ' + e.message;
      }
    },

    // ── energy ───────────────────────────────────────────────────────────────

    energyMonthLabel() {
      return new Date(this.energyYear, this.energyMonth - 1, 1)
        .toLocaleString('en', { month: 'long', year: 'numeric' });
    },

    canNextEnergyMonth() {
      const now = new Date();
      return this.energyYear * 12 + this.energyMonth <
             now.getFullYear() * 12 + now.getMonth() + 1;
    },

    shiftEnergyMonth(delta) {
      if (delta > 0 && !this.canNextEnergyMonth()) return;
      const m = this.energyYear * 12 + (this.energyMonth - 1) + delta;
      this.energyYear  = Math.floor(m / 12);
      this.energyMonth = m % 12 + 1;
      this.loadEnergy();
    },

    _initEnergyChart() {
      const cv = document.getElementById('ce');
      if (!cv || !window.Chart) return;
      ENERGY_CHART = new window.Chart(cv, {
        type: 'bar',
        data: { labels: [], datasets: [] },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          plugins: {
            title:  { display: true, text: 'Daily energy (kWh)', align: 'start',
                      color: AXIS_CLR, font: AXIS_FONT },
            legend: { display: true,
                      labels: { boxWidth: 12, boxHeight: 12,
                                color: AXIS_CLR, font: AXIS_FONT } },
            tooltip: {
              titleFont: AXIS_FONT, bodyFont: AXIS_FONT,
              callbacks: {
                label: ctx => ctx.dataset.label + ': ' + ctx.parsed.y.toFixed(1) + ' kWh',
              },
            },
          },
          scales: {
            x: { ticks: { color: AXIS_CLR, font: AXIS_FONT }, grid: { color: GRID_CLR } },
            y: { ticks: { color: AXIS_CLR, font: AXIS_FONT, callback: v => v + ' kWh' },
                 grid: { color: GRID_CLR }, min: 0 },
          },
        },
      });
    },

    drawEnergyBars() {
      if (!ENERGY_CHART) return;
      ENERGY_CHART.data.labels = ENERGY_ROWS.map((_, i) => String(i + 1));
      const hidden = hiddenLabels(ENERGY_CHART);
      ENERGY_CHART.data.datasets = ENERGY_BARS[this.energyTab].map(([label, key, color]) => ({
        label, backgroundColor: color, borderColor: color, hidden: hidden.has(label),
        data: ENERGY_ROWS.map(e => e ? e[key] / 1000 : 0),
      }));
      ENERGY_CHART.update('none');
    },

    _drawPies() {
      for (const pie of this.energyPies) {
        const cv = document.getElementById('pie-' + pie.id);
        if (!cv || !window.Chart) continue;
        const data = {
          labels:   pie.slices.map(sl => sl.label),
          datasets: [{ data: pie.slices.map(sl => sl.kwh),
                       backgroundColor: pie.slices.map(sl => sl.color),
                       borderWidth: 1 }],
        };
        if (ENERGY_PIES[pie.id]) {
          ENERGY_PIES[pie.id].data = data;
          ENERGY_PIES[pie.id].update('none');
          continue;
        }
        ENERGY_PIES[pie.id] = new window.Chart(cv, {
          type: 'doughnut',
          data,
          options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            cutout: '55%',
            plugins: {
              legend: { display: false },
              tooltip: {
                titleFont: AXIS_FONT, bodyFont: AXIS_FONT,
                callbacks: { label: ctx => ctx.label + ': ' + ctx.parsed.toFixed(1) + ' kWh' },
              },
            },
          },
        });
      }
    },

    async loadEnergy() {
      this.energyStatus = 'loading…';
      try {
        const r = await fetch(`${API.daily}?${this.invQ(this.energyInverter)}&year=${this.energyYear}&month=${this.energyMonth}`);
        if (!r.ok) throw new Error(r.status);
        const j = await r.json();

        const days = new Date(this.energyYear, this.energyMonth, 0).getDate();
        const byDate = {};
        j.data.forEach(d => { byDate[d.date] = d; });
        const prefix = `${this.energyYear}-${String(this.energyMonth).padStart(2, '0')}-`;
        ENERGY_ROWS = [];
        for (let day = 1; day <= days; day++)
          ENERGY_ROWS.push(byDate[prefix + String(day).padStart(2, '0')] || null);

        const sum = {};
        for (const e of j.data) {
          const parts = Object.assign({}, e, splitDay(e));
          for (const k in parts)
            if (typeof parts[k] === 'number') sum[k] = (sum[k] || 0) + parts[k];
        }
        const k = key => (sum[key] || 0) / 1000;

        this.energyPies = [
          { id: 'load', title: 'Consumption', total: k('load_wh'), slices: [
              { label: 'From solar',   kwh: k('loadFromPv'), color: C.pv },
              { label: 'From battery', kwh: k('battToLoad'), color: C.batt },
              { label: 'From grid',    kwh: k('gridToLoad'), color: C.grid },
          ] },
          { id: 'pv', title: 'Generation', total: k('pv_wh'), slices: [
              { label: 'To load',      kwh: k('pvToLoad'),   color: C.load },
              { label: 'To battery',   kwh: k('pvToBatt'),   color: C.batt },
              { label: 'To grid',      kwh: k('pvToGrid'),   color: C.sell },
          ] },
        ];
        this.energyTotals = [
          ['Solar generated',     k('pv_wh').toFixed(1)],
          ['Load consumed',       k('load_wh').toFixed(1)],
          ['Grid bought',         k('grid_buy_wh').toFixed(1)],
          ['Grid sold',           k('grid_sell_wh').toFixed(1)],
          ['Battery charged',     k('batt_charge_wh').toFixed(1)],
          ['Battery discharged',  k('batt_discharge_wh').toFixed(1)],
        ];

        await this.$nextTick();
        this._drawPies();
        if (!ENERGY_CHART) this._initEnergyChart();
        if (!ENERGY_CHART) { this.energyStatus = 'chart error'; return; }
        ENERGY_CHART.resize();
        this.drawEnergyBars();

        this.energyStatus = j.data.length
          ? new Date().toLocaleTimeString() : 'no data for this month';
      } catch (e) {
        this.energyStatus = 'error: ' + e.message;
      }
    },

    // ── info ─────────────────────────────────────────────────────────────────

    async loadInfo() {
      try {
        const name = this.inverter;
        const r = await fetch(`${API.info}?${this.invQ()}`);
        if (r.ok && name === this.inverter) this.info = await r.json();
      } catch (_) {}
    },

    // Labels of the commands the control block offers, by API name.
    cmdLabel(name) {
      return { power_on: 'Turn on', power_off: 'Turn off' }[name] || name;
    },

    // The command is only queued here; its outcome arrives with api/live
    // as a new last_command.
    // The power button opens the password dialog; the command goes out
    // from there, with the password, which the page does not keep.
    runCommand(c) {
      this.pw = { open: true, cmd: c, value: '', error: '', busy: false };
      this.$nextTick(() => { const el = document.getElementById('pw-input'); if (el) el.focus(); });
    },

    closePassword() { this.pw.open = false; this.pw.value = ''; },

    async submitPassword() {
      const c = this.pw.cmd;
      const prevAt = this.live.last_command ? this.live.last_command.at : 0;
      this.pw.busy  = true;
      this.pw.error = '';
      let r;
      try {
        r = await fetch(API.command, {
          method:  'POST',
          headers: { 'Content-Type': 'application/json' },
          body:    JSON.stringify({ inverter: this.inverter, name: c.name, password: this.pw.value }),
        });
      } catch (e) {
        this.pw.error = e.message;
        this.pw.busy  = false;
        return;
      }
      this.pw.busy = false;
      if (r.status !== 202) {
        // Wrong password and the lockout keep the dialog open to retry.
        this.pw.error = (await r.json()).error;
        this.pw.value = '';
        return;
      }
      this.closePassword();

      this.cmdBusy   = true;
      this.cmdStatus = `${this.cmdLabel(c.name)}: queued…`;
      const t0 = Date.now();
      while (Date.now() - t0 < CMD_WAIT_MS) {
        await new Promise(res => setTimeout(res, LIVE_POLL_MS));
        const lc = this.live.last_command;
        if (lc && lc.at !== prevAt) {
          this.cmdStatus = lc.ok
            ? `${this.cmdLabel(lc.name)}: done (register ${lc.before} → ${lc.after})`
            : `${this.cmdLabel(lc.name)}: failed: ${lc.error}`;
          this.cmdBusy = false;
          return;
        }
      }
      this.cmdStatus = 'no outcome reported';
      this.cmdBusy = false;
    },

    infoRows() {
      const i = this.info;
      if (!i) return [['Status', 'loading…']];
      const rows = [
        ['Logger address',   `${i.logger.host}:${i.logger.port}`],
        ['Logger serial',    String(i.logger.serial)],
        ['Modbus slave id',  String(i.logger.slave_id)],
        ['Poll period',      (i.logger.poll_period_ms / 1000) + ' s'],
        ['Connected',        i.logger.connected ? 'yes' : 'no'],
      ];
      if (i.inverter) {
        rows.push(['Inverter serial', i.inverter.serial]);
        rows.push(['Device type',     String(i.inverter.device_type)]);
      }
      rows.push(['Cycles ok / failed', `${i.stats.cycles_ok} / ${i.stats.cycles_failed}`]);
      rows.push(['Connections made',   String(i.stats.reconnects)]);
      rows.push(['Last cycle time',    i.stats.last_cycle_ms + ' ms']);
      if (i.stats.last_error)
        rows.push(['Last error', new Date(i.stats.last_error_at).toLocaleString() +
                                 ' — ' + i.stats.last_error]);
      return rows;
    },
  };
}
