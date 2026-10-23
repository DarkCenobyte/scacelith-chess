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

## Swedish

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| en passant | with the t: heard "en patient", "en procent", "passent" 6 times in 9 | `passang` (respell.passant): no t in 9, "en passang" 3 in 3; "en" stays (the article) |
| 27, 37 ... 97 (tjugosju, fyrtiosju ...) | the s of "sju" goes to the tens: "fyrtios ju" (probe), heard "trettioås u", "sextiosju" as 60-80; the right number 0 times in 12 | `fyrtio sju`... (number.X7.spoken, with a space): right 4 in 12, the sj said in most of the rest ("sker", "skik"); a hyphen did no better (0 in 6) |
| Birds (Birds öppning) | the Swedish i, "Birrds" (probe), heard "bygds", "bereds" | `Börds` (respell.birds): heard "börs" 2 in 3 |
| Miles (Tony Miles) | two syllables, "Mi les" (probe), heard "Bailas", "Meiles" | `Majls` (respell.miles): heard "Miles" 3 in 3, against 1 |
| Stonewall, Stonewallangreppet, Stonewallvarianten | three Swedish syllables, "Stonevall" (probe), heard "stående av alla" 3 in 3 | `Stounwåll`... (respell.*, one per compound): two syllables, heard "stanboll", "stanvård" |
| Grand Prix | the x said, "Grand Priks" (probe, far from "Grand Pri"), heard "pricks" 2 in 6 | `Pri` (respell.prix): no x in 3 |

- The main round trip (300 lines, Whisper medium set to `sv`, seed 0; Parakeet missed 70 % of the words) ran
  before the 30 s padding, so long lines were cut. Then 3 seeds on 110 short lines (the reviewers' list, the
  file letters, the numbers, the opening names and eponyms in "Öppningen är X, inte Y.", the words missed
  most), 49 lines of A/B variants, and probes (4 seeds) on the doubtful readings.
- Checked and fine: the file letters (a, be, se, dé, é, eff, ghé, hå, heard as the letters or as notation),
  `ell` (heard L 6 times in 6), Elo (probe: `Élo`, the long e; "yellow" is the recogniser), rating, Hmm (hummed),
  Aj, Varsågod, dammatt, kvävningsmatt, epålettmatt, femtio- and sjuttiofemdragsregeln, blundrar, the numbers elva,
  tolv, tretton, fjorton, sjutton, nitton, 21, 34, 64, 85 procent, 92, "plus ett komma fem", patt (probe: `pat`), Tarrasch (`Tarrasj`), Diemer
  (`Dimer`), Bongcloud (`Bångklaud`), Caro-Kann, Philidor, Petrov, Zukertort, Aljechin, Nimzowitsch, Owen,
  Horwitz, Ponziani, Torre, Colle, Bogo, Tjigorin, Nimzo-Larsen, Grob, Mieses, Van Geet, Jobava, From,
  Karlsbad, Alapin, Smith-Morra, Rossolimo, Svesjnikov, Tajmanov, Winawer, Rubinstein, Panov, Giuoco piano,
  Evans, Fegatello, Traxler, Morphy, Marshall, Parham, Halloween, Sämisch, Falkbeer, Maróczy, Känguru, Semi-Tarrasch,
  Englund, Saragossa, Leningrad, Botvinnik, Karpov, Breyer, Zajtsev, Sozin, fianchetterar, remiaktig.
- Left as is: "torn", "tornet", "tornen" are heard "tårn", "tån", "tånet" most of the time, and the probe puts
  "torn" nearest "tårn"; `toorn` and `tuorn` were heard the same (the recogniser may also be writing
  Norwegian). Pirc is read "Pirk" (probe); `Pirts` was heard "pitch", "pets", no better. Schliemann is read
  "Sliman" and `Sjlimann` was heard the same. Fajarowicz ("Fire Witch") and `Fajarovitj`: the same. Pterodaktyl
  (the "pte" garbled), Scheveningen, Richter ("Rikter"), Réti and Najdorf are heard badly, with no
  candidate. "remi" before "enligt" is heard "rea med", "VME" (probe: `rémi`, no clear candidate). "arton"
  and "sjuttioett" read well alone (probe), but after "tog" they were never heard right. "dam" is
  sometimes heard "damm" or "dom" (probe: dam, damm and daam alike).
