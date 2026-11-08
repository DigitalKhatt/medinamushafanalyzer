#!/usr/bin/env python3
"""Local, transcript-free SVG ink annotation. Python standard library only."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from collections import Counter
from datetime import datetime, timezone
import hashlib
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import mimetypes
import os
from pathlib import Path
import re
import secrets
import sqlite3
import struct
import threading
import unicodedata
from urllib.parse import urlparse, parse_qs
import xml.etree.ElementTree as ET
import zlib
from workbench_paths import DATA, svg_source
from review_bridge import reconcile, apply_review, export_subwords,proposal_digest
from automatic.query import Corpus
from automatic import transcript_mark_search
from automatic.scope import load_scope,scoped_review,clip_runs,assets_folder

ROOT = Path(__file__).resolve().parent
ROLES = {"unknown": 255, "body": 1, "kashida": 2, "dot": 3,
         "mark": 4, "symbol": 5, "ornament": 6, "other": 7}
POSITIONS = {"unknown", "initial", "medial", "final", "isolated"}
LETTER_ROLES = {"body", "kashida", "dot", "mark"}
CHARACTER_REQUIRED_ROLES = {"body", "kashida"}
MARK_ROLES = {"dot", "mark"}


def character_identity(group):
    label = unicodedata.normalize("NFKC", group.get("label", "").strip())
    return label if len(label) == 1 and unicodedata.category(label).startswith("L") else None


def complete_character(group):
    return (character_identity(group) is not None and
            group.get("position", "unknown") in POSITIONS - {"unknown"})


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def atomic_json(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(6)}.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(obj, stream, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def split_for(page_id):
    bucket = int(hashlib.sha256(f"hafs1441-v1:{page_id}".encode()).hexdigest()[:8], 16) % 100
    return "test" if bucket < 10 else "validation" if bucket < 20 else "train"


def prepare(config, data_dir):
    source = Path(config["source_dir"]).expanduser().resolve()
    files = sorted(source.glob(config.get("page_pattern", "page[0-9][0-9][0-9].svg")))
    if not files:
        raise ValueError(f"No page SVGs found in {source}")
    pages = []
    for path in files:
        content = path.read_bytes()
        doc = ET.fromstring(content)
        viewbox = [float(x) for x in doc.attrib["viewBox"].replace(",", " ").split()]
        if len(viewbox) != 4 or not all(math.isfinite(x) for x in viewbox) or min(viewbox[2:]) <= 0:
            raise ValueError(f"Invalid viewBox in {path.name}")
        tags = Counter(e.tag.rsplit("}", 1)[-1] for e in doc.iter())
        pages.append({"id": path.stem, "filename": path.name, "sha256": hashlib.sha256(content).hexdigest(),
                      "bytes": len(content), "viewBox": viewbox, "elements": dict(tags),
                      "split": split_for(path.stem)})
    corpus = {"schemaVersion": 1, "name": config["name"], "sourceDir": str(source),
              "createdAt": datetime.now(timezone.utc).isoformat(), "pages": pages,
              "transcript": None, "splitPolicy": "sha256(hafs1441-v1:page-id); 80/10/10 by page"}
    atomic_json(data_dir / "corpus.json", corpus)
    print(f"Indexed {len(pages)} pages ({sum(p['bytes'] for p in pages):,} source bytes).")
    print("Splits:", dict(Counter(p["split"] for p in pages)))
    print(f"Inventory: {data_dir / 'corpus.json'}")
    return corpus


def integer(value, name, lo=0, hi=100_000_000):
    if type(value) is not int or not lo <= value <= hi:
        raise ValueError(f"{name} must be an integer between {lo} and {hi}.")
    return value


def describe_runs(runs, width):
    area, left, top, right, bottom = 0, width, 100_000_000, -1, -1
    for start, length in zip(runs[::2], runs[1::2]):
        area += length
        while length:
            y, x = divmod(start, width)
            n = min(length, width - x)
            left, top, right, bottom = min(left, x), min(top, y), max(right, x + n - 1), max(bottom, y)
            start, length = start + n, length - n
    return area, [left, top, right - left + 1, bottom - top + 1]


def validate_annotation(doc, page):
    if not isinstance(doc, dict) or doc.get("schemaVersion") != 1 or doc.get("pageId") != page["id"]:
        raise ValueError("Annotation schema or page identity does not match.")
    if doc.get("sourceSha256") != page["sha256"]:
        raise ValueError("Source hash changed. Annotations cannot be applied to a different SVG.")
    raster = doc.get("raster", {})
    width = integer(raster.get("width"), "raster width", 1, 10000)
    height = integer(raster.get("height"), "raster height", 1, 15000)
    if width * height > 40_000_000:
        raise ValueError("Raster exceeds 40 million pixels.")
    if raster.get("viewBox") != page["viewBox"]:
        raise ValueError("SVG coordinate transform changed.")
    integer(raster.get("threshold"), "ink threshold", 1, 254)
    integer(raster.get("chroma"), "maximum chroma", 0, 255)
    scale = raster.get("scale")
    if type(scale) not in (int, float) or not math.isfinite(scale) or scale <= 0:
        raise ValueError("Invalid raster scale.")
    if width != round(page["viewBox"][2] * scale) or height != round(page["viewBox"][3] * scale):
        raise ValueError("Raster dimensions do not match the SVG and scale.")
    expected = integer(doc.get("inkPixels"), "ink pixel count", 1, width * height)
    groups = doc.get("groups", {})
    metadata_version = doc.get("characterMetadataVersion", 0)
    if type(metadata_version) is not int or metadata_version not in (0, 1):
        raise ValueError("Unsupported character metadata version.")
    if not isinstance(groups, dict) or len(groups) > 100_000:
        raise ValueError("Invalid character groups.")
    for group_id, group in groups.items():
        if not isinstance(group_id, str) or not 1 <= len(group_id) <= 128 or not isinstance(group, dict):
            raise ValueError("Invalid character group.")
        if not isinstance(group.get("label", ""), str) or len(group.get("label", "")) > 128:
            raise ValueError("Character identity must be short text or empty.")
        if group.get("position", "unknown") not in POSITIONS:
            raise ValueError("Joining position must be initial, medial, final, isolated, or unknown.")
        form = group.get("form", "unknown")
        if not isinstance(form, str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", form):
            raise ValueError("Letter form must be a lowercase tag such as zigzag or expanded.")
        if type(group.get("reviewed", False)) is not bool:
            raise ValueError("Invalid character review state.")
    parts = doc.get("parts")
    if not isinstance(parts, list) or not 1 <= len(parts) <= 100_000:
        raise ValueError("Invalid parts list.")
    seen = bytearray(width * height)
    ids, total = set(), 0
    for part in parts:
        part_id = part.get("id")
        if not isinstance(part_id, str) or not 1 <= len(part_id) <= 128 or part_id in ids:
            raise ValueError("Part IDs must be unique, nonempty strings.")
        ids.add(part_id)
        role = part.get("role")
        if role not in ROLES or type(part.get("reviewed")) is not bool:
            raise ValueError("Invalid part role or review state.")
        if role == "unknown" and part["reviewed"]:
            raise ValueError("An unlabelled region cannot be approved for training.")
        if part.get("owner") is not None and part["owner"] not in groups:
            raise ValueError("Part refers to an unknown character group.")
        mark_type = part.get("markType", "unknown")
        if not isinstance(mark_type, str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", mark_type):
            raise ValueError("Mark type must be a lowercase tag such as fatha or double-dot.")
        if metadata_version == 1 and part["reviewed"] and role in CHARACTER_REQUIRED_ROLES:
            owner = groups.get(part.get("owner"), {})
            if not complete_character(owner) or not owner.get("reviewed", False):
                raise ValueError("Approved letter ink requires a reviewed character and joining position.")
        if part.get("line") is not None:
            integer(part["line"], "line number", 1, 200)
        runs = part.get("runs")
        if not isinstance(runs, list) or len(runs) == 0 or len(runs) % 2:
            raise ValueError("Invalid ink run encoding.")
        previous = -1
        for start, length in zip(runs[::2], runs[1::2]):
            integer(start, "run offset", 0, width * height - 1)
            integer(length, "run length", 1, width * height - start)
            if start < previous or any(seen[start:start + length]):
                raise ValueError("Ink runs overlap or are not sorted.")
            seen[start:start + length] = b"\x01" * length
            previous = start + length
        area, bbox = describe_runs(runs, width)
        if part.get("area") != area or part.get("bbox") != bbox:
            raise ValueError("Ink area or bounding box does not match its mask.")
        landmarks = part.get("landmarks", [])
        if not isinstance(landmarks, list) or len(landmarks) > 128:
            raise ValueError("A region can have up to 128 landmarks.")
        left, top, span_x, span_y = bbox
        landmark_keys = set()
        for point in landmarks:
            if not isinstance(point, dict) or point.get("kind") not in (
                "contour-left", "contour-right", "contour-top", "contour-bottom",
                "contour-turn", "skeleton-end", "skeleton-branch",
            ) or point.get("source") not in ("manual", "detected"):
                raise ValueError("Invalid landmark type or source.")
            x, y = point.get("x"), point.get("y")
            if (type(x) not in (int, float) or type(y) not in (int, float)
                    or not left <= x <= left + span_x or not top <= y <= top + span_y):
                raise ValueError("Landmark lies outside its ink region.")
            key = (point["kind"], x, y, point["source"])
            if key in landmark_keys:
                raise ValueError("Duplicate landmark.")
            landmark_keys.add(key)
        total += area
    if total != expected:
        raise ValueError("Ink coverage changed: a split must preserve all input ink.")
    subwords = doc.get("subwords", {})
    if not isinstance(subwords, dict) or len(subwords) > 100_000:
        raise ValueError("Invalid subword contexts.")
    for sid, sub in subwords.items():
        if not isinstance(sid, str) or not 1 <= len(sid) <= 128 or not isinstance(sub, dict):
            raise ValueError("Invalid subword identity.")
        if not isinstance(sub.get("text"), str) or not 1 <= len(sub["text"]) <= 256 or type(sub.get("reviewed")) is not bool:
            raise ValueError("Invalid subword transcript or review state.")
    for part in parts:
        if part.get("subwordId") is not None and part["subwordId"] not in subwords:
            raise ValueError("Ink refers to an unknown subword.")
        if type(part.get("subwordBody", False)) is not bool:
            raise ValueError("Invalid subword body membership.")
        if type(part.get("subwordAttachmentReviewed", False)) is not bool:
            raise ValueError("Invalid subword attachment review state.")
        if part.get("subwordBody") and subwords.get(part.get("subwordId"), {}).get("reviewed") and part["reviewed"] and part["role"] in {"dot", "mark", "symbol", "ornament", "other"}:
            raise ValueError("Approved subword body cannot include approved dot/mark/sign ink.")
    return seen


def png_bytes(width, height, pixels, bit_depth=8):
    stride = width * (bit_depth // 8)
    if len(pixels) != stride * height:
        raise ValueError("PNG dimensions do not match pixel buffer.")
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    rows = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def character_records(doc, group_map):
    """Only reviewed character attributes become classification targets."""
    instances, regions = [], []
    for owner, group in doc["groups"].items():
        if not group.get("reviewed", False) or not complete_character(group):
            continue
        assigned = [p for p in doc["parts"] if p.get("owner") == owner and p["role"] in LETTER_ROLES]
        parts = [p for p in assigned if p["reviewed"]]
        if not parts:
            continue
        form = group.get("form", "unknown")
        targets = {"character": character_identity(group), "position": group["position"],
                   "form": None if form == "unknown" else form}
        instance_id = group_map[owner]
        left = min(p["bbox"][0] for p in parts)
        top = min(p["bbox"][1] for p in parts)
        right = max(p["bbox"][0] + p["bbox"][2] for p in parts)
        bottom = max(p["bbox"][1] + p["bbox"][3] for p in parts)
        instances.append({"instanceId": instance_id, "groupId": owner,
                          "bbox": [left, top, right-left, bottom-top],
                          "regionIds": [p["id"] for p in parts], "targets": targets,
                          "hasReviewedBody": any(p["role"] == "body" for p in parts),
                          "classificationEligible": any(p["role"] == "body" for p in parts) and
                          all(p["reviewed"] for p in assigned)})
        for part in parts:
            regions.append({"regionId": part["id"], "instanceId": instance_id,
                            "groupId": owner, "role": part["role"],
                            "bbox": part["bbox"], **targets})
    return {"schemaVersion": 1, "pageId": doc["pageId"], "instances": instances, "regions": regions,
            "note": "Unknown form is null and must be ignored by the form classification loss."}


def export_page(doc, destination, page,scope=None):
    ink = validate_annotation(doc, page)
    width, height = doc["raster"]["width"], doc["raster"]["height"]
    allowed=None
    if scope:
        if scope['sourceSha256']!=page['sha256'] or scope['width']!=width or scope['height']!=height:raise ValueError('Frame scope differs from the annotation source')
        allowed=bytearray(width*height)
        for start,length in zip(scope['runs'][::2],scope['runs'][1::2]):allowed[start:start+length]=b'\1'*length
    semantic = bytearray(b'\xff')*(width*height) if scope else bytearray(width * height)
    image = bytearray(b"\xff") * (width * height)
    instances = bytearray(b'\xff\xff')*(width*height) if scope else bytearray(width * height * 2)
    if scope:
        for start,length in zip(scope['runs'][::2],scope['runs'][1::2]):
            semantic[start:start+length]=b'\0'*length;instances[start*2:(start+length)*2]=b'\0\0'*length
    for p, value in enumerate(ink):
        if value and (allowed is None or allowed[p]):
            image[p], semantic[p] = 0, 255  # Unknown ink is ignore, never background.
    group_map = {key: i + 1 for i, key in enumerate(sorted(doc["groups"]))}
    if len(group_map) > 65535:
        raise ValueError("Too many character instances for a 16-bit mask.")
    training_doc={**doc,'parts':[],'subwords':{id:dict(s) for id,s in doc.get('subwords',{}).items()}}
    for part in doc['parts']:
        runs=clip_runs(part['runs'],scope['runs']) if scope else part['runs']
        if not runs:continue
        area,bbox=describe_runs(runs,width);clipped=area!=part['area']
        training_doc['parts'].append({**part,'runs':runs,'area':area,'bbox':bbox,'reviewed':part['reviewed'] and not clipped})
        if clipped and part.get('subwordBody') and part.get('subwordId') in training_doc['subwords']:training_doc['subwords'][part['subwordId']]['reviewed']=False
    reviewed = 0
    for part in training_doc["parts"]:
        if not part["reviewed"]:
            continue
        reviewed += part["area"]
        label = ROLES[part["role"]]
        owner = struct.pack(">H", group_map.get(part["owner"], 0))
        for start, length in zip(part["runs"][::2], part["runs"][1::2]):
            semantic[start:start + length] = bytes([label]) * length
            instances[start * 2:(start + length) * 2] = owner * length
    destination.mkdir(parents=True, exist_ok=True)
    for name, pixels, depth in [("image.png", image, 8), ("semantic.png", semantic, 8), ("instances.png", instances, 16)]:
        (destination / name).write_bytes(png_bytes(width, height, pixels, depth))
    atomic_json(destination / "annotation.json", doc)
    atomic_json(destination / "characters.json", character_records(training_doc, group_map))
    subword_records, subword_mask = export_subwords(training_doc, png_bytes,scope)
    atomic_json(destination / "subwords.json", subword_records)
    (destination / "subword-instances.png").write_bytes(subword_mask)
    pilot_classes = {"other": 0, "dot": 1, "mark": 2}
    candidates = []
    scoped_parts={p['id']:p for p in training_doc['parts']}
    for part in doc["parts"]:
        scoped_part=scoped_parts.get(part['id'])
        target = pilot_classes.get(part["role"]) if scoped_part and scoped_part['reviewed'] else None
        subtype = part.get("markType", "unknown")
        candidates.append({"regionId": part["id"], "bbox": part["bbox"], "area": part["area"],
                           "runs": scoped_part['runs'] if scoped_part else part['runs'], "target": target,"inScope":bool(scoped_part),
                           "markType": subtype if target in (1, 2) and subtype != "unknown" else None,
                           "characterOwner": part.get("owner") if scoped_part else None,
                           "subwordOwner": part.get("subwordId") if scoped_part and part.get("subwordAttachmentReviewed") else None,
                           "subwordBodyReviewed": bool(doc.get("subwords", {}).get(part.get("subwordId"), {}).get("reviewed"))
                               if scoped_part and part.get("subwordAttachmentReviewed") else False})
    atomic_json(destination / "marks.json", {"schemaVersion": 1, "pageId": doc["pageId"],
                "sourceSha256": page["sha256"], "raster": doc["raster"], "classes": pilot_classes,
                "candidates": candidates,
                "note": "Only reviewed dot/mark/other ink has a target. Null targets are unlabelled, not negatives. A region may contain multiple disconnected components after merging."})
    atomic_json(destination / "metadata.json", {"pageId": page["id"], "sourceSha256": page["sha256"],
                "split": page["split"], "raster": doc["raster"], "instanceIds": group_map,
                "groups": doc["groups"], "reviewedInkPixels": reviewed, "totalInkPixels":sum(p['area'] for p in training_doc['parts']),
                "sourceInkPixels":doc['inkPixels'],"sourceScope":{k:scope[k] for k in ('scopeSha256','framePathId','version')} if scope else None,
                "classes": ROLES, "background": 0, "ignoreIndex": 255})
    return reviewed


class Store:
    def __init__(self, config, data_dir):
        self.config, self.data_dir = config, Path(data_dir)
        self.corpus = read_json(self.data_dir / "corpus.json")
        if Path(config["source_dir"]).expanduser().resolve() != Path(self.corpus["sourceDir"]):
            raise ValueError("Source directory changed. Run prepare again.")
        self.pages = {p["id"]: p for p in self.corpus["pages"]}
        self.lock = threading.RLock()
        self.token = secrets.token_urlsafe(32)
        self.automatic = Corpus(self.data_dir / "automatic")
        self.corpus_home = False

    def source(self, page_id):
        page = self.pages[page_id]
        content = (Path(self.corpus["sourceDir"]) / page["filename"]).read_bytes()
        if hashlib.sha256(content).hexdigest() != page["sha256"]:
            raise ValueError("Source SVG changed since prepare. Re-index and reconcile annotations first.")
        return content

    def annotation(self, page_id):
        path = self.data_dir / "annotations" / f"{page_id}.json"
        if not path.exists():
            return None
        doc = read_json(path)
        if doc["sourceSha256"] != self.pages[page_id]["sha256"]:
            raise ValueError("Saved annotation belongs to a different source SVG.")
        return doc

    def proposals(self, page_id,source='latest'):
        self.source(page_id)
        if source not in ('latest','automatic','legacy'):raise ValueError('Unknown proposal source')
        automatic=self.data_dir/'automatic'/'proposals'/f'{page_id}.json'
        if source=='latest':source='automatic' if automatic.exists() else 'legacy'
        path=automatic if source=='automatic' else self.data_dir/'proposals'/f'{page_id}.json'
        proposal = read_json(path)
        doc = self.annotation(page_id)
        if not doc:
            raise ValueError("Initialize this page's source ink before reviewing proposals.")
        key='automatic:'+proposal_digest(proposal)
        review_doc={**doc,'proposalReview':doc.get('proposalReviewSets',{}).get(key,{})} if source=='automatic' else doc
        result=scoped_review(reconcile(review_doc, proposal),doc,load_scope(self.data_dir/'automatic',page_id,self.pages[page_id]['sha256']))
        result.update(proposalSource=source,proposalReviewSetKey=key if source=='automatic' else None)
        return result

    def review(self, page_id, request):
        with self.lock:
            doc = self.annotation(page_id)
            if not doc or request.get("revision") != doc["revision"]:
                raise Conflict("This page was changed elsewhere. Reload review before applying labels.")
            proposals=self.proposals(page_id,request.get('proposalSource','latest'))
            key=proposals['proposalReviewSetKey']
            review_doc={**doc,'proposalReview':doc.get('proposalReviewSets',{}).get(key,{})} if key else doc
            updated = apply_review(review_doc, proposals, request, describe_runs)
            if key:
                records=updated.pop('proposalReview',{})
                updated['proposalReviewSets']={**doc.get('proposalReviewSets',{}),key:records}
                if 'proposalReview' in doc:updated['proposalReview']=doc['proposalReview']
            saved = self.save(page_id, updated)
            return {"revision": saved["revision"], "savedAt": saved["savedAt"]}

    @contextmanager
    def write_lock(self):
        with self.lock:
            # The workbench and serve-corpus may run in separate processes.
            # Keep their revision check and atomic snapshot write in one lock.
            with (self.data_dir / '.annotation-write.lock').open('a+b') as file:
                if os.name == 'posix':
                    import fcntl
                    fcntl.flock(file, fcntl.LOCK_EX)
                try:
                    yield
                finally:
                    if os.name == 'posix':fcntl.flock(file, fcntl.LOCK_UN)

    def save(self, page_id, doc):
        if doc.get("characterMetadataVersion") != 1:
            raise ValueError("Reload the updated workbench before saving: character metadata is now required.")
        self.source(page_id)
        coverage = validate_annotation(doc, self.pages[page_id])
        with self.write_lock():
            previous = self.annotation(page_id)
            revision = previous["revision"] if previous else 0
            if doc.get("revision") != revision:
                raise Conflict("This page was saved elsewhere. Reload before editing further.")
            if previous and (doc["raster"] != previous["raster"] or doc["inkPixels"] != previous["inkPixels"]):
                raise ValueError("An existing annotation's raster settings cannot be changed.")
            if previous:
                old_coverage = bytearray(len(coverage))
                for part in previous["parts"]:
                    for start, length in zip(part["runs"][::2], part["runs"][1::2]):
                        old_coverage[start:start + length] = b"\x01" * length
                if coverage != old_coverage:
                    raise ValueError("An edit moved or deleted original ink pixels.")
            saved = {**doc, "revision": revision + 1, "savedAt": datetime.now(timezone.utc).isoformat()}
            atomic_json(self.data_dir / "history" / page_id / f"revision-{revision + 1:06d}.json", saved)
            atomic_json(self.data_dir / "annotations" / f"{page_id}.json", saved)
            return saved


class Conflict(ValueError):
    pass


def handler_for(store):
    class Handler(BaseHTTPRequestHandler):
        def send(self, status, payload, mime="application/json",cache_control="no-store"):
            if mime == "application/json":
                payload = json.dumps(payload, ensure_ascii=False, allow_nan=False,separators=(',',':')).encode()
            compressed=mime=='application/json' and len(payload)>2048 and 'gzip' in self.headers.get('Accept-Encoding','')
            if compressed:payload=gzip.compress(payload,compresslevel=1)
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", cache_control)
            if mime=='application/json':self.send_header('Vary','Accept-Encoding')
            if compressed:self.send_header('Content-Encoding','gzip')
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            try:self.wfile.write(payload)
            except (BrokenPipeError,ConnectionResetError):pass  # Cancelled search.

        def do_GET(self):
            try:
                path = urlparse(self.path).path
                if path.startswith('/api/automatic/'):
                    action=path.removeprefix('/api/automatic/')
                    params=parse_qs(urlparse(self.path).query)
                    first=lambda key,default='': params.get(key,[default])[0]
                    if action.startswith('marks/'):
                        with store.automatic.connect() as db:
                            if action=='marks/metadata': result=transcript_mark_search.metadata(db)
                            elif action=='marks/search': result=transcript_mark_search.search(db,params)
                            elif action=='marks/image.svg':
                                svg=transcript_mark_search.image(db,first('id'),first('context')=='1')
                                self.send(200,svg.encode(),'image/svg+xml')
                                return
                            elif action.startswith('marks/report/'):
                                self.send(200,transcript_mark_search.report_file(db,action.removeprefix('marks/report/')),'text/html; charset=utf-8')
                                return
                            elif action=='marks/report':
                                self.send_response(302)
                                self.send_header('Location','/api/automatic/marks/report/'+first('page')+'.html'
                                                 if re.fullmatch(r'page\d{3}',first('page')) else '/api/automatic/marks/report/index.html')
                                self.end_headers()
                                return
                            else:
                                self.send(404,{'error':'Unknown mark search endpoint'})
                                return
                    elif action=='status': result=store.automatic.status()
                    elif action=='metadata': result=store.automatic.metadata()
                    elif action=='search': result=store.automatic.search(params)
                    elif action=='detail': result=store.automatic.detail(first('id'),first('entity','item'))
                    elif action=='similar': result=store.automatic.similar(first('id'))
                    elif action=='page.png':
                        page_id=first('page')
                        if page_id not in store.pages:raise ValueError('Unknown source page')
                        store.source(page_id)
                        source=store.data_dir/'automatic'/'render'/f'{page_id}.png'
                        self.send(200,source.read_bytes(),'image/png')
                        return
                    elif action=='image.svg':
                        self.send(200,store.automatic.image(first('id'),first('entity','item'),first('source','final')).encode(), 'image/svg+xml',
                            cache_control='private, max-age=3600' if first('v') else 'no-store')
                        return
                    else:
                        self.send(404,{'error':'Corpus endpoint not found'})
                        return
                    self.send(200,result)
                    return
                if path == "/api/corpus":
                    pages = []
                    for page in store.corpus["pages"]:
                        saved_path = store.data_dir / "annotations" / f"{page['id']}.json"
                        pages.append({**page, "hasAnnotation": saved_path.exists(),
                            "hasProposals": (store.data_dir / "proposals" / f"{page['id']}.json").exists() or (store.data_dir/'automatic'/'proposals'/f"{page['id']}.json").exists(),
                            "hasLegacyProposals":(store.data_dir/'proposals'/f"{page['id']}.json").exists(),
                            "hasAutomaticProposals":(store.data_dir/'automatic'/'proposals'/f"{page['id']}.json").exists()})
                    self.send(200, {"name": store.config["name"], "pages": pages, "config": store.config, "token": store.token})
                    return
                match = re.fullmatch(r"/api/pages/(page\d{3})(/source\.svg|/annotation|/export|/proposals)?", path)
                if match and match[1] in store.pages:
                    page_id, action = match[1], match[2]
                    if action == "/source.svg":
                        original=store.source(page_id)
                        scoped=parse_qs(urlparse(self.path).query).get('scope')==['interior']
                        geometry=load_scope(store.data_dir/'automatic',page_id,store.pages[page_id]['sha256']) if scoped else None
                        source=(assets_folder(store.data_dir/'automatic',geometry['version'])/f'{page_id}.svg').read_bytes() if geometry else original
                        self.send(200, source, "image/svg+xml")
                    elif action == "/proposals":
                        self.send(200, store.proposals(page_id,parse_qs(urlparse(self.path).query).get('source',['latest'])[0]))
                    elif action == "/export":
                        doc = store.annotation(page_id)
                        if not doc:
                            raise ValueError("Save this page before exporting.")
                        dest = store.data_dir / "dataset" / store.pages[page_id]["split"] / page_id
                        count = export_page(doc, dest, store.pages[page_id],load_scope(store.data_dir/'automatic',page_id,store.pages[page_id]['sha256']))
                        self.send(200, {"directory": str(dest), "reviewedInkPixels": count})
                    else:
                        store.source(page_id)
                        self.send(200, {"page": store.pages[page_id], "annotation": store.annotation(page_id),
                            "sourceScope":load_scope(store.data_dir/'automatic',page_id,store.pages[page_id]['sha256'])})
                    return
                relative = ("corpus.html" if store.corpus_home else "index.html") if path == "/" else path.lstrip("/")
                file = (ROOT / "app" / relative).resolve()
                if not file.is_relative_to(ROOT / "app") or not file.is_file():
                    self.send(404, {"error": "Not found"})
                    return
                mime = "text/javascript" if file.suffix == ".mjs" else mimetypes.guess_type(file.name)[0] or "application/octet-stream"
                self.send(200, file.read_bytes(), mime)
            except (ValueError, KeyError, OSError, sqlite3.Error) as error:
                self.send(400, {"error": str(error)})

        def do_PUT(self):
            try:
                if self.headers.get("X-Annotation-Token") != store.token:
                    self.send(403, {"error": "Invalid local annotation token."})
                    return
                match = re.fullmatch(r"/api/pages/(page\d{3})/(annotation|proposal-review)", urlparse(self.path).path)
                if not match or match[1] not in store.pages:
                    self.send(404, {"error": "Page not found"})
                    return
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 64 * 1024 * 1024:
                    raise ValueError("Invalid annotation size.")
                if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                    raise ValueError("Expected JSON annotation.")
                doc = json.loads(self.rfile.read(length))
                if match[2] == "proposal-review":
                    self.send(200, store.review(match[1], doc))
                    return
                saved = store.save(match[1], doc)
                self.send(200, {"revision": saved["revision"], "savedAt": saved["savedAt"]})
            except Conflict as error:
                self.send(409, {"error": str(error)})
            except (ValueError, KeyError, TypeError, OSError) as error:
                self.send(400, {"error": str(error)})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DATA)
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare", help="Read-only inventory of source SVGs")
    prep.add_argument("--source", type=Path)
    serve = sub.add_parser("serve", help="Run the local annotation workbench")
    serve.add_argument("--port", type=int, default=8765)
    corpus_server = sub.add_parser("serve-corpus", help="Browse the automatic vector corpus, measurements and learned predictions")
    corpus_server.add_argument("--port", type=int, default=8767)
    letter_server = sub.add_parser("serve-letter-corpus", help="Browse letter bodies and mark-to-letter attachments")
    letter_server.add_argument("--port", type=int, default=8768)
    letter_server.add_argument("--corpus", type=Path)
    letter_server.add_argument("--reviews", type=Path)
    sub.add_parser("validate", help="Validate saved annotations and source hashes")
    export = sub.add_parser("export", help="Export reviewed labels as training masks")
    export.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command == "serve-letter-corpus":
        from letter_corpus.server import serve as serve_letters
        serve_letters(args.corpus or args.data_dir/'automatic/letter-corpus', args.port, args.reviews)
        return
    config = read_json(ROOT / "project.json")
    config["source_dir"] = str(svg_source())
    if args.command == "prepare":
        if args.source:
            config["source_dir"] = str(args.source.expanduser().resolve())
            atomic_json(ROOT / "project.json", config)
        prepare(config, args.data_dir)
        return
    if not (args.data_dir / "corpus.json").exists():
        prepare(config, args.data_dir)
    store = Store(config, args.data_dir)
    if args.command in ("serve", "serve-corpus"):
        store.corpus_home = args.command == "serve-corpus"
        server = ThreadingHTTPServer(("127.0.0.1", args.port), handler_for(store))
        print(f"Mushaf Ink Segmentation: http://127.0.0.1:{args.port}", flush=True)
        print(f"Annotations: {args.data_dir / 'annotations'}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
    else:
        exported = []
        for page_id, page in store.pages.items():
            doc = store.annotation(page_id)
            if not doc:
                continue
            store.source(page_id)
            validate_annotation(doc, page)
            if args.command == "export" and (any(p["reviewed"] for p in doc["parts"]) or
                    any(s.get("reviewed") for s in doc.get("subwords", {}).values())):
                destination = (args.output or args.data_dir / "dataset") / page["split"] / page_id
                export_page(doc, destination, page,load_scope(store.data_dir/'automatic',page_id,page['sha256']))
                exported.append({"pageId": page_id, "split": page["split"], "directory": str(destination.resolve())})
            print(f"Valid: {page_id}, revision {doc['revision']}")
        if args.command == "export":
            destination = args.output or args.data_dir / "dataset"
            atomic_json(destination / "manifest.json", {"schemaVersion": 1, "classes": ROLES,
                        "background": 0, "ignoreIndex": 255, "pages": exported})
            print(f"Exported {len(exported)} pages with reviewed annotations to {destination}")


if __name__ == "__main__":
    main()
