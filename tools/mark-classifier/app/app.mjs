import {detectLandmarks, LANDMARK_TYPES, sameLandmark} from './landmarks.mjs';
import {ROLES, labelMap, splitRuns, describeRuns} from './core.mjs';
import {CHARACTERS, POSITIONS, LETTER_ROLES, MARK_ROLES, characterMetadata, completeCharacter, isSingleCharacter,
  partApprovalIssue, formPresets, applyCharacterMetadata, upgradeCharacters} from './characters.mjs';
import {constrainCamera, scrollLayout, zoomAt, resizeCamera, fitBounds, wheelFactor} from './viewport.mjs';
import {assignSubword, subwordApprovalIssue} from './subwords.mjs';
import {scopeMask,scopedArea,clipLabels,scopePath} from './scope.mjs';

const $ = id => document.getElementById(id);
const canvas = $('canvas'), ctx = canvas.getContext('2d');
const scrollport = $('page-scroll'), spacer = $('page-spacer');
let corpus, page, doc, sourceCanvas, colourCanvas, labels, selected = new Set();
let camera = {scale: 1, x: 0, y: 0}, tool = 'select', polygon = [], box = null, drag = null, space = false;
let history = [], editVersion = 0, savedVersion = 0, saveTimer, saving = null, toastTimer, loading = false;
let generation = 0;
let transcriptContext = null;
const initialLinkedPage = location.hash.slice(1);
let previousViewport = null, fitPageMode = true;
let allowedInk=null,frameInterior=null,scopeAreas=new Map();
let landmarkPart = null, landmarkGeometry = null, placingLandmark = false;
const landmarkCache = new WeakMap();
const roleMap = new Map(ROLES.map(r => [r.id, r]));

async function api(url, options) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}

function toast(message, persistent = false) {
  $('toast').textContent = message; $('toast').hidden = false;
  clearTimeout(toastTimer);
  if (!persistent) toastTimer = setTimeout(() => $('toast').hidden = true, 5000);
}

function visibleParts(){return doc?doc.parts.filter(p=>(scopeAreas.get(p.id)??p.area)>0):[];}
function selectedParts() { return visibleParts().filter(p=>selected.has(p.id)); }
function updateScopeGeometry(){
  scopeAreas=new Map(doc.parts.map(p=>[p.id,scopedArea(p.runs,allowedInk)]));
  labels=clipLabels(labelMap(doc.parts,doc.raster.width,doc.raster.height),allowedInk);
}

function beforeEdit() {
  selected=new Set(selectedParts().map(p=>p.id));
  history.push(JSON.stringify({parts: doc.parts, groups: doc.groups, nextGroup: doc.nextGroup,
    subwords: doc.subwords || {}, proposalReview: doc.proposalReview || {}}));
  if (history.length > 25) history.shift();
}

function afterEdit(geometry = false, inkStyle = true) {
  editVersion++;
  if (geometry) updateScopeGeometry();
  if (inkStyle) updateColourCanvas();
  updateUI(); render();
  $('save-status').textContent = 'Unsaved changes';
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => save().catch(error => toast(error.message, true)), 1400);
}

async function save() {
  clearTimeout(saveTimer);
  if (saving) { await saving; if (editVersion !== savedVersion) return save(); return; }
  if (!doc || editVersion === savedVersion) return;
  const version = editVersion, current = doc;
  $('save-status').textContent = 'Saving…';
  saving = api(`/api/pages/${page.id}/annotation`, {method: 'PUT', headers: {
    'Content-Type': 'application/json', 'X-Annotation-Token': corpus.token,
  }, body: JSON.stringify(doc)});
  try {
    const result = await saving;
    current.revision = result.revision;
    savedVersion = version;
    $('save-status').textContent = editVersion === savedVersion ? `Saved · revision ${result.revision}` : 'Unsaved changes';
    const option = [...$('page').options].find(o => o.value === current.pageId);
    if (option && !option.textContent.endsWith(' •')) option.textContent += ' •';
  } catch (error) {
    $('save-status').textContent = 'Save failed · changes remain in this tab';
    throw error;
  } finally { saving = null; }
}

