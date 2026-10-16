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
  before a pause (punctuation or the end of the line; a hyphen joining the next word is no pause). The
  spoken form is one word too (no space or hyphen; the unit test below counts it as one): a number said
  with hyphens goes in `number.<n>.spoken` instead (Danish `to-og-tyve`). See `src/coach/catalog.h`.
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

## European Portuguese

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| the file letter e (squares) | swallowed after "em", "para", "era": "toma em é cinco" said "toma em cinco" | `éh` (file.e.spoken) |
| xeque-mate, xeques, xadrez | an initial x read "s" ("seque-mate", "sékes", "sadrez"); "xeque" alone right | `cheque`, `cheques`, `chadrez` (respell.*; "xeque" in "xeque-mate" is a word of its own) |
| en passant | "em passante" (with the t) | `ã passã` (respell.en, respell.passant) |
| troco (a troco de nada) | "tróco", the open o of the verb | `trôco` (respell.troco: every line has the noun) |
| Deste (you gave) starting a sentence | "dêste", the demonstrative | `Déste` (.spoken of appraisal.improve.coach_hung.b3, lesson.safety.gave) |
| Najdorf, Nimzowitsch, Winawer, Bogoljubow, Tartakower, Janowski, Horwitz | w as in English, j as in "jogo": "Najedorf", "Nimzouitch", "Uinauer" | `Naidorf`, `Nimzóvitch`, `Vináver`, `Bogoliúbov`, `Tartakóver`, `Ianóvski`, `Hórvitz` |
| Fajarowicz | "Fajaro X", "Fajarwix" | `faiaróvitch` (found in the Brazilian pass, same voice) |

- Round trip over 750 lines (Parakeet, seed 0), then 3 seeds on every word missed in more than a third of
  its uses, the reviewers' list, the file letters, the numbers and the opening names. The file letter: "é"
  came through 55 times in 111 in hard positions, "éh" 82 times; "éé" did as well as "éh" on the first 42
  but was sometimes heard as two vowels ("é a"); "á" after a vowel is heard "a" ("á", "áh", "áá" alike): left as is. The opening texts
  take file.e.spoken when tools/gen_opening_lang.py runs again.
- Checked and fine (probe, 4-8 seeds): acordo, esforço, cedo, este (closed), gambito (stress on bi),
  fianqueto, ala (the dark l makes it sound like "aula" to the recogniser), ao ([aw], not "o"), saem, sei,
  tens and the other 2nd-person -s (said; see below), Hum (said "um"), Elo, blitz, éle, á/agá said alone,
  Philidor ("Filidôr"), Evans, Fischer, Marshall, Trompowsky, Scheveningen, Sämisch ("Sémisch").
- Left as is: the verb "jogo" (eu jogo) is said with the closed o of the noun (probe, 6 seeds, two
  contexts); "jógo" gives the open o, but the recogniser then hears "joga" 11 times in 21 against 4, so the
  person could be misheard. "começo" (eu começo), "doze", "erro", "torre" are close calls between the open
  and the closed vowel, changing with the context. "Ups!" is said "óps" (probe): the same interjection.
- Recogniser habits: it writes Brazilian forms (tem for tens, pode for podes, suas for tuas, seu for teu)
  while the -s and the t are said (probe: "tens" against "tem"); "é cinco" as "S5", "roque" as "rock",
  "ala" as "aula"; it drops a line's first word when it is an interjection or a short title ("Olá!",
  "Xeque-mate!"), and it cannot write most eponyms, nor short lines that end in one (it switches to English
  or Polish).

## Brazilian Portuguese

