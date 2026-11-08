CREATE TABLE IF NOT EXISTS catalog_page_art (
    page_id TEXT PRIMARY KEY REFERENCES pages(page_id) ON DELETE CASCADE,
    source_sha TEXT NOT NULL, report_sha TEXT NOT NULL, vectors BLOB NOT NULL
);
