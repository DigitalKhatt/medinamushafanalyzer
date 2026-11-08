"""Detect an open medial-hah structure from raw SVG ink, without route templates.

Input is ONLY a path and a general shape configuration. The raster skeleton is a
locator, not the final segmentation. A native geometric bridge subsequently snaps
the proposed normals to the original cubic contour and partitions its ink.
"""
from collections import defaultdict
import itertools
import math

import numpy as np
from scipy import ndimage as ndi
from skimage.morphology import skeletonize
from PySide6.QtCore import Qt, QRect
from PySide6.QtGui import QImage, QPainter, QPainterPath, QTransform


def painter_path(encoded):
    path=QPainterPath()
    path.setFillRule(Qt.FillRule.WindingFill if encoded.get('fillRule')=='nonzero' else Qt.FillRule.OddEvenFill)
    for op in encoded['commands']:
        if op[0]=='M':path.moveTo(*op[1:])
        elif op[0]=='L':path.lineTo(*op[1:])
        elif op[0]=='C':path.cubicTo(*op[1:])
        else:raise ValueError('Unsupported vector command')
    return path


def rasterize(encoded, config):
    path=painter_path(encoded);box=path.boundingRect();padding=config['padding']
    scale=config['longestSide']/max(box.width(),box.height())
    width=math.ceil(box.width()*scale)+2*padding+1;height=math.ceil(box.height()*scale)+2*padding+1
    origin=np.array([box.x()-padding/scale,box.y()-padding/scale])
    image=QImage(width,height,QImage.Format.Format_Grayscale8);image.fill(0)
    painter=QPainter(image);painter.setPen(Qt.PenStyle.NoPen);painter.setBrush(Qt.GlobalColor.white)
    painter.scale(scale,scale);painter.translate(-origin[0],-origin[1]);painter.drawPath(path);painter.end()
    mask=np.frombuffer(image.constBits(),np.uint8).reshape(height,image.bytesPerLine())[:,:width].copy()>127
    return mask,origin,scale


def skeleton_graph(skeleton):
    """Contract junction pixel clusters; remove redundant diagonal triangle edges."""
    yy,xx=np.nonzero(skeleton);pixels=set(zip(yy.tolist(),xx.tolist()))
    adjacent={}
    for y,x in sorted(pixels):
        neighbors=[]
        for dy,dx in itertools.product((-1,0,1),repeat=2):
            if not (dy or dx) or (y+dy,x+dx) not in pixels:continue
            if dy and dx and ((y+dy,x) in pixels or (y,x+dx) in pixels):continue
            neighbors.append((y+dy,x+dx))
        adjacent[(y,x)]=neighbors
    special={p for p,ns in adjacent.items() if len(ns)!=2};lookup={};nodes=[]
    for start in sorted(special):
        if start in lookup:continue
        number=len(nodes);pending=[start];component=[];lookup[start]=number
        while pending:
            p=pending.pop();component.append(p)
            for q in adjacent[p]:
                if q in special and q not in lookup:lookup[q]=number;pending.append(q)
        nodes.append({'xy':np.mean(np.array(component)[:,::-1],axis=0),'pixels':component,'edges':[]})
    visited=set();edges=[]
    for start,node_id in sorted(lookup.items()):
        for neighbor in adjacent[start]:
            key=tuple(sorted((start,neighbor)))
            if key in visited or neighbor in lookup:continue
            visited.add(key);chain=[start,neighbor];previous,current=start,neighbor
            while current not in lookup:
                ns=[p for p in adjacent[current] if p!=previous]
                if len(ns)!=1:break
                nxt=ns[0];visited.add(tuple(sorted((current,nxt))));chain.append(nxt);previous,current=current,nxt
            if current not in lookup:continue
            other=lookup[current]
            points=np.array([nodes[node_id]['xy'],*np.array(chain)[:,::-1],nodes[other]['xy']],float)
            keep=np.r_[True,np.linalg.norm(np.diff(points,axis=0),axis=1)>1e-8];points=points[keep]
            edge={'nodes':(node_id,other),'points':points,'length':float(np.linalg.norm(np.diff(points,axis=0),axis=1).sum())}
            idx=len(edges);edges.append(edge);nodes[node_id]['edges'].append(idx);nodes[other]['edges'].append(idx)
    return nodes,edges


