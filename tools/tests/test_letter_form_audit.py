import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from letter_audit.audit_letter_forms import audit
from letter_audit.review_letter_forms import geometry_changed


class LetterFormAuditTests(unittest.TestCase):
    def test_owner_provenance_is_not_a_geometry_change(self):
        before = {'bodies': [{'commands': [['M', 0, 0], ['L', 1, 1]], 'fillRule': 'evenodd'}], 'joins': []}
        after = copy.deepcopy(before)
        after['bodies'][0].update(letter='ف', labelSource='remainder', reviewRequired=True)
        self.assertFalse(geometry_changed(before, after))
        after['bodies'][0]['commands'][1][1] = 2
        self.assertTrue(geometry_changed(before, after))

    def cache(self):
        return {"page": 3, "letterFormsSha256": "forms", "pageDataSha256": "page",
                "templateConfig": {"version": 2, "initial": [], "medial": [], "final": [],
                                   "kashidaDefaults": {"trackingEnabled": False}},
                "subwords": [{"line": 1, "word": 1, "subword": 1, "text": "بِمْ",
                    "complete": False, "bodies": [{}, {}], "trace": [], "letters": [
                        {"letterIndex": 1, "character": "ب", "position": "initial", "status": "matched",
                         "template": "beh-init", "candidates": ["beh-init"]},
                        {"letterIndex": 2, "character": "م", "position": "final", "status": "blocked",
                         "template": "", "candidates": ["meem-counter-fina"]}]}]}

    def test_region_count_is_not_body_coverage(self):
        result = audit(self.cache())
        self.assertEqual(result["summary"]["expectedRegionCount"], 1)
        self.assertEqual(result["summary"]["completeConnectedSubwords"], 0)
        self.assertEqual(result["letterStatuses"]["blocked"], 1)
        self.assertFalse(any(group["status"] == "no-template" for group in result["groups"]))

    def test_tatweel_and_attached_marks_do_not_create_body_slots(self):
        cache = self.cache()
        text = "بِـٔــۧمْ"
        cache["subwords"][0]["text"] = text
        result = audit(cache)
        self.assertEqual(result["summary"]["expectedRegionCount"], 1)
        self.assertEqual(result["summary"]["connectedSubwords"], 1)
        self.assertEqual(result["unresolvedSubwords"][0]["text"], text)
        self.assertEqual(result["unresolvedSubwords"][0]["letters"][0]["character"], "م")
        # Older caches with an extra diagnostic for the spacer need regeneration.
        cache["subwords"][0]["letters"].insert(1, {
            "letterIndex": 2, "character": "ـ", "position": "medial",
            "status": "no-template", "template": "", "candidates": []})
        with self.assertRaises(ValueError):
            audit(cache)

    def test_missing_selectors_and_failed_matches_stay_separate(self):
        cache = self.cache()
        second = copy.deepcopy(cache["subwords"][0])
        second["word"] = 2
        cache["subwords"].append(second)
        cache["subwords"][0]["letters"][1].update(status="no-template", candidates=[])
        second["letters"][1]["status"] = "no-match"
        result = audit(cache)
        self.assertEqual({g["status"] for g in result["groups"]}, {"no-template", "no-match"})
        self.assertEqual({g["examples"][0]["word"] for g in result["groups"]}, {1, 2})

    def test_rejects_stale_or_inconsistent_diagnostics(self):
        for mutation in (lambda c: c.pop("templateConfig"),
                         lambda c: c["subwords"][0]["letters"].pop(),
                         lambda c: c["subwords"][0].update(complete=True)):
            cache = self.cache()
            mutation(cache)
            with self.assertRaises(ValueError):
                audit(cache)

    def test_remainder_is_provisional_and_still_needs_a_template(self):
        cache = self.cache()
        sub = cache["subwords"][0]
        sub.update(text="فِي", provisional=True)
        sub["letters"][0].update(character="ف", status="remainder", template="", candidates=["feh-init"])
        sub["letters"][1].update(character="ي", status="matched", template="yeh-fina", candidates=["yeh-fina"])
        result = audit(cache)
        self.assertEqual(result["summary"]["provisionalSubwords"], 1)
        self.assertEqual(result["summary"]["completeSubwords"], 0)
        self.assertEqual(result["groups"][0]["status"], "remainder")
        self.assertEqual(result["unresolvedSubwords"][0]["reason"], "provisional-remainder")
        sub["complete"] = True
        with self.assertRaises(ValueError): audit(cache)


if __name__ == "__main__":
    unittest.main()
