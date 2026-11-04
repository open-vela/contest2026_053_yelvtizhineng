"""Only an exact speech-repeat request and an untruncated completed reply."""
import re
_QUOTE=re.compile(r'''["'“”‘’「」『』]''')
_PUNCT=re.compile(r'[\s，。！？,.!?~～、：:；;]+')
_ADDRESS=re.compile(r'^(植小伴|哈喽小伴|小伴|小范|小半|小班|小万|小贩|小棒|小曼|小胖)')
_REQUEST=re.compile(r'^(?:(?:刚才那个|刚才那句|刚才的话|你刚才说的|刚才|刚刚|上一句))?'
                    r'(?:(?:我没听懂|我没听清楚|我没听清|我没有听清|我没听到|没听懂|没听清))?'
                    r'(?:(?:请你|麻烦你|能不能|可以|能否|请|能|帮我|给我|你))*'
                    r'(?:再说|再讲|再重复|重复)(?:一遍|一次|一下)'
                    r'(?:好不好|可以吗|好吗|行吗|吗|吧)?$')

def is_request(text,names=()):
    if not isinstance(text,str) or '\n' in text or '\r' in text or _QUOTE.search(text):
        return False
    raw=_PUNCT.sub('',text)
    if not 4<=len(raw)<=48:return False
    prefix=_ADDRESS.match(raw)
    if prefix:
        if prefix[1] in set(names or ()):return False
        raw=raw[prefix.end():]
    return bool(_REQUEST.fullmatch(raw))

def complete_answer(history):
    if not isinstance(history,list) or not history:return None
    last=history[-1]
    if not isinstance(last,dict) or last.get('role')!='assistant':return None
    answer=last.get('text')
    # Context truncates at240chars. Boundarylength must fall back; never invent
    # a continuation or replay a partial long story as the complete answer.
    if not isinstance(answer,str) or not 1<=len(answer)<240 or not answer.strip():
        return None
    return answer

def is_failure_marker(text):
    # The provider returns plain ASR text. A substring is not an error signal:
    # "我没听清，请你再说一次" is a valid request, as is a definition question.
    return _PUNCT.sub('',text or '') in {'没听清','没听清楚','没听清请再说一遍','没听清请再说一次'}
