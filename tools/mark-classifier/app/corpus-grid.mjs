// Keep both DOM size and browser scroll height bounded, even for All results.
export const BATCH_SIZE=1000;
export const CARD_STRIDE=244;
export function chunkWindow({start,end,count,batchSize=BATCH_SIZE}){
  if(!count||end<=start)return {visible:[],prefetch:[]};
  const first=Math.floor(start/batchSize),last=Math.floor((Math.min(count,end)-1)/batchSize),max=Math.ceil(count/batchSize)-1;
  const visible=Array.from({length:last-first+1},(_,i)=>first+i);
  // One batch ahead and behind, bounded even when "All" contains 600k items.
  const prefetch=[last+1,first-1].filter(i=>i>=0&&i<=max);
  return {visible,prefetch};
}
export function pageRange(total,offset,size){
  const count=Math.max(0,Math.min(total-offset,size==='all'?total:Number(size)));
  return {count,start:count?offset+1:0,end:offset+count,previous:offset>0,next:offset+count<total};
}
export function gridWindow({count,width,viewportHeight,scrollOffset=0,minWidth=145,stride=CARD_STRIDE}){
  const columns=Math.max(1,Math.floor((width+12)/(minWidth+12))),rows=Math.ceil(count/columns);
  const naturalHeight=Math.max(0,rows*stride-12),height=Math.min(8_000_000,naturalHeight);
  const physical=Math.max(0,Math.min(scrollOffset,Math.max(0,height-viewportHeight)));
  const logical=height>viewportHeight?physical*(Math.max(0,naturalHeight-viewportHeight)/(height-viewportHeight)):0;
  const first=Math.max(0,Math.floor(logical/stride)-2),last=Math.min(rows,Math.ceil((logical+viewportHeight)/stride)+2);
  return {columns,height,top:physical+first*stride-logical,start:Math.min(count,first*columns),end:Math.min(count,last*columns)};
}
