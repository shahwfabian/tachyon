// Tachyon site: renders benchmark + fuzz results and drives the WebAssembly engine.

const $ = (s, r = document) => r.querySelector(s);
const fmt = (x, d = 0) => Number(x).toLocaleString("en-US", { maximumFractionDigits: d, minimumFractionDigits: d });
const ns = (x) => (x >= 1000 ? `${fmt(x / 1000, 2)} µs` : `${fmt(x, 0)} ns`);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

async function loadJSON(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(`${path}: ${r.status}`);
  return r.json();
}

// ---- headline stats ---------------------------------------------------------------------------

function renderStats(bench, fuzz) {
  const t = bench.results[0];
  const set = (k, v) => ($(`.stat .v[data-k="${k}"]`).textContent = v);
  set("mops", `${fmt(t.mops, 1)}M`);
  set("p50", ns(t.latency_ns.p50));
  set("p99", ns(t.latency_ns.p99));
  set("speedup", `${fmt(bench.speedup.throughput, 1)}×`);
  set("bugs", `${fuzz.summary.found}/${fuzz.summary.total}`);
  const lens = fuzz.bugs.filter((b) => b.found).map((b) => b.shrunk_length);
  set("shrunk", `≤ ${Math.max(...lens)}`);
  $("#machine").textContent =
    `Measured on a laptop ${bench.machine.cpu} (${bench.machine.compiler}), single core, ` +
    `${fmt(bench.workload.events)} operations over a book of ~${fmt(bench.workload.target_resting / 1000)}k resting orders. ` +
    `Timer overhead of ${fmt(bench.machine.timer_overhead_ns, 1)} ns subtracted.`;
}

// ---- benchmark --------------------------------------------------------------------------------

function renderHistogram(bench) {
  const edges = bench.histogram_edges_ns;
  const [A, B] = bench.results;
  const norm = (h) => {
    const tot = h.reduce((a, b) => a + b, 0);
    return h.map((x) => x / tot);
  };
  const ha = norm(A.histogram), hb = norm(B.histogram);
  const W = 640, H = 280, L = 44, R = 12, T = 14, Bm = 34;
  const lo = Math.log(edges[0]), hi = Math.log(edges[edges.length - 1]);
  const x = (v) => L + ((Math.log(Math.max(v, edges[0])) - lo) / (hi - lo)) * (W - L - R);
  const ymax = Math.max(...ha, ...hb) * 1.12;
  const y = (f) => T + (1 - f / ymax) * (H - T - Bm);
  // Bin i covers [edges[i-1], edges[i]); bin 0 is below edges[0] and is folded into the first bar.
  const path = (h) => {
    let d = `M${x(edges[0])},${y(0)}`;
    for (let i = 1; i < edges.length; i++) {
      const f = h[i] + (i === 1 ? h[0] : 0);
      d += ` L${x(edges[i - 1])},${y(f)} L${x(edges[i])},${y(f)}`;
    }
    return d + ` L${x(edges[edges.length - 1])},${y(0)} Z`;
  };
  const ticks = [10, 30, 100, 300, 1000, 3000, 10000, 30000];
  const pct = (r, cls, key, dy) => {
    const v = r.latency_ns[key];
    const X = x(v);
    return `<line class="pline ${cls}" x1="${X}" x2="${X}" y1="${T}" y2="${H - Bm}"/>` +
      `<text class="plabel" x="${X + 4}" y="${T + 10 + dy}" fill="currentColor">${key} ${ns(v)}</text>`;
  };
  $("#hist").innerHTML = `
    <svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Latency histogram, Tachyon versus std::map reference">
      <line class="axis" x1="${L}" x2="${W - R}" y1="${H - Bm}" y2="${H - Bm}"/>
      ${ticks.map((t) => `<line class="axis" x1="${x(t)}" x2="${x(t)}" y1="${H - Bm}" y2="${H - Bm + 4}"/>
        <text class="tick" x="${x(t)}" y="${H - Bm + 17}" text-anchor="middle">${t >= 1000 ? t / 1000 + "µs" : t + "ns"}</text>`).join("")}
      <text class="tick" x="${L - 6}" y="${T + 8}" text-anchor="end">share</text>
      <path class="b" d="${path(hb)}" stroke-width="1.2"/>
      <path class="a" d="${path(ha)}" stroke-width="1.2"/>
      <g style="color:var(--bid)">${pct(A, "pl-a", "p50", 0)}${pct(A, "pl-a", "p99", 26)}</g>
      <g style="color:var(--ref)">${pct(B, "pl-b", "p50", 13)}${pct(B, "pl-b", "p99", 39)}</g>
    </svg>`;
}