- Recogniser habits: squares and numbers as notation; compounds joined or split ("i stället" written
  "istället", "kungs flygel"); v heard as b ("Bits tid", "bitslöppare"); the sj sound written "sk", "ch" or
  "tj"; the first word of a short line lost; eponyms garbled. Whisper writes the spelling it knows ("Grand
  Prix", "Miles") whatever the voice says.

## Polish

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| the file letter a (squares) | runs into the word before it: "na a sześć" heard "na sześć", "ruch a trzy" heard "rucha trzy" (the probe puts "na a sześć" near "na sześć") | `aa` (file.a.spoken): heard as the a-file 22 times in 30, against 12 for "a", over 10 contexts |
| sto (appraisal.num.acc_explain.b3, .b3.2) | said, but heard as "co", "to" or "z" 8 times in 8 | `sto procent` (.spoken): 6 times in 6 |
| fianchetto, fianchetta, fianchettem, fianchettami | "fiancetto" (probe), heard "Jan Szeto", "Piętrze to" | `fianketto`... (respell.*): heard with a k ("fialkietem", "ankietem") |
| Giuoco | "gi-uoko" (probe: Dżuoko far), heard "Duoco", "Głoco" | `dżuoko`: heard "Dżłoko" 2 times in 3 |
| Grand Prix | the d and the x said ("Grand Priks", probe), heard "pliks", "freaks" | `gran pri` (respell.grand, respell.prix): no x in 3 |
| Ponzianiego | "Ponzjaniego" (probe), heard "Pundzianiego" | `poncjaniego`: heard "poncieniego", "Punc Janiego" |
| Steinitz(a), Rubinstein(a), Spielmann(a), Schliemann(a) | as written: "Stejnica", "Rubinstejna", "Spilmana", "Szlemana" (probe) | `sztajnic(a)`, `rubinsztajn(a)`, `szpilman(a)`, `szliman(a)`: heard "Sztajnica", "Spielmana" 3 in 3 (against "Spearman", "spiralny"), "ślimana" |
| Sämisch(a), Jaenisch(a), Grünfeld(a), Miesesa | "Samisza", "Ja-e-nisza", "Grunfelda", "Mjesesa" (probe) | `zemisz(a)`, `jenisz(a)`, `grynfeld(a)`, `mizesa`: "Zemisza" 3 in 3 (against "Samisha"), "Grimfelda", "mi zesa" |
| Kieseritzky | "Kjeseritzki" (probe) | `kizericki` ("Kizericki'ego"): heard "Kizerickiego", "Kizeryckiego" |
| Maróczy | the Polish ó: "Maruczy" (probe, heard "maruczego" 2 in 3) | `maroczy`: "maroczego" 3 in 3 |
| Réti, Rétiego | é as "ej" or "y": "Rejtiego" (probe), heard "Rytiego", "rytmiego" | `reti`, `retiego`: heard "retiego", "trzeciego" |
| Owena, Milesa, McCutcheon(a), Kruijsa, Geeta, Jobavy | read as Polish: "Owena" (w = v), "Milesa", "Mkuczona", "Krui-sa", "Gieta", "Jobawy" (probe) | `ołena`, `majlsa`, `makaczon(a)`, `krajsa`, `chejta`, `dżobawy`: "Majsa", "makaczona" 2 in 3 (against none), "drzabawy" (against "obawy" 3 in 3) |

- Parakeet missed most words; the round trips are Whisper medium set to `pl`: 300 lines (seed 0, 18% of words
  missed), then 3 seeds on 109 lines (the reviewers' list, 100 opening names and eponyms in short carrier
  lines, the file letters, the L, numbers 11 to 100, decimals, the words missed most), on 32 lines of fixes,
  and on 32 lines of A/B variants of the file letters; probes (4 seeds) for every name and doubtful word.
- Checked and fine: goniec, gońca, gońcem (probe: "koniec", "końca" far; the recogniser often writes a k),
  hetman (the h said; heard "headman", "hitman"), the numbers (jedenaście to dziewięćdziesiąt dziewięć,
  sześćdziesiąt cztery, dwudziesty pierwszy, "plus dwa przecinek trzy" as the right digits, 3 seeds), el for the
  L, Hmm, Elo, blitz ("blic"), „poprawiam”, w przelocie ("pszelocie"), umknęło, zagram, epoletowy, jeż ("jesz"),
  Caro-Kann, Philidora (= "Filidora"), Horwitza, Collego, Tarrascha, Scheveningen (= "Szeweningen"), Marshalla,
  Morphy'ego, Halloween, Breyera, Evansa, Fischera, Schlechtera, Capablanki, Greco, Maxa Langego, Najdorfa,
  Richtera-Rauzera, Czigorina, Torrego, Bongcloud (= "Bongklaud"), Smitha (= "Smita"), Blackmara (= "Blekmara"),
  Falkbeera (= "Falkbera"), Stauntona ("Stałntona", near the English), the Russian and Polish names.
- Left as is: the file letter e melts the same way ("na e cztery" near "na cztery", a line-initial "e bije"
  lost), but `ee` did no clearly better (17 times in 33 against 14; heard "i" or "A"; `eh` is said "ech"). The
  opening texts keep "a" until tools/gen_opening_lang.py runs again (their squares come from file.*.spoken).
  Close calls in the probe, the recogniser hearing another word: czarne ("czarny"), bierki ("bielki"), roszadę
  ("ruszadę", "rozsada"). No settled Polish reading, left: Stonewall (no candidate close), Euwe ("Ełwe"),
  Nimzowitscha and nimzoindyjska (said with z, as many Polish players do), Diemera ("Djemera": `Dimera` heard the
  same), Van't.
- Recogniser habits: it writes "piąka" for pionka (the same sound: [pʲɔŋka]), splits "skoczka" as "z koczka",
  writes gońca as "końca", final -ę as -e and -e as -y ("czarny", "matę"), roszada as "rozsada", squares as
  notation (A6, E4), and garbles short lines and lines of names.

## Turkish

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| Caro-Kann | the Turkish c ("dj"): heard "Jarokan", "Çarokan" 3 times in 3 (probe: nearest "Çaro-Kan") | `Karo-Kan` (respell.caro, respell.kann): "karokan" 3 in 3, and in opening.say.transposed |
| Pirc | the Turkish c: heard "peş", "pirse" (probe: nearest "Pirç") | `Pirts` (respell): "Pirç", "pırtis", "pırt" in opening lines, no "dj" |
| Alekhine | "Alekine", the e said (probe; heard "Aleki'ne", "ailekine") | `Alehin` (respell): "aleyhin", "Alehint" |
| Nimzowitsch | heard "Nemzowic'i", "nemsobit" | `Nimzoviç` (respell): "Nemzovic" 2 in 3, "nebzeviç" |
| Van't Kruijs | "Kruiys" (probe), heard "Crease", "Screech", "Cruise" | `Kröys` (respell.kruijs): "Krois", "Cray's" |
| Diemer (Blackmar-Diemer) | "Diyemer" (probe), heard "Marbeam Er-", "Marley" | `Dimer` (respell): "Mardine" 3 in 3, the d heard |
| Najdorf | the Turkish j ("zh"): heard "Neisdorf", "Ajdorf" | `Naydorf` (respell): "Naidorf" 3 in 3 |
| Giuoco (Piano, Pianissimo) | "Giyuoko" (probe), heard "Gyoza", "güvo ko" | `Cuoko` (respell): "Joko", "Choco", "çok o" |
| Richter (Richter-Veresov, Richter-Rauzer) | ch as "ç": heard "Lister", "dekleri", "Richter" once in 3 | `Rihter` (respell): "Richter" 3 in 3, and 3 in 3 in classical_sicilian.advanced |
| Mieses | "Miyeses" (probe), heard "mss" twice, "mesele" | `Mizes` (respell): "Mises", "mizas" |
| Tarrasch (also Yarı Tarrasch) | "Tarrasç" (probe), heard "tarif", "tarah" | `Tarraş` (respell): "taraş", "taraj" 3 in 3 |
| Grand Prix | the x said (probe: nearest "Priks"), heard "predicts" once in 3 | `Pri` (respell.prix): "Grand Prix" 3 in 3 |
| Maróczy | no candidate close (probe), heard "Maroti", "Maroxi", "Marauder" | `Maroçi` (respell): "Maroç", "Marocca", and "maroç" in accelerated_dragon.advanced |
| Fajarowicz | heard "fajarobik", "frontrager" | `Fayaroviç` (respell): "Fyarovich", "Fayyarovic", "Fajarović" 3 in 3 in budapest.advanced |
| Colle | two syllables: "koalya", "kola", "koal et" | `Kol` (respell): one syllable, "kual" 2 in 3 |
| L ("Başka bir L dene", lesson.knight.wrong) | "le" melts after "bir": "birader ne", "bir nedeni", "bile deme" (probe: nearest "bir ne") | `lee` (.spoken): "leydene", "Leyden'i" 2 in 3 |

- The voice reads every word with Turkish letter values, foreign names too (c "dj", j "zh", ch "ç", ie two
  vowels, w as v). A new eponym needs checking for these letters first.
- The main round trip (300 lines, Whisper medium set to `tr`, seed 0) covered few opening names. Then 3 seeds on
  99 lines (file letters, numbers, ordinals, decimals, the reviewers' list, 60 opening names and eponyms in "Bu X,
  Y değil.", the words missed most), 43 lines of A/B variants in "Bugün X çalışacağız.", 7 real lines with the
  fixes, and probes (4 seeds).
- Checked and fine: the numbers (11 to 98, "yüzde yetmiş bir", "artı bir virgül beş", "eksi iki virgül sıfır",
  the ordinals: heard right 3 in 3, mostly as digits), the file letters a, ce, de, e, he (heard as the letter or as
  notation), "Le şeklinde" (2 in 3), Elo, Of, Vay, Hımm (hummed or dropped), Philidor (probe: read "Filidor"; "pide
  dör" is the recogniser), berabere (probe: the final e said; Whisper writes "beraber"), mattı, buluştu, gambiti
  (probe: as written; Whisper writes "gamete"), fil (probe: not "film"), vezir (probe: not "bezir"/"mezir"), piyonu
  (probe: no candidate better; Whisper writes "piyano", "pion"), kâğıdına, imkânsızsa, Kalem, Owen ("Oven"), Horwitz,
  Réti, Ponziani, Trompowsky, Elephant, Saragossa, Stonewall, Bongcloud, Scheveningen, Rubinstein,
  Evans, Traxler, Morphy, Marshall, Schliemann, Stafford, Parham, Halloween, Sämisch, Falkbeer, Zukertort,
  Sveşnikov, Taymanov, Alapin, Benoni, Benko, Pterodaktil, Nimzo-Hint, Bogo-Hint, Nimzo-Larsen, Grob, Çigorin,
  Petrov, Englund, Borg, Torre, Panov, Sicilya, Slav, Rauzer, Botvinnik, Karpov, Miles, Breyer.
- Left as is: hâlâ and hâlde are read like hala and halde, short vowels and a hard l (probe; "haalaa" is farther,
  and the recogniser cannot tell them apart), as many speakers say them. Bird is read with the Turkish i (probe:
  `Börd` differs) but neither was heard right ("bir de", "bir arada"). Jobava is read with the Turkish j;
  `Cobava` was heard worse ("Cobaba" once, "kopağ"). Aziz George was heard "George" 3 in 3 though the probe finds no
  close candidate; `Corc` was heard "Kord", "Korkut". Winawer and `Vinaver` were heard alike badly ("WinAware",
  "binavar"; probe: close). Carlsbad leans to "Çarlsbad" (probe) but plain and `Karlsbad` were heard the same
  ("CarSpot"). The file letter a after a vowel melts into it ("kale a üç" heard "kala üç"), and fe and be run into
  their rank ("bebeş", "otwcb"), as with any letter before a number. Unclear, no candidate tried: Smith-Morra
  ("Smithmore'a" once in 3), Van Geet ("van-git", "OneGate"), Rossolimo ("Rosoliman").
- Recogniser habits: squares and numbers as notation; v heard as b or m ("Bezir", "mezir", "ve zirve"); "hamlesi"
  heard "hamilesi"; "hâlâ" written "hala"; a word's final vowel swallowed into a frequent word (berabere as
  "beraber", piyonu as "piyano"); eponyms in English spellings; short lines garbled.
- The opening texts said "dört'e" in the round trip's renderings (an older openings file); the repository's
  `assets/coach/openings/tr.lang` already says "dörde", "beşe" (tools/gen_opening_lang.py, TR_SQUARE_SUFFIX_RE).

## Estonian

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| Grand Prix' (rünnak) | the x said: "Priks" (probe: Priks +0.47, Prii +9.18) | `prii` (respell.prix): read "Prii" (probe), no x in 3 renderings of the game line ("prii", "prüü") |
| Giuoco (piano, pianissimo) | the g of "gaas", as written: heard "kiuoko" 3 times in 6 (probe: Džuoko +2.23, the farthest) | `džuoko` (respell): heard "juoko", "jooko", "Joko" 6 times in 6 |
| Milesi (variant) | "Mi-lesi", as written: heard "millesi" 3 times in 3 | `mailsi` (respell): heard "mailsi", "mäilsi" 3 in 3, "mails" 2 in 3 in the game line |
| Birdi (avang) | "Birdi", as written: heard "pirdi", "püüdi" (probe: neither Bördi nor Beerdi close) | `bördi` (respell): heard "pöördi" 2 times in 3 |
| Breyer, Breyeri | "Brejeri": heard "prejeri" 2 times in 3 (probe: Brejeri -0.36, Braieri +1.13) | `braier`, `braieri` (respell): heard "brajeri", "prajeri" 4 times in 6 |
| Parhami (rünnak) | the h said: heard "parhami", "parhaami" 2 times in 3 | `paarami` (respell): heard "paarami" 2 in 3, "paarame" in the game line |
| Sämisch, Sämischi | the sch as "s" (probe: Sämissi +0.07, Sämiši +0.39): heard "semisi", "missi", once "samishi" | `sämiš`, `sämiši` (respell): "sh" heard 4 times in 6 against 1 in 3 |
| Nimzo-, Nimzowitsch, Nimzowitschi | the z as "s" (probe: Nimsovitši closest): heard "nimsu", "nimse vitsi", never a ts in 9 | `nimtso`, `nimtsovitš`, `nimtsovitši` (respell): "ts" heard 4 times in 9 ("nimtsu", "nimtsavits") |

- Parakeet could not do Estonian; the main round trip is Whisper medium set to `et`, 300 lines (seed 0, the first 195
  before the 30 s padding), with half the words missed: the voice's Estonian is understandable, the recogniser's
  Estonian weak. Then 3 seeds on 48 lines (the reviewers' coach words, the file letters in carriers, the numbers),
  30 lines naming 60 openings and eponyms ("See on X, mitte Y.") with 15 respelled variants, 51 lines of game text
  (the fixes and the most missed words), and probes (4 seeds) on 50 readings.
- Checked and fine: the file letters aa, bee, tsee, dee, eff, gee, haa (heard as letters or notation); `ee` alone
  is said long (probe: "täht ee" nearest "täht eee", far from "täht"); `ell` (heard "el" 6 in 6); Hmm (hummed, probe,
  heard "Hmm" 6 in 6), Vau, Oi-oi, Hei, Tuli!, Elo (heard "elo"), Ekspert, kandidaatkäike, forsseeritud, parandan, kella;
  the numbers (üksteist ... üheksateist, 21-99, sada, sada viiskümmend, koma, protsent, miinus, ordinals: mostly
  written as the right digits); ettur, lipp, ratsu, vanker, oda, matt, viik, tuld in game lines. Names read well
  as written: Philidori ("filidori"), Caro-Kanni, Tarraschi (heard "tarashi" 3 in 3: the probe had it wrong),
  Marshalli, Rubinsteini, Scheveningeni, Oweni, Horwitzi, Réti, Ponziani, Trompowsky, Torre, Colle, Miesesi,
  Zaragoza, Richter, Bongcloudi, Fromi, Karlsbadi, Smith-Morra, Najdorfi, Rauzeri, Sozini, Winaweri, Traxleri,
  Morphy, Zaitsevi, Staffordi, Aljehhini, Grünfeldi, Pirci, Tšigorini, Svešnikovi, Evansi, Falkbeeri, Pterodaktüli,
  Fiankettovariant.
- Left as is (respelled variant heard no better): Zukertorti ("Sukertorti", the z as s; `Tsukertorti` was heard
  "sugertorti" all the same), Kruijsi (`Kröisi` heard "kreisi", "kruisi" as before), Jobava (`Džobava` heard
  "jobava"), Stonewalli (`Stounvolli` no clearer), Fajarowiczi, Schliemanni (`Šliimanni` the same), Halloweeni
  (read "Halloveeni", recognised anyway), Maróczy (heard "maroks", no candidate close in the probe), Van Geeti (g
  as k), Blackmar-Diemeri. "ee neli" after "välja" or at the start of a line is heard "enel", "lenneli": the
  letter is said (probe), "neli" is what the recogniser loses; "tähtedega aa" melts a little ("tähtetegaa"), but
  the probe keeps the aa (without it +0.71). "reitinguks" is a close call between "reetinguks" and "räitinguks".
- Recogniser habits: squares and numbers as notation ("A6", "F7", "86%"); final -u as -o (sinu "sino", käiku
  "käikoo"; the probe cannot tell them apart); geminates and quantity lost or added (etturi "eturi", lipu "lippu",
  käik "keik"); v as b or p ("palge", "Banker"); words joined across a space ("valgeselitab", "partiikestis"),
  the v of "väljale" lost after "läheb"; English words for names ("Gambit", "Crazy", "Prix" whatever is said);
  "neli" after a vowel written "nedi", "lii".

## Greek

No word needed respelling: `common.lang` has no respell.* entry, and no `.spoken` line was added for the
voice's sake.

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| the file letter e after "το", "στο", "κίνηση" ("το ε τέσσερα") | often swallowed: no e in about 35 of the 53 e-squares of the 300-line round trip ("το 4"), and no e in 5 of 12 renderings of 4 test lines | left `ε`: with `έ` the e was missing in 7 of 12, and `εε` / `έε` garbled other words of the line. In new lines, put the square after a consonant ("στον", "με") |
| a question mark ";" inside a line ("Πάλι την ίδια κίνηση; Εντάξει...") | read like a semicolon: a short pause, or none at all | fixed in code: the voice front end speaks every Greek ";" as "?" (`tts::text::spokenPunctuation`, see below) |

- Round trip: Parakeet missed 93 % of the words, so the round trip is Whisper medium set to `el`. It covered
  300 lines (seed 0), then 3 seeds on about 60 lines: the reviewers' list, words missed in more than a third of
  their uses (in short carrier sentences), the file letters, and the variants tried for ε.
- Checked and fine: έιτς (two vowels, "έι τς"), α and μπι said alone, γάμμα, Έλο, αν πασάν inside a sentence,
  μπλιτς, ράπιντ, Σερί, ρομπότ, τουρνουά, Ωχ, Ωπ, Αχ, Πω πω, κανόνες, στήλες, Αρχάριος. The elisions are each
  said as one word ("Πάρ’ τον" heard "Πάρτον", "στ’ αλήθεια" heard "σταλήθεια", "Γι’ αυτό"). The voice keeps
  the dialytika: Ταϊμάνοφ, Νιμτσοϊνδική and Μπογκοϊνδική sit next to "Ταημάνοφ", "Νιμτσοηνδική" and "Μπογκο
  ινδική", and far from the digraph readings "Τεμάνοφ" and "Νιμτσιντική" (probe in context, 4 seeds).
  Όιβε is said "Ό-ι-βε", ΜακΚάτσον "Μακ Κάτσον" (not spelled out), and Μπόνγκκλαουντ and Στόουνγουολ come out
  as two syllable groups. επιτίθενται sits next to "επιτίθεντε" (+0.05) and far from "επιτίθεται" (+1.42), and
  the voice says the σ of σκακιέρα, even though Whisper drops both.
- Left as is (close calls; the probe cannot settle stress): φινάλε (Whisper writes "final"), Γκαμπί
  (heard "Γαμπί"; the probe puts "γαμπί" +0.5 against "γκάμπι" +0.95), πτέρυγα (heard "τέρυγα" in 16 of 17
  renderings, though in "στην πτέρυγα" no variant is close), παγίδες (heard "παγιέδες" 3 times in 3; in
  context "πάγιδες" and "παγέδες" are as close as each other). Τζιόκο leans to "Τζι-όκο" over "Τζόκο".
  Μπογκολιούμποφ, Σεβενίνγκεν and Φαγιαρόβιτς came out as ties between stresses. Said alone, "Πατ." is heard
  "Ματ" 3 times in 3, and `Πάτ.` did no better; inside a sentence it is heard right. Χμ and Χμμ get a vowel
  ("Χαμέ", "Καμ"); `Μμ` too ("Μεμ").
- Greek ";" (U+003B, or U+037E, which NFKD folds to U+003B before the voice sees it): at the end of a line the
  voice reads it like "." or "?", with no difference the probe can measure. Inside a line, it reads it as a
  semicolon. The pause after it is 0.01 to 0.19 s, against 0.21 to 0.36 s for "?" or ".", over 5 sentences and
  4 seeds. In "Πάλι την ίδια κίνηση; Εντάξει, ας δούμε." it ran straight into the next sentence in 4 seeds of
  4, and that rendering is closest to the comma version. "?" gives a sentence pause there. 17 catalog lines
  have a ";" inside the line. Greek has no semicolon of its own (it uses the ano teleia ·), so
  `tts::text::spokenPunctuation` turns every ";" and U+037E of Greek text into "?" before chunking. Left as is:
  src/coach/pacing.cpp counts only U+037E as a sentence end, not U+003B, which the Greek catalog uses; it still
  finds the pause there, only with a comma's weight.
- Grammar found on the way: `ex.retake.b6` said "Το ίππος παίρνει στο ε5 παίρνει..." when the reply is a piece
  move. It now follows the analysis convention "η απάντηση {reply}" and uses κερδίζει.
- Recogniser habits: Whisper writes squares as notation (E4, f7, "το 4"), and confuses the letters that sound
  alike (ι/η/υ/ει/οι, ο/ω, ε/αι: "ήπος" for ίππος, "μια" for μία, "και" for κι). It drops the first consonant
  of a cluster (σκακιέρα as "κακιέρα", βαθμολογία as "βασμολογία"), writes loanwords in English ("Bleach",
  "final", "classic chess"), and pads a short line with a repeated phrase ("Τι έχεις; Τι έχεις; ...").

## Finnish

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| Aljehin, Aljehinin | the j dropped: "Alihinin" (probe: nearest of the candidates), heard "Alihinin", "Alihin", "alihene" 8 times in 9 | `Alljehin`, `Alljehinin` (respell.*): the j heard 7 times in 9 ("Aljehin" 6 in 6 in a short line), against 1 in 9 |
| Pircin | with an s: "Pirsin" (probe), heard "piirsin" 2 times in 3 in a short line | `Pirtsin` (respell.pircin): "Pirtsin" 6 in 6, against 4 in 6 |

- Parakeet missed most words; the round trips are Whisper medium set to `fi`: the main one stopped after 111 of
  its 300 lines (seed 0, 1,035 words, 42 % of words missed: Whisper's Finnish, long lines end in loops), then
  3 seeds on 80 lines (the reviewers' list, the file letters in squares and compounds, numbers 11 to 120,
  decimals, the inflected 21 and 71, 75 opening names and eponyms in "Tänään pelattiin X ja Y.", the words
  missed most), 14 lines of A/B variants and 6 of the fixes in game lines; probes (4 seeds) on 24 names and words.
- Checked and fine: the file letters aa, bee, see, dee, ee, äf, gee, hoo (heard as notation; also in "äf-linja",
  "dee-sotilas"), `äl` for the L ("äl-kirjaimen", "äl-hyppyä": "L-kirjaimen", "älyhyppyä"), aasta, hoohon,
  kirjain ee, the numbers (yksitoista to sata, kaksikymmentä, "plus kaksi pilkku kolme", "miinus yksi pilkku
  viisi", kahdellakymmenelläyhdellä, seitsemänkymmenenyhden as the right digits), Hmm (hummed), Auts, Hups
  (probe: nearest "Hhups"; heard "Ops", "Vaps"), shakki (probe: `šakki`, as it should; the recogniser writes
  "sakki"), Elo-luvuksi, epolettimatti, oppositio, valkea, siirrolla, ratsu, torni, lähetti, kuningatar,
  linnoittautua, Caro-Kann, Philidorin, Owenin (probe: "Ouenin"), Horwitzin, Grünfeldin, Rétin ("Reetin"),
  Ponzianin, Tarraschin and Marshallin (š, probe), Schliemannin, Tšigorinin, Nimzowitschin, Nimzo, Bogo,
  Miesesin (long i, probe), Zaragozan, Jobava, Stonewall, Richter-Veresov, Smith-Morra, Grand Prix, Najdorfin
  ("Naidorf" 6 in 6; `Naidorfin` heard no better), Svešnikovin, Kanin, Winawerin, Rubinsteinin, Fegatello,
  Traxlerin, Morphyn, Parhamin (h silent, probe), Halloween, Semi-Tarrasch, Meranin, Sämischin (probe:
  "Sämišin"), Falkbeerin, Englundin, Benkon, Rossolimon, Taimanovin, Alapinin, Panovin, Evansin, Staffordin,
  Grobin, Borgin, Pterodaktyyli, Fianketto, Carlsbad, Fajarowiczin, Maróczy, Breyerin, Zaitsevin (probe:
  "Zajtsevin"), Richter-Rauzer, Milesin, Lucenan, Anastasian, Bullet, Petrovin.
- Left as is: Giuoco is read "gi-uoko" (probe), but Whisper writes "Juoko" either way and `Džuoko`, `Djuoko`
  were heard no better. Birdin is read with the Finnish i ("Birrdin", probe); `Bördin` was heard "pöydän",
  no better. Zukertortin is said with an s (probe: "Sukertortin"), as many Finnish players say it. Scheveningenin
  and Van Geetin are heard badly, with no candidate clearly closer. The file letter aa melts into a word ending
  in a vowel before it ("Paras oli aa neljä" heard "Parasolja 4" 2 times in 3, "Torni aa kahdeksan" "tornia
  8"), but the probe puts the line nearest "oli aaa neljä", the aa said: the recogniser, as in Polish.
- Recogniser habits: squares and numbers as notation ("E4", "21"); shakki written "sakki", "sähkö" or "Chuck";
  valkealla as "valkeilla", siirrolla as "siirralla", "siirolla"; English spellings for loanwords
  ("Gambit", "Halloween"); a long line ends in a loop of one word ("pelaimasta pelaimasta...").

## Dutch

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| fianchetteert | the ch said s or sj: "fiansetteert" (probe), heard "fianceteert", "fiansteert", never with a k in 15 | `fianketteert` (respell): a k heard 7 times in 15 |
| Grand Prix | d and x said: "grant priks" (probe), heard "grondbricks", "krant op riks" | `gran`, `prie` (respell.grand, respell.prix): "Grand Prix" 7 in 9 against 1 in 6 |
| Winawer | the aw said like au: "winauer" (probe), heard "Wienauer" 9 in 9 | `winaver` (respell): "Wienhaver", no au in 6; `wienaawer` still "Wienauer" |
| Fajarowicz | heard "Firewix" 4 in 4 | `fajarovitsj` (respell): "Vajarovits" 2 in 3 |
| coach | the t lost: "koosj" (probe), heard "kosh", "koorts", "koos", "course" | `kootsj` (respell): "coach" 3 in 9 against 1 in 6, otherwise "koots", "codes" (with a t) |
| eenentwintig (21) | first syllable lost: "enentwintig" (probe), heard "in twintig" | `éénentwintig` (number.21.spoken): 21 eleven times in 12 against 7 in 12 |
| Hm. (event.filler, said alone) | with a vowel: "hem" (probe, heard so 2 in 3) | `Hmm.` (event.filler.spoken): "Hmm" 2 in 3. In a sentence "Hm," is fine (3 in 3) and "Hmm." is dropped |

- Parakeet does not do this voice's Dutch: the round trip used Whisper medium set to `nl` (300 lines,
  seed 0), then 3 seeds on about 130 lines (the reviewers' list, file letters, numbers, every eponym in
  "Ik antwoordde met ..., en jij koos voor ...", every fix), and probes. Whisper hallucinates on a word
  alone ("Remise." heard "Mijn meeste", "Pat." "Bakker"): check a name in a sentence.
- Checked and fine: remise ("remieze"; `remieze` heard worse), mat (like "mad"), pat (like "pad"), dame
  ("daame"), gambiet (Dutch g, like "chambiet"), zwart (short a), Au (like "Auw"), Elo, rating, blitz,
  rapid, blunder, super, Perfect, el (the L), aa, ee, Halfgevorderd, vóór, the -zettenregel compounds, 31
  and 71 (accents did no better), Tarrasch and Sämisch (final sch said sj, probe), Aljechin ("aljechien"),
  Réti ("reetie"), Pirc ("pirts"), Miles ("majls"), Caro-Kann, Philidor, Nimzowitsch, Grünfeld, Bogo,
  Owen, Horwitz, Van 't Kruijs, Englund, Benoni, Benko, Bird, Ponziani, Blackmar-Diemer, Trompowsky,
  Torre, Colle, Tsjigorin, Albin, Larsen, Grob, Mieses, Saragossa, Rapport-Jobava, Stonewall, Richter,
  Bongcloud, Alapin, Smith-Morra, Rossolimo, Najdorf, Svesjnikov, Tajmanov, Rubinstein, Panov, Giuoco,
  Evans, Fegatello, Traxler, Morphy, Marshall, Schliemann, Stafford, Parham, Halloween, Falkbeer, Meraner,
  Pterodactylus, Breyer, Zajtsev, Rauzer, Sozin, Maróczy, Karlsbad, Karpov, Botvinnik.
- Left as is: "en passant" is said with the t (probe), but `an passan` and `en passan` were heard no
  better; "en" must not be respelled (the word "and"). "fianchetto" is a close call between k and s
  (probe); `fianketto` was heard worse ("fianket"), as was `fiankettovariant`. "trucs" is read with a
  short u ("truks"); `truuks` was heard the same ("trucks"). "epauletmat" is heard badly either way
  (`epooletmat` no better). `haa` for the h did no better than `ha`. Zukertort keeps its z.
- Recogniser habits: zwart as "zwaard", wit as "het", "dit", "met", zet as "z" (the letter's name), koning
  as "coding", dame as "daam", "duim"; ee as "de", "een"; ha as "A" (A8); gambiet as "gambit", "come
  beat"; Tarrasch as "Taras", -isch as "-ies" (right in Dutch); "min" as "met" after "ongeveer".

## Czech

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| Hm, (before a sentence: "Hm, moje hrozba...") | a vowel: heard "Hem", "Hehm", "Hn", or lost (0 hums in 5); the probe puts it nearest "Hem" | `hmm` (respell.hm): hummed 7 times in 9. "Hm." alone was heard as a hum either way |
| fianchetto, fianchetta, fianchettem, fianchetty | the Italian ch as č: "fiančeto" (probe: +0.34, `fianketo` +1.34) | `fianketo`, `fianketa`, `fianketem`, `fiankety` (respell.*): heard with a k 12 times in 12 (Whisper writes "fianchetto" for the plain word whatever is said) |
| Giuoco | the g said: heard "gyuoko", "Geoxo", "Guoko" | `džuoko` (respell): "Djoko", "Duoko", "džuoko", "Jouko" |
| Ponzianiho, Zukertortovo | the z as z or s ("Ponsianiho", "Sukertortovo", probe) | `poncianiho`, `cukertortovo`: c heard 5 in 6 and 6 in 6 |
| Grecova, Grecův, Capablancova, Capablancův | c as ts: "Gretsův" (probe +0.01, `Grekův` +2.18), "Kapablancova" (heard "kapablancová" 3 in 3) | `grekova`, `grekův`, `kapablankova`, `kapablankův`: k heard 6 in 6 and 5 in 6 |
| Steinitzova, Rubinsteinova, Spielmannova (and -ův) | as Czech letters: "Stajnicova", "Rubinstejnova", "Spijelmanova" (probe); heard "stejnicová" 3 in 3 | `štajnicova`, `rubinštajnova`, `špílmanova`...: š heard 6 in 6, 8 in 9, 2 in 6 |
| Tarraschova, Tarraschovy, Tarraschův, Polotarraschova | s + ch: nearest "Tarasova" (probe +0.68, `Tarašova` +1.74) | `tarašova`...: š heard 6 times in 9 ("Tarašova", "starášovi") |
| Schliemannova, Schliemannův, Sämischova, Sämischův | "Schlijeman", "Semischova" (probe); heard "Shrebanov", "škri manu", "semisková" | `šlímanův`, `zémišova`...: "šlimanův" 6 in 6, "zemišová" 6 in 6 |
| Miesesovo, Kieseritzkého | ie as i-je: "Mijesesovo", "Kijeserického" (probe); heard "měsíce o", "k jezerického" | `mízesovo`, `kízerického`: "Mises" 6 in 6, "kízerického" 6 in 6 |
| Grünfeldova, Grünfeldův | "Grunfeldova" (probe +0.01, `Grýnfeldova` +1.10); heard "Grunfeld" 4 in 6 | `grýnfeldova`, `grýnfeldův`: "Greenfeld" 6 in 6, no u |
| Fajarowiczova | "Fajarovicova" (probe +0.03, `Fajarovičova` +0.40); heard "fajarovicevá" 2 in 3 | `fajarovičova`: no extra syllable in 6 (Whisper writes c or č) |
| Philidorova, Philidorovou, Philidorův | ph as p: "Pilidorova" (probe +0.07, `Filidorova` +0.79); heard "Pjolidorová", "Pylidorová" | `filidorova`...: f heard 9 in 9 |
| Marshallova, Marshallovu, Marshallův | s + ch: "Marschalův" (probe +0.17, `Maršalův` +0.51) | `maršalova`...: Whisper writes "Marshal" either way |
| Milesova (výhra nad Karpovem) | "Mi-le-sova": heard "Melesová", "malasová" | `majlsova`: "Milesová" 3 in 3 in a short line, "Majasová" 3 in 3 in the opening text |
| Halloweenský | "Haloveenský" (probe +0.18, `Helouvínský` +1.05); heard "halověnský", "halobjenský" | `helouvínský`: "Halloweenský" 5 in 6, against 4 in 6 |
| Grand Prix | d and x said: "Grand Priks" (probe +0.09) | `gran`, `prí` (respell.grand, respell.prix): no x; Whisper writes "Grand Prix" either way |
| Euweho | "Euveho" (probe); heard "uvehlo", "EUV", "EU-vehla" | `éveho`: "éveho" 5 in 6 |
| Rétiho | soft t: "Réťiho" (probe −0.16, `Rétyho` +0.48) | `rétyho`: heard "Rétyho" 6 in 9 (Whisper's y for a hard t) |

- Parakeet missed 73 % of the words on this voice; the main round trip is Whisper medium set to `cs`, 300 lines
  (seed 0, 28 % of 4,519 words missed), then 3 seeds on 97 lines (the reviewers' list, 24 A/B pairs of eponyms in
  "Tohle je X a tohle Y.", 53 more opening names and eponyms in lists, the file letters in squares, the L, numbers,
  the words missed most), and 3 seeds on 30 lines of the fixes in the game's own lines and in opening.say
  sentences, with the plain form beside the doubtful ones; probes (4 to 6 seeds) on about 90 readings.
- Checked and fine: the file letters á bé cé dé é ef gé há (heard as notation), the L (`el`, probe; heard "L"),
  Elo, blitz ("blic"), rapid ("rapit"), rating ("rejting"), Ouha ("ou-ha"), an pasan, „upravuji“, mat (the t said;
  "Mat třetím" merges the two t), tah ("tach"), the numbers (92 %, 12, 13, 17, 19, 21krát, 34, 122, "plus jedna
  čárka tři", "minus nula čárka pět", ordinals). The loanwords keep a hard d, t, n (probe): diagonála, aktivní,
  etiketa, taktika, indická, motivy, tip, Benoni, Poncianiho, Štajnic, pianissimo. Names read well as written:
  Caro-Kann ("Karo"), Fischer ("Fišer"), Schlechter (š), Falkbeer ("Falkbér"), Smith ("Smit"), Bongcloud
  ("Bongklaud"), Breyer ("Brejer", as in Hungarian), Morphy (heard with an f 6 in 6, though the probe leans to a
  p), Owen (heard "Owen" 4 in 6; `Ouenova` 5 in 6, a tie in the probe), Diemer ("Deemer" 3 in 3), Stonewall,
  Trompowského, Horwitz, Torre, Maróczy, Staunton, Evans, Stafford, Parham, Janowski, Botvinnik, Najdorf,
  Svešnikov, Rossolimo, Traxler, Englund, Jobava, Richter-Veresov, Rauzer, Sozin, Pterodaktylí, Bogoljubov,
  Tartakower, Paulsen, Anderssen, Keres, Lasker, Lange, Nimcovič, Čigorin, Zajcev, Alapin, Aljechin, Petrosjan,
  Spasskij, Kasparov, Karpov, Averbach, Bronštejn, Saragosa, Leningrad, Meran, From, Pirc, Borg, Grob, Sokolskij,
  Albin, Tajmanov, Kan, Smyslov, Tal, Petrov, Panov, Winawer, Benko, Fegatello.
- Left as is: partie, partii, partiích lean to a soft t alone and in "Dobrá partie!" but are ties or hard in other
  contexts (probe, 7 contexts); `partyje` was heard "partie je" once in 3. kandidátní and kritický are ties
  between hard and soft. Colle is said "Koleův" (the e said; probe −0.97), and `Kolův` was heard no better.
  Jaenisch ("Ja-eniš"), McCutcheon ("Mekučeon"), Van't Kruijs ("Krujs"), Van Geet ("Gét"), Bird ("Birdovo"),
  Scheveningen (near "Sceveningenská") and Blackmar are read as spelled: `Jénišův`, `Mekačenova`, `Krajsovo`,
  `Chétovo`, `Bérdovo`, `Ševeningenská` and `Blekmar` were heard no better. The h-file after a vowel melts
  into the word ("pole há osm" heard "polehá osm", "polená 8"): Czech h is voiced, as said; "tah á tři" is heard
  "tahá tři" the same way.
- Recogniser habits: Whisper writes voicing assimilation as heard (jezdcem "jestcem", zpět "z pět", then "z 5"),
  Slovak and colloquial forms (pěšec "piešec", jezdci "jazdci", rychlý "rychlej", navrhuju "navrhuji"), the
  wrong vowel length (pružné "průžné", pořadí "pořádí") and y for i (figurami "figurámy"), tvůj as "svůj" or
  "tvoj", squares and numbers as notation ("D5", "polehá 8"), splits and joins ("proti gambit", "hra je", "boju
  je"), and known names in their own spelling ("Halloween", "Grand Prix", "Marshal", "Steinitz") whatever the
  voice says: check a name with the probe. A line's first word after a colon is often lost.

## Romanian

| Written | Voice said | Spoken spelling |
| --- | --- | --- |
| en passant | "ien pasant", with the t: heard "Ien pasant", "pasand", "pasunt", a t 5 times in 6 | `an pasan` (respell.en, respell.passant): no t 8 times in 9 ("an pasan", "anpasan") |
| Pirc | "pirk" (probe: Pirk +0.03, Pirț +3.20), heard "PIRC" 3 times in 3 | `pirț` (respell): "Pirtz", "pierți" 2 in 3 |
| Najdorf, Fajarowicz | the j of "joc" (probe: nearest "Nadjdorf", "Fajarovicz"), heard "na H2", "fajarovics", "fără JaroX" | `naidorf`, `faiarovici` (respell): "Naidorf", "n-ai dor" 2 in 3; "-vici"/"-vic" 3 in 3 |
| Winawer | "winauer" (probe), heard "Wienauer", "win hour" 3 times in 3 | `vinaver` (respell): "vinăver", "Vinavar", no au in 3 |
| Kruijs (Van't Kruijs) | "kru-ijs", heard "creas", "Crease", "cruijs" | `crăis` (respell): "Crais", "Creis" 3 in 3 |
| Geet (Van Geet) | the g of "ger" (probe: nearest "Djeet"), heard "ghid", "Jeet" | `heet` (respell): "Hit" 3 in 3 |
| Mieses, Diemer (Blackmar-Diemer) | "mi-e-ses", "di-e-mer": heard "miestes", "de Emer" | `mizes`, `dimer` (respell): "mizes", "Mises" 3 in 3; "Dimer" 2 in 3 |
| Bird | as written (probe: Börd, Bărd far), heard "bieurde", "biurt" | `bărd` (respell): "bărdă", "băt", "Bird" |
| Ponziani | with a z (probe: Ponțiani +0.96), heard "Ponsiani", "ponzian" | `ponțiani` (respell): "Pontian", "Pontiani" 3 in 3 |
| Lange (Max Lange) | the g of "ger" (probe: nearest "Lanje"), heard "Lunge" 2 times in 3 | `langhe` (respell): "Langa" 3 in 3 |
| Grand Prix, Halloween | the d and the x (probe: nearest "Gran Prics", +5 from "Gran Pri"); w as v ("Haloveen") | `gran`, `pri`, `helouin` (respell): Whisper writes "Grand Prix", "Halloween" 3 times in 3 either way |

- Parakeet missed 73% of the words; the round trips are Whisper medium set to `ro`: 300 lines (seed 0; a third of the
  words come back wrong even with the diacritics ignored), then 3 seeds on 35 lines (the reviewers' list, file letters,
  the L, numbers, the words missed most, A/B variants) and on 9 pairs of lines of eponyms with their respellings, 1 seed
  on 19 lines holding the other opening names and eponyms, and probes (4 seeds) for every name and doubtful word.
- Checked and fine: regele (probe: the g of "ger"; "regile" is the recogniser's), "litera e" and "pe e patru" (probe:
  nearest "é", not "ie"), a be ce de ef ge haș (C4, D5 3 in 3, "a trei" 3 in 3), le for the L, the numbers (11, 14, 34,
  "șaptezeci și una", "nouăzeci și nouă", "plus unu virgulă cinci", written as the right digits), Hm (probe: "Hmm"),
  Uau (one syllable, probe), Elo (one word, not "ie-lo" nor letters), blitz, tempo, Mut-o, Câștig-o (probe:
  "Muto", "Câștigo"), Hopa (h said), o sută, Philidor ("Filidor"), Grünfeld ("Grünfelt"), Schliemann ("Șliman" in
  context), Tarrasch ("Taraș"), Trompowsky, Owen ("Ouen"), Horwitz, Réti, Richter ("Rihter"), Smith ("Smit"), Morphy
  ("Morfi"), Maróczy (heard "Maroții" 3 in 3), Sämisch ("Semiș"), Bongcloud, Traxler, Carlsbad, Steinitz, Rubinstein,
  Marshall, and in the round trip Caro-Kann, Alehin, Alapin, Taimanov, Sveșnikov, Rossolimo, Meran, Evans, Richter-Rauzer,
  Sozin, From, Zaițev, Cigorin, Botvinnik, Bogoliubov, Richter-Veresov, Pterodactil, Hipopotam, Torre, Englund.
- Left as is: fianchetto leans to "fiancetto" in the probe (alone and in "face fianchetto cu"), but was heard with a k
  ("anchetă") and `fianketto` did no better (one "fianchetto" in 3 each). Scheveningen is read "skeveningen" (probe);
  `Șeveninghen` was heard the same ("cheveningă"). Stonewall is read "stonevall", but heard "Stonewall" 2 in 3 and
  `Stounuol` worse; Jobava (the j of "joc") and Miles ("mi-les") were heard no better as `Giobava` and `Mails`.
  Nimzowitsch and Zukertort keep a z (heard "nimzovic", "zucărtort"), as many Romanian players say them; Colle keeps its
  e ("Cole", probe); Breyer is a close call ("Breier"). A line-initial "Șah" before a word loses its h ("Șah dublu" near
  "Șa dublu", probe; heard "Dacă dublu", "Ce-a înseamnă" 5 times in 6), with no other spelling; inside a line and in
  "Șah mat" the h is said. The u of "mutare" is heard "o" about one time in three ("motare"); the probe cannot tell u from
  o, and `muutare` did no better. "pioni" ends with a vowel heard (probe near "pionă"), heard "pion" or "pionii";
  "partide", "partidele" are heard "partii de", "partitele" nearly always (a tie in the probe). câmp is said close to
  "cămp" (probe); "cîmp" is read otherwise.
- Recogniser habits: it drops the diacritics (ș, ț, â, ă; ş with a cedilla), writes Negrul as "Negru-l", a line-initial
  "Dama" as "Da, ma" (the probe has "Dáma"), "dama ta" as "damata", "Hopa" as "Opa", regele as "regile", Albul as
  "Arbul", squares and numbers as notation ("F7", "30 și 4"); it drops a line's first words and loops on some long lines
  ("adesea adesea ...").
