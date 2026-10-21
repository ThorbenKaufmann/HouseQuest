#!/usr/bin/env python3
"""HouseQuest game service for the Pi — claims (tasks) on a time-boxed plan.

The database is the source of truth; MQTT is only the interface:
  house/quest/cmd/<client>    request  {"id": "...", "op": "...", ...}
  house/quest/reply/<client>  answer   {"id": "...", "ok": true|false, "error": "..."}
  house/quest/catalog         retained view of the task templates
  house/quest/plan            retained view of the current plan and its claims
Requests are handled one at a time, so two players can't claim the same task.

Claim states:  open → claimed → review → done
                       ▲          │
                       └─reject───┘      cancel (admin) / plan end → cancelled / expired
XP is booked on approval; a claimed task that is cancelled or still not done
when the plan ends costs its HP. Tasks waiting for review survive the plan end.

Environment:
  HOUSEQUEST_DB              SQLite file           (default: game.db next to this file)
  HOUSEQUEST_MQTT_HOST/PORT  broker                (default: localhost:1883)
  CREDENTIALS_DIRECTORY/mqtt.json   {"mqtt": {"user": ..., "password": ...}}  (systemd LoadCredential)
"""

import datetime
import json
import os
import sqlite3
import time
from pathlib import Path

T_CMD     = "house/quest/cmd/"
T_REPLY   = "house/quest/reply/"
T_PLAN    = "house/quest/plan"
T_CATALOG = "house/quest/catalog"
T_PLAYERS = "house/quest/players/"

WRITER_ID = "game"
ECHO_S    = 4.0   # player state writes: give up without echo
SETTLE_S  = 1.0   # racing writes from panels show up within this window

TITLE_MAX = 30
PLAN_MAX_HOURS = 24

SCHEMA = """
CREATE TABLE IF NOT EXISTS template (
  id      INTEGER PRIMARY KEY,
  title   TEXT    NOT NULL,
  xp      INTEGER NOT NULL DEFAULT 0,
  hp      INTEGER NOT NULL DEFAULT 0,
  active  INTEGER NOT NULL DEFAULT 1,
  created TEXT    NOT NULL
);
CREATE TABLE IF NOT EXISTS plan (
  id     INTEGER PRIMARY KEY,
  starts TEXT NOT NULL,
  ends   TEXT NOT NULL,
  closed TEXT
);
-- title/xp/hp are copied from the template: later catalog edits don't rewrite history
CREATE TABLE IF NOT EXISTS claim (
  id          INTEGER PRIMARY KEY,
  plan_id     INTEGER NOT NULL REFERENCES plan(id),
  template_id INTEGER REFERENCES template(id),
  title       TEXT    NOT NULL,
  xp          INTEGER NOT NULL,
  hp          INTEGER NOT NULL,
  player      TEXT,
  state       TEXT    NOT NULL,
  assigned    INTEGER NOT NULL DEFAULT 0,
  created     TEXT    NOT NULL,
  claimed     TEXT,
  submitted   TEXT,
  decided     TEXT
);
CREATE INDEX IF NOT EXISTS claim_plan  ON claim(plan_id);
CREATE INDEX IF NOT EXISTS claim_state ON claim(state);
CREATE TABLE IF NOT EXISTS event (
  id       INTEGER PRIMARY KEY,
  ts       TEXT NOT NULL,
  plan_id  INTEGER,
  claim_id INTEGER,
  action   TEXT NOT NULL,
  player   TEXT,
  actor    TEXT
);
"""


def now():
    return datetime.datetime.now().astimezone()

def iso(t):
    return t.isoformat(timespec="seconds")


class GameError(Exception):
    """Refused request; the message is shown on the panel."""


# =============================================================================
# Player states (retained MQTT topics, shared with ESP and board)
# =============================================================================

