"""Choose one shared boundary through a connector for the colored page."""
import re
from math import ceil, hypot, sqrt


def center(points):
    return tuple(sum(p[k] for p in points)/len(points) for k in (0,1))


def blend(a, b, t):
    return tuple(x+(y-x)*t for x,y in zip(a,b))


def delta(a, b):
    return (a[0]-b[0], a[1]-b[1])


def dot(a, b):
    return a[0]*b[0]+a[1]*b[1]


def outline_edges(path):
    """Flatten the catalog's absolute M/L/C outlines for ink-width measurement."""
    edges = []
    first = last = None
    for command, coordinates in re.findall(r'([MLC])([^MLC]*)', path):
        values = tuple(map(float, coordinates.split()))
        end = values[-2:]
        if command == 'M':
            if first is not None and last != first:
                edges.append((last, first))
            first = end
        elif command == 'L':
            edges.append((last, end))
        else:
            x0,y0 = last
            x1,y1,x2,y2,x3,y3 = values
            # Linear interpolation error is at most max|B″| / (8 n²).
            # Keep chords within 0.005 raster pixels of the original curve.
            curvature = max(hypot(x0-2*x1+x2, y0-2*y1+y2), hypot(x1-2*x2+x3, y1-2*y2+y3))
            steps = max(1, ceil(sqrt(.75 * curvature / .005)))
            for i in range(1, steps):
                t = i / steps
                s = 1 - t
                a,b,c,d = s**3, 3*s*s*t, 3*s*t*t, t**3
                point = (a*x0+b*x1+c*x2+d*x3, a*y0+b*y1+c*y2+d*y3)
                edges.append((last, point))
                last = point
            edges.append((last, end))
        last = end
    if first is not None and last != first:
        edges.append((last, first))
    return edges


def section_span(edges, origin, direction):
    # Intersect a cross-section with the two flanks, counting each vertex once.
    crossings = []
    ox,oy = origin
    dx,dy = direction
    offset = dx*oy - dy*ox
    for (ax,ay),(bx,by) in edges:
        u = dx*ay - dy*ax - offset
        v = dx*by - dy*bx - offset
        if (u>0) != (v>0):
            t = u / (u-v)
            crossings.append(dx*(ax-ox+t*(bx-ax)) + dy*(ay-oy+t*(by-ay)))
    crossings.sort()
    spans = list(zip(crossings[::2],crossings[1::2]))
    if not spans:
        return None
    return min(spans,key=lambda span: max(span[0],-span[1],0))


def horizontal_midpoint(caps, edges, travel, chord_center):
    if not edges or not travel[0]:
        return None
    xs = [point[0] for edge in edges for point in edge]
    left, right = min(xs), max(xs)
    x = (left+right)/2
    step = min(1, (right-left)*.05)
    if step < 1e-7:
        return None

    # The center of the actual ink at this x can be far from the chord between
    # the two caps. Measure the local centerline on either side of that point.
    y = chord_center[1]
    span = section_span(edges,(x,y),(0,1))
    if span is None:
        return None
    y += sum(span)/2
    before = section_span(edges,(x-step,y),(0,1))
    after = section_span(edges,(x+step,y),(0,1))
    if before is None or after is None:
        return None
    rise = (sum(after)-sum(before))/2
    length = hypot(2*step,rise)
    tangent = (2*step/length,rise/length)
    if dot(tangent,travel)<0:
        tangent = (-tangent[0],-tangent[1])
    point = (x,y)
    # A cut must leave each attachment intact. For steep or bent joins, the
    # estimated normal may cross an end cap instead of the two flanks.
    horizontal = (1 if travel[0]>0 else -1,0)
    for axis in (tangent,horizontal):
        if (all(dot(delta(p,point),axis)<=0 for p in caps[0]) and
                all(dot(delta(p,point),axis)>=0 for p in caps[1])):
            return point,axis
    return None


def choose_boundary(caps, edges):
    # Cap zero touches the preceding (right-hand in ordinary RTL) letter.
    a,b = caps
    first,last = center(a),center(b)
    travel = delta(last,first)
    point = blend(first,last,.5)
    midpoint = horizontal_midpoint(caps,edges,travel,point)
    if midpoint:
        point,axis = midpoint
        return point,axis,'midpoint'

    # A very steep join can have both caps at the same x. Use the middle
    # between its cap centers and the average cap direction in that case.
    da,db = delta(a[1],a[0]),delta(b[1],b[0])
    if dot(da,db)<0:
        db = (-db[0],-db[1])
    vx,vy = blend(da,db,.5)
    length = hypot(vx,vy)
    if length<1e-7:
        vx,vy = -travel[1],travel[0]
        length = hypot(vx,vy)
    axis = (-vy/length,vx/length) if length else (-1,0)
    if dot(axis,travel)<0:
        axis = (-axis[0],-axis[1])
    return point,axis,'midpoint'


def half_plane(point, axis, width, height):
    rect = [(0,0),(width,0),(width,height),(0,height)]
    poly = []
    for a,b in zip(rect,rect[1:]+rect[:1]):
        u,v = dot(delta(a,point),axis),dot(delta(b,point),axis)
        if u>=0: poly.append(a)
        if (u>=0)!=(v>=0): poly.append(blend(a,b,u/(u-v)))
    return ' '.join(f'{x:.5f},{y:.5f}' for x,y in poly)