def oriented(edge, node):
    return edge['points'] if edge['nodes'][0]==node else edge['points'][::-1]


def arc_lengths(points):
    return np.r_[0,np.cumsum(np.linalg.norm(np.diff(points,axis=0),axis=1))]


def at_distance(points, distance):
    lengths=arc_lengths(points);distance=float(np.clip(distance,0,lengths[-1]));i=max(0,min(len(points)-2,int(np.searchsorted(lengths,distance)-1)))
    t=(distance-lengths[i])/max(1e-9,lengths[i+1]-lengths[i])
    return points[i]*(1-t)+points[i+1]*t


def at_x(points, x):
    lengths=arc_lengths(points)
    for i,(a,b) in enumerate(zip(points,points[1:])):
        if abs(b[0]-a[0])<1e-8:continue
        t=(x-a[0])/(b[0]-a[0])
        if 0<=t<=1:return a+t*(b-a),float(lengths[i]+t*(lengths[i+1]-lengths[i]))
    return None


def normal_probe(points, distance, stroke, settings, origin, scale, half_length):
    lengths=arc_lengths(points)
    if distance<=0 or distance>=lengths[-1]:return None
    point=at_distance(points,distance)
    before=at_distance(points,distance-settings['tangentWindow']*stroke)
    after=at_distance(points,distance+settings['tangentWindow']*stroke)
    tangent=after-before;norm=np.linalg.norm(tangent)
    if norm<1e-6:return None
    normal=np.array([-tangent[1],tangent[0]])/norm
    world=lambda p:(p/scale+origin).tolist()
    return {'center':world(point),'normal':normal.tolist(),'stroke':stroke/scale,
            'line':[world(point-half_length*stroke*normal),world(point+half_length*stroke*normal)]}


def graph_paths(nodes, edges, start, limit, forbidden=()):
    """Bounded simple paths; topology traversal, not directional contour recipes."""
    pending=[([start],[],np.array([nodes[start]['xy']]))]
    while pending:
        node_path,edge_path,points=pending.pop()
        if edge_path:yield node_path,edge_path,points
        if len(edge_path)>=limit:continue
        node=node_path[-1]
        for ei in sorted(nodes[node]['edges'],reverse=True):
            edge=edges[ei];other=edge['nodes'][1] if edge['nodes'][0]==node else edge['nodes'][0]
            if other in node_path or other in forbidden:continue
            pending.append((node_path+[other],edge_path+[ei],np.vstack([points,oriented(edge,node)[1:]])))


def contour_corners(encoded, minimum_turn):
    """Cubic endpoint tangent discontinuities, independent of curve numbering."""
    segments=[];current=None;start=None;result=[]
    for op in encoded['commands']:
        if op[0]=='M':
            if segments:break  # Outer contour only.
            current=np.array(op[1:],float);start=current
        elif op[0]=='L':
            end=np.array(op[1:],float);segments.append((current,end-current,end,end-current));current=end
        elif op[0]=='C':
            a,b,end=np.array(op[1:],float).reshape(3,2);segments.append((current,a-current,end,end-b));current=end
    for previous,following in zip(segments,segments[1:]+segments[:1]):
        if np.linalg.norm(previous[2]-following[0])>1e-5:continue
        a,b=previous[3],following[1];denom=np.linalg.norm(a)*np.linalg.norm(b)
        if denom<1e-12:continue
        angle=math.degrees(math.acos(np.clip(np.dot(a,b)/denom,-1,1)))
        if angle>=minimum_turn:result.append({'xy':previous[2].tolist(),'turn':angle})
    return result


def corner_probes(points, attachment, stroke, settings, corners, corner_settings, origin, scale, half_length):
    probes=[];lengths=arc_lengths(points)
    for corner in corners:
        pixel=(np.array(corner['xy'])-origin)*scale
        if np.linalg.norm(pixel-attachment)>corner_settings['maxAttachmentDistance']*stroke:continue
        i=int(np.linalg.norm(points-pixel,axis=1).argmin())
        probe=normal_probe(points,float(np.clip(lengths[i],1e-5,lengths[-1]-1e-5)),stroke,settings,origin,scale,half_length)
        if probe is None:continue
        probe['center']=corner['xy'];n=np.array(probe['normal']);center=np.array(corner['xy']);s=stroke/scale
        probe['line']=[(center-half_length*s*n).tolist(),(center+half_length*s*n).tolist()]
        probe['cornerTurn']=corner['turn'];probes.append(probe)
    return probes


