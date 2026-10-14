# Coach voice: pronunciation notes

The coach speaks through Supertonic 3 (`src/tts`). The voice reads the text it is given and sometimes
misreads it: a silent letter said aloud, a stress on the wrong syllable, a short word swallowed. The
speech catalog fixes this in the spoken text only, the subtitles keeping the normal spelling. This page
keeps what was found per language, so that new lines and translations avoid the same traps.

## How a correction is written

- `key.spoken` in `assets/coach/speech/<code>/*.lang`: what the voice says instead of a line (digits
  and file letters in words, a respelled word in that line only).
- `respell.<word> = <spoken>` in `common.lang`: the word is replaced in every spoken line of the
  language, whole words in any case ("Mat !" keeps its capital). `respell.<word>.pause` replaces it only
  before a pause (punctuation or the end of the line; a hyphen joining the next word is no pause). See
  `src/coach/catalog.h`.
- Opening texts are generated: `tools/gen_opening_lang.py` spells squares with `FILE_WORDS` and
  `RANK_WORDS` and respells words with `SPOKEN_WORDS`; run it again after a change (`--check` tells
  whether the files are up to date). The respell.* keys apply to the opening lines too.
- A Russian or Ukrainian stress mark is the combining acute U+0301 after the vowel ("коня́"): the voice
  follows it. Write it in spoken text only.
- The unit test `coach_catalog_every_respelling_in_sentences` checks every `respell.*` word on the
  lines of its language that hold it (said respelled, the subtitles unchanged), and on a sentence of its
  own while no line holds it.

The quality setting (flow-matching steps) does not change a reading: the text encoder and the duration
predictor decide what is said, the steps only refine the sound. Same misreadings at 5 and 9 steps.

## How to check a reading

- `tools/tts_pronunciation.py --model DIR --probe "fr:six=sisse|sixe|si"` synthesises a form and its
  candidate readings with several noise seeds and ranks the candidates by acoustic distance (DTW on
  log-mel spectrograms): the closest is how the voice reads the form. Listen to the `--wav` files on a
  close call.
- A round trip catches what nobody thought to probe: synthesise the lines, transcribe them with an
  offline speech recogniser and compare. Used here: NVIDIA Parakeet TDT 0.6b v3 (sherpa-onnx, int8) for
  English, French, German, Spanish, Russian and Ukrainian; Whisper medium for Arabic and Japanese. The
  recogniser has its own habits (it writes "e4" for "е четыре", hears homophones, guesses from context):
  a word counts as misread when it fails over several seeds and a respelled variant fixes it.
  sherpa-onnx's Whisper drops the tokens that hold part of a UTF-8 character: most kanji and some
  Arabic letters vanish from its transcripts ("الضغط" comes out "الغط"). Give it a tokens file whose
  tokens are the hex of their bytes and decode the text from hex afterwards.
- Both recognisers come from the `asr-models` release of sherpa-onnx on GitHub
  (`sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8`, `sherpa-onnx-whisper-medium`); `pip install sherpa-onnx`
  runs them. A line with `{best}` or `{move}` is checked with a piece move and a pawn move in its place
  ("rook a3", "e4"): a pawn move puts a lone file letter after the word before it.
