# Native letter cutter

The application, native exporters, and geometry tests link the same cutter
objects. [detail.h](detail.h) declares types and functions shared inside the
cutter. The application calls the cutter through `PageAnalysisResult`.

| Area | Where to start |
| --- | --- |
| Segmentation and retries | [segmentsubword.cpp](../segmentsubword.cpp) selects the pass and handles retries. [search_pass.cpp](engine/search_pass.cpp) chooses candidates in transcript order, checks ownership and join compatibility, and records the result. |
| Template dispatch | [dispatch.cpp](forms/dispatch.cpp) gathers proposals for a slot and connects declared shapes to their recognizers and cut policies. |
| Template families | [Stems and routes](forms/stems_and_routes.cpp), [stem boundaries](forms/stem_boundaries.cpp), [teeth](forms/teeth.cpp), [counters and bowls](forms/counters_and_bowls.cpp), and [heads and ligatures](forms/heads_and_ligatures.cpp) provide reusable geometry. |
| Contour operations | [geometry](geometry) contains intersections, contour walks and projections, stroke sections, join order, and path partitioning. |
| Final assembly | [body_cut_plan.cpp](engine/body_cut_plan.cpp) gathers evidence and reads attachment policies. [attachment_assembly.cpp](engine/attachment_assembly.cpp) applies fork and shared-return policies. [partition_assembly.cpp](engine/partition_assembly.cpp) builds the letter and connector paths and checks ink coverage. |
| Component assembly | [component_assembly.cpp](engine/component_assembly.cpp) fits folded arms, split entries and pen lifts using the source contours. |

Template settings live in [letter-forms.json](../templates/letter-forms.json).
Recognizers use the outline and `body` settings to find evidence. Cut functions
use `cuts` settings to propose boundaries. The search pass decides which
proposals fit together. Each template declares the boundary at either end
of its body. A shared join identifies which template supplies the seam, and
the diagnostics record that template on both letters. The final form's
body match is recorded separately from the source of its entry. In lām–alef,
`cuts.beforeFinal` names the terminal template that supplies the seam at
the inward corners. Lām keeps its lower return.

`lam-turning-flanks-medi` follows both contour edges from the same shaft cap.
`body.shaft.turnAngle` locates the first turn on each edge. The width comes
from a section between those edges. `cuts.shaft.walkWidths` measures a walk
from each turn in local stroke widths. `atTurnWhenTight` also allows normals
at the turns. A fork belonging to the same shaft can provide the half-unit
walks or the shared-tip normals, and `forkJunction` allows a chord between
the incoming turn and that fork. Assembly checks the order of the cuts and
retains the recognized landmarks. The lām–alef return has its own policy.

A medial tooth's `body` finds its tip, inner bend, flanks and local stroke
width on the uncut outline. The shared recognition cache keeps that evidence
available when a `cuts` setting changes. Each form projects both boundaries
before placement. At a tight join the assembler can share an existing seam
if both bodies retain their landmarks. The inspection shows which template
supplied the shared cut. Final forms that declare `cuts.precedingTooth`
can offer their own entry while placing the tooth.

A tooth-cluster body identifies three tips and two valleys on the uncut
outline. Final clusters also need to retain the bowl. A pen lift inside
the word can end the medial cluster on its own contour. The template
projects its entry at the opposing bends of the first tooth when
`cuts.cluster.entryAtJunction` is set. With `cuts.cluster.exitAtHollow`,
it proposes normals at the lower bend beyond the last tooth. Assembly
checks those sections against the receiving width and the lower bulge
of the tooth. It keeps the other cap of an existing connector and checks
landmark ownership before accepting the partition. An initial cluster
can open a connector when it follows one stroke and meets
`cuts.minConnectorWidths`. An exit placed at the declared contour
offset keeps that offset.

The `initial`, `medial` and `final` arrays set template order within each
priority. One search tries ordinary placements and eligible `onIncomplete`
forms before widening the search to include `onUnresolved` forms. These
attempts share the source outline and recognized bodies. The wider search
checks body proportions and ownership before accepting a partition. If it
fails, assembly can share the existing seam of a matched initial form that
declares `tightJoin` and `ligatureWith` when a connector wraps around the
leading stroke. The shared cut must preserve the bodies and cover the source
ink. Assembly checks any still-unmatched counter outlines against the source
counters and the caps of the proposed partition. A final template with
`cuts.shareEntryWhenUnresolved` can supply the shared seam at its entry.
Assembly rebuilds the affected bodies and checks their counter outlines with
the same rules, using `requiresExitTemplate` for a rim that needs that
particular seam. A folded terminal form can set `cuts.return.maxClipWidths`
to limit how much of its recognized bowl another cut may clip. The cutter
checks the free tip and shoulder before accepting the partition. The search
uses the original contour in page coordinates. It can also split at a template's
declared entry and fit the resulting component with the usual body rules.
Medial tooth alternatives become eligible in
`recoveryStage` order during placement backtracking.
An `onUnresolvedPartition` form becomes eligible after the existing search
fails to assemble the subword. This attempt uses bracketed intersections on
the original curves, recovers tangents at repeated control points, and applies
the same ink ownership checks.
An initial form with `sharedFoldWithFinal` takes its turn in the initial array
and uses the first matching final boundary for the declared adjacent letters.
`sharedFoldForm` can name the specific final template supplying the cut.

