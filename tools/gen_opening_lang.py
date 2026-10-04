#!/usr/bin/env python3
"""Generates the Coach's opening texts (10 languages) and the opening metadata.

Inputs (tools/openings_source/):
  families.json        curated opening families and notable variations: names in the 10 UI languages (citation
                       form, form with article, gender), side, tier, umbrella, lichess name prefixes, glossary of
                       variation-name words, eponyms (from the openings research of 2026-09-30)
  texts/<code>.txt     hand-written texts per language, same "key = value" format as the .lang files:
                       sentence templates (opening.say.*), comments on what each opening leads to
                       (opening.family.<id>.beginner/.advanced, opening.variation.<id>...), optional overrides
                       of generated names (opening.variation.<id>.def, ...)

Outputs:
  assets/coach/openings/<code>.lang          catalog keys of every language (the same key set in all of them)
  assets/coach/openings_data/openings.json   metadata read by src/coach/openings.cpp (no text)

Key scheme (every key starts with "opening."):
  opening.say.<name>[.N]            sentence templates, variants .2, .3 ...
  opening.family.<id>               citation name ("Sicilian Defense"); .def = the form used inside a sentence
                                    (with its article: "the Sicilian Defense", "la défense sicilienne", Russian
                                    and Ukrainian names in lower case); .beginner / .advanced = comments
  opening.variation.<id>            same for the notable variations (.beginner only where a beginner meets it)
  <text key>.spoken                 what the voice says instead of <text key>, present in every language when at
                                    least one language needs it: English respellings for the TTS ("Nigh-dorf"),
                                    squares and files spelled out ("ee four"), " — " inside Russian/Ukrainian
                                    names turned into a hyphen. Chinese keeps its text (Chinese speech is English).
  opening.compose.<type>            how a lichess variation name "<Proper> <Type>" is said ("variante {X}",
                                    "вариант {X:gen}"); opening.component.<slug>: fixed phrases
                                    ("Exchange Variation"); opening.eponym.<slug> (+ .gen): proper names

Run from the repository root:  python3 tools/gen_opening_lang.py [--check]
  --check  writes nothing; exits 1 when an output is missing or out of date.
"""
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC = os.path.join(ROOT, 'tools', 'openings_source')
OUT_LANG = os.path.join(ROOT, 'assets', 'coach', 'openings')
OUT_META = os.path.join(ROOT, 'assets', 'coach', 'openings_data', 'openings.json')
LANGS = ['en', 'fr', 'de', 'es', 'ru', 'uk', 'ar', 'ja', 'zh-Hans', 'zh-Hant']

# Types of lichess variation names that compose ("<Proper> <Type>"), with their catalog slug.
COMPOSE_TYPES = ['Variation', 'Attack', 'Gambit', 'Defense', 'System', 'Countergambit', 'Counterattack', 'Line',
                 'Trap', 'Opening']
# Whole lichess components translated as such (from the glossary). The Scotch and Danish ones are the frequent
# components whose English adjective (a common word below) would otherwise leave them unsaid in fr, de and es.
FIXED_COMPONENTS = ['Exchange Variation', 'Advance Variation', 'Classical Variation', 'Modern Variation',
                    'Open Variation', 'Closed Variation', 'Fianchetto Variation', 'Symmetrical Variation',
                    'Scotch Gambit', 'Scotch Variation', 'Danish Variation']