- Stress (Russian, Ukrainian): synthesise the plain word and one variant per vowel with U+0301 after
  it; the variant closest to the plain rendering is where the voice puts the stress. Compare with a
  dictionary (espeak-ng's is wrong for many Ukrainian words; check the doubtful ones by hand).
- A final consonant said or not (French "coup", "six"): count the stop closures (silences of 20 ms or
  more, 30 dB under the peak) over many seeds, against a control word without the consonant ("cou").

## French

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| six, dix (before a pause) | "sixe", "dixe" (with a k) | `sisse`, `disse` (respell.*.pause) |
| huit (before a pause) | "hui" | `huite` (respell.huit.pause) |
| a square's 6 and 8 (e6, a8) | "sixe", "hui", even before a word | `sisse`, `huite` (rank.*.spoken) |
| dix-sept, dix-huit, dix-neuf (and 77-79, 97-99) | "dixe-sept", "dixe-neuf" | `di-sept`, `dize-huit`, `dize-neuf` |
| dix-neuvième (opening texts) | "dixe-neuvième" | `dize-neuvième` (SPOKEN_WORDS) |
| coup | sometimes "coupe" before a vowel ("au coup un") | `cou` |
| mat, pat | "mât", "pas" | `matte`, `patte` |
| the file letter e | too short ("c'est e quatre" heard "cé quatre"); "eu" read like "u" | `euh` |

"six" and "dix" before the noun they count are fine ("six coups", "dix erreurs"), as is "huit coups"
(silent t). Checked and fine: "sept" (no p). A round trip over 300 lines found nothing else, homophones
aside (cent/sans, roque/roc, ma dame/madame).

"J’aurais peut-être joué {best}" lost "être" in 2 renderings of 6 ("peut-joué", the spoken "p'têt");
"Peut-être fallait-il jouer {best}" came through 8 times in 8 (ex.verdict.inaccuracy.maybe.2). Seen on
the way, left as is: "J’aurais joué tour a trois" (ex.verdict.inaccuracy.b5.2) was heard without "joué"
twice in 4.

## German

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| Remis | "Remies" (with the s) | `remie` |
| en passant | "ein Passant" | `ang passang` (respell.en, respell.passant) |
| Fianchetto (and fianchettieren...) | "Fiantschetto" | `fianketto`, `fiankettieren`... |
| Pirc | "Pirk" | `pirz` |

Round trip over 800 lines. Unclear in the checks and left as they are: Rochade, rochieren, Giuoco Piano.

Word order around a move: "Ich hätte vielleicht {best} gespielt" lost a word in 4 renderings of 6 ("Ich
hätte vielleicht gespielt", "Ich hätte Turm a drei gespielt"); "Vielleicht hätte ich {best} gespielt"
came through 7 times in 8, once heard "je vier" (ex.verdict.inaccuracy.maybe.2). After "war", a pawn
move's "e" melts: "Vielleicht war e vier" was heard "Vielleicht war ich hier" 3 times in 4, so that line
keeps the move first ("{best} war vielleicht etwas genauer"). Seen on the way, left as is: "Ich hätte
Turm a drei gespielt" (ex.verdict.inaccuracy.b5.2) lost "Turm" 3 times in 4.

## Russian

- The file letter "е" alone is read "и" ("ход и пять"): squares say `йе`, as the letter sounds
  (file.e.spoken, FILE_WORDS).
- Nouns whose stress moves to the ending get it on the stem ("ко́ня", "сло́ном", "ла́дья", "хо́дов"):
  respelled with a stress mark, one respell.* entry per form (коня́, слона́, слоно́м, ладья́, ладье́, ферзя́,
  ходы́, ходо́в, фигу́р, ничья́, доско́й, выи́грывает...: see `common.lang`). A new form of these nouns
  needs its own entry. Homographs stay unmarked (ладьи, хода, ходу, доски, стоит).
- Always write ё: the voice reads "е" as written (the catalog uses ё throughout: чёрные, ещё, конём).
- "даром" is a close call between "да́ром" and "даро́м" (probe, alone and in "можно было взять даром"):
  marked `да́ром`.
- Inherent to the notation, left as is: "а пять" sounds like "опять", "е два" like "едва".
- Round trip over 1,000 lines; the stress of the 140 most frequent words and of the chess nouns compared
  with a dictionary.
- "угрозу" / "угроза" (ex.verdict.ignored) are stressed on the о, as written (probe, 3 seeds).
- "пожалуй" is a close call: alone the voice leans to пожа́луй, in "Пожалуй, я бы сыграл" to по́жалуй
  (probe, 8 seeds): marked `пожа́луй`, the dictionary stress. "возможно" (возмо́жно) and "точнее"
  (точне́е) are read as the dictionary has them (probe, 4 seeds).

## Ukrainian

- The file letter "е" alone is swallowed ("на чотири", "пішака чотири"): squares say `е́` with a
  stress mark (file.e.spoken, FILE_WORDS).
- Mobile stress on the stem, as in Russian: коня́, коне́й, слона́, слоно́ві, слоно́м, слони́, слоні́в,
  пішака́, пішаки́, пішакі́в, пішака́ми, тура́, ферзя́, ферзі́в, королю́, ходи́, хода́ми, нічию́. Round trip
  over 1,000 lines.
- "ферзем" unmarked is read like the Russian "ферзём": `ферзе́м`. Words shared with Russian are the
  ones to check first.
- Adverbs too: "задарма" is read "зада́рма" (probe, 4 seeds), the dictionary stress is `задарма́`.
- "загрозу" alone is a close call between загро́зу and загрозу́ (probe, 6 seeds); in "на мою загрозу"
  the voice says загро́зу, as the dictionary does: left unmarked (ex.hanging_threat.b3 already said it).
- "мабуть" is read мабу́ть, alone and in "Мабуть, я б зіграв так" (probe, 8 seeds); the dictionary
  stress is `ма́буть`. "можливо" (можли́во) and "трохи" (тро́хи) are read as the dictionary has them.

## Arabic

- The file letter "a" is `أيه` ("ay"): "إيه" is read "īh", hardly apart from "إي" ("e"); the
  recogniser took "بـ إيه ستة" for e6 four times in six, never with "أيه" (file.a.spoken,
  FILE_WORDS).
- Unvowelled words are read with their usual vowels (الملك as al-malik, نقلة, الوزير, شوكة); the
  catalog sets a diacritic only where a reading is ambiguous (رخّ, ملِكي). Round trip over 100 lines
  (Whisper): nothing else misread. The file letter "e" can melt into a word ending in "ī" before it
  ("في إي ثلاثة" heard "في ثلاثة" twice in six), as in Spanish.
- "أخذ" as a verbal noun ("فرصة أخذ", a chance to take) can be read as the verb "أَخَذَ": the probe
  could not tell the two apart. A line that means "a capture" uses another noun ("مكسب مجاني"), or the
  article ("الأخذ"), which no verb takes.
- The recogniser writes no short vowels, so a vowel misreading only shows in a direct comparison of
  vowelled spellings (`--probe "ar:الملك=المَلِك|المَلَك|المُلك"`); a candidate that adds only one
  diacritic is the fairest comparison, full vocalisation changes the delivery.
- "تهديدي" (my threat, ex.verdict.ignored) is read tahdīdī (probe against تُهْدِيدِي and تَهَدُّدِي).
- "ربما" (perhaps, ex.verdict.inaccuracy.maybe) is read rubbamā (probe against رُبما, رَبما and رِبما,
  4 seeds).

## Japanese

- Kana for everything the voice must not guess (file letters, numbers, counters): already the rule of
  `common.lang`. Checked over 160 lines that cover the most frequent kanji compounds; the rest read right.
- 代わり is read "たわり" in every try: `かわり`.
- 一局指し: the voice voices 指し right after 一局 ("いっきょくざし"): `一局さし`. With を ("一局を指し")
  it is fine.
- 一マス / 二マス: say `ひとマス` / `ふたマス` (count.square.spoken does).
- 狙い (the coach's threat, ex.verdict.ignored) is read ねらい, alone and in 私の狙い (probe, 3 seeds).
- を指した is read さした (probe against ざした and ゆびさした, 4 seeds).
- respell.* cannot match inside a Japanese sentence (no spaces between words): a misread word needs a
  `.spoken` line in each catalog line that uses it, and an entry in SPOKEN_WORDS for the opening texts.

## Spanish

- Checked over 300 lines: no word misread, apart from the file letter "e" after a word ending in a
  vowel, which melts into it ("la casilla e cinco", "sobre e cinco" heard without the e). "é", "ee",
  "eh" and "e-e" did no clearly better, so it stays "e". After a consonant it is heard ("en e siete"):
  prefer that in new lines.
- ex.verdict.inaccuracy.maybe puts the move after a consonant ("Quizás {best}", "por {best}"): the
  pawn move's "e" came through every time (round trip, 3 seeds).

## English

- "en passant" as written is said "in passant": `ahn pahssahn` (respell.en, respell.passant), the
  pronunciation dictionaries give. Otherwise clean over 250 lines.
- "Perhaps {best} was ..." lost the piece name in 2 renderings of 3 ("Perhaps A three"): the line starts
  with the move instead ("{best} may have been a little more precise", 8 times in 8).
