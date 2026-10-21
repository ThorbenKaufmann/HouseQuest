"use strict";
// HouseQuest board — kiosk on the Pi and any browser on the LAN.
// Game rules mirror the ESP panel (HouseQuest/src/main.cpp): same steps, limits
// and last_action format, same retained topics.
//
// Concurrent writes (ESP + Pi, or two browsers): every player state carries
// "rev" (incremented per write) and "by" (writer id). One write per player is
// in flight at a time. If another writer's message with the same rev arrives
// after our own echo, ours was overwritten — the operation is re-applied on top
// of the newer state.

const XP_STEP   = 5;
const HP_STEP   = 10;
const SETTLE_MS = 1000;    // a racing write from another panel shows up within this window
const ECHO_MS   = 4000;    // no echo by then → give up on that write
const IDLE_MS   = 90000;   // overlays close (and admin locks) after this long without touch

const T_PLAYERS  = "house/quest/players/";
const T_SETTINGS = "house/quest/settings";
const T_AWARDS   = "house/quest/awards";

const $   = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

const store = {
  get(k)    { try { return JSON.parse(localStorage.getItem(k)); } catch (e) { return null; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* private mode */ } },
  del(k)    { try { localStorage.removeItem(k); } catch (e) { /* private mode */ } },
};

let cfg      = null;          // { user, password, pin, host, port, client }
let client   = null;
let clientId = "";
let level    = null;
let award    = null;
const players = new Map();    // key → state
const pending = new Map();    // key → { queue: [op], inflight: { rev, op, echoed, t } }

let sections = [];


// =============================================================================
// Game rules (as on the ESP)
// =============================================================================

const clampHp = hp => Math.max(-100, Math.min(100, hp));
const signed  = d  => (d >= 0 ? "+" : "") + d;

const opXp = d => s => ({ ...s, xp: s.xp + d, last_action: `xp${signed(d)}` });
const opHp = d => s => ({ ...s, hp: clampHp(s.hp + d), last_action: `hp${signed(d)}` });

// Explicit target instead of a toggle, so a re-applied write can't flip it back
const opRole = guest => s => {
  if (guest && s.hp >= 0)  return { ...s, hp: -1, last_action: "demote" };
  if (!guest && s.hp < 0)  return { ...s, hp: 0,  last_action: "promote" };
  return s;
};

const opRule = r => s => {
  let act;
  if (r.hp > 0 && r.xp > 0) act = `hp-${r.hp},xp+${r.xp}:${r.id}`;
  else if (r.hp > 0)        act = `hp-${r.hp}:${r.id}`;
  else if (r.xp > 0)        act = `xp+${r.xp}:${r.id}`;
  else                      act = `rule:${r.id}`;
  return { ...s,
           hp: r.hp > 0 ? clampHp(s.hp - r.hp) : s.hp,
           xp: r.xp > 0 ? Math.min(999, s.xp + r.xp) : s.xp,
           last_action: act };
};

function describeAction(a) {
  if (!a) return "";
  if (a === "demote")  return "→ Gast";
  if (a === "promote") return "→ Mitbewohner";
  const [deltas, rule] = a.split(":");
  const parts = deltas === "rule" ? [] : deltas.split(",").map(d => {
    const m = /^(hp|xp)([+-]\d+)$/.exec(d);
    return m ? `${m[2].replace("-", "−")} ${m[1].toUpperCase()}` : d;
  });
  if (rule) parts.push(`Regel ${rule}`);
  return parts.join(" · ");
}

const num = (v, dflt) => Number.isFinite(+v) && v !== null && v !== "" ? Math.trunc(+v) : dflt;

function normalize(key, raw) {
  return { ...raw,
           name: raw.name || key,
           xp: num(raw.xp, 0),
           hp: num(raw.hp, 100),
           last_action: raw.last_action || "",
           rev: num(raw.rev, 0) };
}


// =============================================================================
// Writes with revision check
// =============================================================================

function pendingFor(key) {
  if (!pending.has(key)) pending.set(key, { queue: [], inflight: null });
  return pending.get(key);
}

// Returns false when offline (nothing queued then).
function submit(key, op) {
  if (!client || !client.connected || !players.has(key)) return false;
  pendingFor(key).queue.push(op);
  pump(key);
  return true;
}

