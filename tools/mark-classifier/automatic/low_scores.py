"""Review queue diagnostics; never change predictions or approve labels."""
from collections import Counter
from contextlib import closing
import csv
import json
from pathlib import Path
import sqlite3

from .query import network_outputs,get_meta


def reason(row,outputs):
    if row['teacher_kind']=='unknown':return 'missing-preparation-label'
    if row['teacher_kind']=='subword-ink' and row['prediction_kind'] in ('mark','dot','symbol'):
        return 'body-versus-mark'
    context_pairs=({'fatha','kasra'},{'tanwin-fath','tanwin-kasr'},{'hamza-above','hamza-below'})
    if any({row['teacher_type'],row['prediction_type']}==pair for pair in context_pairs):
        return 'placement-or-context'
    if outputs and outputs['roles'][0]['label']!=row['prediction_kind']:return 'role-head-disagreement'
    if row['model_margin'] is not None and row['model_margin']<.15:return 'competing-labels'
    return 'weak-model-evidence'


def audit(root,baseline=None):
    root=Path(root);directory=root/'low-score-audit';directory.mkdir(exist_ok=True)
    fields=['id','page','split','predicted_role','predicted_type','score','previous_score',
        'original_role','original_type','prepared_correction','reason','review_url']
    cases=[];types=Counter();reasons=Counter();splits=Counter()
    labels=root/'training-labels/corrections.json'
    fixes={v['itemId']:v for v in json.loads(labels.read_text())} if labels.exists() else {}
    with closing(sqlite3.connect(root/'corpus.sqlite')) as db:
        db.row_factory=sqlite3.Row;inference=get_meta(db,'inference')
        if baseline:db.execute('ATTACH DATABASE ? AS previous',(str(Path(baseline)/'corpus.sqlite'),))
        extra='o.model_confidence previous_score' if baseline else 'NULL previous_score'
        join='LEFT JOIN previous.items o USING(id)' if baseline else ''
        sql=f'''SELECT i.*,p.split,{extra} FROM items i JOIN pages p USING(page_id) {join}
            WHERE i.in_scope=1 AND i.model_confidence<.90 ORDER BY i.model_confidence,i.id'''
        for row in db.execute(sql):
            why=reason(row,network_outputs(dict(row),inference));fix=fixes.get(row['id'])
            cases.append(dict(zip(fields,[row['id'],row['page_id'],row['split'],row['prediction_kind'],
                row['prediction_type'],row['model_confidence'],row['previous_score'],row['teacher_kind'],
                row['teacher_type'],fix['type'] if fix else '',why,
                f'/review.html?proposalSource=automatic&candidate={row["id"].split(":",1)[1]}#{row["page_id"]}'])))
            types[row['prediction_type'] if row['prediction_type']!='unknown' else row['prediction_kind']]+=1
            reasons[why]+=1;splits[row['split']]+=1
        summary={'threshold':.9,'cases':len(cases),'byType':dict(types.most_common()),
            'byReason':dict(reasons.most_common()),'bySplit':dict(splits),
            'preparedCorrections':sum(v.get('action','relabel')=='relabel' for v in fixes.values()),
            'preparedEvidenceItems':len(fixes),'preparedEvidenceByAction':dict(Counter(v.get('action','relabel') for v in fixes.values())),
            'policy':'All low-score cases inspected programmatically. Reasons are review hints, not verified labels. No score rescaling or human approvals.',
            'modelSha256':inference.get('modelSha256')}
        if baseline:
            summary['before']=db.execute('SELECT COUNT(*) FROM previous.items WHERE in_scope=1 AND model_confidence<.90').fetchone()[0]
            summary['previousCasesNowAboveThreshold']=db.execute('''SELECT COUNT(*) FROM items n JOIN previous.items o USING(id)
                WHERE n.in_scope=1 AND o.model_confidence<.9 AND n.model_confidence>=.9''').fetchone()[0]
    (directory/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
    (directory/'cases.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2))
    with (directory/'cases.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(cases)
    return summary
