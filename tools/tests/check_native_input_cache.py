"""Check cold/warm parity, fresh cuts after template edits, and cache corruption recovery."""
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/mushafcatalog'))
from segment_letters import prepare_page, native_binary
from store import DB


def main():
    with TemporaryDirectory(prefix='medina-native-input-check-') as temporary:
        work = Path(temporary)
        cache = work/'cache'
        templates = ROOT/'templates/letter-forms.json'

        def run(name, rules=templates):
            folder = work/name
            folder.mkdir()
            prepare_page(DB,'page415',native_binary(),folder,{'page415:l11-w6-s1'},rules,
                         profile=True,input_cache=cache)
            hit = json.loads((folder/'page415.profile.json').read_bytes())['inputCacheHit']
            return hit, (folder/'page415.vectors').read_bytes()

        cold, before = run('cold')
        warm, same = run('warm')
        assert not cold and warm and before==same, 'Cached input changed the resulting masks'

        draft = json.loads(templates.read_bytes())
        lam = next(f for f in draft['medial'] if f['id']=='lam-medi')
        lam['lamCuts']['forkOffset'] = .6
        edited = work/'edited.json'
        edited.write_text(json.dumps(draft,ensure_ascii=False))
        reused, different = run('edited',edited)
        assert reused and different!=same, 'A template edit reused stale letter cuts'

        entries = list(cache.glob('*.dat'))
        assert len(entries)==1, 'Unexpected source cache inventory'
        entries[0].write_bytes(entries[0].read_bytes()[:64])
        rebuilt, recovered = run('recovered')
        assert not rebuilt and recovered==same, 'Corrupt source cache was not regenerated'
    print('Native input cache: cold/warm parity, template independence, corruption recovery passed')


if __name__=='__main__':
    main()
