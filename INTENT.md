# Intent schema

The narrator turns world state into prose. This schema covers the other
direction: turning what a player said into something the engine can act on.

    voice --[Whisper]--> text --[classifier]--> intent --> engine
                                                             |
                                              world state <--+
                                                             |
                                        narrator LLM <-------+
                                                             |
                                                    Piper --> speech

The engine is authoritative. The LLM never decides what happened; it only
describes what the engine already decided. That is the whole point of the
split, and it is what keeps a small model from inventing state.

## Why classification, not parsing

A MUD command space is closed. There are perhaps forty verbs and, in any
given room, a handful of nouns. That is a classification problem with a
fixed output space, not a generation problem.

This matters for the hardware. A verb classifier is a fixed-shape encoder
with no KV cache -- exactly what an NPU runs well, and what a 2-bit LLM
runs badly. It also sidesteps the open-vocabulary problem: the engine
already knows what is in the room, so the model picks from a candidate set
rather than inventing strings from audio.

## The intent record

One line, tab-separated, so it can be read at an offset without parsing the
whole file:

    verb <TAB> object <TAB> indirect <TAB> confidence <TAB> raw

    verb        one of the closed verb set below
    object      entity id from the room's candidate set, or empty
    indirect    entity id for "with"/"to"/"at" arguments, or empty
    confidence  0.00 - 1.00, the classifier's margin
    raw         the original transcript, kept for logging and appeals

Example:

    EXAMINE→terminal→→0.94→uh, go look at that terminal thing

Empty slots stay empty rather than absent, so field count is constant and a
reader can split on tab without counting.

## Verb set

Closed, and deliberately small. Anything outside it is UNKNOWN, which the
engine surfaces as "I don't understand" rather than guessing.

Movement      GO NORTH SOUTH EAST WEST UP DOWN ENTER EXIT
Perception    LOOK EXAMINE LISTEN SMELL TOUCH READ SEARCH
Manipulation  TAKE DROP OPEN CLOSE PUSH PULL TURN UNLOCK LOCK
Inventory     INVENTORY WEAR REMOVE WIELD
Interaction   TALK ASK TELL GIVE SHOW ATTACK USE
Meta          WAIT AGAIN HELP QUIT SAVE
Fallback      UNKNOWN

Compass directions are their own verbs rather than GO+direction, because
"north" is by far the most common utterance in a MUD and deserves the
shortest path through the classifier.

## Slot filling

The engine supplies a candidate set with each classification request: the
entity ids visible in the room, plus the player's inventory, plus exits.
The classifier scores the utterance against those candidates rather than
against an open vocabulary.

    candidates: terminal, stair, tower, lamp, self
    utterance:  "look at that warm screen thing"
    ->          EXAMINE  terminal  ""  0.81

This is what makes the model small. It never needs to know the word
"terminal" in general -- only that this utterance refers to one of five
things the engine named.

Unresolvable references produce empty slots with lowered confidence, and
the engine asks for clarification. That is better than guessing, because a
wrong guess mutates world state.

## Confidence and the ladder of failure

    >= 0.75   act on it
    0.40-0.75 act, but have the narrator hedge ("you reach for the
              terminal, unsure if that is what you meant")
    < 0.40    do not act; ask which thing was meant
    UNKNOWN   do not act; narrator says it did not understand

Speech recognition compounds error, so the middle band matters more here
than in a typed MUD. A hedge is cheap; an unintended action is not.

## Exposure over 9P

The same pattern as the narrator: files, one connection per player.

    /intent/ctl        write: candidate set for the current room
    /intent/utterance  write: transcript text
    /intent/result     read:  the intent record above
    /intent/info       read:  model, backend, confidence thresholds

A voice client writes the transcript and reads the record. A text client
skips Whisper and writes directly. The engine consumes records either way,
so speech is an input method rather than a separate code path.

Placing this behind 9P means the classifier can live on whichever machine
suits it -- the NPU node once its driver supports one, a CPU box until
then -- without any client changing.

## Why this improves narration

Today the player's raw text goes straight into the narrator prompt, so the
model is simultaneously parsing intent and writing prose. Separating them
means the narrator receives a resolved action and authoritative state:

    System: <persona>
    World:  <pinned room state>
    Event:  the player examines the dead terminal
    ->      two sentences of description

That is a much easier generation task than "figure out what they meant and
also be evocative", and it is likely to improve output quality more than
any amount of prompt tuning.

## Hardware fit

    Whisper encoder-decoder   fixed shapes, no KV cache   -> NPU
    Intent classifier          small encoder, int8         -> NPU
    Narrator LLM               open-ended, KV cache        -> GPU or CPU
    Piper TTS                  ~7x realtime                -> CPU

The two NPU jobs are the bounded ones. Neither needs ternary weights, which
is why they fit hardware that cannot run BitNet.

## Open questions

- Which encoder converts cleanly to RKNN. MiniLM-class is the obvious
  starting point; needs checking against the toolkit's supported ops.
- Whether verb and slot are one multi-head model or two passes. One pass is
  faster; two are easier to train and debug.
- How multi-object commands ("put the lamp in the bag") are represented.
  The indirect slot covers it, but the training data has to reflect it.
- Whether to keep a rule-based parser as a fast path. Exact matches like
  "north" or "inventory" need no model at all, and skipping it saves both
  latency and error.