function renderTables(bench) {
  const [A, B] = bench.results;
  const keys = [["p50", "p50"], ["p90", "p90"], ["p99", "p99"], ["p999", "p99.9"], ["mean", "mean"]];
  $("#pct").innerHTML =
    `<tr><th></th><th>Tachyon</th><th>std::map</th><th>speedup</th></tr>` +
    `<tr><td>throughput</td><td class="good">${fmt(A.mops, 2)}M/s</td><td>${fmt(B.mops, 2)}M/s</td><td class="good">${fmt(A.mops / B.mops, 1)}×</td></tr>` +
    keys.map(([k, label]) =>
      `<tr><td>${label}</td><td class="good">${fmt(A.latency_ns[k], 0)}</td><td>${fmt(B.latency_ns[k], 0)}</td>` +
      `<td class="good">${fmt(B.latency_ns[k] / Math.max(A.latency_ns[k], 0.1), 1)}×</td></tr>`).join("");
  const ops = ["add", "cancel", "modify", "market"];
  $("#bytype").innerHTML =
    `<tr><th>op</th><th>share</th><th>p50</th><th>p99</th><th>vs map p99</th></tr>` +
    ops.map((o) => {
      const share = { add: bench.workload.mix.passive_add + bench.workload.mix.aggressive_add, cancel: bench.workload.mix.cancel,
        modify: bench.workload.mix.modify, market: bench.workload.mix.market }[o];
      return `<tr><td>${o}</td><td>${share}%</td><td>${fmt(A.by_type[o].p50)}</td><td>${fmt(A.by_type[o].p99)}</td>` +
        `<td class="good">${fmt(B.by_type[o].p99 / A.by_type[o].p99, 1)}×</td></tr>`;
    }).join("");
  const m = bench.workload.mix;
  $("#workload").textContent =
    `Workload: ${m.passive_add}% passive limit orders clustered near the touch, ${m.aggressive_add}% aggressive limits, ` +
    `${m.cancel}% cancels, ${m.modify}% modifies, ${m.market}% market orders; ${fmt(bench.fills)} fills totalling ` +
    `${fmt(bench.volume)} lots, identical across both books. Throughput is the median of ${A.runs_mops.length} runs.`;
}

// ---- fuzzing ----------------------------------------------------------------------------------

function renderFuzz(fuzz) {
  const c = fuzz.clean;
  $("#clean").innerHTML =
    `<span>Clean engine (no bugs enabled):</span>
     <span><b>${fmt(c.sequences)}</b> random sequences</span>
     <span><b>${fmt(c.events)}</b> events checked</span>
     <span><b class="ok">${c.failures} false positives</b></span>`;
  const rows = fuzz.bugs.map((b, i) => `
    <tr data-i="${i}">
      <td><b>${esc(b.name)}</b><div class="desc">${esc(b.description)}</div></td>
      <td>${b.found ? '<span class="pill ok">caught</span>' : '<span class="pill">missed</span>'}</td>
      <td><span class="pill kind">${esc(b.failure_kind || "—")}</span></td>
      <td class="num">${fmt(b.sequences_tried)}</td>
      <td class="num shrink">${b.found ? `<s>${b.original_length}</s> → <b>${b.shrunk_length}</b>` : "—"}</td>
      <td class="num">${b.found ? fmt(b.shrink_runs) : "—"}</td>
    </tr>`).join("");
  $("#bugs").innerHTML = `<thead><tr><th>Planted bug</th><th>Result</th><th>Detected by</th>
      <th class="num">Sequences</th><th class="num">Events: failing → minimal</th><th class="num">Shrink runs</th></tr></thead>
      <tbody>${rows}</tbody>`;
  $("#bugs").addEventListener("click", (e) => {
    const tr = e.target.closest("tr[data-i]");
    if (tr) showRepro(fuzz, +tr.dataset.i, true);
  });
  const first = fuzz.bugs.findIndex((b) => b.key === "word_boundary");
  showRepro(fuzz, first >= 0 ? first : 0, false);
}

