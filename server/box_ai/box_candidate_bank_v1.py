"""Read-only BOX FAQ snapshot. The generating database is never queried live."""
import difflib
import json
import re
import sqlite3
from contextlib import closing
from pathlib import Path

NORMALIZER_VERSION = 'box-candidate-v1'
PUNCT = re.compile(r"[\s，。！？、,.!?~…·\-—_（）()【】\[\]\"'“”‘’:：;；]+")
FILLER = '呀啊呢吧吗哦喔哈啦嘛喽呗~！!？?。.'
LIVE = re.compile(r'现在|实时|刚才|上次|之前|这盆|这棵|我的|我们|今天')
COMPLEX = re.compile(r'然后|并且|先.+再|区别|比较|哪个|还是')
NEGATION = re.compile(r'不|别|没')


def normalize(text):
    value = PUNCT.sub('', text or '').lower().strip(FILLER)
    value = re.sub(r'^(?:(?:小伴|植小伴|请问|我想知道|告诉我|能不能给我讲讲|能给我讲讲|能给我说说|能解释一下|可以解释一下|帮我解释一下|能告诉我))+', '', value)
    value = re.sub(r'为啥|为何', '为什么', value)
    value = re.sub(r'如何|怎样|怎么样', '怎么', value).replace('多长时间', '多久')
    return value


def canonical(text):
    value = normalize(text)
    for phrase in ('有什么用处', '有什么用途', '有什么用', '有何作用', '的作用是什么', '有哪些功能', '有什么功能'):
        value = value.replace(phrase, '有什么作用')
    if '有什么作用' in value:
        return 'purpose:' + value.replace('有什么作用', '').replace('的', '')
    if '为什么' in value:
        return 'why:' + value.replace('为什么', '').replace('的', '')
    if value.startswith('什么是'):
        return 'what:' + value[3:]
    if value.endswith('是什么'):
        return 'what:' + value[:-3]
    if '怎么' in value:
        return 'how:' + value.replace('怎么', '')
    return value


def keys(text):
    return tuple(dict.fromkeys(('plain:' + normalize(text), 'canonical:' + canonical(text))))


def grams(text):
    return set(text[i:i + 2] for i in range(len(text) - 1))


class CandidateBank:
    def __init__(self, path):
        self.path = Path(path).resolve()
        with closing(self.connect()) as connection:
            self.metadata = dict(connection.execute('SELECT key,value FROM metadata'))
        if self.metadata.get('normalizer_version') != NORMALIZER_VERSION:
            raise ValueError('Candidate normalizer mismatch')
        self.count = int(self.metadata['count'])

    def connect(self):
        # Published snapshots are immutable. Each request closes its connection
        # and holds only a bounded list of candidate questions in memory.
        connection = sqlite3.connect(self.path.as_uri() + '?mode=ro&immutable=1', uri=True, timeout=1)
        connection.execute('PRAGMA query_only=ON')
        connection.execute('PRAGMA cache_size=-2048')
        return connection

    def match(self, text):
        if not isinstance(text, str) or not text.strip() or len(text) > 512 or LIVE.search(normalize(text)):
            return None
        connection = self.connect()
        try:
            wanted = keys(text)
            placeholders = ','.join('?' for _ in wanted)
            ids = [row[0] for row in connection.execute(
                'SELECT DISTINCT fact_id FROM aliases WHERE alias IN (' + placeholders + ') LIMIT 2', wanted)]
            if len(ids) == 1:
                row = connection.execute('SELECT payload FROM entries WHERE id=?', ids).fetchone()
                return json.loads(row[0]) if row else None
            # Normalized collisions are ambiguous, including conflicting
            # conditions. Never pick whichever answer happens to be first.
            if ids or NEGATION.search(normalize(text)) or COMPLEX.search(normalize(text)):
                return None
            query = canonical(text)
            if len(query.split(':', 1)[-1]) < 6:
                return None
            tokens = sorted(grams(query))[:128]
            if not tokens:
                return None
            placeholders = ','.join('?' for _ in tokens)
            candidates = connection.execute(
                'SELECT fact_id,COUNT(*) AS score FROM grams WHERE gram IN (' + placeholders + ') '
                'GROUP BY fact_id ORDER BY score DESC,fact_id LIMIT 60', tokens).fetchall()
            ranked = []
            kind = query.split(':', 1)[0] if ':' in query else ''
            for fact_id, _ in candidates:
                row = connection.execute('SELECT normalized,subject FROM entries WHERE id=?', (fact_id,)).fetchone()
                if row is None:
                    continue
                candidate, subject = row
                # Similar wording about another planet, animal or material
                # must not borrow a factual answer about the wrong subject.
                if len(subject) < 2 or subject not in normalize(text):
                    continue
                if kind != (candidate.split(':', 1)[0] if ':' in candidate else ''):
                    continue
                score = difflib.SequenceMatcher(None, query.split(':', 1)[-1], candidate.split(':', 1)[-1]).ratio()
                ranked.append((score, fact_id))
            ranked.sort(reverse=True)
            # Retrieval is not an answer: require a strong similarity and a
            # clear margin before returning a stored factual answer.
            if not ranked or ranked[0][0] < .90 or (len(ranked) > 1 and ranked[0][0] - ranked[1][0] < .06):
                return None
            row = connection.execute('SELECT payload FROM entries WHERE id=?', (ranked[0][1],)).fetchone()
            return json.loads(row[0]) if row else None
        finally:
            connection.close()

    def source(self, fact_id):
        connection = self.connect()
        try:
            row = connection.execute('SELECT payload FROM entries WHERE id=?', (fact_id,)).fetchone()
            if not row:
                return None
            entry = json.loads(row[0])
            return {key: entry[key] for key in ('id', 'question', 'source', 'references', 'license', 'attribution', 'independent_fact_verification') if key in entry}
        finally:
            connection.close()
