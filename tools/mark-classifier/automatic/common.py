"""Shared geometry, features, and provenance for the automatic vector corpus."""
from __future__ import annotations
import hashlib
import json
import math
from pathlib import Path
import unicodedata
import zlib

FEATURE_DIM=528  # 16x16 mask, 16x16 local source context, 16 geometry values.


def input_root(root):
    """Use current workbench inputs when served there, otherwise frozen inputs."""
    root=Path(root)
    if (root.parent/'corpus.json').exists():return root.parent
    if (root/'inputs/corpus.json').exists():return root/'inputs'
    return root.parent


def plain(text):
    text=''.join(c for c in unicodedata.normalize('NFD',text) if not unicodedata.combining(c))
    return text.translate(str.maketrans({'ٱ':'ا','ک':'ك','ی':'ي','ـ':None,'ۥ':None,'ۦ':None}))


def digest_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pack_runs(runs):
    return zlib.compress(json.dumps(runs,separators=(',',':')).encode(),6)


def unpack_runs(value):
    return json.loads(zlib.decompress(value))


def merge_runs(parts):
    result=[]
    for start,length in sorted((s,n) for p in parts for s,n in zip(p['runs'][::2],p['runs'][1::2])):
        if result and result[-2]+result[-1]==start:result[-1]+=length
        else:result.extend((start,length))
    return result


def bounding_box(parts):
    if not parts:return [0,0,0,0]
    boxes=[p['bbox'] for p in parts];x=min(b[0] for b in boxes);y=min(b[1] for b in boxes)
    return [x,y,max(b[0]+b[2] for b in boxes)-x,max(b[1]+b[3] for b in boxes)-y]


def local_mask(runs,bbox,width):
    import numpy as np
    x,y,w,h=bbox
    result=np.zeros((h,w),dtype=np.uint8)
    for start,length in zip(runs[::2],runs[1::2]):
        while length:
            py,px=divmod(start,width);n=min(length,width-px)
            if y<=py<y+h:
                left=max(px,x);right=min(px+n,x+w)
                if right>left:result[py-y,left-x:right-x]=255
            start+=n;length-=n
    return result


def ink_components(runs,bbox,width):
    """Count actual eight-connected ink, independently of annotation grouping."""
    import cv2
    return int(cv2.connectedComponents(local_mask(runs,bbox,width),connectivity=8)[0]-1)


def feature_vector(runs,bbox,source,baseline,components=1,scale=4):
    import cv2
    import numpy as np
    x,y,w,h=bbox
    mask=local_mask(runs,bbox,source.shape[1]);normalized=cv2.resize(mask,(16,16),interpolation=cv2.INTER_AREA)
    # Context is visual, never transcript characters or the teacher label.
    pad=max(12,min(80,round(max(w,h)*.65)))
    x0=max(0,x-pad);y0=max(0,y-pad);x1=min(source.shape[1],x+w+pad);y1=min(source.shape[0],y+h+pad)
    context=cv2.resize(source[y0:y1,x0:x1]*255,(16,16),interpolation=cv2.INTER_AREA)
    area=sum(runs[1::2]);sx=max(w/scale,.01);sy=max(h/scale,.01)
    cy=y+h/2;relative=(cy-baseline)/scale
    values=[math.log1p(sx)/5,math.log1p(sy)/5,math.log1p(area/(scale*scale))/9,
        .5+math.log(max(.01,sx/sy))/10,area/max(1,w*h),.5+relative/48,
        components/8,min(w,h)/max(1,w,h),math.log1p(w*h)/14,
        float(cy>baseline),float(y+h<baseline),float(y>baseline),
        np.count_nonzero(mask[0])/max(1,w),np.count_nonzero(mask[-1])/max(1,w),
        np.count_nonzero(mask[:,0])/max(1,h),np.count_nonzero(mask[:,-1])/max(1,h)]
    geometry=np.asarray(np.clip(values,0,1)*255,dtype=np.uint8)
    return np.concatenate((normalized.ravel(),context.ravel(),geometry))


def mask_path(runs,width,dx=0,dy=0):
    chunks=[]
    for start,length in zip(runs[::2],runs[1::2]):
        while length:
            y,x=divmod(start,width);n=min(length,width-x)
            chunks.append(f'M{x-dx} {y-dy}h{n}v1h-{n}z')
            start+=n;length-=n
    return ''.join(chunks)
