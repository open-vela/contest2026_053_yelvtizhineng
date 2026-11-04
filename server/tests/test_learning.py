import importlib.util
import json
import sqlite3
import sys
import tempfile
import types
import unittest
from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
from pathlib import Path

root=Path(__file__).resolve().parents[1]/'plant_server'
tmp=tempfile.TemporaryDirectory(prefix='plant_learning_test_')
dbpath=Path(tmp.name)/'isolated.sqlite'
pkg=types.ModuleType('release_test_app');pkg.__path__=[str(root/'app')];sys.modules[pkg.__name__]=pkg
cfg=types.ModuleType(pkg.__name__+'.config');cfg.DB_PATH=str(dbpath);sys.modules[cfg.__name__]=cfg
util=types.ModuleType(pkg.__name__+'.utils');sys.modules[util.__name__]=util
today='2026-11-04'
util.bj_now=lambda:(today+' 12:00:00',today,'12:00',0)
import uuid
util.new_id=lambda prefix:prefix+'_'+uuid.uuid4().hex[:12]
routers=types.ModuleType(pkg.__name__+'.routers');routers.__path__=[str(root/'app/routers')];sys.modules[routers.__name__]=routers
from fastapi import HTTPException
def either(auth,device):
    if auth not in ['Bearer owner','Bearer other']:
        raise HTTPException(401,'auth required')
    return {'sub':auth.split()[1]},'user'
routers.either=either
spec=importlib.util.spec_from_file_location(routers.__name__+'.learning',root/'app/routers/learning.py')
m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)


