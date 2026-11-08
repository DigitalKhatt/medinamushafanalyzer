"""Generate final role/type labels from a saved model and human overrides.

Automatic training targets are not final-label overrides. Native source masks
remain fixed; structural tests flag predictions unless filtering is requested.
"""
from collections import Counter
import json
from pathlib import Path
import sqlite3


def finalize(db,structural_filter=False):
    """Do not read teacher targets here: only model output and reviewed labels."""
    columns={r[1] for r in db.execute('PRAGMA table_info(items)')}
    if 'final_source' not in columns:db.execute('ALTER TABLE items ADD COLUMN final_source TEXT')
    # Drafts do not override predictions. Reviewed source labels do, regardless
    # of whether their class disagrees with the model or a structural check.
    db.execute('''UPDATE items SET final_kind=COALESCE(prediction_kind,'unknown'),
        final_type=COALESCE(prediction_type,'unknown'),final_source='model',refined=0,
        resolution_method=NULL,resolution_score=NULL,unresolved_reason=NULL,source_diagnostics=NULL,
        assignment_status=CASE WHEN subword_id IS NOT NULL OR word_id IS NOT NULL THEN 'linked' ELSE 'unassigned' END''')
    db.execute('''UPDATE items SET final_kind=kind,final_type=COALESCE(mark_type,'unknown'),final_source='human-reviewed'
        WHERE human_reviewed=1 AND origin='human-reviewed' ''')
    db.execute("UPDATE items SET final_source='unresolved' WHERE final_kind='unknown' AND final_source='model'")
    db.execute("UPDATE pages SET unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND final_kind='unknown'),0)")
    db.execute("UPDATE pages SET scope_unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND in_scope=1 AND final_kind='unknown'),0) WHERE scope_ink_pixels IS NOT NULL")
    report={'policy':'Model predictions plus explicit human-reviewed overrides; automatic training labels never override predictions.',
        'structuralPolicy':'filter' if structural_filter else 'diagnostic-only',
        'sources':dict(db.execute('SELECT final_source,COUNT(*) FROM items WHERE in_scope=1 GROUP BY final_source')),
        'classes':dict(db.execute('SELECT final_kind,COUNT(*) FROM items WHERE in_scope=1 GROUP BY final_kind')),
        'structuralFlags':db.execute('SELECT COUNT(*) FROM items WHERE in_scope=1 AND prediction_guard IS NOT NULL').fetchone()[0],
        'geometryWarning':'The model classifies prepared vector regions. Region grouping and transcript links are supplied by preparation, not a learned character-segmentation decoder.'}
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('inference',json.dumps(report)))
    db.execute("DELETE FROM meta WHERE key='resolution'")
    return report


