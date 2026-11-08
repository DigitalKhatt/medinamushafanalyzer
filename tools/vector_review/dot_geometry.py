"""Scale-independent dot layout checks for this vector Mushaf edition."""


def has_counter(component):
    """Find enclosed empty pixels without filling or changing any source ink."""
    ink=set(component)
    if not ink:return False
    ys,xs=zip(*ink);top,bottom=min(ys)-1,max(ys)+1;left,right=min(xs)-1,max(xs)+1
    empty={(y,x) for y in range(top,bottom+1) for x in range(left,right+1)}-ink
    pending=[(top,left)];empty.remove(pending[0])
    while pending:
        y,x=pending.pop()
        for point in ((y-1,x),(y+1,x),(y,x-1),(y,x+1)):
            if point in empty:empty.remove(point);pending.append(point)
    return bool(empty)


def triple_layout(single, pair):
    """One dot sits above a wider joined pair, with approximately aligned centers.

    Counting paths or testing distance alone can join dots of adjacent letters.
    This checks their arrangement; it does not assign character ownership.
    """
    sx,sy,sw,sh=single;px,py,pw,ph=pair
    if min(sw,sh,pw,ph)<=0:return False
    if not .55<=sw/sh<=1.6 or not 1.2<=pw/ph<=2.8:return False
    if not 1.3<=pw/sw<=2.8 or not .5<=ph/sh<=1.8:return False
    dx=abs(sx+sw/2-px-pw/2);dy=py+ph/2-sy-sh/2
    return dx<=pw*.5 and sh*.45<=dy<=sh*2.2
