"""Conservative geometry for detached fatha and kasra stroke candidates."""
import math


def connected_ink(runs, width):
    ink = {divmod(pixel, width) for start, count in zip(runs[::2], runs[1::2])
           for pixel in range(start, start + count)}
    pieces = []
    while ink:
        pending = [ink.pop()]
        component = []
        while pending:
            y, x = pending.pop()
            component.append((y, x))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    neighbor = (y + dy, x + dx)
                    if neighbor in ink:
                        ink.remove(neighbor)
                        pending.append(neighbor)
        pieces.append(sorted(component))
    return sorted(pieces, key=lambda component: component[0])


def stroke(component):
    if len(component) < 8:
        return None
    ys, xs = zip(*component)
    x, y = sum(xs) / len(xs), sum(ys) / len(ys)
    xx = sum((v - x) ** 2 for v in xs)
    yy = sum((v - y) ** 2 for v in ys)
    xy = sum((a - x) * (b - y) for a, b in zip(xs, ys))
    spread = math.hypot(xx - yy, 2 * xy)
    major, minor = xx + yy + spread, xx + yy - spread
    angle = math.atan2(2 * xy, xx - yy) / 2
    if major < 9 * max(minor, 1) or not -.85 <= angle <= .12:
        return None
    return x, y, angle, math.sqrt(major / len(xs))


def stroke_pair(paths, staggered=False):
    if len(paths) != 2 or any(path.get('stroke') is None for path in paths):
        return False
    first, second = (path['stroke'] for path in paths)
    x1, y1, angle1, length1 = first
    x2, y2, angle2, length2 = second
    angle = (angle1 + angle2) / 2
    dx, dy = x2 - x1, y2 - y1
    along = abs(dx * math.cos(angle) + dy * math.sin(angle))
    across = abs(dy * math.cos(angle) - dx * math.sin(angle))
    scale = min(length1, length2)
    compact = (abs(angle1 - angle2) <= .23 and max(length1, length2) <= 1.6 * scale
               and along <= 2.25 * scale and .25 * scale <= across <= 1.5 * scale)
    if compact:
        return True
    if not staggered or any('bbox' not in path for path in paths):
        return False
    upper, lower = sorted((path['bbox'] for path in paths), key=lambda box: box[1])
    gap = lower[1] - (upper[1] + upper[3])
    overlap = min(upper[0]+upper[2], lower[0]+lower[2]) - max(upper[0], lower[0])
    # Staggered strokes are stacked, with overlapping horizontal spans. Two
    # side-by-side kasras can share the same along/across distances; they must
    # not acquire this longer reach. This test reads geometry only.
    return (gap >= 0 and overlap > 0 and dx * dy < 0 and abs(angle1 - angle2) <= .08
            and max(length1, length2) <= 1.15 * scale
            and along <= 2.5 * scale and .75 * scale <= across <= 1.25 * scale)


def stroke_paths(path, width, split):
    """Expose disconnected strokes while keeping each component's filled ink intact."""
    pieces = connected_ink(path['runs'], width)
    profiles = [stroke(component) for component in pieces]
    if len(pieces) == 1:
        return [{**path, 'stroke': profiles[0]}]
    if not split or not pieces or any(profile is None for profile in profiles):
        return [path]
    return [
        {**component_path(path, width, component, f"stroke-{index}"), 'stroke': profile}
        for index, (component, profile) in enumerate(zip(pieces, profiles), 1)
    ]


def component_path(path, width, component, suffix):
    ys, xs = zip(*component)
    runs = []
    for y, x in component:
        offset = y * width + x
        if runs and offset == runs[-2] + runs[-1]:
            runs[-1] += 1
        else:
            runs.extend((offset, 1))
    return {
        **path, 'id': f"{path['id']}-{suffix}", 'sourcePathId': path['id'],
        'bbox': [min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1],
        'runs': runs, 'contourCount': 1, 'templateScores': {}, 'signature': '0',
    }
