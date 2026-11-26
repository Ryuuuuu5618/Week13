// 요청 실험실: 버튼을 누르면 같은 서버(8080)에 fetch로 요청을 보내고,
// 응답과 함께 "요청이 서버 안에서 지나간 길"을 흐름도 위에 그린다.

const $ = (sel) => document.querySelector(sel);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

document.getElementById("js-check").textContent = "JS도 제대로 불러와졌습니다 (application/javascript)";
document.getElementById("js-check").classList.add("ok");

// demo/routes.conf 와 같은 규칙. 이 접두사로 시작하면 서버가 proxy_forward로 보낸다.
const PROXY_PREFIXES = ["/api/", "/b/", "/dead/", "/slow/"];
const isProxyPath = (uri) => PROXY_PREFIXES.some((p) => uri.startsWith(p));

/* ───────────── 공통: 요청 보내기 ───────────── */

// 요청 하나를 보내고 상태, 헤더, 본문, 걸린 시간을 돌려준다. 연결 자체가 실패하면 status 0.
async function send(method, path, body, { animate = true } = {}) {
  const start = performance.now();
  let r;
  try {
    const res = await fetch(path, { method, body, cache: "no-store" });
    const buf = await res.arrayBuffer();
    r = {
      status: res.status,
      reason: res.statusText,
      type: res.headers.get("content-type") || "-",
      length: res.headers.get("content-length") || String(buf.byteLength),
      text: new TextDecoder().decode(buf),
      ms: performance.now() - start,
    };
  } catch (e) {
    r = { status: 0, reason: "연결 실패", type: "-", length: "-", text: "", ms: performance.now() - start };
  }
  if (animate) flow.show(method, path, r);
  return r;
}

