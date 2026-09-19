'use strict';
/* 植小伴 · 服务器管理台（原生 JS，无外网依赖） */

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));

const TABS = [
  ['overview', '总览', '📊'],
  ['devices', '设备', '📟'],
  ['plants', '植物', '🌿'],
  ['telemetry', '遥测', '📈'],
  ['tasks', '任务·事件', '✅'],
  ['talk', '对话语音', '🎙️'],
  ['media', '媒体库', '🖼️'],
  ['ai', 'AI 调试', '🤖'],
  ['actuators', '自动执行', '🤖'],
  ['billing', '会员订阅', '💎'],
  ['logs', '日志', '📜'],
];

const state = { tab: 'overview', cache: {}, logTimer: null };

/* ── 基础工具 ── */
function esc(s) {
  return String(s === null || s === undefined ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
function fmtBytes(n) {
  if (!n) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0, v = Number(n);
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)) + ' ' + u[i];
}
function fmtDur(sec) {
  sec = Math.max(0, Math.floor(sec || 0));
  const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600);
  const m = Math.floor(sec % 3600 / 60), s = sec % 60;
  if (d) return d + ' 天 ' + h + ' 小时';
  if (h) return h + ' 小时 ' + m + ' 分';
  if (m) return m + ' 分 ' + s + ' 秒';
  return s + ' 秒';
}
function hhmm(t) { return t ? String(t).replace('T', ' ').slice(5, 16) : '-'; }
function full(t) { return t ? String(t).replace('T', ' ').slice(0, 19) : '-'; }

function tagOf(status) {
  const map = { ok: ['ok', '成功'], empty: ['empty', '无结果'], error: ['error', '失败'],
                running: ['running', '进行中'], done: ['ok', '已完成'], open: ['warn', '待完成'] };
  const m = map[status] || ['', status || '-'];
  return '<span class="tag ' + m[0] + '">' + esc(m[1]) + '</span>';
}

let toastTimer = null;
function toast(msg, kind) {
  const el = $('#toast');
  el.textContent = msg;
  el.className = 'toast' + (kind === 'bad' ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.add('hidden'), 4200);
}

async function api(path, opts) {
  const r = await fetch('/api/v1/admin' + path, Object.assign({
    credentials: 'same-origin', cache: 'no-store',
  }, opts || {}));
  if (r.status === 401) { showLogin(); throw new Error('未登录'); }
  if (!r.ok) {
    let msg = 'HTTP ' + r.status;
    try { const j = await r.json(); msg = j.detail || msg; } catch (e) {}
    throw new Error(msg);
  }
  return r.json();
}

/* ── 登录 ── */
function showLogin() {
  $('#login').classList.remove('hidden');
  $('#app').classList.add('hidden');
}
function showApp(me) {
  $('#login').classList.add('hidden');
  $('#app').classList.remove('hidden');
  $('#login-tip').textContent = '';
  if (me && me.password_is_default) {
    toast('注意：正在使用默认管理口令，云端部署前请改环境变量 PLANT_ADMIN_PASSWORD', 'bad');
  }
  renderNav();
  go(state.tab);
}

$('#login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const btn = $('#login-btn');
  btn.disabled = true;
  $('#login-err').textContent = '';
  try {
    const r = await fetch('/api/v1/admin/login', {
      method: 'POST', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ password: $('#pwd').value }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.detail || ('HTTP ' + r.status));
    $('#pwd').value = '';
    const me = await api('/me');
    showApp(me);
  } catch (err) {
    $('#login-err').textContent = err.message || '登录失败';
  } finally {
    btn.disabled = false;
  }
});

$('#btn-logout').addEventListener('click', async () => {
  await fetch('/api/v1/admin/logout', { method: 'POST', credentials: 'same-origin' });
  showLogin();
});

/* 重启服务：先拿到响应，再等服务自己回来（后端见 app/services/service_ctl.py） */
$('#btn-restart').addEventListener('click', async () => {
  if (!confirm('确认重启服务器进程？重启期间设备接入与管理台会中断约 10 秒，服务会自动恢复。')) return;
  const btn = $('#btn-restart');
  const old = btn.textContent;
  btn.disabled = true;
  btn.textContent = '重启中…';
  try {
    await api('/service/restart', { method: 'POST' });
    toast('重启已下发，等待服务恢复…');
    const back = await waitServerBack();
    toast(back ? '服务已恢复' : '服务还没回来，请稍后手动刷新', back ? '' : 'bad');
  } catch (e) {
    toast('重启失败：' + e.message, 'bad');
  } finally {
    btn.disabled = false;
    btn.textContent = old;
  }
});