class Players:
    """Cache of house/quest/players/<key>/state plus writes with the panels'
    rev/by scheme: one write per player in flight; if another writer's message
    with the same rev arrives after our echo, the operation is re-applied."""

    def __init__(self, publish):
        self.publish = publish          # publish(topic, payload_str)
        self.states  = {}               # key → state dict
        self.pending = {}               # key → {"queue": [op], "inflight": {...} | None}

    def known(self, key):
        return key in self.states

    def name(self, key):
        return self.states.get(key, {}).get("name", key)

    def on_message(self, key, payload):
        if not payload:
            self.states.pop(key, None)
            self.pending.pop(key, None)
            return
        try:
            raw = json.loads(payload)
        except ValueError:
            return
        st = dict(raw)
        st["name"] = raw.get("name") or key
        st["xp"]   = _int(raw.get("xp"), 0)
        st["hp"]   = _int(raw.get("hp"), 100)
        st["rev"]  = _int(raw.get("rev"), 0)
        self.states[key] = st

        p = self.pending.get(key)
        f = p and p["inflight"]
        if not f:
            return
        if raw.get("by") == WRITER_ID and st["rev"] == f["rev"]:
            f["echoed"], f["t"] = True, time.monotonic()
        elif raw.get("by") != WRITER_ID:
            if st["rev"] == f["rev"] and f["echoed"]:
                # Same revision from a panel after ours → ours got overwritten
                p["inflight"] = None
                p["queue"].insert(0, f["op"])
            elif st["rev"] > f["rev"]:
                p["inflight"] = None
        self._pump(key)

    def write(self, key, op):
        self.pending.setdefault(key, {"queue": [], "inflight": None})["queue"].append(op)
        self._pump(key)

    def tick(self):
        t = time.monotonic()
        for key, p in self.pending.items():
            f = p["inflight"]
            if f and t - f["t"] > (SETTLE_S if f["echoed"] else ECHO_S):
                p["inflight"] = None
                self._pump(key)

    def _pump(self, key):
        p = self.pending.get(key)
        if not p or (p["inflight"] and not p["inflight"]["echoed"]) or not p["queue"]:
            return
        base = self.states.get(key)
        if not base:
            p["queue"].clear()          # player deleted meanwhile
            return
        op  = p["queue"].pop(0)
        new = op(dict(base))
        new["role"] = "resident" if new["hp"] >= 0 else "guest"
        new["rev"]  = base["rev"] + 1
        new["by"]   = WRITER_ID
        p["inflight"] = {"rev": new["rev"], "op": op, "echoed": False, "t": time.monotonic()}
        self.publish(T_PLAYERS + key + "/state", json.dumps(new))


def _int(v, default):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default

def clamp_hp(hp):
    return max(-100, min(100, hp))

def op_task_xp(xp, title):
    def op(s):
        s["xp"] = min(999, s["xp"] + xp)
        s["last_action"] = f"xp+{xp}:task:{title}" if xp > 0 else f"task:{title}"
        return s
    return op

def op_task_hp(hp, title):
    def op(s):
        s["hp"] = clamp_hp(s["hp"] - hp)
        s["last_action"] = f"hp-{hp}:task:{title}"
        return s
    return op


# =============================================================================
# Game logic (no MQTT here — testable on its own)
# =============================================================================