function roleColour(role) {
  const hex = roleMap.get(role).color;
  return [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
}

function updateColourCanvas() {
  const {width, height} = doc.raster;
  colourCanvas = document.createElement('canvas'); colourCanvas.width = width; colourCanvas.height = height;
  const context = colourCanvas.getContext('2d'), pixels = context.createImageData(width, height);
  for (const part of doc.parts) {
    if (part.role === 'unknown') continue;
    const [r, g, b] = roleColour(part.role), alpha = part.reviewed ? 215 : 140;
    for (let j = 0; j < part.runs.length; j += 2) for (let p = part.runs[j]; p < part.runs[j] + part.runs[j + 1]; p++) {
      const i = p * 4; pixels.data[i] = r; pixels.data[i + 1] = g; pixels.data[i + 2] = b; pixels.data[i + 3] = alpha;
    }
  }
  context.putImageData(pixels, 0, 0);
}

function updateUI() {
  if (!doc) return;
  const parts = selectedParts(), roles = new Set(parts.map(p => p.role));
  updateLandmarks(parts);
  $('selection-count').textContent = parts.length ? `${parts.length} selected` : 'No selection';
  for (const button of $('roles').children) {
    button.classList.toggle('active', roles.size === 1 && roles.has(button.dataset.role));
    const role = button.dataset.role;
    button.querySelector('small').textContent = visibleParts().filter(p => p.role === role).length;
  }
  const visible=visibleParts(),inkPixels=visible.reduce((n,p)=>n+(scopeAreas.get(p.id)??p.area),0);
  const reviewed = visible.reduce((n, p) => n + (p.reviewed ? (scopeAreas.get(p.id)??p.area) : 0), 0);
  const percent = reviewed / Math.max(1,inkPixels) * 100;
  $('progress').style.width = `${percent}%`;
  $('reviewed').textContent = `${percent.toFixed(1)}% approved`;
  $('region-count').textContent = `${visible.length.toLocaleString()} regions`;
  $('coverage').textContent = `${reviewed.toLocaleString()} of ${inkPixels.toLocaleString()} ink pixels approved.${allowedInk?' Quran text area only; excluded ink is retained in the original annotation.':' Region boundaries preserve every detected ink pixel.'}`;
  const canAssignTranscript = !!transcriptContext && $('transcript-letter').value !== '' &&
    parts.length > 0 && parts.every(p => ['unknown','body','kashida'].includes(p.role));
  $('transcript-body').disabled = $('transcript-join').disabled = !canAssignTranscript;
  const allReviewed = parts.length && parts.every(p => p.reviewed);
  $('approve').disabled = !parts.length || (!allReviewed && parts.some(p => partApprovalIssue(p, doc.groups)));
  $('approve').innerHTML = parts.length && parts.every(p => p.reviewed) ? 'Reopen selected <kbd>R</kbd>' : 'Approve selected <kbd>R</kbd>';
  $('merge').disabled = parts.length < 2;
  $('group').disabled = !parts.length;
  $('undo').disabled = !history.length;
  const owners = new Set(parts.map(p => p.owner));
  const owner = owners.size === 1 ? parts[0]?.owner : null;
  $('owner').replaceChildren(new Option(owners.size > 1 ? 'Mixed character groups' : 'No character group', ''));
  for (const [id, group] of Object.entries(doc.groups)) {
    const metadata = characterMetadata(group);
    const description = [metadata.label, metadata.position !== 'unknown' ? metadata.position : '', metadata.form !== 'unknown' ? metadata.form : ''].filter(Boolean).join(' · ');
    $('owner').add(new Option(`${id}${description ? ` · ${description}` : ''}`, id));
  }
  $('owner').value = owner || '';
  const metadata = characterMetadata(owner ? doc.groups[owner] : {});
  $('identity').value = metadata.label;
  $('character-position').value = metadata.position;
  $('character-form').value = metadata.form === 'unknown' ? '' : metadata.form;
  updateFormOptions();
  $('identity-save').disabled = !parts.length;
  $('select-group').disabled = !owner;
  const marks = parts.length && parts.every(p=>MARK_ROLES.has(p.role));
  $('mark-type-save').disabled = !marks;
  const types = new Set(parts.map(p=>p.markType || 'unknown'));
  $('mark-type').value = marks && types.size === 1 && !types.has('unknown') ? parts[0].markType : '';
  $('character-status').textContent = owner ? (completeCharacter(metadata) ?
    `${metadata.reviewed ? 'Reviewed' : 'Draft'} character details · changes apply to all regions of ${owner}.` :
    'Choose one character and its joining position before approving letter ink.') :
    'Apply details to create an instance, or choose an existing character above.';
  if (marks && !owner) $('character-status').textContent = 'Dot/mark ink can be approved independently. Character ownership is optional for this pilot.';
  const contexts = new Set(parts.map(p=>p.subwordId || ''));
  const contextId = contexts.size === 1 ? parts[0]?.subwordId || '' : '';
  $('subword-owner').replaceChildren(new Option('No subword', ''));
  for (const [id, sub] of Object.entries(doc.subwords || {}))
    $('subword-owner').add(new Option(`${sub.text} · ${id}${sub.reviewed ? ' ✓' : ''}`,id));
  $('subword-owner').value = contextId;
  $('subword-label').value = doc.subwords?.[contextId]?.text || '';
  $('subword-body').checked = !!parts.length && parts.every(p=>p.subwordBody);
  updateSubwordFields();
  const lines = new Set(parts.map(p => p.line));
  $('line').value = lines.size === 1 ? parts[0]?.line || '' : '';
  const visibleLines=allowedInk?doc.lines.filter(line=>visible.some(p=>p.bbox[1]<line.bottom&&p.bbox[1]+p.bbox[3]>line.top)):doc.lines;
  $('page-info').textContent = `${page.id} · ${doc.raster.width} × ${doc.raster.height} · ${visibleLines.length} line proposals · character / position / form`;
}

function updateLandmarks(parts = selectedParts()) {
  const enabled = $('landmarks').checked;
  $('landmark-panel').hidden = !enabled;
  const bodies = enabled ? parts.filter(p => p.role === 'body') : [];
  const previous = landmarkPart;
  landmarkPart = bodies.find(p => p.id === previous?.id) || bodies[0] || null;
  if (previous !== landmarkPart) placingLandmark = false;
  $('landmark-region').replaceChildren(...bodies.map(p => {
    const meta = characterMetadata(doc.groups[p.owner]);
    return new Option([meta.label || 'Body', meta.position, meta.form, p.id].join(' · '), p.id);
  }));
  $('landmark-region').disabled = !landmarkPart;
  $('landmark-region').value = landmarkPart?.id || '';
  landmarkGeometry = null;
  if (landmarkPart) {
    if (!landmarkCache.has(landmarkPart))
      landmarkCache.set(landmarkPart, detectLandmarks(landmarkPart, doc.raster.width, allowedInk));
    landmarkGeometry = landmarkCache.get(landmarkPart);
  }
  const saved = landmarkPart?.landmarks || [];
  $('landmark-status').textContent = placingLandmark ? 'Click inside this body’s bounding box to place the landmark. Esc cancels.' :
    landmarkGeometry ? `${saved.length} kept · check a feature or click its marker in Select mode.` :
    landmarkPart ? 'This region is too large. Select a smaller letter body.' : 'Select a letter body to see its features.';
  $('landmark-add').disabled = !landmarkGeometry || saved.length >= 128;
  $('landmark-add').setAttribute('aria-pressed', String(placingLandmark));
  $('landmark-list').replaceChildren(...landmarkPoints().filter(landmarkVisible).map(point => {
    const label = document.createElement('label'), checkbox = document.createElement('input');
    const text = document.createElement('span');
    checkbox.type = 'checkbox';
    checkbox.checked = saved.some(p => sameLandmark(p, point));
    checkbox.onchange = () => toggleLandmark(point);
    text.textContent = `${LANDMARK_TYPES[point.kind]} · ${point.x}, ${point.y}${point.source === 'manual' ? ' · manual' : ''}`;
    label.append(checkbox, text);
    return label;
  }));
}

function landmarkPoints() {
  const detected = landmarkGeometry?.points || [];
  const saved = landmarkPart?.landmarks || [];
  return [...detected, ...saved.filter(p => !detected.some(candidate => sameLandmark(p, candidate)))];
}

function landmarkVisible(point) {
  return $(point.kind.startsWith('contour') ? 'landmark-contour' : 'landmark-skeleton').checked;
}

function toggleLandmark(point) {
  const saved = landmarkPart.landmarks || [];
  const index = saved.findIndex(p => sameLandmark(p, point));
  if (index < 0 && saved.length >= 128) {toast('This body already has 128 landmarks.'); return;}
  beforeEdit();
  landmarkPart.landmarks = index < 0 ? [...saved, point] : saved.filter((_, i) => i !== index);
  afterEdit(false, false);
}

function renderLandmarks() {
  if (!landmarkGeometry) return;
  const size = 1 / camera.scale;
  ctx.save();
  ctx.lineWidth = 1.5 * size;
  if ($('landmark-contour').checked) {
    ctx.strokeStyle = '#b54708';
    ctx.beginPath();
    for (const loop of landmarkGeometry.contours) {
      loop.forEach(([x,y],i) => i ? ctx.lineTo(x,y) : ctx.moveTo(x,y));
      ctx.closePath();
    }
    ctx.stroke();
  }
  if ($('landmark-skeleton').checked) {
    ctx.fillStyle = '#00bfd1';
    for (const [x,y] of landmarkGeometry.skeleton) ctx.fillRect(x-.5,y-.5,1,1);
  }
  const saved = landmarkPart.landmarks || [];
  for (const point of landmarkPoints().filter(landmarkVisible)) {
    const kept = saved.some(p => sameLandmark(p,point));
    ctx.beginPath();
    ctx.arc(point.x, point.y, (kept ? 5 : 3.5) * size, 0, Math.PI * 2);
    ctx.strokeStyle = point.kind.startsWith('contour') ? '#a43c00' : '#006b78';
    ctx.fillStyle = kept ? ctx.strokeStyle : '#ffffffdb';
    ctx.fill(); ctx.stroke();
    if (kept) {
      ctx.fillStyle = '#fff';
      ctx.fillRect(point.x-size,point.y-size,2*size,2*size);
    }
  }
  ctx.restore();
}

function landmarkClick([x,y]) {
  if (!landmarkGeometry) return false;
  if (placingLandmark) {
    const [left,top,w,h] = landmarkPart.bbox;
    if (x < left || y < top || x > left+w || y > top+h) {
      toast('Place the landmark within the selected letter body’s bounding box.');
      return true;
    }
    placingLandmark = false;
    const kind = $('landmark-kind').value;
    $(kind.startsWith('contour') ? 'landmark-contour' : 'landmark-skeleton').checked = true;
    toggleLandmark({kind, x:Math.round(x*2)/2, y:Math.round(y*2)/2, source:'manual'});
    return true;
  }
  const hit = landmarkPoints().filter(landmarkVisible).map(point => [point,Math.hypot(x-point.x,y-point.y)])
    .filter(([,distance]) => distance * camera.scale <= 7).sort((a,b) => a[1]-b[1])[0];
  if (hit) toggleLandmark(hit[0]);
  return !!hit;
}

function updateSubwordFields() {
  const id = $('subword-owner').value, sub = doc.subwords?.[id];
  $('subword-assign').disabled = !selected.size;
  $('subword-approve').disabled = !id || (!sub?.reviewed && !!subwordApprovalIssue(doc,id));
  $('subword-approve').textContent = sub?.reviewed ? 'Reopen subword boundary' : 'Approve body & text';
  $('subword-status').textContent = sub ? `${sub.reviewed ? 'Reviewed' : 'Draft'} subword · character and kashida cuts remain separate.` : 'Create or choose a subword to assign the selected ink.';
}

function updateFormOptions() {
  $('form-options').replaceChildren(...formPresets($('identity').value).map(form => new Option(form.label, form.id)));
}

function setRole(role) {
  const parts = selectedParts();
  if (!parts.length) {toast('Select ink regions first.'); return;}
  beforeEdit();
  parts.forEach(p => {
    p.role = role; p.reviewed = false;
    if (!MARK_ROLES.has(role)) delete p.markType;
    if (p.subwordBody && ['dot','mark','symbol','ornament','other'].includes(role)) {
      if (doc.subwords?.[p.subwordId]) Object.assign(doc.subwords[p.subwordId],{reviewed:false,decision:'pending'});
      p.subwordBody = false; p.subwordAttachmentReviewed = false;
    }
  });
  afterEdit();
}

function approve() {
  const parts = selectedParts();
  if (!parts.length) return;
  const reopening = parts.every(p => p.reviewed);
  const issue = reopening ? null : parts.map(p => partApprovalIssue(p, doc.groups)).find(Boolean);
  if (issue) {toast(issue); return;}
  beforeEdit();
  parts.forEach(p => {
    p.reviewed = !reopening;
    if (!reopening && LETTER_ROLES.has(p.role) && p.owner && completeCharacter(doc.groups[p.owner]))
      doc.groups[p.owner].reviewed = true;
  });
  afterEdit();
}

function group() {
  const parts = selectedParts(); if (!parts.length) return;
  beforeEdit();
  const id = `char-${String(doc.nextGroup++).padStart(4, '0')}`;
  doc.groups[id] = characterMetadata();
  parts.forEach(p => {p.owner = id; p.reviewed = false;});
  afterEdit();
  toast(`Created ${id}. Choose its character, joining position and form, then apply the details.`);
}

function setTool(next) {
  tool = next; polygon = []; box = null;
  if (placingLandmark) {placingLandmark = false; updateLandmarks();}
  for (const name of ['select', 'box', 'polygon', 'pan']) $(name + '-tool').classList.toggle('active', name === next);
  $('tool-hint').textContent = {
    select: 'Click ink to select. Shift-click adds regions. Scroll to zoom; hold Space to pan.',
    box: 'Drag a rectangle to select ink within it. Shift-drag adds to the selection.',
    polygon: 'Select regions first. Click around the portion to separate; press Enter to split. Escape cancels. All ink is retained.',
    pan: 'Drag to move the page. Scroll to zoom. Press V to return to selection.',
  }[tool];
  canvas.style.cursor = {select:'default', box:'crosshair', polygon:'crosshair', pan:'grab'}[tool];
  render();
}

function resize() {
  const viewport = viewportSize(), dpr = devicePixelRatio || 1;
  canvas.style.width = `${viewport.width}px`; canvas.style.height = `${viewport.height}px`;
  canvas.width = Math.round(viewport.width * dpr); canvas.height = Math.round(viewport.height * dpr);
  const previous = previousViewport;
  previousViewport = viewport;
  if (!doc) {render(); return;}
  if (fitPageMode) fit();
  else setCamera(previous ? resizeCamera(camera, doc.raster, previous, viewport) : camera);
}

function viewportSize() {return {width:scrollport.clientWidth,height:scrollport.clientHeight};}

function setCamera(next) {
  if (!doc) return;
  const layout = constrainCamera(next, doc.raster, viewportSize());
  spacer.style.width = `${layout.width}px`; spacer.style.height = `${layout.height}px`;
  scrollport.scrollLeft = layout.scrollLeft; scrollport.scrollTop = layout.scrollTop;
  // Read the browser's actual subpixel scroll offsets so drawing and hit tests
  // share exactly the same transform as the native scrollbar positions.
  camera = {scale:next.scale,x:layout.originX-scrollport.scrollLeft,y:layout.originY-scrollport.scrollTop};
  render();
}

function fit(bbox = null) {
  if (!doc) return;
  const [x, y, width, height] = bbox || [0, 0, doc.raster.width, doc.raster.height];
  fitPageMode = bbox === null;
  setCamera(fitBounds({x,y,width,height},viewportSize()));
}

async function loadTranscriptContext(pageId, params, requested) {
  transcriptContext = null;
  $('transcript-labeling').hidden = true;
  if (!params.has('subword') && !requested.size) return;
  try {
    const source = params.get('proposalSource') === 'legacy' ? 'legacy' : 'automatic';
    const proposal = await api(`/api/pages/${pageId}/proposals?source=${source}`);
    const matching = new Set(proposal.candidates
      .filter(candidate => candidate.kind === 'subword-ink' && candidate.inScope !== false &&
        candidate.regionIds.some(id => requested.has(id)))
      .map(candidate => candidate.subwordId).filter(Boolean));
    const id = params.get('subword') || (matching.size === 1 ? [...matching][0] : null);
    const subword = proposal.subwords.find(entry => entry.id === id && entry.inScope !== false);
    if (!subword) return;
    const letters = (subword.characters || []).flatMap(({text, position}, offset) =>
      isSingleCharacter(text) && POSITIONS.includes(position) && position !== 'unknown'
        ? [{label: text.normalize('NFKC'), position, offset}] : []);
    if (!letters.length) return;
    const bodyIds = new Set(subword.bodyCandidateIds);
    const regions = new Set(proposal.candidates.filter(candidate => bodyIds.has(candidate.id))
      .flatMap(candidate => candidate.regionIds));
    transcriptContext = {id: subword.id, proposal: proposal.proposalSha256, letters, regions};
    $('transcript-context').textContent = `Transcript · ${subword.text}`;
    $('transcript-letter').replaceChildren(new Option('Choose a letter', ''),
      ...letters.map((letter, index) =>
        new Option(`${index + 1}. ${letter.label} · ${letter.position}`, String(index))));
    $('transcript-labeling').hidden = false;
  } catch (error) {
    if (params.has('subword')) toast(`Transcript unavailable. ${error.message}`);
  }
}

function assignTranscriptInk(role) {
  const value = $('transcript-letter').value;
  const letter = value === '' ? null : transcriptContext?.letters[Number(value)];
  const parts = selectedParts();
  if (!letter || !parts.length || parts.some(p => !['unknown','body','kashida'].includes(p.role))) return;
  const key = `${transcriptContext.proposal}:${transcriptContext.id}:${letter.offset}`;
  const matchingGroups = Object.entries(doc.groups).filter(([, group]) =>
    group.label === letter.label && group.position === letter.position);
  const localOwners = new Set(doc.parts.filter(part => transcriptContext.regions.has(part.id)).map(part => part.owner));
  const localMatches = matchingGroups.filter(([id, group]) => !group.transcriptLetter && localOwners.has(id));
  const repeated = transcriptContext.letters.some(other => other !== letter &&
    other.label === letter.label && other.position === letter.position);
  const existing = matchingGroups.find(([, group]) => group.transcriptLetter === key) ||
    (!repeated && localMatches.length === 1 ? localMatches[0] : null);
  const changed = parts.filter(part => part.role !== role || part.owner !== existing?.[0]);
  if (!changed.length) return;
  beforeEdit();
  const owner = existing?.[0] || `char-${String(doc.nextGroup++).padStart(4, '0')}`;
  if (!existing) {
    doc.groups[owner] = {label: letter.label, position: letter.position, form: 'unknown',
      reviewed: false, transcriptLetter: key};
  }
  for (const part of changed) Object.assign(part, {owner, role, reviewed: false});
  afterEdit();
}

function fitSelection() {
  const parts = selectedParts(); if (!parts.length) return;
  const x = Math.min(...parts.map(p => p.bbox[0])), y = Math.min(...parts.map(p => p.bbox[1]));
  const right = Math.max(...parts.map(p => p.bbox[0] + p.bbox[2])), bottom = Math.max(...parts.map(p => p.bbox[1] + p.bbox[3]));
  fit([x - 20, y - 20, right - x + 40, bottom - y + 40]);
}

function render() {
  const dpr = devicePixelRatio || 1;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, canvas.width / dpr, canvas.height / dpr);
  if (!doc || !sourceCanvas) return;
  ctx.translate(camera.x, camera.y); ctx.scale(camera.scale, camera.scale);
  ctx.shadowColor = '#29412a20'; ctx.shadowBlur = 18 / camera.scale;
  ctx.fillStyle = 'white'; ctx.fillRect(0, 0, doc.raster.width, doc.raster.height); ctx.shadowBlur = 0;
  ctx.save();if(frameInterior)ctx.clip(frameInterior,'evenodd');
  ctx.drawImage(sourceCanvas, 0, 0);
  if ($('overlay').checked && colourCanvas) ctx.drawImage(colourCanvas, 0, 0);
  if ($('line-guides').checked) {
    ctx.fillStyle = '#71976514'; ctx.strokeStyle = '#71976588'; ctx.lineWidth = 1 / camera.scale;
    ctx.setLineDash([4 / camera.scale, 4 / camera.scale]);
    for (const line of doc.lines) {
      ctx.fillRect(0, line.top, doc.raster.width, line.bottom - line.top);
      ctx.strokeRect(0, line.top, doc.raster.width, line.bottom - line.top);
    }
    ctx.setLineDash([]);
  }
  ctx.strokeStyle = '#eab63f'; ctx.lineWidth = 1.5 / camera.scale;
  for (const part of selectedParts()) {
    const [x, y, w, h] = part.bbox, padding = 2 / camera.scale;
    ctx.strokeRect(x - padding, y - padding, w + 2 * padding, h + 2 * padding);
  }
  renderLandmarks();
  if (polygon.length) {
    ctx.beginPath(); polygon.forEach(([x, y], i) => i ? ctx.lineTo(x, y) : ctx.moveTo(x, y));
    if (polygon.length > 2) ctx.closePath();
    ctx.fillStyle = '#eab63f22'; ctx.fill(); ctx.strokeStyle = '#b27807'; ctx.stroke();
    for (const [x, y] of polygon) {ctx.beginPath(); ctx.arc(x, y, 3 / camera.scale, 0, Math.PI * 2); ctx.fillStyle = '#b27807'; ctx.fill();}
  }
  if (box) {ctx.strokeStyle = '#42764c'; ctx.fillStyle = '#42764c18'; ctx.fillRect(box[0], box[1], box[2], box[3]); ctx.strokeRect(box[0], box[1], box[2], box[3]);}
  ctx.restore();
  $('zoom').textContent = `${Math.round(camera.scale * doc.raster.scale * 100)}%`;
}

