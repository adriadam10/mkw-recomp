"""The NPLN methods tools/npln/server.py answers, and the store behind them (users, scores, ghost files).

Why: one place for every method, each written from the request the game sends (logged in full by call()) and the
fields the game reads back. A method nobody wrote yet answers UNIMPLEMENTED and its request is in the log.

Usage: imported by tools/npln/server.py.
"""

import hashlib
import json
import logging
import re
import secrets
import threading
import time
from pathlib import Path

import grpc

import pb

log = logging.getLogger("npln")

# Methods whose responses are streams (server-streaming RPCs)
SERVER_STREAMS = {
    "/nn.npln.ugcstore.v1.Ugcstore/BulkGetDocuments",
    "/nn.npln.friends.v1.Friends/SubscribeFriendUsers",
    "/nn.npln.friends.v1.PresenceService/SubscribePresences",
    "/nn.npln.maintenance.v1.MaintenanceScheduleService/SubscribeMaintenanceSchedules",
}

TOKEN_TTL = 3600


class Store:
    """Everything the server keeps, as JSON and files under data/: users by NSA id, scores, ghost blobs."""

    def __init__(self, data: Path, http_base: str):
        self.data, self.http_base = data, http_base
        self.lock = threading.Lock()
        self.blobs = data / "blobs"
        self.blobs.mkdir(exist_ok=True)
        self.db_path = data / "db.json"
        self.db = json.loads(self.db_path.read_text()) if self.db_path.exists() else {}
        self.db.setdefault("users", {})  # nsa id -> {"user": "u-...", "token": ...}
        self.db.setdefault("tokens", {})  # access token -> user
        self.db.setdefault("scores", {})  # chart -> {user: score record}
        self.db.setdefault("documents", {})  # name -> document record
        self.db.setdefault("uploads", {})  # upload path -> user

    def save(self) -> None:
        tmp = self.db_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.db, indent=1))
        tmp.replace(self.db_path)

    # ---- users and tokens
    def user_for_nsa(self, nsa: str) -> str:
        with self.lock:
            u = self.db["users"].get(nsa)
            if not u:
                u = {"user": "u-" + hashlib.sha256(nsa.encode()).hexdigest()[:20]}
                self.db["users"][nsa] = u
                self.save()
                log.info("new user %s for NSA %s", u["user"], nsa)
            return u["user"]

    def new_token(self, user: str) -> tuple[str, str]:
        with self.lock:
            access, refresh = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
            self.db["tokens"][access] = user
            self.db["tokens"]["r:" + refresh] = user
            self.save()
            return access, refresh

    def token_user(self, meta: dict) -> str | None:
        auth = meta.get("authorization", "")
        tok = auth.split(" ", 1)[1] if " " in auth else auth
        return self.db["tokens"].get(tok)

    # ---- files (UGC): uploads go to /upload/<id>, downloads come from /ugc/<id>
    def blob_get(self, path: str) -> bytes | None:
        m = re.fullmatch(r"/(?:ugc|upload)/([A-Za-z0-9_-]{8,64})", path.split("?")[0])
        p = self.blobs / m.group(1) if m else None
        return p.read_bytes() if p and p.is_file() else None

    def blob_put(self, path: str, body: bytes) -> bool:
        m = re.fullmatch(r"/upload/([A-Za-z0-9_-]{8,64})", path.split("?")[0])
        if not m or m.group(1) not in self.db["uploads"] or len(body) > 8 << 20:
            return False
        (self.blobs / m.group(1)).write_bytes(body)
        return True


# ---- auth: any NSA id token is accepted; the user is named after the NSA id in it
def nsa_from_token(tok: str) -> str:
    """The "sub" of the runtime's id token (ov_npln.c), or a hash of whatever came."""
    import base64

    try:
        part = tok.split(".")[1]
        claims = json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))
        return str(claims["sub"])
    except (IndexError, ValueError, KeyError):
        return hashlib.sha256(tok.encode()).hexdigest()[:16]


def token_message(user: str, access: str, refresh: str) -> bytes:
    return pb.msg((1, user), (2, access), (3, refresh), (4, pb.msg((1, TOKEN_TTL))))


