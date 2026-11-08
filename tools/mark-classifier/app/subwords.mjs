// Subword instances are independent of character identities and ink roles.
export function subwordApprovalIssue(doc,id){
  const sub=doc.subwords?.[id];
  if(!sub?.text?.trim())return 'Enter the subword transcript first.';
  const body=doc.parts.filter(p=>p.subwordId===id&&p.subwordBody);
  if(!body.length)return 'Assign the subword’s body ink before approving its boundary.';
  if(body.some(p=>p.reviewed&&['dot','mark','symbol','ornament','other'].includes(p.role)))return 'Reopen any approved mark/sign labels before assigning that ink to a subword body.';
  return null;
}
export function assignSubword(doc,parts,{id,text,body}){
  if(!parts.length)throw new Error('Select ink regions first.');
  if(id&&(!text.trim()||text.length>256))throw new Error('Enter a subword transcript of 1–256 characters.');
  if(id&&body&&parts.some(p=>p.reviewed&&['dot','mark','symbol','ornament','other'].includes(p.role)))throw new Error('Reopen approved mark/sign ink before assigning it as body.');
  doc.subwords ||= {};
  for(const p of parts){const previous=doc.subwords[p.subwordId];if(previous&&p.subwordBody){previous.reviewed=false;previous.decision='pending';}}
  if(id){
    const current=doc.subwords[id];
    const unchangedBody=!body&&current?.text===text.trim();
    doc.subwords[id]={...current,text:text.trim(),reviewed:unchangedBody?!!current.reviewed:false,
      decision:unchangedBody?(current.decision||'pending'):'pending'};
  }
  for(const p of parts){p.subwordId=id||null;p.subwordBody=!!id&&body;p.subwordAttachmentReviewed=!!id&&!body;}
}
