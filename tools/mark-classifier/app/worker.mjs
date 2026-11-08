import {makeInkMask, connectedComponents, proposeLines} from './core.mjs';
self.onmessage = ({data}) => {
  try {
    const mask = makeInkMask(new Uint8ClampedArray(data.rgba), data.threshold, data.chroma);
    const {parts} = connectedComponents(mask, data.width, data.height);
    self.postMessage({parts, lines: proposeLines(mask, data.width, data.height),
      inkPixels: parts.reduce((n, p) => n + p.area, 0)});
  } catch (error) { self.postMessage({error: String(error.message || error)}); }
};
