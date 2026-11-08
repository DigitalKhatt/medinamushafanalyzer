#!/usr/bin/env python3
"""Freeze small geometric templates; no classifier predictions or reviews are read."""
import argparse
import hashlib
import json
from pathlib import Path


def build(font_file, inputs):
    from PySide6.QtCore import QFile, QIODevice, QDataStream, Qt
    from PySide6.QtGui import QPainterPath, QImage, QPainter

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    font_file, inputs = Path(font_file), Path(inputs)
    file = QFile(str(font_file))
    if not file.open(QIODevice.ReadOnly):
        raise ValueError('Cannot open native font')
    stream = QDataStream(file)
    if stream.readDouble() != 1:
        raise ValueError('Unsupported native font serialization')
    templates = []
    wanted = {'sad.waqf', 'hah.waqf', 'onedot.waqf', 'twodots.waqf', 'onedot'}
    for _ in range(stream.readUInt32()):
        name = stream.readQString(); stream.readQString()
        path = QPainterPath(); stream >> path
        if name not in wanted:
            continue
        box = path.boundingRect()
        image = QImage(16, 16, QImage.Format_RGB32); image.fill(Qt.white)
        painter = QPainter(image)
        painter.scale(16/box.width(), 16/box.height())
        painter.translate(-box.left(), -box.top())
        painter.fillPath(path, Qt.black); painter.end()
        bits = 0
        for y in range(16):
            for x in range(16):
                bits = (bits << 1) | (image.pixelColor(x, y).red() < 128)
        templates.append(dict(name=name, signature=f'{bits:064x}', aspect=box.width()/box.height(),
            heightPdf=box.height()/66, source=dict(file='input/font.dat', sha256=digest(font_file), glyph=name)))
    if stream.status() != QDataStream.Ok or not file.atEnd():
        raise ValueError('Invalid native font serialization')
    # The meem exemplar already belongs to the frozen geometric shape bank.
    # The qaf body fills a missing component in the original eight-template font.
    # Both are explicit, reproducible geometry examples, not training approvals.
    for name, pid, path_id in [('meem.waqf', 'page130', 'l4-w9-p13'),
                               ('qaf.waqf.body', 'page100', 'l14-w10-p8')]:
        raw_file = inputs/f'{pid}.raw.json'; geo_file = inputs/'geometry'/f'{pid}.json'
        raw = json.loads(raw_file.read_text()); geometry = json.loads(geo_file.read_text())
        if geometry['rawInputSha256'] != digest(raw_file):
            raise ValueError('Geometry does not belong to frozen native paths')
        path = next(p for p in raw['paths'] if p['id'] == path_id)
        shape = geometry['paths'][path_id]
        templates.append(dict(name=name, signature=shape['signature'],
            aspect=path['bbox'][2]/path['bbox'][3], heightPdf=path['bbox'][3]/raw['provenance']['pdfToRaster'][3],
            source=dict(pageId=pid, pathId=path_id, rawInputSha256=digest(raw_file), geometrySha256=digest(geo_file))))
    return dict(schemaVersion=1, signatureSize=16, templates=sorted(templates, key=lambda t:t['name']),
        policy='Frozen geometric templates only. No learned labels, saved owner assignments, reviews or training approvals. One-pixel signature tolerance changes matching only; source ink is never dilated.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, default=Path('input/font.dat'))
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1]/'waqf_templates.json')
    args = parser.parse_args()
    args.output.write_text(json.dumps(build(args.font, args.inputs), ensure_ascii=False, sort_keys=True, indent=2)+'\n')
    print(args.output)
