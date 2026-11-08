import test from 'node:test';
import assert from 'node:assert/strict';
import {pageRange,gridWindow,chunkWindow} from '../app/corpus-grid.mjs';

test('page sizes cover 1000, 10000 and all without overlapping page ranges',()=>{
  assert.deepEqual(pageRange(25001,0,'1000'),{count:1000,start:1,end:1000,previous:false,next:true});
  assert.deepEqual(pageRange(25001,10000,'10000'),{count:10000,start:10001,end:20000,previous:true,next:true});
  assert.deepEqual(pageRange(25001,20000,'10000'),{count:5001,start:20001,end:25001,previous:true,next:false});
  assert.deepEqual(pageRange(640397,0,'all'),{count:640397,start:1,end:640397,previous:false,next:false});
  assert.equal(pageRange(0,0,'all').count,0);
});
test('All keeps the scroll surface and mounted cards bounded while reaching the last item',()=>{
  const options={count:640397,width:680,viewportHeight:600};
  const top=gridWindow(options),end=gridWindow({...options,scrollOffset:8_000_000});
  assert.equal(top.start,0);assert.equal(top.height,8_000_000);
  assert.ok(top.end-top.start<60);assert.ok(end.end-end.start<60);
  assert.equal(end.end,640397);assert.ok(end.top>=0&&end.top<end.height);
});
test('ordinary pages retain natural scroll positions and respond to narrow widths',()=>{
  const options={count:1000,width:680,viewportHeight:600,scrollOffset:2440};
  const result=gridWindow(options);
  assert.equal(result.columns,4);assert.equal(result.start,32);assert.equal(result.top,1952);
  assert.equal(gridWindow({...options,width:200}).columns,1);
  assert.equal(gridWindow({...options,count:0}).end,0);
});
test('prefetch stays bounded and prioritizes visible batches across a boundary',()=>{
  assert.deepEqual(chunkWindow({start:0,end:30,count:1000}),{visible:[0],prefetch:[]});
  assert.deepEqual(chunkWindow({start:0,end:30,count:640397}),{visible:[0],prefetch:[1]});
  assert.deepEqual(chunkWindow({start:990,end:1020,count:640397}),{visible:[0,1],prefetch:[2]});
  assert.deepEqual(chunkWindow({start:3100,end:3140,count:640397}),{visible:[3],prefetch:[4,2]});
  assert.deepEqual(chunkWindow({start:640370,end:640397,count:640397}),{visible:[640],prefetch:[639]});
  assert.deepEqual(chunkWindow({start:0,end:0,count:0}),{visible:[],prefetch:[]});
});