The European Portuguese voice reads the Brazilian text (language `pt`): only misreadings are respelled, a
European vowel is left as it is. Most findings of the European Portuguese pass hold here too.

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| the file letter e (squares) | swallowed after "em", "para", "até": "toma em é cinco" said "toma em cinco" | `éh` (file.e.spoken): heard 36 times in 54 against 24 |
| até {sq} | the letter melts into "até", even as "éh": "até quatro", "até a quatro" (right 2 times in 9) | `até a casa {sq}` (.spoken of lesson.first.ask, ex.promotion.b1/b2, ex.promotion_missed.*, ex.push_passed.*): 6 in 9 |
| xeque-mate, um xeque, xeques, xadrez | an initial x read "s" ("sec mate", "sex", "sadrez"); "xeque" alone mostly right | `cheque`, `cheques`, `chadrez` (respell.*): 37 times in 78 heard with "ch", 61 respelled; xadrez 2 in 12, 11 respelled |
| en passant | "em passante", with the t (8 times in 15) | `ã passã` (respell.en, respell.passant): no t in 15 |
| troco (a troco de nada), torno (em torno de) | the open o of the verb: "a troca de nada" 6 times in 6; probe "tórno" in two contexts | `trôco` ("troco" 4 in 6), `tôrno` (respell.*: every use is the noun) |
| para, the verb (lesson.knight.l, lesson.ep.watch, an.end.unfinished, .2) | the preposition "pra" (probe, 3 contexts): "ele sempre pode", "parabéns ao lado" | `pára` (.spoken): heard "para" 9 times in 12 against 4 |
| força, the verb (an.brilliant.mate.2) | the noun "fôrça" (probe, 6 seeds) | `fórça` (.spoken) |
| Tente starting lesson.values.trade | "A gente nunca entregar" (5 times in 7) | `Tênte` (.spoken): right 5 in 7 |
| Najdorf, Nimzowitsch, Winawer, Bogoljubow, Tartakower, Janowski, Horwitz, Fajarowicz | w as in English, j as in "jogo": "Nash-dorf", "Nimzo-witch", "Tarta-call", "Fajaro-wix" | `Naidorf`, `Nimzóvitch`, `Vináver`, `Bogoliúbov`, `Tartakóver`, `Ianóvski`, `Hórvitz`, `Faiaróvitch` |

- Round trip over 720 lines (Parakeet, seed 0), then 3 seeds on the words missed in more than a third of
  their uses, the reviewers' list, the file letters, the numbers, the opening names, and every fix; probes
  (4-6 seeds) for the homographs. The opening texts take `éh` from file.e.spoken ("o peão éh", no longer
  the verb); "chegar a éh quatro" is heard 5 times in 6.
- Checked and fine: rating (said "rêitin", probe), Elo, blitz, Philidor ("Filidor"), éle, á/agá said alone,
  "letra é", Uau, Opa, the numbers (quatorze, dezesseis, dezessete, dezenove, vinte e uma, cinquenta...
  written as the right digits, 3 seeds), pode (open o), sobre, the verb jogo in "jogo com calma" (open o,
  probe), Meran, Leningrado, Iugoslavo, Moscou.
- Left as is: close calls between the open and the closed vowel, changing with the context (probe): "eu
  tomo", "eu começo", "olho" (open alone and in "Fique de olho", a tie in longer contexts), erros, "cerca de" (no candidate close;
  "cerca" alone is "cêrca"). "Tente" elsewhere: "Tênte" did no better over 36 renderings (9 against 9).
  "onze" is read "ônze" (probe) but heard only 5 times in 12 ("ônze" 8, the same reading). Jaenisch
  ("Jaenich", j of "jogo") and Euwe ("Eu-ue"): no settled Brazilian reading to respell to.
- Recogniser habits: as in European Portuguese ("ala" as "aula", "roque" as "rock", "mate" as "match",
  short lines and interjections such as "Hum" dropped, eponyms unwritable), plus the reduced final e of
  Brazilian imperatives and subjunctives written as other forms ("Pense" as "pensa", "avance" as "avança", "Tente" as
  "sente" or "tem"), "peão" as "pião" or "pão", "doze" as "dose", "Este é" as "Eu sou".

