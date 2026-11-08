PRAGMA journal_mode=WAL;
CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE pages(page_id TEXT PRIMARY KEY,split TEXT NOT NULL,source_sha TEXT NOT NULL,pdf_sha TEXT,
 width INTEGER,height INTEGER,ink_pixels INTEGER,matched_pixels INTEGER,unknown_pixels INTEGER,component_count INTEGER,
 source_kind TEXT,proposal_sha TEXT,annotation_revision INTEGER,scope_ink_pixels INTEGER,scope_unknown_pixels INTEGER);
CREATE TABLE words(id TEXT PRIMARY KEY,page_id TEXT,line INTEGER,ordinal INTEGER,text TEXT,bases TEXT,
 x INTEGER,y INTEGER,w INTEGER,h INTEGER,flags INTEGER,transcript_matches INTEGER,in_scope INTEGER NOT NULL DEFAULT 1);
CREATE TABLE subwords(id TEXT PRIMARY KEY,page_id TEXT,word_id TEXT,line INTEGER,text TEXT,bases TEXT,
 x INTEGER,y INTEGER,w INTEGER,h INTEGER,expected TEXT,issues TEXT,in_scope INTEGER NOT NULL DEFAULT 1);
CREATE TABLE items(id TEXT PRIMARY KEY,page_id TEXT,word_id TEXT,subword_id TEXT,line INTEGER,
 kind TEXT,mark_type TEXT,text TEXT,bases TEXT,x INTEGER,y INTEGER,w INTEGER,h INTEGER,area INTEGER,
 components INTEGER,contours INTEGER,runs BLOB NOT NULL,feature_row INTEGER UNIQUE,review_candidate TEXT,
 quality REAL,origin TEXT,human_reviewed INTEGER,issues TEXT,teacher_kind TEXT,teacher_type TEXT,
 proposal_origin TEXT,review_attribution TEXT,
 prediction_kind TEXT,prediction_type TEXT,model_confidence REAL,model_margin REAL,model_agrees INTEGER,
 final_kind TEXT,final_type TEXT,refined INTEGER DEFAULT 0,model_round INTEGER,inventory_before INTEGER,inventory_after INTEGER,
 unresolved_reason TEXT,resolution_method TEXT,resolution_score REAL,assignment_status TEXT,source_diagnostics TEXT,context_bbox TEXT,context_text TEXT,in_scope INTEGER NOT NULL DEFAULT 1,
 raw_prediction_kind TEXT,raw_prediction_type TEXT,raw_model_confidence REAL,raw_model_margin REAL,prediction_guard TEXT,prediction_structure TEXT,
 role_scores BLOB,type_scores BLOB);
CREATE INDEX item_class ON items(final_kind,final_type);
CREATE INDEX item_final_browse ON items(in_scope,final_kind,final_type,w,id);
CREATE INDEX item_teacher_browse ON items(in_scope,teacher_kind,teacher_type,w,id);
CREATE INDEX item_model_browse ON items(in_scope,prediction_kind,prediction_type,w,id);
CREATE INDEX item_page ON items(page_id,line);
CREATE INDEX item_word ON items(word_id);
CREATE INDEX item_subword ON items(subword_id);
CREATE INDEX item_quality ON items(quality,model_confidence);
CREATE INDEX item_size ON items(w,h,area);
CREATE INDEX item_width_browse ON items(in_scope,w DESC,id,h,area,components,quality,model_confidence);
CREATE INDEX item_final_supported_browse ON items(in_scope,final_kind,final_type,w,id,h,area,components,quality,model_confidence)
 WHERE ((human_reviewed=1 AND origin='human-reviewed') OR (model_confidence>=0.90 AND prediction_guard IS NULL));
CREATE INDEX item_model_supported_browse ON items(in_scope,prediction_kind,prediction_type,w,id,h,area,components,quality,model_confidence)
 WHERE (model_confidence>=0.90 AND prediction_guard IS NULL);
CREATE INDEX item_type_model ON items(prediction_type,model_confidence);
CREATE INDEX item_attention_score ON items(in_scope,model_confidence,id)
 WHERE model_confidence<0.90 OR prediction_guard IS NOT NULL OR final_kind='unknown';
CREATE INDEX item_label_summary ON items(in_scope,teacher_kind,teacher_type,prediction_kind,prediction_type,final_kind,final_type,model_agrees,refined,subword_id);
CREATE INDEX subword_bases ON subwords(bases);
