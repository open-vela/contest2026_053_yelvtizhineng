/* 植小伴 · 手机端 App（PWA，零依赖、零构建）
 * 由服务器静态托管：/mobile/ ，接口与页面同源 /api/v1，无需配置服务器地址。
 * 页面：首页 / 数据 / 日记 / 社区 / 我的  ＋  AI 问答 / AI 体检 / 我的植物
 */
(function () {
  'use strict';

  var API = '/api/v1';
  var K_TOKEN = 'zx_token';
  var K_PLANT = 'zx_plant_id';
  var K_USER = 'zx_user';

  var TABS = [
    { k: '#/home', t: '首页', i: '🏠' },
    { k: '#/data', t: '数据', i: '📊' },
    { k: '#/diary', t: '日记', i: '📖' },
    { k: '#/community', t: '社区', i: '👥' },
    { k: '#/me', t: '我的', i: '🌿' }
  ];

  var SENSORS = [
    { k: 'temp', n: '空气温度', u: '°C', d: 1, lo: 18, hi: 28 },
    { k: 'moisture', n: '土壤水分', u: '%', d: 0, lo: 40, hi: 70 },
    { k: 'ec', n: 'EC 电导率', u: 'µS/cm', d: 0, lo: 200, hi: 800 },
    { k: 'ph', n: 'pH 酸碱度', u: '', d: 1, lo: 5.5, hi: 7 },
    { k: 'salt', n: '盐分', u: 'mg/kg', d: 0, lo: 0, hi: 200 },
    { k: 'nitrogen', n: '氮 N', u: 'mg/kg', d: 0, lo: 20, hi: 60 },
    { k: 'phosphorus', n: '磷 P', u: 'mg/kg', d: 0, lo: 15, hi: 40 },
    { k: 'potassium', n: '钾 K', u: 'mg/kg', d: 0, lo: 60, hi: 150 }
  ];

  var METRICS = {
    moisture: { n: '土壤水分', u: '%', c: '#3b82f6' },
    temp: { n: '空气温度', u: '°C', c: '#f59e0b' },
    ec: { n: 'EC 电导率', u: 'µS/cm', c: '#16a34a' }
  };

  var state = {
    token: null, user: null,
    plants: [], plantId: null, plant: null,
    detail: null, lastSync: 0,
    tasks: [], taskSync: 0,
    events: [], series: null,
    metric: 'moisture',
    chat: [], convId: null, chatBusy: false,
    diag: null, diagBusy: false,
    community: { tab: 'recommend', posts: [], topics: [], rank: null,
                 hasMore: false, q: '', loading: false },
    postId: null, post: null,
    ins: null, claimPlant: null, claimReason: null, claimDesc: '',
    claimPhoto: null, claimResult: null,
    photos: [], profile: null, bindPlant: null,
    guard: null, guardPlant: null, member: null,
    userId: null, uhome: null
  };

  var dev = { batt: null, net: '', charging: false };
  /* ────────── 小工具 ────────── */
  function $(s, r) { return (r || document).querySelector(s); }

  function esc(v) {
    return String(v == null ? '' : v).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }

  function pad2(x) { return (x < 10 ? '0' : '') + x; }

  function tsDate(ts) {
    var m = String(ts || '').match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})/);
    if (!m) return null;
    return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  }

  function hm(ts) {
    var d = tsDate(ts);
    return d ? pad2(d.getHours()) + ':' + pad2(d.getMinutes()) : '';
  }

  function dayLabel(ts) {
    var d = tsDate(ts);
    if (!d) return '';
    var now = new Date();
    var a = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    var b = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    var diff = Math.round((a - b) / 86400000);
    if (diff === 0) return '今天';
    if (diff === 1) return '昨天';
    if (diff === 2) return '前天';
    return (d.getMonth() + 1) + '月' + d.getDate() + '日';
  }

  function shortDate(s) {
    var p = String(s || '').split('-');
    return p.length === 3 ? (+p[1]) + '/' + (+p[2]) : String(s || '');
  }

  function num(v) { var x = parseFloat(v); return isFinite(x) ? x : null; }

  function fix(v, d) {
    var x = num(v);
    return x == null ? '--' : x.toFixed(d == null ? 1 : d);
  }

  function fmtTick(v) {
    if (v >= 100) return v.toFixed(0);
    if (v >= 10) return v.toFixed(1);
    return v.toFixed(2);
  }

  function toast(msg) {
    var t = $('#toast');
    t.textContent = msg;
    t.hidden = false;
    clearTimeout(toast._t);
    toast._t = setTimeout(function () { t.hidden = true; }, 2400);
  }

  function loading(msg) {
    return '<div class="loading"><div class="spinner"></div>' + esc(msg || '正在加载…') + '</div>';
  }

  function emptyBox(e1, e2, e3) {
    return '<div class="empty"><div class="e1">' + e1 + '</div><div class="e2">' + esc(e2) + '</div>'
      + (e3 ? '<div class="e3">' + esc(e3) + '</div>' : '') + '</div>';
  }

  function pageHead(title, back) {
    return '<div class="page-head">'
      + (back ? '<button class="back" data-go="' + esc(back) + '">‹</button>' : '')
      + '<h1>' + esc(title) + '</h1></div>';
  }

  function normRoute(h) {
    h = h || '#/home';
    if (!/^#\//.test(h)) h = '#/home';
    return h;
  }

  function isTab(route) {
    for (var i = 0; i < TABS.length; i++) { if (TABS[i].k === route) return true; }
    return false;
  }
  /* ────────── 与服务器通信 ────────── */
  function api(path, opt) {
    opt = opt || {};
    var headers = {};
    if (opt.auth !== false && state.token) headers['Authorization'] = 'Bearer ' + state.token;
    var body;
    if ('json' in opt) {
      headers['Content-Type'] = 'application/json';
      body = JSON.stringify(opt.json);
    } else if ('body' in opt) {
      body = opt.body;
      if (opt.type) headers['Content-Type'] = opt.type;
    }
    return fetch(API + path, { method: opt.method || 'GET', headers: headers, body: body })
      .then(function (res) {
        if (res.status === 401 && opt.auth !== false) {
          // 多用户之后不能再"悄悄用演示账号重登"了 —— 那是别人的账号。
          clearLogin();
          if (normRoute(location.hash) !== '#/login') {
            go('#/login');
            setTimeout(function () { toast('登录已失效，请重新登录'); }, 60);
          }
          return Promise.reject(new Error('登录已失效，请重新登录'));
        }
        if (!res.ok) {
          return res.text().then(function (txt) {
            var msg = 'HTTP ' + res.status;
            try {
              var j = JSON.parse(txt);
              if (j && typeof j.detail === 'string') msg = j.detail;
              else if (j && j.detail) msg = '请求参数有问题';
            } catch (e) { }
            throw new Error(msg);
          });
        }
        var ct = res.headers.get('content-type') || '';
        return ct.indexOf('json') >= 0 ? res.json() : res.text();
      });
  }

  function saveLogin(j) {
    state.token = j.token;
    state.user = j.user;
    state.profile = null;
    try { localStorage.setItem(K_TOKEN, j.token); } catch (e) { }
    try { localStorage.setItem(K_USER, JSON.stringify(j.user || {})); } catch (e) { }
  }

  function clearLogin() {
    state.token = null;
    state.user = null;
    state.profile = null;
    try {
      localStorage.removeItem(K_TOKEN);
      localStorage.removeItem(K_USER);
      localStorage.removeItem(K_PLANT);
    } catch (e) { }
  }

  function logout() {
    api('/auth/logout', { method: 'POST' }).then(function () { }, function () { });
    clearLogin();
    location.hash = '#/login';
    setTimeout(function () { location.reload(); }, 30);
  }

  function loadPlants() {
    return api('/plants').then(function (j) {
      state.plants = j.plants || [];
      var saved = null;
      try { saved = localStorage.getItem(K_PLANT); } catch (e) { }
      var pick = null;
      for (var i = 0; i < state.plants.length; i++) {
        if (state.plants[i].id === saved) pick = state.plants[i];
      }
      state.plant = pick || state.plants[0] || null;
      state.plantId = state.plant ? state.plant.id : null;
      if (state.plantId) { try { localStorage.setItem(K_PLANT, state.plantId); } catch (e) { } }
      return state.plants;
    });
  }

  function ensureDetail(force) {
    if (!state.plantId) return Promise.resolve();
    if (!force && state.detail && Date.now() - state.lastSync < 15000) return Promise.resolve();
    return api('/plants/' + state.plantId).then(function (j) {
      state.detail = j;
      state.lastSync = Date.now();
    });
  }

  function ensureTasks(force) {
    if (!state.plantId) return Promise.resolve();
    if (!force && state.tasks.length && Date.now() - state.taskSync < 15000) return Promise.resolve();
    return api('/tasks/today?plant_id=' + encodeURIComponent(state.plantId)).then(function (j) {
      state.tasks = j.tasks || [];
      state.taskSync = Date.now();
    });
  }

  function ensureSeries(force) {
    if (!state.plantId) return Promise.resolve();
    if (!force && state.series) return Promise.resolve();
    return api('/plants/' + state.plantId + '/telemetry?days=7').then(function (j) {
      state.series = j.series || null;
    });
  }

  function ensureEvents(force) {
    if (!state.plantId) return Promise.resolve();
    if (!force && state.events.length) return Promise.resolve();
    return api('/events?plant_id=' + encodeURIComponent(state.plantId) + '&limit=30')
      .then(function (j) { state.events = j.events || []; });
  }

  /* ────────── 顶部状态：时间 / 电量 / 网络 ────────── */
  function netLabel() {
    var c = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
    if (c && c.type === 'wifi') return 'Wi-Fi';
    if (c && c.effectiveType) return String(c.effectiveType).toUpperCase();
    return navigator.onLine === false ? '离线' : '在线';
  }

  function statusLine() {
    var d = new Date();
    var wk = '日一二三四五六'.charAt(d.getDay());
    var s = pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' 周' + wk + ' '
      + pad2(d.getHours()) + ':' + pad2(d.getMinutes());
    if (dev.batt != null) s += ' · ' + (dev.charging ? '⚡' : '🔋') + dev.batt + '%';
    if (dev.net) s += ' · 📶' + dev.net;
    return s;
  }

  function paintStatus() {
    var e = $('#status-line');
    if (e) e.textContent = statusLine();
  }

  function initDeviceInfo() {
    dev.net = netLabel();
    window.addEventListener('online', function () { dev.net = netLabel(); paintStatus(); });
    window.addEventListener('offline', function () { dev.net = netLabel(); paintStatus(); });
    var c = navigator.connection;
    if (c && c.addEventListener) {
      c.addEventListener('change', function () { dev.net = netLabel(); paintStatus(); });
    }
    if (navigator.getBattery) {
      navigator.getBattery().then(function (b) {
        function upd() {
          dev.batt = Math.round(b.level * 100);
          dev.charging = !!b.charging;
          paintStatus();
        }
        upd();
        b.addEventListener('levelchange', upd);
        b.addEventListener('chargingchange', upd);
      }, function () { });
    }
    setInterval(paintStatus, 20000);
  }
  /* ────────── 路由与渲染 ────────── */
  var renderSeq = 0;

  function renderTabs(route) {
    var h = '';
    for (var i = 0; i < TABS.length; i++) {
      var t = TABS[i];
      h += '<div class="tab' + (t.k === route ? ' on' : '') + '" data-go="' + t.k + '">'
        + '<span class="ic">' + t.i + '</span><span>' + t.t + '</span></div>';
    }
    $('#tabbar').innerHTML = h;
  }

  function fetchFor(route) {
    if (route === '#/login') return Promise.resolve();
    if (route === '#/community') return loadFeed(false);
    if (route === '#/post') return loadPost();
    if (route === '#/user') return loadUser();
    if (route === '#/compose') return loadPhotos();
    if (route === '#/insurance' || route === '#/claim') return loadInsurance();
    if (route === '#/profile') return loadProfile();
    if (route === '#/bind') return loadPlants();
    if (route === '#/guard') return loadGuard();
    if (route === '#/member') return loadMember();
    if (route === '#/me') {
      // 「我的」要看会员状态和自动守护摘要，顺手一起拉
      return Promise.all([loadMember(), loadGuard(),
        state.plantId ? Promise.resolve() : loadPlants()]);
    }
    if (!state.plantId) return loadPlants();
    if (route === '#/home') return Promise.all([ensureDetail(false), ensureTasks(false), ensureEvents(false)]);
    if (route === '#/data') return Promise.all([ensureDetail(false), ensureSeries(false)]);
    if (route === '#/diary') return Promise.all([ensureDetail(false), ensureEvents(false)]);
    if (route === '#/community') return Promise.resolve();
    return ensureDetail(false);
  }

  function htmlFor(route) {
    if (route === '#/data') return htmlData();
    if (route === '#/diary') return htmlDiary();
    if (route === '#/community') return htmlCommunity();
    if (route === '#/post') return htmlPostView();
    if (route === '#/user') return htmlUserHome();
    if (route === '#/compose') return htmlCompose();
    if (route === '#/insurance') return htmlInsurance();
    if (route === '#/claim') return htmlClaim();
    if (route === '#/profile') return htmlProfile();
    if (route === '#/bind') return htmlBind();
    if (route === '#/guard') return htmlGuard();
    if (route === '#/member') return htmlMember();
    if (route === '#/login') return htmlLogin();
    if (route === '#/me') return htmlMe();
    if (route === '#/chat') return htmlChat();
    if (route === '#/diag') return htmlDiag();
    if (route === '#/plant') return htmlPlant();
    return htmlHome();
  }

  function render() {
    var route = normRoute(location.hash);
    if (route !== '#/chat') cancelVoice();
    /* 离开「聊天 / 体检」页就停掉朗读：不然切页了声音还在念，很出戏 */
    if (route !== '#/chat' && route !== '#/diag') speakStop();
    var seq = ++renderSeq;
    $('#app').classList.toggle('login-mode', route === '#/login');
    $('#app').classList.toggle('has-tabbar', isTab(route));
    renderTabs(route);
    var view = $('#view');
    view.innerHTML = loading();
    view.scrollTop = 0;
    Promise.resolve()
      .then(function () { return fetchFor(route); })
      .then(function () {
        if (seq !== renderSeq) return;
        view.innerHTML = htmlFor(route);
        bindFor(route, view);
      }, function (e) {
        if (seq !== renderSeq) return;
        view.innerHTML = emptyBox('😵', '加载失败', e && e.message ? e.message : '')
          + '<div style="text-align:center;padding-bottom:20px">'
          + '<button class="chip g" data-act="reload">点这里重试</button></div>';
      });
  }

  function eachPost(id, fn) {
    var lists = [state.community.posts, (state.post ? [state.post] : []),
                 (state.uhome ? state.uhome.posts : [])];
    for (var i = 0; i < lists.length; i++) {
      for (var j = 0; j < lists[i].length; j++) {
        if (lists[i][j].id === id) fn(lists[i][j]);
      }
    }
  }

  function rerender() {
    var route = normRoute(location.hash);
    var v = $('#view');
    if (!v) return;
    v.innerHTML = htmlFor(route);
    bindFor(route, v);
  }

  function go(route) {
    if (normRoute(location.hash) === route) render();
    else location.hash = route;
  }

  /* ────────── 首页 ────────── */
  function hstat(val, lab) {
    return '<div class="h-stat"><div class="h-val">' + esc(val) + '</div>'
      + '<div class="h-lab">' + esc(lab) + '</div></div>';
  }

  function taskEmoji(s) {
    s = String(s || '');
    if (s.indexOf('水') >= 0) return '💧';
    if (s.indexOf('晒') >= 0 || s.indexOf('光') >= 0) return '☀️';
    if (s.indexOf('拍') >= 0) return '📷';
    if (s.indexOf('AI') >= 0 || s.indexOf('问') >= 0) return '💬';
    return '🌱';
  }

  function tasksHTML() {
    var ts = state.tasks || [];
    if (!ts.length) return '<div class="card muted">今天还没有任务</div>';
    return ts.map(function (t) {
      var done = t.status === 'done';
      return '<div class="task' + (done ? ' done' : '') + '">'
        + '<div class="ti" style="background:' + (done ? 'var(--green-100)' : 'var(--amber-100)') + '">'
        + (done ? '✅' : taskEmoji(t.content)) + '</div>'
        + '<div class="tt"><div class="tn">' + esc(t.content) + '</div>'
        + '<div class="td">' + (done ? '已完成' : '完成得 +' + (t.growth_delta || 5) + ' 成长值') + '</div></div>'
        + (done ? '<span class="tb ok">已完成</span>'
          : '<button class="tb" data-task="' + esc(t.id) + '">完成</button>')
        + '</div>';
    }).join('');
  }

  function tlHTML(list, withPhoto) {
    if (!list || !list.length) return '<div class="card muted">还没有记录，拍一张照片开始吧</div>';
    return '<div class="timeline">' + list.map(function (e) {
      var ico = e.type === 'diagnose' ? '🔍' : (e.type === 'water' ? '💧' : '🌱');
      return '<div class="tl"><div class="dot" style="background:var(--amber-100)">' + ico + '</div>'
        + '<div class="body">'
        + '<div class="t1"><span>' + esc(dayLabel(e.event_ts)) + ' ' + esc(hm(e.event_ts)) + '</span>'
        + '<span>' + esc(e.source === 'device' ? '板卡' : 'App') + '</span></div>'
        + '<div class="t2">' + esc(e.title) + '</div>'
        + '<div class="t3">' + esc(e.summary) + '</div>'
        + (withPhoto && e.media_id ? '<img class="photo" loading="lazy" alt="植物照片" src="'
          + API + '/media/' + esc(e.media_id) + '/file">' : '')
        + '</div></div>';
    }).join('') + '</div>';
  }

  function badgeChips(badges) {
    var cls = ['g', 'a', 'b', 'v'];
    return (badges || []).map(function (b, i) {
      return '<span class="chip ' + cls[i % 4] + '">🏅' + esc(b.name) + '</span>';
    }).join(' ');
  }
  function htmlHome() {
    var d = state.detail || {};
    var p = d.plant || state.plant || {};
    var t = d.recent_telemetry || {};
    var g = d.growth || {};
    var score = num(p.health_score);
    if (score == null) score = 0;
    var open = 0, done = 0;
    (state.tasks || []).forEach(function (x) { if (x.status === 'done') done++; else open++; });
    var badges = g.badges || [];
    var recent = (state.events || []).slice(0, 2);
    return ''
      + '<div class="top-greet">'
      + '<div class="ava">' + esc(p.mood || '🌿') + '</div>'
      + '<div class="who"><div class="hi">你好，' + esc((state.user && state.user.nickname) || '园丁') + '</div>'
      + '<div class="sub" id="status-line">' + esc(statusLine()) + '</div></div>'
      + '<div class="acts">'
      + '<button class="icon-btn" data-go="#/chat">💬</button>'
      + '<button class="icon-btn" data-go="#/diag">📷</button>'
      + '</div></div>'
      + '<div class="hero" data-go="#/data">'
      + '<div class="h-top"><div class="h-ava">🪴</div>'
      + '<div class="h-info"><div class="h-name">' + esc(p.name || '还没有植物') + '</div>'
      + '<div class="h-type">' + esc(p.species || '')
      + (p.location ? ' · ' + esc(p.location) : '') + '</div>'
      + '<div class="h-health"><div class="h-bar"><div class="h-fill" style="width:'
      + Math.max(0, Math.min(100, score)) + '%"></div></div>'
      + '<span class="h-score">' + score + ' 分 · ' + esc(p.health_level || '') + '</span></div></div></div>'
      + '<div class="h-stats">'
      + hstat(fix(t.moisture, 0) + '%', '土壤水分')
      + hstat(fix(t.temp, 1) + '°', '空气温度')
      + hstat(fix(t.ec, 0), 'EC µS/cm')
      + '</div>'
      + '<div class="h-foot"><span>数据更新于 ' + esc(hm(t.ts) || '--:--')
      + '</span><span>看全部数据 <b>›</b></span></div>'
      + '</div>'
      + '<div class="ai-quick">'
      + '<div class="q q1" data-go="#/chat"><div class="qi">💬</div>'
      + '<div class="ql">问 AI</div><div class="qs">什么都能聊</div></div>'
      + '<div class="q q2" data-go="#/diag"><div class="qi">📷</div>'
      + '<div class="ql">拍一拍</div><div class="qs">AI 体检</div></div>'
      + '<div class="q q3" data-go="#/data"><div class="qi">📊</div>'
      + '<div class="ql">看数据</div><div class="qs">八项指标</div></div>'
      + '</div>'
      + '<div class="sec"><h3>今日任务</h3><span class="more">' + done + '/' + (done + open) + ' 完成</span></div>'
      + tasksHTML()
      + '<div class="sec"><h3>成长日记</h3><span class="more" data-go="#/diary">全部 ›</span></div>'
      + tlHTML(recent, false)
      + '<div class="sec"><h3>我的徽章</h3><span class="more">' + badges.length + ' 枚</span></div>'
      + '<div class="card"><div style="display:flex;gap:7px;flex-wrap:wrap">'
      + (badges.length ? badgeChips(badges) : '<span class="muted">还没有徽章</span>')
      + '</div></div>';
  }

  /* ────────── 数据页 ────────── */
  function senState(s, v) {
    if (v == null) return '';
    if (v < s.lo) return 'low';
    if (v > s.hi) return 'warn';
    return '';
  }

  function htmlData() {
    var d = state.detail || {};
    var t = d.recent_telemetry || {};
    var h = pageHead('实时数据', '#/home');
    h += '<div class="card" style="padding:11px 14px"><div style="display:flex;justify-content:space-between;align-items:center">'
      + '<span style="font-size:12px;color:var(--gray)">采集时间 ' + esc(t.ts || '暂无') + '</span>'
      + '<button class="chip g" data-act="reload">刷新</button></div></div>';
    h += '<div class="sensors">';
    SENSORS.forEach(function (s) {
      var v = num(t[s.k]);
      var st = senState(s, v);
      var tail = v == null ? ' · 无数据' : (st === 'low' ? ' · 偏低' : (st === 'warn' ? ' · 偏高' : ' · 正常'));
      h += '<div class="sen ' + st + '">'
        + '<div class="sv">' + fix(v, s.d) + (s.u ? '<small>' + esc(s.u) + '</small>' : '') + '</div>'
        + '<div class="sn">' + esc(s.n) + '</div>'
        + '<div class="sh">参考 ' + s.lo + '~' + s.hi + (s.u ? ' ' + esc(s.u) : '') + tail + '</div>'
        + '</div>';
    });
    h += '</div>';
    h += '<div class="sec"><h3>近 7 天趋势</h3><span class="more">按日平均</span></div>';
    h += '<div style="display:flex;gap:7px;padding:0 16px 8px;flex-wrap:wrap">';
    Object.keys(METRICS).forEach(function (k) {
      h += '<button class="chip ' + (state.metric === k ? 'g' : 'demo') + '" data-metric="' + k + '">'
        + esc(METRICS[k].n) + '</button>';
    });
    h += '</div>';
    h += chartCard();
    h += '<div class="card" style="font-size:11.5px;color:var(--gray);line-height:1.65">'
      + '说明：EC（电导率）自 9 月 9 日起改为真实标定口径（µS/cm），更早的历史值是旧口径，'
      + '两者量纲不同，所以 EC 曲线只画真实口径的那几天。光照一项当前板卡未上报，暂不展示。'
      + '</div>';
    return h;
  }

  function chartCard() {
    var s = state.series;
    if (!s || !s.dates || !s.dates.length) return '<div class="card muted">还没有历史数据</div>';
    var m = METRICS[state.metric];
    var dates = s.dates.slice();
    var vals = (s[state.metric] || []).slice();
    var note = '';
    if (state.metric === 'ec') {
      var d2 = [], v2 = [];
      for (var i = 0; i < vals.length; i++) {
        var x = num(vals[i]);
        if (x != null && x >= 50) { d2.push(dates[i]); v2.push(x); }
      }
      if (d2.length < 2) return '<div class="card muted">EC 的真实口径数据还不足 2 天，暂时画不出趋势</div>';
      note = '（仅真实口径数据）';
      dates = d2;
      vals = v2;
    }
    var svg = lineChart(dates, vals, m.c);
    if (!svg) return '<div class="card muted">这段时间没有数据</div>';
    return '<div class="chart-wrap">'
      + '<div class="chart-legend"><span class="lg"><span class="dot" style="background:' + m.c + '"></span>'
      + esc(m.n) + (m.u ? '（' + esc(m.u) + '）' : '') + esc(note) + '</span></div>'
      + svg + '</div>';
  }
  function lineChart(dates, vals, color) {
    var W = 320, H = 132, pl = 34, pr = 10, pt = 12, pb = 24;
    var pts = [], nums = [];
    for (var i = 0; i < vals.length; i++) {
      var v = num(vals[i]);
      if (v == null) { pts.push(null); } else { pts.push(v); nums.push(v); }
    }
    if (!nums.length) return '';
    var mn = Math.min.apply(null, nums), mx = Math.max.apply(null, nums);
    if (mx - mn < 1e-6) { mn = mn - 1; mx = mx + 1; }
    var pad = (mx - mn) * 0.18;
    mn -= pad;
    mx += pad;
    var iw = W - pl - pr, ih = H - pt - pb;
    function X(i) { return pl + (dates.length < 2 ? iw / 2 : iw * i / (dates.length - 1)); }
    function Y(v) { return pt + ih * (1 - (v - mn) / (mx - mn)); }
    var segs = [], cur = [];
    for (var j = 0; j < pts.length; j++) {
      if (pts[j] == null) { if (cur.length) segs.push(cur); cur = []; } else { cur.push(j); }
    }
    if (cur.length) segs.push(cur);
    var dpath = segs.map(function (sg) {
      return 'M' + sg.map(function (k) {
        return X(k).toFixed(1) + ',' + Y(pts[k]).toFixed(1);
      }).join('L');
    }).join(' ');
    var dots = '';
    for (var k2 = 0; k2 < pts.length; k2++) {
      if (pts[k2] == null) continue;
      dots += '<circle cx="' + X(k2).toFixed(1) + '" cy="' + Y(pts[k2]).toFixed(1)
        + '" r="2.6" fill="#fff" stroke="' + color + '" stroke-width="1.8"/>';
    }
    var grid = '', ylab = '';
    for (var g = 0; g <= 2; g++) {
      var gy = pt + ih * g / 2;
      grid += '<line x1="' + pl + '" y1="' + gy.toFixed(1) + '" x2="' + (W - pr)
        + '" y2="' + gy.toFixed(1) + '" stroke="#f1f5f9" stroke-width="1"/>';
      var vv = mx - (mx - mn) * g / 2;
      ylab += '<text x="' + (pl - 5) + '" y="' + (gy + 3.5).toFixed(1)
        + '" font-size="9" fill="#9ca3af" text-anchor="end">' + fmtTick(vv) + '</text>';
    }
    var xlab = '';
    if (dates.length) {
      xlab += '<text x="' + pl + '" y="' + (H - 7) + '" font-size="9" fill="#9ca3af">'
        + esc(shortDate(dates[0])) + '</text>';
      if (dates.length > 1) {
        xlab += '<text x="' + (W - pr) + '" y="' + (H - 7)
          + '" font-size="9" fill="#9ca3af" text-anchor="end">'
          + esc(shortDate(dates[dates.length - 1])) + '</text>';
      }
    }
    return '<svg viewBox="0 0 ' + W + ' ' + H + '">'
      + grid + ylab + xlab
      + '<path d="' + dpath + '" fill="none" stroke="' + color
      + '" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round"/>'
      + dots + '</svg>';
  }

  /* ────────── 日记页 ────────── */
  function htmlDiary() {
    var d = state.detail || {};
    var g = d.growth || {};
    var evs = state.events || [];
    var h = pageHead('成长日记', '#/home');
    h += '<div class="hero" style="padding:14px">'
      + '<div style="color:#fff;font-size:16px;font-weight:900">' + esc((d.plant && d.plant.name) || '小绿绿')
      + ' 的成长记录</div>'
      + '<div class="h-stats" style="margin-top:11px">'
      + hstat(String(g.total_growth == null ? 0 : g.total_growth), '成长值')
      + hstat(String((g.badges || []).length), '徽章')
      + hstat(String(evs.length), '日记条目')
      + '</div></div>';
    h += '<div class="sec"><h3>获得的徽章</h3><span class="more">' + (g.badges || []).length + ' 枚</span></div>';
    h += '<div class="card"><div style="display:flex;gap:7px;flex-wrap:wrap">'
      + ((g.badges || []).length ? badgeChips(g.badges) : '<span class="muted">还没有徽章</span>')
      + '</div></div>';
    h += '<div class="sec"><h3>时间线</h3><span class="more">共 ' + evs.length + ' 条</span></div>';
    h += tlHTML(evs.slice(0, 20), true);
    return h;
  }

  /* ────────── 社区（示例占位） ────────── */
  var CTABS = [['recommend', '推荐'], ['follow', '关注'], ['photo', '晒图'],
               ['help', '求助'], ['rank', '排行榜']];

  function loadFeed(force) {
    var c = state.community;
    if (!force && c.posts.length) return Promise.resolve();
    c.loading = true;
    if (c.tab === 'rank') {
      return api('/community/leaderboard').then(function (j) {
        c.rank = j.leaderboard || [];
        c.loading = false;
      });
    }
    var url = '/community/feed?tab=' + c.tab;
    if (c.q) url += '&q=' + encodeURIComponent(c.q);
    return api(url).then(function (j) {
      c.posts = j.posts || [];
      c.hasMore = !!j.has_more;
      c.loading = false;
    });
  }

  function loadMorePosts() {
    var c = state.community;
    var url = '/community/feed?tab=' + c.tab + '&offset=' + c.posts.length;
    if (c.q) url += '&q=' + encodeURIComponent(c.q);
    return api(url).then(function (j) {
      c.posts = c.posts.concat(j.posts || []);
      c.hasMore = !!j.has_more;
      rerender();
    });
  }

  function rankHTML() {
    var r = state.community.rank;
    if (r == null) return loading();
    if (!r.length) return emptyBox('🏆', '排行榜还空着', '发帖、攒点赞就能上榜');
    return '<div class="card rank">' + r.map(function (u, i) {
      var medal = i === 0 ? '🥇' : (i === 1 ? '🥈' : (i === 2 ? '🥉' : ('#' + u.rank)));
      return '<div class="rank-row' + (u.is_me ? ' me' : '') + '"' + (u.is_me ? '' : ' data-user="' + esc(u.id) + '"') + '>'
        + '<span class="rk">' + medal + '</span>'
        + '<span class="ra">' + esc(u.avatar) + '</span>'
        + '<span class="rn">' + esc(u.nickname) + (u.is_me ? ' <small>我</small>' : '')
        + (u.city ? '<small>' + esc(u.city) + '</small>' : '') + '</span>'
        + '<span class="rs"><b>' + u.likes + '</b> 赞 · <b>' + u.posts + '</b> 帖</span></div>';
    }).join('') + '</div>';
  }

  function topicHTML() {
    var t = (state.community.topics || [])[0];
    if (!t || state.community.q) return '';
    return '<div class="hot"><div class="hi">' + t.icon + '</div><div class="ht">'
      + '<div class="h1">' + esc(t.title) + '</div><div class="h2">' + esc(t.sub) + '</div></div>'
      + '<span class="chip a" data-topic="' + esc(t.tag) + '">参加 →</span></div>';
  }

  function postHTML(p) {
    var h = '<div class="post" data-open="' + p.id + '">'
      + '<div class="ph"><div class="pa">' + esc(p.author.avatar) + '</div>'
      + '<div class="pu">' + esc(p.author.nickname) + '<small>' + esc(p.ago)
      + (p.author.city ? ' · ' + esc(p.author.city) : '') + '</small></div>'
      + (p.is_mine ? '</div>' : '<span class="pf' + (p.followed ? ' done' : '')
        + '" data-follow="' + esc(p.author.id) + '">' + (p.followed ? '已关注' : '+ 关注') + '</span></div>')
      + '<div class="pt">'
      + (p.topic ? '<span class="pt-topic">' + esc(p.topic) + '</span>' : '')
      + esc(p.text) + '</div>';
    if (p.images && p.images.length) {
      h += '<div class="pi">' + p.images.map(function (m) {
        return '<img src="' + API + '/media/' + esc(m.id) + '/file" loading="lazy" alt="植物照片">';
      }).join('') + '</div>';
    }
    h += '<div class="pfo">'
      + '<span class="pact' + (p.liked ? ' on' : '') + '" data-like="' + p.id + '">❤️ ' + p.like_count + '</span>'
      + '<span class="pact" data-open2="' + p.id + '">💬 ' + p.comment_count + '</span>'
      + '<span class="pact' + (p.faved ? ' on' : '') + '" data-fav="' + p.id + '">⭐ 收藏</span>'
      + '<span class="pact">' + esc(p.category_name) + '</span>'
      + '</div></div>';
    return h;
  }

  function htmlCommunity() {
    var c = state.community;
    var h = '<div class="page-head"><h1>🌱 植小伴社区</h1>'
      + '<button class="back post-btn" id="compose-btn">✏️</button></div>';
    h += '<div class="comm-tabs">' + CTABS.map(function (t) {
      return '<div class="comm-tab' + (c.tab === t[0] ? ' on' : '') + '" data-ctab="' + t[0] + '">' + t[1] + '</div>';
    }).join('') + '</div>';
    h += '<div class="comm-search"><span>🔍</span><input id="cm-search" placeholder="搜索植物、养护技巧…"'
      + ' value="' + esc(c.q) + '" autocomplete="off"></div>';
    if (c.tab === 'rank') return h + rankHTML() + '<div style="height:74px"></div>';
    if (c.loading && !c.posts.length) return h + loading();
    h += topicHTML();
    if (c.q) h += '<div class="search-tip">搜索「' + esc(c.q) + '」的结果：' + c.posts.length + ' 条</div>';
    h += c.posts.length
      ? c.posts.map(postHTML).join('')
      : emptyBox(c.q ? '🔍' : '🌿', c.q ? '没搜到相关内容' : (c.tab === 'follow' ? '还没关注谁' : '还没有内容'),
                 c.q ? '换个词试试' : (c.tab === 'follow' ? '去「推荐」里关注几个邻居' : '点右上角 ✏️ 发第一条'));
    if (c.hasMore) h += '<div style="text-align:center;padding:6px 0 14px">'
      + '<button class="chip g" data-act="more-posts">加载更多</button></div>';
    h += '<div style="height:74px"></div>';
    return h;
  }

  function loadPost() {
    if (!state.postId) return Promise.resolve();
    return api('/community/posts/' + state.postId).then(function (j) {
      state.post = j.post;
    });
  }

  function htmlPostView() {
    var p = state.post;
    if (!p) return loading('正在打开帖子…');
    var h = pageHead('帖子详情', '#/community');
    h += postHTML(p);
    var cs = p.comments || [];
    h += '<div class="card"><div class="ct">评论 ' + cs.length + '</div>'
      + (cs.length ? cs.map(function (c) {
        return '<div class="cmt"><div class="ca">' + esc(c.author.avatar) + '</div>'
          + '<div class="cb"><div class="cn">' + esc(c.author.nickname)
          + '<small>' + esc(c.ago) + (c.author.city ? ' · ' + esc(c.author.city) : '') + '</small></div>'
          + '<div class="cc">' + esc(c.text) + '</div></div></div>';
      }).join('') : '<div class="muted" style="font-size:12.5px">还没有人说话，来占个沙发</div>')
      + '</div><div style="height:96px"></div>'
      + '<div class="chat-bar"><input id="cm-in" placeholder="友善地聊两句…" autocomplete="off">'
      + '<button id="cm-send">➤</button></div>';
    return h;
  }

  function sendComment() {
    var el = $('#cm-in');
    var t = el ? String(el.value || '').trim() : '';
    if (!t) return;
    api('/community/posts/' + state.postId + '/comments', {
      method: 'POST', json: { text: t }
    }).then(function () {
      if (el) el.value = '';
      return loadPost();
    }).then(function () {
      rerender();
      toast('已评论');
    }, function (e) { toast('评论失败：' + (e.message || '')); });
  }

  /* ────────── 发帖 ────────── */
  var COMPOSE = { text: '', cat: 'share', topic: '', media: [] };

  function loadPhotos() {
    return api('/media?limit=12').then(function (j) {
      state.photos = j.media || [];
    });
  }

  function htmlCompose() {
    var h = pageHead('发帖子', '#/community');
    h += '<div class="card">';
    if (COMPOSE.topic) {
      h += '<div class="cf-topic">' + esc(COMPOSE.topic)
        + '<span data-act="cf-topic-x">✕</span></div>';
    }
    h += '<textarea id="cf-text" class="cf-text" maxlength="500" '
      + 'placeholder="分享你的养植心得，或者向大家求助…">' + esc(COMPOSE.text) + '</textarea>';
    h += '<div class="pick-row">' + [['share', '📷 晒图'], ['help', '🆘 求助'], ['daily', '🌿 日常']]
      .map(function (c) {
        return '<button class="chip ' + (COMPOSE.cat === c[0] ? 'g' : '') + '" data-ccat="'
          + c[0] + '">' + c[1] + '</button>';
      }).join('') + '</div>';
    h += '<div class="ct" style="margin-top:14px">配图（最多 3 张）</div>'
      + '<div class="cf-photos">' + (state.photos.length ? state.photos.map(function (m) {
        var on = COMPOSE.media.indexOf(m.id) >= 0;
        return '<div class="cf-ph' + (on ? ' on' : '') + '" data-photo="' + esc(m.id) + '">'
          + '<img src="' + API + '/media/' + esc(m.id) + '/file" loading="lazy" alt="">'
          + (on ? '<span class="ck">✓</span>' : '') + '</div>';
      }).join('') : '<div class="muted" style="font-size:12px">还没有照片，先去「AI 体检」给小绿拍一张</div>')
      + '</div>';
    h += '<div class="muted" style="font-size:11.5px;margin-top:8px">帖子只能挂自己的照片，别人看不到你的相册</div>';
    h += '</div>';
    h += '<div style="padding:0 14px 24px"><button class="btn" id="cf-send" style="width:100%">发布</button></div>';
    return h;
  }

  function sendPost() {
    var t = String((($('#cf-text') || {}).value) || '').trim();
    COMPOSE.text = t;
    if (!t) { toast('写点什么再发吧'); return; }
    api('/community/posts', {
      method: 'POST',
      json: { text: t, category: COMPOSE.cat, topic: COMPOSE.topic, media_ids: COMPOSE.media }
    }).then(function () {
      COMPOSE.text = ''; COMPOSE.topic = ''; COMPOSE.media = [];
      state.community.posts = []; state.community.q = '';
      toast('发布成功');
      go('#/community');
    }, function (e) { toast('发布失败：' + (e.message || '')); });
  }

  /* ────────── 我的 ────────── */
  /* ────────── 自动守护（自动执行层） ──────────
     这一层是把"建议"变成"动作"：土壤水分低于阈值，服务器自动排一条浇水指令，
     板卡取走执行、回执，App 这边看到的就是"守护中"。               */
  function loadGuard(plantId) {
    if (plantId) state.guardPlant = plantId;
    var q = state.guardPlant ? ('?plant_id=' + encodeURIComponent(state.guardPlant)) : '';
    return api('/actuators' + q).then(function (j) { state.guard = j; });
  }

  var GUARD_ST = { idle: ['待命', 'ok'], running: ['执行中', 'warn'],
                   error: ['需要看看', 'bad'], pending: ['等板卡执行', 'warn'] };
  var GUARD_ICO = { water: '💧', light: '💡', fan: '🌀', feeder: '🍚', heat: '🔥' };

  function guardAgo(ts) {
    if (!ts) return '还没执行过';
    var d = new Date(ts.replace(/-/g, '/'));
    var mins = Math.floor((Date.now() - d.getTime()) / 60000);
    if (mins < 1) return '刚刚';
    if (mins < 60) return mins + ' 分钟前';
    if (mins < 60 * 24) return Math.floor(mins / 60) + ' 小时前';
    return Math.floor(mins / (60 * 24)) + ' 天前';
  }

  function htmlGuard() {
    var g = state.guard;
    if (!g) return loading('正在读取执行器…');
    var acts = g.actuators || [];
    var on = acts.filter(function (a) { return a.mode === 'auto'; }).length;
    var h = pageHead('自动守护', '#/me');

    var allOn = acts.length > 0 && on === acts.length;
    var heroT = !g.device_bound ? '未接板卡'
      : (on === 0 ? '⚪ 自动已关闭' : (allOn ? '🟢 守护中' : '🟡 部分开启'));
    var heroD = !g.device_bound ? '这盆还没绑定板卡，指令发出去没人执行'
      : (on === 0 ? '自动执行都关掉了，只能手动来一下'
        : (allOn ? '土壤水分低于阈值时，服务器自动安排浇水，不用你惦记'
          : '还有 ' + (acts.length - on) + ' 路是关着的，需要它出手时不会动'));
    h += '<div class="ga-hero' + (on ? '' : ' off') + '">'
      + '<div class="gh-t">' + heroT + '</div>'
      + '<div class="gh-d">' + heroD + '</div>'
      + (acts.length ? '<div class="gh-s">' + acts.length + ' 路执行器 · 自动开着 '
        + on + ' 路 · 待执行 ' + g.pending + ' 条</div>' : '') + '</div>';

    if (g.hint) h += '<div class="note-line">💡 ' + esc(g.hint) + '</div>';
    if (!g.can_auto) h += '<div class="note-line">🔒 自动执行是 PRO 会员功能，升级后在「我的会员」里开通</div>';

    if ((state.plants || []).length > 1) {
      h += '<div class="card"><div class="ct">看哪一盆</div><div class="pick-row">'
        + state.plants.map(function (p) {
          return '<button class="chip ' + (p.id === g.plant_id ? 'g' : '')
            + '" data-ga-plant="' + esc(p.id) + '">' + esc(p.mood || '🌿') + ' '
            + esc(p.name) + '</button>';
        }).join('') + '</div></div>';
    }

    h += '<div class="card"><div class="ct">执行器</div>';
    if (!acts.length) {
      h += '<div class="muted" style="font-size:12.5px;line-height:1.8">'
        + '这盆还没上报执行器。<br>板卡接入后会自动出现在这里（水泵/补光灯…），'
        + '或者先在「绑定设备」把板卡连上。</div>';
    } else {
      acts.forEach(function (a) {
        var st = GUARD_ST[a.state] || GUARD_ST.idle;
        var auto = a.mode === 'auto';
        h += '<div class="ga-row">'
          + '<div class="ga-i">' + (GUARD_ICO[a.kind] || '⚙️') + '</div>'
          + '<div class="ga-m">'
          + '<div class="ga-n">' + esc(a.name || a.label) + '</div>'
          + '<div class="ga-s"><span class="st ' + st[1] + '">' + st[0] + '</span>'
          + '<span class="ga-when">' + esc(guardAgo(a.last_run_at)) + '</span>'
          + (a.today_runs ? '<span class="ga-when">· 今天自动 ' + a.today_runs + ' 次</span>' : '')
          + '</div>'
          + (a.last_run_reason ? '<div class="ga-r">' + esc(a.last_run_reason) + '</div>' : '')
          + '</div>'
          + '<div class="ga-act">'
          + '<button class="chip' + (auto ? ' g' : '') + '" data-ga-mode="' + esc(a.id) + '|auto">自动</button>'
          + '<button class="chip' + (auto ? '' : ' g') + '" data-ga-mode="' + esc(a.id) + '|off">关闭</button>'
          + '</div></div>';
        h += '<button class="btn ghost ga-run" data-ga-run="' + esc(a.id) + '">'
          + '立即' + esc(a.label) + (a.kind === 'water' ? '（抽 '
            + (a.config && a.config.duration_s || 15) + ' 秒）' : '') + '</button>';
      });
    }
    h += '</div>';

    if (g.recent && g.recent.length) {
      h += '<div class="card"><div class="ct">最近执行</div><div class="ga-hist">';
      g.recent.forEach(function (r) {
        var desc = r.source === 'auto' ? '自动' : (r.source === 'app' ? '手动' : '管理台');
        var flag = r.status === 'done' ? '✅' : (r.status === 'failed' ? '❌' : '⏳');
        h += '<div class="ga-h-row"><span class="ga-h-t">' + esc((r.created_at || '').slice(5, 16))
          + '</span><span class="ga-h-d">' + flag + ' ' + desc + esc(r.label || '')
          + (r.result ? ' · ' + esc(r.result) : '') + '</span></div>';
      });
      h += '</div></div>';
    }

    h += '<div class="card muted" style="font-size:12px;line-height:1.8">'
      + '📡 指令下发后，板卡在下一次心跳（约 60 秒）取走执行并回执；'
      + '执行成功会自动记一条成长日记。<br>'
      + '当前默认规则：土壤水分低于 '
      + esc((acts[0] && acts[0].config && acts[0].config.moisture_below) || 25)
      + '% 自动补水，' + esc((acts[0] && acts[0].config && acts[0].config.cooldown_min) || 30)
      + ' 分钟内不重复，一天最多 '
      + esc((acts[0] && acts[0].config && acts[0].config.daily_max) || 6) + ' 次。</div>';
    return h;
  }

  /* ────────── 我的会员 ────────── */
  function loadMember() {
    return api('/billing/summary').then(function (j) { state.member = j; });
  }

  function yuan(c) { return '¥' + (c / 100).toFixed(c % 100 ? 1 : 0); }

  function htmlMember() {
    var m = state.member;
    if (!m) return loading('正在读取会员信息…');
    var s = m.subscription;
    var h = pageHead('我的会员', '#/me');
    h += '<div class="mb-hero"><div class="mh-top">'
      + '<span class="mh-badge">' + esc(s.plan_name) + '</span>'
      + '<span class="mh-day">' + (s.days_left > 0 ? ('还剩 ' + s.days_left + ' 天') : '已到期') + '</span></div>'
      + '<div class="mh-t">' + (s.status === 'active' ? '会员生效中' : '会员已到期') + '</div>'
      + '<div class="mh-d">到期日 ' + esc(s.expires_at || '-')
      + (s.auto_renew ? ' · 到期自动续费' : ' · 已关闭自动续费') + '</div></div>';

    h += '<div class="card"><div class="ct">硬件押金</div>'
      + '<div class="mb-row"><div><div class="mb-k">'
      + (s.deposit_status === 'held' ? '已交 ' + yuan(s.deposit_cents) : '还没交押金') + '</div>'
      + '<div class="mb-v">' + (s.deposit_status === 'held'
        ? '满 ' + s.deposit_refund_months + ' 个月退还' : '设备免费，收押金防止闲置') + '</div></div>'
      + '</div></div>';

    h += '<div class="card"><div class="ct">订阅套餐</div>';
    (m.plans || []).forEach(function (p) {
      var cur = p.key === s.plan;
      h += '<div class="plan-card' + (p.recommend ? ' rec' : '') + (cur ? ' cur' : '') + '">'
        + '<div class="pl-h"><span class="pl-n">' + esc(p.name)
        + (p.recommend ? '<span class="pl-tag">推荐</span>' : '') + '</span>'
        + '<span class="pl-p">' + yuan(p.price_cents) + '<small>/月</small></span></div>'
        + '<div class="pl-f">' + p.features.map(function (f) { return '· ' + esc(f); }).join('<br>') + '</div>'
        + (cur ? '<div class="pl-cur">当前套餐</div>'
               : '<button class="btn ghost" data-plan="' + esc(p.key) + '">选这个</button>')
        + '</div>';
    });
    h += '</div>';

    h += '<div class="card"><div class="ct">加购</div>'
      + (m.addons || []).map(function (a) {
        return '<div class="mb-row"><div><div class="mb-k">' + esc(a.name)
          + ' <span class="mb-p">' + yuan(a.price_cents) + '/月</span></div>'
          + '<div class="mb-v">' + esc(a.tagline) + '</div></div>'
          + '<span class="st ' + (m.entitlements && m.entitlements.insurance ? 'ok' : '') + '">'
          + (m.entitlements && m.entitlements.insurance ? '已含' : '未开通') + '</span></div>';
      }).join('') + '</div>';

    h += '<div class="card"><div class="ct">续费</div>'
      + '<button class="btn ghost" data-bill="' + (s.auto_renew ? 'cancel' : 'resume') + '">'
      + (s.auto_renew ? '关闭自动续费' : '恢复自动续费') + '</button>'
      + '<div class="muted" style="font-size:11.5px;margin-top:10px;line-height:1.7">'
      + '关掉后当期还能用到到期日，只是不再自动扣费。</div></div>';

    h += '<div class="card muted" style="font-size:12px;line-height:1.8">'
      + '💡 ' + esc(m.note || '') + '<br>'
      + '服务器当前' + (m.enforce ? '已开启' : '未开启') + '套餐门禁（演示期不拦人）。</div>';
    return h;
  }

  function htmlMe() {
    var p = state.profile || {};
    var u = p.user || state.user || {};
    var st = p.stats || {};
    var h = pageHead('我的', '#/home');
    h += '<div class="profile-head"><div class="pa">' + esc(u.avatar || '🌿') + '</div>'
      + '<div><div class="pn">' + esc(u.nickname || '园丁') + '</div>'
      + '<div class="ps">' + esc(u.phone_masked || '') + (u.city ? ' · ' + esc(u.city) : '')
      + '</div></div>'
      + '<button class="chip g" data-go="#/profile" style="margin-left:auto">编辑</button></div>';
    if (u.bio) h += '<div class="muted" style="padding:0 16px 10px;font-size:12.5px">' + esc(u.bio) + '</div>';
    h += '<div class="stats">'
      + '<div class="stat"><div class="sv">' + (st.posts == null ? (state.plants || []).length : st.posts)
      + '</div><div class="sl">帖子</div></div>'
      + '<div class="stat"><div class="sv">' + (st.following || 0) + '</div><div class="sl">关注</div></div>'
      + '<div class="stat"><div class="sv">' + (st.followers || 0) + '</div><div class="sl">粉丝</div></div>'
      + '<div class="stat"><div class="sv">' + ((state.ins && state.ins.policies) ? state.ins.policies.length
        : ((state.plants || []).length)) + '</div><div class="sl">保障中</div></div>'
      + '</div>';
    var mb = state.member && state.member.subscription;
    h += '<div class="sub-card" data-go="#/member" style="cursor:pointer">'
      + '<span class="badge">' + (mb ? esc(mb.plan_name) : '参赛演示版') + '</span>'
      + '<div class="st">' + (mb ? (mb.status === 'active'
        ? '会员生效中 · 还剩 ' + mb.days_left + ' 天' : '会员已到期') : '植小伴 Pro') + '</div>'
      + '<div class="sd">' + (mb
        ? ((mb.deposit_status === 'held' ? '押金 ¥' + (mb.deposit_cents / 100).toFixed(0) + ' 已交 · ' : '')
           + '点这里看套餐与续费')
        : '全部功能已开放，正式版将提供更多植物位与云端历史') + '</div></div>';
    h += '<div class="menu">'
      + mi('amber', '🛡️', '绿植保险', (state.ins && state.ins.coverage_total_text) || '保障中', '#/insurance')
      + mi('green', '🤖', '自动守护', (state.guard && state.guard.actuators
          ? state.guard.actuators.length + ' 路执行器' : '自动浇水'), '#/guard')
      + mi('amber', '💎', '我的会员', (mb ? mb.plan_name : ''), '#/member')
      + mi('green', '🪴', '我的植物', (state.plants || []).length + ' 盆', '#/plant')
      + mi('blue', '📡', '绑定设备', '', '#/bind')
      + mi('violet', '💬', 'AI 问答', '', '#/chat')
      + mi('violet', '📷', 'AI 体检', '', '#/diag')
      + mi('green', '📊', '实时数据', '', '#/data')
      + mi('pink', '✏️', '编辑资料', '', '#/profile')
      + mi('blue', '🖥', '服务器地址', location.origin, 'act:server')
      + mi('violet', '🌱', '关于植小伴', 'V1.1', 'act:about')
      + '</div>';
    h += '<div style="text-align:center;padding:6px 0 24px">'
      + '<button class="chip demo" data-act="logout">退出登录</button></div>';
    return h;
  }

  function mi(cls, ico, label, val, target) {
    var attr = target.indexOf('act:') === 0
      ? 'data-act="' + esc(target.slice(4)) + '"'
      : 'data-go="' + esc(target) + '"';
    return '<div class="mi" ' + attr + '>'
      + '<div class="mii" style="background:var(--' + cls + '100)">' + ico + '</div>'
      + '<span>' + esc(label) + '</span>'
      + (val ? '<span class="mv">' + esc(val) + '</span>' : '<span class="arr">›</span>')
      + '</div>';
  }
  /* ────────── 账号：登录 / 注册 / 资料 ────────── */
  var LOGIN = { phone: '', code: '', busy: false, cd: 0, timer: null, tip: '' };

  function loginTip(msg) {
    var t = $('#lg-tip');
    if (t && msg) t.textContent = msg;
  }

  function htmlLogin() {
    return '<div class="login-wrap">'
      + '<div class="login-logo">🌱</div>'
      + '<div class="login-name">植小伴</div>'
      + '<div class="login-sub">陪你和植物一起长大</div>'
      + '<div class="login-card">'
      + '<input id="lg-phone" class="lg-input" type="tel" inputmode="numeric" maxlength="11"'
      + ' placeholder="请输入手机号" value="' + esc(LOGIN.phone) + '">'
      + '<div class="lg-row">'
      + '<input id="lg-code" class="lg-input" type="tel" inputmode="numeric" maxlength="6"'
      + ' placeholder="验证码" value="' + esc(LOGIN.code) + '">'
      + '<button id="lg-send" class="lg-send">获取验证码</button>'
      + '</div>'
      + '<button id="lg-go" class="btn" style="width:100%;margin-top:4px">登录 / 注册</button>'
      + '<div id="lg-tip" class="lg-tip">' + esc(LOGIN.tip || '第一次登录会自动注册，不用单独注册') + '</div>'
      + '</div>'
      + '<button id="lg-demo" class="chip g" style="margin-top:18px">一键演示登录</button>'
      + '<div class="lg-foot">演示版 · 登录即表示同意《用户协议》与《隐私政策》</div>'
      + '</div>';
  }

  function sendCode() {
    var el = $('#lg-phone');
    LOGIN.phone = String((el && el.value) || '').trim();
    if (!/^[0-9]{6,20}$/.test(LOGIN.phone)) { loginTip('手机号看着不太对，检查一下'); return; }
    if (LOGIN.cd > 0) return;
    loginTip('正在发送…');
    api('/auth/sms', { method: 'POST', json: { phone: LOGIN.phone }, auth: false })
      .then(function (j) {
        if (j.dev_code) {
          LOGIN.code = j.dev_code;
          var c = $('#lg-code');
          if (c) c.value = j.dev_code;
          loginTip('演示环境验证码：' + j.dev_code + '（已自动填好）');
        } else {
          loginTip('验证码已发送');
        }
        LOGIN.cd = 60;
        var b = $('#lg-send');
        if (b) { b.disabled = true; b.textContent = '60s 后重发'; }
        if (LOGIN.timer) clearInterval(LOGIN.timer);
        LOGIN.timer = setInterval(function () {
          LOGIN.cd--;
          var bb = $('#lg-send');
          if (LOGIN.cd <= 0) {
            clearInterval(LOGIN.timer);
            LOGIN.timer = null;
            if (bb) { bb.disabled = false; bb.textContent = '获取验证码'; }
            return;
          }
          if (bb) bb.textContent = LOGIN.cd + 's 后重发';
        }, 1000);
      }, function (e) { loginTip('发送失败：' + (e.message || '')); });
  }

  function doLogin() {
    var p1 = $('#lg-phone'), p2 = $('#lg-code');
    LOGIN.phone = String((p1 && p1.value) || '').trim();
    LOGIN.code = String((p2 && p2.value) || '').trim();
    if (!/^[0-9]{6,20}$/.test(LOGIN.phone)) { loginTip('手机号看着不太对'); return; }
    if (!LOGIN.code) { loginTip('请先获取并填写验证码'); return; }
    if (LOGIN.busy) return;
    LOGIN.busy = true;
    loginTip('正在登录…');
    api('/auth/login', { method: 'POST', auth: false,
                         json: { phone: LOGIN.phone, code: LOGIN.code } })
      .then(function (j) {
        saveLogin(j);
        location.hash = '#/home';
        location.reload();
      }, function (e) {
        LOGIN.busy = false;
        loginTip('登录失败：' + (e.message || ''));
      });
  }

  function demoLogin() {
    if (LOGIN.busy) return;
    LOGIN.busy = true;
    loginTip('正在进入演示账号…');
    api('/auth/demo', { method: 'POST', auth: false, json: {} }).then(function (j) {
      saveLogin(j);
      location.hash = '#/home';
      location.reload();
    }, function (e) {
      LOGIN.busy = false;
      loginTip('演示登录失败：' + (e.message || ''));
    });
  }

  /* ────────── 资料 ────────── */
  var AVA = ['🌿', '🌱', '🪴', '🌻', '🌸', '🌵', '🍀', '🌴', '🐱', '🐶', '🧑‍🌾', '👩‍🌾'];

  function loadProfile() {
    return api('/auth/me').then(function (j) { state.profile = j; });
  }

  function htmlProfile() {
    var p = (state.profile || {}).user || state.user || {};
    var h = pageHead('编辑资料', '#/me');
    h += '<div class="card"><div class="ct">头像</div><div class="ava-grid" id="ava-grid">'
      + AVA.map(function (a) {
        return '<button class="ava-pick' + (a === (p.avatar || '🌿') ? ' on' : '')
          + '" data-ava="' + esc(a) + '">' + a + '</button>';
      }).join('') + '</div></div>';
    h += '<div class="card">'
      + '<div class="ct">昵称</div>'
      + '<input id="pf-nick" class="lg-input" maxlength="16" placeholder="给自己起个名字" value="' + esc(p.nickname || '') + '">'
      + '<div class="ct" style="margin-top:14px">城市</div>'
      + '<input id="pf-city" class="lg-input" maxlength="12" placeholder="例如：杭州" value="' + esc(p.city || '') + '">'
      + '<div class="ct" style="margin-top:14px">一句话简介</div>'
      + '<input id="pf-bio" class="lg-input" maxlength="40" placeholder="例如：龟背竹重度爱好者" value="' + esc(p.bio || '') + '">'
      + '</div>';
    h += '<div style="padding:0 14px 26px"><button class="btn" id="pf-save" style="width:100%">保存</button>'
      + '<div class="muted" style="text-align:center;margin-top:10px;font-size:11.5px">手机号 '
      + esc(p.phone_masked || '') + '（不可修改）</div></div>';
    return h;
  }

  function saveProfile() {
    var ava = document.querySelector('.ava-pick.on');
    var body = {
      nickname: String((($('#pf-nick') || {}).value) || ''),
      city: String((($('#pf-city') || {}).value) || ''),
      bio: String((($('#pf-bio') || {}).value) || ''),
      avatar: ava ? ava.getAttribute('data-ava') : ((state.user || {}).avatar || '🌿')
    };
    api('/auth/me', { method: 'PATCH', json: body }).then(function (j) {
      state.user = j.user;
      state.profile = null;
      state.community.posts = [];
      try { localStorage.setItem(K_USER, JSON.stringify(j.user || {})); } catch (e) { }
      toast('已保存');
      go('#/me');
    }, function (e) { toast('保存失败：' + (e.message || '')); });
  }

  /* ────────── 绑定设备 ────────── */
  function htmlBind() {
    var h = pageHead('绑定设备', '#/me');
    h += '<div class="card"><div class="ct">设备序列号 SN</div>'
      + '<input id="bd-sn" class="lg-input" placeholder="例如 ZXB-DEMO-0001" autocomplete="off">'
      + '<div class="muted" style="font-size:11.5px;margin:10px 2px 0;line-height:1.7">'
      + 'SN 印在板卡背面，也会显示在开机欢迎页上。<br>演示板卡：<b>ZXB-DEMO-0001</b></div></div>';
    var ps = state.plants || [];
    h += '<div class="card"><div class="ct">绑到哪一盆</div><div class="pick-row">'
      + (ps.length ? ps.map(function (p) {
        return '<button class="chip ' + (state.bindPlant === p.id ? 'g' : '')
          + '" data-bd-plant="' + esc(p.id) + '">' + esc(p.mood || '🌿') + ' '
          + esc(p.name) + (p.device_id ? ' · 已连卡' : ' · 未绑卡') + '</button>';
      }).join('') : '<div class="muted" style="font-size:12px">还没有植物，绑定后会给你新建一盆</div>')
      + '</div>'
      + '<div class="muted" style="font-size:11.5px;margin-top:10px;line-height:1.7">'
      + '不选也可以：会自动挂到还没绑设备的那一盆；一盆都没有就新建一盆。'
      + '一台板卡同一时间只挂一盆植物。</div>'
      + '<button class="btn" id="bd-go" style="width:100%;margin-top:14px">绑定这台设备</button></div>';
    h += '<div class="card muted" style="font-size:12px;line-height:1.7">'
      + '绑定后，这台板卡上报的土壤八项数据、摄像头照片、AI 体检结果都会进到这盆植物名下。</div>';
    return h;
  }

  function bindDevice() {
    var sn = String((($('#bd-sn') || {}).value) || '').trim();
    if (!sn) { toast('先填设备 SN'); return; }
    var body = { sn: sn };
    if (state.bindPlant) body.plant_id = state.bindPlant;
    api('/devices/bind', { method: 'POST', json: body }).then(function (j) {
      toast('绑定成功' + (j.plant && j.plant.name ? '：' + j.plant.name : ''));
      state.bindPlant = null;
      state.plants = []; state.detail = null; state.lastSync = 0;
      state.community.posts = [];
      return loadPlants();
    }).then(function () { go('#/home'); },
      function (e) { toast('绑定失败：' + (e.message || '')); });
  }

  /* ────────── 其他用户主页 ────────── */
  function loadUser() {
    if (!state.userId) return Promise.resolve();
    return api('/community/users/' + state.userId).then(function (j) { state.uhome = j; });
  }

  function htmlUserHome() {
    var u = state.uhome;
    if (!u) return loading();
    var h = pageHead(u.user.nickname, '#/community');
    h += '<div class="profile-head"><div class="pa">' + esc(u.user.avatar) + '</div>'
      + '<div><div class="pn">' + esc(u.user.nickname) + '</div>'
      + '<div class="ps">' + (u.user.city ? esc(u.user.city) + ' · ' : '')
      + u.plants + ' 盆植物 · ' + u.followers + ' 粉丝</div></div>'
      + (u.is_me ? '' : '<button class="chip ' + (u.followed ? '' : 'g')
        + '" id="uh-follow" style="margin-left:auto">' + (u.followed ? '已关注' : '+ 关注') + '</button>')
      + '</div>';
    h += u.posts.length ? u.posts.map(postHTML).join('')
      : emptyBox('🌿', '还没有发过帖子', '');
    h += '<div style="height:24px"></div>';
    return h;
  }

  /* ────────── 绿植保险 / 一键理赔 ────────── */
  var CLAIM_ST = {
    approved: ['已通过', 'ok'], reviewing: ['人工复核中', 'warn'],
    need_more_info: ['待补材料', 'warn'], shipped: ['已补发', 'ok'],
    rejected: ['未通过', 'bad'], submitted: ['已提交', 'warn']
  };

  function loadInsurance() {
    return api('/insurance/summary').then(function (j) { state.ins = j; });
  }

  function claimRowHTML(c) {
    var st = CLAIM_ST[c.status] || ['处理中', 'warn'];
    return '<div class="claim-row" data-claim="' + esc(c.id) + '">'
      + '<div class="cr-l"><div class="cr-no2">' + esc(c.reason_name) + '<small>' + esc(c.claim_no) + '</small></div>'
      + '<div class="cr-time">' + esc((c.created_at || '').slice(5, 16)) + '</div></div>'
      + '<div class="cr-r"><span class="st ' + st[1] + '">' + st[0] + '</span>'
      + (c.payout_text ? '<small>' + esc(c.payout_text) + '</small>' : '')
      + '</div></div>';
  }

  function htmlInsurance() {
    var s = state.ins;
    if (!s) return loading('正在读取保单…');
    var pols = s.policies || [];
    var h = pageHead('绿植保险', '#/me');
    h += '<div class="ins-hero">'
      + '<div class="ih-top"><span class="ih-badge">🛡️ 保障中</span>'
      + '<span class="ih-co">' + esc(s.coverage_total_text) + '</span></div>'
      + '<div class="ih-t">' + esc(s.plan.name) + '</div>'
      + '<div class="ih-d">' + esc(s.plan.desc) + '</div></div>';
    h += '<div class="card"><div class="ct">已保障的植物（' + pols.length + ' 盆）</div>'
      + (pols.length ? pols.map(function (p) {
        return '<div class="pol-row"><span class="pr-a">' + esc(p.plant_mood) + '</span>'
          + '<span class="pr-n">' + esc(p.plant_name)
          + '<small>保额 ' + esc(p.coverage_text) + ' · 保障至 ' + esc(p.expires_at) + '</small></span>'
          + '<span class="pr-s' + (p.claimed ? ' used' : '') + '">'
          + (p.claimed ? '本期已理赔' : '保障中') + '</span></div>';
      }).join('') : '<div class="muted" style="font-size:12.5px">还没有植物，先去「绑定设备」或「我的植物」添加一盆</div>')
      + '</div>';
    h += '<div class="claim-cta" id="go-claim"><div class="cc-i">⚡</div>'
      + '<div class="cc-t">一键理赔<div class="cc-s">传感器 + AI 体检自动取证 · 秒级预审</div></div>'
      + '<span class="cc-a">›</span></div>';
    h += '<div class="card"><div class="ct">我的理赔记录</div>'
      + (s.claims.length ? s.claims.map(claimRowHTML).join('')
        : '<div class="muted" style="font-size:12.5px">还没有理赔记录</div>')
      + '</div>';
    h += '<div class="card muted" style="font-size:12px;line-height:1.75">'
      + '<b>为什么能做到「一键」？</b><br>'
      + '点下去的那一刻，服务器已经把这盆植物的传感器历史、AI 体检记录、照片、'
      + '养护打卡率、设备在线情况全部取出来当证据，按规则当场算预审分：'
      + '证据齐全的免人工直接通过；材料不够的，直接告诉你缺什么。'
      + '不用填表，也不用等人工。</div>';
    return h;
  }

  // 这盆植物是不是已经有一张在处理中的单子（服务端只允许同时存在一张）
  function openClaimOf(pid) {
    var cs = (state.ins && state.ins.claims) || [];
    for (var i = 0; i < cs.length; i++) {
      var st = cs[i].status;
      if (cs[i].plant_id === pid &&
          (st === 'submitted' || st === 'reviewing' || st === 'need_more_info')) return cs[i];
    }
    return null;
  }

  function openClaim(id) {
    state.claimResult = null;
    api('/insurance/claims/' + id).then(function (j) {
      state.claimResult = j.claim;
      rerender();
    }, function (e) { toast('打不开：' + (e.message || '')); });
  }

  function htmlClaim() {
    var s = state.ins;
    var h = pageHead('一键理赔', '#/insurance');
    // 必须先看理赔结果：提交成功（或点开历史单）时 state.ins 会被清空，
    // 若先判 ins 就会一直卡在「正在加载…」，看不到预审结论。
    if (state.claimResult) return h + claimResultHTML(state.claimResult);
    if (!s) return loading();
    var pols = s.policies || [];
    if (!pols.length) {
      return h + emptyBox('🪴', '还没有可理赔的植物', '先去绑定设备，或添加一盆植物');
    }
    h += '<div class="card"><div class="ct">① 哪一盆出问题了</div><div class="pick-row">'
      + pols.map(function (p) {
        return '<button class="chip ' + (state.claimPlant === p.plant_id ? 'g' : '')
          + '" data-cl-plant="' + esc(p.plant_id) + '">' + esc(p.plant_mood) + ' '
          + esc(p.plant_name) + (openClaimOf(p.plant_id) ? ' · 处理中' : '') + '</button>';
      }).join('') + '</div></div>';
    h += '<div class="card"><div class="ct">② 怎么了</div><div class="pick-row">'
      + (s.reasons || []).map(function (r) {
        return '<button class="chip ' + (state.claimReason === r.key ? 'g' : '')
          + '" data-cl-reason="' + esc(r.key) + '">' + r.icon + ' ' + esc(r.name) + '</button>';
      }).join('') + '</div></div>';
    h += '<div class="card"><div class="ct">③ 说两句（选填）</div>'
      + '<textarea id="cl-desc" class="cf-text" maxlength="300" '
      + 'placeholder="比如：上周开始叶子发黄，这几天更明显了">' + esc(state.claimDesc || '') + '</textarea></div>';
    h += '<div class="card"><div class="ct">④ 凭证照片</div>'
      + '<div class="muted" style="font-size:12px;line-height:1.7">不选也没关系 —— 会自动带上这盆植物最近的一张照片，'
      + '配合传感器和 AI 体检记录一起作为凭证。</div></div>';
    h += '<div style="padding:0 14px 26px">'
      + '<button class="btn" id="cl-go" style="width:100%;font-size:16.5px;padding:15px">⚡ 一键提交理赔</button>'
      + '<div class="muted" style="text-align:center;margin-top:9px;font-size:11.5px">提交后立即自动取证并给出预审结论</div></div>';
    return h;
  }

  function evLine(icon, name, val) {
    return '<div class="ev-line"><span class="ev-i">' + icon + '</span>'
      + '<span class="ev-n">' + esc(name) + '</span>'
      + '<span class="ev-v">' + esc(val) + '</span></div>';
  }

  function claimResultHTML(c) {
    var ev = c.evidence || {};
    var good = (c.status === 'approved' || c.status === 'shipped');
    var h = '<div class="claim-res ' + (good ? 'ok' : 'warn') + '">'
      + '<div class="cr-score">' + (c.auto_score == null ? '--' : c.auto_score)
      + '<small>预审分</small></div>'
      + '<div class="cr-t">' + esc(c.auto_note || '') + '</div>'
      + '<div class="cr-no">理赔单号 ' + esc(c.claim_no) + '</div>'
      + (c.payout_text ? '<div class="cr-pay">赔付方案：' + esc(c.payout_text) + '（补发同款绿植）</div>' : '')
      + '</div>';
    h += '<div class="card"><div class="ct">自动取证（服务器自己找到的）</div><div class="ev-list">'
      + evLine('📷', '照片', (ev.photo_count || 0) + ' 张'
        + (ev.last_photo_at ? '，最近 ' + ev.last_photo_at.slice(5, 16) : ''))
      + evLine('🔍', 'AI 体检', (ev.diagnose_count || 0) + ' 次'
        + (ev.last_diagnose_score == null ? '' : '，最近健康分 ' + ev.last_diagnose_score))
      + evLine('📊', '传感器', (ev.telemetry_count || 0) + ' 条记录'
        + (ev.moisture ? '，水分 ' + ev.moisture.min + '~' + ev.moisture.max + '%' : ''))
      + evLine('✅', '养护打卡', ev.task_rate == null ? '暂无数据' : Math.round(ev.task_rate * 100) + '%')
      + evLine('📡', '设备', (ev.device && ev.device.sn)
        ? ev.device.sn + '（' + (ev.device.status === 'online' ? '在线' : '离线') + '）' : '未绑定')
      + '</div></div>';
    if (c.notes && c.notes.length) {
      h += '<div class="card"><div class="ct">预审判定依据</div>'
        + c.notes.map(function (n) { return '<div class="note-line">· ' + esc(n) + '</div>'; }).join('')
        + '</div>';
    }
    h += '<div class="card"><div class="ct">处理进度</div>'
      + (c.timeline || []).map(function (t) {
        return '<div class="tl-line"><span class="tdot"></span><span>'
          + esc(String(t.ts || '').slice(5, 16)) + ' ' + esc(t.text) + '</span></div>';
      }).join('') + '</div>';
    h += '<div style="padding:0 14px 26px">';
    if (c.status === 'approved') {
      h += '<button class="btn" id="cl-accept" style="width:100%">接受赔付，补发新苗</button>';
    } else if (c.status === 'reviewing') {
      h += '<button class="btn" id="cl-advance" style="width:100%">（演示）模拟人工审核通过</button>'
        + '<div class="muted" style="text-align:center;margin-top:9px;font-size:11.5px">'
        + '正式版这里是后台客服审核，App 里不会有这个按钮</div>';
    } else if (c.status === 'need_more_info') {
      h += '<button class="btn" id="cl-again" style="width:100%">回去补充材料</button>';
    } else if (c.status === 'shipped') {
      h += '<button class="btn ghost" id="cl-back" style="width:100%">已经补发，回保险首页</button>';
    }
    h += '<button class="chip g" id="cl-back2" style="width:100%;margin-top:10px">返回保险首页</button></div>';
    return h;
  }

  function submitClaim() {
    if (!state.claimPlant) { toast('先选一盆植物'); return; }
    if (!state.claimReason) { toast('说一下是怎么了'); return; }
    var dup = openClaimOf(state.claimPlant);
    if (dup) {
      toast('这盆还在理赔中，直接给你打开那张单子');
      openClaim(dup.id);
      return;
    }
    var el = $('#cl-desc');
    state.claimDesc = String((el && el.value) || '').trim();
    var btn = $('#cl-go');
    if (btn) { btn.disabled = true; btn.textContent = '正在自动取证…'; }
    api('/insurance/claims', {
      method: 'POST',
      json: { plant_id: state.claimPlant, reason: state.claimReason,
              description: state.claimDesc }
    }).then(function (j) {
      state.claimResult = j.claim;
      state.ins = null;
      rerender();
    }, function (e) {
      if (btn) { btn.disabled = false; btn.textContent = '⚡ 一键提交理赔'; }
      toast('提交失败：' + (e.message || ''));
    });
  }

  function backToInsurance() {
    state.claimResult = null;
    state.ins = null;
    go('#/insurance');
  }

  function claimAction(url, okMsg) {
    api(url, { method: 'POST', json: {} }).then(function (j) {
      state.claimResult = j.claim;
      state.ins = null;
      toast(okMsg);
      rerender();
    }, function (e) { toast('操作失败：' + (e.message || '')); });
  }

  /* ────────── AI 问答 ────────── */
  function chatHTML() {
    if (!state.chat.length) return '';
    return state.chat.map(function (m) {
      if (m.role === 'sys') return '<div class="msg sys">' + esc(m.text) + '</div>';
      return '<div class="msg ' + (m.role === 'user' ? 'me' : 'ai') + '">' + esc(m.text) + '</div>';
    }).join('');
  }

  function htmlChat() {
    var p = state.plant || {};
    var body = state.chat.length
      ? chatHTML()
      : emptyBox('💬', '和 ' + (p.name || '小绿绿') + ' 聊聊天',
        '植物怎么养、作业题、笑话、故事…什么都能聊');
    // 快捷问句：一半养护、一半趣味 —— 让用户一眼看出"不只是聊植物"
    var quicks = ['它今天状态怎么样？', '讲个笑话给我听',
      '叶子发黄怎么办？', '为什么天空是蓝色的？'];
    return pageHead('AI 问答', '#/home')
      + '<div class="chat-body" id="chat-body">' + body + '</div>'
      + '<div style="display:flex;gap:7px;flex-wrap:wrap;padding:0 14px 10px">'
      + quicks.map(function (q) {
        return '<button class="chip g" data-q="' + esc(q) + '">' + esc(q) + '</button>';
      }).join('')
      + '</div><div style="height:96px"></div>'
      + '<div class="chat-listen" id="chat-listen" hidden></div>'
      + '<div class="chat-bar">'
      + '<button id="chat-mic" class="mic" title="语音输入">🎤</button>'
      + '<button id="chat-spk" class="mic" title="自动朗读">🔊</button>'
      + '<input id="chat-in" placeholder="聊点什么都行…" autocomplete="off">'
      + '<button id="chat-send">➤</button></div>';
  }

  function paintChat() {
    var b = $('#chat-body');
    if (b) b.innerHTML = chatHTML();
    scrollChat();
  }

  function scrollChat() {
    var v = $('#view');
    if (v) v.scrollTop = v.scrollHeight;
  }

  function sendChat(text) {
    text = String(text || '').trim();
    if (!text || state.chatBusy) return;
    state.chatBusy = true;
    state.chat.push({ role: 'user', text: text });
    state.chat.push({ role: 'ai', text: '正在想…' });
    paintChat();
    var inp = $('#chat-in');
    if (inp) inp.value = '';
    var payload = { plant_id: state.plantId, text: text };
    if (state.convId) payload.conversation_id = state.convId;
    api('/ai/chat', { method: 'POST', json: payload }).then(function (j) {
      state.convId = j.conversation_id || state.convId;
      var reply = (j.message && j.message.text) || '（没有回复）';
      state.chat[state.chat.length - 1] = {
        role: 'ai',
        text: reply
      };
      /* 自动朗读：小朋友不用看字也能听（关掉开关则不念） */
      speakText(reply);
    }, function (e) {
      state.chat[state.chat.length - 1] = { role: 'sys', text: '发送失败：' + (e.message || '') };
    }).then(function () {
      state.chatBusy = false;
      paintChat();
    });
  }

  /* ────────── 语音输入（App 原生采音 → 服务器转文字） ──────────
   * 为什么不用网页录音：页面是 http（非安全上下文），Chrome/WebView 会把
   * navigator.mediaDevices 直接藏起来，网页根本拿不到麦克风；而且
   * MediaRecorder 产出的是 webm/opus，服务器也解不了。所以在植小伴 App 里
   * 由原生层录 16k 单声道 PCM，直接 POST /voice/transcribe 换成文字，
   * 填进输入框后照常提问（这样能带上上下文，也不会多出一段语音回复）。
   * 浏览器里打开时按钮会提示"要在 App 里用"。 */
  var voice = { on: false, busy: false, t0: 0, timer: null, guard: null };

  function micReady() {
    try { return !!(window.ZXB && window.ZXB.hasVoice && window.ZXB.hasVoice()); }
    catch (e) { return false; }
  }

  function voiceSecs() {
    return voice.t0 ? Math.max(0, Math.round((Date.now() - voice.t0) / 1000)) : 0;
  }

  function paintVoice() {
    var b = $('#chat-mic');
    if (b) {
      b.classList.toggle('rec', voice.on);
      b.disabled = voice.busy;
      b.textContent = voice.on ? '⏹' : '🎤';
    }
    var t = $('#chat-listen');
    if (t) {
      if (voice.on) {
        t.hidden = false;
        t.textContent = '🎙 正在听… ' + voiceSecs() + ' 秒（说完再点一下）';
      } else if (voice.busy) {
        t.hidden = false;
        t.textContent = '✨ 正在把你说的变成文字…';
      } else {
        t.hidden = true;
      }
    }
  }

  function voiceReset() {
    if (voice.timer) { clearInterval(voice.timer); voice.timer = null; }
    if (voice.guard) { clearTimeout(voice.guard); voice.guard = null; }
    voice.on = false;
    voice.busy = false;
    voice.t0 = 0;
    paintVoice();
  }

  function startVoice() {
    if (voice.on || voice.busy) return;
    if (!micReady()) { toast('语音输入要在植小伴 App 里用哦'); return; }
    var r = 'no';
    try { r = String(window.ZXB.startVoice(state.token || '')); } catch (e) { r = 'no'; }
    if (r === 'ask') { toast('请在弹窗里点“允许”，然后再点一次麦克风'); return; }
    if (r !== 'ok') { toast('打不开麦克风，请检查手机的麦克风权限'); return; }
    voice.on = true;
    voice.t0 = Date.now();
    voice.timer = setInterval(paintVoice, 200);
    paintVoice();
  }

  function stopVoice() {
    if (!voice.on) return;
    voice.on = false;
    voice.busy = true;
    voice.t0 = 0;
    if (voice.timer) { clearInterval(voice.timer); voice.timer = null; }
    paintVoice();
    try { window.ZXB.stopVoice(); } catch (e) {
      voiceReset();
      toast('结束录音失败，请再试一次');
      return;
    }
    voice.guard = setTimeout(function () {
      if (voice.busy) { voiceReset(); toast('识别超时了，请再试一次'); }
    }, 40000);
  }

  function cancelVoice() {
    if (!voice.on && !voice.busy) return;
    try { if (voice.on) window.ZXB.cancelVoice(); } catch (e) { }
    voiceReset();
  }

  function toggleMic() { if (voice.on) stopVoice(); else startVoice(); }

  /* 原生层回调（Java 用 evaluateJavascript 调用）：
     {event:'started'|'denied'|'error'|'text', text, error} */
  window.__zxbVoice = function (m) {
    m = m || {};
    if (m.event === 'started') {
      if (!voice.on) {
        voice.on = true;
        voice.t0 = Date.now();
        voice.timer = setInterval(paintVoice, 200);
      }
      paintVoice();
    } else if (m.event === 'denied') {
      voiceReset();
      toast('没有麦克风权限，语音用不了哦');
    } else if (m.event === 'error') {
      voiceReset();
      toast(m.error || '语音输入失败了，请再试一次');
    } else if (m.event === 'text') {
      voiceReset();
      var t = String(m.text || '').trim();
      if (!t) { toast('没听清，请再说一次'); return; }
      if (state.chatBusy) {
        var i0 = $('#chat-in');
        if (i0) i0.value = t;
        toast('上一条还在回复，稍等一下再问');
        return;
      }
      var inp = $('#chat-in');
      if (inp) inp.value = t;
      sendChat(t);
    }
  };

  /* ────────── AI 体检（拍照） ────────── */
  /* ────────── 自动朗读：把 AI 的回复念出来 ──────────
   * 给小朋友用：拍照体检和问 AI 的回复都自动读一遍，不看字也能听懂。
   *
   * 为什么不用浏览器自带的 speechSynthesis：Android WebView 没有实现
   * Web Speech API，网页里根本念不出声。改成让服务器 TTS 合成、前端播
   * 免鉴权直链 —— 手机上听到的声音和板卡上完全一样，而且改前端不用重装 APK
   * （原生壳已经设了 setMediaPlaybackRequiresUserGesture(false)，能自动播）。
   *
   * 开关存 localStorage：家长想静音点一下即可，默认开。 */
  var speak = { on: localStorage.getItem('zxb_speak') !== '0', audio: null, seq: 0 };

  function paintSpeak() {
    var b = $('#chat-spk');
    if (!b) return;
    b.textContent = speak.on ? '🔊' : '🔇';
    b.title = speak.on ? '自动朗读：开（点一下静音）' : '自动朗读：关（点一下打开）';
  }

  function toggleSpeak() {
    speak.on = !speak.on;
    try { localStorage.setItem('zxb_speak', speak.on ? '1' : '0'); } catch (e) { }
    if (!speak.on) speakStop();
    paintSpeak();
    toast(speak.on ? '回复会自动念出来' : '已静音，只显示文字');
  }

  /* 停掉当前朗读，并作废在途请求 —— 否则早发出的那条晚回来会把新的顶掉 */
  function speakStop() {
    speak.seq++;
    if (speak.audio) {
      try { speak.audio.pause(); } catch (e) { }
      speak.audio = null;
    }
    /* App 里的系统 TTS 也要停：不然切页了它还在念 */
    try { if (window.ZXB && window.ZXB.stopSpeak) window.ZXB.stopSpeak(); } catch (e) { }
  }

  /* App 里的系统语音引擎可用吗（不可用就退回服务器合成） */
  function nativeTtsOk() {
    try {
      return !!(window.ZXB && window.ZXB.ttsReady && window.ZXB.ttsReady());
    } catch (e) {
      return false;
    }
  }

  /* 念之前把 emoji / 颜文字去掉：系统语音会把它们念成乱七八糟的东西。
   * 用 new RegExp 包起来是为了万一老 WebView 不支持 u 标志也不至于整个
   * 脚本解析失败。 */
  var EMOJI_RE = null;
  try {
    EMOJI_RE = new RegExp('[\\u{1F000}-\\u{1FAFF}\\u{2600}-\\u{27BF}'
                          + '\\u{FE0F}\\u{200D}\\u{2190}-\\u{21FF}]', 'gu');
  } catch (e) { EMOJI_RE = null; }

  function stripEmoji(t) {
    t = String(t || '');
    if (EMOJI_RE) {
      try { t = t.replace(EMOJI_RE, ''); } catch (e) { }
    }
    return t.replace(/\s+/g, ' ').trim();
  }

  function speakText(text) {
    text = String(text || '').trim();
    if (!speak.on || !text) return;
    speakStop();
    var mine = speak.seq;

    /* ① 装的是植小伴 App：优先用手机自带的语音引擎 —— 本地合成几乎瞬时
     *    出声，不用等服务器（那条实测 2.6~5 秒）。 */
    if (nativeTtsOk()) {
      var s = stripEmoji(text);
      if (s) {
        try {
          window.ZXB.speak(s);
          return;
        } catch (e) { /* 掉下去走服务器 */ }
      }
    }

    /* ② 浏览器里打开、或手机没装中文语音包：退回服务器合成 */
    api('/voice/speak', { method: 'POST', json: { text: text, plant_id: state.plantId } })
      .then(function (j) {
        if (!speak.on || mine !== speak.seq || !j || !j.audio_url) return;
        var a = new Audio(j.audio_url);
        speak.audio = a;
        var p = a.play();
        if (p && p.catch) p.catch(function () { /* 自动播放被拦：静默 */ });
      })
      .catch(function () { /* 念不出来不影响看字 */ });
  }

  /* 体检结果要念什么：植物名 + 小结 + 第一条建议（服务器那边还会限长 120 字） */
  function diagSpeakText(r) {
    r = r || {};
    var out = (r.name ? r.name + '。' : '') + String(r.summary || '').trim();
    var sug = (r.suggestions && r.suggestions[0] && r.suggestions[0].text) || '';
    if (sug) out += '。建议：' + sug;
    return out;
  }

  function htmlDiag() {
    var h = pageHead('AI 体检', '#/home');
    h += '<div class="card" style="text-align:center;padding:18px 14px">'
      + '<div style="font-size:38px">📷</div>'
      + '<div style="font-size:14px;font-weight:800;margin-top:6px">拍一张植物的照片</div>'
      + '<div style="font-size:11.5px;color:var(--gray);margin-top:4px">AI 会告诉你它是什么植物、健不健康、需要注意什么</div>'
      + '<button class="btn" id="diag-pick" style="margin-top:12px">拍照 / 从相册选</button>'
      + '<input type="file" accept="image/*" id="diag-file" style="display:none">'
      + '</div>'
      + '<div id="diag-box"></div>';
    return h;
  }

  function paintDiag() {
    var box = $('#diag-box');
    if (!box) return;
    var d = state.diag;
    if (!d) {
      box.innerHTML = '<div class="card" style="font-size:11.5px;color:var(--gray);line-height:1.65">'
        + '提示：照片会上传到服务器由 AI 分析，同时存进这盆植物的成长日记里。</div>';
      return;
    }
    var h = '';
    if (d.preview) h += '<div class="card flush"><img src="' + d.preview + '" style="width:100%;display:block"></div>';
    if (d.stage) h += '<div class="loading"><div class="spinner"></div>' + esc(d.stage) + '</div>';
    if (d.error) h += '<div class="card" style="color:#dc2626;font-size:13px">' + esc(d.error) + '</div>';
    var r = d.result;
    if (r) {
      var score = num(r.health_score);
      if (score == null) score = 0;
      h += '<div class="card">'
        + '<div style="display:flex;align-items:center;gap:14px">'
        + ring(score)
        + '<div style="flex:1;min-width:0"><div style="font-size:18px;font-weight:900">'
        + esc(r.name || '未识别') + '</div>'
        + '<div style="font-size:11.5px;color:var(--gray-2);margin-top:2px">'
        + esc(r.latin || '') + (r.match ? ' · 匹配度 ' + r.match + '%' : '') + '</div>'
        + '<div style="margin-top:6px">' + levelChip(score) + '</div></div></div>'
        + '<div style="font-size:13px;color:var(--ink-2);line-height:1.6;margin-top:11px">'
        + esc(r.summary || '') + '</div></div>';
      if (r.problems && r.problems.length) {
        h += '<div class="sec"><h3>发现的问题</h3></div>';
        r.problems.forEach(function (pb) {
          h += '<div class="card"><div style="font-size:13.5px;font-weight:800">⚠️ '
            + esc(pb.title || pb.name || '问题') + '</div>'
            + (pb.detail || pb.tip || pb.text
              ? '<div style="font-size:12px;color:var(--gray);margin-top:4px">'
              + esc(pb.detail || pb.tip || pb.text) + '</div>' : '')
            + '</div>';
        });
      }
      if (r.suggestions && r.suggestions.length) {
        h += '<div class="sec"><h3>养护建议</h3></div>';
        r.suggestions.forEach(function (sg) {
          h += '<div class="card"><div style="display:flex;gap:9px;align-items:flex-start">'
            + '<div style="font-size:18px">' + actIcon(sg.action) + '</div>'
            + '<div style="font-size:13px;color:var(--ink-2);line-height:1.6">' + esc(sg.text || '') + '</div>'
            + '</div></div>';
        });
      }
      h += '<div style="text-align:center;padding:4px 0 20px">'
        + '<button class="chip g" id="diag-again">再拍一张</button></div>';
    }
    box.innerHTML = h;
    var again = $('#diag-again');
    if (again) again.onclick = function () { pickDiag(); };
  }

  function actIcon(a) {
    if (a === 'water') return '💧';
    if (a === 'light') return '☀️';
    if (a === 'fertilize') return '🌾';
    if (a === 'move') return '📦';
    if (a === 'prune') return '✂️';
    return '🌱';
  }

  function levelChip(score) {
    if (score >= 80) return '<span class="chip g">状态良好</span>';
    if (score >= 60) return '<span class="chip a">需要留意</span>';
    return '<span class="chip v">需要帮助</span>';
  }

  function ring(score) {
    var r = 26, c = 2 * Math.PI * r;
    var off = c * (1 - Math.max(0, Math.min(100, score)) / 100);
    var col = score >= 80 ? '#16a34a' : (score >= 60 ? '#f59e0b' : '#ec4899');
    return '<svg width="66" height="66" viewBox="0 0 66 66" style="flex:0 0 auto">'
      + '<circle cx="33" cy="33" r="' + r + '" fill="none" stroke="#f1f5f9" stroke-width="6"/>'
      + '<circle cx="33" cy="33" r="' + r + '" fill="none" stroke="' + col + '" stroke-width="6"'
      + ' stroke-linecap="round" stroke-dasharray="' + c.toFixed(1) + '" stroke-dashoffset="' + off.toFixed(1) + '"'
      + ' transform="rotate(-90 33 33)"/>'
      + '<text x="33" y="37" text-anchor="middle" font-size="16" font-weight="700" fill="'
      + col + '">' + score + '</text></svg>';
  }
  function shrinkImage(file, maxSide) {
    return new Promise(function (res, rej) {
      var url = URL.createObjectURL(file);
      var img = new Image();
      img.onload = function () {
        var w = img.width, h = img.height;
        var s = Math.min(1, maxSide / Math.max(w, h));
        w = Math.max(1, Math.round(w * s));
        h = Math.max(1, Math.round(h * s));
        var cv = document.createElement('canvas');
        cv.width = w;
        cv.height = h;
        cv.getContext('2d').drawImage(img, 0, 0, w, h);
        URL.revokeObjectURL(url);
        cv.toBlob(function (b) {
          if (b) res(b); else rej(new Error('图片处理失败'));
        }, 'image/jpeg', 0.85);
      };
      img.onerror = function () {
        URL.revokeObjectURL(url);
        rej(new Error('这张图片读不出来'));
      };
      img.src = url;
    });
  }

  function pickDiag() {
    var f = $('#diag-file');
    if (f) f.click();
  }

  function runDiag(file) {
    if (!file || state.diagBusy) return;
    if (!state.plantId) { toast('还没有植物，先去「我的植物」添加一盆'); return; }
    state.diagBusy = true;
    state.diag = { preview: URL.createObjectURL(file), stage: '正在压缩图片…', result: null, error: '' };
    paintDiag();
    var mediaId = null;
    shrinkImage(file, 1024)
      .then(function (blob) {
        state.diag.stage = '正在上传（' + Math.round(blob.size / 1024) + ' KB）…';
        paintDiag();
        return api('/media/image?fmt=jpeg&plant_id=' + encodeURIComponent(state.plantId), {
          method: 'POST', body: blob, type: 'image/jpeg'
        });
      })
      .then(function (j) {
        mediaId = j.media && j.media.id;
        if (!mediaId) throw new Error('上传没有拿到图片编号');
        state.diag.stage = 'AI 正在看照片…（大约 5~15 秒）';
        paintDiag();
        return api('/ai/diagnose', {
          method: 'POST', json: { media_id: mediaId, plant_id: state.plantId }
        });
      })
      .then(function (j) {
        state.diag.stage = '';
        state.diag.result = j.result || {};
        /* 自动朗读体检结论：小朋友不用看字也能听懂（关掉开关则不念） */
        speakText(diagSpeakText(state.diag.result));
        if (j.recognized === false) {
          state.diag.error = 'AI 没认出这是什么植物，换一张更清楚的试试';
        }
        state.detail = null;
        state.events = [];
        state.lastSync = 0;
      }, function (e) {
        state.diag.stage = '';
        state.diag.error = e && e.message ? e.message : '体检失败';
      })
      .then(function () {
        state.diagBusy = false;
        paintDiag();
      });
  }

  /* ────────── 我的植物 ────────── */
  function htmlPlant() {
    var h = pageHead('我的植物', '#/me');
    h += '<div class="sensors">';
    (state.plants || []).forEach(function (p) {
      var on = p.id === state.plantId;
      h += '<div class="sen' + (on ? '' : ' low') + '" data-plant="' + esc(p.id) + '" style="cursor:pointer">'
        + '<div class="sv">' + esc(p.mood || '🌿') + '<small>' + esc(p.health_level || '') + '</small></div>'
        + '<div class="sn">' + esc(p.name) + '</div>'
        + '<div class="sh">' + esc(p.species || '') + ' · ' + p.health_score + ' 分'
        + (on ? ' · 当前' : '')
        + (p.device_id ? '' : ' · 未绑卡') + '</div>'
        + '</div>';
    });
    h += '<div class="sen" data-act="toggle-add" style="cursor:pointer;border-left-color:var(--gray-2)">'
      + '<div class="sv">＋</div><div class="sn">添加植物</div><div class="sh">给它起个名字</div></div>';
    h += '</div>';
    h += '<div class="card" id="add-form" hidden>'
      + '<div style="font-size:14px;font-weight:800;margin-bottom:9px">添加一盆新植物</div>'
      + '<input id="np-name" placeholder="名字，例如：小绿绿" style="width:100%;background:#f3f4f6;border-radius:12px;padding:10px 13px;margin-bottom:8px">'
      + '<input id="np-species" placeholder="品种，例如：绿萝" style="width:100%;background:#f3f4f6;border-radius:12px;padding:10px 13px;margin-bottom:8px">'
      + '<input id="np-location" placeholder="摆放位置，例如：客厅窗台" style="width:100%;background:#f3f4f6;border-radius:12px;padding:10px 13px;margin-bottom:11px">'
      + '<button class="btn" data-act="save-plant">保存</button></div>';
    return h;
  }

  function savePlant() {
    var name = ($('#np-name') || {}).value || '';
    var species = ($('#np-species') || {}).value || '';
    var location = ($('#np-location') || {}).value || '';
    if (!String(name).trim()) { toast('先给它起个名字吧'); return; }
    api('/plants', {
      method: 'POST',
      json: { name: String(name).trim(), species: String(species).trim() || '绿植', location: String(location).trim() }
    }).then(function (j) {
      state.plantId = j.plant.id;
      try { localStorage.setItem(K_PLANT, state.plantId); } catch (e) { }
      state.detail = null;
      state.tasks = [];
      state.events = [];
      state.series = null;
      state.lastSync = 0;
      return loadPlants();
    }).then(function () {
      toast('添加成功 🌱');
      render();
    }, function (e) { toast('添加失败：' + (e.message || '')); });
  }

  function completeTask(id) {
    api('/tasks/' + id + '/complete', { method: 'POST' }).then(function () {
      toast('完成啦，成长值 +5 🌱');
      state.detail = null;
      state.lastSync = 0;
      state.taskSync = 0;
      return ensureDetail(true);
    }).then(function () { return ensureTasks(true); })
      .then(function () {
        if (normRoute(location.hash) === '#/home') {
          var v = $('#view');
          v.innerHTML = htmlHome();
          bindFor('#/home', v);
        }
      }, function (e) { toast('操作失败：' + (e.message || '')); });
  }

  /* ────────── 事件绑定 ────────── */
  function bindFor(route, view) {
    if ($('#diag-box', view)) paintDiag();
    var pick = $('#diag-pick', view);
    if (pick) pick.onclick = pickDiag;
    var file = $('#diag-file', view);
    if (file) {
      file.onchange = function () {
        var f = file.files && file.files[0];
        file.value = '';
        if (f) runDiag(f);
      };
    }
    var send = $('#chat-send', view);
    if (send) send.onclick = function () { sendChat(($('#chat-in') || {}).value); };
    var mic = $('#chat-mic', view);
    if (mic) mic.onclick = toggleMic;
    var spk = $('#chat-spk', view);
    if (spk) { spk.onclick = toggleSpeak; paintSpeak(); }
    var lgGo = $('#lg-go', view);
    if (lgGo) lgGo.onclick = doLogin;
    var lgSend = $('#lg-send', view);
    if (lgSend) lgSend.onclick = sendCode;
    var lgDemo = $('#lg-demo', view);
    if (lgDemo) lgDemo.onclick = demoLogin;
    var pfSave = $('#pf-save', view);
    if (pfSave) pfSave.onclick = saveProfile;
    var bdGo = $('#bd-go', view);
    if (bdGo) bdGo.onclick = bindDevice;
    var cfSend = $('#cf-send', view);
    if (cfSend) cfSend.onclick = sendPost;
    var cfText = $('#cf-text', view);
    if (cfText) {
      cfText.oninput = function () { COMPOSE.text = cfText.value; };
    }
    var clGo = $('#cl-go', view);
    if (clGo) clGo.onclick = submitClaim;
    var clAcc = $('#cl-accept', view);
    if (clAcc) {
      clAcc.onclick = function () {
        claimAction('/insurance/claims/' + state.claimResult.id + '/accept', '已确认补发');
      };
    }
    var clAdv = $('#cl-advance', view);
    if (clAdv) {
      clAdv.onclick = function () {
        claimAction('/insurance/claims/' + state.claimResult.id + '/advance', '人工复核通过');
      };
    }
    var clAgain = $('#cl-again', view);
    if (clAgain) {
      clAgain.onclick = function () {
        state.claimResult = null;
        state.ins = null;
        loadInsurance().then(rerender, rerender);
      };
    }
    var clBack = $('#cl-back', view);
    if (clBack) clBack.onclick = backToInsurance;
    var clBack2 = $('#cl-back2', view);
    if (clBack2) clBack2.onclick = backToInsurance;
    var goClaim = $('#go-claim', view);
    if (goClaim) {
      goClaim.onclick = function () {
        state.claimResult = null; state.claimPlant = null;
        state.claimReason = null; state.claimDesc = '';
        go('#/claim');
      };
    }
    var uhf = $('#uh-follow', view);
    if (uhf) {
      uhf.onclick = function () {
        api('/community/users/' + state.userId + '/follow', { method: 'POST', json: {} })
          .then(function (j) { state.uhome.followed = j.followed; rerender(); },
                function (e) { toast('操作失败：' + (e.message || '')); });
      };
    }
    var cmSend = $('#cm-send', view);
    if (cmSend) cmSend.onclick = sendComment;
    var cmIn = $('#cm-in', view);
    if (cmIn) {
      cmIn.onkeydown = function (ev) {
        if (ev.key === 'Enter') { ev.preventDefault(); sendComment(); }
      };
    }
    var cmSearch = $('#cm-search', view);
    if (cmSearch) {
      cmSearch.onkeydown = function (ev) {
        if (ev.key !== 'Enter') return;
        ev.preventDefault();
        state.community.q = cmSearch.value.trim();
        state.community.posts = [];
        loadFeed(true).then(rerender);
      };
    }
    var inp = $('#chat-in', view);
    if (inp) {
      inp.onkeydown = function (ev) {
        if (ev.key === 'Enter') { ev.preventDefault(); sendChat(inp.value); }
      };
    }
    var qs = view.querySelectorAll('[data-q]');
    for (var i = 0; i < qs.length; i++) {
      qs[i].onclick = function () { sendChat(this.getAttribute('data-q')); };
    }
    var tbs = view.querySelectorAll('[data-task]');
    for (var j = 0; j < tbs.length; j++) {
      tbs[j].onclick = function () { completeTask(this.getAttribute('data-task')); };
    }
    var ms = view.querySelectorAll('[data-metric]');
    for (var k = 0; k < ms.length; k++) {
      ms[k].onclick = function () {
        state.metric = this.getAttribute('data-metric');
        var v = $('#view');
        v.innerHTML = htmlData();
        bindFor('#/data', v);
      };
    }
    var ps = view.querySelectorAll('[data-plant]');
    for (var q = 0; q < ps.length; q++) {
      ps[q].onclick = function () {
        state.plantId = this.getAttribute('data-plant');
        try { localStorage.setItem(K_PLANT, state.plantId); } catch (e) { }
        state.detail = null;
        state.tasks = [];
        state.events = [];
        state.series = null;
        state.lastSync = 0;
        state.taskSync = 0;
        loadPlants().then(function () { go('#/home'); });
      };
    }
  }
  function reload(force) {
    var route = normRoute(location.hash);
    if (route === '#/data') state.series = null;
    if (route === '#/diary' || route === '#/home') state.events = [];
    if (force) { state.detail = null; state.lastSync = 0; }
    render();
  }

  /* ────────── 全局点击 ────────── */
  document.addEventListener('click', function (ev) {
    var t = ev.target;
    // 注意：这里必须用局部变量往上找，不能改写外层 t ——
    // 一旦改写，第一次 up() 就会把 t 走到 document，后面所有判断都会落空，
    // 结果就是除点赞外的点击（底部导航、社区 tab、退出登录…）全部失灵。
    function up(sel) {
      var n = t;
      while (n && n !== document) {
        if (n.hasAttribute && n.hasAttribute(sel)) return n;
        n = n.parentNode;
      }
      return null;
    }
    var lk = up('data-like');
    if (lk) {
      var lid = lk.getAttribute('data-like');
      api('/community/posts/' + lid + '/like', { method: 'POST', json: {} }).then(function (j) {
        eachPost(lid, function (q) { q.liked = j.liked; q.like_count = j.like_count; });
        rerender();
      }, function (e) { toast('点赞失败：' + (e.message || '')); });
      return;
    }
    var fv = up('data-fav');
    if (fv) {
      var fid = fv.getAttribute('data-fav');
      api('/community/posts/' + fid + '/fav', { method: 'POST', json: {} }).then(function (j) {
        eachPost(fid, function (q) { q.faved = j.faved; });
        rerender();
        toast(j.faved ? '已收藏' : '已取消收藏');
      }, function (e) { toast('操作失败：' + (e.message || '')); });
      return;
    }
    var fo = up('data-follow');
    if (fo) {
      var ouid = fo.getAttribute('data-follow');
      api('/community/users/' + ouid + '/follow', { method: 'POST', json: {} }).then(function (j) {
        var c = state.community;
        for (var i = 0; i < c.posts.length; i++) {
          if (c.posts[i].author.id === ouid) c.posts[i].followed = j.followed;
        }
        if (state.post && state.post.author.id === ouid) state.post.followed = j.followed;
        rerender();
        toast(j.followed ? '已关注' : '已取消关注');
      }, function (e) { toast('操作失败：' + (e.message || '')); });
      return;
    }
    var op = up('data-open') || up('data-open2');
    if (op) {
      state.postId = op.getAttribute('data-open') || op.getAttribute('data-open2');
      state.post = null;
      go('#/post');
      return;
    }
    var ct = up('data-ctab');
    if (ct) {
      var c2 = state.community;
      c2.tab = ct.getAttribute('data-ctab');
      c2.posts = []; c2.rank = null; c2.q = '';
      loadFeed(true).then(rerender);
      return;
    }
    var tp = up('data-topic');
    if (tp) {
      COMPOSE.topic = tp.getAttribute('data-topic');
      go('#/compose');
      return;
    }
    var cc = up('data-ccat');
    if (cc) {
      COMPOSE.cat = cc.getAttribute('data-ccat');
      rerender();
      return;
    }
    var ph = up('data-photo');
    if (ph) {
      var mid = ph.getAttribute('data-photo');
      var i2 = COMPOSE.media.indexOf(mid);
      if (i2 >= 0) COMPOSE.media.splice(i2, 1);
      else if (COMPOSE.media.length >= 3) { toast('最多 3 张'); return; }
      else COMPOSE.media.push(mid);
      rerender();
      return;
    }
    var av = up('data-ava');
    if (av) {
      var all = document.querySelectorAll('.ava-pick');
      for (var k = 0; k < all.length; k++) all[k].classList.remove('on');
      av.classList.add('on');
      return;
    }
    var bp = up('data-bd-plant');
    if (bp) { state.bindPlant = bp.getAttribute('data-bd-plant'); rerender(); return; }
    var gm = up('data-ga-mode');
    if (gm) {
      var gm2 = gm.getAttribute('data-ga-mode').split('|');
      api('/actuators/' + gm2[0] + '/mode', { method: 'POST', json: { mode: gm2[1] } })
        .then(function () { return loadGuard(); })
        .then(function () { rerender(); toast(gm2[1] === 'auto' ? '已开启自动执行' : '已关闭自动执行'); },
              function (e) { toast(e.message || '设置失败'); });
      return;
    }
    var gr = up('data-ga-run');
    if (gr) {
      var gid = gr.getAttribute('data-ga-run');
      toast('指令已下发，等板卡执行…');
      api('/actuators/' + gid + '/run', { method: 'POST', json: {} })
        .then(function () { return loadGuard(); })
        .then(function () { rerender(); toast('已排队，板卡下一次心跳取走执行'); },
              function (e) { toast(e.message || '下发失败'); });
      return;
    }
    var gp = up('data-ga-plant');
    if (gp) { loadGuard(gp.getAttribute('data-ga-plant')).then(rerender); return; }
    var pl = up('data-plan');
    if (pl) {
      var pk = pl.getAttribute('data-plan');
      api('/billing/subscribe', { method: 'POST', json: { plan: pk, months: 1 } }).then(function (j) {
        toast('已开通 ' + ((j.subscription && j.subscription.plan) || pk) + '（演示期直接生效）');
        return loadMember();
      }).then(rerender, function (e) { toast('开通失败：' + (e.message || '')) });
      return;
    }
    var bl = up('data-bill');
    if (bl) {
      var act = bl.getAttribute('data-bill') === 'cancel' ? '/billing/cancel' : '/billing/subscribe';
      var body = bl.getAttribute('data-bill') === 'cancel' ? {} : { plan: (state.member && state.member.subscription.plan) || 'pro', months: 1 };
      api(act, { method: 'POST', json: body }).then(function () {
        toast(bl.getAttribute('data-bill') === 'cancel' ? '已关闭自动续费' : '已恢复自动续费');
        return loadMember();
      }).then(rerender, function (e) { toast('操作失败：' + (e.message || '')) });
      return;
    }
    var cp = up('data-cl-plant');
    if (cp) { state.claimPlant = cp.getAttribute('data-cl-plant'); rerender(); return; }
    var cr = up('data-cl-reason');
    if (cr) { state.claimReason = cr.getAttribute('data-cl-reason'); rerender(); return; }
    var cw = up('data-claim');
    if (cw) {
      state.claimResult = null;
      api('/insurance/claims/' + cw.getAttribute('data-claim')).then(function (j) {
        state.claimResult = j.claim;
        go('#/claim');
      }, function (e) { toast('打不开：' + (e.message || '')); });
      return;
    }
    var uu = up('data-user');
    if (uu) { state.userId = uu.getAttribute('data-user'); state.uhome = null; go('#/user'); return; }
    var g = up('data-go');
    if (g) { go(g.getAttribute('data-go')); return; }
    var a = up('data-act');
    if (a) {
      var act = a.getAttribute('data-act');
      if (act === 'reload') reload(true);
      else if (act === 'logout') { logout(); }
      else if (act === 'more-posts') { loadMorePosts(); }
      else if (act === 'cf-topic-x') { COMPOSE.topic = ''; rerender(); }
      else if (act === 'server') { toast('服务器：' + location.origin); }
      else if (act === 'about') { toast('植小伴 V1.0 · 参赛演示版'); }
      else if (act === 'toggle-add') {
        var f = $('#add-form');
        if (f) f.hidden = !f.hidden;
      } else if (act === 'save-plant') { savePlant(); }
    }
  });

  /* ────────── 启动 ────────── */
  function boot() {
    try {
      state.token = localStorage.getItem(K_TOKEN);
      state.user = JSON.parse(localStorage.getItem(K_USER) || 'null');
    } catch (e) { }
    window.addEventListener('hashchange', render);
    if (!state.token) {
      if (normRoute(location.hash) !== '#/login') location.hash = '#/login';
      render();
      $('#boot').hidden = true;
      $('#app').hidden = false;
      return;
    }
    Promise.resolve().then(loadPlants)
      .then(function () {
        initDeviceInfo();
        if (!location.hash || location.hash === '#/login') location.hash = '#/home';
        render();
        $('#boot').hidden = true;
        $('#app').hidden = false;
      })
      .then(function () {
        setInterval(function () {
          if (document.hidden) return;
          if (normRoute(location.hash) !== '#/home') return;
          state.detail = null;
          state.lastSync = 0;
          state.taskSync = 0;
          Promise.all([loadPlants(), ensureDetail(true), ensureTasks(true)])
            .then(function () {
              if (normRoute(location.hash) !== '#/home') return;
              var v = $('#view');
              v.innerHTML = htmlHome();
              bindFor('#/home', v);
            }, function () { });
        }, 60000);
      })
      .then(null, function (e) {
        var b = $('#boot-sub');
        b.style.color = '#dc2626';
        b.textContent = '连接不上服务器：' + (e && e.message ? e.message : '') + '（点一下重试）';
        b.onclick = function () { location.reload(); };
      });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
