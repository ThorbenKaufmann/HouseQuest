#!/usr/bin/env python3
"""Tests for the game logic (no broker needed):  python3 pi/test_game.py"""

import datetime
import json
import unittest

import game
from game import Game, GameError, Players


class Fixture(unittest.TestCase):
    def setUp(self):
        self.sent = []      # (topic, payload) published by the player writer
        self.players = Players(lambda t, p: self.sent.append((t, json.loads(p))))
        for key, name in (("mia", "Mia"), ("ben", "Ben")):
            self.players.on_message(key, json.dumps({"name": name, "xp": 10, "hp": 50, "rev": 3}))
        self.g = Game(":memory:", self.players)
        self.g.handle({"op": "catalog.save", "template": {"title": "Bad putzen", "xp": 15, "hp": 10}})
        self.g.handle({"op": "catalog.save", "template": {"title": "Müll raus", "xp": 5, "hp": 5}})
        self.tpl = {t["title"]: t["id"] for t in self.g.catalog_view()["templates"]}

    def plan(self, **counts):
        items = [{"template": self.tpl[t], "count": n} for t, n in counts.items()]
        self.g.handle({"op": "plan.create", "hours": 12, "items": items})
        return self.claims()

    def claims(self):
        return self.g.plan_view()["claims"]

    def claim_of(self, cid):
        return next(c for c in self.claims() if c["id"] == cid)

    def echo(self):
        """Broker echo of our last player write."""
        topic, st = self.sent[-1]
        self.players.on_message(topic.split("/")[3], json.dumps(st))
        return st


class CatalogTest(Fixture):
    def test_validation(self):
        for t in ({"title": "", "xp": 1, "hp": 1}, {"title": "x" * 31, "xp": 1, "hp": 1},
                  {"title": "ok", "xp": 101, "hp": 0}, {"title": "ok", "xp": 0, "hp": -1}):
            with self.assertRaises(GameError):
                self.g.handle({"op": "catalog.save", "template": t})

    def test_edit_and_deactivate(self):
        tid = self.tpl["Müll raus"]
        self.g.handle({"op": "catalog.save", "template": {"id": tid, "title": "Müll rausbringen",
                                                          "xp": 6, "hp": 4, "active": False}})
        t = next(t for t in self.g.catalog_view()["templates"] if t["id"] == tid)
        self.assertEqual((t["title"], t["xp"], t["hp"], t["active"]), ("Müll rausbringen", 6, 4, False))
        with self.assertRaises(GameError):      # inactive templates can't be planned
            self.g.handle({"op": "plan.create", "hours": 1, "items": [{"template": tid}]})


