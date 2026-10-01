Scacelith coach voice
=====================

This folder holds the text-to-speech model the coach of Scacelith speaks with: Supertonic 3 by
Supertone Inc. (https://huggingface.co/Supertone/supertonic-3), in the 8-bit (INT8) ONNX conversion
published by the sherpa-onnx project (sherpa-onnx-supertonic-3-tts-int8-2026-05-11).

WHERE THE FILES CAME FROM. {source} Every file was checked against its SHA-256 before being kept.
Scacelith does not redistribute this model: the files are not part of the Scacelith program, are
not covered by its GPL-3.0 licence, and were fetched from their publishers on this computer at the
player's request.

LICENCES. The Supertonic 3 model is licensed under the BigScience Open RAIL-M licence, in
Supertonic-3-OpenRAIL-M.txt. Its paragraph 5 and Attachment A list use restrictions that apply to
anyone using or redistributing the model or these files, and you must comply with them; among
them, content it generates must be disclosed as machine generated (restriction e) and must not be
used to impersonate anyone (restriction g). The coach is a robot character and its voice is
synthetic. LICENSE in this folder is the MIT licence of Supertone's sample code, copied by
sherpa-onnx from Supertone's repository: it does not cover the model. README.md is Supertone's
description of the model.

MODIFIED FILES. vector_estimator.int8.onnx and vocoder.int8.onnx are modified versions of the
Supertonic 3 model: they were quantized to 8-bit integers by the sherpa-onnx project.
duration_predictor.int8.onnx and text_encoder.int8.onnx are the original graphs; unicode_indexer.bin
and voice.bin are binary conversions of the original character indexer and voice styles; tts.json is
the original configuration. Scacelith uses these files unchanged.

Deleting this folder is harmless: the coach then shows its lines as subtitles only, and offers to
download its voice again.