function position(event) {
  const rect = canvas.getBoundingClientRect();
  return [(event.clientX - rect.left - camera.x) / camera.scale, (event.clientY - rect.top - camera.y) / camera.scale];
}

function hitPart(x, y) {
  const {width, height} = doc.raster, radius = Math.min(12, Math.ceil(4 / camera.scale));
  let best = 0, distance = Infinity;
  for (let dy = -radius; dy <= radius; dy++) for (let dx = -radius; dx <= radius; dx++) {
    const px = Math.floor(x) + dx, py = Math.floor(y) + dy;
    if (px < 0 || px >= width || py < 0 || py >= height || dx * dx + dy * dy >= distance) continue;
    const label = labels[py * width + px];
    if (label) {best = label; distance = dx * dx + dy * dy;}
  }
  return best ? doc.parts[best - 1] : null;
}

function finishSplit() {
  const parts = selectedParts();
  if (!parts.length || polygon.length < 3) {toast('Select ink and draw a polygon with at least three points.'); return;}
  const replacements = new Map(), innerIds = [];
  for (const part of parts) {
    const {inner, outer} = splitRuns(part.runs, doc.raster.width, polygon);
    if (!inner.length || !outer.length) continue;
    const inside = {...part, id: `part-${crypto.randomUUID()}`, runs: inner, reviewed: false, ...describeRuns(inner, doc.raster.width)};
    const outside = {...part, runs: outer, reviewed: false, ...describeRuns(outer, doc.raster.width)};
    delete inside.landmarks; delete outside.landmarks;
    replacements.set(part.id, [outside, inside]); innerIds.push(inside.id);
  }
  if (!replacements.size) {toast('The polygon must divide selected ink into two nonempty portions.'); return;}
  beforeEdit();
  doc.parts = doc.parts.flatMap(p => replacements.get(p.id) || [p]);
  selected = new Set(innerIds); polygon = []; setTool('select');
  afterEdit(true); toast('Split preserved all ink. The inside portion is selected; assign its role.');
}