async function waitServerBack(maxMs) {
  const deadline = Date.now() + (maxMs || 90000);
  await new Promise((r) => setTimeout(r, 4500));
  while (Date.now() < deadline) {
    try {
      const r = await fetch('/api/v1/health', { cache: 'no-store' });
      if (r.ok) { go(state.tab, true); return true; }
    } catch (e) { /* 还在重启，继续等 */ }
    await new Promise((r) => setTimeout(r, 1500));
  }
  return false;
}
$('#btn-refresh').addEventListener('click', () => go(state.tab, true));
$('#modal-close').addEventListener('click', closeModal);
$('#modal').addEventListener('click', (e) => { if (e.target.id === 'modal') closeModal(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeModal(); });

function closeModal() { $('#modal').classList.add('hidden'); $('#modal-body').innerHTML = ''; }
function openModal(html) {
  $('#modal-body').innerHTML = html;
  $('#modal').classList.remove('hidden');
}

/* ── 导航 ── */
function renderNav() {
  $('#nav').innerHTML = TABS.map(([k, name, ico]) =>
    '<button data-tab="' + k + '"' + (k === state.tab ? ' class="active"' : '') + '>' +
    '<span class="ico">' + ico + '</span>' + esc(name) + '</button>').join('');
  $$('#nav button').forEach((b) => b.addEventListener('click', () => go(b.dataset.tab)));
}

function freshContent() {
  const prev = document.getElementById('content');
  const el = document.createElement('div');
  el.id = 'content';
  el.className = prev ? prev.className : 'content';
  if (prev) prev.replaceWith(el);
  else document.querySelector('.main').appendChild(el);
  return el;
}

async function go(tab, force) {
  state.tab = tab;
  $$('#nav button').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
  const meta = TABS.find((t) => t[0] === tab);
  $('#page-title').textContent = meta ? meta[1] : tab;
  if (state.logTimer) { clearInterval(state.logTimer); state.logTimer = null; }
  const el = freshContent();
  el.innerHTML = '<div class="card"><div class="empty-note">加载中…</div></div>';
  try {
    await (PAGES[tab] || PAGES.overview)(el, force);
  } catch (err) {
    if (err.message !== '未登录') {
      el.innerHTML = '<div class="card"><div class="empty-note">加载失败：' + esc(err.message) + '</div></div>';
    }
  }
  refreshTop();
}

async function refreshTop() {
  try {
    const me = await api('/me');
    $('#chip-clock').textContent = me.server_time ? me.server_time.slice(11, 19) : '--';
    const ov = state.cache.overview;
    if (ov) {
      $('#chip-ai').textContent = 'AI ' + ov.service.ai_mode + ' · ' + ov.service.model;
      $('#chip-dev').textContent = '设备在线 ' + ov.counts.devices_online + '/' + ov.counts.devices;
    }
  } catch (e) { /* ignore */ }
}
setInterval(() => {
  const c = $('#clock');
  if (c) c.textContent = new Date().toLocaleString('zh-CN', { hour12: false });
  const t = $('#chip-clock');
  if (t && !t.textContent.includes(':') === false) { /* noop */ }
}, 1000);
setInterval(refreshTop, 20000);

/* ── 折线图（canvas 自绘） ── */
function lineChart(canvas, pts, opts) {
  opts = opts || {};
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || 320, h = canvas.clientHeight || 150;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  const pad = { l: 42, r: 12, t: 12, b: 18 };
  const iw = w - pad.l - pad.r, ih = h - pad.t - pad.b;
  const vals = pts.filter((p) => p.v !== null && p.v !== undefined && !isNaN(p.v));
  g.font = '11px sans-serif';
  if (!vals.length) {
    g.fillStyle = '#8b9a92';
    g.textAlign = 'center';
    g.fillText('暂无数据', w / 2, h / 2);
    return;
  }
  let lo = Math.min.apply(null, vals.map((p) => p.v));
  let hi = Math.max.apply(null, vals.map((p) => p.v));
  if (hi === lo) { hi = lo + 1; lo = lo - 1; }
  const span = hi - lo;
  lo -= span * 0.08; hi += span * 0.08;
  const X = (i) => pad.l + (pts.length <= 1 ? iw / 2 : iw * i / (pts.length - 1));
  const Y = (v) => pad.t + ih - ih * (v - lo) / (hi - lo);
  g.strokeStyle = '#eaf1ec'; g.lineWidth = 1;
  for (let k = 0; k <= 3; k++) {
    const y = pad.t + ih * k / 3;
    g.beginPath(); g.moveTo(pad.l, y); g.lineTo(pad.l + iw, y); g.stroke();
    g.fillStyle = '#8b9a92'; g.textAlign = 'right';
    g.fillText((hi - (hi - lo) * k / 3).toFixed(0), pad.l - 6, y + 4);
  }
  const color = opts.color || '#2e9e5b';
  g.beginPath();
  let started = false;
  pts.forEach((p, i) => {
    if (p.v === null || p.v === undefined || isNaN(p.v)) { started = false; return; }
    const x = X(i), y = Y(p.v);
    if (!started) { g.moveTo(x, y); started = true; } else { g.lineTo(x, y); }
  });
  g.strokeStyle = color; g.lineWidth = 1.8; g.stroke();
  g.lineTo(X(pts.length - 1), pad.t + ih);
  g.lineTo(X(0), pad.t + ih);
  g.closePath();
  g.fillStyle = color + '18'; g.fill();
  const lastPt = vals[vals.length - 1];
  g.beginPath();
  g.arc(X(vals[vals.length - 1].i), Y(lastPt.v), 3, 0, Math.PI * 2);
  g.fillStyle = color; g.fill();
  g.fillStyle = '#4a5c53'; g.textAlign = 'left';
  g.fillText(opts.unit ? (lastPt.v.toFixed(opts.digits === undefined ? 1 : opts.digits) + ' ' + opts.unit) : lastPt.v.toFixed(1),
    pad.l + 4, pad.t + 10);
  g.fillStyle = '#8b9a92'; g.textAlign = 'left';
  g.fillText(hhmm(pts[0].t), pad.l, h - 4);
  g.textAlign = 'right';
  g.fillText(hhmm(pts[pts.length - 1].t), pad.l + iw, h - 4);
}

function chartCard(title, id, color) {
  return '<div class="card"><div class="card-head"><h3>' + esc(title) + '</h3></div>' +
    '<div class="card-body"><div class="chart-wrap"><canvas class="chart" id="' + id + '"></canvas></div></div></div>';
}

function seriesOf(rows, key) {
  return rows.map((r, i) => ({ t: r.ts, v: r[key] === null || r[key] === undefined ? null : Number(r[key]), i: i }));
}
/* ── 公共片段 ── */
function stat(k, v, s) {
  return `<div class="stat"><div class="k"><span>${esc(k)}</span></div>
    <div class="v">${esc(v)}</div><div class="s">${esc(s || '')}</div></div>`;
}
function onlineTag(v) {
  const d = (v && typeof v === 'object') ? v : { online: !!v };
  const seen = d.last_seen_at ? String(d.last_seen_at).replace(' ', 'T') : '';
  const ageS = seen ? (Date.now() - new Date(seen).getTime()) / 1000 : null;
  const tip = ' title="最后活动 ' + full(d.last_seen_at) + '"';
  if (d.online) {
    return '<span class="chip" style="background:#e7f6ee;color:#177a4e"' + tip +
      '><i class="dot"></i>在线</span>';
  }
  if (ageS !== null && ageS >= 0 && ageS <= 86400) {
    return '<span class="chip" style="background:#fdf3e4;color:#96631a"' + tip +
      '><i class="dot"></i>最近活跃</span>';
  }
  return '<span class="chip gray"' + tip + '><i class="dot"></i>离线</span>';
}
const EV_ICO = { photo: '📷', water: '💧', task: '✅', diagnose: '🩺', badge: '🏆', milestone: '🌱',
                 voice: '🎙️', chat: '💬', sync: '🔄', sensor: '📈' };
function evItem(e) {
  return `<div class="item"><div class="ico">${EV_ICO[e.type] || '•'}</div>
    <div class="t"><b>${esc(e.title || e.type)}</b>
      <p>${esc(e.summary || '')}${e.plant_name ? ' · ' + esc(e.plant_name) : ''}${e.source ? ' · 来源 ' + esc(e.source) : ''}</p></div>
    <div class="ts">${full(e.event_ts)}</div></div>`;
}
function downloadCsv(name, rows, cols) {
  const head = cols.map((c) => c[0]).join(',');
  const body = rows.map((r) => cols.map((c) => {
    const v = r[c[1]];
    if (v === null || v === undefined) return '';
    const s = String(v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }).join(',')).join('\n');
  const blob = new Blob(['\ufeff' + head + '\n' + body], { type: 'text/csv;charset=utf-8' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a); a.click(); a.remove();
}

const PAGES = {};

/* ── 总览 ── */
PAGES.overview = async (el) => {
  const d = await api('/overview');
  state.cache.overview = d;
  const c = d.counts, s = d.service, t = d.today;
  const dp = d.device_policy || { heartbeat_s: 60, offline_after_s: 150 };
  el.innerHTML = `
  <div class="grid stats">
    ${stat('设备在线', c.devices_online + ' / ' + c.devices, '已注册设备')}
    ${stat('植物', c.plants, '用户 ' + c.users)}
    ${stat('AI 今日调用', t.ai_jobs, '配额 ' + t.ai_quota + ' / 天')}
    ${stat('任务今日', t.tasks_done + ' / ' + t.tasks, '已完成 / 总数')}
    ${stat('事件累计', c.events, '今日 ' + t.events)}
    ${stat('媒体', c.media, '图片 + 音频')}
    ${stat('遥测点', c.telemetry, '传感器上报')}
    ${stat('语音消息', c.messages, 'AI 对话')}
  </div>
  <div class="grid two">
    <div class="card">
      <div class="card-head"><h3>服务状态</h3><span class="chip">${esc(s.ai_mode)}</span></div>
      <div class="card-body"><div class="kv">
        <div class="k">AI 引擎</div><div>${esc(s.provider)} · ${esc(s.model)}</div>
        <div class="k">运行时长</div><div>${fmtDur(s.uptime_s)}　PID ${s.pid}</div>
        <div class="k">服务器时间</div><div>${esc(s.server_time)}</div>
        <div class="k">数据库</div><div class="mono small">${esc(s.db_path)}<br>${fmtBytes(s.db_bytes)}</div>
        <div class="k">媒体目录</div><div class="mono small">${esc(s.media_dir)}<br>${fmtBytes(s.data_bytes)}（数据目录总量）</div>
        <div class="k">磁盘</div><div>可用 ${fmtBytes(s.disk_free)} / 共 ${fmtBytes(s.disk_total)}</div>
      </div></div>
    </div>
    <div class="card">
      <div class="card-head"><h3>最近事件</h3></div>
      <div class="card-body"><div class="feed">
        ${d.recent_events.map(evItem).join('') || '<div class="empty-note">暂无事件</div>'}
      </div></div>
    </div>
  </div>
  <div class="card">
    <div class="card-head"><h3>设备</h3><span class="chip gray">板卡心跳 ${dp.heartbeat_s} 秒一次；${dp.offline_after_s} 秒内有请求=在线，24 小时内=最近活跃</span></div>
    <table><thead><tr><th>SN</th><th>型号</th><th>固件</th><th>状态</th><th>最后活动</th><th>植物</th></tr></thead>
    <tbody>${d.devices.map((v) => `<tr><td class="mono">${esc(v.sn)}</td><td>${esc(v.model || '-')}</td>
      <td>${esc(v.fw_version || '-')}</td><td>${onlineTag(v)}</td>
      <td class="num">${full(v.last_seen_at)}</td><td>${v.plant_n}</td></tr>`).join('')
      || '<tr><td colspan="6" class="muted">暂无设备</td></tr>'}</tbody></table>
  </div>`;
};

/* ── 设备 ── */
let DEV_CACHE = [];
PAGES.devices = async (el) => {
  const $$ = (s) => Array.from(el.querySelectorAll(s));
  const d = await api('/devices');
  DEV_CACHE = d.devices;
  el.innerHTML = `<div class="card">
    <div class="card-head"><h3>设备列表</h3><span class="chip gray">共 ${d.devices.length} 台</span></div>
    <table><thead><tr><th>SN</th><th>型号</th><th>固件</th><th>状态</th><th>最后活动</th>
      <th>绑定植物</th><th>遥测</th><th>事件</th><th></th></tr></thead>
    <tbody>${d.devices.map((v, i) => `<tr class="clickable" data-dev="${i}">
      <td class="mono">${esc(v.sn)}</td><td>${esc(v.model || '-')}</td><td>${esc(v.fw_version || '-')}</td>
      <td>${onlineTag(v)}</td><td class="num">${full(v.last_seen_at)}</td>
      <td>${v.plants.map((p) => esc(p.name)).join('、') || '<span class="muted">未绑定</span>'}</td>
      <td class="num">${v.telemetry_n}</td><td class="num">${v.event_n}</td>
      <td class="actions"><button class="ghost">详情</button></td></tr>`).join('')
      || '<tr><td colspan="9" class="muted">暂无设备</td></tr>'}</tbody></table></div>`;
  $$('[data-dev]').forEach((tr) =>
    tr.addEventListener('click', () => deviceModal(DEV_CACHE[Number(tr.dataset.dev)])));
};

function deviceModal(v) {
  const t = v.last_telemetry;
  openModal(`<h3>设备 ${esc(v.sn)}</h3>
    <div class="kv" style="grid-template-columns:110px 1fr">
      <div class="k">设备 ID</div><div class="mono">${esc(v.id)}</div>
      <div class="k">型号 / 固件</div><div>${esc(v.model || '-')} / ${esc(v.fw_version || '-')}</div>
      <div class="k">状态</div><div>${onlineTag(v)}　最后活动 ${full(v.last_seen_at)}</div>
      <div class="k">注册时间</div><div>${full(v.created_at)}</div>
      <div class="k">绑定植物</div><div>${v.plants.map((p) => esc(p.name) + '（' + esc(p.species || '') + '，健康 ' + p.health_score + '）').join('<br>') || '<span class="muted">未绑定</span>'}</div>
      <div class="k">最近遥测</div><div>${t ? `${full(t.ts)}　湿度 ${fmtNum(t.moisture)}%　温度 ${fmtNum(t.temp)}°C　EC ${fmtNum(t.ec)}　pH ${fmtNum(t.ph)}` : '<span class="muted">无</span>'}</div>
      <div class="k">遥测 / 事件</div><div>${v.telemetry_n} 条 / ${v.event_n} 条</div>
      <div class="k">设备凭证</div><div><code id="tok" class="mono">${esc(mask(v.token))}</code>
        <button class="ghost" id="tok-btn" style="margin-left:8px">显示</button></div>
    </div>`);
  const btn = $('#tok-btn');
  if (btn) btn.addEventListener('click', () => {
    const on = btn.textContent === '隐藏';
    $('#tok').textContent = on ? mask(v.token) : (v.token || '');
    btn.textContent = on ? '显示' : '隐藏';
  });
}
function mask(s) { return s ? String(s).slice(0, 10) + '…' + String(s).slice(-6) : '-'; }
function fmtNum(v) { return (v === null || v === undefined) ? '-' : Number(v).toFixed(1); }

/* ── 植物 ── */
let PLANT_CACHE = [];
PAGES.plants = async (el) => {
  const $$ = (s) => Array.from(el.querySelectorAll(s));
  const d = await api('/plants');
  PLANT_CACHE = d.plants;
  el.innerHTML = `<div class="card"><div class="card-head"><h3>植物</h3>
      <span class="chip gray">共 ${d.plants.length} 盆</span></div>
    <div class="card-body"><div class="plant-grid">
    ${d.plants.map((p, i) => {
      const lt = p.last_telemetry;
      const pct = Math.max(0, Math.min(100, p.health_score | 0));
      return `<div class="plant-card" data-plant="${i}">
        <div class="top"><div class="mood">${esc(p.mood || '🌱')}</div>
          <div style="flex:1;min-width:0"><div class="name">${esc(p.name)}</div>
          <div class="meta">${esc(p.species || '未知品种')} · ${esc(p.location || '未设置位置')}</div></div></div>
        <div style="margin-top:10px"><div class="rowline small"><span>健康 ${p.health_score}</span>
          <span class="muted">${esc(p.health_level || '')}</span></div>
          <div class="bar"><i style="width:${pct}%;background:${pct >= 80 ? '#2e9e5b' : pct >= 60 ? '#d99425' : '#d9534f'}"></i></div></div>
        <div class="metrics">
          <span>成长 ${p.growth}</span><span>徽章 ${p.badge_n}</span>
          <span>任务 ${p.tasks_done}/${p.tasks_total}</span>
          <span>事件 ${p.event_n}</span><span>媒体 ${p.media_n}</span><span>对话 ${p.msg_n}</span>
        </div>
        <div class="meta" style="margin-top:8px">${lt ? '最近：湿 ' + fmtNum(lt.moisture) + '% · ' + fmtNum(lt.temp) + '°C · EC ' + fmtNum(lt.ec) + ' · pH ' + fmtNum(lt.ph) : '暂无遥测'}</div>
      </div>`;
    }).join('') || '<div class="empty-note">暂无植物</div>'}
    </div></div></div>`;
  $$('[data-plant]').forEach((c) =>
    c.addEventListener('click', () => plantModal(PLANT_CACHE[Number(c.dataset.plant)])));
};

async function plantModal(p) {
  openModal(`<h3>${esc(p.name)} · 加载中…</h3>`);
  const d = await api('/plants/' + encodeURIComponent(p.id) + '?days=7');
  const rows = d.telemetry.map((r, i) => Object.assign({ i: i, t: r.ts }, r));
  openModal(`<h3>${esc(p.mood || '')} ${esc(p.name)} <span class="muted small">${esc(p.species || '')}</span></h3>
    <div class="grid stats" style="margin-bottom:12px">
      ${stat('健康分', p.health_score, p.health_level || '')}
      ${stat('成长值', p.growth, '累计')}
      ${stat('徽章', (d.badges || []).length, '已解锁')}
      ${stat('任务完成', p.tasks_done + '/' + p.tasks_total, '')}
    </div>
    <div class="grid two">
      <div>${chartCard('近 7 天土壤湿度（%）', 'pm1', '#3d8bd4')}</div>
      <div>${chartCard('近 7 天温度（°C）', 'pm2', '#d99425')}</div>
    </div>
    <div class="card" style="margin-top:12px"><div class="card-head"><h3>最近事件</h3></div>
      <div class="card-body"><div class="feed">${(d.events || []).slice(0, 8).map(evItem).join('') || '<div class="empty-note">暂无</div>'}</div></div></div>
    <div class="card" style="margin-top:12px"><div class="card-head"><h3>成长日志</h3></div>
      <div class="card-body"><table><thead><tr><th>时间</th><th>原因</th><th>成长</th></tr></thead><tbody>
      ${(d.growth || []).slice(0, 10).map((g) => `<tr><td class="num">${full(g.created_at)}</td>
        <td>${esc(g.reason || '')}</td><td class="num">+${g.delta}</td></tr>`).join('') || '<tr><td colspan="3" class="muted">暂无</td></tr>'}
      </tbody></table></div></div>`);
  lineChart($('#pm1'), seriesOf(rows, 'moisture'), { color: '#3d8bd4', unit: '%', digits: 0 });
  lineChart($('#pm2'), seriesOf(rows, 'temp'), { color: '#d99425', unit: '°C' });
}

/* ── 遥测 ── */
let TEL_STATE = { plant: '', days: 7 };
PAGES.telemetry = async (el) => {
  const $ = (s) => el.querySelector(s);
  if (!PLANT_CACHE.length) { try { PLANT_CACHE = (await api('/plants')).plants; } catch (e) {} }
  el.innerHTML = `
  <div class="card"><div class="card-head"><h3>筛选</h3>
    <div class="rowline">
      <select id="tel-plant">
        <option value="">全部植物</option>
        ${PLANT_CACHE.map((p) => `<option value="${esc(p.id)}"${TEL_STATE.plant === p.id ? ' selected' : ''}>${esc(p.name)}</option>`).join('')}
      </select>
      <select id="tel-days">
        ${[1, 3, 7, 30].map((n) => `<option value="${n}"${TEL_STATE.days === n ? ' selected' : ''}>近 ${n} 天</option>`).join('')}
      </select>
      <button class="primary" id="tel-load">查询</button>
      <button class="ghost" id="tel-csv">导出 CSV</button>
      <span class="chip gray" id="tel-count">-</span>
    </div></div></div>
  <div class="grid two">
    ${chartCard('土壤湿度（%）', 'tc1', '#3d8bd4')}
    ${chartCard('温度（°C）', 'tc2', '#d99425')}
    ${chartCard('电导率 EC（µS/cm）', 'tc3', '#8a7fd0')}
    ${chartCard('酸碱度 pH', 'tc4', '#2e9e5b')}
    ${chartCard('盐分（mg/kg）', 'tc5', '#c9584f')}
    ${chartCard('氮 N（mg/kg）', 'tc6', '#3fa7a0')}
    ${chartCard('磷 P（mg/kg）', 'tc7', '#b06bb0')}
    ${chartCard('钾 K（mg/kg）', 'tc8', '#7a8b3f')}
  </div>`;
  const load = async () => {
    TEL_STATE.plant = $('#tel-plant').value;
    TEL_STATE.days = Number($('#tel-days').value);
    const d = await api('/telemetry?plant_id=' + encodeURIComponent(TEL_STATE.plant) + '&days=' + TEL_STATE.days);
    const rows = d.rows.map((r, i) => Object.assign({ i: i }, r));
    $('#tel-count').textContent = '共 ' + d.total + ' 点' + (d.total > rows.length ? '（抽样 ' + rows.length + '）' : '');
    lineChart($('#tc1'), seriesOf(rows, 'moisture'), { color: '#3d8bd4', unit: '%', digits: 0 });
    lineChart($('#tc2'), seriesOf(rows, 'temp'), { color: '#d99425', unit: '°C' });
    lineChart($('#tc3'), seriesOf(rows, 'ec'), { color: '#8a7fd0', unit: '', digits: 0 });
    lineChart($('#tc4'), seriesOf(rows, 'ph'), { color: '#2e9e5b', unit: '' });
    lineChart($('#tc5'), seriesOf(rows, 'salt'), { color: '#c9584f', unit: '', digits: 0 });
    lineChart($('#tc6'), seriesOf(rows, 'nitrogen'), { color: '#3fa7a0', unit: '', digits: 0 });
    lineChart($('#tc7'), seriesOf(rows, 'phosphorus'), { color: '#b06bb0', unit: '', digits: 0 });
    lineChart($('#tc8'), seriesOf(rows, 'potassium'), { color: '#7a8b3f', unit: '', digits: 0 });
    el.dataset.csv = JSON.stringify(rows.map((r) => ({
      ts: r.ts, moisture: r.moisture, temp: r.temp, ec: r.ec, ph: r.ph,
      salt: r.salt, nitrogen: r.nitrogen, phosphorus: r.phosphorus,
      potassium: r.potassium, source: r.source })));
  };
  $('#tel-load').addEventListener('click', load);
  $('#tel-csv').addEventListener('click', () => {
    const rows = JSON.parse(el.dataset.csv || '[]');
    if (!rows.length) { toast('还没有数据'); return; }
    downloadCsv('植小伴_遥测_' + new Date().toISOString().slice(0, 10) + '.csv', rows,
      [['时间', 'ts'], ['湿度%', 'moisture'], ['温度℃', 'temp'], ['EC', 'ec'], ['pH', 'ph'],
       ['盐分', 'salt'], ['氮N', 'nitrogen'], ['磷P', 'phosphorus'], ['钾K', 'potassium'],
       ['来源', 'source']]);
  });
  await load();
};

/* ── 任务·事件 ── */
PAGES.tasks = async (el) => {
  const $ = (s) => el.querySelector(s);
  const today = new Date().toLocaleDateString('sv-SE');
  el.innerHTML = `
  <div class="card"><div class="card-head"><h3>任务</h3>
    <div class="rowline"><input type="date" id="tk-date" value="${today}">
      <button class="primary" id="tk-load">查询</button>
      <span class="chip gray" id="tk-sum">-</span></div></div>
    <div id="tk-body"></div></div>
  <div class="card"><div class="card-head"><h3>事件流</h3>
    <div class="rowline"><select id="ev-days">${[7, 30, 90].map((n) => `<option value="${n}">近 ${n} 天</option>`).join('')}</select></div></div>
    <div class="card-body"><div class="feed" id="ev-body"><div class="empty-note">加载中…</div></div></div></div>`;
  const loadTasks = async () => {
    const d = await api('/tasks?date=' + $('#tk-date').value);
    const done = d.tasks.filter((t) => t.status === 'done').length;
    $('#tk-sum').textContent = `${d.date}：完成 ${done} / ${d.tasks.length}`;
    $('#tk-body').innerHTML = `<table><thead><tr><th>状态</th><th>任务</th><th>植物</th>
      <th>来源</th><th>完成时间</th><th>成长</th></tr></thead><tbody>
      ${d.tasks.map((t) => `<tr><td>${tagOf(t.status)}</td><td>${esc(t.content)}</td>
        <td>${esc(t.plant_name || '-')}</td><td>${esc(t.source || '-')}</td>
        <td class="num">${t.completed_at ? full(t.completed_at) : '-'}</td>
        <td class="num">+${t.growth_delta}</td></tr>`).join('') || '<tr><td colspan="6" class="muted">当天没有任务</td></tr>'}
      </tbody></table>`;
  };
  const loadEvents = async () => {
    const d = await api('/events?days=' + $('#ev-days').value + '&limit=60');
    $('#ev-body').innerHTML = d.events.map((e) => evItem(e) +
      (e.media_url ? `<div class="item"><div class="ico">🖼️</div><div class="t">
        <a href="${esc(e.media_url)}" target="_blank"><img src="${esc(e.media_url)}" style="max-width:150px;border-radius:8px;border:1px solid var(--line)"></a>
        <p class="muted small">点击查看原图</p></div><div class="ts"></div></div>` : '')).join('')
      || '<div class="empty-note">该时间段没有事件</div>';
  };
  $('#tk-load').addEventListener('click', loadTasks);
  $('#ev-days').addEventListener('change', loadEvents);
  await Promise.all([loadTasks(), loadEvents()]);
};

/* ── 对话语音 ── */
PAGES.talk = async (el) => {
  const $ = (s) => el.querySelector(s);
  const $$ = (s) => Array.from(el.querySelectorAll(s));
  const d = await api('/conversations');
  el.innerHTML = `<div class="card"><div class="card-head"><h3>语音对话</h3>
      <span class="chip gray">共 ${d.conversations.length} 个会话</span>
      <span class="chip gray">点击左侧会话查看全文与录音</span></div>
    <div class="card-body"><div class="grid two" style="grid-template-columns:280px 1fr">
      <div><table><tbody>${d.conversations.map((c, i) => `<tr class="clickable" data-cv="${i}">
        <td><b>${esc(c.plant_name || '未命名')}</b><div class="small muted">${esc((c.last_text || '').slice(0, 24))}</div>
          <div class="small muted">${full(c.last_ts || c.updated_at)} · ${c.msg_n} 条</div></td></tr>`).join('')
        || '<tr><td class="muted">暂无会话</td></tr>'}</tbody></table></div>
      <div id="cv-body"><div class="empty-note">选择左边一个会话</div></div>
    </div></div></div>`;
  const open = async (c) => {
    const d2 = await api('/conversations/' + encodeURIComponent(c.id));
    $('#cv-body').innerHTML = `<div class="talk">${d2.messages.map((m) => {
      const user = m.role === 'user';
      return `<div class="msg ${user ? 'user' : 'bot'}">${esc(m.text)}
        ${m.audio_url ? `<audio controls preload="none" src="${esc(m.audio_url)}"></audio>` : ''}
        <div class="meta">${user ? '用户' : '小绿'} · ${full(m.ts)}${m.audio_media_id ? ' · 🎙️ 语音' : ''}</div></div>`;
    }).join('') || '<div class="empty-note">没有消息</div>'}</div>`;
  };
  $$('[data-cv]').forEach((tr) =>
    tr.addEventListener('click', () => open(d.conversations[Number(tr.dataset.cv)])));
  if (d.conversations.length) open(d.conversations[0]);
};

/* ── 媒体库 ── */
let MEDIA_STATE = { kind: '', days: 30, plant: '' };
PAGES.media = async (el) => {
  const $ = (s) => el.querySelector(s);
  if (!PLANT_CACHE.length) { try { PLANT_CACHE = (await api('/plants')).plants; } catch (e) {} }
  el.innerHTML = `<div class="card"><div class="card-head"><h3>筛选</h3><div class="rowline">
      <select id="md-kind">
        ${[['', '全部类型'], ['image', '图片'], ['audio', '音频']].map(([v, n]) => `<option value="${v}"${MEDIA_STATE.kind === v ? ' selected' : ''}>${n}</option>`).join('')}
      </select>
      <select id="md-plant"><option value="">全部植物</option>
        ${PLANT_CACHE.map((p) => `<option value="${esc(p.id)}"${MEDIA_STATE.plant === p.id ? ' selected' : ''}>${esc(p.name)}</option>`).join('')}</select>
      <select id="md-days">${[1, 7, 30, 365].map((n) => `<option value="${n}"${MEDIA_STATE.days === n ? ' selected' : ''}>近 ${n} 天</option>`).join('')}</select>
      <button class="primary" id="md-load">查询</button>
      <span class="chip gray" id="md-sum">-</span></div></div></div>
    <div id="md-body"><div class="card"><div class="empty-note">加载中…</div></div></div>`;
  const load = async () => {
    MEDIA_STATE.kind = $('#md-kind').value;
    MEDIA_STATE.plant = $('#md-plant').value;
    MEDIA_STATE.days = Number($('#md-days').value);
    const d = await api('/media?kind=' + MEDIA_STATE.kind + '&plant_id=' + encodeURIComponent(MEDIA_STATE.plant) +
      '&days=' + MEDIA_STATE.days + '&limit=400');
    const imgs = d.media.filter((m) => m.kind === 'image');
    const auds = d.media.filter((m) => m.kind === 'audio');
    $('#md-sum').textContent = `${d.media.length} 个 · ${fmtBytes(d.bytes)}`;
    $('#md-body').innerHTML = `
      <div class="card"><div class="card-head"><h3>图片 (${imgs.length})</h3></div><div class="card-body">
        <div class="media-grid">${imgs.map((m) => `<a class="media-cell" href="${esc(m.url)}" target="_blank">
          <img loading="lazy" src="${esc(m.url)}" alt="${esc(m.id)}">
          <div class="cap"><span>${esc((m.created_at || '').slice(5, 16))}</span><span>${fmtBytes(m.bytes)}</span></div></a>`).join('')
          || '<div class="empty-note">没有图片</div>'}</div></div></div>
      <div class="card" style="margin-top:14px"><div class="card-head"><h3>音频 (${auds.length})</h3>
        <span class="chip gray">用户录音 + 小绿语音</span></div><div class="card-body">
        <div class="grid" style="grid-template-columns:repeat(auto-fill,minmax(260px,1fr))">
        ${auds.map((m) => `<div class="audio-cell">
          <div class="small"><b>${esc(m.meta || 'voice')}</b> <span class="muted">${esc(m.plant_name || '')}</span></div>
          <audio controls preload="none" src="${esc(m.url)}"></audio>
          <div class="small muted">${full(m.created_at)} · ${fmtBytes(m.bytes)}</div></div>`).join('')
          || '<div class="empty-note">没有音频</div>'}</div></div></div>`;
  };
  $('#md-load').addEventListener('click', load);
  await load();
};

/* ── AI 调试 ── */
PAGES.ai = async (el) => {
  const $ = (s) => el.querySelector(s);
  const $$ = (s) => Array.from(el.querySelectorAll(s));
  el.innerHTML = `<div class="card"><div class="card-head"><h3>AI 调用记录</h3>
      <div class="rowline">
        <select id="ai-status">
          ${[['', '全部状态'], ['ok', '成功'], ['empty', '无结果'], ['error', '失败'], ['running', '进行中']].map(([v, n]) => `<option value="${v}">${n}</option>`).join('')}
        </select>
        <button class="primary" id="ai-load">查询</button>
        <span class="chip gray" id="ai-stat">-</span></div></div>
      <div id="ai-body"><div class="empty-note">加载中…</div></div></div>`;
  const load = async () => {
    const d = await api('/ai_jobs?status=' + $('#ai-status').value + '&limit=200');
    const s = d.stats;
    $('#ai-stat').textContent = `共 ${s.total} · 成功 ${s.ok} · 无结果 ${s.empty} · 失败 ${s.error} · 今日 ${s.today}`;
    $('#ai-body').innerHTML = `<table><thead><tr><th>时间</th><th>类型</th><th>状态</th>
      <th>模型</th><th>来源</th><th>输入</th><th>结果大小</th><th></th></tr></thead><tbody>
      ${d.jobs.map((j) => `<tr><td class="num">${full(j.created_at)}</td><td>${esc(j.job_type)}</td>
        <td>${tagOf(j.status)}</td><td class="small">${esc(j.model || '-')}</td>
        <td>${esc(j.request_source || '-')}</td><td class="mono small">${esc(j.input_ref || '-')}</td>
        <td class="num">${j.sj_len ? fmtBytes(j.sj_len) : '-'}</td>
        <td class="actions"><button class="ghost" data-job="${esc(j.id)}">详情</button></td></tr>`).join('')
        || '<tr><td colspan="8" class="muted">没有记录</td></tr>'}
      </tbody></table>`;
    $$('#ai-body [data-job]').forEach((b) => b.addEventListener('click', () => jobModal(b.dataset.job)));
  };
  $('#ai-load').addEventListener('click', load);
  await load();
};

async function jobModal(id) {
  openModal('<h3>加载中…</h3>');
  const d = await api('/ai_jobs/' + encodeURIComponent(id));
  const j = d.job;
  let pretty = '';
  try { pretty = JSON.stringify(JSON.parse(j.structured_json || '{}'), null, 2); }
  catch (e) { pretty = j.structured_json || ''; }
  openModal(`<h3>AI 调用 ${esc(j.id)}</h3>
    <div class="kv" style="grid-template-columns:110px 1fr;margin-bottom:12px">
      <div class="k">时间</div><div>${full(j.created_at)}</div>
      <div class="k">类型 / 状态</div><div>${esc(j.job_type)}　${tagOf(j.status)}</div>
      <div class="k">模型</div><div>${esc(j.provider || '')} · ${esc(j.model || '')}</div>
      <div class="k">来源 / 输入</div><div>${esc(j.request_source || '-')} · <span class="mono">${esc(j.input_ref || '-')}</span></div>
      ${j.error ? `<div class="k">错误</div><div style="color:#b33">${esc(j.error)}</div>` : ''}
    </div>
    ${j.media_url ? `<div style="margin-bottom:12px"><img src="${esc(j.media_url)}" style="max-width:220px;border-radius:8px;border:1px solid var(--line)"></div>` : ''}
    <div class="rowline" style="margin-bottom:8px">
      <b class="small">结构化结果</b>
      ${j.job_type === 'diagnose' && j.media_url ? '<button class="primary" id="rerun">重跑这次诊断</button>' : ''}
      <span class="small muted" id="rerun-note"></span></div>
    <pre class="json">${esc(pretty || '（空）')}</pre>
    <div class="rowline" style="margin:12px 0 6px"><b class="small">模型原始输出</b></div>
    <pre class="json">${esc(j.raw_model_text || '（空）')}</pre>`);
  const b = $('#rerun');
  if (b) b.addEventListener('click', async () => {
    if (!confirm('重跑会调用一次真实 AI（消耗配额）并写入一条新的体检事件，继续？')) return;
    b.disabled = true;
    $('#rerun-note').textContent = '重跑中…（可能 10-60 秒）';
    try {
      const r = await api('/ai_jobs/' + encodeURIComponent(id) + '/rerun', { method: 'POST' });
      $('#rerun-note').textContent = '完成，新任务 ' + (r.new_job_id || '');
      toast('重跑完成：' + (r.recognized ? '识别成功' : '未识别'));
      PAGES.ai($('#content'));
    } catch (e) {
      $('#rerun-note').textContent = '失败：' + e.message;
      toast('重跑失败：' + e.message, 'bad');
    } finally { b.disabled = false; }
  });
}

/* ── 日志 ── */
PAGES.logs = async (el) => {
  const $ = (s) => el.querySelector(s);
  el.innerHTML = `<div class="card"><div class="card-head"><h3>服务日志</h3><div class="rowline">
      <select id="lg-file"></select>
      <select id="lg-lines">${[100, 200, 500, 2000].map((n) => `<option value="${n}"${n === 200 ? ' selected' : ''}>${n} 行</option>`).join('')}</select>
      <input type="text" id="lg-filter" placeholder="过滤关键字（支持多个，空格分隔）" style="width:230px">
      <label class="small muted"><input type="checkbox" id="lg-auto"> 每 5 秒自动刷新</label>
      <button class="primary" id="lg-load">刷新</button>
      <span class="chip gray" id="lg-info">-</span></div></div>
    <div class="card-body"><pre class="log" id="lg-pre">加载中…</pre></div></div>`;
  const load = async () => {
    const name = $('#lg-file').value;
    const d = await api('/logs?lines=' + $('#lg-lines').value + (name ? '&file=' + encodeURIComponent(name) : ''));
    if (!$('#lg-file').options.length) {
      $('#lg-file').innerHTML = d.files.map((f) => `<option value="${esc(f)}"${f === d.file ? ' selected' : ''}>${esc(f)}</option>`).join('');
    }
    const keys = $('#lg-filter').value.trim().split(/\s+/).filter(Boolean);
    let lines = d.lines;
    if (keys.length) lines = lines.filter((l) => keys.every((k) => l.indexOf(k) >= 0));
    $('#lg-info').textContent = `${d.file} · ${fmtBytes(d.size)} · 更新于 ${d.mtime}`;
    $('#lg-pre').textContent = lines.join('\n') || '（没有匹配的行）';
    const pre = $('#lg-pre');
    pre.scrollTop = pre.scrollHeight;
  };
  $('#lg-load').addEventListener('click', () => load().catch((e) => toast('读取日志失败：' + e.message, 'bad')));
  $('#lg-file').addEventListener('change', () => load());
  $('#lg-filter').addEventListener('input', () => load());
  $('#lg-auto').addEventListener('change', (e) => {
    if (state.logTimer) { clearInterval(state.logTimer); state.logTimer = null; }
    if (e.target.checked) state.logTimer = setInterval(() => load().catch(() => {}), 5000);
  });
  await load();
};


/* ── 自动执行（执行器）──────────────────────────────────────────── */
let ACT_CACHE = {};
PAGES.actuators = async (el) => {
  const d = await api('/actuators');
  ACT_CACHE = d;
  const on = d.actuators.filter((a) => a.mode === 'auto').length;
  const wait = d.jobs.filter((j) => j.status === 'pending' || j.status === 'sent').length;
  el.innerHTML = `
  <div class="grid stats">
    ${stat('执行器', d.actuators.length, '自动开着 ' + on + ' 路')}
    ${stat('排队中指令', wait, '等板卡取走')}
    ${stat('累计执行', d.jobs.filter((j) => j.status === 'done').length, '已完成回执')}
  </div>
  <div class="card">
    <div class="card-head"><h3>执行器</h3><span class="chip gray">共 ${d.actuators.length} 路</span></div>
    <table><thead><tr><th>设备</th><th>植物</th><th>种类</th><th>名称</th><th>模式</th>
      <th>状态</th><th>上次执行</th><th>说明</th><th></th></tr></thead>
    <tbody>${d.actuators.map((a) => `<tr>
      <td class="mono">${esc(a.sn || '-')}</td>
      <td>${a.plant_name ? esc(a.plant_name) : '<span class="muted">未绑定</span>'}</td>
      <td>${esc(a.kind)}</td><td>${esc(a.name || '-')}</td>
      <td>${a.mode === 'auto' ? '<span class="chip green">自动</span>' : '<span class="chip gray">关闭</span>'}</td>
      <td>${esc(a.state || '-')}</td><td class="num">${full(a.last_run_at)}</td>
      <td class="small muted">${esc(a.last_run_reason || '-')}</td>
      <td class="actions">
        <button class="ghost" data-act-run="${esc(a.id)}">立即执行</button>
        <button class="ghost" data-act-mode="${esc(a.id)}|${a.mode === 'auto' ? 'off' : 'auto'}">
          ${a.mode === 'auto' ? '关掉自动' : '开启自动'}</button>
      </td></tr>`).join('')
      || '<tr><td colspan="9" class="muted">还没有执行器（板卡声明能力后会出现）</td></tr>'}</tbody></table>
  </div>
  <div class="card">
    <div class="card-head"><h3>指令记录</h3><span class="chip gray">最近 ${d.jobs.length} 条</span></div>
    <table><thead><tr><th>时间</th><th>设备</th><th>种类</th><th>来源</th><th>时长</th>
      <th>状态</th><th>原因</th><th>回执</th></tr></thead>
    <tbody>${d.jobs.map((j) => `<tr>
      <td class="num">${full(j.created_at)}</td><td class="mono">${esc(j.sn || '-')}</td>
      <td>${esc(j.kind)}</td>
      <td>${j.source === 'auto' ? '自动' : (j.source === 'app' ? 'App' : '管理台')}</td>
      <td class="num">${j.duration_s}s</td>
      <td>${esc(j.status)}</td><td class="small">${esc(j.reason || '-')}</td>
      <td class="small muted">${esc(j.result || '-')}</td></tr>`).join('')
      || '<tr><td colspan="8" class="muted">还没有指令</td></tr>'}</tbody></table>
    <div class="hint" style="padding:10px 14px">指令下发后由板卡在下一次心跳（约 60 秒）取走执行并回执；
      固件实现 <span class="mono">GET /api/v1/actuators/pending</span> 与
      <span class="mono">POST /api/v1/actuators/jobs/{id}/ack</span> 即可接入。</div>
  </div>`;
  el.querySelectorAll('[data-act-run]').forEach((b) => b.addEventListener('click', async () => {
    if (!confirm('立刻下发一条执行指令？板卡约 60 秒内取走。')) return;
    try {
      await api('/actuators/' + b.dataset.actRun + '/run', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ reason: '管理台手动下发' }),
      });
      toast('指令已排队');
      go('actuators', true);
    } catch (e) { toast('下发失败：' + e.message, 'bad'); }
  }));
  el.querySelectorAll('[data-act-mode]').forEach((b) => b.addEventListener('click', async () => {
    const [id, mode] = b.dataset.actMode.split('|');
    try {
      await api('/actuators/' + id + '/mode', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode }),
      });
      toast(mode === 'auto' ? '已开启自动' : '已关闭自动');
      go('actuators', true);
    } catch (e) { toast('设置失败：' + e.message, 'bad'); }
  }));
};

