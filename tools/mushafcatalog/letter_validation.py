"""Check medial lām body heights against a minimum in native page units."""
from itertools import groupby
import json
from math import isfinite, sqrt

from page_join import outline_commands
from page_view import decode_vectors, matching_part
from store import require


def cubic_vertical_extrema(start, control1, control2, end):
    # The derivative is three times the quadratic with these coefficients.
    first, middle, last = control1-start, control2-control1, end-control2
    a, b, c = first-2*middle+last, 2*(middle-first), first
    if a == 0:
        roots = (-c/b,) if b else ()
    else:
        discriminant = b*b-4*a*c
        roots = ((-b-sqrt(discriminant))/(2*a), (-b+sqrt(discriminant))/(2*a)) if discriminant >= 0 else ()
    for t in roots:
        if 0 < t < 1:
            s = 1-t
            yield s**3*start+3*s*s*t*control1+3*s*t*t*control2+t**3*end


def outline_height(path, transform):
    # Invert the linear part of the raster transform. Translation cancels
    # when taking the difference between the vertical extrema.
    a, b, c, d, _, _ = transform
    determinant = a*d-b*c
    require(isfinite(determinant) and determinant != 0, 'Invalid page-to-raster transform')
    vertical = []
    end = None
    for command, points in outline_commands(path):
        heights = [(a*y-b*x)/determinant for x,y in zip(points[::2],points[1::2])]
        if command == 'C':
            vertical.extend(cubic_vertical_extrema(end, *heights))
        end = heights[-1]
        vertical.append(end)
    require(vertical and all(map(isfinite,vertical)), 'Missing or invalid body outline')
    return max(vertical)-min(vertical)


def validate_medial_lam_height(db):
    minimum = 5
    rows = db.execute("""SELECT s.page_id, o.id, o.subword_id, p.variant, p.runs
        FROM catalog_owners o JOIN catalog_subwords s ON s.id=o.subword_id
        LEFT JOIN catalog_parts p ON p.owner_id=o.id AND p.role='body'
        WHERE o.character='ل' AND o.form='medial' AND o.role='letter'
        ORDER BY s.page_id, o.id""")
    measured = 0
    shortest = None
    below_minimum, unmeasured = [], []
    for page, letters in groupby(rows, key=lambda row: row[0]):
        coordinates = db.execute('SELECT pdf_to_raster FROM catalog_coordinates WHERE page_id=?', (page,)).fetchone()
        art = db.execute('SELECT vectors FROM catalog_page_art WHERE page_id=?', (page,)).fetchone()
        vectors = decode_vectors(art[0] if art else None)
        transform = json.loads(coordinates[0]) if coordinates else None
        for _, owner, subword, variant, runs in letters:
            letter = dict(id=owner, subword=subword, variant=variant)
            outline = matching_part(vectors,owner,'body',runs) if runs is not None else None
            reason = None
            if runs is None:
                reason = 'missing-body'
            elif not outline:
                reason = 'missing-verified-outline'
            elif not transform:
                reason = 'missing-page-transform'
            if reason:
                unmeasured.append(dict(letter, reason=reason))
                continue
            height = outline_height(outline['d'], transform)
            measured += 1
            if shortest is None or height < shortest['height']:
                shortest = dict(letter, height=height)
            if height < minimum:
                below_minimum.append(dict(letter, height=height))
    return dict(passed=measured > 0 and not below_minimum and not unmeasured,
                minimumHeight=minimum, units='page', total=measured+len(unmeasured), measured=measured,
                shortest=shortest, belowMinimum=sorted(below_minimum,key=lambda item: (item['height'],item['id'])),
                unmeasured=unmeasured)
