'use strict';

// All API URLs are relative (no leading slash) so the page keeps working
// when a reverse proxy mounts this service under a path prefix.
const API = {
  live:    'api/live',
  info:    'api/info',
  history: 'api/history',
  daily:   'api/daily',
  command: 'api/command',
};

const LIVE_POLL_MS = 2000;
// Below this a flow line is drawn idle: sensors read a few watts at rest.
const FLOW_MIN_W   = 20;
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

// series = [{ label, color, fn }]; fn maps a sample to its y value.
function setSeries(chart, data, series) {
  if (!chart) return;
  chart.data.datasets = series.map(s => ({
    label:           s.label,
    borderColor:     s.color,
    backgroundColor: s.color,
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
    histHours:    24,
    histStatus:   '',
    energyYear:   new Date().getFullYear(),
    energyMonth:  new Date().getMonth() + 1,
    energyStatus: '',
    energyTotals: '',
    cmdBusy:      false,
    cmdStatus:    '',

    get s() { return this.live.sample || null; },

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

      this._startLivePolling();
      this.loadInfo();   // the centre button needs to know if control is on
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
    degC(dc)  { return (dc / 10).toFixed(1) + ' °C'; },
    pvText(n) {
      const s = this.s;
      return s['pv' + n + '_power_w'] + ' W · ' +
             (s['pv' + n + '_voltage_mv'] / 1000).toFixed(1) + ' V · ' +
             (s['pv' + n + '_current_ma'] / 1000).toFixed(1) + ' A';
    },
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

    canControl() {
      return !!(this.info && this.info.control.enabled) && !this.cmdBusy;
    },

    // The centre button switches the inverter, when control is enabled.
    toggleInverter() {
      if (!this.canControl()) return;
      const name = this.s.run_state === 2 ? 'power_off' : 'power_on';
      const c = this.info.control.commands.find(c => c.name === name);
      if (c) this.runCommand(c);
    },

    detailRows() {
      const s = this.s;
      const v = mv => (mv / 1000).toFixed(1) + ' V';
      const rows = [
        ['PV1', this.pvText(1)],
        ['PV2', this.pvText(2)],
        ['Battery', v(s.batt_voltage_mv) + ' · ' + this.amps(s.batt_current_ma) +
                    ' · ' + this.degC(s.batt_temp_dc)],
        ['Grid frequency', (s.grid_freq_chz / 100).toFixed(2) + ' Hz'],
        ['Inverter output', this.watts(s.inv_power_w)],
        ['Inverter DC / AC temp', this.degC(s.dc_temp_dc) + ' / ' + this.degC(s.ac_temp_dc)],
        ['Total PV', this.kwh(s.total_pv_wh)],
        ['Total load', this.kwh(s.total_load_wh)],
        ['Total bought / sold', this.kwh(s.total_grid_buy_wh) + ' / ' + this.kwh(s.total_grid_sell_wh)],
        ['Total charged / discharged', this.kwh(s.total_batt_charge_wh) + ' / ' +
                                       this.kwh(s.total_batt_discharge_wh)],
      ];
      return rows;
    },

    _startLivePolling() {
      const poll = async () => {
        try {
          const r = await fetch(API.live);
          if (!r.ok) throw new Error(r.status);
          this.live = await r.json();
          this.status = this.live.online
            ? new Date().toLocaleTimeString()
            : (this.live.connected ? 'stale data' : 'logger offline');
        } catch (e) {
          this.status = 'error: ' + e.message;
        }
      };
      poll();
      setInterval(poll, LIVE_POLL_MS);
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
        const r = await fetch(`${API.history}?time_start=${t0}&time_end=${t1}&count=600`);
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

    async loadEnergy() {
      this.energyStatus = 'loading…';
      try {
        const r = await fetch(`${API.daily}?year=${this.energyYear}&month=${this.energyMonth}`);
        if (!r.ok) throw new Error(r.status);
        const j = await r.json();

        await this.$nextTick();
        if (!ENERGY_CHART) this._initEnergyChart();
        if (!ENERGY_CHART) { this.energyStatus = 'chart error'; return; }
        ENERGY_CHART.resize();

        const days = new Date(this.energyYear, this.energyMonth, 0).getDate();
        const byDate = {};
        j.data.forEach(d => { byDate[d.date] = d; });
        const prefix = `${this.energyYear}-${String(this.energyMonth).padStart(2, '0')}-`;
        const rows = [];
        for (let day = 1; day <= days; day++)
          rows.push(byDate[prefix + String(day).padStart(2, '0')] || null);

        const fields = [
          ['PV',             'pv_wh',             C.pv],
          ['Load',           'load_wh',           C.load],
          ['Grid buy',       'grid_buy_wh',       C.grid],
          ['Grid sell',      'grid_sell_wh',      C.sell],
          ['Batt charge',    'batt_charge_wh',    C.batt],
          ['Batt discharge', 'batt_discharge_wh', C.dis],
        ];
        ENERGY_CHART.data.labels = rows.map((_, i) => String(i + 1));
        ENERGY_CHART.data.datasets = fields.map(([label, key, color]) => ({
          label, backgroundColor: color, borderColor: color,
          data: rows.map(e => e ? e[key] / 1000 : 0),
        }));
        ENERGY_CHART.update('none');

        const total = key => j.data.reduce((a, e) => a + e[key], 0) / 1000;
        this.energyTotals = j.data.length
          ? fields.map(([label, key]) => `${label}: ${total(key).toFixed(1)} kWh`).join('   ')
          : '';
        this.energyStatus = j.data.length
          ? new Date().toLocaleTimeString() : 'no data for this month';
      } catch (e) {
        this.energyStatus = 'error: ' + e.message;
      }
    },

    // ── info ─────────────────────────────────────────────────────────────────

    async loadInfo() {
      try {
        const r = await fetch(API.info);
        if (r.ok) this.info = await r.json();
      } catch (_) {}
    },

    // Labels of the commands the control block offers, by API name.
    cmdLabel(name) {
      return { power_on: 'Turn on', power_off: 'Turn off' }[name] || name;
    },

    async runCommand(c) {
      if (!confirm(`${this.cmdLabel(c.name)}: ${c.description}?`)) return;
      this.cmdBusy   = true;
      this.cmdStatus = 'sending…';
      try {
        const r = await fetch(API.command, {
          method:  'POST',
          headers: { 'Content-Type': 'application/json' },
          body:    JSON.stringify({ name: c.name }),
        });
        const j = await r.json();
        this.cmdStatus = j.ok
          ? `done (register ${j.before} → ${j.after})`
          : 'failed: ' + j.error;
      } catch (e) {
        this.cmdStatus = 'failed: ' + e.message;
      }
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
