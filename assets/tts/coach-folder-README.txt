Scacelith coach voice
=====================

This folder holds the text-to-speech model the coach of Scacelith speaks with: Supertonic 3 by
Supertone Inc., in its official ONNX release (https://huggingface.co/Supertone/supertonic-3, where
Supertone describes the model).

WHERE THE FILES CAME FROM. {source} Every file was checked against its SHA-256 before being kept.
Scacelith does not redistribute this model: the files are not part of the Scacelith program, are
not covered by its GPL-3.0 licence, and were fetched from their publisher on this computer at the
player's request.

THE FILES. They are Supertone's files, unmodified, placed side by side without the subfolders of
Supertone's repository. duration_predictor.onnx, text_encoder.onnx, vector_estimator.onnx and
vocoder.onnx (onnx/ in the repository) are the four neural networks of the model; tts.json (onnx/)
is its configuration; unicode_indexer.json (onnx/) maps the characters of a text to the model's
input; M3.json (voice_styles/) is the voice style the coach speaks with. Scacelith uses these
files unchanged.

LICENCES. The Supertonic 3 model is licensed under the BigScience Open RAIL-M licence, in
Supertonic-3-OpenRAIL-M.txt (the LICENSE file of Supertone's repository). Its paragraph 5 and
Attachment A list use restrictions that apply to anyone using or redistributing the model or these
files, and you must comply with them; among them, content it generates must be disclosed as machine
generated (restriction e) and must not be used to impersonate anyone (restriction g). The coach is
a robot character and its voice is synthetic.

Deleting this folder is harmless: the coach then shows its lines as subtitles only, and offers to
download its voice again.
