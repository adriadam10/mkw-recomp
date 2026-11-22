"""GameBanana's Mario Kart World mods for the start screen's Mods page: list, search, a mod's page and its download.

Why: players find MKW mods on GameBanana (game 22152); the launcher lists and installs them from there at the player's
request, so this project hosts no mod and no game file (the mods are their authors', under their own licenses).
It uses GameBanana's public apiv11 (no key), with a User-Agent of ours, answers cached for 10 minutes and at most 60
requests a minute. A download comes only from gamebanana.com, only a file of that mod that GameBanana's scan calls
clean, and its MD5 is checked.

Usage: gamebanana.py --cache DIR browse [--page N] [--sort downloads|new|likes]
       gamebanana.py --cache DIR search QUERY [--page N]
       gamebanana.py --cache DIR info MOD_ID
       gamebanana.py --cache DIR download MOD_ID FILE_ID DEST_DIR
       gamebanana.py --cache DIR images URL...      preview pictures into DIR/img (only images.gamebanana.com)
  Lines on stdout, one event each, tab-separated (fields hold no tab; a text's line breaks are written \\n):
    mod ID NAME AUTHOR LIKES VIEWS CATEGORY URL THUMB VIS RATED   a list or search row (VIS: show, warn or
                           hide, the content rating's visibility; RATED: 1 when GameBanana marks the mod)
    page N TOTAL           after the rows: this page and how many mods the query has
    info ID NAME AUTHOR LICENSE URL VERSION VIS RATINGS   a mod's page, then its
    text TEXT              description (plain text) and one line per file:
    file ID NAME SIZE SCAN NOTE
    image URL FULL         its preview pictures (the authors' own screenshots, shown from GameBanana): 530 pixels
                           wide, and the original size for the enlarged view
    image URL PATH         from images: a picture now in the local cache
    progress <done> <size> while downloading
    done <path>            the downloaded file
    error <code> <detail>  codes: net, notfound, unsafe, md5, usage
  Exit 0 done, 1 failed.
"""

import argparse
import hashlib
import html
import http.client
import json
import os
import queue
import re
import socket
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

GAME = 22152  # Mario Kart World on GameBanana
API = "https://gamebanana.com/apiv11"
USER_AGENT = "mkw-recomp (+https://github.com/adriadam10/mkw-recomp)"
TTL, PER_MINUTE, PER_PAGE = 600, 60, 20
SORTS = {"downloads": "Generic_MostDownloaded", "new": "Generic_Newest", "likes": "Generic_MostLiked"}


def _race_create_connection(address, timeout=socket._GLOBAL_DEFAULT_TIMEOUT, source_address=None):
    """socket.create_connection with every resolved address at once, staggered (Happy Eyeballs): CPython tries them
    one after another with the full timeout each, which stalls for tens of seconds when IPv6 is blackholed but IPv4
    answers (or the reverse). The first connection wins and the losers are closed; when all fail, the error comes
    as soon as the last one has."""
    host, port = address
    addrs = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    if len(addrs) < 2:
        return socket.create_connection(address, timeout, source_address)
    results: queue.Queue = queue.Queue()
    lock = threading.Lock()
    won = []

    def attempt(family, kind, proto, addr):
        sock = None
        try:
            sock = socket.socket(family, kind, proto)
            if timeout is not socket._GLOBAL_DEFAULT_TIMEOUT:
                sock.settimeout(timeout)
            if source_address:
                sock.bind(source_address)
            sock.connect(addr)
        except OSError as e:
            if sock:
                sock.close()
            results.put(e)
            return
        with lock:
            first = not won
            if first:
                won.append(sock)
                results.put(sock)
        if not first:
            sock.close()

    deadline = None if timeout in (None, socket._GLOBAL_DEFAULT_TIMEOUT) else time.monotonic() + timeout
    started = failed = 0
    last: OSError | None = None
    while failed < len(addrs):
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            with lock:
                if won:
                    won[0].close()
                else:
                    won.append(None)  # a connection finishing later must close itself
            raise TimeoutError(f"connecting to {host}:{port} timed out") from last
        if started < len(addrs):
            family, kind, proto, _, addr = addrs[started]
            threading.Thread(target=attempt, args=(family, kind, proto, addr), daemon=True).start()
            started += 1
        wait = 0.25 if started < len(addrs) else None
        if remaining is not None:
            wait = min(wait, remaining) if wait is not None else remaining
        try:
            got = results.get(timeout=wait)
        except queue.Empty:
            continue
        if isinstance(got, OSError):
            failed, last = failed + 1, got
            continue
        return got
    raise last or OSError(f"no address of {host}:{port} answered")


_orig_init = http.client.HTTPConnection.__init__


def _init(self, *args, **kwargs):
    _orig_init(self, *args, **kwargs)
    self._create_connection = _race_create_connection   # connect() keeps its TCP_NODELAY and its proxy tunnel


http.client.HTTPConnection.__init__ = _init


def field(v) -> str:
    return str(v).replace("\t", " ").replace("\r", "").replace("\n", "\\n")


