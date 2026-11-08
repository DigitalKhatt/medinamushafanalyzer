PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS pages(
 id TEXT PRIMARY KEY, split TEXT NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL,
 source_sha TEXT NOT NULL, provenance TEXT NOT NULL, fingerprint TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS words(
 id TEXT PRIMARY KEY, page_id TEXT NOT NULL REFERENCES pages(id) ON DELETE CASCADE,
 line INTEGER NOT NULL, ordinal INTEGER NOT NULL, text TEXT NOT NULL, bases TEXT NOT NULL,
 x INTEGER, y INTEGER, w INTEGER, h INTEGER);
CREATE TABLE IF NOT EXISTS subwords(
 id TEXT PRIMARY KEY, word_id TEXT NOT NULL REFERENCES words(id) ON DELETE CASCADE,
 page_id TEXT NOT NULL, ordinal INTEGER NOT NULL, text TEXT NOT NULL, bases TEXT NOT NULL,
 x INTEGER, y INTEGER, w INTEGER, h INTEGER,
 expected_letters INTEGER NOT NULL, model_bodies INTEGER NOT NULL,
 status TEXT NOT NULL, alignment TEXT NOT NULL, diagnostics TEXT NOT NULL);
-- Logical identities exist even if the model has not resolved their bodies.
CREATE TABLE IF NOT EXISTS letters(
 id TEXT PRIMARY KEY, subword_id TEXT NOT NULL REFERENCES subwords(id) ON DELETE CASCADE,
 word_id TEXT NOT NULL, page_id TEXT NOT NULL, ordinal INTEGER NOT NULL, word_ordinal INTEGER NOT NULL,
 char TEXT NOT NULL, form TEXT NOT NULL, text_offset INTEGER NOT NULL, shape_class TEXT NOT NULL,
 status TEXT NOT NULL, model_region_id TEXT,
 x INTEGER, y INTEGER, w INTEGER, h INTEGER, area INTEGER,
 raw_coverage REAL);
-- Geometry is stored separately. All runs use the original full-page raster.
-- Unknown source ink is a region too: it is never silently made background.
CREATE TABLE IF NOT EXISTS regions(
 id TEXT PRIMARY KEY, subword_id TEXT NOT NULL REFERENCES subwords(id) ON DELETE CASCADE,
 page_id TEXT NOT NULL,
 role TEXT NOT NULL CHECK(role IN ('body','kashida','unknown')),
 letter_id TEXT REFERENCES letters(id), x INTEGER, y INTEGER, w INTEGER, h INTEGER,
 area INTEGER NOT NULL, runs BLOB NOT NULL, raw_runs BLOB, raw_area INTEGER,
 mask_sha TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS source_items(
 id TEXT PRIMARY KEY, page_id TEXT NOT NULL REFERENCES pages(id) ON DELETE CASCADE,
 word_id TEXT, subword_id TEXT, kind TEXT NOT NULL, type TEXT, x INTEGER, y INTEGER,
 w INTEGER, h INTEGER, area INTEGER NOT NULL, runs BLOB NOT NULL, metadata TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS marks(
 id TEXT PRIMARY KEY REFERENCES source_items(id) ON DELETE CASCADE,
 page_id TEXT NOT NULL, word_id TEXT, subword_id TEXT, kind TEXT NOT NULL, type TEXT,
 predicted_letter_id TEXT REFERENCES letters(id), status TEXT NOT NULL,
 method TEXT NOT NULL, margin REAL, candidates TEXT NOT NULL, fingerprint TEXT NOT NULL,
 type_reviewed INTEGER NOT NULL DEFAULT 0, type_confidence REAL);
CREATE TABLE IF NOT EXISTS expected_marks(
 id TEXT PRIMARY KEY, subword_id TEXT NOT NULL REFERENCES subwords(id) ON DELETE CASCADE,
 letter_id TEXT REFERENCES letters(id), type TEXT NOT NULL, side TEXT, text_offset INTEGER,
 character TEXT, source_character TEXT, word_text_offset INTEGER, dot_count INTEGER,
 mark_id TEXT REFERENCES marks(id));
CREATE TABLE IF NOT EXISTS mark_transcripts(
 mark_id TEXT PRIMARY KEY REFERENCES marks(id) ON DELETE CASCADE,
 page_id TEXT NOT NULL, subword_id TEXT,
 slot_id TEXT REFERENCES expected_marks(id) ON DELETE CASCADE,
 expected_type TEXT, character TEXT, source_character TEXT, word_text_offset INTEGER, dot_count INTEGER,
 status TEXT NOT NULL, method TEXT NOT NULL, margin REAL, candidates TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS mark_validation(
 mark_id TEXT PRIMARY KEY REFERENCES marks(id) ON DELETE CASCADE,
 classifier_kind TEXT, classifier_type TEXT, classifier_confidence REAL,
 expected_type TEXT, verdict TEXT NOT NULL, provenance TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS letter_search ON letters(char,form,w);
CREATE INDEX IF NOT EXISTS letter_shape ON letters(shape_class,form,w);
CREATE INDEX IF NOT EXISTS letter_word ON letters(word_id,word_ordinal);
CREATE INDEX IF NOT EXISTS subword_page ON subwords(page_id,word_id);
CREATE INDEX IF NOT EXISTS region_subword ON regions(subword_id);
CREATE INDEX IF NOT EXISTS region_letter ON regions(letter_id);
CREATE INDEX IF NOT EXISTS mark_owner ON marks(predicted_letter_id);
CREATE INDEX IF NOT EXISTS mark_word ON marks(word_id);
CREATE INDEX IF NOT EXISTS mark_type ON marks(type,status);
CREATE INDEX IF NOT EXISTS source_word ON source_items(word_id);