def infer_corpus(root,progress,structural_filter=False):
    import numpy as np
    from .common import FEATURE_DIM
    from .labels import digest_file
    from .learn import ROLES,Network,infer,predictions,decoder_policy
    from .structure import migrate,guard_triples
    root=Path(root);metadata=json.loads((root/'models/model.json').read_text())
    if 'typeRoles' not in metadata:raise ValueError('This older model lacks an independent type vocabulary. Train using the frozen label dataset first.')
    weights_path=root/'models/selected.npz'
    if digest_file(weights_path)!=metadata['modelSha256']:raise ValueError('Model weights and vocabulary metadata do not match')
    with np.load(weights_path) as weights:
        net=Network(FEATURE_DIM,weights['w1'].shape[1],len(metadata['types']));net.p={k:weights[k] for k in net.p}
        mean,std=weights['mean'],weights['std']
    db=sqlite3.connect(root/'corpus.sqlite');db.row_factory=sqlite3.Row;migrate(db)
    rows=list(db.execute('SELECT id,feature_row FROM items ORDER BY feature_row'))
    if [r['feature_row'] for r in rows]!=list(range(len(rows))):raise ValueError('Invalid feature order')
    features=np.memmap(root/'features.u8',mode='r',dtype='u1',shape=(len(rows),FEATURE_DIM))
    rp,tp=infer(net,features,mean,std,metadata['typeRoles'],progress,metadata['selectedRound'],root)
    roles,types,scores,margins=predictions(rp,tp,metadata['typeRoles'])
    from .hamza import apply as apply_hamza
    hamzas, hamza_report = apply_hamza(root, metadata, features, rp, tp, roles, types, scores, margins)
    from .damma_yeh import apply as apply_damma_yeh
    pair_evidence,pair_report=apply_damma_yeh(root,metadata,features,rp,tp,roles,types,scores,margins,excluded=set(hamzas))
    db.executemany('''UPDATE items SET prediction_kind=?,prediction_type=?,model_confidence=?,model_margin=?,model_round=?,role_scores=?,type_scores=? WHERE id=?''',
        ((ROLES[int(roles[i])],hamzas[i]['type'] if i in hamzas else metadata['types'][int(types[i])] if types[i]>=0 else 'unknown',float(scores[i]),float(margins[i]),metadata['selectedRound'],
          rp[i].astype('<f4',copy=False).tobytes(),tp[i].astype('<f4',copy=False).tobytes(),r['id']) for i,r in enumerate(rows)))
    db.execute('''UPDATE items SET raw_prediction_kind=prediction_kind,raw_prediction_type=prediction_type,
        raw_model_confidence=model_confidence,raw_model_margin=model_margin,prediction_guard=NULL,prediction_structure=NULL''')
    db.executemany('UPDATE items SET prediction_structure=? WHERE id=?',
                   ((json.dumps({'hamza': value}), rows[i]['id']) for i, value in hamzas.items()))
    db.executemany('UPDATE items SET prediction_structure=? WHERE id=?',
                   ((json.dumps({'shapePair':value}),rows[i]['id']) for i,value in pair_evidence.items()))
    checks=guard_triples(db);checks['flaggedTripleDots']=checks['withheldTripleDots']
    if not structural_filter:checks['withheldTripleDots']=0
    if not structural_filter:
        # Preserve the diagnostic and raw answer; do not turn a rule violation
        # into a different model prediction or an abstention by default.
        db.execute('''UPDATE items SET prediction_kind=raw_prediction_kind,prediction_type=raw_prediction_type,
            model_confidence=raw_model_confidence,model_margin=raw_model_margin WHERE prediction_guard IS NOT NULL''')
    db.execute('''UPDATE items SET model_agrees=CASE WHEN teacher_kind='unknown' THEN NULL ELSE CAST(prediction_kind=teacher_kind
        AND (teacher_kind NOT IN ('dot','mark','symbol') OR teacher_type='unknown' OR teacher_type=prediction_type OR (teacher_type IN ('hamza-above','hamza-below') AND prediction_type='mark-'||teacher_type)) AS INTEGER) END''')
    report=finalize(db,structural_filter);report['modelSha256']=metadata['modelSha256'];report['predictionChecks']=checks
    if hamza_report is not None:report['hamzaClassifier']=hamza_report
    if pair_report is not None:report['shapePairClassifier']=pair_report
    report['decoder']=decoder_policy()
    report['networkOutputs']={'roles':ROLES,'types':metadata['types'],'typeRoles':metadata['typeRoles'],'storage':'float32-le'}
    audit_path=root/'models/decoder-evaluation.json'
    if audit_path.exists():
        audit=json.loads(audit_path.read_text())
        if audit['modelSha256']==metadata['modelSha256'] and audit['decoder']['version']==report['decoder']['version']:
            report['decoderEvaluation']={'createdAt':audit['createdAt'],'referenceWarning':audit['referenceWarning'],
                'splits':{split:{'examples':v['eligibleQualityAtLeast02']['after']['examples'],
                    'beforeJointAccuracy':v['eligibleQualityAtLeast02']['before']['jointLabel']['accuracy'],
                    'afterJointAccuracy':v['eligibleQualityAtLeast02']['after']['jointLabel']['accuracy'],
                    'corrected':v['corrected'],'regressed':v['regressed'],'human':v['human']} for split,v in audit['splits'].items()}}
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('inference',json.dumps(report)))
    # A preparation rebuild creates a new database while keeping the selected
    # model. Publish its verified vocabulary/metadata too, so browsing and
    # similarity keep working when build and infer run without retraining.
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('learning',json.dumps(metadata)))
    db.commit();db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    temp=root/'inference.building.json';temp.write_text(json.dumps(report,ensure_ascii=False,indent=2));temp.replace(root/'inference.json')
    progress('inference-complete',len(rows),len(rows),{'sources':report['sources'],'structuralFlags':report['structuralFlags']})
    return report