def say(*parts):
    try:
        print("\t".join(field(p) for p in parts), flush=True)
    except BrokenPipeError:
        os._exit(0)   # the launcher went away


def throttle(cache: Path) -> None:
    """At most PER_MINUTE requests in any minute, counted across runs in cache/requests."""
    log = cache / "requests"
    now = time.time()
    try:
        times = [float(t) for t in log.read_text().split() if now - float(t) < 60]
    except (OSError, ValueError):
        times = []
    if len(times) >= PER_MINUTE:
        time.sleep(60 - (now - times[0]) + 0.1)
    times.append(time.time())
    log.write_text("\n".join(f"{t:.3f}" for t in times[-PER_MINUTE:]))


def get_json(cache: Path, path: str, query: dict | None = None) -> dict:
    url = f"{API}/{path}" + ("?" + urllib.parse.urlencode(query) if query else "")
    key = cache / (hashlib.sha256(url.encode()).hexdigest()[:24] + ".json")
    try:
        if time.time() - key.stat().st_mtime < TTL:
            return json.loads(key.read_text(encoding="utf-8"))
    except OSError, ValueError:
        pass  # A missing or damaged entry is a cache miss.
    throttle(cache)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        data = json.loads(r.read().decode("utf-8"))
    # Keep the last entry until its replacement is complete, even if the launcher stops this process.
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=cache, delete_on_close=False) as tmp:
        json.dump(data, tmp)
        tmp.close()  # Windows requires the temporary file closed before renaming it.
        Path(tmp.name).replace(key)
    return data


def plain(text: str | None, cap: int = 0) -> str:
    """HTML to one paragraph of plain text (the launcher draws no HTML)."""
    t = re.sub(r"<br\s*/?>|</p>|</li>|</h\d>", "\n", text or "", flags=re.I)
    t = html.unescape(re.sub(r"<[^>]+>", "", t))
    t = re.sub(r"[ \t]+", " ", re.sub(r"\n\s*\n+", "\n", t)).strip()
    return t[:cap].rstrip() + "…" if cap and len(t) > cap else t


def license_text(raw: str | None) -> str:
    """The license field is HTML (a Creative Commons badge and link, or the author's own words): its name or text."""
    m = re.search(r"creativecommons\.org/licenses/([a-z-]+)/([\d.]+)", raw or "")
    return f"CC {m.group(1).upper()} {m.group(2)}" if m else plain(raw, 300) or "-"


def row(r: dict) -> dict:
    img = (r.get("_aPreviewMedia") or {}).get("_aImages") or []
    thumb = f"{img[0]['_sBaseUrl']}/{img[0].get('_sFile220') or img[0]['_sFile']}" if img else ""
    return {"id": r["_idRow"], "name": r.get("_sName", ""), "author": (r.get("_aSubmitter") or {}).get("_sName", ""),
            "likes": r.get("_nLikeCount", 0), "views": r.get("_nViewCount", 0), "date": r.get("_tsDateAdded", 0),
            "category": (r.get("_aRootCategory") or {}).get("_sName", ""), "thumb": thumb,
            "vis": r.get("_sInitialVisibility", "show") or "show", "rated": 1 if r.get("_bHasContentRatings") else 0,
            "url": r.get("_sProfileUrl", f"https://gamebanana.com/mods/{r['_idRow']}")}


def listing(cache: Path, data: dict, page: int) -> int:
    for r in data.get("_aRecords", []):
        if r.get("_sModelName", "Mod") == "Mod":
            m = row(r)
            say("mod", m["id"], m["name"], m["author"], m["likes"], m["views"], m["category"], m["url"], m["thumb"],
            m["vis"], m["rated"])
    say("page", page, (data.get("_aMetadata") or {}).get("_nRecordCount", 0))
    return 0


def info(cache: Path, mod_id: int) -> dict:
    p = get_json(cache, f"Mod/{mod_id}/ProfilePage")
    if (p.get("_aGame") or {}).get("_idRow") != GAME:
        raise LookupError(f"mod {mod_id} is not a Mario Kart World mod")
    files = [{"id": f["_idRow"], "name": f.get("_sFile", ""), "size": f.get("_nFilesize", 0),
              "md5": f.get("_sMd5Checksum", ""), "scan": f.get("_sAvResult", ""),
              "note": plain(f.get("_sDescription"), 200), "url": f.get("_sDownloadUrl", "")}
             for f in p.get("_aFiles") or []]
    shots = [(f"{i['_sBaseUrl']}/{i.get('_sFile530') or i['_sFile']}", f"{i['_sBaseUrl']}/{i['_sFile']}")
             for i in ((p.get("_aPreviewMedia") or {}).get("_aImages") or [])[:6]]
    ratings = ", ".join((p.get("_aContentRatings") or {}).values())
    return {"id": mod_id, "name": p.get("_sName", ""), "images": shots, "author": (p.get("_aSubmitter") or {}).get("_sName", ""),
            "text": plain(p.get("_sText"), 1500), "license": license_text(p.get("_sLicense")),
            "url": p.get("_sProfileUrl", f"https://gamebanana.com/mods/{mod_id}"), "version": p.get("_sVersion", ""),
            "vis": p.get("_sInitialVisibility", "show") or "show", "ratings": ratings, "files": files}


