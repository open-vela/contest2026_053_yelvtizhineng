"""Closed device address variants, complete existing FAQ aliases only.

ASR-only caller must enforce owner and interaction scope. Never rewrite the
transcript, use partial ASR, infer a substring, or create a question/answer.
"""
import re
_PUNCT=re.compile(r'[\s，。！？、,.!?~…·（）()【】\[\]:：;；]+')
_QUOTE=re.compile(r'''["'“”‘’「」『』]''')
_BLOCK=re.compile(r'不|别|没|现在|今天|刚才|上次|之前|这盆|这棵|我的|我们|它|这个|那个|然后|并且|但是|如果|还是|或者|区别|比较|先.+再|和.+(?:为什么|怎么|什么)')
META_INSTRUCTION='用户现在询问词语或句子的含义。只解释被讨论表达的意思，不要回答被引用的原问题。不要用机器人的身份、年龄、功能或自我介绍充当释义或举例。需要举例时用小明等普通人物，简短说明这个表达用于询问什么或表达什么。'
_PROFILE=frozenset({'who','greet','age','chat_cando'})

_META=re.compile(r'这(?:句(?:话)?|个词|些字|几个字)|那(?:句(?:话)?|个词)|什么意思|啥意思|什么含义|(?:一词|词语|句子|称呼)的?(?:意思|含义)|(?:意思|含义)是什么|(?:是|在)问什么')

_META_DENY=re.compile(r'^(?:(?:小伴|植小伴|小范|小半|小班|小万|小贩|请|你|先|麻烦你|麻烦))*(?:不要|不用|不必|无需|别)(?:再|给我|帮我|详细|继续|去|来)*(?:解释|讲解|说明|解读|翻译)')

def is_meta_question(text):
    return isinstance(text,str) and bool(_META.search(_PUNCT.sub('',text))) and not _META_DENY.search(_PUNCT.sub('',text))

def definition_entry(entry):
    return entry.get('id','').startswith(('faq_','sci_'))

def definition_candidates(text,entries):
    return [e for e in entries if definition_entry(e)] if is_meta_question(text) else list(entries)

def make_index(qa_bank):
    table={}
    for alias,entry,_entity in qa_bank._device_aliases()[0]:
        eid=entry.get('id','')
        if eid not in _PROFILE and not eid.startswith(('faq_','sci_')):
            continue
        if alias:
            table.setdefault(alias,set()).add(eid)
    return table

def select(text,qa_bank,index,names=()):
    if not isinstance(text,str) or _QUOTE.search(text) or '\n' in text or '\r' in text:
        return None
    raw=_PUNCT.sub('',text).lower()
    if not 4<=len(raw)<=48 or _BLOCK.search(raw):
        return None
    match=re.fullmatch(r'(小范|小半|小班|小万|小贩)(.+)',raw)
    if not match or match[1] in set(names or ()):
        return None
    ids=index.get(qa_bank._device_norm(match[2]),set())
    if len(ids)!=1:
        return None
    hit=qa_bank.match_device(match[2])
    # Return the current bank entry, never a stale indexed answer after reload.
    return hit if hit and hit.get('id') in ids and (not is_meta_question(text) or definition_entry(hit)) else None
