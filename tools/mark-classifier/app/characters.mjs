// Independent instance attributes: never fold character/position/form into role IDs.
export const POSITIONS = ['unknown', 'initial', 'medial', 'final', 'isolated'];
export const LETTER_ROLES = new Set(['body', 'kashida', 'dot', 'mark']);
export const CHARACTER_REQUIRED_ROLES = new Set(['body', 'kashida']);
export const MARK_ROLES = new Set(['dot', 'mark']);
export const CHARACTERS = [
  ['ء','Hamza'], ['ا','Alef'], ['أ','Alef with hamza above'], ['إ','Alef with hamza below'],
  ['آ','Alef with madda'], ['ٱ','Alef wasla'], ['ب','Beh'], ['ت','Teh'], ['ث','Theh'],
  ['ج','Jeem'], ['ح','Hah'], ['خ','Khah'], ['د','Dal'], ['ذ','Thal'], ['ر','Reh'],
  ['ز','Zain'], ['س','Seen'], ['ش','Sheen'], ['ص','Sad'], ['ض','Dad'], ['ط','Tah'],
  ['ظ','Zah'], ['ع','Ain'], ['غ','Ghain'], ['ف','Feh'], ['ق','Qaf'], ['ك','Kaf'],
  ['ل','Lam'], ['م','Meem'], ['ن','Noon'], ['ه','Heh'], ['ة','Teh marbuta'],
  ['و','Waw'], ['ؤ','Waw with hamza'], ['ي','Yeh'], ['ئ','Yeh with hamza'], ['ى','Alef maksura'],
];

export function characterMetadata(group = {}) {
  return {label: group.label || '', position: group.position || 'unknown',
    form: group.form || 'unknown', reviewed: group.reviewed === true};
}

export function isSingleCharacter(label) {
  const normalized = (label || '').trim().normalize('NFKC');
  return [...normalized].length === 1 && /^\p{Letter}$/u.test(normalized);
}

export function completeCharacter(group) {
  const metadata = characterMetadata(group);
  return isSingleCharacter(metadata.label) && POSITIONS.includes(metadata.position) && metadata.position !== 'unknown';
}

export function partApprovalIssue(part, groups) {
  if (part.role === 'unknown') return 'Assign an ink role first.';
  if (CHARACTER_REQUIRED_ROLES.has(part.role) && (!part.owner || !completeCharacter(groups[part.owner])))
    return 'Associate letter ink with one character and its joining position before approving.';
  return null;
}

export function formPresets(character) {
  const forms = [{id:'unknown', label:'Not specified'}, {id:'standard', label:'Standard'}];
  if (character.trim().normalize('NFKC') === 'ك') forms.push({id:'zigzag', label:'Zigzag kaf'});
  if (character.trim().normalize('NFKC') === 'ن') forms.push({id:'expanded', label:'Expanded noon'});
  return forms;
}

export function applyCharacterMetadata(doc, owner, values) {
  doc.groups[owner] = {...doc.groups[owner], ...values, reviewed:false};
  // A changed identity, position or form affects every cut belonging to this instance.
  for (const part of doc.parts) if (part.owner === owner) part.reviewed = false;
}

export function upgradeCharacters(doc) {
  const legacy = doc.characterMetadataVersion !== 1;
  for (const [id, group] of Object.entries(doc.groups)) doc.groups[id] = {...group, ...characterMetadata(group)};
  let reopened = 0;
  for (const part of doc.parts) {
    if (part.reviewed && CHARACTER_REQUIRED_ROLES.has(part.role) &&
      (partApprovalIssue(part, doc.groups) || doc.groups[part.owner]?.reviewed !== true)) {
      part.reviewed = false; reopened++;
    }
  }
  doc.characterMetadataVersion = 1;
  return {changed:legacy || reopened > 0, reopened};
}
