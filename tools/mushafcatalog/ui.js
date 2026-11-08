export const $ = id => document.getElementById(id);
export const fmt = n => Number(n).toLocaleString('en');
export const labels = {
  pending: 'Body pending', segmented: 'Segmented', partial: 'Partly segmented', sign: 'Sign',
  match: 'Exact match', 'accepted-form': 'Accepted form', different: 'Different identity',
  missing: 'Missing ink', ambiguous: 'Ambiguous', unmatched: 'Unmatched ink',
};
export function el(tag, text, cls) {
  const node = document.createElement(tag);
  if (text != null) node.textContent = text;
  if (cls) node.className = cls;
  return node;
}
export function button(label, fn, cls) {
  const b = el('button', label, cls);
  b.type = 'button'; b.onclick = fn;
  return b;
}
export function kv(key, value) {
  const box = el('div', null, 'kv');
  box.append(el('span', key), el('span', value ?? '—'));
  return box;
}
export async function api(path, params = {}, signal) {
  const r = await fetch(path + '?' + new URLSearchParams(params), {signal});
  if (!r.ok) {
    const doc = new DOMParser().parseFromString(await r.text(), 'text/html');
    throw Error(doc.querySelector('p')?.textContent?.replace(/^Message: /, '') || r.statusText);
  }
  return r.json();
}
export function imageURL(kind, id, context, view = {}) {
  const p = new URLSearchParams({kind, id, context});
  if (kind === 'letters') for (const k of ['pair','kleft','kright']) if (view[k]) p.set(k,'1');
  return '/api/image?' + p;
}
export function inkImage(kind, id, context, cls, view) {
  const node = el('img', null, cls);
  node.src = imageURL(kind,id,context,view);
  node.alt = 'Source ink for ' + id;
  node.loading = 'lazy';
  return node;
}
