#!/usr/bin/env python3
"""Group freshly regenerated, incomplete native subwords for template review."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import unicodedata

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RESOLVED={'matched','not-required'}
DIRECT={'no-template','no-match'}
STATUSES=RESOLVED|DIRECT|{'blocked'}

def write(path,value):path.write_text(json.dumps(value,ensure_ascii=False,separators=(',',':'))+'\n')
def body_letters(text):return ''.join(c for c in text if c!='\u0640' and not unicodedata.category(c).startswith('M'))
def has_letter(text):return any(unicodedata.category(c).startswith('L') for c in text)
def identifier(page,row):return f"page{page:03}:l{row['line']}-w{row['word']}-s{row['subword']}"

def group_key(letter):
    # Identical position/selectors describe a shape family, including dotted
    # variants. No-template cases stay character-specific until a form is known.
    candidates=tuple(sorted(set(letter.get('candidates',[]))))
    return (letter['status'],letter['position'],candidates or (letter['character'],))

def group_id(key):return hashlib.sha256(json.dumps(key,ensure_ascii=False).encode()).hexdigest()[:12]

def analyse_page(cache, partition_failures=frozenset()):
    config=cache.get('templateConfig',{})
    if config.get('version')!=2 or any(k not in config for k in ['initial','medial','final']):raise ValueError('Missing current template snapshot')
    if config['kashidaDefaults'].get('trackingEnabled'):raise ValueError('Tracker enabled')
    page=cache['page'];counts=Counter();statuses=Counter();items=[];occurrences=[];known_words={};partial_words=set();word_rows={}
    inventory={(w['line'],w['word']):w for w in cache.get('sourceWords',[])}
    lines={line['line']:line for line in cache.get('sourceLines',[])}
    for row in cache['subwords']:
        word_id=(row['line'],row['word']);word=inventory.get(word_id,{})
        text=row['text'];base=body_letters(text)
        if lines.get(row['line'],{}).get('transcriptWords')==0:
            counts['excludedHeadingOrBasmalaSubwords']+=1;continue
        if not has_letter(base):counts['excludedNonLetterSubwords']+=1;continue
        counts['subwords']+=1;connected=len(base)>1
        counts['connectedSubwords' if connected else 'singleLetterSubwords']+=1
        known_words[word_id]=word.get('text',row.get('wordText',text));word_rows[word_id]=word
        letters=[dict(letter,status=letter.get('templateStatus',letter['status'])) for letter in row.get('letters',[])]
        for letter in letters:
            if letter['status'] not in STATUSES:raise ValueError(f"Unknown status {letter['status']}")
            statuses[letter['status']]+=1
        missing=[l for l in letters if l['status'] not in RESOLVED]
        direct=[l for l in missing if l['status'] in DIRECT]
        blocked=[l for l in missing if l['status']=='blocked']
        reason='unresolved-forms'
        consistent=len(letters)==len(base)
        failed_partition=identifier(page,row) in partition_failures
        complete=bool(row['complete']) and not missing and consistent and len(row['bodies'])==len(base) and not failed_partition
        if complete:
            counts['completeSubwords']+=1
            counts['completeConnectedSubwords']+=connected
            continue
        counts['incompleteSubwords']+=1;counts['incompleteConnectedSubwords']+=connected
        partial_words.add(word_id)
        if not consistent:
            reason='missing-diagnostics';primary={'status':'input-issue','position':'unknown','character':'?', 'letterIndex':0,'candidates':[]}
            direct=[];blocked=[]
        elif direct:primary=direct[0]
        elif blocked:primary=blocked[0]
        else:
            reason='ink-partition' if failed_partition else 'cut-conflict'
            primary={'status':'cut-conflict','position':'assembly','character':'?', 'letterIndex':0,'candidates':[]}
        if row['complete'] and not complete:counts['reportedCompleteButInconsistent']+=1
        relevant=missing if missing and consistent else [primary]
        keys={group_id(group_key(l)) for l in relevant};primary_id=group_id(group_key(primary))
        item={'id':identifier(page,row),'page':page,'line':row['line'],'word':row['word'],'subword':row['subword'],
              'text':text,'baseText':base,'wordText':known_words[word_id],'primaryGroup':primary_id,'groups':sorted(keys),
              'direct':direct,'blocked':blocked,'reason':reason,
              'bodyCount':len(row['bodies']),'expectedBodyCount':len(base),'bounds':row.get('bounds'),
              'reportedComplete':bool(row['complete'])}
        items.append(item)
        for letter in relevant:occurrences.append((letter,item))
    # An Arabic word with no subword decomposition must not disappear from the
    # denominator merely because no letter matcher was invoked.
    for word_id,word in inventory.items():
        if word_id in known_words or not has_letter(word.get('text','')) or not word.get('inTranscript',True):continue
        if lines.get(word_id[0],{}).get('transcriptWords')==0:continue
        known_words[word_id]=word['text'];partial_words.add(word_id)
        counts['wordsWithoutLetterSubwords' if word.get('subwords',0) else 'wordsWithoutSubwords']+=1
        primary={'status':'input-issue','position':'unknown','character':'?', 'letterIndex':0,'candidates':[]}
        gid=group_id(group_key(primary));item={'id':f'page{page:03}:l{word_id[0]}-w{word_id[1]}-s0','page':page,'line':word_id[0],
             'word':word_id[1],'subword':0,'text':word['text'],'baseText':body_letters(word['text']),'wordText':word['text'],
             'primaryGroup':gid,'groups':[gid],'direct':[],'blocked':[],'reason':'word-without-letter-subwords' if word.get('subwords',0) else 'word-without-subwords',
             'bodyCount':0,'expectedBodyCount':0,'bounds':None,'reportedComplete':False}
        items.append(item);occurrences.append((primary,item))
    counts['words']=len(known_words);counts['incompleteWords']=len(partial_words);counts['completeWords']=len(known_words)-len(partial_words)
    return {'page':page,'summary':dict(counts),'statuses':dict(statuses),'items':items,'occurrences':occurrences,
            'lineMismatches':[l for l in lines.values() if l['transcriptWords'] and l['sourceWords']!=l['transcriptWords']]}

def build(folder,allow_partial=False):
    folder=folder.resolve();generation=json.loads((folder/'generation.json').read_text())
    if not allow_partial and (generation['pagesCompleted']!=604 or generation['failures']):raise ValueError('Regeneration is incomplete')
    template=json.loads((folder/'letter-forms.json').read_text())
    partition_failures=set()
    partition_file=folder/'ink-partition.json'
    if partition_file.exists():
        partition=json.loads(partition_file.read_text())
        if partition.get('generationSha256')!=hashlib.sha256((folder/'generation.json').read_bytes()).hexdigest():
            raise ValueError('Ink partition check belongs to a different snapshot')
        partition_failures={item['id'] for item in partition['failures']}
    groups={};items=[];pages=[];totals=Counter();statuses=Counter();template_hash=None
    for page in generation['completedPages']:
        cache=json.loads((folder/'pages'/f'page{page:03}.json').read_text())
        if cache['templateConfig']!=template:raise ValueError('Mixed template configurations')
        if template_hash is not None and cache['letterFormsSha256']!=template_hash:raise ValueError('Mixed template hashes')
        template_hash=cache['letterFormsSha256'];result=analyse_page(cache,partition_failures)
        totals.update(result['summary']);statuses.update(result['statuses']);items.extend(result['items'])
        pages.append({k:result[k] for k in ['page','summary','statuses','lineMismatches']})
        for letter,item in result['occurrences']:
            key=group_key(letter);gid=group_id(key)
            if gid not in groups:groups[gid]={'id':gid,'status':key[0],'position':key[1],'candidates':letter.get('candidates',[]),
                'characters':set(),'occurrences':0,'items':{},'firstUnresolvedSubwords':0,'soleDirectIssueSubwords':set(),'unblockedSoleIssueSubwords':set()}
            group=groups[gid];group['characters'].add(letter['character']);group['occurrences']+=1;group['items'][item['id']]=item
        for item in result['items']:
            groups[item['primaryGroup']]['firstUnresolvedSubwords']+=1
            direct_groups={group_id(group_key(l)) for l in item['direct']}
            if len(direct_groups)==1:
                group=groups[next(iter(direct_groups))];group['soleDirectIssueSubwords'].add(item['id'])
                if not item['blocked']:group['unblockedSoleIssueSubwords'].add(item['id'])
    final_groups=[]
    for group in groups.values():
        affected=list(group.pop('items').values());group['characters']=''.join(sorted(group['characters']))
        group['affectedSubwords']=len(affected);group['affectedWords']=len({(x['page'],x['line'],x['word']) for x in affected})
        group['pageCount']=len({x['page'] for x in affected});group['textVariants']=len({x['baseText'] for x in affected})
        for name in ['soleDirectIssueSubwords','unblockedSoleIssueSubwords']:group[name]=len(group[name])
        # Representative locations differ in text and aspect ratio; these are
        # inspection samples, not claims that one template fits every example.
        examples=[];seen=set()
        for item in affected:
            box=item.get('bounds');aspect=round(box[2]/max(box[3],.01)) if box else 0
            signature=(item['baseText'],aspect)
            if signature in seen:continue
            seen.add(signature);examples.append(item['id'])
            if len(examples)==6:break
        group['examples']=examples;final_groups.append(group)
    final_groups.sort(key=lambda g:(g['status']=='blocked',-g['firstUnresolvedSubwords'],-g['affectedSubwords'],g['id']))
    items.sort(key=lambda x:(x['page'],x['line'],x['word'],x['subword']))
    words={}
    for item in items:
        wid=item['id'].rsplit('-s',1)[0]
        words.setdefault(wid,{'id':wid,'page':item['page'],'line':item['line'],'word':item['word'],'text':item['wordText'],'incompleteSubwords':[]})['incompleteSubwords'].append(item['id'])
    if len(words)!=totals['incompleteWords']:raise ValueError('Incomplete word inventory mismatch')
    manifest={'schemaVersion':1,'pageCount':len(pages),'expectedPages':604,'templateHash':template_hash,'templateFileSha256':generation['templateFileSha256'],
        'trackerEnabled':False,'summary':dict(totals),'letterStatuses':dict(statuses),'groups':final_groups,'pages':pages,
        'provenance':'machine-template-audit','humanReviewed':False,'scope':'Native Quran-text subwords; heading/basmala lines and non-letter symbols excluded. All input/diagnostic issues remain visible.',
        'interpretation':'Complete means all required templates were accepted, not that a human verified every cut. Group counts are possible review leverage, not promised fixes. Blocked letters were not independently tested.'}
    write(folder/'index.json',manifest);write(folder/'items.json',items);write(folder/'incomplete-words.json',list(words.values()))
    for name in ['incomplete_corpus.html','incomplete_corpus.mjs','incomplete_corpus.css']:
        shutil.copyfile(HERE/name,folder/('index.html' if name.endswith('.html') else name))
    shutil.copyfile(ROOT/'tools/mark-classifier/mark_assignment_report.woff',folder/'report-quran.woff')
    print(json.dumps({'summary':dict(totals),'letterStatuses':dict(statuses),'groups':len(final_groups),'topGroups':final_groups[:8]},ensure_ascii=False,indent=2))
    return manifest

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--folder',type=Path,required=True);parser.add_argument('--allow-partial',action='store_true')
    args=parser.parse_args();build(args.folder,args.allow_partial)
