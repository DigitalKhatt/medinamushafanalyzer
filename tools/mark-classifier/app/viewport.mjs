// All camera and scroll coordinates are CSS pixels, independent of device scale.
export const MIN_SCALE = 0.03;
export const MAX_SCALE = 12;
export const PAGE_PADDING = 32;

export function scrollLayout(page, viewport, scale) {
  const width = Math.ceil(Math.max(viewport.width, page.width * scale + PAGE_PADDING * 2) - 1e-8);
  const height = Math.ceil(Math.max(viewport.height, page.height * scale + PAGE_PADDING * 2) - 1e-8);
  return {width, height, originX:(width - page.width * scale) / 2,
    originY:(height - page.height * scale) / 2,
    maxLeft:Math.max(0, width - viewport.width), maxTop:Math.max(0, height - viewport.height)};
}

export function constrainCamera(camera, page, viewport) {
  const layout = scrollLayout(page, viewport, camera.scale);
  const scrollLeft = Math.max(0, Math.min(layout.maxLeft, layout.originX - camera.x));
  const scrollTop = Math.max(0, Math.min(layout.maxTop, layout.originY - camera.y));
  return {...layout, scrollLeft, scrollTop,
    camera:{scale:camera.scale, x:layout.originX - scrollLeft, y:layout.originY - scrollTop}};
}

export function zoomAt(camera, page, viewport, pointer, factor) {
  // Wheel events over blank margin use the visible page centre rather than
  // magnifying a distant point outside the page and pushing the ink off screen.
  const inside = pointer.x >= camera.x && pointer.x <= camera.x + page.width * camera.scale &&
    pointer.y >= camera.y && pointer.y <= camera.y + page.height * camera.scale;
  const anchor = inside ? pointer : {
    x:(Math.max(0,camera.x) + Math.min(viewport.width,camera.x + page.width*camera.scale)) / 2,
    y:(Math.max(0,camera.y) + Math.min(viewport.height,camera.y + page.height*camera.scale)) / 2,
  };
  const scale = Math.max(MIN_SCALE, Math.min(MAX_SCALE, camera.scale * factor));
  return constrainCamera({scale,
    x:anchor.x - (anchor.x-camera.x) * scale/camera.scale,
    y:anchor.y - (anchor.y-camera.y) * scale/camera.scale}, page, viewport).camera;
}

export function resizeCamera(camera, page, previous, viewport) {
  const x = (previous.width / 2 - camera.x) / camera.scale;
  const y = (previous.height / 2 - camera.y) / camera.scale;
  return constrainCamera({scale:camera.scale, x:viewport.width/2-x*camera.scale,
    y:viewport.height/2-y*camera.scale},page,viewport).camera;
}

export function fitBounds(bounds, viewport) {
  const scale = Math.min(MAX_SCALE, Math.max(MIN_SCALE,
    Math.min(Math.max(1,viewport.width-PAGE_PADDING*2)/bounds.width,
      Math.max(1,viewport.height-PAGE_PADDING*2)/bounds.height)));
  return {scale,x:(viewport.width-bounds.width*scale)/2-bounds.x*scale,
    y:(viewport.height-bounds.height*scale)/2-bounds.y*scale};
}

export function wheelFactor(deltaY, deltaMode, viewportHeight) {
  const pixels = deltaY * (deltaMode === 1 ? 16 : deltaMode === 2 ? viewportHeight : 1);
  return Math.exp(Math.max(-1.5, Math.min(1.5, -pixels * 0.0015)));
}