# Words of lichess variation names that are ordinary English (adjectives, nouns, nationalities, cities with a
# name of their own in other languages). French, German and Spanish only compose "<Proper> <Type>" when no word of
# <Proper> is in this list ("attaque Paulsen", but no "attaque English"); the game then names the family only.
COMMON_WORDS = sorted(set('''
Accelerated Accepted Advance Aged Alien Amazon American Anglo Anti Apocalypse Arctic Argentine Argentinian Ark
Armenian Attack Australian Austrian Baltic Banker Banzai Basque Battery Bavarian Bayonet Beach Been Big Bishop Black
Blockade Bouncing Brick Bulgarian Castling Cavalry Center Central Chameleon Check Cheese Chinese Clam Classical Claw
Closed Cobra Compromised Corkscrew Cormorant Counterthrust Crab Crocodile Czech Declined Defensive Deferred Delayed
Development Dog Double Drill Drunken Dutch Dynamic Edge Elbow Endgame English Exchange Fianchetto Finnish First Fishing
Flank Flexible Fluid Forcing Forgotten Fort Four French Fried Full Fully Gambit German Giraffe Grab Grand Great Hammer
Hawk Head Hedgehog Hippopotamus Holding Hook Horny Horsefly Hungarian Icelandic Improved Indian Inner Intermezzo Irish
Italian King Kingfisher Kingside Knight Knights Lizard Lobster Long Mad Mafia Main Martian Meadow Mediterranean
Mexican Modern Mongoose Mosquito Mustang Neo New Nightingale Normal Norwegian Old Open Original Orthodox Outflank Pawn
Pawns Penguin Pin Plasma Poisoned Pole Polish Portuguese Positional Prickly Primitive Provincial Pseudo Push Queen
Queenside Quiet Raptor Rare Retreat Return Reversed Romanian Rooks Rotary Russian Sacrifice Scandinavian Scorpion
Second Semi Sharp Short Shuffle Shy Siberian Sicilian Simul Six Slav Slow Small Snail Snake Spanish Spike Standard
Stone Stonewall Storm Storming Swap Swedish Swiss Symmetric Symmetrical Symmetry Three Toilet Tortoise Tour
Traditional Transpositional Two Ukrainian Ultra Unicorn Valencian Viennese Walk Wall Wasp Wayward Westphalian Whip
Wild Wind Wing Winter Wolf Woodchuck Yugoslav Hyperaccelerated Accelerated Wing Beginner Dragon Lion Hippo Rat
Catalan Danish Florentine Kazakh Netherlands Scotch
Amsterdam Belgrade Berlin Birmingham Bremen Breslau Brooklyn Brussels Budapest Cambridge Chicago Cologne Copenhagen
Cracow Dresden Edinburgh Frankfurt Hastings Leningrad Lisbon London Manhattan Melbourne Moscow Oxford Paris Prague
Riga Seville Stockholm Tashkent Venice Vienna Warsaw York Zagreb Zurich Nuremberg Nürnberg Düsseldorf Wiesbaden
Petersburg Buenos Aires Janeiro Rio Monte Carlo Santa San St. Dr. La El De Del Der Von The and with de del der von
'''.split()))

# French, German and Spanish spellings of eponyms that differ from the English one (the others keep it).
EPONYM_LATIN = {
    'fr': {'Chigorin': 'Tchigorine', 'Taimanov': 'Taïmanov', 'Sveshnikov': 'Svechnikov', 'Petrosian': 'Petrossian',
           'Zaitsev': 'Zaïtsev', 'Scheveningen': 'Scheveningue'},
    'de': {'Alekhine': 'Aljechin', 'Chigorin': 'Tschigorin', 'Botvinnik': 'Botwinnik', 'Smyslov': 'Smyslow',
           'Spassky': 'Spasski', 'Taimanov': 'Taimanow', 'Sveshnikov': 'Sweschnikow', 'Karpov': 'Karpow',
           'Kasparov': 'Kasparow', 'Petrosian': 'Petrosjan', 'Averbakh': 'Awerbach', 'Zaitsev': 'Saizew',
           'Petrov': 'Petrow', 'Panov': 'Panow', 'Keres': 'Keres', 'Bronstein': 'Bronstein',
           'Scheveningen': 'Scheveninger'},
    'es': {'Taimanov': 'Taimánov', 'Karpov': 'Kárpov', 'Kasparov': 'Kaspárov', 'Petrosian': 'Petrosián',
           'Averbakh': 'Averbaj', 'Zaitsev': 'Záitsev'},
}