## Danish

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| remis | with the s: "remiss" (probe), heard "ramis", "remiss", "remise" 9 times in 9 | `remi` (respell.remis): no s in 9 ("Remy", "rem i") |
| en passant | "en pasant", with the t (heard "pasant", "patient") | `passang` (respell.passant): heard "passang", "passering" 8 times in 9; "en" stays (the article: "ang passang" heard no better, 6 in 9 against 8) |
| fianchetto, fianchettoen, fianchetterer, ... | "fiantjetto" (probe), heard "fjernshadow", "fjernsjættere" | `fianketto`, `fianketterer`... (respell.*, one per form): heard "fianchetto" 2 times in 6 against 0 |
| Grand Prix | the x said: "grand priks" (probe), heard "pricks" | `pri` (respell.prix): "Grand Prix" 2 in 3 against 0 |
| epauletmat | the "au" said, heard "i Paulemat" | `epoletmat` (respell): "epolemat", "jepolet", no "au" in 6 |
| ratede | a vowel too many, heard "rejattede" 4 times in 6 | `rejtede` (respell): "rejtede" 4 in 6 |
| elleve | three syllables, as written (probe); never heard as 11 in 6 | `elve` (respell): 11 three times in 6 |
| 21, 31 ... 91; 22, 32 ... 92 | "enog", "toog" run together: 71 heard "81", "endnu halvfjerds"; 92 "2,5" | `en-og-halvfjerds` (number.X1.spoken), `to-og-halvfems` (number.X2.spoken): 71 three in 3 against 0, 32 two in 3 against 0, 92 five in 6 against 2; 52 the same either way, 86 worse with hyphens, so other units stay joined |

- Parakeet could not do Danish: its language detection wrote English or German (or nothing) for 592 of the 750
  lines of the first round trip. Redone with Whisper medium set to `da`: 219 lines covering 1,120 words (seed
  0; the second chunk stopped at the time limit), then 3 seeds on 103 lines (the reviewers' list, file
  letters, numbers, eponyms, every fix), and probes. Even then 35% of the words come back wrong: the voice's
  Danish is soft and the recogniser guesses.
