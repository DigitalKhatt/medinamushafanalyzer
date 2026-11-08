import test from 'node:test';
import assert from 'node:assert/strict';
import {assignSubword,subwordApprovalIssue} from '../app/subwords.mjs';
test('subword body supervision requires text and ink but no character cuts',()=>{
 const part={id:'one',role:'unknown',reviewed:false},doc={parts:[part],groups:{}};
 assignSubword(doc,[part],{id:'s1',text:'قُلْ',body:true});
 assert.equal(subwordApprovalIssue(doc,'s1'),null);assert.equal(part.role,'unknown');assert.equal(part.reviewed,false);
});
test('changing body boundaries reopens their former and new subword contexts',()=>{
 const part={subwordId:'a',subwordBody:true,role:'unknown',reviewed:false};
 const doc={parts:[part],subwords:{a:{text:'ب',reviewed:true},b:{text:'ت',reviewed:true}}};
 assignSubword(doc,[part],{id:'b',text:'ت',body:true});assert.equal(doc.subwords.a.reviewed,false);assert.equal(doc.subwords.b.reviewed,false);
 assert.equal(part.subwordId,'b');assert.equal(part.subwordBody,true);
});
test('a checked mark attachment is independent of body and character ownership',()=>{
 const part={role:'mark',reviewed:true,owner:null},doc={parts:[part]};
 assignSubword(doc,[part],{id:'s1',text:'بِ',body:false});assert.equal(part.subwordAttachmentReviewed,true);assert.equal(part.owner,null);
 assert.match(subwordApprovalIssue(doc,'s1'),/body ink/);
});
test('approved marks cannot silently enter a body mask',()=>{
 const part={role:'dot',reviewed:true},doc={parts:[part]};
 assert.throws(()=>assignSubword(doc,[part],{id:'s1',text:'ب',body:true}),/Reopen/);assert.equal(part.subwordId,undefined);
});