function levelsEqual(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

function bookHTML(bids, asks, other) {
  const oth = new Map();
  if (other) for (const l of [...other.bids, ...other.asks]) oth.set(l.p, JSON.stringify(l));
  const row = (l, cls) => {
    const diff = other && oth.get(l.p) !== JSON.stringify(l);
    const orders = l.o.map(([id, q]) => `#${id}×${q}`).join("  ");
    return `<div class="r ${cls} ${diff ? "diff" : ""}"><span>${l.p}</span><span>${l.q}</span><span class="q">${orders}</span></div>`;
  };
  const a = [...asks].reverse().map((l) => row(l, "ask")).join("");
  const b = bids.map((l) => row(l, "bid")).join("");
  // Levels present in the other book but missing here also count as a difference.
  let missing = "";
  if (other) {
    const mine = new Set([...bids, ...asks].map((l) => l.p));
    for (const l of [...other.asks, ...other.bids]) if (!mine.has(l.p))
      missing += `<div class="r diff"><span>${l.p}</span><span>—</span><span class="q">missing level</span></div>`;
  }
  const body = a + b + missing;
  return `<div class="book">${body || '<div class="empty">empty book</div>'}</div>`;
}

function bboHTML(bbo, other) {
  const txt = (b) => `best bid ${b[0] < 0 ? "none" : b[0]} · best ask ${b[1] >= 1024 ? "none" : b[1]}`;
  const diff = other && (bbo[0] !== other[0] || bbo[1] !== other[1]);
  return `<div class="bbo ${diff ? "diff" : ""}">${txt(bbo)}${diff ? "  ← cached top of book is wrong" : ""}</div>`;
}

function fillsText(f) {
  return f.length ? f.map(([t, m, p, q]) => `#${t} hit #${m}: ${q} @ ${p}`).join(" · ") : "no fills";
}

function showRepro(fuzz, i, scroll) {
  const b = fuzz.bugs[i];
  document.querySelectorAll("#bugs tr").forEach((tr) => tr.classList.toggle("sel", +tr.dataset.i === i));
  const el = $("#repro");
  if (!b.found) {
    el.innerHTML = `<p>No failure found for ${esc(b.name)}.</p>`;
    return;
  }
  const last = b.trace.length - 1;
  const draw = (k) => {
    const s = b.trace[k];
    const eng = { bids: s.bids, asks: s.asks }, ref = { bids: s.ref_bids, asks: s.ref_asks };
    const fillsDiffer = JSON.stringify(s.fills) !== JSON.stringify(s.ref_fills);
    el.innerHTML = `
      <div class="repro-head">
        <h3>${esc(b.name)}: minimal reproduction</h3>
        <span class="muted mono">${b.original_length} → ${b.shrunk_length} events in ${fmt(b.shrink_runs)} shrink runs</span>
      </div>
      <div class="steps">${b.trace.map((t, j) =>
        `<button class="step ${j === k ? "on" : ""} ${j === last ? "bad" : ""}" data-k="${j}">${j + 1}. ${esc(t.event.text)}</button>`).join("")}</div>
      ${k === last ? `<div class="err">✗ ${esc(b.failure_kind)}: ${esc(b.failure)}</div>` : ""}
      <div class="cmp">
        <div><h4>Tachyon (bug enabled)${s.status !== s.ref_status ? ` · <span style="color:var(--ask)">${s.status}</span>` : ""}</h4>
          ${s.bbo ? bboHTML(s.bbo, s.ref_bbo) : ""}
          ${bookHTML(s.bids, s.asks, ref)}
          <div class="fills ${fillsDiffer ? "diff" : ""}">${fillsText(s.fills)}</div></div>
        <div><h4>Reference book</h4>
          ${s.ref_bbo ? bboHTML(s.ref_bbo, null) : ""}
          ${bookHTML(s.ref_bids, s.ref_asks, null)}
          <div class="fills">${fillsText(s.ref_fills)}</div></div>
      </div>
      <p class="fine">Highlighted rows differ from the reference. Rows show price, total quantity and the queue in time-priority order (#id×qty).</p>`;
    el.querySelectorAll(".step").forEach((btn) => btn.addEventListener("click", () => draw(+btn.dataset.k)));
  };
  draw(last);
  if (scroll) el.scrollIntoView({ behavior: "smooth", block: "nearest" });
}

// ---- live WebAssembly engine ------------------------------------------------------------------

async function startLive() {
  const TICKS = 1024, MAX_IDS = 1 << 21, SHOW = 11;
  const bytes = await (await fetch("tachyon.wasm")).arrayBuffer();
  $("#wasm-size").textContent = `${fmt(bytes.byteLength / 1024)} KB`;
  const wasi = new Proxy({}, { get: () => () => 0 });  // engine never touches I/O
  const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
  const e = instance.exports;
  if (e._initialize) e._initialize();

  let mid, nextId, synthetic, mine, tape, volume, running = true, opsDone = 0;
  const reset = () => {
    e.tk_init(TICKS, MAX_IDS);
    mid = TICKS / 2; nextId = 1; synthetic = []; mine = new Map(); tape = []; volume = 0;
    for (let i = 0; i < 300; i++) passive();
  };
  const rnd = (n) => Math.floor(Math.random() * n);
  const qty = () => 1 + rnd(rnd(4) === 0 ? 120 : 30);
  const readFills = (n, takerSide) => {
    if (n <= 0) return;
    const f = new Int32Array(e.memory.buffer, e.tk_fills(), n * 4);
    for (let i = 0; i < n; i++) {
      const [taker, maker, price, q] = [f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]];
      volume += q;
      const me = mine.has(taker) || mine.has(maker);
      if (mine.has(maker)) {
        const o = mine.get(maker);
        o.left -= q;
        if (o.left <= 0) mine.delete(maker);
      }
      tape.unshift({ side: takerSide, price, q, me, t: new Date() });
    }
    if (tape.length > 40) tape.length = 40;
  };
  function passive() {
    const side = rnd(2);
    const off = 1 + Math.min(rnd(6), rnd(24));
    const px = side === 0 ? mid - off : mid + off;
    const id = nextId++;
    readFills(e.tk_add(id, side, px, qty()), side);
    synthetic.push(id);
  }
  function step(aggr) {
    if (nextId > MAX_IDS - 10) reset();
    if (rnd(40) === 0) mid = Math.min(TICKS - 40, Math.max(40, mid + (rnd(2) ? 1 : -1)));
    const r = rnd(100);
    const resting = e.tk_resting();
    if (r < aggr) {
      const side = rnd(2);
      if (rnd(3) === 0) readFills(e.tk_market(nextId++, side, qty()), side);
      else {
        const px = side === 0 ? mid + rnd(3) : mid - rnd(3);
        const id = nextId++;
        readFills(e.tk_add(id, side, px, qty()), side);
        synthetic.push(id);
      }
    } else if (r < 55 && resting < 420) {
      passive();
    } else if (synthetic.length) {
      const i = rnd(synthetic.length);
      const id = synthetic[i];
      synthetic[i] = synthetic[synthetic.length - 1];
      synthetic.pop();
      e.tk_cancel(id);
    }
  }

  const depth = (side) => {
    const n = e.tk_depth(side, SHOW);
    const d = new Int32Array(e.memory.buffer, e.tk_depth_ptr(), n * 3);
    const out = [];
    for (let i = 0; i < n; i++) out.push({ p: d[i * 3], q: d[i * 3 + 1], c: d[i * 3 + 2] });
    return out;
  };

  let flash = new Map();
  function render() {
    const bids = depth(0), asks = depth(1);
    const max = Math.max(1, ...bids.map((l) => l.q), ...asks.map((l) => l.q));
    const minePx = new Set([...mine.values()].map((o) => o.price));
    const row = (l, side) => {
      const w = `${(l.q / max) * 100}%`;
      const cls = `${minePx.has(l.p) ? "mine" : ""} ${flash.has(l.p) ? "flash" : ""}`;
      return side === 1
        ? `<div class="lrow ${cls}"><div class="bq"></div><div class="px">${l.p}</div><div class="aq"><div class="bar" style="width:${w}"></div><span class="n">${fmt(l.q)} <span class="muted">(${l.c})</span></span></div></div>`
        : `<div class="lrow ${cls}"><div class="bq"><div class="bar" style="width:${w}"></div><span class="n"><span class="muted">(${l.c})</span> ${fmt(l.q)}</span></div><div class="px">${l.p}</div><div class="aq"></div></div>`;
    };
    const pad = (arr, side) => {
      const rows = arr.map((l) => row(l, side));
      while (rows.length < SHOW) rows.push(`<div class="lrow"><div class="bq"></div><div class="px muted">·</div><div class="aq"></div></div>`);
      return rows;
    };
    $("#ladder").innerHTML = pad(asks, 1).reverse().join("") + `<div class="lrow mid"></div>` + pad(bids, 0).join("");
    const bb = e.tk_best_bid(), ba = e.tk_best_ask();
    $("#spread").textContent = bb >= 0 && ba < TICKS ? `${bb} / ${ba} · spread ${ba - bb}` : "one-sided";
    $("#live-stats").textContent = `${fmt(opsDone)} orders · ${fmt(e.tk_resting())} resting`;
    $("#vol").textContent = `${fmt(volume)} lots`;
    $("#tape").innerHTML = tape.slice(0, 16).map((t) =>
      `<div class="${t.me ? "me" : t.side === 0 ? "b" : "s"}"><span class="muted">${t.t.toLocaleTimeString([], { hour12: false })}</span>` +
      `<span>${t.side === 0 ? "BUY" : "SELL"}${t.me ? " ★" : ""}</span><span>${fmt(t.q)}</span><span>@ ${t.price}</span></div>`).join("");
    const px = $("#t-px");
    if (!px.value) px.value = mid;
  }

  function frame() {
    if (running) {
      const n = +$("#speed").value, aggr = +$("#aggr").value;
      for (let i = 0; i < n; i++) step(aggr);
      opsDone += n;
    }
    for (const [p, k] of flash) (k <= 1 ? flash.delete(p) : flash.set(p, k - 1));
    render();
    requestAnimationFrame(frame);
  }

  // ticket
  let side = 0;
  document.querySelectorAll(".seg-btn").forEach((b) => b.addEventListener("click", () => {
    side = +b.dataset.side;
    document.querySelectorAll(".seg-btn").forEach((x) => x.classList.toggle("active", x === b));
  }));
  const msg = (t) => ($("#t-msg").textContent = t);
  $("#t-limit").addEventListener("click", () => {
    const q = Math.max(1, +$("#t-qty").value | 0), p = +$("#t-px").value | 0;
    const id = nextId++;
    const before = volume;
    const n = e.tk_add(id, side, p, q);
    if (n < 0) return msg("Rejected (price outside the 0–1023 tick grid?)");
    mine.set(id, { price: p, left: q });
    readFills(n, side);
    const filled = volume - before;
    const o = mine.get(id);
    if (o) o.left = q - filled;
    if (o && o.left <= 0) mine.delete(id);
    flash.set(p, 20);
    msg(`#${id}: ${filled ? `filled ${filled}` : "no fill"}${filled < q ? `, ${q - filled} resting at ${p}` : ""}.`);
  });
  $("#t-mkt").addEventListener("click", () => {
    const q = Math.max(1, +$("#t-qty").value | 0);
    const id = nextId++;
    const before = volume;
    mine.set(id, { price: -1, left: 0 });
    readFills(e.tk_market(id, side, q), side);
    mine.delete(id);
    msg(`Market #${id}: filled ${volume - before} of ${q}.`);
  });
  $("#play").addEventListener("click", () => {
    running = !running;
    $("#play").textContent = running ? "Pause" : "Resume";
  });

  reset();
  requestAnimationFrame(frame);
}

// ---- boot -------------------------------------------------------------------------------------

(async () => {
  try {
    const [bench, fuzz] = await Promise.all([loadJSON("data/bench.json"), loadJSON("data/fuzz.json")]);
    renderStats(bench, fuzz);
    renderHistogram(bench);
    renderTables(bench);
    renderFuzz(fuzz);
  } catch (err) {
    console.error(err);
  }
  startLive().catch((err) => {
    console.error(err);
    $("#ladder").innerHTML = `<p class="muted">WebAssembly failed to load: ${esc(err.message)}</p>`;
  });
})();