class LearningTest(unittest.TestCase):
    def setUp(self):
        with sqlite3.connect(dbpath) as con:
            for row in con.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
                con.execute('DROP TABLE '+row[0])
            con.executescript('CREATE TABLE users(id TEXT PRIMARY KEY); INSERT INTO users VALUES("owner"),("other"); CREATE TABLE plants(id TEXT PRIMARY KEY,user_id TEXT,name TEXT,species TEXT,started_at TEXT,created_at TEXT); CREATE TABLE events(id TEXT PRIMARY KEY,plant_id TEXT,user_id TEXT,type TEXT,title TEXT,summary TEXT,growth_delta INTEGER,source TEXT,event_ts TEXT,created_at TEXT);')
            con.executemany('INSERT INTO plants VALUES(?,?,?,?,?,?)', [('p1','owner','one','绿萝','2026-10-01','2026-10-01'),('p2','owner','two','黄金葛','2026-10-02','2026-10-02'),('p3','owner','unknown','多肉','2026-10-03','2026-10-03'),('secret','other','private','绿萝','2026-01-01','2026-01-01')])
        m.init_learning()

    def overview(self):return m.overview('Bearer owner')
    def pothos(self):return next(p for p in self.overview()['entries'] if p['id']=='pothos')
    def test_deduplicate_species_and_unknown(self):
        j=self.overview();self.assertEqual(len([p for p in j['entries'] if p['owned']]),1);self.assertEqual(len(self.pothos()['plants']),2);self.assertEqual(self.pothos()['level'],1);self.assertEqual(len(j['plants']),3)
    def test_auth_required(self):
        with self.assertRaises(HTTPException) as cm:m.overview(None)
        self.assertEqual(cm.exception.status_code,401)
    def test_cross_user_access_denied(self):
        with self.assertRaises(HTTPException):m.observe(m.ObservationIn(plant_id='secret',choice='wet'),'Bearer owner')
        with self.assertRaises(HTTPException):m.assign(m.AssignIn(plant_id='secret',species_id='aloe'),'Bearer owner')
        with self.assertRaises(HTTPException):m.observations('secret','Bearer owner')
    def test_quiz_wrong_and_idempotent(self):
        self.assertFalse(m.complete(m.QuizIn(article_id='water',answer=0),'Bearer owner')['correct'])
        for i in range(2):m.complete(m.QuizIn(article_id='water',answer=1),'Bearer owner')
        self.assertEqual(self.overview()['learned'],['water'])
    def test_bookmark_persists_and_user_isolation(self):
        m.bookmark(m.BookmarkIn(article_id='water',saved=True),'Bearer owner');self.assertEqual(self.overview()['bookmarks'],['water']);self.assertEqual(m.overview('Bearer other')['bookmarks'],[])
        m.bookmark(m.BookmarkIn(article_id='water',saved=False),'Bearer owner');self.assertEqual(self.overview()['bookmarks'],[])
    def test_concurrent_observation_one_event(self):
        def save(_):return m.observe(m.ObservationIn(plant_id='p1',choice='wet',note='test'),'Bearer owner')
        with ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(save,range(8)))
        with sqlite3.connect(dbpath) as con:self.assertEqual(con.execute('SELECT COUNT(*) FROM events').fetchone()[0],1)
        self.assertEqual(self.pothos()['metrics']['observations'],1)
    def test_multiple_same_species_same_day_counts_once(self):
        for p in ['p1','p2']:m.observe(m.ObservationIn(plant_id=p,choice='no_change'),'Bearer owner')
        self.assertEqual(self.pothos()['metrics']['observations'],1)
    def test_assign_preserves_original_species(self):
        m.assign(m.AssignIn(plant_id='p3',species_id='aloe'),'Bearer owner')
        self.assertTrue(next(p for p in self.overview()['entries'] if p['id']=='aloe')['owned'])
        with sqlite3.connect(dbpath) as con:self.assertEqual(con.execute('SELECT species FROM plants WHERE id="p3"').fetchone()[0],'多肉')
    def add_history(self):
        with sqlite3.connect(dbpath) as con:
            for i in range(12):con.execute('INSERT INTO zx_observations VALUES(?,?,?,?,?,?,?)',('o'+str(i),'owner','p1',(date(2026,10,1)+timedelta(days=i)).isoformat(),'no_change','real observation','2026-10-01'))
    def test_comparison_validity_and_level_gates(self):
        self.add_history()
        for a in ['water','light','leaf','pot']:m.complete(m.QuizIn(article_id=a,answer=m.ARTICLES[a]['correct']),'Bearer owner')
        self.assertEqual(self.pothos()['level'],3)
        with self.assertRaises(HTTPException):m.compare(m.ComparisonIn(plant_id='p1',before_id='o0',after_id='o1',note='too close'),'Bearer owner')
        m.compare(m.ComparisonIn(plant_id='p1',before_id='o0',after_id='o3',note='noticing no visible change'),'Bearer owner');self.assertEqual(self.pothos()['level'],4)
        with self.assertRaises(HTTPException):m.compare(m.ComparisonIn(plant_id='p1',before_id='o0',after_id='o3',note='duplicate'),'Bearer owner')
        m.compare(m.ComparisonIn(plant_id='p1',before_id='o3',after_id='o6',note='another careful comparison'),'Bearer owner')
        m.reflect(m.ReflectionIn(plant_id='p1',note='I checked the soil and waited, then observed the leaves over several days.'),'Bearer owner');self.assertEqual(self.pothos()['level'],5)
    def test_reflection_edits_one_diary_entry(self):
        for i in range(2):m.reflect(m.ReflectionIn(plant_id='p1',note='A meaningful reflection with observations, decisions and follow up '+str(i)),'Bearer owner')
        with sqlite3.connect(dbpath) as con:self.assertEqual(con.execute('SELECT COUNT(*) FROM events').fetchone()[0],1)
    def test_no_phantom_old_task_progress(self):
        with sqlite3.connect(dbpath) as con:con.execute('INSERT INTO events VALUES("old","p1","owner","water","dry run","simulated",100,"device","2026-10-01","2026-10-01")')
        self.assertEqual(self.pothos()['level'],1);self.assertEqual(self.pothos()['metrics']['observations'],0)
    def test_untrusted_article_rejected(self):
        with self.assertRaises(HTTPException):m.complete(m.QuizIn(article_id='invented',answer=1),'Bearer owner')

if __name__=='__main__':
    import gc
    runner=unittest.main(verbosity=2,exit=False)
    gc.collect()
    tmp.cleanup()
    sys.exit(0 if runner.result.wasSuccessful() else 1)
