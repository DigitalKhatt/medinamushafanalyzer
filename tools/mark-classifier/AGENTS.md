# Project boundaries

- The manual workbench works without a transcript. Read-only adapters may import
  proposals from other analyzers with source hashes and coordinate provenance.
  Keep generated proposals separate from human annotations. Never silently
  approve, overwrite human labels, or treat cached analysis as ground truth.
- The configured Mushaf SVG directory is a read-only reference. Preserve source
  hashes, viewBoxes and SVG-to-raster transforms. Never rewrite source artwork.
- Character ownership and ink role are separate annotations. Preserve body,
  connection/kashida, dot, mark, symbol and ornament as distinct classes.
- A connection at its natural width is still a connection. A widened body is
  still body. Do not infer extra-extension pixels from a single occurrence.
- Unknown boundaries remain explicit. Component proposals are not characters,
  and a semantic role model alone does not solve character instance segmentation.
- The user requested exclusion of the green SVG frame and all exterior text.
  Use its exact transformed inner contours. Exclude surah frames and their
  enclosed title text, including the separate title panels on pages 1 and 2;
  retain their Quran text openings. Keep excluded ink explicitly accounted for; preserve original source
  artwork, masks, hashes and human annotations. Scope is a view/export boundary,
  never a reason to silently delete annotation pixels.
- All editing operations must conserve the detected source ink, including tiny
  marks and counters. Do not reconstruct hidden strokes in segmentation masks.
- Only reviewed labels become training targets. Unknown and draft ink is 255
  (ignore); it must never silently become background. Predictions must be stored
  separately from reviewed human annotations when a model is introduced.
- The user explicitly authorized an autonomous weak-supervision experiment.
  Its automatic labels, training targets, models and metrics live separately in
  data/automatic. They must retain machine/weak provenance and never set human
  review flags. Existing reviewed-only exports keep their original contract.
  Teacher agreement and transcript consistency are proxy metrics, not measured
  segmentation accuracy. Select models with train/validation pages only; leave
  test pages out of fitting, pseudo-label selection and round selection.
- Retain page-level dataset splits; crops from one page cannot cross splits.
- Body/kashida approvals require a character instance, one base letter and a
  joining position (initial/medial/final/isolated). The dots/marks pilot allows
  dot and mark roles to be reviewed without a letter owner. Keep letter forms
  independent and extensible; unknown form is not standard. Changes to character
  attributes reopen all its associated cuts.
- Mark/dot type belongs to the ink region, independently of letter identity or
  position. Merge the regions of a multipart mark into one semantic region.
  Other ink (role 7) is a reviewed pilot negative, not pixel background. In
  marks.json unreviewed candidates have null targets; never infer negatives from
  unlabelled ink. No trained inference is provided by adding these labels.
- The workbench currently uses Python standard library and browser modules.
  Preserve its dependency-free startup unless a new dependency has a clear need.
- Run `node --test tests/*.test.mjs` and
  `python3 -m unittest discover -s tests -p 'test_*.py'` after mask/export changes.
