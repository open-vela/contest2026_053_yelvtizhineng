"""Rebuild the read-only FAQ index from attributed source entries; no network."""
from pathlib import Path
import argparse
import gzip
import json
import sqlite3
import sys

SERVER = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER / 'box_ai'))
from box_candidate_bank_v1 import canonical, normalize, keys, grams, NORMALIZER_VERSION


def build(source, schema_path, target):
    source, schema_path, target = Path(source), Path(schema_path), Path(target)
    if target.exists():
        raise FileExistsError('Refusing to replace existing FAQ index: ' + str(target))
    definition = json.loads(schema_path.read_text(encoding='utf-8'))
    if definition['metadata']['normalizer_version'] != NORMALIZER_VERSION:
        raise ValueError('Source normalizer does not match the runtime')
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + '.building')
    if temporary.exists():
        raise FileExistsError('A previous build file exists: ' + str(temporary))
    database = sqlite3.connect(temporary)
    try:
        database.executescript('PRAGMA journal_mode=DELETE; PRAGMA cache_size=-4096;')
        for item in definition['schema']:
            database.execute(item['sql'])
        count = 0
        with gzip.open(source, 'rt', encoding='utf-8', newline='') as stream:
            for line in stream:
                entry = json.loads(line)
                if entry.get('license') != 'CC-BY-SA-4.0' or not entry.get('attribution') or not entry.get('references'):
                    raise ValueError('Missing FAQ attribution or license')
                fact_id, question = entry['id'], entry['question']
                normalized = canonical(question)
                subject = normalize(entry['fact_key'].split('|', 1)[0])
                payload = json.dumps(entry, ensure_ascii=False, separators=(',', ':'))
                database.execute('INSERT INTO entries VALUES(?,?,?,?,?)', (fact_id, question, normalized, subject, payload))
                aliases = set()
                for alias in [question, *entry.get('examples', [])]:
                    if isinstance(alias, str):
                        aliases.update(keys(alias))
                database.executemany('INSERT OR IGNORE INTO aliases VALUES(?,?)', [(alias, fact_id) for alias in sorted(aliases)])
                database.executemany('INSERT OR IGNORE INTO grams VALUES(?,?)', [(gram, fact_id) for gram in sorted(grams(normalized))])
                count += 1
                if count % 5000 == 0:
                    database.commit()
        if count != definition['count']:
            raise ValueError('FAQ entry count differs from source metadata')
        database.executemany('INSERT INTO metadata VALUES(?,?)', definition['metadata'].items())
        database.commit()
        if database.execute('PRAGMA quick_check').fetchone()[0] != 'ok':
            raise ValueError('FAQ database integrity check failed')
    finally:
        database.close()
    temporary.rename(target)
    return {'entries': count, 'output': str(target), 'bytes': target.stat().st_size}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SERVER / 'box_ai/resources/qa_entries.jsonl.gz')
    parser.add_argument('--schema', type=Path, default=SERVER / 'box_ai/resources/qa_database_schema.json')
    parser.add_argument('--output', type=Path, default=SERVER / 'box_ai/qa_bank_v490_20261103.sqlite3')
    args = parser.parse_args()
    print(json.dumps(build(args.source, args.schema, args.output), ensure_ascii=False))