function pump(key) {
  const p = pendingFor(key);
  if (p.inflight && !p.inflight.echoed) return;   // one write in flight per player
  const op   = p.queue.shift();
  const base = players.get(key);
  if (!op || !base) return;

  const next = op({ ...base });
  next.role = next.hp >= 0 ? "resident" : "guest";
  next.rev  = base.rev + 1;
  next.by   = clientId;
  p.inflight = { rev: next.rev, op, echoed: false, t: Date.now() };
  client.publish(T_PLAYERS + key + "/state", JSON.stringify(next), { qos: 1, retain: true });
}

function onPlayerState(key, st) {
  players.set(key, st);
  const p = pending.get(key);
  const f = p && p.inflight;
  if (f) {
    if (st.by === clientId && st.rev === f.rev) {
      f.echoed = true;
      f.t = Date.now();
    } else if (st.by !== clientId) {
      if (st.rev === f.rev && f.echoed) {
        // Same revision from someone else after ours → ours got overwritten
        p.inflight = null;
        p.queue.unshift(f.op);
      } else if (st.rev > f.rev) {
        p.inflight = null;
      }
    }
    pump(key);
  }
}

setInterval(() => {
  const now = Date.now();
  for (const [key, p] of pending) {
    const f = p.inflight;
    if (!f) continue;
    if (f.echoed ? now - f.t > SETTLE_MS : now - f.t > ECHO_MS) {
      if (!f.echoed) { p.queue = []; adminMessage("Keine Bestätigung vom Broker – bitte prüfen."); }
      p.inflight = null;
      pump(key);
    }
  }
}, 250);


// =============================================================================
// MQTT
// =============================================================================

function setConn(state) {
  $("conn").className = "dot " + (state === "ok" ? "ok" : state === "bad" ? "bad" : "");
  $("conn").title = { ok: "Verbunden", bad: "Nicht verbunden", wait: "Verbinde…" }[state];
}

function connect() {
  if (client) client.end(true);
  players.clear(); pending.clear(); level = null; award = null;
  renderBoard();

  const host = cfg.host || location.hostname || "localhost";
  const port = cfg.port || 9001;
  const url  = `${location.protocol === "https:" ? "wss" : "ws"}://${host}:${port}`;
  setConn("wait");
  $("boardMsg").textContent = "Verbinde…";

  client = mqtt.connect(url, {
    username: cfg.user, password: cfg.password,
    clientId: clientId + "-" + Math.random().toString(16).slice(2, 8),
    reconnectPeriod: 3000, connectTimeout: 8000,
  });
  client.brokerUrl = url;

  client.on("connect", () => {
    setConn("ok");
    if (level === null) level = 1;   // ESP default until the retained settings arrive
    client.subscribe([T_PLAYERS + "+/state", T_SETTINGS, T_AWARDS], { qos: 1 });
    $("boardMsg").textContent = "Noch keine Spieler.";
    renderBoard();
  });
  client.on("close", () => { setConn("bad"); renderBoard(); });
  client.on("error", err => {
    // 4/5 = bad credentials / not authorized: retrying won't help
    if (err && (err.code === 4 || err.code === 5 || err.code === 134 || err.code === 135)) {
      client.end(true);
      setConn("bad");
      store.del("hq.login");
      showLogin("Anmeldung abgelehnt – Benutzer/Passwort prüfen.");
    }
  });
  client.on("message", (topic, payload) => {
    const text = payload.toString();
    let doc = null;
    if (text) { try { doc = JSON.parse(text); } catch (e) { return; } }

    if (topic.startsWith(T_PLAYERS)) {
      const key = topic.split("/")[3];
      if (doc) onPlayerState(key, normalize(key, doc));
      else     players.delete(key);           // retained message deleted
    } else if (topic === T_SETTINGS) {
      level = doc ? num(doc.level, 1) : 1;
    } else if (topic === T_AWARDS) {
      award = doc && doc.title ? doc : null;
    }
    renderBoard();
    renderAdmin();
    if (!$("rules").hidden) renderRules();
  });
}


// =============================================================================
// Board
// =============================================================================

function hpColor(hp) {
  return hp < 15 ? "var(--hp-crit)" : hp < 25 ? "var(--hp-low)" : hp < 75 ? "var(--hp-mid)" : "var(--hp-hi)";
}