def cap_points(nodes, edges, attachment, edge_id, forbidden, stroke, settings):
    """A short flat terminal extension belongs to the head, not an attachment."""
    paths=[(ns,es,ps) for ns,es,ps in graph_paths(nodes,edges,attachment,settings['maxEdges'],forbidden)
           if es[0]==edge_id]
    if not paths:return None
    ends=[(ns,es,ps) for ns,es,ps in paths if len(nodes[ns[-1]]['edges'])==1]
    frontier=[ns for ns,es,ps in paths if len(es)==settings['maxEdges'] and len(nodes[ns[-1]]['edges'])>1]
    if not ends or frontier:return None
    points=np.vstack([ps for _,_,ps in ends]);height=np.ptp(points[:,1]);width=np.ptp(points[:,0]);length=max(arc_lengths(ps)[-1] for _,_,ps in ends)
    return points if height<=settings['maxHeight']*stroke and length<=settings['maxLength']*stroke and width>=settings['minWidthToHeight']*max(1.,height) else None


def attachment_options(nodes, edges, attachment, edge_id, forbidden, stroke, config):
    """Separate a head extension from the neck attaching alongside its terminal cap.

    The mouth root can precede the actual attachment. A short, nearly horizontal
    head stroke may continue to a cap while the neck leaves it above or below.
    Its own graph locates that junction; no adjacent letter or previous cut is used.
    """
    edge=edges[edge_id];other=edge['nodes'][1] if edge['nodes'][0]==attachment else edge['nodes'][0]
    forward=oriented(edge,attachment);delta=forward[-1]-forward[0];refined=[]
    if (edge['length']<=config['attachment']['maxHeadLength']*stroke and
            abs(delta[1])<=config['attachment']['maxHeadRise']*stroke and delta[0]>.3*stroke and
            len(nodes[other]['edges'])>=3):
        remaining=[ei for ei in nodes[other]['edges'] if ei!=edge_id]
        for cap_id in remaining:
            cap=cap_points(nodes,edges,other,cap_id,set(forbidden)|{attachment},stroke,config['cap'])
            # Tiny diagonal skeleton spurs at a blunt end also form a cap.
            cap_edge=edges[cap_id];cap_node=cap_edge['nodes'][1] if cap_edge['nodes'][0]==other else cap_edge['nodes'][0]
            if cap is None and len(nodes[cap_node]['edges'])==1 and cap_edge['length']<stroke:
                cap=oriented(cap_edge,other)
            if cap is None:continue
            for neck_id in remaining:
                if neck_id==cap_id:continue
                neck=oriented(edges[neck_id],other)
                end=edges[neck_id]['nodes'][1] if edges[neck_id]['nodes'][0]==other else edges[neck_id]['nodes'][0]
                if len(nodes[end]['edges'])==1 and edges[neck_id]['length']<stroke:continue
                if cap_points(nodes,edges,other,neck_id,set(forbidden)|{attachment},stroke,config['cap']) is not None:continue
                mid=at_distance(neck,min(stroke,arc_lengths(neck)[-1]))-neck[0]
                if abs(mid[1])<.3*stroke:continue
                refined.append((other,neck,cap))
    return refined or [(attachment,forward,None)]


def attachment_stroke_end(points, stroke, settings, tangent_window):
    """Keep the attaching stroke with hah until its first rightward bend.

    Travel outward from the head. A descending or rising attachment belongs to
    the head through its turn into the joining stroke. Search only that local
    branch, stopping before a terminal/junction or the configured length limit;
    neither a neighboring letter landmark nor a previous boundary is consulted.
    An already rightward neck has no such bend and keeps its short-neck rule.
    """
    length=arc_lengths(points)[-1]
    window=tangent_window*stroke
    limit=min(settings['maxLength']*stroke,length-window)
    minimum=settings['minLength']*stroke
    if limit<=minimum:return None

    def angle(distance):
        delta=at_distance(points,distance+window)-at_distance(points,distance-window)
        return math.degrees(math.atan2(delta[1],delta[0]))

    initial=angle(min(window,length/2))
    if not settings['initialMinAngle']<=abs(initial)<135:return None
    step=.05*stroke
    stability=settings['stabilityLength']*stroke
    # Require a sustained rightward heading, not a single raster staircase.
    for distance in np.arange(minimum,limit-stability+step/2,step):
        if all(abs(angle(d))<=settings['turnAngle']
               for d in np.linspace(distance,distance+stability,5)):
            return float(distance)
    return None