def auth_issue_token(store: Store, meta: dict, req: dict, ctx) -> bytes:
    ext = pb.sub(req, 2)
    nsa = nsa_from_token(pb.text(ext, 1) or pb.text(ext, 2))
    user = store.user_for_nsa(nsa)
    access, refresh = store.new_token(user)
    return pb.msg((1, token_message(user, access, refresh)))


def auth_refresh_token(store: Store, meta: dict, req: dict, ctx) -> bytes:
    user = store.db["tokens"].get("r:" + pb.text(req, 2)) or pb.text(req, 1)
    access, refresh = store.new_token(user)
    return pb.msg((1, token_message(user, access, refresh)))


def auth_create_user(store: Store, meta: dict, req: dict, ctx) -> bytes:
    ext = pb.sub(req, 3)
    user = store.user_for_nsa(nsa_from_token(pb.text(ext, 1) or pb.text(ext, 2)))
    return pb.msg((1, user))


def auth_issue_prearranged(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """The game's first call: tenant 1, user index 2, external id token 3 -> User 1 and Token 2."""
    tenant = pb.text(req, 1) or "tenants/" + meta.get("npln-tenant-id", "t-e96531c6-lp1")
    ext = pb.sub(req, 3)
    uid = store.user_for_nsa(nsa_from_token(pb.text(ext, 1) or pb.text(ext, 2)))
    name = f"{tenant}/users/{uid}"
    access, refresh = store.new_token(name)
    now = time.time()
    user = pb.msg(
        (1, name),
        (2, f"accounts/{uid}"),
        (3, pb.timestamp(now)),
        (4, pb.timestamp(now)),
        (5, int(hashlib.sha256(uid.encode()).hexdigest()[:12], 16)),
    )
    return pb.msg((1, user), (2, token_message(name, access, refresh)))


def empty(store: Store, meta: dict, req: dict, ctx) -> bytes:
    return b""


def caller(store: Store, meta: dict) -> str:
    """The calling user's resource name, from the access token (or the uid header)."""
    return store.token_user(meta) or "tenants/current/users/" + meta.get("uid", "unknown")


# ---- leaderboard holders: the player's display name for the rankings, kept per user
def lb_update_holder(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """UpdateHolder(holder 1, update_mask 2) -> Holder: name 1, display_name 3, fields 4, owner 5, times 7/8."""
    h = pb.sub(req, 1)
    user = caller(store, meta)
    with store.lock:
        rec = store.db.setdefault("holders", {}).setdefault(user, {"create": time.time()})
        rec["display_name"] = pb.text(h, 3) or rec.get("display_name", "")
        rec["update"] = time.time()
        store.save()
    return pb.msg(
        (1, pb.text(h, 1) or "tenants/current/holders/default"),
        (3, rec["display_name"]),
        (4, b""),
        (5, user),
        (7, pb.timestamp(rec["create"])),
        (8, pb.timestamp(rec["update"])),
    )


# ---- leaderboard scores: one per user and season (a time trial category is one course); lower is better (ms)
def season_of(score_name: str) -> str:
    """A score's season: the game names its score <season>/scoreHolders/default (RegisterScore)."""
    return re.sub(r"/(scores|scoreHolders)(/.*)?$", "", score_name)


def holder_of(user: str) -> str:
    """The game names holders tenants/current/holders/<user id> (BrowseLeaderboard's user filter)."""
    return "tenants/current/holders/" + user.rsplit("/", 1)[-1]


def score_message(season: str, user: str, rec: dict, rank: int | None, holder_detail: bool = True) -> bytes:
    holder = holder_of(user)
    detail = None
    if holder_detail:
        detail = pb.msg((1, holder), (3, rec.get("display_name", "")), (4, b""), (5, user))
    return pb.msg(
        (1, f"{season}/scoreHolders/{user.rsplit('/', 1)[-1]}"),
        (2, holder),
        (3, rec["score"]),
        (4, rec.get("resource_link") or None),
        (5, bytes.fromhex(rec["fields"]) if rec.get("fields") else None),
        (6, pb.timestamp(rec["time"])),
        (7, detail),
        (8, pb.msg((1, rank), (2, rank - 1)) if rank else None),
    )


def ranked(store: Store, season: str) -> list[tuple[str, dict]]:
    """(user, record) best first; display names from the holders."""
    holders = store.db.get("holders", {})
    rows = []
    for user, rec in store.db["scores"].get(season, {}).items():
        rows.append((user, dict(rec, display_name=holders.get(user, {}).get("display_name", ""))))
    rows.sort(key=lambda r: (r[1]["score"], r[1]["time"]))
    return rows


def lb_register_score(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """RegisterScore(score 1, force 2) -> Score. Score.name 1 is the season's scores collection or the score itself."""
    s = pb.sub(req, 1)
    season = season_of(pb.text(s, 1)) or "unknown"
    user = caller(store, meta)
    new = {
        "score": pb.get(s, 3, 0),
        "resource_link": pb.text(s, 4),
        "fields": (pb.get(s, 5) or b"").hex(),
        "time": time.time(),
    }
    with store.lock:
        board = store.db["scores"].setdefault(season, {})
        old = board.get(user)
        if pb.get(req, 2) or not old or new["score"] < old["score"]:
            board[user] = new
            store.save()
            log.info("score %s: %s -> %d ms", season, user, new["score"])
    rows = ranked(store, season)
    rank = next(i for i, (u, _) in enumerate(rows, 1) if u == user)
    return score_message(season, user, rows[rank - 1][1], rank)


def lb_get_score(store: Store, meta: dict, req: dict, ctx) -> bytes:
    name = pb.text(req, 1)
    season = season_of(name)
    rows = ranked(store, season)
    for i, (u, rec) in enumerate(rows, 1):
        mine = name.endswith(("/current", "/default"))   # the caller's own score
        if name.endswith("/" + u.rsplit("/", 1)[-1]) or mine:
            if mine and u != caller(store, meta):
                continue
            return score_message(season, u, rec, i)
    ctx.abort(grpc.StatusCode.NOT_FOUND, "no score")


def lb_browse(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """BrowseLeaderboard(season 1; user 3 / near 4 / range 5 filter) -> Leaderboard{season 1, scores 2}."""
    season = pb.text(req, 1)
    rows = ranked(store, season)
    if 3 in req:   # given holders only
        want = {pb.text({1: [h]}, 1) for h in pb.sub(req, 3).get(1, [])}
        picked = [(i, r) for i, r in enumerate(rows, 1) if holder_of(r[0]) in want or "current" in str(want)]
    elif 4 in req:   # around a holder
        f = pb.sub(req, 4)
        size = pb.get(f, 2, 10) or 10
        me = next((i for i, r in enumerate(rows) if r[0] == caller(store, meta)), 0)
        lo = max(0, me - size // 2)
        picked = [(i, r) for i, r in enumerate(rows[lo : lo + size], lo + 1)]
    else:   # a range from the top
        f = pb.sub(req, 5)
        off, size = pb.get(f, 1, 0) or 0, pb.get(f, 2, 0) or 100
        picked = [(i, r) for i, r in enumerate(rows[off : off + size], off + 1)]
    scores = [score_message(season, u, rec, i) for i, (u, rec) in picked]
    return pb.msg((1, pb.msg((1, season), (2, scores))))


def hold_open(ctx, first: bytes, every: bytes | None = None, period: int = 30):
    """A subscription stream: first now, then (if given) every each period seconds, until the game closes it."""
    yield first
    while True:
        for _ in range(period):
            if not ctx.is_active():
                return
            time.sleep(1)
        if every is not None:
            yield every


# ---- ugcstore: files (ghosts) go up to a URL of the HTTP port, through the game's curl (TLS is pass-through there)
def ugc_issue_upload_uri(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """IssueUploadUri(tenant 1, resumable 2) -> uri 1: a one-use upload slot."""
    uid = secrets.token_urlsafe(18)
    with store.lock:
        store.db["uploads"][uid] = {"user": caller(store, meta), "time": time.time()}
        store.save()
    return pb.msg((1, store.http_base.replace("http://", "https://", 1) + "/upload/" + uid))


def doc_owner_ok(store: Store, meta: dict, name: str) -> bool:
    """Only a user's own documents (.../documents/users/<user id>/...) can be written."""
    m = re.search(r"/documents/users/([^/]+)/", name)
    return bool(m) and caller(store, meta).rsplit("/", 1)[-1] == m.group(1)


def document_message(name: str, rec: dict) -> bytes:
    return pb.msg(
        (1, name),
        (2, bytes.fromhex(rec["fields"]) if rec.get("fields") else None),
        (3, pb.timestamp(rec["create"])),
        (4, pb.timestamp(rec["update"])),
    )


def ugc_commit(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """CommitDocuments(tenant 1, write_operations 2) -> write_results 1 (one per operation), commit_time 2.
    Operations: update_document 1 {document {name, fields}}, delete_document 2 {name}, import_attachment 5
    {parent, uri of an upload slot}, set_attachment 3 {parent, body}, unset_attachment 4 {parent}."""
    now, results = time.time(), []
    with store.lock:
        docs = store.db["documents"]
        for raw in req.get(2, []):
            op = pb.decode(raw)
            if 1 in op:
                d = pb.sub(pb.sub(op, 1), 1)
                name = pb.text(d, 1)
                if not doc_owner_ok(store, meta, name):
                    ctx.abort(grpc.StatusCode.PERMISSION_DENIED, "not your document")
                rec = docs.setdefault(name, {"create": now})
                rec["fields"], rec["update"] = (pb.get(d, 2) or b"").hex(), now
                log.info("document %s written", name)
            elif 2 in op:
                name = pb.text(pb.sub(op, 2), 1)
                if doc_owner_ok(store, meta, name) and docs.pop(name, None):
                    log.info("document %s deleted", name)
            elif 5 in op or 3 in op or 4 in op:
                f = pb.sub(op, 5 if 5 in op else 3 if 3 in op else 4)
                name = pb.text(f, 1)
                if not doc_owner_ok(store, meta, name):
                    ctx.abort(grpc.StatusCode.PERMISSION_DENIED, "not your document")
                rec = docs.setdefault(name, {"create": now, "update": now})
                if 5 in op:
                    slot = pb.text(f, 2).rsplit("/upload/", 1)[-1].split("?")[0]
                    if slot not in store.db["uploads"] or not (store.blobs / slot).is_file():
                        ctx.abort(grpc.StatusCode.FAILED_PRECONDITION, "nothing uploaded there")
                    rec["attachment"] = slot
                elif 3 in op:
                    slot = secrets.token_urlsafe(18)
                    (store.blobs / slot).write_bytes(pb.get(f, 2) or b"")
                    rec["attachment"] = slot
                else:
                    rec.pop("attachment", None)
                rec["update"] = now
                log.info("document %s attachment %s", name, rec.get("attachment"))
            else:
                log.warning("write operation not handled: %s", sorted(op))
            results.append(pb.msg((1, pb.timestamp(now))))
        store.save()
    return pb.msg((1, results), (2, pb.timestamp(now)))


def ugc_get_document(store: Store, meta: dict, req: dict, ctx) -> bytes:
    name = pb.text(req, 1)
    rec = store.db["documents"].get(name)
    if not rec:
        ctx.abort(grpc.StatusCode.NOT_FOUND, "no such document")
    return document_message(name, rec)


def ugc_bulk_get(store: Store, meta: dict, req: dict, ctx):
    now = pb.timestamp(time.time())
    for raw in req.get(2, []):
        name = raw.decode("utf-8", "replace")
        rec = store.db["documents"].get(name)
        yield pb.msg((1, document_message(name, rec)), (4, now)) if rec else pb.msg((2, name), (4, now))


def ugc_issue_attachment_uri(store: Store, meta: dict, req: dict, ctx) -> bytes:
    """IssueAttachmentUri(parent 1, ...) -> uri 1 where the document's file downloads from."""
    rec = store.db["documents"].get(pb.text(req, 1))
    if not rec or not rec.get("attachment"):
        ctx.abort(grpc.StatusCode.NOT_FOUND, "no attachment")
    return pb.msg((1, store.http_base.replace("http://", "https://", 1) + "/ugc/" + rec["attachment"]))


# ---- maintenance: no maintenance ever; the stream stays open with a keep-alive now and then
def maintenance_subscribe(store: Store, meta: dict, req: dict, ctx):
    yield from hold_open(ctx, pb.msg((2, b"")), pb.msg((2, b"")))


# ---- friends: nobody has friends on this server (no friend accounts, no blocked ones)
def friends_subscribe(store: Store, meta: dict, req: dict, ctx):
    yield from hold_open(ctx, b"")


METHODS = {
    "/nn.npln.matchmaking.v1.GameSessionService/ListLatencyMeasurementServers": empty,   # none: no online races
    "/nn.npln.leaderboard.v1.LeaderboardService/UpdateHolder": lb_update_holder,
    "/nn.npln.leaderboard.v1.LeaderboardService/BrowseLeaderboard": lb_browse,
    "/nn.npln.leaderboard.v1.LeaderboardService/RegisterScore": lb_register_score,
    "/nn.npln.leaderboard.v1.LeaderboardService/GetScore": lb_get_score,
    "/nn.npln.friends.v1.Friends/ActivateUser": empty,
    "/nn.npln.ugcstore.v1.Ugcstore/IssueUploadUri": ugc_issue_upload_uri,
    "/nn.npln.ugcstore.v1.Ugcstore/CommitDocuments": ugc_commit,
    "/nn.npln.ugcstore.v1.Ugcstore/GetDocument": ugc_get_document,
    "/nn.npln.ugcstore.v1.Ugcstore/IssueAttachmentUri": ugc_issue_attachment_uri,
    "/nn.npln.auth.v1.Auth/IssueToken": auth_issue_token,
    "/nn.npln.auth.v1.Auth/IssuePrearrangedUserToken": auth_issue_prearranged,
    "/nn.npln.auth.v1.Auth/RefreshToken": auth_refresh_token,
    "/nn.npln.auth.v1.Auth/CreateUser": auth_create_user,
    "/nn.npln.auth.v1.Auth/ValidateToken": empty,
}
STREAMS = {
    "/nn.npln.maintenance.v1.MaintenanceScheduleService/SubscribeMaintenanceSchedules": maintenance_subscribe,
    "/nn.npln.friends.v1.Friends/SubscribeFriendUsers": friends_subscribe,
    "/nn.npln.ugcstore.v1.Ugcstore/BulkGetDocuments": ugc_bulk_get,
}


def _log_call(method: str, meta: dict, req: bytes) -> dict:
    who = {k: v for k, v in meta.items() if k in ("npln-tenant-id", "uid", "user-agent")}
    log.info("call %s %s (%d bytes)\n%s", method, who, len(req), pb.dump(req) or "  (empty)")
    try:
        return pb.decode(req)
    except (ValueError, IndexError):
        return {}


def call(store: Store, method: str, meta: dict, req: bytes, ctx) -> bytes:
    fields = _log_call(method, meta, req)
    fn = METHODS.get(method)
    if not fn:
        log.warning("not implemented: %s", method)
        ctx.abort(grpc.StatusCode.UNIMPLEMENTED, "not implemented by mkw-recomp's server")
    t = time.perf_counter()
    out = fn(store, meta, fields, ctx)
    log.info("  -> %d bytes in %.1f ms\n%s", len(out), (time.perf_counter() - t) * 1e3, pb.dump(out) or "  (empty)")
    return out


def call_stream(store: Store, method: str, meta: dict, req: bytes, ctx):
    fields = _log_call(method, meta, req)
    fn = STREAMS.get(method)
    if not fn:
        log.warning("not implemented (stream): %s", method)
        ctx.abort(grpc.StatusCode.UNIMPLEMENTED, "not implemented by mkw-recomp's server")
    yield from fn(store, meta, fields, ctx)