function mergeParts() {
  const parts = selectedParts(); if (parts.length < 2) return;
  beforeEdit();
  const spans = parts.flatMap(p => Array.from({length: p.runs.length / 2}, (_, i) => [p.runs[i * 2], p.runs[i * 2 + 1]])).sort((a, b) => a[0] - b[0]);
  const runs = [];
  for (const [start, length] of spans) {
    if (runs.length && runs[runs.length - 2] + runs[runs.length - 1] === start) runs[runs.length - 1] += length;
    else runs.push(start, length);
  }
  const common = key => parts.every(p => p[key] === parts[0][key]) ? parts[0][key] : null;
  const merged = {...parts[0], runs, role: common('role') || 'unknown', owner: common('owner'), line: common('line'),
    markType: common('markType') || 'unknown', reviewed: false, subwordId: common('subwordId'),
    subwordBody: parts.every(p => p.subwordBody === true) && !!common('subwordId'),
    subwordAttachmentReviewed: parts.every(p => p.subwordAttachmentReviewed === true) && !!common('subwordId'),
    ...describeRuns(runs, doc.raster.width)};
  delete merged.proposalSource;
  delete merged.landmarks;
  for (const p of parts) if (p.subwordId && doc.subwords?.[p.subwordId] &&
      (p.subwordId !== merged.subwordId || !!p.subwordBody !== merged.subwordBody))
    Object.assign(doc.subwords[p.subwordId], {reviewed:false,decision:'pending'});
  doc.parts = doc.parts.filter(p => !selected.has(p.id)); doc.parts.push(merged);
  selected = new Set([merged.id]); afterEdit(true);
}