class FlowTest(Fixture):
    def test_plan_create_counts_and_single_plan(self):
        cs = self.plan(**{"Bad putzen": 1, "Müll raus": 2})
        self.assertEqual([c["title"] for c in cs], ["Bad putzen", "Müll raus", "Müll raus"])
        self.assertTrue(all(c["state"] == "open" for c in cs))
        with self.assertRaises(GameError):
            self.plan(**{"Bad putzen": 1})

    def test_claim_cannot_be_taken_twice(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "claim", "claim": cid, "player": "mia"})
        with self.assertRaises(GameError) as e:
            self.g.handle({"op": "claim", "claim": cid, "player": "ben"})
        self.assertIn("Mia", str(e.exception))
        self.assertEqual(self.claim_of(cid)["player"], "mia")

    def test_only_holder_can_submit(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "claim", "claim": cid, "player": "mia"})
        with self.assertRaises(GameError):
            self.g.handle({"op": "submit", "claim": cid, "player": "ben"})
        self.g.handle({"op": "submit", "claim": cid, "player": "mia"})
        self.assertEqual(self.claim_of(cid)["state"], "review")

    def test_approve_books_xp(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "claim", "claim": cid, "player": "mia"})
        self.g.handle({"op": "submit", "claim": cid, "player": "mia"})
        self.g.handle({"op": "approve", "claim": cid})
        topic, st = self.sent[-1]
        self.assertEqual(topic, "house/quest/players/mia/state")
        self.assertEqual((st["xp"], st["hp"], st["rev"], st["by"]), (25, 50, 4, "game"))
        self.assertEqual(st["last_action"], "xp+15:task:Bad putzen")
        self.assertEqual(self.claim_of(cid)["state"], "done")

    def test_reject_goes_back_to_claimed(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "claim", "claim": cid, "player": "mia"})
        self.g.handle({"op": "submit", "claim": cid, "player": "mia"})
        self.g.handle({"op": "reject", "claim": cid})
        c = self.claim_of(cid)
        self.assertEqual((c["state"], c["player"], c["submitted"]), ("claimed", "mia", None))
        self.assertEqual(self.sent, [])

    def test_cancel_costs_hp_only_when_claimed(self):
        a, b = [c["id"] for c in self.plan(**{"Bad putzen": 1, "Müll raus": 1})]
        self.g.handle({"op": "cancel", "claim": b})              # open → no penalty
        self.assertEqual(self.sent, [])
        self.g.handle({"op": "claim", "claim": a, "player": "ben"})
        self.g.handle({"op": "cancel", "claim": a})
        topic, st = self.sent[-1]
        self.assertEqual((topic.split("/")[3], st["hp"], st["last_action"]), ("ben", 40, "hp-10:task:Bad putzen"))
        with self.assertRaises(GameError):
            self.g.handle({"op": "cancel", "claim": a})

    def test_plan_end(self):
        a, b, c = [x["id"] for x in self.plan(**{"Bad putzen": 2, "Müll raus": 1})]
        self.g.handle({"op": "claim", "claim": a, "player": "mia"})    # claimed → expires with penalty
        self.g.handle({"op": "claim", "claim": b, "player": "ben"})
        self.g.handle({"op": "submit", "claim": b, "player": "ben"})   # review → survives
        # time is up
        self.g.db.execute("UPDATE plan SET ends = ?", (game.iso(game.now() - datetime.timedelta(seconds=1)),))
        self.assertTrue(self.g.tick())
        view = self.g.plan_view()
        self.assertIsNone(view["plan"])
        self.assertEqual([(x["id"], x["state"]) for x in view["claims"]], [(b, "review")])
        self.assertEqual([(t.split("/")[3], s["hp"]) for t, s in self.sent], [("mia", 40)])
        states = dict(self.g.db.execute("SELECT id, state FROM claim").fetchall())
        self.assertEqual(states, {a: "expired", b: "review", c: "expired"})
        # review from the closed plan can still be approved
        self.g.handle({"op": "approve", "claim": b})
        self.assertEqual(self.sent[-1][1]["xp"], 25)
        self.assertFalse(self.g.tick())

    def test_claim_after_plan_closed(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "plan.close"})
        with self.assertRaises(GameError):
            self.g.handle({"op": "claim", "claim": cid, "player": "mia"})

    def test_events_logged(self):
        cid = self.plan(**{"Bad putzen": 1})[0]["id"]
        self.g.handle({"op": "claim", "claim": cid, "player": "mia"}, actor="pi-kiosk")
        actions = [r[0] for r in self.g.db.execute("SELECT action FROM event ORDER BY id")]
        self.assertEqual(actions, ["plan.create", "add", "claim"])


class PlayersTest(Fixture):
    def test_rebase_on_conflict(self):
        self.players.write("mia", game.op_task_xp(15, "Bad putzen"))
        self.echo()                                                    # our rev 4 arrived
        # a panel wrote rev 4 too, after ours → re-apply on top of it
        self.players.on_message("mia", json.dumps({"name": "Mia", "xp": 0, "hp": 20, "rev": 4, "by": "pi-kiosk"}))
        topic, st = self.sent[-1]
        self.assertEqual((len(self.sent), st["xp"], st["hp"], st["rev"]), (2, 15, 20, 5))

    def test_queue_one_in_flight(self):
        self.players.write("ben", game.op_task_xp(5, "a"))
        self.players.write("ben", game.op_task_xp(5, "b"))
        self.assertEqual(len(self.sent), 1)                            # second waits for the echo
        self.echo()
        self.assertEqual((len(self.sent), self.sent[-1][1]["xp"], self.sent[-1][1]["rev"]), (2, 20, 5))


if __name__ == "__main__":
    unittest.main()