`cuts.turnBoundaries` can place a boundary at the turn on the same contour
flank. Set `left` or `right` to `trim` to move a cut off a long connector
and back toward the body. The incoming policies `descender` and `stem`
follow a clipped stroke to its turn. `stem-foot` trims a long horizontal
run from a final stem. `cuts.skipEntryTurnAfter` lists preceding letters
whose attachment calls for retaining the original entry. Assembly checks the other cap and keeps the
recognized tooth tips and the ink partition intact.

For an initial tooth, `cuts.flankExitAngle` sets the angle in degrees
where its descending flank becomes a connector. The template proposes the
normal from its own contour route. Assembly can use it when it preserves
the receiving body and the intervening ink forms a distinct stroke.
An initial route with `body.route.start: "lower-right"` starts at the
rightmost outward turn in the lower half of the outline when another
stroke extends farther right above it. The usual route and body checks
still apply.

Final mīm and other counter forms project their entry with the same corner
and counter-relative probe used by medial loops. A terminal neck keeps
the source counter and the free end. A form with `cuts.entryRim` can also
offer a section at the turn where the lower rim meets the incoming stroke.
`minRiseWidths` measures the proposed entry's vertical span in global stroke
widths. `maxSlope` limits its horizontal run divided by its rise. Assembly
checks that both adjacent bodies retain their source counters.

Open heads use sections under their own two lips. When an intersection is
missing, the section probe brackets the crossing on the same Bézier curve.
If a preceding cut coincides with the exit of an independently recognized
open head, the cutter can reconsider that placement using forms marked
`preferAtHeadOverlap`. The new partition must pass the body and ink checks.
The signed area of the original curves determines contour winding. Final
bowl forms follow the outside edge from the free tip to find the entry
junction, including when both nearby landmarks are higher than the tip.
With `cuts.loop.exitTooth.returningFoot`, a loop whose attached tooth returns
below the connector can cut at the turn on the lower edge if the usual tooth
cut is unavailable. The section must keep the bend and the return.
In counter ligatures, the cutter can recover the exact floor vertex from
the source Bézier when the extrema list omits it. It checks candidate
shoulders in order against both cuts of the body.
For horizontal counter pairs, `cuts.loop.floorExit` can find the outgoing
normal below both counters when the ordinary rim projection fails. The
new body must keep both source contours. Its offset is in counter heights
and its maximum exit width is relative to the entry width.

An initial contour normal can use `snapOppositeToOrigin` to end on the
outward turn when it would otherwise put that turn in the connector. The
value bounds the shift as a fraction of the cut width. Medial lām uses
the shaft's own traced fork when an intervening crest yields no valid
route. When ordinary assembly fails, the cutter can try a declared
terminal attachment alongside the final letter's usual boundaries.

For a medial lām exit that overlaps a terminal entry, `cuts.overlappingExit`
can name a final template to try at its own counter shoulder. The shift
limit is relative to the current entry's width. The minimum separation is
relative to `forkOffset`. Assembly keeps lām's fork anchors fixed and
checks the replacement against the final body's counter.

A layered head such as `hah-init` can recognize an exit corner on its lower
lip with `body.head.exitRim`. The distance is relative to the head's counter
height and the angle measures the inward turn. `cuts.head.rimWalkWidths`
lists normal cuts at fixed contour distances from that corner in global
stroke widths. When the head and lām's entry overlap, assembly checks these
exits in order and retains the head's counter and lām's fixed fork cuts.
For the standard layered-head exit, recognition takes the rightmost enclosed
counter and verifies that its upper rim rises at `body.head.sectionInset`
page units from the right edge of the source. The cut projects once at
`cuts.head.counterOffset` page units left of the counter. Both endpoints must
lie on the outer contour.

`cuts.sharedEntryCorner` can name an initial contour template for a tight
medial join. If the proposed shared cut clips the upper rim by more than
`maxClipWidths` times the corner cut's length, assembly uses the initial
template's cut at the corner. The cut must retain the same counter and the
medial body's crest and fork.

Ordinary medial teeth use local stroke offsets around their tip and inner
bend. The lower flank can supply the stroke width for those offsets.
`followedByFinal` limits the raised-entry cut policies to a final nūn, rāʾ
or zāy. The upright tooth has its own measured tip-to-bend range and turn
policy.
On a three-tooth body, `allowUnshiftedEntry` uses the facing shoulder and
inner bend when the shifted entry cannot produce a valid section.

