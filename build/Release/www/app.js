/* V-Wise Cloud Core 联调看板 —— 原生 JS，1s 轮询，零依赖 */
"use strict";

const $ = (id) => document.getElementById(id);
const fmt = (n) => (n >= 1e6 ? (n / 1e6).toFixed(2) + "M" : n >= 1e4 ? (n / 1e3).toFixed(1) + "k" : String(n));
const esc = (s) => String(s).replace(/[<>&"]/g, (c) => ({"<":"&lt;",">":"&gt;","&":"&amp;",'"':"&quot;"}[c]));

/* ---------------- 手绘多序列折线图（canvas，时间窗自适应） ---------------- */
function drawChart(canvasId, series, opts = {}) {
  const cv = $(canvasId);
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth || 400, h = cv.clientHeight || 150;
  if (cv.width !== w * dpr || cv.height !== h * dpr) { cv.width = w * dpr; cv.height = h * dpr; }
  const ctx = cv.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const pad = { l: 44, r: 8, t: 8, b: 18 };
  const all = [];
  series.forEach((s) => s.points.forEach((p) => all.push(p)));
  if (all.length === 0) { ctx.fillStyle = "#7a8b9d"; ctx.fillText("无数据", w / 2 - 24, h / 2); return; }

  let t0 = all[0].t, t1 = all[all.length - 1].t;
  if (t1 - t0 < 10000) t0 = t1 - 10000; /* 至少 10s 窗口 */
  let ymin = Infinity, ymax = -Infinity;
  series.forEach((s) => s.points.forEach((p) => { if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y; }));
  if (opts.ymin != null) ymin = Math.min(ymin, opts.ymin);
  if (opts.ymax != null) ymax = Math.max(ymax, opts.ymax);
  if (ymax <= ymin) ymax = ymin + 1;

  const X = (t) => pad.l + ((t - t0) / (t1 - t0)) * (w - pad.l - pad.r);
  const Y = (v) => h - pad.b - ((v - ymin) / (ymax - ymin)) * (h - pad.t - pad.b);

  /* 网格 + y 轴标签 */
  ctx.strokeStyle = "#223041"; ctx.fillStyle = "#7a8b9d"; ctx.font = "10px Consolas";
  for (let g = 0; g <= 4; g++) {
    const v = ymin + ((ymax - ymin) * g) / 4;
    const y = Y(v);
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
    ctx.fillText(v.toFixed(ymax - ymin > 20 ? 0 : 1), 4, y + 3);
  }
  ctx.fillText(new Date(t0).toLocaleTimeString(), pad.l, h - 4);
  ctx.textAlign = "right";
  ctx.fillText(new Date(t1).toLocaleTimeString(), w - pad.r, h - 4);
  ctx.textAlign = "left";

  /* 序列 */
  series.forEach((s) => {
    if (s.points.length === 0) return;
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.6;
    ctx.beginPath();
    s.points.forEach((p, i) => (i ? ctx.lineTo(X(p.t), Y(p.y)) : ctx.moveTo(X(p.t), Y(p.y))));
    ctx.stroke();
  });

  /* 图例 */
  ctx.font = "11px system-ui";
  let lx = pad.l + 4;
  series.forEach((s) => {
    if (s.points.length === 0) return;
    ctx.fillStyle = s.color;
    ctx.fillRect(lx, 2, 8, 8);
    ctx.fillStyle = "#9fb0c3";
    ctx.fillText(s.label, lx + 12, 10);
    lx += 12 + ctx.measureText(s.label).width + 14;
  });
}

/* ---------------- 数据拉取与渲染 ---------------- */
let connOk = false;

async function getJSON(url) {
  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(url + " " + r.status);
  return r.json();
}

function badge(on) { return `<span class="badge ${on ? "on" : "off"}">${on ? "alive" : "down"}</span>`; }

async function refreshOverview() {
  try {
    const d = await getJSON("/api/overview");
    if (!connOk) { $("conn").textContent = "已连接"; $("conn").className = "conn ok"; connOk = true; }

    $("mode-uplink").textContent = d.term_mode || "-";
    $("mode-dl").textContent = d.dl_mode || "-";
    $("mode-ebpf").textContent = d.term_ebpf === "on" ? "on" : "fallback";
    $("mode-src").textContent = d.term_alive ? (d.term_source || "-") : "无终端数据";

    /* 终端链路卡 */
    if (d.term_links && d.term_links.length) {
      $("links-body").innerHTML = d.term_links.map((l) =>
        `<div class="kv"><span>#${l.id}</span><span>${badge(l.alive)}
          rtt ${l.rtt_ms >= 0 ? l.rtt_ms.toFixed(1) + "ms" : "-"}
          loss ${l.loss_pct >= 0 ? l.loss_pct.toFixed(1) + "%" : "-"}
          bw ${l.bw_kbps >= 0 ? (l.bw_kbps / 1000).toFixed(1) + "Mbps" : "-"}</span></div>`
      ).join("");
    }

    $("up-unique").textContent = fmt(d.uplink.unique);
    $("up-dup").textContent = fmt(d.uplink.dup);
    $("up-first").textContent = `${fmt(d.uplink.first[0])} / ${fmt(d.uplink.first[1])}`;
    $("up-anchorrx").textContent = `${fmt(d.uplink.anchor_rx[0])} / ${fmt(d.uplink.anchor_rx[1])}`;
    $("up-ack").textContent = `${fmt(d.uplink.probe_ack[0])} / ${fmt(d.uplink.probe_ack[1])}`;

    $("dl-sent").textContent = fmt(d.downlink.sent);
    $("dl-anchor").textContent = `${fmt(d.downlink.by_anchor[0])} / ${fmt(d.downlink.by_anchor[1])}`;
    $("td-rx").textContent = `${fmt(d.term_dedup.rx)} / ${fmt(d.term_dedup.unique)}`;
    $("td-dup").textContent = fmt(d.term_dedup.dup);
    $("td-oos").textContent = `${fmt(d.term_dedup.oos)} / ${fmt(d.term_dedup.last_seq)}`;

    /* 验收面板 */
    if (d.acceptance && d.acceptance.length) {
      $("acceptance").innerHTML = d.acceptance.map((a) => {
        const cls = a.status === "pass" ? "st-pass" : a.status === "fail" ? "st-fail" : "st-na";
        const mark = a.status === "pass" ? "✓" : a.status === "fail" ? "✗" : "–";
        return `<div class="acc-row"><span>${mark} <b>${a.id}</b> <span class="desc">${esc(a.desc)}</span></span>` +
               `<span><span class="val">${a.value >= 0 ? a.value.toFixed(3) : "n/a"}</span><span class="${cls}">${a.status}</span></span></div>`;
      }).join("");
    }
  } catch (e) {
    if (connOk) { $("conn").textContent = "连接断开"; $("conn").className = "conn err"; connOk = false; }
  }
}

async function refreshHistory() {
  try {
    const d = await getJSON("/api/history");
    const rows = d.samples || [];
    const pts = (i) => rows.map((r) => ({ t: r[0], y: r[i] }));
    drawChart("ch-dup", [
      { label: "云侧dup%", color: "#4fc3f7", points: pts(1) },
      { label: "终端dup%", color: "#ffb300", points: rows.map((r) => ({ t: r[0], y: r[7] })) },
    ], { ymin: 0 });
    drawChart("ch-rtt", [
      { label: "link0", color: "#4caf50", points: pts(2) },
      { label: "link1", color: "#4fc3f7", points: pts(3) },
    ]);
    drawChart("ch-thr", [
      { label: "锚点0 pps", color: "#4fc3f7", points: pts(4) },
      { label: "锚点1 pps", color: "#ce93d8", points: pts(5) },
      { label: "下行 pps", color: "#ef5350", points: pts(6) },
    ], { ymin: 0 });
  } catch (e) { /* overview 轮询会报告断连 */ }
}

async function refreshEvents() {
  try {
    const d = await getJSON("/api/events?n=40");
    const evs = d.events || [];
    if (evs.length) {
      $("events-body").innerHTML = evs.map((e) =>
        `<div class="ev ${esc(e.type)}"><span class="t">${new Date(e.ts).toLocaleTimeString()}</span>` +
        `[${esc(e.type)}] ${esc(e.detail)}</div>`
      ).join("");
    }
  } catch (e) { /* ignore */ }
}

/* ---------------- 控制台 ---------------- */
async function postCtl(payload) {
  const msg = $("ctl-msg");
  try {
    const r = await fetch("/api/ctl", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const d = await r.json();
    if (d.ok) { msg.textContent = "✓ " + JSON.stringify(payload); msg.style.color = "#4caf50"; }
    else { msg.textContent = "✗ " + (d.error || "failed"); msg.style.color = "#ef5350"; }
  } catch (e) {
    msg.textContent = "✗ 网络错误"; msg.style.color = "#ef5350";
  }
  setTimeout(refreshEvents, 500);
}

document.querySelectorAll("button[data-act]").forEach((btn) => {
  btn.addEventListener("click", () => {
    const act = btn.dataset.act;
    const mode = btn.dataset.mode;
    if (act === "clear_stats" || act === "train") postCtl({ action: act });
    else postCtl({ action: act, mode: mode });
  });
});

/* ---------------- 启动轮询 ---------------- */
refreshOverview(); refreshHistory(); refreshEvents();
setInterval(refreshOverview, 1000);
setInterval(refreshHistory, 2000);
setInterval(refreshEvents, 3000);
