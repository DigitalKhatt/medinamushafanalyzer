CREATE INDEX IF NOT EXISTS catalog_kashida_left_order ON catalog_measurements(kashida_left,owner_id)
    WHERE kashida_left IS NOT NULL;
CREATE INDEX IF NOT EXISTS catalog_kashida_right_order ON catalog_measurements(kashida_right,owner_id)
    WHERE kashida_right IS NOT NULL;
CREATE INDEX IF NOT EXISTS catalog_body_dimensions ON catalog_parts(owner_id,w,h) WHERE role='body';