`ligatures` takes priority for two-letter subwords without an explicit tatweel.
Each entry selects the letters and names the initial and final templates.
Optional `body` and `cuts` settings apply only in that ligature. The final
template supplies the shared cut, and both bodies must pass ownership checks.
If the first eligible entry fails, the subword stays unresolved.
With `minConnectorWidths`, the cutter measures the horizontal distance
from the recognized initial exit to the final entry in global stroke widths.
At or above that minimum, the ordinary cutter places the separate connector
and keeps it through final assembly.

A medial form can declare `terminalArmRecovery` with a final `prefixForm`
for its own letters and an `attached` or `detached` mode. Each mode names a
final arm profile and a catalog variant label. An attached mode uses a tapered
arm profile, and a detached mode uses a component arm profile. The prefix is
segmented with the usual matching priority. `prefixCuts` overrides cut
settings for the named prefix form if it matches, and `maxTipGap` controls how
far the arm tip may sit from the prefix body. The recipe's `cuts` override the
medial cut policy after recovery. Medial lām uses both arm modes.
`cuts.detachedEntry` can select a separate source contour when a connected
final form captured only a small piece of its stroke. The named component
template defines the stroke and supplies its original contour.

Use the [native tests and catalog comparison](../tools/README.md#letter-audit)
to check a change. Audit and training snapshots fingerprint the cutter’s
sources and headers.

## Fixed anchor search

Set `fixedAnchors: true` on a migrated template. The search uses the transcript
to look up enabled templates by letter in the `initial`, `medial` and `final`
lists. Their order in each list sets priority. A subword can use the search
when every letter has an eligible form. Each form proposes sections on the
uncut outline and identifies the landmarks and ink it must retain. Counter
forms identify the source contours their body must own.
Medial heh uses its own stacked counters and outer feet to place both anchors.
A counter form can locate the entry at the rounded foot using `cuts.entryRim`
before assembly.
Medial lām traces both sides of its shaft to the fork and projects the
two configured contour walks. When there is no room, both normals can
start at the fork tip. The declared folded return can share final yeh's
entry if the seam retains the lām shaft.
During migration an explicitly disabled fixed-anchor form can reserve its
own contour for the existing cutter. For example, the hanging mīm's upper
entry and exit corners identify a stacked attachment before an ordinary
mīm can claim that counter.
[The search](engine/anchor_search.cpp) builds each form's candidates once per
outline and backtracks over compatible bodies. Their entry and exit sections
stay fixed. Coincident sections share a seam. A template with
`cuts.tightJoin: "shared-boundary"` can use the sending section when the two
proposed sections overlap and the shared cut retains the receiving body's
landmarks and ink. A separate connector must enclose a single stroke. Catalog
export also checks that every letter owns source pixels. A rejected partition
returns control to the same search with the original anchor candidates. If no
complete partition fits, the existing cutter handles the subword.

Use the following command from the project root to compare each fixed anchor
partition with the original cutter and check the exported geometry against
the catalog. The report counts matching results, differences and fallbacks.

```sh
python3 tools/mushafcatalog/catalog.py check-letter-corpus --compare-anchors --report output/anchor-audit
```

## Future improvements

Use one backtracking search to match letter bodies in reading order, then
place the cuts. Each template would find candidates in the uncut ink using
its shape and stroke measurements. A candidate would return its right entry,
left exit, and the landmarks and contour sections it must own. An anchor
identifies a position on a source contour. An initial or final form has one
free end, and a pen lift can supply a boundary between separate contours.

1. Extend the fixed-anchor search to more forms. Use the previous exit to
   find compatible entries and reject a candidate that claims ink already
   owned by another body. Add support for shared ligature seams and pen lifts
   as their recognizers supply the required contour evidence.
2. For a complete sequence, try the preferred cuts within the space allowed
   by the bodies. Fall back to the anchor cuts if needed. Backtrack if the
   cuts cannot preserve the required landmarks and partition the source ink.
3. Stop at the first valid result for normal segmentation, or keep collecting
   valid results for inspection.

The existing `cuts.boundaries` declarations describe how to find the anchors.
All initial and final forms declare their joining side, and most medial forms
declare both. The two exposed-head recipes combine existing forms. The
current result stores the anchors chosen for connected joins. Before the
search can use all candidates, some matchers need to expose the alternatives
they currently select internally. For example, a medial loop matcher can
return each eligible loop and let the search check it against the preceding
exit.

Migrate more groups using the same comparison across all 604 pages. Start
with forms that can already supply their own sections. Extend the search to
shared ligature seams and separate ink components as their body recognizers
supply that evidence. Remove a retry path when the new search handles its
cases and the catalog comparison confirms the expected results.
