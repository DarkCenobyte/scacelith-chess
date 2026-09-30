Scacelith coach voice
=====================

This folder holds the text-to-speech model the coach speaks with: Supertonic 3 by Supertone Inc.
(https://huggingface.co/Supertone/supertonic-3). Scacelith reads these files at run time; they are
not part of the Scacelith program and are not covered by its GPL-3.0 licence.

MODIFIED FILES. vector_estimator.int8.onnx and vocoder.int8.onnx are modified versions of the
Supertonic 3 model: they were quantized to 8-bit integers by the sherpa-onnx project (release
sherpa-onnx-supertonic-3-tts-int8-2026-05-11). duration_predictor.int8.onnx and
text_encoder.int8.onnx are the original graphs; unicode_indexer.bin and voice.bin are binary
conversions of the original character indexer and voice styles. Scacelith redistributes all of
these files unchanged from that release.

The model is licensed under the BigScience Open RAIL-M licence, in Supertonic-3-OpenRAIL-M.txt.
Its paragraph 5 and Attachment A list use restrictions that apply to anyone using or redistributing
the model or these files, and you must comply with them; among them, content it generates must be
disclosed as machine generated (restriction e) and must not be used to impersonate anyone
(restriction g). The coach is a robot character and its voice is synthetic.

Deleting this folder is harmless: the coach then shows its lines as subtitles only.