/* ── 会员订阅 ─────────────────────────────────────────────────── */
PAGES.billing = async (el) => {
  const d = await api('/subscriptions');
  el.innerHTML = `
  <div class="grid stats">
    ${stat('有效订阅', d.active, '共 ' + d.subscriptions.length + ' 个账号')}
    ${stat('月经常性收入', '¥' + (d.mrr_cents / 100).toFixed(1), '按当前套餐折算')}
    ${stat('硬件押金', '¥' + (d.deposit_cents / 100).toFixed(0), '满 12 个月退')}
    ${stat('套餐门禁', d.enforce ? '已开启' : '未开启', d.enforce ? '高级功能按套餐限制' : '演示期放行')}
  </div>
  <div class="card">
    <div class="card-head"><h3>套餐</h3></div>
    <table><thead><tr><th>套餐</th><th>价格</th><th>包含</th></tr></thead>
    <tbody>${d.plans.map((p) => `<tr><td><b>${esc(p.name)}</b>${p.recommend ? ' <span class="chip green">推荐</span>' : ''}</td>
      <td class="num">¥${(p.price_cents / 100).toFixed(1)}/月</td>
      <td class="small">${p.features.map(esc).join(' · ')}</td></tr>`).join('')}
    ${d.addons.map((a) => `<tr><td><b>${esc(a.name)}</b></td>
      <td class="num">¥${(a.price_cents / 100).toFixed(1)}/月</td>
      <td class="small">${a.features.map(esc).join(' · ')}</td></tr>`).join('')}</tbody></table>
  </div>
  <div class="card">
    <div class="card-head"><h3>用户订阅</h3><span class="chip gray">共 ${d.subscriptions.length} 个</span></div>
    <table><thead><tr><th>用户</th><th>手机号</th><th>套餐</th><th>状态</th><th>到期日</th>
      <th>剩余</th><th>自动续费</th><th>押金</th></tr></thead>
    <tbody>${d.subscriptions.map((s) => `<tr>
      <td>${esc(s.nickname || '-')}</td><td class="mono">${esc(s.phone || '-')}</td>
      <td>${esc(s.plan_name || s.plan)}</td>
      <td>${s.status === 'active' && s.days_left >= 0 ? '<span class="chip green">生效中</span>' : '<span class="chip gray">' + esc(s.status) + '</span>'}</td>
      <td class="num">${esc((s.expires_at || '-').slice(0, 10))}</td>
      <td class="num">${s.days_left} 天</td>
      <td>${s.auto_renew ? '是' : '否'}</td>
      <td>${s.deposit_status === 'held' ? '¥' + (s.deposit_cents / 100).toFixed(0) + ' 已交' : '未交'}</td>
    </tr>`).join('')
      || '<tr><td colspan="8" class="muted">还没有订阅记录</td></tr>'}</tbody></table>
    <div class="hint" style="padding:10px 14px">演示期「开通订阅」不经过支付通道（App 里点一下直接生效）。
      真上线接支付时，把付款成功回调接到
      <span class="mono">billing_ops.subscribe(user_id, plan, months)</span> 即可。</div>
  </div>`;
};

/* ── 启动 ── */
(async function boot() {
  try {
    const me = await api('/me');
    if (me.logged_in) showApp(me); else showLogin();
  } catch (e) { showLogin(); }
})();
