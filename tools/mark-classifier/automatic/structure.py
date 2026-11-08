"""Visual-only structural decoding. Preserve raw guesses and allow abstention."""
from collections import Counter
import json
from .common import local_mask,unpack_runs
from .propose import teacher_module


def triple_structure(mask,native_contours=None):
    import cv2
    _,_,stats,_=cv2.connectedComponentsWithStats(mask,connectivity=8)
    components=len(stats)-1
    result={'components':components,'accepted':False}
    if native_contours is not None:
        result['nativeContours']=int(native_contours)
        if native_contours!=2:
            result['reason']='The native vector ink does not contain the single contour plus joined-pair contour used by letter triple dots in this edition.'
            return result
    if components not in (2,3):
        result['reason']='Triple dots require an upper dot and a joined or near-touching lower pair; this region has '+str(components)+' connected ink pieces.'
        return result
    _,hierarchy=cv2.findContours(mask,cv2.RETR_CCOMP,cv2.CHAIN_APPROX_SIMPLE)
    holes=sum(int(h[3]>=0) for h in hierarchy[0]) if hierarchy is not None else 0
    result['holes']=holes
    if holes:
        result['reason']='A triple-dot cluster has no enclosed counters.';return result
    if components==2:
        single,pair=sorted(stats[1:],key=lambda r:r[4]);result['lowerPair']='joined'
    else:
        single,*lower=sorted(stats[1:],key=lambda r:r[1]+r[3]/2)
        left,right=sorted(lower,key=lambda r:r[0])
        gap=int(right[0]-left[0]-left[2]);result['lowerPairRasterGap']=gap
        # The native joined pair sometimes loses a very thin connection at the
        # raster threshold. Allow that gap, not a widely spaced waqf triangle.
        if gap>max(1,min(left[2],right[2])*.25) or abs(left[1]+left[3]/2-right[1]-right[3]/2)>max(left[3],right[3])*.6:
            result['reason']='The lower dots are separate rather than a joined or near-touching pair.';return result
        if max(r[4] for r in (single,left,right))/max(1,min(r[4] for r in (single,left,right)))>1.8:
            result['reason']='The three pieces do not have comparable dot sizes.';return result
        x=min(left[0],right[0]);y=min(left[1],right[1])
        pair=[x,y,max(left[0]+left[2],right[0]+right[2])-x,max(left[1]+left[3],right[1]+right[3])-y,left[4]+right[4]]
        result['lowerPair']='near-touching raster variant'
    ratio=float(single[4]/max(1,pair[4]))
    if not .25<=ratio<=.8 or not teacher_module().triple_layout(single[:4],pair[:4]):
        result['reason']='The two pieces do not form an aligned single dot above a joined double-dot pair.';return result
    result['accepted']=True;return result


def migrate(db):
    columns={r[1] for r in db.execute('PRAGMA table_info(items)')}
    for name,type in {'raw_prediction_kind':'TEXT','raw_prediction_type':'TEXT','raw_model_confidence':'REAL','raw_model_margin':'REAL',
                      'prediction_guard':'TEXT','prediction_structure':'TEXT','role_scores':'BLOB','type_scores':'BLOB'}.items():
        if name not in columns:db.execute(f'ALTER TABLE items ADD COLUMN {name} {type}')
    if {'in_scope','model_confidence','final_kind','id'}<=columns:
        db.execute("CREATE INDEX IF NOT EXISTS item_attention_score ON items(in_scope,model_confidence,id) WHERE model_confidence<0.90 OR prediction_guard IS NOT NULL OR final_kind='unknown'")
    db.commit()


def guard_triples(db):
    rows=db.execute("SELECT i.id,i.runs,i.x,i.y,i.w,i.h,i.contours,p.width FROM items i JOIN pages p ON p.page_id=i.page_id WHERE i.in_scope=1 AND i.prediction_kind='dot' AND i.prediction_type='triple-dot'").fetchall()
    rejected=Counter()
    for row in rows:
        mask=local_mask(unpack_runs(row['runs']),[row[k] for k in ('x','y','w','h')],row['width'])
        evidence=triple_structure(mask,row['contours'])
        db.execute('UPDATE items SET prediction_structure=? WHERE id=?',(json.dumps(evidence),row['id']))
        if not evidence['accepted']:
            reason=evidence['reason'];rejected[reason]+=1
            db.execute("UPDATE items SET prediction_kind='unknown',prediction_type='unknown',model_confidence=0,model_margin=0,prediction_guard=?,model_agrees=CASE WHEN teacher_kind='unknown' THEN NULL ELSE 0 END WHERE id=?",(reason,row['id']))
    return {'checkedTripleDots':len(rows),'acceptedTripleDots':len(rows)-sum(rejected.values()),'withheldTripleDots':sum(rejected.values()),
        'reasons':dict(rejected),'policy':'Native contour topology plus mask components, counters and single-above-pair layout, including tiny raster gaps in the lower pair; no transcript or teacher-label decoding. Raw network guesses are retained.'}