# Spoken squares and files. Files are said by their letter's name, ranks as number words (as file.*.spoken and
# rank.*.spoken in assets/coach/speech/<code>/common.lang; French "eu" and "huite": the voice says a lone "e" too
# briefly and reads "huit" as "hui").
FILE_WORDS = {
    'en': ['ay', 'bee', 'see', 'dee', 'ee', 'eff', 'gee', 'aitch'],
    'fr': ['a', 'bé', 'cé', 'dé', 'eu', 'effe', 'gé', 'ache'],
    'de': ['a', 'be', 'ce', 'de', 'e', 'ef', 'ge', 'ha'],
    'es': ['a', 'be', 'ce', 'de', 'e', 'efe', 'ge', 'hache'],
    'ru': ['а', 'бэ', 'цэ', 'дэ', 'е', 'эф', 'жэ', 'аш'],
    'uk': ['а', 'бе', 'це', 'де', 'е', 'еф', 'же', 'аш'],
    'ar': ['إيه', 'بي', 'سي', 'دي', 'إي', 'إف', 'جي', 'إتش'],
    'ja': ['エー', 'ビー', 'シー', 'ディー', 'イー', 'エフ', 'ジー', 'エイチ'],
}
RANK_WORDS = {
    'en': ['one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight'],
    'fr': ['un', 'deux', 'trois', 'quatre', 'cinq', 'six', 'sept', 'huite'],
    'de': ['eins', 'zwei', 'drei', 'vier', 'fünf', 'sechs', 'sieben', 'acht'],
    'es': ['uno', 'dos', 'tres', 'cuatro', 'cinco', 'seis', 'siete', 'ocho'],
    'ru': ['один', 'два', 'три', 'четыре', 'пять', 'шесть', 'семь', 'восемь'],
    'uk': ['один', 'два', 'три', 'чотири', "п'ять", 'шість', 'сім', 'вісім'],
    'ar': ['واحد', 'اثنان', 'ثلاثة', 'أربعة', 'خمسة', 'ستة', 'سبعة', 'ثمانية'],
    'ja': ['いち', 'に', 'さん', 'よん', 'ご', 'ろく', 'なな', 'はち'],
}
SQUARE_SEP = {'ja': ''}
# A file named by its letter next to its noun ("the c-file", "la colonne c", "c-Linie", "cファイル").
FILE_PATTERNS = {
    'en': (r'\b([a-h])-(files?|pawns?)\b', lambda m, w: w[m.group(1)] + ' ' + m.group(2)),
    'fr': (r'\b(colonnes?|pions?) ([a-h])\b', lambda m, w: m.group(1) + ' ' + w[m.group(2)]),
    'de': (r'\b([a-h])-(Linien?|Bauern?|Bauers)\b', lambda m, w: w[m.group(1)] + '-' + m.group(2)),
    'es': (r'\b(columnas?|peón|peones) ([a-h])\b', lambda m, w: m.group(1) + ' ' + w[m.group(2)]),
    'ru': (r'(лини[яиюей]+|вертикал[ьиюей]+|пешк[аиуойе]+) ([a-h])\b', lambda m, w: m.group(1) + ' ' + w[m.group(2)]),
    'uk': (r'(ліні[яїюєй]+|вертикал[ьіюей]+|пішак[аиуомів]*) ([a-h])\b',
           lambda m, w: m.group(1) + ' ' + w[m.group(2)]),
    'ar': (r'(العمود|عمود|البيدق|بيدق) ([a-h])\b', lambda m, w: m.group(1) + ' ' + w[m.group(2)]),
    'ja': (r'([a-h])(ファイル|ポーン)', lambda m, w: w[m.group(1)] + m.group(2)),
}
SQUARE_RE = re.compile(r'(?<![A-Za-z0-9])([a-h])([1-8])(?![0-9A-Za-z])')
UPPER = 'A-ZА-ЯЁІЇЄҐ'
NAME_DASH_RE = re.compile(r'([' + UPPER + r'][\w\'’]*) — ([' + UPPER + r'])')


def fail(msg):
    sys.stderr.write('gen_opening_lang: ' + msg + '\n')
    sys.exit(2)


def slug(s):
    t = s.lower()
    for a, b in (('ü', 'u'), ('é', 'e'), ('ó', 'o'), ('ä', 'a'), ('ö', 'o'), ('á', 'a'), ('í', 'i'), ('ć', 'c')):
        t = t.replace(a, b)
    t = re.sub(r'[^a-z0-9]+', '_', t).strip('_')
    return t


def parse_lang(path):
    """Parses a key = value file; returns an ordered list of (key, value)."""
    out = []
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            if '=' not in s:
                fail('%s:%d: missing "="' % (path, n))
            k, v = s.split('=', 1)
            k, v = k.strip(), v.strip()
            if not k or not v:
                fail('%s:%d: empty key or value' % (path, n))
            out.append((k, v))
    keys = [k for k, _ in out]
    if len(keys) != len(set(keys)):
        dup = sorted({k for k in keys if keys.count(k) > 1})
        fail('%s: duplicate keys %s' % (path, dup))
    return out


