import { $, api, button, el, imageURL, inkImage, kv, labels } from './ui.js';
let inspectionId = null, inspectRevision = 0, preview = {};
const inkLegend = 'Pale green for surrounding ink, dark green for the matched body, brown for marks and dots, and bright blue for the selected connector.';
function selectInk(kind, id, description) {
  const image = $('wordInk');
  if (!image) return;
  image.src = imageURL(kind, id, 'word', preview);
  image.alt = description;
  $('selection').textContent = description;
  $('inkLegend').textContent = kind === 'marks' && preview.marks !== false
    ? 'The selected mark is highlighted in the whole word.'
    : preview.marks === false ? 'Marks and dots are hidden.' : inkLegend;
  for (const b of $('inspection').querySelectorAll('button[data-ink]'))
    b.classList.toggle('selected', b.dataset.ink === id);
}
function inkButton(title, kind, id, description) {
  const b = button(title, () => selectInk(kind, id, description), 'inline-ink');
  b.dataset.ink = id;
  return b;
}
export async function openWord(wid, focus, kind, { marks = true, kleft, kright } = {}) {
  const view = { marks, kleft, kright };
  preview = view;
  const token = [wid,focus,kind,JSON.stringify(view)].join('|');
  if (inspectionId === token) return;
  inspectionId = token;
  const request = ++inspectRevision;
  if (!wid) {
    $('inspection').replaceChildren(el('p','Select a result to inspect its ink and transcript.','hint'));
    return;
  }
  $('inspection').replaceChildren(el('p', 'Loading word…'));
  try {
    const w = await api('/api/word', { id: wid });
    if (request !== inspectRevision) return;
    const box = $('inspection'),
      icon = inkImage('words', wid, 'word', 'inspect-art', view),
      description = el('p', wid, 'focus-label');
    icon.id = 'wordInk';
    description.id = 'selection';
    const title = el('div', null, 'arabic selected-word');
    title.lang='ar'; title.dir='rtl';
    const ids = new Set((focus || '').split(','));
    const matched = w.subwords.flatMap(s => s.owners).filter(o => ids.has(o.id));
    if (kind==='letters' && matched.length) {
      const chars=Array.from(w.text), first=matched[0], last=matched.at(-1);
      const start=first.word_offset, end=last.word_offset+Array.from(last.text).length;
      title.append(chars.slice(0,start).join(''),el('mark',chars.slice(start,end).join('')),chars.slice(end).join(''));
    } else title.textContent=w.text;
    const legend = el('p', marks ? inkLegend : 'Marks and dots are hidden.', 'legend');
    legend.id = 'inkLegend';
    box.replaceChildren(title, kv('word',wid), icon, description, legend);
    const details = el('details');
    details.append(el('summary','Inspect subwords, letters and marks'));
    box.append(details);
    let initial = null;
    for (const sub of w.subwords) {
      const section = el('section', null, 'sub-detail'),
        title = el('h3');
      const subInfo = sub.id + ' · ' + labels[sub.coverage];
      title.append(inkButton(sub.text, 'subwords', sub.id, subInfo), el('span', sub.id, 'address'));
      section.append(title);
      section.append(
        el(
          'p',
          (sub.letter_count ? sub.letter_count + ' letters · ' : '') +
            sub.mask_count +
            ' assigned masks · ' +
            labels[sub.coverage] +
            (sub.producer ? ' · ' + sub.producer : ''),
          'caption'
        )
      );
      if (sub.body_key) {
        const source = el('a', 'Native body mask & coordinates', 'quiet');
        source.href = '/api/body-source?' + new URLSearchParams({ id: sub.id });
        source.target = '_blank';
        source.rel = 'noopener';
        section.append(source);
      }
      if (sub.id === focus) initial = ['subwords', focus, subInfo];
      for (const o of sub.owners) {
        const row = el('div', null, 'owner-row'),
          ownerInfo = o.id + ' · ' + o.text + ' · ' + o.form + ' · ' + labels[sub.coverage];
        if (o.role === 'letter') row.append(inkButton(o.text, 'letters', o.id, ownerInfo));
        else row.append(el('span', o.text, 'arabic'));
        row.append(el('span', (o.role === 'letter' ? o.form : 'word sign') + ' · ' + o.id, 'address'));
        if (o.id === focus) initial = ['letters', o.id, ownerInfo];
        for (const m of o.marks) {
          const target = m.character_id || 'Unlinked';
          const info = [
            m.id,
            'transcript ' + (m.source_character || '') + ' ' + (m.codepoint || '') + ' at ' + target,
            'owner ' + o.id,
            m.attached_letter_id && 'attached letter ' + m.attached_letter_id,
            'expected ' + (m.expected_type || '—'),
            'classifier ' + (m.classifier_type || '—'),
            m.score == null ? null : 'confidence ' + (100 * m.score).toFixed(2) + '%',
            m.raw_type && m.raw_type !== m.classifier_type ? 'raw type ' + m.raw_type : null,
            labels[m.verdict] || m.verdict,
            m.method,
            m.path_ids.join(', '),
          ]
            .filter(Boolean)
            .join(' · ');
          const item = el('div', null, 'caption');
          item.append(
            inkButton(m.expected_type || 'Unlinked', 'marks', m.id, info),
            el('span', (m.source_character || '') + ' ' + (m.codepoint || '') + ' · ')
          );
          if (m.character_id) {
            const link = el('a', target);
            link.href = '/?' + new URLSearchParams({ kind: 'marks', character: target });
            item.append(link);
          } else item.append(el('span', target));
          if (m.attached_letter_id) {
            const letter = el('a', m.attached_letter_id);
            letter.href = '/?' + new URLSearchParams({kind:'letters',q:m.attached_letter_id});
            item.append(' · attached letter ',letter);
          }
          item.append(
            el(
              'span',
              ' · ' + (m.classifier_type || 'no ink') + ' · ' + (labels[m.verdict] || m.verdict)
            )
          );
          row.append(item);
          if (m.id === focus) initial = ['marks', m.id, info];
        }
        if (o.metrics) {
          row.append(
            el(
              'p',
              Object.entries(o.metrics)
                .filter(([key]) => key !== 'advance' && key !== 'spine')
                .map(([key, value]) => key + ' ' + value)
                .join(' · '),
              'caption'
            )
          );
        }
        for (const part of o.parts) {
          const id = o.id + '/' + part.role,
            info = id + ' · ' + part.area + ' px · ' + (part.variant || sub.producer);
          row.append(inkButton(part.role, 'parts', id, info));
          if (id === focus) initial = ['parts', id, info];
        }
        section.append(row);
      }
      details.append(section);
    }
    if (focus?.includes(',') && kind === 'letters')
      initial = ['letters', focus, 'Letter sequence · ' + focus];
    if (initial) selectInk(...initial);
  } catch (err) {
    if (request === inspectRevision) $('inspection').replaceChildren(el('p', err.message, 'empty'));
  }
}
