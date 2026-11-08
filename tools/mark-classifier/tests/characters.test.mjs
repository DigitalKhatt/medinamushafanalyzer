import test from 'node:test';
import assert from 'node:assert/strict';
import {characterMetadata, isSingleCharacter, completeCharacter, partApprovalIssue,
  formPresets, applyCharacterMetadata, upgradeCharacters} from '../app/characters.mjs';

test('joining position and form are independent of the character and ink role', () => {
  for (const position of ['initial','medial','final','isolated'])
    assert.equal(completeCharacter({label:'ن',position,form:'expanded'}),true);
  assert.equal(completeCharacter({label:'ن',position:'unknown'}),false);
  assert.equal(completeCharacter({label:'',position:'final'}),false);
  assert.equal(isSingleCharacter('لا'),false);
  assert.equal(isSingleCharacter('ﻻ'),false); // A presentation ligature expands to two letters.
  assert.equal(isSingleCharacter('ﻥ'),true); // A presentation glyph normalizes to noon.
});

test('form examples are scoped to the selected character and remain extensible', () => {
  assert.ok(formPresets('ك').some(f=>f.id==='zigzag'));
  assert.ok(!formPresets('ك').some(f=>f.id==='expanded'));
  assert.ok(formPresets('ن').some(f=>f.id==='expanded'));
  assert.ok(!formPresets('ن').some(f=>f.id==='zigzag'));
  assert.equal(characterMetadata({label:'ن',form:'wide-bowl-v2'}).form,'wide-bowl-v2');
});

test('body approval requires a character; dot and mark roles can be reviewed independently', () => {
  assert.ok(partApprovalIssue({role:'body',owner:null},{}));
  assert.ok(partApprovalIssue({role:'kashida',owner:'a'},{a:{label:'ك',position:'unknown'}}));
  assert.equal(partApprovalIssue({role:'dot',owner:'a'},{a:{label:'ن',position:'final'}}),null);
  assert.equal(partApprovalIssue({role:'dot',owner:null},{}),null);
  assert.equal(partApprovalIssue({role:'mark',owner:null},{}),null);
  assert.equal(partApprovalIssue({role:'other',owner:null},{}),null);
  assert.equal(partApprovalIssue({role:'ornament',owner:null},{}),null);
  assert.equal(partApprovalIssue({role:'symbol',owner:null},{}),null);
});

test('changing character attributes reopens all its cuts, not other characters', () => {
  const doc={groups:{a:{label:'ن',position:'final',form:'standard',reviewed:true},b:{label:'ك'}},
    parts:[{owner:'a',role:'body',reviewed:true},{owner:'a',role:'dot',reviewed:true},{owner:'b',reviewed:true}]};
  applyCharacterMetadata(doc,'a',{label:'ن',position:'final',form:'expanded'});
  assert.equal(doc.groups.a.reviewed,false);
  assert.deepEqual(doc.parts.map(p=>p.reviewed),[false,false,true]);
});

test('legacy annotation migration preserves ink and reopens incomplete character labels', () => {
  const doc={groups:{a:{label:'ن'}},parts:[
    {role:'body',owner:'a',reviewed:true,runs:[4,2]},
    {role:'mark',owner:null,reviewed:true,runs:[10,1]},
    {role:'symbol',owner:null,reviewed:true,runs:[20,1]},
  ]};
  const runs=JSON.stringify(doc.parts.map(p=>p.runs));
  assert.deepEqual(upgradeCharacters(doc),{changed:true,reopened:1});
  assert.equal(doc.characterMetadataVersion,1);
  assert.equal(JSON.stringify(doc.parts.map(p=>p.runs)),runs);
  assert.equal(doc.parts[2].reviewed,true);
  assert.equal(doc.parts[1].reviewed,true); // Independent mark review survives migration.
  assert.deepEqual(upgradeCharacters(doc),{changed:false,reopened:0});
});

test('independently reviewed marks survive reopening a saved page',()=>{
  const doc={characterMetadataVersion:1,groups:{},parts:[
    {role:'dot',owner:null,reviewed:true,runs:[0,1]},
    {role:'mark',owner:null,reviewed:true,runs:[4,2]},
  ]};
  assert.deepEqual(upgradeCharacters(doc),{changed:false,reopened:0});
  assert.ok(doc.parts.every(p=>p.reviewed));
});