async function extract(source, raster) {
  const context = source.getContext('2d', {willReadFrequently: true});
  const rgba = context.getImageData(0, 0, raster.width, raster.height).data;
  const worker = new Worker('/worker.mjs', {type: 'module'});
  return new Promise((resolve, reject) => {
    worker.onmessage = ({data}) => {worker.terminate(); data.error ? reject(new Error(data.error)) : resolve(data);};
    worker.onerror = error => {worker.terminate(); reject(new Error(error.message));};
    worker.postMessage({...raster, rgba: rgba.buffer}, [rgba.buffer]);
  });
}

async function openPage(id) {
  if (loading) return;
  loading = true;
  const currentGeneration = ++generation;
  $('loading').hidden = false;
  $('save').disabled = true; $('page').disabled = true;
  try {
    await save();
    const data = await api(`/api/pages/${id}`);
    const nextPage = data.page;
    const raster = data.annotation?.raster || {
      width: Math.round(nextPage.viewBox[2] * corpus.config.raster_scale),
      height: Math.round(nextPage.viewBox[3] * corpus.config.raster_scale),
      scale: corpus.config.raster_scale, threshold: corpus.config.ink_threshold,
      chroma: corpus.config.max_chroma, viewBox: nextPage.viewBox,
    };
    const img = new Image(); img.src = `/api/pages/${id}/source.svg`;
    await img.decode();
    const source = document.createElement('canvas'); source.width = raster.width; source.height = raster.height;
    const context = source.getContext('2d', {willReadFrequently: true});
    context.fillStyle = '#fff'; context.fillRect(0, 0, raster.width, raster.height);
    context.drawImage(img, 0, 0, raster.width, raster.height);
    let nextDoc = data.annotation;
    if (!nextDoc) {
      const result = await extract(source, raster);
      nextDoc = {schemaVersion: 1, characterMetadataVersion: 1, pageId: id, sourceSha256: nextPage.sha256, revision: 0,
        raster, ...result, groups: {}, nextGroup: 1};
    }
    const migration = upgradeCharacters(nextDoc);
    if (currentGeneration !== generation) return;
    page = nextPage; doc = nextDoc; sourceCanvas = source;
    allowedInk=scopeMask(data.sourceScope,raster);frameInterior=scopePath(data.sourceScope,raster);updateScopeGeometry();
    if(data.sourceScope){
      const clean=new Image();clean.src=`/api/pages/${id}/source.svg?scope=interior`;await clean.decode();
      context.fillStyle='#fff';context.fillRect(0,0,raster.width,raster.height);context.drawImage(clean,0,0,raster.width,raster.height);
    }
    selected.clear(); history = []; editVersion = savedVersion = 0;
    $('save-status').textContent = doc.revision ? `Saved · revision ${doc.revision}` : 'New page · unlabelled proposals';
    $('page').value = id; $('split').textContent = nextPage.split;
    location.hash = id; polygon = []; box = null;
    updateColourCanvas(); updateUI(); fit();
    $('vector-review').href = `/review.html#${id}`;
    const params = new URLSearchParams(location.search);
    const linkedPage = params.get('contextPage') || initialLinkedPage;
    const requested = new Set(id === linkedPage ? (params.get('regions') || '').split(',').filter(Boolean) : []);
    if (id === linkedPage && params.get('landmarks') === '1') $('landmarks').checked = true;
    await loadTranscriptContext(id, id === linkedPage ? params : new URLSearchParams(), requested);
    selected = new Set(visibleParts().filter(p => requested.has(p.id)).map(p => p.id));
    updateUI();
    if (selected.size) fitSelection();
    if (id === linkedPage && params.get('landmarks') === '1') $('landmark-panel').scrollIntoView({block:'nearest'});
    if (data.annotation && migration.changed) {
      editVersion++;
      $('save-status').textContent = 'Character metadata updated · save to retain';
      if (migration.reopened) toast(`${migration.reopened} legacy letter regions reopened: add character and position before approving.`);
    }
  } catch (error) {
    toast(error.message, true);
    if (page) $('page').value = page.id;
  } finally {
    loading = false; $('loading').hidden = true;
    $('save').disabled = false; $('page').disabled = false;
  }
}

