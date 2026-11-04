"""Prepare first-run FAQ resources without replacing existing runtime data."""
from pathlib import Path
import argparse
import json
import os

from build_box_qa_snapshot import build

SERVER = Path(__file__).resolve().parents[1]


def prepare(plant_server, box_ai, data_dir=None):
    plant_server, box_ai = Path(plant_server).resolve(), Path(box_ai).resolve()
    data_dir = Path(data_dir or os.environ.get('PLANT_DATA_DIR') or plant_server / 'data').resolve()
    for relative in ('', 'media/raw', 'media/image', 'media/audio', 'logs'):
        (data_dir / relative).mkdir(parents=True, exist_ok=True)
    bank = data_dir / 'qa_bank.json'
    copied = False
    if not bank.exists():
        with bank.open('xb') as stream:
            stream.write((plant_server / 'resources/qa_bank.json').read_bytes())
        copied = True
    index = box_ai / 'qa_bank_v490_20261103.sqlite3'
    if not index.exists():
        build(box_ai / 'resources/qa_entries.jsonl.gz', box_ai / 'resources/qa_database_schema.json', index)
    return {'data_dir': str(data_dir), 'main_bank_copied': copied, 'box_index': str(index)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plant-server', type=Path, default=SERVER / 'plant_server')
    parser.add_argument('--box-ai', type=Path, default=SERVER / 'box_ai')
    parser.add_argument('--data-dir', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.plant_server, args.box_ai, args.data_dir), ensure_ascii=False))
