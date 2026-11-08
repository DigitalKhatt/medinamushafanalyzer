"""Read the analyzer's actual initialized transcript, never cached word text."""
import json
import os
from pathlib import Path
import re
import subprocess

from .common import digest_file


def validate_snapshot(snapshot, source):
    raw = {f'page{int(n)+1:03}': text for n, text in
           re.findall(r'R"page(\d+)\((.*?)\)page\1"', source, re.S)}
    if len(raw) != 604:
        raise ValueError('Expected all 604 native quran.cpp raw transcript pages')
    if snapshot.get('producer') != 'medinamushafanalyzer-QuranTextByWord-v1':
        raise ValueError('Transcript must come from the native QuranTextByWord exporter')
    pages = snapshot.get('pages', [])
    if len(pages) != 604 or {p['pageId'] for p in pages} != set(raw):
        raise ValueError('Native transcript export must cover exactly 604 pages')
    words = {}
    for page in pages:
        pid = page['pageId']
        if page['rawText'] != raw[pid]:
            raise ValueError(f'{pid}: native binary transcript differs from current quran.cpp; rebuild the analyzer')
        words[pid] = {}
        for word in page['words']:
            if (word['id'] != f"{pid}:l{word['line']}-w{word['ordinal']}" or
                    word['id'] in words[pid] or
                    ''.join(c['text'] for c in word['characters']) != word['text']):
                raise ValueError('Invalid native transcript word or character records')
            words[pid][word['id']] = word
    return words


def export_transcript(native, output, *, binary=None):
    native, output = Path(native), Path(output)
    binary = Path(binary) if binary is not None else native/'build/ninja-nmc/Debug/medinamushafanalyzer'
    if not binary.exists():
        raise ValueError(f'Build the native transcript exporter at {binary} first')
    for filename in ('quran.cpp', 'pageanalysisresult.cpp', 'proposalexport.cpp', 'main.cpp'):
        if (native/filename).stat().st_mtime > binary.stat().st_mtime:
            raise ValueError(f'Native transcript binary predates {filename}; rebuild medinamushafanalyzer')
    subprocess.run([str(binary), '--export-transcript', str(output), '--pages', 'all'],
                   cwd=native, env={**os.environ, 'QT_QPA_PLATFORM':'offscreen'},
                   check=True, capture_output=True)
    snapshot = json.loads(output.read_text())
    words = validate_snapshot(snapshot, (native/'quran.cpp').read_text())
    provenance = dict(source=str(native/'quran.cpp'),
                      initialization='PageAnalysisResult::initQuranText / QuranTextByWord',
                      transcriptSha256=digest_file(native/'quran.cpp'),
                      initializationSha256=digest_file(native/'pageanalysisresult.cpp'),
                      snapshotSha256=digest_file(output), binarySha256=digest_file(binary))
    return words, provenance


def subword_characters(native_word, subwords):
    """Align saved subwords to native characters without inventing boundaries."""
    result = {}
    offset = 0
    for sub in subwords:
        text = sub['text']
        if native_word['text'][offset:offset+len(text)] != text:
            return {}  # explicit mismatch; never manufacture a new transcript
        result[sub['id']] = native_word['characters'][offset:offset+len(text)]
        offset += len(text)
    # These word-level signs are deliberately outside letter subword bodies.
    if any(c not in '۩۞' for c in native_word['text'][offset:]):
        return {}
    return result
