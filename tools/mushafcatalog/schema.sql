CREATE TABLE IF NOT EXISTS catalog_pages (
    page_id TEXT PRIMARY KEY REFERENCES pages(page_id), report_sha TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS catalog_coordinates (
    page_id TEXT PRIMARY KEY REFERENCES catalog_pages ON DELETE CASCADE, raster TEXT NOT NULL, pdf_to_raster TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS catalog_word_bounds (
    word_id TEXT PRIMARY KEY REFERENCES words(id), page_id TEXT NOT NULL REFERENCES catalog_pages ON DELETE CASCADE,
    x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS catalog_subwords (
    seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, page_id TEXT NOT NULL REFERENCES catalog_pages,
    word_id TEXT NOT NULL REFERENCES words, ordinal INTEGER NOT NULL, text TEXT NOT NULL, bases TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('text','sign','decoration')), members TEXT NOT NULL,
    body_path TEXT, baseline REAL, x INTEGER, y INTEGER, w INTEGER, h INTEGER, runs BLOB,
    body_key TEXT, letter_count INTEGER NOT NULL, mask_count INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS catalog_sub_page ON catalog_subwords(page_id,seq);
CREATE INDEX IF NOT EXISTS catalog_sub_word ON catalog_subwords(word_id,ordinal);
CREATE TABLE IF NOT EXISTS catalog_owners (
    seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, subword_id TEXT NOT NULL REFERENCES catalog_subwords(id) ON DELETE CASCADE,
    word_id TEXT NOT NULL REFERENCES words, role TEXT NOT NULL, word_offset INTEGER NOT NULL, character TEXT NOT NULL,
    text TEXT NOT NULL, form TEXT NOT NULL, shape_class TEXT, ordinal INTEGER, prev_letter TEXT, next_letter TEXT,
    UNIQUE(word_id,word_offset)
);
CREATE INDEX IF NOT EXISTS catalog_owner_sub ON catalog_owners(subword_id,seq);
CREATE INDEX IF NOT EXISTS catalog_owner_class ON catalog_owners(shape_class,form,seq);
CREATE INDEX IF NOT EXISTS catalog_owner_char ON catalog_owners(character,form,seq);
CREATE INDEX IF NOT EXISTS native_marks_owner ON native_marks(owner_id,seq);
CREATE TABLE IF NOT EXISTS catalog_splits (
    subword_id TEXT PRIMARY KEY REFERENCES catalog_subwords(id) ON DELETE CASCADE,
    source_key TEXT NOT NULL, producer TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS catalog_parts (
    owner_id TEXT NOT NULL REFERENCES catalog_owners(id) ON DELETE CASCADE,
    role TEXT NOT NULL CHECK(role IN ('body','connection','kashida')),
    subword_id TEXT NOT NULL REFERENCES catalog_splits(subword_id) ON DELETE CASCADE,
    next_owner_id TEXT REFERENCES catalog_owners(id), variant TEXT,
    x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL, area INTEGER NOT NULL, runs BLOB NOT NULL,
    PRIMARY KEY(owner_id,role)
);
CREATE INDEX IF NOT EXISTS catalog_parts_sub ON catalog_parts(subword_id);
CREATE TABLE IF NOT EXISTS catalog_measurements (
    owner_id TEXT PRIMARY KEY, role TEXT NOT NULL DEFAULT 'body' CHECK(role='body'),
    advance REAL, spine REAL, kashida_left REAL, kashida_right REAL, confidence REAL, quality TEXT,
    FOREIGN KEY(owner_id,role) REFERENCES catalog_parts(owner_id,role) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS catalog_words_page ON words(page_id,line,ordinal);