def placeholder_names(v):
    return sorted({m.split(':')[0] for m in re.findall(r'\{([^{}]*)\}', v)})


def lower_first(s):
    return s[:1].lower() + s[1:] if s else s


# ---- Forms of variation names used inside sentences (families carry theirs in families.json) -------------------
FR_ARTICLE = {  # first word (lower case) -> article
    'variante': 'la ', 'attaque': "l'", 'défense': 'la ', 'gambit': 'le ', 'contre-attaque': 'la ',
    'contre-gambit': 'le ', 'sicilienne': 'la ', 'espagnole': "l'", 'catalane': 'la ', 'dragon': 'le ',
    'giuoco': 'le ', 'benoni': 'la ',
}
ES_ARTICLE = {
    'variante': 'la ', 'ataque': 'el ', 'gambito': 'el ', 'defensa': 'la ', 'contraataque': 'el ',
    'contragambito': 'el ', 'siciliana': 'la ', 'española': 'la ', 'catalana': 'la ', 'dragón': 'el ',
    'giuoco': 'el ', 'benoni': 'la ',
}
KEEP_CAPITAL = {'Benoni'}  # proper nouns that start a French or Spanish name


def romance_def(name, table, lang, what):
    first = name.split(' ')[0]
    art = table.get(first.lower())
    if art is None:
        fail('no %s article rule for "%s" (%s): add an override to texts/%s.txt' % (lang, name, what, lang))
    body = name if first in KEEP_CAPITAL else lower_first(name)
    return art + body


def variation_def(lang, name, what):
    if lang == 'en':
        return 'the ' + name
    if lang == 'fr':
        return romance_def(name, FR_ARTICLE, lang, what)
    if lang == 'es':
        return romance_def(name, ES_ARTICLE, lang, what)
    if lang in ('ru', 'uk'):
        return lower_first(name)
    if lang == 'de':
        return None  # German variation forms are hand-written in texts/de.txt (adjective endings)
    return name


def family_def(lang, fam):
    n = fam['names'][lang]
    if lang in ('en', 'fr', 'de', 'es'):
        if not n.get('def'):
            fail('family %s has no %s "def" form' % (fam['id'], lang))
        return n['def']
    if lang in ('ru', 'uk'):
        return lower_first(n['name'])
    return n['name']


# ---- Speech ------------------------------------------------------------------------------------------------------
def spoken(lang, text, respell):
    if lang.startswith('zh'):
        return text  # Chinese speech is the English line
    t = text
    if lang == 'en':
        for k in sorted(respell, key=len, reverse=True):
            t = t.replace(k, respell[k])
    if lang in ('ru', 'uk'):
        t = NAME_DASH_RE.sub(r'\1-\2', t)
    fw = dict(zip('abcdefgh', FILE_WORDS[lang]))
    pat = FILE_PATTERNS.get(lang)
    if pat:
        t = re.sub(pat[0], lambda m: pat[1](m, fw), t)
    sep = SQUARE_SEP.get(lang, ' ')
    t = SQUARE_RE.sub(lambda m: fw[m.group(1)] + sep + RANK_WORDS[lang][int(m.group(2)) - 1], t)
    return t


def typography(lang, text):
    """The typography of the game's own translations (assets/i18n): French non-breaking spaces before : ; ? ! and
    inside guillemets, typographic apostrophes in French and Ukrainian. Sources use plain spaces and straight
    apostrophes. The voice reads both forms the same way."""
    t = text
    if lang in ('fr', 'uk'):
        t = t.replace("'", '\u2019')
    if lang == 'fr':
        t = re.sub(r' ([:;?!])', '\u00a0\\1', t)
        t = t.replace('\u00ab ', '\u00ab\u00a0').replace(' \u00bb', '\u00a0\u00bb')
    return t


