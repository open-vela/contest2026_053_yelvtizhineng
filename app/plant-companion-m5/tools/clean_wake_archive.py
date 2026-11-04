"""Remove every stale copy of the exact verified wake SDK members under BIN.lock."""
import collections,hashlib,json,subprocess,sys
from pathlib import Path
ar,target,metadata=sys.argv[1:4];target=Path(target);metadata=Path(metadata)
r=json.loads(metadata.read_text());assert r['pass'] and r['SDK_loaded_section_bytes_flags_alignment_unchanged']
expected={'libmultinet.a','libdl_lib.a','libhufzip.a','libc_speech_features.a','libfst.a','libnewlib_setjmp.a'}
assert {x['library'] for x in r['libraries']}==expected
def members(path):return subprocess.check_output([ar,'t',str(path)],text=True).splitlines()
selected=[]
for row in r['libraries']:
    library=metadata.parent/'lib'/row['library']
    assert hashlib.sha256(library.read_bytes()).hexdigest()==row['after_sha256'],'SDK source drift'
    names=members(library);assert names==row['members'];selected+=names
math=metadata.parent/'lib/libplant_wake_math.a'
assert hashlib.sha256(math.read_bytes()).hexdigest()==r['math_closure_sha256']
selected+=members(math)
assert selected==r['cleanup_allowlist'] and len(selected)==len(set(selected))
assert all(Path(n).name==n and n not in {'.','..'} for n in selected)
before=members(target);counts=collections.Counter(before)
assert max((counts[n] for n in selected),default=0)<=100,'Unexpected SDK archive multiplication'
args=[n for n in selected for _ in range(counts[n])]
assert sum(len(n)+1 for n in args)<1000000,'Command needs a bounded cleanup batch'
# GNU ar accepts a repeated member name once per copy. One rewrite removes
# the old 56-copy SDK accumulation without reserializing the archive 56 times.
if args:subprocess.run([ar,'d',str(target),*args],check=True)
after=members(target)
assert not set(selected).intersection(after)
assert collections.Counter(n for n in before if n not in selected)==collections.Counter(after)
print(json.dumps({'removed_stale_wake_members':len(args),'max_copies':max((counts[n] for n in selected),default=0),'foreign_member_counts_preserved':True}))
