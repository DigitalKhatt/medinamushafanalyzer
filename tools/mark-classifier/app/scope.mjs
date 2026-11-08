export function scopeMask(scope,raster){
  if(!scope)return null;
  if(scope.width!==raster.width||scope.height!==raster.height)throw new Error('Frame scope raster does not match this page.');
  const mask=new Uint8Array(raster.width*raster.height);
  for(let i=0;i<scope.runs.length;i+=2)mask.fill(1,scope.runs[i],scope.runs[i]+scope.runs[i+1]);
  return mask;
}
export function scopedArea(runs,mask){
  if(!mask)return runs.reduce((n,v,i)=>n+(i%2?v:0),0);
  let area=0;for(let i=0;i<runs.length;i+=2)for(let p=runs[i];p<runs[i]+runs[i+1];p++)area+=mask[p];
  return area;
}
export function clipLabels(labels,mask){
  if(mask)for(let i=0;i<labels.length;i++)if(!mask[i])labels[i]=0;
  return labels;
}
export function scopePath(scope,raster){
  if(!scope)return null;
  const [x,y,w,h]=scope.viewBox,sx=raster.width/w,sy=raster.height/h;
  const path=new Path2D();path.addPath(new Path2D(scope.interiorPath),new DOMMatrix([sx,0,0,sy,-x*sx,-y*sy]));return path;
}