class Game:
    def __init__(self, db_path, players):
        self.db = sqlite3.connect(db_path)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys = ON")
        self.db.executescript(SCHEMA)
        self.players = players

    # ── Views (published retained) ───────────────────────────────────────────

    def catalog_view(self):
        rows = self.db.execute("SELECT id, title, xp, hp, active FROM template ORDER BY title COLLATE NOCASE")
        return {"templates": [dict(r) | {"active": bool(r["active"])} for r in rows]}

    def plan_view(self):
        plan = self.active_plan()
        # Current plan's claims, plus anything still waiting for review from earlier plans
        rows = self.db.execute(
            "SELECT id, plan_id, title, xp, hp, player, state, assigned, claimed, submitted, decided "
            "FROM claim WHERE plan_id = ? OR state = 'review' ORDER BY id",
            (plan["id"] if plan else -1,))
        return {
            "plan": dict(plan) if plan else None,
            "claims": [dict(r) | {"assigned": bool(r["assigned"])} for r in rows],
        }

    def active_plan(self):
        return self.db.execute("SELECT id, starts, ends FROM plan WHERE closed IS NULL "
                               "ORDER BY id DESC LIMIT 1").fetchone()

    # ── Requests ─────────────────────────────────────────────────────────────

    def handle(self, cmd, actor=""):
        """Runs one request. Returns True if the plan/catalog views changed."""
        op = cmd.get("op", "")
        fn = getattr(self, "op_" + op.replace(".", "_"), None)
        if not fn:
            raise GameError(f"Unbekannte Anfrage: {op}")
        with self.db:
            bookings = fn(cmd, actor) or []
        # Book XP/HP only after the commit
        for key, player_op in bookings:
            self.players.write(key, player_op)
        return True

    def _claim(self, cmd):
        row = self.db.execute("SELECT * FROM claim WHERE id = ?", (_int(cmd.get("claim"), -1),)).fetchone()
        if not row:
            raise GameError("Aufgabe nicht gefunden.")
        return row

    def _event(self, action, claim=None, plan_id=None, player=None, actor=""):
        self.db.execute("INSERT INTO event (ts, plan_id, claim_id, action, player, actor) VALUES (?,?,?,?,?,?)",
                        (iso(now()), claim["plan_id"] if claim else plan_id, claim["id"] if claim else None,
                         action, player if player is not None else (claim["player"] if claim else None), actor))

    def _set(self, claim, **fields):
        cols = ", ".join(f"{k} = ?" for k in fields)
        self.db.execute(f"UPDATE claim SET {cols} WHERE id = ?", (*fields.values(), claim["id"]))

    def op_claim(self, cmd, actor):
        c      = self._claim(cmd)
        player = str(cmd.get("player", ""))
        if c["state"] != "open":
            who = self.players.name(c["player"]) if c["player"] else "jemand anderes"
            raise GameError(f"„{c['title']}“ hat schon {who} übernommen.")
        if not self.active_plan() or self.active_plan()["id"] != c["plan_id"]:
            raise GameError("Der Plan ist schon beendet.")
        if not self.players.known(player):
            raise GameError("Spieler unbekannt.")
        self._set(c, state="claimed", player=player, claimed=iso(now()))
        self._event("claim", c, player=player, actor=actor)

    def op_submit(self, cmd, actor):
        c      = self._claim(cmd)
        player = str(cmd.get("player", ""))
        if c["state"] != "claimed" or c["player"] != player:
            raise GameError(f"„{c['title']}“ kann nur {self.players.name(c['player'])} als erledigt melden."
                            if c["player"] and c["state"] == "claimed" else "Diese Aufgabe ist nicht offen.")
        self._set(c, state="review", submitted=iso(now()))
        self._event("submit", c, actor=actor)

    def op_approve(self, cmd, actor):
        c = self._claim(cmd)
        if c["state"] != "review":
            raise GameError("Diese Aufgabe wartet nicht auf Bestätigung.")
        if not self.players.known(c["player"]):
            raise GameError(f"Spieler {c['player']} gibt es nicht mehr.")
        self._set(c, state="done", decided=iso(now()))
        self._event("approve", c, actor=actor)
        return [(c["player"], op_task_xp(c["xp"], c["title"]))]

    def op_reject(self, cmd, actor):
        c = self._claim(cmd)
        if c["state"] != "review":
            raise GameError("Diese Aufgabe wartet nicht auf Bestätigung.")
        self._set(c, state="claimed", submitted=None)
        self._event("reject", c, actor=actor)

    def op_cancel(self, cmd, actor):
        c = self._claim(cmd)
        if c["state"] not in ("open", "claimed", "review"):
            raise GameError("Diese Aufgabe ist schon abgeschlossen.")
        self._set(c, state="cancelled", decided=iso(now()))
        self._event("cancel", c, actor=actor)
        if c["state"] in ("claimed", "review") and c["hp"] > 0 and self.players.known(c["player"]):
            return [(c["player"], op_task_hp(c["hp"], c["title"]))]

    def _templates(self, items):
        out = []
        for it in items or []:
            count = _int(it.get("count"), 1)
            if count <= 0:
                continue
            t = self.db.execute("SELECT * FROM template WHERE id = ? AND active = 1",
                                (_int(it.get("template"), -1),)).fetchone()
            if not t:
                raise GameError("Vorlage nicht gefunden.")
            out += [t] * min(count, 10)
        if not out:
            raise GameError("Keine Aufgaben ausgewählt.")
        return out

    def _add_claims(self, plan_id, templates, actor):
        for t in templates:
            cur = self.db.execute(
                "INSERT INTO claim (plan_id, template_id, title, xp, hp, state, created) VALUES (?,?,?,?,?,?,?)",
                (plan_id, t["id"], t["title"], t["xp"], t["hp"], "open", iso(now())))
            self._event("add", {"id": cur.lastrowid, "plan_id": plan_id, "player": None}, actor=actor)

    def op_plan_create(self, cmd, actor):
        if self.active_plan():
            raise GameError("Es läuft schon ein Plan.")
        hours = _int(cmd.get("hours"), 0)
        if not 1 <= hours <= PLAN_MAX_HOURS:
            raise GameError(f"Laufzeit 1–{PLAN_MAX_HOURS} Stunden.")
        templates = self._templates(cmd.get("items"))
        start = now()
        cur = self.db.execute("INSERT INTO plan (starts, ends) VALUES (?, ?)",
                              (iso(start), iso(start + datetime.timedelta(hours=hours))))
        self._event("plan.create", plan_id=cur.lastrowid, actor=actor)
        self._add_claims(cur.lastrowid, templates, actor)

    def op_plan_add(self, cmd, actor):
        plan = self.active_plan()
        if not plan:
            raise GameError("Kein Plan aktiv.")
        self._add_claims(plan["id"], self._templates(cmd.get("items")), actor)

    def op_plan_close(self, cmd, actor):
        plan = self.active_plan()
        if not plan:
            raise GameError("Kein Plan aktiv.")
        return self._close(plan, actor)

    def _close(self, plan, actor):
        """Ends the plan: open tasks lapse, claimed-but-not-done ones cost their HP.
        Tasks waiting for review stay in the queue."""
        t = iso(now())
        bookings = []
        for c in self.db.execute("SELECT * FROM claim WHERE plan_id = ? AND state IN ('open', 'claimed')",
                                 (plan["id"],)).fetchall():
            self._set(c, state="expired", decided=t)
            self._event("expire", c, actor=actor)
            if c["state"] == "claimed" and c["hp"] > 0 and self.players.known(c["player"]):
                bookings.append((c["player"], op_task_hp(c["hp"], c["title"])))
        self.db.execute("UPDATE plan SET closed = ? WHERE id = ?", (t, plan["id"]))
        self._event("plan.close", plan_id=plan["id"], actor=actor)
        return bookings

    def op_catalog_save(self, cmd, actor):
        t     = cmd.get("template") or {}
        title = str(t.get("title", "")).strip()
        xp, hp = _int(t.get("xp"), -1), _int(t.get("hp"), -1)
        if not 1 <= len(title) <= TITLE_MAX:
            raise GameError(f"Titel 1–{TITLE_MAX} Zeichen.")
        if not (0 <= xp <= 100 and 0 <= hp <= 100):
            raise GameError("XP und HP 0–100.")
        active = 1 if t.get("active", True) else 0
        if t.get("id"):
            cur = self.db.execute("UPDATE template SET title = ?, xp = ?, hp = ?, active = ? WHERE id = ?",
                                  (title, xp, hp, active, _int(t["id"], -1)))
            if not cur.rowcount:
                raise GameError("Vorlage nicht gefunden.")
        else:
            self.db.execute("INSERT INTO template (title, xp, hp, active, created) VALUES (?,?,?,?,?)",
                            (title, xp, hp, active, iso(now())))

    # ── Timer ────────────────────────────────────────────────────────────────

    def tick(self):
        """Closes the plan when its time is up. Returns True if something changed."""
        plan = self.active_plan()
        if not plan or datetime.datetime.fromisoformat(plan["ends"]) > now():
            return False
        with self.db:
            bookings = self._close(plan, "timer")
        for key, player_op in bookings:
            self.players.write(key, player_op)
        return True


