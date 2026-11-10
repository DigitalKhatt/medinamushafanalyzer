# Native letter cutter

The application, native exporters, and geometry tests link the same cutter
objects. [detail.h](detail.h) declares types and functions shared inside the
cutter. The application calls the cutter through `PageAnalysisResult`.

| Area | Where to start |
| --- | --- |
| Segmentation and retries | [segmentsubword.cpp](../segmentsubword.cpp) selects the pass and handles retries. [search_pass.cpp](engine/search_pass.cpp) chooses candidates in transcript order, checks ownership and join compatibility, and records the result. |
| Template dispatch | [dispatch.cpp](forms/dispatch.cpp) gathers proposals for a slot and connects declared shapes to their recognizers and cut policies. |
| Template families | [Lām](forms/lam), [teeth](forms/teeth.cpp), [counters and bowls](forms/counters_and_bowls.cpp), and [heads and ligatures](forms/heads_and_ligatures.cpp) keep shape geometry and cut policies together. Several forms can share the same recognizer. |
| Contour operations | [geometry](geometry) contains intersections, contour walks and projections, stroke sections, join order, and path partitioning. |
| Final assembly | [body_cut_plan.cpp](engine/body_cut_plan.cpp) gathers reusable evidence and reads attachment policies. [partition_assembly.cpp](engine/partition_assembly.cpp) builds letter and connector paths and checks recovered bodies and ink coverage. |

Template settings live in [letter-forms.json](../templates/letter-forms.json).
Recognizers use the outline and `body` settings to find evidence. Cut functions
use `cuts` settings to propose boundaries. The search pass decides which
proposals fit together. For lām, [recognition.cpp](forms/lam/recognition.cpp)
verifies the fork from the uncut ink and proposes cuts.
[attachment_policy.cpp](forms/lam/attachment_policy.cpp) applies the chosen
fork or ligature policy.

The `initial`, `medial` and `final` arrays set the priority within each pass.
An initial form with `sharedFoldWithFinal` takes its turn in the initial array
and uses the first matching final boundary for the declared adjacent letters.
`sharedFoldForm` can name the specific final template supplying the cut.
`onUnresolved` reserves a form for extended recovery. `onIncomplete` enables
it on a retry within that pass. The folded fāʾ form uses both settings.

A medial form can declare `terminalArmRecovery` with a final `prefixForm`
for its own letters and an `attached` or `detached` mode. Each mode names a
final arm profile and a catalog variant label. An attached mode uses a tapered
arm profile, and a detached mode uses a component arm profile. The prefix is
segmented with the usual matching priority. `prefixSection` overrides section
settings for the named prefix form if it matches, and `maxTipGap` controls how
far the arm tip may sit from the prefix body. The recipe's `cuts` override the
medial cut policy after recovery. Medial lām uses both arm modes.

Use the [native tests and catalog comparison](../tools/README.md#letter-audit)
to check a change. Audit and training snapshots fingerprint the cutter’s
sources and headers.
