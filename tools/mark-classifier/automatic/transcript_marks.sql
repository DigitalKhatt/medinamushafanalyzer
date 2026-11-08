CREATE TABLE IF NOT EXISTS native_mark_pages (
    page_id TEXT PRIMARY KEY REFERENCES pages(page_id),
    report_sha TEXT NOT NULL, assignment_sha TEXT NOT NULL, provenance TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS native_mark_words (
    word_id TEXT PRIMARY KEY REFERENCES words(id),
    page_id TEXT NOT NULL REFERENCES native_mark_pages(page_id) ON DELETE CASCADE,
    x REAL NOT NULL, y REAL NOT NULL, w REAL NOT NULL, h REAL NOT NULL, runs BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS native_mark_words_page ON native_mark_words(page_id);
CREATE TABLE IF NOT EXISTS native_marks (
    seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE,
    page_id TEXT NOT NULL REFERENCES native_mark_pages(page_id) ON DELETE CASCADE,
    word_id TEXT NOT NULL REFERENCES native_mark_words(word_id) ON DELETE CASCADE, subword_id TEXT NOT NULL,
    owner_id TEXT NOT NULL, base_character TEXT, base_offset INTEGER,
    character_id TEXT UNIQUE, word_offset INTEGER, source_character TEXT, codepoint TEXT,
    expected_type TEXT, equivalence TEXT, method TEXT, candidates TEXT,
    verdict TEXT NOT NULL CHECK(verdict IN ('match','different','accepted-form','missing','ambiguous','unmatched')),
    classifier_type TEXT, raw_type TEXT, classifier_kind TEXT, score REAL, margin REAL,
    path_ids TEXT, x INTEGER, y INTEGER, w INTEGER, h INTEGER, runs BLOB,
    attached_letter_id TEXT,
    CHECK ((verdict='missing') = (runs IS NULL))
);
CREATE INDEX IF NOT EXISTS native_marks_page ON native_marks(page_id,seq);
CREATE INDEX IF NOT EXISTS native_marks_word ON native_marks(word_id,seq);
CREATE INDEX IF NOT EXISTS native_marks_subword ON native_marks(subword_id,seq);
CREATE INDEX IF NOT EXISTS native_marks_expected ON native_marks(expected_type,seq);
CREATE INDEX IF NOT EXISTS native_marks_predicted ON native_marks(classifier_type,seq);
CREATE INDEX IF NOT EXISTS native_marks_base ON native_marks(base_character,seq);
CREATE INDEX IF NOT EXISTS native_marks_character ON native_marks(codepoint,seq);
CREATE INDEX IF NOT EXISTS native_marks_verdict ON native_marks(verdict,seq);