def detect(encoded, config):
    """Return structural detections and candidate probes; no transcript or labels."""
    mask,origin,scale=rasterize(encoded,config['raster'])
    distance=ndi.distance_transform_edt(mask);skeleton=skeletonize(mask)
    nodes,edges=skeleton_graph(skeleton);detections=[];probes=[];rejected=defaultdict(int)
    corners=contour_corners(encoded,config['corners']['minTurnDegrees'])
    world=lambda p:(np.asarray(p)/scale+origin).tolist()
    seen=set()
    for terminal,node in enumerate(nodes):
        if len(node['edges'])!=1:continue
        for lip_nodes,lip_edges,reverse_lip in graph_paths(nodes,edges,terminal,config['graph']['maxLipJunctions']):
            root=lip_nodes[-1]
            if len(nodes[root]['edges'])<3:continue
            lip=reverse_lip[::-1];lengths=arc_lengths(lip)
            inner=lip[(lengths>.2*lengths[-1])&(lengths<.8*lengths[-1])]
            if not len(inner):continue
            ix=np.clip(np.rint(inner[:,0]).astype(int),0,mask.shape[1]-1);iy=np.clip(np.rint(inner[:,1]).astype(int),0,mask.shape[0]-1)
            stroke=2*float(np.median(distance[iy,ix]))
            if stroke<2:continue
            junction=lip[0];tip=lip[-1];reach=junction[0]-tip[0]
            cfg=config['lip']
            if not cfg['minReach']*stroke<=reach<=cfg['maxReach']*stroke or tip[1]>junction[1]+cfg['maxTipBelowRoot']*stroke:
                rejected['lip-proportions']+=1;continue
            for return_nodes,return_edges,returning in graph_paths(nodes,edges,root,config['graph']['maxReturnEdges'],lip_nodes[:-1]):
                outlet=at_x(returning,tip[0])
                if outlet is None:rejected['return-does-not-reach-mouth']+=1;continue
                before_outlet=returning[arc_lengths(returning)<=outlet[1]]
                if before_outlet[:,0].max()>junction[0]+config['return']['maxRightExcursion']*stroke:continue
                samples=[]
                for fraction in config['opening']['stations']:
                    x=junction[0]-fraction*reach;upper=at_x(lip,x);lower=at_x(returning,x)
                    if upper is None or lower is None:continue
                    u,l=upper[0],lower[0]
                    if l[1]-u[1]<(1+config['opening']['minClearance'])*stroke:continue
                    midpoint=(u+l)/2;px,py=np.rint(midpoint).astype(int)
                    if 0<=py<mask.shape[0] and 0<=px<mask.shape[1] and not mask[py,px]:
                        samples.append({'upper':world(u),'lower':world(l),'air':world(midpoint)})
                if len(samples)<config['opening']['minVisibleStations']:
                    rejected['no-open-mouth']+=1;continue
                for attachment in lip_nodes[1:]:
                  for entry_index in nodes[attachment]['edges']:
                    if entry_index in lip_edges or entry_index in return_edges:continue
                    signature=(tuple(lip_edges),return_edges[0],entry_index)
                    if signature in seen:continue
                    seen.add(signature)
                    cap=cap_points(nodes,edges,attachment,entry_index,set(lip_nodes+return_nodes)-{attachment},stroke,config['cap'])
                    if cap is not None:
                        rejected['terminal-head-extension']+=1;continue
                    options=attachment_options(nodes,edges,attachment,entry_index,set(lip_nodes+return_nodes)-{attachment},stroke,config)
                    # Multiple cap/neck interpretations remain explicit alternatives.
                    for actual_attachment,entering,head_cap in options:
                    # The remaining attachment must leave the mouth root to the right or above.
                      forward=at_distance(entering,min(1.5*stroke,arc_lengths(entering)[-1]))-entering[0]
                      if forward[0]<-.3*stroke and forward[1]>-.3*stroke:
                        rejected['entry-on-mouth-side']+=1;continue
                      features={'root':world(junction),'lipTip':world(tip),'crest':world(lip[np.argmin(lip[:,1])]),
                              'attachment':world(entering[0]),
                              'returnOutlet':world(outlet[0]),'stroke':stroke/scale,'openingReach':reach/stroke,
                              'mouthSamples':samples,'lipBranch':[world(p) for p in lip[::max(1,len(lip)//40)]],
                              'returnBranch':[world(p) for p in returning[::max(1,len(returning)//50)]],
                              'entryBranch':[world(p) for p in entering[::max(1,len(entering)//35)]]}
                      features['attachmentRefined']=actual_attachment!=attachment
                      if head_cap is not None:features['headCap']=[world(p) for p in head_cap[::max(1,len(head_cap)//20)]]
                      attach_settings=config['entry'].get('attachmentStroke')
                      attach_end=attachment_stroke_end(entering,stroke,attach_settings,config['entry']['tangentWindow']) if attach_settings else None
                      if attach_end is not None:
                        features['attachmentStrokeEnd']=world(at_distance(entering,attach_end))
                        features['attachmentStroke']=[world(at_distance(entering,d)) for d in np.linspace(0,attach_end,30)]
                        features['attachmentStrokeLength']=attach_end/stroke
                      features['entryRule']='attachment-turn' if attach_end is not None else 'short-neck'
                      feature_id=len(detections);features['id']=feature_id;detections.append(features)
                    # The endpoints are proposed from intrinsic attachment and mouth geometry.
                    # Offsets create explicit competing hypotheses, never hidden exceptions.
                      if attach_end is not None:
                        # Replace the near-head cuts: they would amputate the
                        # attachment even if the ranker preferred their shape.
                        right_options=[(offset,normal_probe(entering,attach_end+offset*stroke,stroke,config['entry'],origin,scale,config['cut']['halfLength'])) for offset in attach_settings['offsets']]
                      else:
                        right_options=[(offset,normal_probe(entering,offset*stroke,stroke,config['entry'],origin,scale,config['cut']['halfLength'])) for offset in config['entry']['offsets']]
                        right_options.extend(('corner',p) for p in corner_probes(entering,entering[0],stroke,config['entry'],corners,config['corners'],origin,scale,config['cut']['halfLength']))
                      left_options=[(offset,normal_probe(returning,outlet[1]+offset*stroke,stroke,config['return'],origin,scale,config['cut']['halfLength'])) for offset in config['return']['outletOffsets']]
                      left_options.extend(('corner',p) for p in corner_probes(returning,outlet[0],stroke,config['return'],corners,config['corners'],origin,scale,config['cut']['halfLength']))
                      for (right_offset,right),(left_offset,left) in itertools.product(right_options,left_options):
                        if right is None or left is None:continue
                        required=[world(at_distance(lip,f*lengths[-1])) for f in (.25,.55,.85)]+[p['lower'] for p in samples]
                        if head_cap is not None:required.append(world(head_cap[len(head_cap)//2]))
                        if attach_end is not None:
                          required.extend(world(at_distance(entering,f*attach_end)) for f in (.25,.5,.75,.9))
                        probes.append({'featureId':feature_id,'rightProbe':right,'leftProbe':left,
                                       'parameters':{'entryOffset':right_offset,'outletOffset':left_offset,
                                                     'entryReference':features['entryRule']},
                                       'requiredInk':required,'features':features})
    return {'structures':detections,'probes':probes,'rejections':dict(rejected),
            'graph':{'nodes':[{'xy':world(n['xy']),'degree':len(n['edges'])} for n in nodes],
                     'edges':[{'nodes':list(e['nodes']),'points':[world(p) for p in e['points'][::max(1,len(e['points'])//60)]]} for e in edges]},
            'sampling':{'scale':scale,'origin':origin.tolist(),'width':mask.shape[1],'height':mask.shape[0]}}
