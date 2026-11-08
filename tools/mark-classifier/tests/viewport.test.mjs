import test from 'node:test';
import assert from 'node:assert/strict';
import {constrainCamera,scrollLayout,zoomAt,resizeCamera,fitBounds,wheelFactor,MIN_SCALE,MAX_SCALE} from '../app/viewport.mjs';

const page={width:2041,height:2918}, viewport={width:970,height:545};
const close=(actual,expected)=>assert.ok(Math.abs(actual-expected)<1e-7,`${actual} != ${expected}`);

test('fit page is centred without phantom scroll ranges on either axis',()=>{
  for(const size of [viewport,{width:640,height:900},{width:1700,height:500}]) {
    const fitted=fitBounds({x:0,y:0,...page},size);
    const layout=constrainCamera(fitted,page,size);
    close(layout.maxLeft,0);close(layout.maxTop,0);
    close(layout.camera.x+page.width*fitted.scale/2,size.width/2);
    close(layout.camera.y+page.height*fitted.scale/2,size.height/2);
  }
});

test('zoom over ink keeps the same image point underneath the pointer',()=>{
  const camera={scale:1,x:-500,y:-700}, pointer={x:400,y:250};
  const before={x:(pointer.x-camera.x)/camera.scale,y:(pointer.y-camera.y)/camera.scale};
  const zoomed=zoomAt(camera,page,viewport,pointer,1.7);
  close((pointer.x-zoomed.x)/zoomed.scale,before.x);
  close((pointer.y-zoomed.y)/zoomed.scale,before.y);
});

test('zooming from blank margins expands around the page, not distant whitespace',()=>{
  const fitted=fitBounds({x:0,y:0,...page},viewport);
  const zoomed=zoomAt(fitted,page,viewport,{x:0,y:0},3);
  close(zoomed.x+page.width*zoomed.scale/2,viewport.width/2);
  close(zoomed.y+page.height*zoomed.scale/2,viewport.height/2);
});

test('zooming out after a deep pan recentres any axis where the page fits',()=>{
  const zoomed=zoomAt({scale:2,x:-2900,y:-4900},page,viewport,{x:500,y:400},.02);
  close(zoomed.x+page.width*zoomed.scale/2,viewport.width/2);
  close(zoomed.y+page.height*zoomed.scale/2,viewport.height/2);
});

test('panning cannot strand the page outside reachable scrollbar bounds',()=>{
  const first=constrainCamera({scale:1,x:1e6,y:1e6},page,viewport);
  assert.equal(first.scrollLeft,0);assert.equal(first.scrollTop,0);
  const last=constrainCamera({scale:1,x:-1e6,y:-1e6},page,viewport);
  assert.equal(last.scrollLeft,last.maxLeft);assert.equal(last.scrollTop,last.maxTop);
  close(last.camera.x+last.scrollLeft,last.originX);
  close(last.camera.y+last.scrollTop,last.originY);
});

test('resizing preserves the image point at the viewport centre when enlarged',()=>{
  const camera={scale:1,x:-500,y:-700}, nextSize={width:1200,height:800};
  const resized=resizeCamera(camera,page,viewport,nextSize);
  close((viewport.width/2-camera.x)/camera.scale,(nextSize.width/2-resized.x)/resized.scale);
  close((viewport.height/2-camera.y)/camera.scale,(nextSize.height/2-resized.y)/resized.scale);
});

test('scroll ranges describe the complete enlarged page with reachable margins',()=>{
  const layout=scrollLayout(page,viewport,2);
  assert.equal(layout.width,page.width*2+64);
  assert.equal(layout.height,page.height*2+64);
  close(layout.originX,32);close(layout.originY,32);
});

test('wheel units and zoom limits are respected',()=>{
  close(wheelFactor(2,1,500),wheelFactor(32,0,500));
  close(wheelFactor(1,2,500),wheelFactor(500,0,500));
  assert.equal(zoomAt({scale:1,x:-100,y:-100},page,viewport,{x:300,y:300},1e10).scale,MAX_SCALE);
  assert.equal(zoomAt({scale:1,x:-100,y:-100},page,viewport,{x:300,y:300},1e-10).scale,MIN_SCALE);
});