- Checked and fine: rating (heard "rating" 5 in 6, once "raiding"; `rejting` heard worse), Elo, `æl` for the L
  (heard "el" 3 in 3), `ét` (long e, probe), `Én` starting a sentence, be, se (B5, C3 every time in a
  sentence), hå (H3, H5 mostly), dé (heard "de", the letter's name), Wow, Ups, Hmm (said, probe), 13, 17, 19,
  86, 103, "ét komma fem", the v and d of halvtreds, halvfjerds, halvfems silent (probe), sort ("sårt"),
  Philidors ("Filidors"), Caro-Kann, Tarrasch ("tarrasj"), Nimzowitsch ("nimzovitsj"), Sämisch, Schliemann
  ("sjliman"), Morphy, Fischer, Ponziani, Horwitz, Winawer, Trompowsky, Stonewall (heard right), Bongcloud,
  Halloween, Breyer, Colle, Torre, Grünfeld ("grynfeld"), Aljechins, Fajarowicz, Marshall.
- Left as is: the file letter é is heard "E" about half the time, "i" otherwise; `ee` and `eh` did no better
  (`eh` heard as "F"). The letter a is heard as A once in about 20 ("at", "af", "R", or lost): `ah` and `á`
  did no better; the recogniser takes Danish A and R for each other. "ge to" was heard "K2" 3 times in 3 ("ge
  tre" is G3). "Fair nok": `Fær nok` heard worse (0 in 6 against 1). Miles is read "mi-les" (probe), but
  `Majls` was heard the same. "remisen", "remisagtige", "remisvåben" keep the s (`remien` heard worse). Close
  calls, left: Giuoco ("gjuoko"), Owens, Evans, Birds, Parham (read the Danish way), Zukertort ("sukertort"),
  Scheveningen ("sh"), Av; the 't of Van 't Kruijs is swallowed.
- Recogniser habits: hvid as "ved" (the voice says "vid": probe), bonden as "bånden", mat as "mad", ad as
  "af", felter as "fælder", én and ét written "en", "et"; it drops "Hmm"; it often writes the numbers from 50
  up as pieces ("enoghalvfjerds" heard "en halv fjerts") though the v and d are silent as they should be (probe).

## Korean

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| 퀸과 (the queen and/with) | "큉과": the final ㄴ turns into ㅇ before the ㄱ, and the recogniser hears 킹과 (the king) 10 times in 16 (퀸 4 times) | `퀸하고` (respell.퀸과, the same "and/with"): 퀸 15 times in 24, 킹 once |
| 퀸까지 (as far as the queen) | "큉까지": 킹까지 8 times in 13 (퀸 3 times) | `퀸에까지` (respell.퀸까지): 퀸 9 times in 15, never 킹 |
| 퀸 근처 (ex.fork.tip.knight) | the same across the space: 킹 근처 3 times in 3 | `퀸의 근처` (.spoken): 퀸 2 times in 3 |

- Round trip over 300 lines (Whisper medium, seed 0), then 3 seeds on about 110 lines: the reviewers'
  list, the file letters, the numbers, the opening names and eponyms and the words missed most, and
  `tools/tts_pronunciation.py --probe` (4-5 seeds) on the doubtful ones. The voice turns a final ㄴ into
  ㅇ before ㄱ ㄲ ㅋ, which standard Korean does not do (퀸과 is [퀸과]). A space does not stop it ("퀸 과",
  "퀸 까지": 킹 7 times in 15). Only 퀸 becomes another piece. "폰과" is said "퐁과" (heard 펑과, 폼과) and
  레닌그라드 is said 레닝그라드: both left as they are. In a new line, avoid 퀸 right before ㄱ ㄲ ㅋ ("이른
  퀸 공격" came through 3 times in 3, "퀸 근처" never).
- Checked and fine: 흑은 [흐근], 백은 [배근], 백의 [배게], 갬빗 (= 겜빗), 피앙케토 (= 피앙캐토, not
  피앙차토), 이른 (not 이런), 수째 (tense, not 수제), 십구 [십꾸], 견고한, 기보지 (not 기본지), 중앙,
  체스, 동형 (with the ㅎ weakened, as Korean speakers say it), 엘로 3 times in 3, 정돈합니다, 오 플러스 삼 3 times in 3,
  칠십오 수 and 오십 수 6 times in 6, 백 점 (heard 100점), 예순네 칸, 최선수, 더블 체크, 칠 랭크, 위나워 (not
  위나와), 나이도르프 (not 나이돌프), 보트비닉 3 times in 3. Also the file letters (에이 비 씨 디 이 에프 지 에이치: after
  a pause 디 is [ti], as with other Korean lax stops) and the hyphen of 블랙마-디머 and 님조-라르센 (a short pause). 엘자 is said
  [엘짜] (probe); written `엘짜` it came through 2 times in 3, against 1 time in 3, too few to change it.
- Left as is: 스테일메이트 is heard 스틸, 스테이 or 스텔 메이트 (right 1 time in 17). The probe puts it near
  스테이메이트 (a weak l, as in 마일스, heard 마이스), and "스테일 메이트" with a space did no better (0 in 9).
  폰 is heard 푼, 톤 or 펀 about two times in five, but the probe has 폰을 = 포늘, far from 푼을 and 펀을.
  두셨어요 was heard 드셨어요 ("you ate") or 주셨어요 5 times in 5. The probe cannot tell ㅜ from ㅡ there (nor in
  주셨어요), and there is no other spelling. 브레이어 at the start of a sentence was heard with a ㄱ (그래요,
  그레이어) 4 times in 4. The probe cannot tell it from 그레이어. 칠십오 is said [칠시보], not [칠씨보]. "나이트 에프
  칠" was heard "나이트의 부츠" 2 times in 3: the f melts into the vowel before it, as e does in Spanish.
- Recogniser habits: it writes 예요 as 에요 (and 무승부예요 as 무승부해요), 백의 as 배개 or 되게, 흑은 as
  혹은 or 흙은, and 기물을 as 기무를 (its sound). It also writes 씨 as 시, 갬빗 as 겐빗, 무승부 as 무슨 부, and 스큐어 as
  스퀘어 (`스큐우어` gave 스퀴어). It writes squares and numbers as notation ("e4", "100점"). The first word of a line is
  often lost or replaced (흑은 and 백은 are heard 이건, 하겐, 되게). It cannot write most eponyms (님조비치 as 닌조비치, 그륀펠트 as
  그린펠트). It turns short lines of names into English or into a caption ("Smart AdMate", "(끝)"). It cuts lines of
  more than about 50 syllables short.

## Italian

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| fianchetto, fianchetti | the c of "cena": heard "fiancetto" 11 times in 21, right 7 | `fianketto`, `fianketti` (respell.*): right 16 in 21, never "fiancetto" |
| en passant | Italian, with the t: heard "in passante", a t 19 times in 21 | `an passàn` (respell.en, respell.passant): a t 6 times in 21 |
| lettone (gambetto lettone) | stressed on the first syllable, "lèttone" (probe): heard "letto" 7 times in 9 | `lettóne` (respell): right 8 in 9 |
| settima (the seventh rank) | the stress leaves the first syllable: heard "settimana" 6 times in 9 | `sèttima` (respell): right 9 in 9 |
| Pirc | "pirch" (probe; heard "Pirce", "Peach") | `Pirts` (respell): heard "Piers", "Piez" |
| Grand Prix | the d and the x said ("Grand Priks", probe; heard "freaks", "breaks", the line taken for English 3 times in 3) | `Gran Prì` (respell): no x in 6, the line heard in Italian 2 in 3 |
| clou | "clo-u" (probe; heard "Clau" 3 times in 3) | `clu` (respell): heard "clou", "clú" |
| dalla a all’acca (lesson.welcome.files) | the letter a melts into "dalla": "dalla lacca" 6 times in 6 | `dalla a, fino all’acca` (.spoken): a heard 2 in 3 |

- Round trip over 750 lines (Parakeet, seed 0; 19% of words missed), then 3 seeds on 205 short segments
  covering every word missed in more than a third of its uses (splits, joins and garbled transcripts set
  aside), the reviewers' list, the file letters, the numbers and the opening names in carrier sentences
  ("Hai aperto con la difesa ..."); probes (3-6 seeds) for the names, the homographs and the stress.
- Checked and fine: the numbers (29 test lines in 30 written as the right digits: undici to novanta,
  ventitré, quarantotto, per cento), Philidor ("Filidor"), Elo, blitz, rapid ("ràpid"), robot ("ròbot"),
  acconcio, di’ ("dì"), ancora (ancóra), capitano (the verb), Mmm (hummed, not spelled), Ops, Ahi, Caspita,
  elle, patta, alfiere, sull’ala, gambetto (probe: none read as the recogniser wrote them), the stress of
  the frequent proparoxytones (muovere, prendere, ultima, pratica, debole...), Najdorf ("Naidorf"), Réti,
  Morphy, Fischer, Sämisch, Tarrasch, Marshall, Sveshnikov (sh), Max Lange (hard g), Chigorin (heard
  "Cigorin" as written: `Cigorin` heard no better), Maróczy, Kieseritzky, Steinitz, Rubinstein, Tartakower,
  Schlechter, Zaitsev, Rossolimo, Halloween.
- Left as is: the king "re" is heard badly (0 in 18 with Parakeet, 3 in 15 with Whisper in six lines): it
  runs into the word before ("tuo re" heard "tuore", "tour"), but it is said (probe), and "Re", "rre",
  "rè" did no better. The file letters e and a melt into a vowel before them ("da e quattro" heard "dai
  quattro", "la casa a sei" "la casa 6"); "é", "eh", "à", "ah" did no better (e 9 times in 27 in such
  lines, "é" 6, "eh" 9): as in Spanish, prefer a consonant before a square in new lines ("in e quattro").
  "subito" is "sùbito" alone, a close call in "devi uscirne subito" (probe). Alekhine keeps its final e
  ("Alekìne", stress right; without the e the stress moved to the first syllable). Close calls or no
  settled Italian reading: Scheveningen (sh), Schliemann, Nimzowitsch (about "Nimzovic"), McCutcheon, Euwe
  ("E-u-e"), Jaenisch, Jobava, Winawer, Bogoljubow, Breyer, Stonewall, Fajarowicz; Miles is "mi-les", but
  `Mails` was heard no better.
- Recogniser habits: it writes the verb forms its language model prefers ("guadagno di materiale" as
  "guadagna", "catturo" as "cattur"; probe: the o is said), merges vowels across words ("Bianco ottiene"
  as "Bianco tiene", "Stavolta arrocca" as "rocca", "di re" as "dire"), splits compounds ("campo chiaro",
  "in chioda", "per mettere"), hears "patta" as "fatta", "acca otto" as "K8", "alfiere" as "alpiere", and
  takes short lines, or a line that starts with an interjection or a foreign name, for another language
  ("Gran bel salto!", "Ahi, ..." came out Finnish, Dutch or English).

## Hindi

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| ग़लती, ग़लतियाँ, ग़लतियों, ग़लत | the nukta ग़ as a weak fricative that melts after a vowel or a nasal: "गंभीर ग़लती" heard "गम भी रलालती", "गम ही रहती" (the g heard 9 times in 18, in five contexts) | `गलती`, `गलतियाँ`, `गलतियों`, `गलत` (respell.*), as most Hindi speakers say them: the g heard 17 times in 18, "गंभीर गलती" 3 times in 3 against 0 |

- The main round trip (300 lines, Whisper medium) is unusable as it stands: sherpa-onnx's Whisper allows about 6 tokens
  per second of audio, and Hindi takes several byte tokens per word, so every transcript stops after a few words.
  The audio padded with silence to 30 s lifts the limit (Whisper pads to 30 s anyway). Redone that way: 114 short lines,
  3 seeds (file letters, squares, numbers 11-99, ordinals, decimals, the reviewers' list, the opening names and
  eponyms, the words missed most), 25 lines of A/B variants, then probes (4 seeds). Spelling differences (nukta,
  ँ/ं, ॉ, vowel length, ह dropped) were counted as heard right.
- Checked and fine: the number words (29 to 99, 21-58, the -हत्तर family: पचहत्तर is said "pach-hattar", probe),
  the ordinals, "दशमलव", "माइनस", "प्लस", the letters (एल, ई, जी, सी, ए, एच as "ech"), खड़ी/घोड़ा (ड़ said), schwa
  deletion (हमला heard "हम्ला", ब्लिट्ज़ and पास्ड end without a vowel, probe), पिर्क (one syllable, the k said),
  पानोव and बोगोल्युबोव (the v said), नाइडोर्फ़, निम्ज़ोविच, पोंज़ियानी, ब्लैकमार-डीमर, कापाब्लांका, फ़ायारोविच,
  कंगारू, इतालवी, ट्रांसपोज़िशन, हाइपरमॉडर्न, सी-फ़ाइल. मोहरा is said "muhra", a common pronunciation.
- Left as is: ऊँट, ऊंट and उँट are heard alike ("उंट", "ओट", "आउट", right about half the time); ओपनिंग is heard
  "अपनिंग"/"आपनिंग" nearly every time, but once written "opening" in an English transcript, and "अोपनिंग" did no
  better (nor "अोर" for ओर, heard "और"); the probe cannot tell ओ from औ or ऊ. एफ़ is heard "एप" in "घोड़ा एफ़ सात"
  but "F3" in "चाल थी: घोड़ा एफ़ तीन": "ऐफ़" and "एफ़्फ़" did no better; "ऐच" no better than एच. "ए छह" is heard
  "इच्छा" whatever the spelling of the six (छह, छः, छे). ख़ूब (heard "कूप") and बग़ल were heard no better without the
  nukta. उफ़ at the start of a line is dropped (उफ़्फ़ heard once in 3). The v of -व्स्की (यानोव्स्की,
  ट्रॉम्पोव्स्की) is weak, as in the Polish -wski. "एल आज़माओ" runs together ("है लाजमाओ"), a liaison.
- Recogniser habits: it writes the frequent word for a letter or a number (बी as "भी", डी as "दी", सात as "साथ",
  पहली as "पहले", उनसठ as "उनसे"), the retroflex ट/ड often as dental, ई as इ, final stops with the wrong voicing
  ("तुरंद", "सफेथ", "मजबूट"), squares as notation ("F3", "D5"); it drops or garbles a line's first word and short
  lines of English words (मिडिलगेम, स्टेलमेट, स्क्यूअर), cannot write most eponyms, and loops ("आपका आपका ...") on
  some lines.

## Bulgarian

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| the file letter a (squares) | melts into the word before it: "на а три" heard "на три" (4 times in 12 heard as A) | `а́` (file.a.spoken): heard A 8 times in 12, and twice more as "ная" |
| дъската | "дъ́ската" (probe, 3 contexts), heard "диската" 7 times in 9 | `дъска́та` (respell): heard "диската" 3 times in 9 |
| ан пасан | "па́сан" (probe, alone and in "се нарича ан пасан") | `паса́н` (respell.пасан) |
| пропусна, Изпусна, отдръпна (aorist, "you missed") | the present: "пропу́сна", "изпу́сна", "отдръ́пна" (probe, alone and in "Тук пропусна шанс", "Пропусна мат", "Изпусна от поглед") | `пропусна́` (respell); `Изпусна́`, `отдръпна́` (.spoken of ex.verdict.ignored.b1.3, praise.saved.b1.2) |
| Върна (aorist, appraisal.num.takebacks.*) | the present: "Въ́рна един ход" (probe) | `Върна́` (.spoken) |
| права линия (lesson.rook.intro) | "права́" (feminine of "прав", right) instead of "пра́ва" (straight) | `пра́ва` (.spoken) |
| царя | "царя́" in "Пази царя си", "шах на царя" (probe); царят, конят, коня are right | `ца́ря` (respell) |
| полета | "поле́та" (probe, alone, "две полета направо", "шестдесет и четири полета") | `полета́` (respell) |
| единадесет ... деветнадесет | stressed "-наде́сет" (probe, every teen); heard "едно на десет" | `едина́десет` ... `деветна́десет` (respell.*): heard as 16 to 19 8 times in 24 (3 of them as whole numbers), never with the plain spelling |
| Мароци | "Маро́ци" (probe) | `ма́роци` (respell) |

- Bulgarian writes no stress, and the voice guesses it: the stress of the 120 most frequent words (and of the
  reviewers' homographs) was compared over every vowel with U+0301, which the voice follows (вода́ against
  во́да), then in a phrase where a word came out wrong alone. Parakeet could not do Bulgarian (English or
  nothing for most lines); the round trip is Whisper medium set to `bg`: 300 lines (seed 0), then 3 seeds on
  about 50 lines (fixes against the plain text, the file letters, the numbers) and 1 seed on 84 more (the
  reviewers' list, 27 opening names, 30 eponyms in carrier sentences).
- Checked and fine (probe): реми (реми́), Филидор in "защитата на Филидор" (Филидо́р), Следи, води, търси, удари
  (both stresses as close), подред, отказа (отка́за, the aorist as the dictionary has it), затова, срещу,
  добър, център, ендшпил, дебют, гамбит, вилица, позиция, атака, черните, царят, конят, коня, губиш, опитай,
  груба, седем, осем, Бенони, Оуен. Heard right (round trip): реми, Опа, Леле, Ох, Хм (mostly), луфт,
  кандидат-ходове, буквата гъ ("га"), от а до аш, the eponyms and opening names that Whisper can write.
- Left as is: the file letter е after "ни" or "е" melts into the vowel ("Трябва ни е четири" heard "ни 4");
  `е́` fixes that (3 in 3) but after "на", the commonest context, it is heard "я" (4 in 6 against 0), so it stays
  "е". "ще върна" (present, ex.rewind) is a close call (probe), and `въ́рна` was heard "върнеш", "върнем": left.
  еф after a vowel is heard "в" ("напада еф седем" as "нападе в седем"); `е́ф` did no better. фианкето is
  said "фиа́нкето" (probe), but `фианке́то` was heard worse ("ФНК ето", "хиянкия": 1 good in 6 against 4).
  двадесет leans to "двадесе́т" alone, a tie in "ход двадесет"; четири leans to "четири́" alone, a tie in "на е
  четири"; рапид is "ра́пид" (the English word's stress); Коле is "Ко́ле", Торе a reduced "Тора", Ело a tie.
  The opening texts take `а́` when tools/gen_opening_lang.py runs again (squares come from file.*.spoken).
- Whisper hears a final stressed а as "ая" or "ай" (`пропусна́` as "пропусная" 2 times in 3, `Върна́` once as
  "Върнай"), though the probe puts `пропусна́` as close to "пропусна" as to "пропусная": kept for the grammar.
- Recogniser habits: it writes squares as notation (б5, ц3, D4, Ж8, "аж 8" for аш осем), teens as "N на 10",
  "единайсет"-style colloquial numbers only, unstressed ъ as "а" or "е" (дъската as "даската"), and Russian
  spellings (сицилиянската, классически, "черни те"); it writes eponyms in English or Latin letters ("Gambit
  England", "Robinshine", "Morphy") and drops the a of a name after "защита" ("защита Лехини").
