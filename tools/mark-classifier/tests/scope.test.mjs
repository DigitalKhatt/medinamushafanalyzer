import test from 'node:test';
import assert from 'node:assert/strict';
import {scopeMask,scopedArea,clipLabels} from '../app/scope.mjs';
test('frame scope hides exterior hits without changing source masks or annotations',()=>{
  const scope={width:8,height:4,runs:[8,4,16,4]},mask=scopeMask(scope,{width:8,height:4});
  const runs=[0,1,8,4,16,4,31,1];assert.equal(scopedArea(runs,mask),8);assert.deepEqual(runs,[0,1,8,4,16,4,31,1]);
  const labels=new Uint32Array(32).fill(3);clipLabels(labels,mask);
  assert.equal(labels[0],0);assert.equal(labels[8],3);assert.equal(labels[31],0);
  assert.equal(scopedArea([31,1],null),1);assert.equal(scopeMask(null,{}),null);
});
test('a scope from another raster is rejected',()=>{
  assert.throws(()=>scopeMask({width:8,height:4,runs:[]},{width:16,height:8}),/does not match/);
});
