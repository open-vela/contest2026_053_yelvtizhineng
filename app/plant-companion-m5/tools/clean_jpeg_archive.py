"""Remove ALL old copies of only the official JPEG SDK archive members.

GNU ar MRI addlib retains duplicates; changing the SDK section placement
otherwise leaves an earlier IRAM copy selected by the linker. Run under the
same libapps.a flock as the subsequent merge, preserving every other member.
"""
import collections,json,re,subprocess,sys
from pathlib import Path
ar,target,library=sys.argv[1:4]
assert Path(target).is_file() and Path(library).is_file()
def members(path):return subprocess.check_output([ar,'t',path],text=True).splitlines()
selected=members(library)
assert selected and len(selected)==len(set(selected))
assert all(re.fullmatch(r'(?:esp_)?jpeg_[A-Za-z0-9_.]+\.obj',name) or name in {'plant_vad_minimal.o','plant_ns_minimal.o','plant_nn_original.o'} for name in selected)
before=members(target);counts=collections.Counter(before)
copies=max(counts[name] for name in selected)
assert copies<=100,'Unexpected archive duplication; inspect before mutation'
for _ in range(copies):subprocess.run([ar,'d',target,*selected],check=True)
after=members(target)
assert not set(selected).intersection(after),'Stale JPEG member remains'
assert collections.Counter(n for n in before if n not in selected)==collections.Counter(after),'Unrelated archive member changed'
print(json.dumps({'removed_stale_JPEG_members':sum(counts[n] for n in selected),'max_copies':copies,'other_members_preserved':True}))