const sortedPlayers = () => [...players.entries()].sort((a, b) => a[1].name.localeCompare(b[1].name, "de"));

function renderBoard() {
  $("level").textContent = level === null ? "Level –" : `Level ${level}`;

  const list = sortedPlayers();
  const n    = list.length;
  const cols = n <= 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4;
  const rows = Math.max(1, Math.ceil(n / cols));
  const el   = $("players");
  el.style.setProperty("--cols", cols);
  el.style.setProperty("--card-scale", rows <= 1 ? "1.6em" : rows === 2 ? "1.2em" : rows === 3 ? "1em" : ".85em");

  el.innerHTML = list.map(([, p]) => {
    const guest = p.hp < 0;
    const width = Math.min(100, Math.abs(p.hp));
    return `<div class="card">
      <div class="head"><span class="name">${esc(p.name)}</span>
        <span class="chip ${guest ? "guest" : ""}">${guest ? "Gast" : "Mitbewohner"}</span></div>
      <div class="stats"><span>XP <b>${p.xp}</b></span><span>HP <b>${p.hp}</b></span></div>
      <div class="hpbar ${guest ? "guest" : ""}"><span style="width:${width}%;background:${guest ? "var(--guest)" : hpColor(p.hp)}"></span></div>
      <div class="last">${esc(describeAction(p.last_action))}</div>
    </div>`;
  }).join("");

  $("boardMsg").hidden = n > 0;
  if (!cfg) $("boardMsg").innerHTML = `Nicht angemeldet. <button class="btn" id="boardLogin">Anmelden</button>`;

  $("award").hidden = !award;
  if (award) {
    $("award").innerHTML = `Haus-Titel: <b>${esc(award.title)}</b>` +
      (award.player ? ` – ${esc(award.player)}` : "") +
      (award.duration ? ` <span class="muted">(${esc(award.duration)})</span>` : "");
  }
}

function updateClock() {
  $("clock").textContent = new Date().toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit" });
}
updateClock();
setInterval(updateClock, 10000);


// =============================================================================
// Rules (viewer and picker)
// =============================================================================

let rulesPick = null;   // callback(rule) when used as picker from admin

function openRules(pick) {
  rulesPick = pick || null;
  $("rulesQ").value = "";
  $("rulesTitle").textContent = pick ? `Regel für ${players.get(admin.target)?.name || "?"}` : "Regeln";
  $("rulesClose").textContent = pick ? "Zurück" : "Schließen";
  $("rules").hidden = false;
  if (pick) $("admin").hidden = true;
  renderRules();
  $("rulesList").scrollTop = 0;
}

function closeRules() {
  $("rules").hidden = true;
  if (rulesPick) { rulesPick = null; $("admin").hidden = false; renderAdmin(); }
}

function renderRules() {
  const q = $("rulesQ").value.trim().toLowerCase();
  const onlyActive = $("rulesOnlyActive").checked && level !== null;
  let html = "", shown = 0;

  // "2.1" → rule-ID prefix; anything else → full-text search
  const byId = /^[\d.]+$/.test(q);
  const matches = (s, r) => !q || (byId ? r.id.startsWith(q)
    : r.t.toLowerCase().includes(q) || s.title.toLowerCase().includes(q));

  const visible = sections.map(s => s.rules.filter(r =>
    (!onlyActive || r.l <= level) && matches(s, r)));

  // A heading is shown if it, or any of its subsections, has visible rules
  const show = new Array(sections.length).fill(false);
  for (let i = sections.length - 1; i >= 0; i--) {
    show[i] = visible[i].length > 0;
    for (let j = i + 1; !show[i] && j < sections.length && sections[j].depth > sections[i].depth; j++)
      show[i] = show[j];
  }

  sections.forEach((s, i) => {
    if (!show[i]) return;
    const h = "h" + Math.min(s.depth + 1, 5);
    html += `<${h}>${esc(s.title.replace(/_/g, ""))}</${h}>`;
    if (!visible[i].length) return;

    html += "<ul>";
    for (const r of visible[i]) {
      const inactive = level !== null && r.l > level;
      const chips = [];
      if (r.hp) chips.push(`<span class="chip hp">−${r.hp} HP</span>`);
      if (r.xp) chips.push(`<span class="chip xp">+${r.xp} XP</span>`);
      chips.push(`<span class="chip">${inactive ? "ab " : ""}Level ${r.l}</span>`);
      const pick = rulesPick && !inactive ? " pick" : "";
      html += `<li class="${inactive ? "inactive" : ""}${pick}" data-id="${esc(r.id)}"><span class="id">${esc(r.id)}</span>` +
              `<span>${esc(r.t)}</span><span class="meta">${chips.join("")}</span></li>`;
      shown++;
    }
    html += "</ul>";
  });
  $("rulesList").innerHTML = shown ? html : `<div class="empty">Keine passenden Regeln.</div>`;

  const total  = sections.reduce((n, s) => n + s.rules.length, 0);
  const active = level === null ? total : sections.reduce((n, s) => n + s.rules.filter(r => r.l <= level).length, 0);
  $("rulesSub").textContent = level === null ? `${total} Regeln` : `Level ${level} · ${active} von ${total} aktiv`;
  $("rulesOnlyActive").parentElement.hidden = level === null;
}