function statusTag(r) {
  const cls = r.status === 0 ? "sx" : "s" + String(r.status)[0];
  const label = r.status === 0 ? "ERR" : r.status + " " + r.reason;
  return `<span class="status ${cls}">${label}</span>`;
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

async function withButton(btn, job) {
  btn.disabled = true;
  try { await job(); } finally { btn.disabled = false; }
}

/* ───────────── 흐름도 애니메이션 ───────────── */

const flow = (() => {
  const svg = $("#flow-svg");
  const dots = $("#dots");
  const P = {
    browser: [65, 97], accept: [210, 97], queue: [355, 97], worker: [515, 97], fork: [610, 97],
    static: [705, 32], proxy: [705, 97], echo: [705, 162], bfork: [797, 97], a: [882, 67], b: [882, 127],
  };
  const BASE = [P.browser, P.accept, P.queue, P.worker];
  const ROUTES = {
    worker: { pts: BASE, nodes: ["browser", "accept", "queue", "worker"] },
    static: { pts: [...BASE, P.fork, [610, 32], P.static], nodes: ["browser", "accept", "queue", "worker", "static"] },
    echo:   { pts: [...BASE, P.fork, [610, 162], P.echo], nodes: ["browser", "accept", "queue", "worker", "echo"] },
    proxy:  { pts: [...BASE, P.fork, P.proxy], nodes: ["browser", "accept", "queue", "worker", "proxy"] },
    a:      { pts: [...BASE, P.fork, P.proxy, P.bfork, [797, 67], P.a], nodes: ["browser", "accept", "queue", "worker", "proxy", "a"] },
    b:      { pts: [...BASE, P.fork, P.proxy, P.bfork, [797, 127], P.b], nodes: ["browser", "accept", "queue", "worker", "proxy", "b"] },
    none:   { pts: [P.browser], nodes: ["browser"] },
  };
  const node = (id) => svg.querySelector("#n-" + id);
  const busy = {};   // 노드별로 지금 몇 개의 점이 지나가는 중인지

  function mark(id, cls, on) {
    busy[id + cls] = (busy[id + cls] || 0) + (on ? 1 : -1);
    node(id).classList.toggle(cls, busy[id + cls] > 0);
  }

  // 점 하나를 꺾은선을 따라 움직인다.
  function moveDot(pts, cls, speed) {
    const seg = [];
    let total = 0;
    for (let i = 1; i < pts.length; i++) {
      const d = Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
      seg.push(d);
      total += d;
    }
    const c = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    c.setAttribute("r", 7);
    c.setAttribute("class", cls);
    c.setAttribute("cx", pts[0][0]);
    c.setAttribute("cy", pts[0][1]);
    dots.appendChild(c);
    const dur = Math.max(250, total / speed);
    return new Promise((resolve) => {
      const t0 = performance.now();
      function step(now) {
        let dist = Math.min(1, (now - t0) / dur) * total;
        let i = 0;
        while (i < seg.length - 1 && dist > seg[i]) { dist -= seg[i]; i++; }
        const f = seg[i] ? Math.min(1, dist / seg[i]) : 1;
        c.setAttribute("cx", pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f);
        c.setAttribute("cy", pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f);
        if (now - t0 < dur) requestAnimationFrame(step);
        else { c.remove(); resolve(); }
      }
      if (pts.length < 2) { c.remove(); resolve(); } else requestAnimationFrame(step);
    });
  }

  // 요청과 응답을 보고 어느 길로 갔는지 정한다 (서버 코드의 분기 순서와 같게).
  function classify(method, uri, r) {
    if (r.status === 0) return { route: "none", ok: false, why: "서버에 연결하지 못함 (서버가 꺼져 있나요?)" };
    if (isProxyPath(uri)) {
      const m = r.text.match(/백엔드 ([AB])/);
      if (r.status === 200 && m) return { route: m[1].toLowerCase(), ok: true, why: `proxy_forward → 백엔드 ${m[1]}` };
      return { route: "proxy", ok: false, why: "proxy_forward → 백엔드 연결 실패" };
    }
    if (method === "POST") return { route: "echo", ok: r.status === 200, why: "POST 본문 읽고 에코" };
    if (method !== "GET") return { route: "worker", ok: false, why: "GET·POST가 아니라서 워커에서 바로 거절" };
    return { route: "static", ok: r.status === 200, why: "serve_static → ./www" + uri.split("?")[0] };
  }

  async function show(method, uri, r) {
    const c = classify(method, uri, r);
    const route = ROUTES[c.route];
    const end = route.nodes[route.nodes.length - 1];
    $("#flow-caption").textContent = `${method} ${uri}  →  ${c.why}  →  ${r.status || "ERR"} ${r.reason}`;

    route.nodes.forEach((n) => mark(n, "hit", true));
    await moveDot(route.pts, "dot-req", 1.4);
    const res = c.ok ? "ok" : "fail";
    mark(end, res, true);
    await moveDot([...route.pts].reverse(), c.ok ? "dot-ok" : "dot-fail", 1.8);
    route.nodes.forEach((n) => mark(n, "hit", false));
    await sleep(500);
    mark(end, res, false);
  }

  $("#flow-pin").addEventListener("change", (e) => $("#flow").classList.toggle("sticky", e.target.checked));
  return { show };
})();

// 페이지를 연 것 자체가 GET / 요청이었다는 걸 처음에 한 번 보여 준다.
window.addEventListener("load", () => {
  flow.show("GET", "/", { status: 200, reason: "OK", text: "" });
});

/* ───────────── 1. 정적 파일 + MIME ───────────── */

const STATIC_FILES = [
  ["/", "www/index.html"],
  ["/style.css", "www/style.css"],
  ["/script.js", "www/script.js"],
  ["/images/dot.png", "www/images/dot.png"],
  ["/sample.txt", "www/sample.txt"],
];

$("#btn-static").addEventListener("click", (e) => withButton(e.target, async () => {
  const body = $("#static-body");
  body.innerHTML = "";
  for (const [path, file] of STATIC_FILES) {
    const r = await send("GET", path);
    body.insertAdjacentHTML("beforeend",
      `<tr><td>${path}</td><td>${file}</td><td>${statusTag(r)}</td>` +
      `<td>${esc(r.type)}</td><td>${r.length}</td><td>${r.ms.toFixed(1)} ms</td></tr>`);
    await sleep(250);
  }
}));

/* ───────────── 2. serve_static 7단계 ───────────── */

const STEP_NAMES = ["쿼리 자르기", "경로 검사", "경로 만들기", "stat(path)", "일반 파일인가?", "open(path)", "헤더 + 본문 전송"];

// 응답 상태 코드로 serve_static의 어느 단계까지 통과했는지 거꾸로 맞춰 본다.
function staticSteps(sent, r) {
  const q = sent.indexOf("?");
  const path = q >= 0 ? sent.slice(0, q) : sent;
  const fsPath = "./www" + path + (path.endsWith("/") ? "index.html" : "");
  const S = STEP_NAMES.map((name) => ({ name, state: "skip", detail: "여기까지 오지 않음" }));
  const pass = (i, d) => { S[i].state = "pass"; S[i].detail = d; };
  const fail = (i, d) => { S[i].state = "fail"; S[i].detail = d; };

  pass(0, q >= 0 ? `"${sent.slice(q)}" 잘라 냄 → ${path}` : "쿼리 없음 → 그대로");
  if (!path.startsWith("/") || path.includes("..")) { fail(1, "'..' 발견 → 403 Forbidden"); return S; }
  pass(1, "'/'로 시작하고 '..' 없음 → 통과");
  if (r.status === 414) { fail(2, "경로가 너무 김 → 414"); return S; }
  pass(2, `./www + ${path}${path.endsWith("/") ? " + index.html" : ""} = ${fsPath}`);
  if (r.status === 404) { fail(3, `${fsPath} 가 없음 (ENOENT) → 404 Not Found`); return S; }
  pass(3, r.status === 200 ? `있음 · st_size = ${r.length}바이트` : "무언가 있음");
  if (r.status === 403) { fail(4, "S_ISREG 거짓: 폴더 같은 것 → 403 Forbidden"); return S; }
  pass(4, "S_ISREG 참: 일반 파일");
  if (r.status !== 200) { fail(5, `열기 실패 → ${r.status} ${r.reason}`); return S; }
  pass(5, "O_RDONLY로 열기 성공 → 파일 fd");
  pass(6, `200 OK · ${r.type} · 8KB씩 read → write_all`);
  return S;
}

function sentNote(input, url) {
  const sent = url.pathname + url.search;
  if (url.hash) return `<span class="warn"><code>${esc(url.hash)}</code>(fragment)는 브라우저 안에서만 쓰고 서버로 보내지 않습니다.</span>`;
  if (input === sent) return "";
  if (input.includes("..")) return `<span class="warn">브라우저가 <code>..</code>을 먼저 정리해서 보냈습니다. 그래서 2단계에 걸릴 일이 없고, 서버의 403 차단은 터미널에서 <code>curl --path-as-is</code>로 확인합니다.</span>`;
  if (/%[0-9A-F]{2}/.test(sent)) return `<span class="warn">브라우저가 공백·한글을 <code>%</code> 인코딩해서 보냈습니다. 우리 서버는 인코딩을 풀지 않아서 그 이름 그대로 파일을 찾습니다.</span>`;
  return `<span class="warn">브라우저가 경로를 정리해서 보냈습니다.</span>`;
}

async function runSteps() {
  const input = $("#steps-uri").value.trim() || "/";
  const url = new URL(input, location.origin);
  const sent = url.pathname + url.search;
  const list = $("#steps-list");
  $("#steps-sent").innerHTML = `서버가 받은 URI: <code>GET ${esc(sent)}</code> ${sentNote(input, url)}`;

  if (isProxyPath(sent)) {
    list.innerHTML = `<li class="on final"><span class="mark">→</span><span>이 경로는 라우트 규칙(<code>${PROXY_PREFIXES.find((p) => sent.startsWith(p))}</code>)에 맞아서 serve_static이 아니라 <b>proxy_forward</b>로 갑니다.</span></li>`;
    await send("GET", sent);
    return;
  }

  const r = await send("GET", sent);
  const steps = staticSteps(sent, r);
  list.innerHTML = steps.map((s, i) =>
    `<li class="${s.state === "skip" ? "" : s.state}"><span class="mark">${s.state === "pass" ? "✓" : s.state === "fail" ? "✗" : "–"}</span>` +
    `<span class="name">${i + 1}. ${s.name}</span><span class="detail">${esc(s.detail)}</span></li>`).join("") +
    `<li class="final"><span class="mark" style="background:var(--primary)">=</span><span>결과 ${statusTag(r)}</span></li>`;
  const items = list.querySelectorAll("li");
  for (const li of items) {
    li.classList.add("on");
    if (li.classList.contains("fail")) { items[items.length - 1].classList.add("on"); break; }
    await sleep(180);
  }
}

$("#btn-steps").addEventListener("click", (e) => withButton(e.target, runSteps));
$("#steps-uri").addEventListener("keydown", (e) => { if (e.key === "Enter") $("#btn-steps").click(); });
document.querySelectorAll(".preset").forEach((b) => b.addEventListener("click", () => {
  $("#steps-uri").value = b.dataset.uri;
  $("#btn-steps").click();
}));

/* ───────────── 3. 오류 응답 ───────────── */

const WHY = {
  403: "stat은 성공했지만 일반 파일이 아님(폴더) → S_ISREG 실패",
  404: "stat 실패: 루트 디렉토리에 그런 파일이 없음",
  501: "GET, POST 말고는 구현하지 않음",
  502: "프록시가 백엔드에 connect 하지 못함",
  0: "서버에 연결하지 못함 (서버가 꺼져 있나요?)",
};

document.querySelectorAll(".btn-error").forEach((btn) => {
  btn.addEventListener("click", () => withButton(btn, async () => {
    const r = await send(btn.dataset.method, btn.dataset.path);
    $("#error-list").insertAdjacentHTML("afterbegin",
      `<li>${statusTag(r)} <span>${btn.dataset.method} ${btn.dataset.path}</span>` +
      `<span class="why">${WHY[r.status] || ""}</span></li>`);
  }));
});

/* ───────────── 4. 리버스 프록시 + Round-Robin ───────────── */

function addChip(r) {
  const track = $("#rr-track");
  track.querySelector(".empty")?.remove();
  const m = r.text.match(/백엔드 ([AB])/);
  const port = r.text.match(/\((\d+)\)/);
  if (r.status === 200 && m) {
    track.insertAdjacentHTML("beforeend",
      `<span class="chip ${m[1]}">${m[1]}<small>${port ? port[1] : ""} · ${r.ms.toFixed(0)}ms</small></span>`);
  } else {
    track.insertAdjacentHTML("beforeend",
      `<span class="chip X">${r.status || "ERR"}<small>${r.status === 502 ? "백엔드 없음" : r.reason}</small></span>`);
  }
}

$("#btn-rr").addEventListener("click", (e) => withButton(e.target, async () => {
  for (let i = 0; i < 6; i++) {
    addChip(await send("GET", "/api/hello.txt"));
    await sleep(350);   // 흐름도에서 A, B로 번갈아 가는 게 보이도록 간격을 둔다
  }
}));

$("#btn-bonly").addEventListener("click", (e) => withButton(e.target, async () => {
  addChip(await send("GET", "/b/only.txt"));
}));

$("#btn-rr-clear").addEventListener("click", () => {
  $("#rr-track").innerHTML = '<span class="empty">버튼을 눌러 보세요</span>';
});

/* ───────────── 5. POST 본문 ───────────── */

function updateBytes() {
  const text = $("#post-text").value;
  const bytes = new TextEncoder().encode(text).length;
  $("#post-bytes").textContent = `글자 ${[...text].length}개 · UTF-8로 ${bytes}바이트 → Content-Length: ${bytes}`;
}
$("#post-text").addEventListener("input", updateBytes);
updateBytes();

$("#btn-post").addEventListener("click", (e) => withButton(e.target, async () => {
  const r = await send("POST", "/echo", $("#post-text").value);
  $("#post-result").innerHTML =
    `${statusTag(r)} 받은 Content-Length: <b>${r.length}</b> · ${r.ms.toFixed(1)} ms` +
    `<pre>${esc(r.text)}</pre>`;
}));

/* ───────────── 6. 동시 요청 + TCP 연결 타임라인 ───────────── */

let burstRun = 0;

$("#btn-burst").addEventListener("click", (e) => withButton(e.target, async () => {
  const n = Math.min(200, Math.max(1, parseInt($("#burst-n").value, 10) || 30));
  const run = ++burstRun;
  performance.setResourceTimingBufferSize(2000);
  performance.clearResourceTimings();

  // 쿼리만 다르게 붙여 브라우저가 요청을 하나로 합치지 않게 한다. 서버는 쿼리를 잘라 내니 같은 파일이다.
  // 흐름도에는 앞의 몇 개만 그린다 (전부 그리면 점이 너무 많아진다).
  const results = await Promise.all(Array.from({ length: n }, (_, i) =>
    send("GET", `/style.css?burst=${run}-${i}`, undefined, { animate: i < 8 })));
  await sleep(50);

  const entries = performance.getEntriesByType("resource")
    .filter((en) => en.name.includes(`burst=${run}-`))
    .sort((x, y) => x.startTime - y.startTime);
  const ok = results.filter((r) => r.status === 200).length;
  if (entries.length === 0) {
    $("#burst-summary").innerHTML = `요청 <b>${n}</b>개 중 <b>${ok}</b>개 200 OK (이 브라우저는 연결 시간 정보를 주지 않습니다)`;
    return;
  }

  const t0 = Math.min(...entries.map((en) => en.startTime));
  const t1 = Math.max(...entries.map((en) => en.responseEnd));
  const span = Math.max(1, t1 - t0);
  const pct = (t) => ((t - t0) / span) * 100;
  const isNew = (en) => en.connectEnd > en.connectStart;
  const fresh = entries.filter(isNew).length;

  $("#burst-summary").innerHTML =
    `요청 <b>${n}</b>개 중 <b>${ok}</b>개 200 OK · 전체 <b>${span.toFixed(0)} ms</b> · ` +
    `새 TCP 연결 <b>${fresh}</b>개 · keep-alive 재사용 <b>${entries.length - fresh}</b>개`;

  const seg = (cls, a, b, min = "1px") =>
    b > a || min !== "1px" ? `<i class="${cls}" style="left:${pct(a)}%;width:max(${min},${pct(b) - pct(a)}%)"></i>` : "";

  $("#timeline").innerHTML = entries.map((en, i) => {
    const fresh = isNew(en);
    const sendAt = Math.max(en.requestStart, en.startTime);
    return `<div class="tl-row ${fresh ? "new" : ""}"><span class="lbl">${fresh ? "새 연결" : "#" + (i + 1)}</span>` +
      seg("seg-stall", en.startTime, fresh ? en.connectStart : sendAt) +
      (fresh ? seg("seg-connect", en.connectStart, en.connectEnd, "4px") : "") +
      seg("seg-wait", sendAt, en.responseStart) +
      seg("seg-recv", en.responseStart, en.responseEnd) + "</div>";
  }).join("");
  $("#timeline-axis").innerHTML = `<span>0 ms</span><span>${(span / 2).toFixed(0)} ms</span><span>${span.toFixed(0)} ms</span>`;
}));