# =============================================================================
# MQTT
# =============================================================================

def main():
    import paho.mqtt.client as mqtt

    here     = Path(__file__).resolve().parent
    db_path  = os.environ.get("HOUSEQUEST_DB", str(here / "game.db"))
    host     = os.environ.get("HOUSEQUEST_MQTT_HOST", "localhost")
    port     = int(os.environ.get("HOUSEQUEST_MQTT_PORT", "1883"))
    creds    = {}
    if os.environ.get("CREDENTIALS_DIRECTORY"):
        with open(Path(os.environ["CREDENTIALS_DIRECTORY"]) / "mqtt.json") as f:
            creds = json.load(f).get("mqtt", {})

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="housequest-game")
    if creds.get("user"):
        client.username_pw_set(creds["user"], creds.get("password"))

    players = Players(lambda topic, payload: client.publish(topic, payload, qos=1, retain=True))
    game    = Game(db_path, players)

    def publish_views():
        client.publish(T_CATALOG, json.dumps(game.catalog_view()), qos=1, retain=True)
        client.publish(T_PLAN, json.dumps(game.plan_view()), qos=1, retain=True)

    def on_connect(c, userdata, flags, reason_code, properties=None):
        if reason_code.is_failure:
            print(f"MQTT connect failed: {reason_code}", flush=True)
            return
        c.subscribe([(T_CMD + "+", 1), (T_PLAYERS + "+/state", 1)])
        publish_views()
        print("MQTT connected", flush=True)

    def on_message(c, userdata, msg):
        if msg.topic.startswith(T_PLAYERS):
            players.on_message(msg.topic.split("/")[3], msg.payload.decode("utf-8", "replace"))
            return
        sender = msg.topic[len(T_CMD):]
        try:
            cmd = json.loads(msg.payload)
        except ValueError:
            return
        reply = {"id": cmd.get("id")}
        try:
            game.handle(cmd, actor=sender)
            reply["ok"] = True
        except GameError as e:
            reply.update(ok=False, error=str(e))
        except Exception as e:          # keep serving; the panel gets a message
            print(f"Error in {cmd.get('op')}: {e!r}", flush=True)
            reply.update(ok=False, error="Interner Fehler im Spieldienst.")
        c.publish(T_REPLY + sender, json.dumps(reply), qos=1)
        if reply["ok"]:
            publish_views()

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect_async(host, port, 60)   # only stores the target; reconnect() below connects
    print(f"HouseQuest game service, db={db_path}, broker={host}:{port}", flush=True)

    # Single thread: network callbacks and timer never run concurrently
    last_tick = 0.0
    while True:
        if client.loop(timeout=0.5) != mqtt.MQTT_ERR_SUCCESS:
            try:
                client.reconnect()
            except OSError:
                time.sleep(2)
        players.tick()
        if time.monotonic() - last_tick > 15:
            last_tick = time.monotonic()
            if game.tick():
                publish_views()


if __name__ == "__main__":
    main()