function findRule(id) {
  for (const s of sections) for (const r of s.rules) if (r.id === id) return r;
  return null;
}

$("rulesPad").innerHTML = ["1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "⌫"]
  .map(k => `<button class="btn" data-key="${k}">${k}</button>`).join("");
$("rulesPad").addEventListener("click", e => {
  const k = e.target.closest("[data-key]")?.dataset.key;
  if (!k) return;
  const q = $("rulesQ");
  // Typing digits after a text search starts a fresh ID search
  if (!/^[\d.]*$/.test(q.value)) q.value = "";
  q.value = k === "⌫" ? q.value.slice(0, -1) : q.value + k;
  renderRules();
});
$("rulesQ").addEventListener("input", renderRules);
$("rulesOnlyActive").addEventListener("change", renderRules);
$("rulesClose").addEventListener("click", closeRules);
$("rulesList").addEventListener("click", e => {
  const li = e.target.closest("li.pick");
  if (!li || !rulesPick) return;
  const r = findRule(li.dataset.id);
  const cb = rulesPick;
  closeRules();
  if (r) cb(r);
});
$("btnRules").addEventListener("click", () => openRules());


// =============================================================================
// Admin
// =============================================================================

const admin = { view: "pin", pin: "", pinError: false, target: null, rule: null, msg: "",
                newName: "", newGuest: false, shift: true };

const MAX_PLAYERS = 16;    // ESP panel limit
const NAME_MAX    = 10;    // fits the ESP's player row

// Topic key as the ESP builds it (String::toLowerCase only touches ASCII)
const playerKey = name => name.replace(/[A-Z]/g, c => c.toLowerCase());

const KEYBOARD = ["1234567890", "QWERTZUIOPÜ", "ASDFGHJKLÖÄ", "YXCVBNMß-"];

function createPlayer() {
  const name = admin.newName.trim();
  const key  = playerKey(name);
  if (!name)                         return "Bitte einen Namen eingeben.";
  if (/[\/+#]/.test(name))           return "Name darf kein / + # enthalten.";
  if (players.has(key))              return `${name} gibt es schon.`;
  if (players.size >= MAX_PLAYERS)   return `Mehr als ${MAX_PLAYERS} Spieler zeigt das ESP-Panel nicht an.`;
  if (!client || !client.connected)  return "Offline – nicht gesendet.";
  const state = { name, xp: 0, hp: admin.newGuest ? -1 : 100, role: admin.newGuest ? "guest" : "resident",
                  last_action: "", rev: 1, by: clientId };
  client.publish(T_PLAYERS + key + "/state", JSON.stringify(state), { qos: 1, retain: true });
  return "";
}

function removePlayer(key) {
  if (!client || !client.connected) return false;
  // Empty retained message deletes the player on the broker and on all panels
  client.publish(T_PLAYERS + key + "/state", "", { qos: 1, retain: true });
  pending.delete(key);
  return true;
}

function openAdmin() {
  Object.assign(admin, { view: cfg && cfg.pin ? "pin" : "select", pin: "", pinError: false, msg: "" });
  $("admin").hidden = false;
  renderAdmin();
}

function closeAdmin() {
  $("admin").hidden = true;
  admin.view = "pin";
}

function adminMessage(text) {
  admin.msg = text;
  renderAdmin();
}

const head = (title, buttons) =>
  `<div class="panel-head"><h2>${title}</h2><span class="spacer"></span>${buttons}</div>`;

function renderAdmin() {
  if ($("admin").hidden) return;
  const el = $("adminPanel");
  const p  = admin.target && players.get(admin.target);
  const msg = admin.msg ? `<div class="note">${esc(admin.msg)}</div>` : "";

  if (admin.view === "pin") {
    const dots = "●".repeat(admin.pin.length) + "○".repeat(4 - admin.pin.length);
    el.innerHTML = head("Admin", `<button class="btn" data-act="close">Schließen</button>`) +
      `<div class="pinbox">${dots}</div>
       <div class="error" style="text-align:center">${admin.pinError ? "Falsche PIN" : ""}</div>
       <div class="pad" style="width:min(420px,100%);margin:0 auto">` +
      ["1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "OK"]
        .map(k => `<button class="btn" data-pin="${k}">${k}</button>`).join("") + `</div>`;
  }

  else if (admin.view === "select") {
    const list = sortedPlayers();
    el.innerHTML = head("Spieler wählen",
        `<button class="btn" data-act="settings">Einstellungen</button>
         <button class="btn" data-act="close">Schließen</button>`) + msg +
      (list.length ? `<div class="select">` + list.map(([key, s]) =>
        `<button class="btn" data-player="${esc(key)}">${esc(s.name)}<small>XP ${s.xp} · HP ${s.hp}</small></button>`
      ).join("") + `<button class="btn plus" data-act="add">+ Spieler<small>anlegen</small></button></div>`
      : `<div class="empty">Noch keine Spieler.</div><button class="btn big plus" data-act="add">+ Spieler anlegen</button>`);
  }

  else if (admin.view === "add") {
    const keys = KEYBOARD.map(row => `<div class="kbrow">` + [...row].map(c => {
      const k = admin.shift ? c : c.toLowerCase();
      return `<button class="btn" data-kb="${esc(k)}">${esc(k)}</button>`;
    }).join("") + `</div>`).join("");
    el.innerHTML = head("Spieler anlegen", `<button class="btn" data-act="select">Zurück</button>`) +
      `<input class="namebox" id="newName" maxlength="${NAME_MAX}" value="${esc(admin.newName)}" placeholder="Name" autocomplete="off">
       <div class="kb">${keys}
         <div class="kbrow"><button class="btn ${admin.shift ? "primary" : ""}" data-kb="shift">⇧</button>
           <button class="btn wide" data-kb=" ">Leerzeichen</button><button class="btn" data-kb="del">⌫</button></div></div>
       <div class="row"><button class="btn ${admin.newGuest ? "" : "primary"}" data-act="asResident">Mitbewohner</button>
         <button class="btn ${admin.newGuest ? "primary" : ""}" data-act="asGuest">Gast</button></div>` + msg +
      `<button class="btn big primary" data-act="create">Anlegen</button>`;
    const inp = $("newName");
    inp.addEventListener("input", () => { admin.newName = inp.value; });
  }

  else if (admin.view === "remove") {
    if (!p) { admin.view = "select"; return renderAdmin(); }
    el.innerHTML = head(`${esc(p.name)} entfernen?`, `<button class="btn" data-act="player">Zurück</button>`) +
      `<div class="confirm">Spieler und Spielstand (XP ${p.xp}, HP ${p.hp}) werden auf allen Panels gelöscht.
         Das lässt sich nicht rückgängig machen.</div>` + msg +
      `<div class="row"><button class="btn big" data-act="player">Abbrechen</button>
         <button class="btn big minus" data-act="doRemove">Entfernen</button></div>`;
  }

  else if (admin.view === "player") {
    if (!p) { admin.view = "select"; return renderAdmin(); }
    const guest = p.hp < 0;
    el.innerHTML = head(`${esc(p.name)} <span class="chip ${guest ? "guest" : ""}">${guest ? "Gast" : "Mitbewohner"}</span>`,
        `<button class="btn" data-act="select">Zurück</button>`) +
      `<div class="card" style="font-size:1.2em"><div class="stats"><span>XP <b>${p.xp}</b></span><span>HP <b>${p.hp}</b></span></div>
         <div class="last">${esc(describeAction(p.last_action))}</div></div>
       <div class="actions">
         <button class="btn big minus" data-op="xp-">−${XP_STEP} XP</button>
         <button class="btn big plus"  data-op="xp+">+${XP_STEP} XP</button>
         <button class="btn big minus" data-op="hp-">−${HP_STEP} HP</button>
         <button class="btn big plus"  data-op="hp+">+${HP_STEP} HP</button>
       </div>
       <button class="btn big primary" data-act="byrule" ${sections.length ? "" : "disabled"}>Nach Regel …</button>
       <div class="row"><button class="btn" data-op="role">${guest ? "→ Als Mitbewohner setzen" : "→ Als Gast setzen"}</button>
         <button class="btn minus" data-act="remove">Spieler entfernen</button></div>` + msg;
  }

  else if (admin.view === "confirm") {
    const r = admin.rule;
    if (!p || !r) { admin.view = "player"; return renderAdmin(); }
    const after   = opRule(r)({ ...p });
    const blocked = level !== null && r.l > level;
    const chips = [];
    if (r.hp) chips.push(`<span class="chip hp">−${r.hp} HP</span>`);
    if (r.xp) chips.push(`<span class="chip xp">+${r.xp} XP</span>`);
    chips.push(`<span class="chip">Level ${r.l}</span>`);
    el.innerHTML = head(`Regel ${esc(r.id)} → ${esc(p.name)}`, `<button class="btn" data-act="player">Zurück</button>`) +
      `<div class="confirm"><div>${esc(r.t)}</div><div class="row">${chips.join("")}</div>
         <div class="preview">${esc(p.name)}: HP ${p.hp} → ${after.hp} · XP ${p.xp} → ${after.xp}</div>
         ${blocked ? `<div class="note">Erst ab Level ${r.l} anwendbar (aktuell ${level}).</div>` : ""}</div>` + msg +
      `<div class="row"><button class="btn big" data-act="byrule">Andere Regel</button>
         <button class="btn big primary" data-act="apply" ${blocked ? "disabled" : ""}>Anwenden</button></div>`;
  }

  else if (admin.view === "settings") {
    el.innerHTML = head("Einstellungen", `<button class="btn" data-act="select">Zurück</button>`) +
      `<div class="muted">Spiel-Level – Regeln über diesem Level sind nicht anwendbar.</div>
       <div class="levelbig">${level === null ? "–" : level}</div>
       <div class="row"><button class="btn big minus" data-act="lvl-">Level −</button>
         <button class="btn big plus" data-act="lvl+">Level +</button></div>` + msg +
      `<div class="muted">Broker: ${esc(client ? client.brokerUrl : "–")} · ${client && client.connected ? "verbunden" : "nicht verbunden"}<br>
         Gerät: ${esc(clientId)}</div>` +
      (cfg && cfg.fromKiosk ? "" : `<button class="btn" data-act="logout">Abmelden (Login in diesem Browser löschen)</button>`);
  }
}

$("adminPanel").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (!b || b.disabled) return;
  const d = b.dataset;
  admin.msg = "";

  if (d.pin) {
    if (d.pin === "⌫")                           admin.pin = admin.pin.slice(0, -1);
    else if (d.pin !== "OK" && admin.pin.length < 4) admin.pin += d.pin;
    admin.pinError = false;
    // Checked at 4 digits (as on the ESP) or on OK
    if (admin.pin.length === 4 || (d.pin === "OK" && admin.pin)) {
      if (admin.pin === cfg.pin) admin.view = "select";
      else admin.pinError = true;
      admin.pin = "";
    }
  }
  else if (d.player) { admin.target = d.player; admin.view = "player"; }
  else if (d.kb) {
    if (d.kb === "shift")    admin.shift = !admin.shift;
    else if (d.kb === "del") admin.newName = admin.newName.slice(0, -1);
    else if (admin.newName.length < NAME_MAX) {
      admin.newName += d.kb;
      admin.shift = false;   // capital first letter, then lower case
    }
  }
  else if (d.op) {
    const p  = players.get(admin.target);
    const op = { "xp-": opXp(-XP_STEP), "xp+": opXp(XP_STEP), "hp-": opHp(-HP_STEP), "hp+": opHp(HP_STEP),
                 "role": opRole(p && p.hp >= 0) }[d.op];
    if (!submit(admin.target, op)) admin.msg = "Offline – nicht gesendet.";
  }
  else switch (d.act) {
    case "close":    closeAdmin(); return;
    case "select":   admin.view = "select"; break;
    case "settings": admin.view = "settings"; break;
    case "player":   admin.view = "player"; break;
    case "add":
      Object.assign(admin, { view: "add", newName: "", newGuest: false, shift: true });
      break;
    case "asResident": admin.newGuest = false; break;
    case "asGuest":    admin.newGuest = true;  break;
    case "create": {
      const err = createPlayer();
      if (err) { admin.msg = err; break; }
      admin.target = playerKey(admin.newName.trim());
      admin.msg    = `${admin.newName.trim()} angelegt.`;
      admin.view   = "select";
      break;
    }
    case "remove":   admin.view = "remove"; break;
    case "doRemove": {
      const name = players.get(admin.target)?.name;
      if (!removePlayer(admin.target)) { admin.msg = "Offline – nicht gesendet."; break; }
      admin.msg  = `${name} entfernt.`;
      admin.view = "select";
      break;
    }
    case "byrule":
      openRules(r => { admin.rule = r; admin.view = "confirm"; renderAdmin(); });
      return;
    case "apply":
      if (!submit(admin.target, opRule(admin.rule))) { admin.msg = "Offline – nicht gesendet."; break; }
      admin.msg  = `Regel ${admin.rule.id} angewendet.`;
      admin.view = "player";
      break;
    case "lvl-":
    case "lvl+": {
      if (!client || !client.connected) { admin.msg = "Offline – nicht gesendet."; break; }
      const cur  = level === null ? 1 : level;
      const next = d.act === "lvl-" ? Math.max(0, cur - 1) : Math.min(9, cur + 1);
      if (next !== cur) client.publish(T_SETTINGS, JSON.stringify({ level: next }), { qos: 1, retain: true });
      break;
    }
    case "logout":
      store.del("hq.login");
      if (client) client.end(true);
      client = null; cfg = null;
      closeAdmin();
      setConn("bad");
      players.clear();
      renderBoard();
      showLogin();
      return;
  }
  renderAdmin();
});
$("btnAdmin").addEventListener("click", () => cfg ? openAdmin() : showLogin());


// =============================================================================
// Login / config
// =============================================================================

function showLogin(error) {
  $("loginError").textContent = error || "";
  $("login").hidden = false;
}

$("loginForm").addEventListener("submit", e => {
  e.preventDefault();
  const f = new FormData(e.target);
  const login = { user: f.get("user"), password: f.get("password"), pin: f.get("pin") || "" };
  store.set("hq.login", login);
  cfg = login;
  $("login").hidden = true;
  e.target.reset();
  connect();
});
$("loginCancel").addEventListener("click", () => { $("login").hidden = true; renderBoard(); });
$("board").addEventListener("click", e => { if (e.target.id === "boardLogin") showLogin(); });

// Overlays close after a while without touch; admin needs the PIN again
let lastTouch = Date.now();
document.addEventListener("pointerdown", () => { lastTouch = Date.now(); }, true);
setInterval(() => {
  if (Date.now() - lastTouch < IDLE_MS) return;
  if (!$("admin").hidden || !$("rules").hidden) {
    rulesPick = null;
    $("rules").hidden = true;
    closeAdmin();
  }
}, 5000);

async function init() {
  // Kiosk on the Pi: login comes from the server (loopback only)
  let kiosk = null;
  try {
    const r = await fetch("config.json");
    if (r.ok) kiosk = await r.json();
  } catch (e) { /* not on the Pi */ }

  clientId = (kiosk && kiosk.client) || store.get("hq.client");
  if (!clientId) {
    clientId = "web-" + Math.random().toString(16).slice(2, 8);
    store.set("hq.client", clientId);
  }

  try {
    sections = (await fetch("rules.json").then(r => r.json())).sections;
  } catch (e) {
    $("btnRules").disabled = true;
  }

  if (kiosk && kiosk.mqtt) {
    cfg = { ...kiosk.mqtt, pin: kiosk.pin || "", fromKiosk: true };
  } else {
    cfg = store.get("hq.login");
  }

  if (cfg) connect();
  else { setConn("bad"); renderBoard(); showLogin(); }
}

init();