canvas.addEventListener('pointerdown', event => {
  if (!doc || loading) return;
  const point = position(event);
  canvas.setPointerCapture(event.pointerId);
  if (space || tool === 'pan' || event.button === 1) {
    event.preventDefault();
    drag = {type: 'pan', x: event.clientX, y: event.clientY, camera: {...camera}}; return;
  }
  if (tool === 'polygon') {
    if (!selected.size) {toast('Select the regions you want to split before drawing.'); return;}
    polygon.push(point); render(); return;
  }
  if (tool === 'box') {drag = {type:'box', point, additive:event.shiftKey}; box = [...point, 0, 0]; return;}
  if (event.button === 0 && landmarkClick(point)) return;
  const part = hitPart(...point);
  if (!event.shiftKey) selected.clear();
  if (part) {if (event.shiftKey && selected.has(part.id)) selected.delete(part.id); else selected.add(part.id);}
  updateUI(); render();
});
canvas.addEventListener('pointermove', event => {
  if (!drag) return;
  if (drag.type === 'pan') {
    fitPageMode = false;
    setCamera({...camera,x:drag.camera.x + event.clientX-drag.x,y:drag.camera.y + event.clientY-drag.y});
    return;
  } else {
    const point = position(event); box = [drag.point[0], drag.point[1], point[0] - drag.point[0], point[1] - drag.point[1]];
  }
  render();
});
canvas.addEventListener('pointerup', () => {
  if (drag?.type === 'box' && box) {
    if (!drag.additive) selected.clear();
    const minX = Math.max(0, Math.floor(Math.min(box[0], box[0] + box[2]))), minY = Math.max(0, Math.floor(Math.min(box[1], box[1] + box[3])));
    const maxX = Math.min(doc.raster.width, Math.ceil(Math.max(box[0], box[0] + box[2]))), maxY = Math.min(doc.raster.height, Math.ceil(Math.max(box[1], box[1] + box[3])));
    for (let y = minY; y < maxY; y++) for (let x = minX; x < maxX; x++) {
      const label = labels[y * doc.raster.width + x]; if (label) selected.add(doc.parts[label - 1].id);
    }
    box = null; updateUI();
  }
  drag = null; render();
});
canvas.addEventListener('wheel', event => {
  event.preventDefault(); if (!doc || loading) return;
  const rect = canvas.getBoundingClientRect(), viewport = viewportSize();
  fitPageMode = false;
  setCamera(zoomAt(camera,doc.raster,viewport,
    {x:event.clientX-rect.left,y:event.clientY-rect.top},wheelFactor(event.deltaY,event.deltaMode,viewport.height)));
}, {passive:false});

