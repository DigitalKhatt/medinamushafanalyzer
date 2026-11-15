# Native letter cutter

The application, catalog exporter and review tools use `PageAnalysisResult::segmentSubword` in [segmentsubword.cpp](../segmentsubword.cpp). A single-letter outline passes through intact. The cutter reports a connected outline as unresolved when it cannot assemble a valid partition.

| Area | Source |
| --- | --- |
| Template schema and inheritance | [configuration.cpp](forms/configuration.cpp) |
| Body recognition and candidate sections | [anchor_candidates.cpp](forms/anchor_candidates.cpp) and the recognizers in [forms](forms) |
| Candidate order, backtracking and partition assembly | [anchor_search.cpp](engine/anchor_search.cpp) |
| Declared ligatures and ink checks | [partition_assembly.cpp](engine/partition_assembly.cpp) |
| Detached arms and pen lifts | [component_assembly.cpp](engine/component_assembly.cpp) |
| Contour landmarks, intersections and partitioning | [geometry](geometry) |

## From letter bodies to cuts

[letter-forms.json](../templates/letter-forms.json) groups the forms by `initial`, `medial` and `final` position. Each form declares `body` evidence and `cuts` policies. The search looks up templates by letter and position in the transcript. Ligature selectors restrict eligibility at the relevant join.

1. Recognize each candidate from the original outline. Record its entry and exit sections, the landmarks it must retain, and any counters the body must own. An initial or final form has one free end.
2. Follow the transcript in reading order. Check a candidate's entry against the previous candidate's exit. Backtracking chooses another body candidate and reuses candidates and compatible joins already calculated for that outline.
3. Use separate sections when the gap contains a complete connecting stroke. A shared junction uses one seam. If a template declares an inward limit, assembly may resolve overlapping preferred sections within that limit while retaining both bodies' landmarks.
4. Partition the source ink and check the complete result. The catalog exporter also checks that every letter owns source pixels. A rejected result returns control to the same anchor search.

Forms enter the primary anchor search by default. A form's `anchorPass` can delay its eligibility to `expanded` or `continuation`, or reserve it for a ligature or component recipe with `assembly`. Per-form `anchorOverrides` hold changes for later passes. The bracketed intersection solver can verify tight sections against the original curves. `fixedAnchorExpansionChecks` limits the number of candidates visited by the expanded search. `strokeWidth` is the global width in page units for policies measured in pen widths. `kashidaDefaults.sharedCutTolerance` controls how close two sections can be when sharing a cut.

Within each pass, lower `priorityWithinPass` values are tried first. The default is `0`, and ties follow template declaration order. Continuation can move a declared initial attachment to the front when its ligature context matches.

## Shape and join policies

- Medial teeth recognize an outer tip, its opposing bend and their flanks. The cut policies place sections on those flanks. Some tooth forms use a stem-foot turn at the entry. `minimumEntryWalk` bounds the inward motion, and `minimumEntryAfterForm` can restrict it to a particular preceding shape.
- Sīn and shīn retain three tips and two valleys. Medial forms use the first tooth junction and the last hollow. Final forms use the first tooth junction as their fixed entry and keep the terminal bowl on a continuous contour.
- Medial lām follows the shaft edges to its fork and places the cuts along its flanks. The two normals can start at a shared tip for a tight join. Lām–alef retains the lower return with lām and can use the inward corners of the alef entry for a shared seam.
- A suspended counter or stacked body can declare an attachment section in `cuts.sharedExitAtAttachment` or `cuts.sharedEntryAtAttachment`. Both shapes retain their landmarks at the shared section.
- The `ligatures` array declares two-letter matches and names the two body templates. The receiving form supplies the seam. A ligature can declare a minimum connector span, in global stroke widths, for using two independent sections.
- A medial form can declare `terminalArmRecovery` with a final `prefixForm` and a `detached` profile for the arm. `prefixCuts` can override the prefix template's boundary policy for that component. A pen lift can supply an attachment between separately matched contours.
- An explicit tatweel must retain a separate stroke for the catalog to assign to that character.

The segmentation dialog shows the matched body, selected anchors and cut policies. It can preview a draft before it becomes the active template file. The diagnostics record body templates and any template that supplies a shared boundary.

## Verification

Run the cutter over all indexed pages and compare the paths, connector caps, raster ownership and template labels with a frozen copy of the local catalog.

```sh
python3 tools/mushafcatalog/catalog.py check-letter-corpus --report output/anchor-audit
```

The report directory contains the visual comparison, `unresolved.csv`, `cutter.log` and elapsed times. `--form seen-fina` checks connected subwords ending in س or ش. For native tests and other catalog commands, see [tools/README.md](../tools/README.md).

## Future improvements

Make each body template return its own entry and exit anchors and the contour sections it must own. The previous exit can narrow the compatible entries before assembly. Candidate boundaries stay fixed during backtracking. The preferred cuts can move only within the declared limits of the recognized body.

Normal segmentation stops at the first valid partition. An inspection mode could also collect additional valid partitions and show which body candidates and cut policies distinguish them. Some recognizers would need to expose every intrinsic candidate before this mode could compare all matches.