# ---- Main --------------------------------------------------------------------------------------------------------
def build():
    with open(os.path.join(SRC, 'families.json'), encoding='utf-8') as f:
        J = json.load(f)
    fams, vars_ = J['families'], J['notable_variations']
    respell = J['meta']['tts_en_respell']
    fam_ids = [x['id'] for x in fams]
    var_ids = [x['id'] for x in vars_]
    texts = {}
    for lang in LANGS:
        p = os.path.join(SRC, 'texts', lang + '.txt')
        if not os.path.exists(p):
            fail('missing ' + p)
        texts[lang] = dict(parse_lang(p))

    # Text keys hand-written in English define the key set of every language.
    en_text_keys = [k for k, _ in parse_lang(os.path.join(SRC, 'texts', 'en.txt'))]
    for k in en_text_keys:
        if not (k.startswith('opening.say.') or re.match(r'opening\.(family|variation)\.[a-z0-9_]+\.(beginner|advanced)$', k)):
            fail('en.txt: unexpected key ' + k)
    for x in fams:
        for lvl in ('beginner', 'advanced'):
            if 'opening.family.%s.%s' % (x['id'], lvl) not in texts['en']:
                fail('en.txt: missing comment opening.family.%s.%s' % (x['id'], lvl))
    for x in vars_:
        for lvl in x['comment']:
            if 'opening.variation.%s.%s' % (x['id'], lvl) not in texts['en']:
                fail('en.txt: missing comment opening.variation.%s.%s' % (x['id'], lvl))
    for k in texts['en']:
        m = re.match(r'opening\.(family|variation)\.([a-z0-9_]+)\.', k)
        if m and m.group(2) not in (fam_ids if m.group(1) == 'family' else var_ids):
            fail('en.txt: unknown id in ' + k)

    # Overrides a language may give (generated names): opening.(family|variation).<id>[.def]
    def override(lang, key):
        return texts[lang].get(key)

    for lang in LANGS:
        for k in texts[lang]:
            if k in texts['en'] and k in en_text_keys:
                continue
            m = re.match(r'opening\.(family|variation)\.([a-z0-9_]+)(\.def)?$', k)
            if m and m.group(2) in (fam_ids if m.group(1) == 'family' else var_ids):
                continue
            fail('%s.txt: unknown or extra key %s' % (lang, k))
        for k in en_text_keys:
            if k not in texts[lang]:
                fail('%s.txt: missing %s' % (lang, k))
            if placeholder_names(texts[lang][k]) != placeholder_names(texts['en'][k]):
                fail('%s.txt: placeholders of %s differ from English' % (lang, k))

    glossary, eponyms = J['glossary'], J['eponyms']
    entries = {lang: [] for lang in LANGS}   # (section, key, value) in order
    spoken_candidates = []                    # keys whose .spoken form is considered

    def add(section, key, values, speak=False):
        for lang in LANGS:
            v = values[lang]
            if v is None or not str(v).strip():
                fail('empty %s value for %s' % (lang, key))
            entries[lang].append((section, key, v))
        if speak:
            spoken_candidates.append(key)

    # Sentences
    for k in en_text_keys:
        if k.startswith('opening.say.'):
            add('Sentences said about the opening', k, {l: texts[l][k] for l in LANGS})

    # Families
    for x in fams:
        base = 'opening.family.' + x['id']
        names = {}
        defs = {}
        for l in LANGS:
            names[l] = override(l, base) or x['names'][l]['name']
            defs[l] = override(l, base + '.def') or family_def(l, x)
        add('Families', base, names, True)
        add('Families', base + '.def', defs, True)
        for lvl in ('beginner', 'advanced'):
            k = base + '.' + lvl
            add('Families', k, {l: texts[l][k] for l in LANGS}, True)

    # Notable variations
    for x in vars_:
        base = 'opening.variation.' + x['id']
        names, defs = {}, {}
        for l in LANGS:
            names[l] = override(l, base) or x['names'][l]
            d = override(l, base + '.def') or variation_def(l, names[l], x['id'])
            if d is None:
                fail('texts/%s.txt: missing %s.def' % (l, base))
            defs[l] = d
        add('Notable variations', base, names, True)
        add('Notable variations', base + '.def', defs, True)
        for lvl in ('beginner', 'advanced'):
            k = base + '.' + lvl
            if k in texts['en']:
                add('Notable variations', k, {l: texts[l][k] for l in LANGS}, True)

    # Composition of other lichess variation names
    compose_types = {}
    for t in COMPOSE_TYPES:
        g = glossary[t]
        s = slug(t)
        compose_types[t] = s
        vals = {'en': '{X} ' + t}
        for l in LANGS[1:]:
            vals[l] = g['compose'][l].replace('{Xgen}', '{X:gen}')
        add('Composition of other variation names', 'opening.compose.' + s, vals)
    components = {}
    for c in FIXED_COMPONENTS:
        g = glossary[c]
        s = slug(c)
        components[c] = s
        vals = {'en': c}
        for l in LANGS[1:]:
            vals[l] = g[l]
        add('Composition of other variation names', 'opening.component.' + s, vals)
    eponym_slugs = {}
    for e, tr in eponyms.items():
        s = slug(e)
        eponym_slugs[e] = s
        nom, gen = {}, {}
        for l in LANGS:
            if l in ('ru', 'uk'):
                nom[l], gen[l] = tr[l]['nom'], tr[l]['gen']
            elif l in ('ar', 'ja', 'zh-Hans', 'zh-Hant'):
                nom[l] = gen[l] = tr[l]
            else:
                nom[l] = gen[l] = EPONYM_LATIN.get(l, {}).get(e, e)
        add('Composition of other variation names', 'opening.eponym.' + s, nom)
        add('Composition of other variation names', 'opening.eponym.' + s + '.gen', gen)

    # Spoken forms: present in every language when one language needs it.
    values = {l: {k: v for _, k, v in entries[l]} for l in LANGS}
    spoken_keys = {}
    for k in spoken_candidates:
        sp = {l: spoken(l, values[l][k], respell) for l in LANGS}
        if any(sp[l] != values[l][k] for l in LANGS):
            spoken_keys[k] = sp

    # Checks: names everywhere, templates only use names that exist, no stray braces.
    for l in LANGS:
        for _, k, v in entries[l]:
            if v.count('{') != v.count('}'):
                fail('%s: unbalanced braces in %s' % (l, k))
    outputs = {}
    for l in LANGS:
        lines = ['# Coach: opening names and what the openings lead to (%s).' % l,
                 '# GENERATED by tools/gen_opening_lang.py from tools/openings_source/: edit those files, then run',
                 '# "python3 tools/gen_opening_lang.py". Key scheme: see the generator.']
        section = None
        for sec, k, v in entries[l]:
            if sec != section:
                lines += ['', '# ---- ' + sec]
                section = sec
            lines.append('%s = %s' % (k, typography(l, v)))
            if k in spoken_keys:
                lines.append('%s.spoken = %s' % (k, typography(l, spoken_keys[k][l])))
        outputs[os.path.join(OUT_LANG, l + '.lang')] = '\n'.join(lines) + '\n'

    # Metadata for the C++ side (no text).
    meta = {
        'source': 'Generated by tools/gen_opening_lang.py from tools/openings_source/families.json (lichess '
                  'chess-openings, CC0, fetched 2026-09-30).',
        'families': [{
            'id': x['id'], 'lichess': x['lichess'], 'side': x['side'], 'tier': x['tier'],
            'generic': bool(x['generic']), 'umbrella': x['umbrella'] or '',
            'answer': {'qgd': 'declined', 'kgd': 'declined', 'qga': 'accepted', 'kga': 'accepted'}.get(x['id'], ''),
            'structure': x['structure'], 'character': x['character']} for x in fams],
        'variations': [{
            'id': x['id'], 'family': x['family'], 'side': x['side'], 'lichess': x['lichess'],
            'beginner': 'beginner' in x['comment']} for x in vars_],
        'ignore_components': J['ignore_components'],
        'compose_types': compose_types,
        'components': components,
        'eponyms': eponym_slugs,
        'common_words': COMMON_WORDS,
        'tts_en_respell': respell,
    }
    outputs[OUT_META] = json.dumps(meta, ensure_ascii=False, indent=1) + '\n'
    return outputs, {l: len(entries[l]) + len(spoken_keys) for l in LANGS}


def main():
    check = '--check' in sys.argv[1:]
    outputs, counts = build()
    stale = []
    for path, text in outputs.items():
        old = None
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                old = f.read()
        if old != text:
            stale.append(path)
            if not check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(text)
    if check:
        for p in stale:
            print('out of date: ' + os.path.relpath(p, ROOT))
        sys.exit(1 if stale else 0)
    print('keys per language: %d; %d file(s) written' % (counts['en'], len(stale)))


if __name__ == '__main__':
    main()