scrollport.addEventListener('scroll', () => {
  if (!doc) return;
  const layout = scrollLayout(doc.raster,viewportSize(),camera.scale);
  camera.x = layout.originX-scrollport.scrollLeft; camera.y = layout.originY-scrollport.scrollTop;
  render();
});

for (const [index, role] of ROLES.entries()) {
  const button = document.createElement('button'); button.className = 'role'; button.dataset.role = role.id;
  button.innerHTML = `<span class="swatch" style="background:${role.color}"></span><span>${role.label}</span><small>0</small><kbd>${index}</kbd>`;
  button.onclick = () => setRole(role.id); $('roles').append(button);
}
for (const name of ['select', 'box', 'polygon', 'pan']) $(name + '-tool').onclick = () => setTool(name);
$('approve').onclick = approve; $('group').onclick = group; $('merge').onclick = mergeParts;
$('finish-split').onclick = finishSplit; $('cancel-split').onclick = () => {polygon = []; render();};
$('fit').onclick = () => fit(); $('fit-selection').onclick = fitSelection;
$('overlay').onchange = render; $('line-guides').onchange = render;
$('transcript-letter').onchange = updateUI;
$('transcript-body').onclick = () => assignTranscriptInk('body');
$('transcript-join').onclick = () => assignTranscriptInk('kashida');
$('owner').onchange = () => {
  const parts = selectedParts(); if (!parts.length) return;
  beforeEdit(); parts.forEach(p => {p.owner = $('owner').value || null; p.reviewed = false;}); afterEdit();
};
$('identity-save').onclick = () => {
  const parts = selectedParts(); if (!parts.length) return;
  const label = $('identity').value.trim().normalize('NFKC');
  const position = $('character-position').value;
  const form = $('character-form').value.trim() || 'unknown';
  if (label && !isSingleCharacter(label)) {toast('Choose one base letter. Keep unresolved multi-letter ligatures in draft.'); return;}
  if (!/^[a-z][a-z0-9_-]{0,63}$/.test(form)) {toast('Use a form tag such as zigzag, expanded, or your own lowercase tag.'); return;}
  beforeEdit();
  let owner = $('owner').value;
  if (!owner) {
    owner = `char-${String(doc.nextGroup++).padStart(4, '0')}`;
    parts.forEach(p => {p.owner = owner;});
  }
  applyCharacterMetadata(doc, owner, {label, position, form});
  afterEdit();
};
$('identity').addEventListener('input', updateFormOptions);
$('mark-type-save').onclick = () => {
  const parts = selectedParts();
  if (!parts.length || !parts.every(p=>MARK_ROLES.has(p.role))) {toast('Assign Dot or Mark to the selected regions first.'); return;}
  const type = $('mark-type').value.trim() || 'unknown';
  if (!/^[a-z][a-z0-9_-]{0,63}$/.test(type)) {toast('Use a lowercase type such as double-dot, fatha, or a custom tag.'); return;}
  beforeEdit(); parts.forEach(p=>{p.markType = type; p.reviewed = false;}); afterEdit();
};
$('line-save').onclick = () => {
  const parts = selectedParts(); if (!parts.length) return;
  const line = $('line').value === '' ? null : Number($('line').value);
  if (line !== null && (!Number.isInteger(line) || line < 1 || line > 200)) {toast('Use a line number from 1 to 200, or leave it empty.'); return;}
  beforeEdit(); parts.forEach(p => {p.line = line; p.reviewed = false;}); afterEdit();
};
$('subword-owner').onchange = () => {
  $('subword-label').value = doc.subwords?.[$('subword-owner').value]?.text || '';
  updateSubwordFields();
};
function applySubword(create) {
  const parts = selectedParts(); if (!parts.length) {toast('Select ink first.'); return;}
  const id = create ? `subword-${crypto.randomUUID()}` : $('subword-owner').value;
  const text = $('subword-label').value.trim();
  try {
    beforeEdit();
    assignSubword(doc,parts,{id,text,body:$('subword-body').checked});
    afterEdit();
  } catch (error) {toast(error.message);}
}
$('subword-new').onclick = () => applySubword(true);
$('subword-assign').onclick = () => applySubword(false);
$('subword-approve').onclick = () => {
  const id = $('subword-owner').value, sub = doc.subwords?.[id];
  if (!sub) return;
  const issue = sub.reviewed ? null : subwordApprovalIssue(doc,id);
  if (issue) {toast(issue); return;}
  beforeEdit(); sub.reviewed = !sub.reviewed; sub.decision = sub.reviewed ? 'approved' : 'pending'; afterEdit();
};
$('select-group').onclick = () => {
  const owner = $('owner').value; if (!owner) return;
  selected = new Set(doc.parts.filter(p => p.owner === owner).map(p => p.id)); updateUI(); render();
};
$('landmark-kind').replaceChildren(...Object.entries(LANDMARK_TYPES).map(([id,name]) => new Option(name,id)));
$('landmark-kind').value = 'contour-turn';
for (const id of ['landmarks', 'landmark-contour', 'landmark-skeleton']) {
  $(id).onchange = () => {
    updateLandmarks(); render();
    if (id === 'landmarks' && $(id).checked) $('landmark-panel').scrollIntoView({block:'nearest'});
  };
}
$('landmark-region').onchange = () => {
  const next = selectedParts().find(p => p.id === $('landmark-region').value);
  if (!next) return;
  placingLandmark = false; landmarkPart = next; updateLandmarks(); render();
};
$('landmark-add').onclick = () => {
  const next = !placingLandmark;
  setTool('select'); placingLandmark = next; updateLandmarks(); render();
};
$('undo').onclick = () => {
  const previous = history.pop(); if (!previous) return;
  Object.assign(doc, JSON.parse(previous)); selected.clear(); polygon = []; afterEdit(true);
};
$('save').onclick = () => {
  if (doc && doc.revision === 0 && editVersion === savedVersion) editVersion++;
  save().catch(error => toast(error.message, true));
};
$('export').onclick = async () => {
  try {
    if (!doc.parts.some(p => p.reviewed) && !Object.values(doc.subwords || {}).some(s=>s.reviewed)) {toast('Approve some labelled ink or subwords before exporting training masks.'); return;}
    await save(); const result = await api(`/api/pages/${page.id}/export`);
    toast(`Exported ${result.reviewedInkPixels.toLocaleString()} approved ink pixels to ${result.directory}`, true);
  } catch (error) {toast(error.message, true);}
};
$('page').onchange = () => openPage($('page').value);
function navigate(delta) {
  if (loading || !page) return;
  const index = corpus.pages.findIndex(p => p.id === page.id) + delta;
  if (corpus.pages[index]) openPage(corpus.pages[index].id);
}
$('previous').onclick = () => navigate(-1); $('next').onclick = () => navigate(1);
document.addEventListener('keydown', event => {
  const editable = /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName);
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 's') {event.preventDefault(); $('save').click(); return;}
  if (editable || loading) return;
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'z') {event.preventDefault(); $('undo').click(); return;}
  if (event.code === 'Space') {space = true; event.preventDefault(); return;}
  if (event.key === 'Escape') {polygon = []; box = null; placingLandmark = false; updateLandmarks(); render(); return;}
  if (event.key === 'Enter' && tool === 'polygon') {finishSplit(); return;}
  const key = event.key.toLowerCase();
  if (/^[0-7]$/.test(key)) setRole(ROLES[Number(key)].id);
  else if (key === 'r') approve(); else if (key === 'g') group();
  else if ({v:'select',b:'box',p:'polygon',h:'pan'}[key]) setTool({v:'select',b:'box',p:'polygon',h:'pan'}[key]);
});
document.addEventListener('keyup', event => {if (event.code === 'Space') space = false;});
window.addEventListener('blur', () => {space = false; drag = null;});
window.addEventListener('beforeunload', event => {if (editVersion !== savedVersion) {event.preventDefault(); event.returnValue = '';}});
new ResizeObserver(resize).observe($('canvas-host'));

async function start() {
  try {
    for (const [character, name] of CHARACTERS) $('character-options').append(new Option(name, character));
    corpus = await api('/api/corpus');
    for (const p of corpus.pages) $('page').add(new Option(`${Number(p.id.slice(4))}${p.hasAnnotation ? ' •' : ''}`, p.id));
    const requested = location.hash.slice(1);
    const first = corpus.pages.find(p => p.id === requested)?.id || corpus.pages.find(p => p.id === 'page003')?.id || corpus.pages[0].id;
    await openPage(first);
  } catch (error) {toast(error.message, true); $('loading').hidden = true;}
}
start();