def recheck(output,progress):
    """Reapply visual decoding to saved network output without retraining weights."""
    from pathlib import Path
    import sqlite3
    output=Path(output);db=sqlite3.connect(output/'corpus.sqlite');db.row_factory=sqlite3.Row;migrate(db)
    row=db.execute("SELECT value FROM meta WHERE key='learning'").fetchone()
    if not row:raise ValueError('Train a model before checking predictions')
    metadata=json.loads(row[0])
    if not db.execute('SELECT 1 FROM items WHERE raw_prediction_kind IS NOT NULL LIMIT 1').fetchone():raise ValueError('This model has no saved raw predictions; retrain before rechecking')
    missing=db.execute('SELECT id,feature_row,raw_prediction_kind,raw_prediction_type FROM items WHERE prediction_guard IS NOT NULL AND raw_model_margin IS NULL').fetchall()
    if missing:
        # Older guard versions did not save the original margin. Recover it
        # from the unchanged selected network, never from a teacher label.
        import numpy as np
        from .learn import Network,predictions,ROLES
        from .common import FEATURE_DIM
        with np.load(output/'models'/'selected.npz') as weights:
            net=Network(FEATURE_DIM,weights['w1'].shape[1],len(metadata['types']));net.p={k:weights[k] for k in net.p}
            mean,std=weights['mean'],weights['std']
        build=json.loads(db.execute("SELECT value FROM meta WHERE key='build'").fetchone()[0])
        features=np.memmap(output/'features.u8',dtype='u1',mode='r',shape=(build['items'],FEATURE_DIM))
        counts={}
        for name,role,count in db.execute('SELECT teacher_type,teacher_kind,COUNT(*) FROM items GROUP BY teacher_type,teacher_kind'):counts.setdefault(name,[]).append((count,role))
        type_roles=[ROLES.index(max(counts[name])[1]) for name in metadata['types']]
        rp,tp,_=net.forward((np.asarray(features[[r['feature_row'] for r in missing]],dtype='f4')/255-mean)/std)
        roles,types,_,margins=predictions(rp,tp,type_roles)
        for i,r in enumerate(missing):
            name=metadata['types'][int(types[i])] if types[i]>=0 else 'unknown'
            if ROLES[int(roles[i])]!=r['raw_prediction_kind'] or name!=r['raw_prediction_type']:raise ValueError('Saved weights differ from the recorded raw predictions; retrain instead')
            db.execute('UPDATE items SET raw_model_margin=? WHERE id=?',(float(margins[i]),r['id']))
    db.execute("UPDATE items SET raw_model_margin=COALESCE(raw_model_margin,model_margin) WHERE raw_prediction_kind='dot' AND raw_prediction_type='triple-dot' AND raw_model_margin IS NULL")
    db.execute("""UPDATE items SET prediction_kind=raw_prediction_kind,prediction_type=raw_prediction_type,model_confidence=raw_model_confidence,
      model_margin=raw_model_margin,prediction_guard=NULL,prediction_structure=NULL,
      model_agrees=CASE WHEN teacher_kind='unknown' THEN NULL ELSE CAST(raw_prediction_kind=teacher_kind AND
      (teacher_kind NOT IN ('dot','mark','symbol') OR teacher_type='unknown' OR teacher_type=raw_prediction_type) AS INTEGER) END
      WHERE prediction_guard IS NOT NULL OR (raw_prediction_kind='dot' AND raw_prediction_type='triple-dot')""")
    checks=guard_triples(db)
    inference=db.execute("SELECT value FROM meta WHERE key='inference'").fetchone()
    if inference:
        prior_inference=json.loads(inference[0]);policy=prior_inference['structuralPolicy']
        if policy=='diagnostic-only':
            db.execute('''UPDATE items SET prediction_kind=raw_prediction_kind,prediction_type=raw_prediction_type,
                model_confidence=raw_model_confidence,model_margin=raw_model_margin WHERE prediction_guard IS NOT NULL''')
        checks['flaggedTripleDots']=checks['withheldTripleDots']
        if policy=='diagnostic-only':checks['withheldTripleDots']=0
        from .inference import finalize
        summary=finalize(db,policy=='filter');summary['predictionChecks']=checks
        for key in ('modelSha256','decoder','networkOutputs','decoderEvaluation','hamzaClassifier','shapePairClassifier'):
            if key in prior_inference:summary[key]=prior_inference[key]
        db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('inference',json.dumps(summary)))
        temp=output/'inference.building.json';temp.write_text(json.dumps(summary,ensure_ascii=False,indent=2));temp.replace(output/'inference.json')
    metadata['predictionChecks']=checks
    db.execute("UPDATE meta SET value=? WHERE key='learning'",(json.dumps(metadata),));db.commit();db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    (output/'models'/'model.json').write_text(json.dumps(metadata,indent=2))
    progress('check-predictions',1,1,checks)
    return metadata