def download(cache: Path, mod_id: int, file_id: int, dest: Path) -> int:
    m = info(cache, mod_id)
    f = next((f for f in m["files"] if f["id"] == file_id), None)
    if not f:
        say("error", "notfound", f"file {file_id} is not one of mod {mod_id}'s")
        return 1
    if f["scan"] != "clean":
        say("error", "unsafe", f"GameBanana's scan says {f['scan'] or 'nothing'}")
        return 1
    url = f["url"] or f"https://gamebanana.com/dl/{file_id}"
    if urllib.parse.urlparse(url).hostname != "gamebanana.com":
        say("error", "unsafe", url)
        return 1
    name = re.sub(r"[^\w.\- ]", "_", f["name"]) or f"{file_id}.zip"
    dest.mkdir(parents=True, exist_ok=True)
    out, part = dest / name, dest / (name + ".part")
    throttle(cache)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    md5, done, last = hashlib.md5(), 0, 0.0
    with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as o:
        host = urllib.parse.urlparse(r.geturl()).hostname or ""
        if host != "gamebanana.com" and not host.endswith(".gamebanana.com"):   # files.gamebanana.com after the 302
            say("error", "unsafe", r.geturl())
            return 1
        while chunk := r.read(1 << 16):
            o.write(chunk)
            md5.update(chunk)
            done += len(chunk)
            if time.time() - last > 0.2:
                say("progress", done, f["size"])
                last = time.time()
    if f["md5"] and md5.hexdigest() != f["md5"].lower():
        part.unlink(missing_ok=True)
        say("error", "md5", f"{md5.hexdigest()} against {f['md5']}")
        return 1
    part.replace(out)
    say("progress", done, f["size"])
    say("done", out)
    return 0


def images(cache: Path, urls: list[str]) -> int:
    """Each picture into cache/img once (named by its URL's hash), only from GameBanana's image host."""
    out = cache / "img"
    out.mkdir(exist_ok=True)
    for url in urls:
        if urllib.parse.urlparse(url).hostname != "images.gamebanana.com":
            continue
        path = out / (hashlib.sha256(url.encode()).hexdigest()[:24] + os.path.splitext(urllib.parse.urlparse(url).path)[1])
        if not path.is_file():
            throttle(cache)
            try:
                req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=20) as r:
                    data = r.read(4 << 20)   # a preview is tens of KB; 4 MB at most
                with tempfile.NamedTemporaryFile(dir=out, delete_on_close=False) as tmp:
                    tmp.close()
                    Path(tmp.name).write_bytes(data)
                    Path(tmp.name).replace(path)
            except OSError:
                continue   # a picture that does not come is just not shown
        say("image", url, path)
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # mods_browse.cpp reads this pipe as UTF-8
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache", type=Path, required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("browse")
    b.add_argument("--page", type=int, default=1)
    b.add_argument("--sort", choices=sorted(SORTS), default="downloads")
    s = sub.add_parser("search")
    s.add_argument("query")
    s.add_argument("--page", type=int, default=1)
    i = sub.add_parser("info")
    i.add_argument("mod_id", type=int)
    d = sub.add_parser("download")
    d.add_argument("mod_id", type=int)
    d.add_argument("file_id", type=int)
    d.add_argument("dest", type=Path)
    im = sub.add_parser("images")
    im.add_argument("urls", nargs="+")
    args = ap.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    try:
        if args.cmd == "browse":
            q = {"_aFilters[Generic_Game]": GAME, "_nPage": args.page, "_nPerpage": PER_PAGE,
                 "_sSort": SORTS[args.sort]}
            return listing(args.cache, get_json(args.cache, "Mod/Index", q), args.page)
        if args.cmd == "search":
            q = {"_sSearchString": args.query, "_idGameRow": GAME, "_sModelName": "Mod", "_nPage": args.page,
                 "_nPerpage": PER_PAGE}
            return listing(args.cache, get_json(args.cache, "Util/Search/Results", q), args.page)
        if args.cmd == "info":
            m = info(args.cache, args.mod_id)
            say("info", m["id"], m["name"], m["author"], m["license"], m["url"], m["version"], m["vis"], m["ratings"])
            say("text", m["text"])
            for f in m["files"]:
                say("file", f["id"], f["name"], f["size"], f["scan"], f["note"])
            for small, full in m["images"]:
                say("image", small, full)
            return 0
        if args.cmd == "images":
            return images(args.cache, args.urls)
        return download(args.cache, args.mod_id, args.file_id, args.dest)
    except LookupError as e:
        say("error", "notfound", e)
    except (OSError, ValueError) as e:   # urllib's errors are OSErrors; a bad answer, ValueError
        say("error", "net", e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
