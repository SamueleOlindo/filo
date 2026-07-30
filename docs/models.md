# Models

Filo can use two models, and it works without either. This is how they get
onto the machine, how Filo finds them, and what each one is worth.

## The short version

Drop two GGUF files into `%LOCALAPPDATA%\filo`:

- an **embedding model**, around 115 MB, which turns on search by meaning;
- an **instruction model**, roughly 2 GB, which turns on question parsing and
  the file descriptions on the space screen.

Then start Filo. There is no setup screen and nothing to configure.

**Filo does not download models.** That is not implemented. You fetch the GGUF
files yourself, from wherever you normally get them, and put them in that
folder. Models are large and licensed separately from Filo, which is also why
`*.gguf` is in `.gitignore`.

## Without either model

Names and content still work, and they are the two layers most searches use.
You keep:

- every file on the volume, searchable by name;
- full-text search over everything the indexing pass extracted;
- the query planner, which still lifts formats, folders and dates out of a
  query using its own vocabulary, in Italian and English;
- the whole space screen, minus the one-sentence explanations.

What you lose is the third result list — the one that finds a document about
the thing you asked for when it does not contain your words — and the ability
to type a whole question instead of keywords.

The window's status line tells you where you stand: it says `content indexed`
when the embedding model is not in use, and `content + meaning indexed` when it
is.

## How Filo finds them

`findModels()` in `src/main.cpp` looks in two folders, in this order:

1. the folder the executable is in, so a portable copy can carry its own
   models and stay one folder;
2. `%LOCALAPPDATA%\filo`, where anything added after installation goes.

In each folder, in this order:

1. the exact names `model.gguf` (embedding) and `question.gguf` (instruction);
2. failing that, **any** `*.gguf` in that folder, assigned to the two roles by
   size.

The size threshold is **700 MB**. A file of 700 MB or more is taken to be the
instruction model; anything smaller is taken to be the embedding model. That
line is not a measurement of anything, it is just a gap: an embedding model is
a hundred-odd megabytes and a generative one is a couple of gigabytes, and
nothing else that would plausibly be sitting in that folder falls between them.

Three details worth knowing before you rely on it:

- **The first file found for a role wins.** Once the embedding slot is filled,
  later candidates are ignored, and the executable's folder is searched before
  the data folder. So a model next to the binary beats one in `%LOCALAPPDATA%`.
- **The scan is not recursive** and skips directories. A model in a subfolder
  is not found.
- **Nothing checks that the file is what its size implies.** Two GGUF files
  under 700 MB in the same folder means one of them is picked as the embedding
  model, and which one depends on the order the file system hands them back —
  not on anything you can predict. If that could happen to you, use the exact
  names, or pass the paths explicitly.

The first version of this demanded `model.gguf` and `question.gguf` and printed
"model cannot be loaded" when they were absent. That is a true statement and a
useless one, when the user has a perfectly good model sitting in the same
folder under its real name.

### Saying it explicitly

```
Filo.exe --model <path.gguf>            embedding model for the window
Filo.exe --question-model <path.gguf>   instruction model
Filo.exe C: --embed <path.gguf>         embedding model for the indexing pass
```

`--model` and `--embed` are different things and are deliberately not the same
flag. `--model` says which model the *window* should search with; `--embed`
starts an embedding run over the whole index and never opens a window.

A path passed on the command line replaces whatever the search above found. A
`--question-model` pointing at a file that does not exist is ignored rather
than being an error.

## The embedding model — about 115 MB

### What it buys

A third result list. Searching for `penale` also finds the contract that only
ever says `clausola risarcitoria`, because the two sit near each other in the
model's vector space and share no letters at all. Those results are fused with
the name and word lists by rank, with a meaning vote worth half a word vote.

The model needs to be small and multilingual. It is not being asked to reason,
only to place a sentence in the right spot, so under about 120 million
parameters is the right size and a large model buys nothing here. On the
development machine the model in use produced 384 dimensions; Filo reads the
number from the model and quantises the stored vectors to one signed byte per
dimension, which costs a little precision and three quarters of the memory.

### What it costs

**It is not enough on its own.** Search by meaning needs three things: the
model, a content index, and a vector file. In order:

```
Filo.exe C: --index               build the content index
Filo.exe C: --embed <model.gguf>  compute a vector per chunk
```

The second one is long. It reads and re-splits every indexed document, because
the FTS5 table is contentless and does not keep the text, then runs the model
over every chunk. On the development machine that produced 579,190 vectors, a
231 MB vector file, and 74 minutes of which 94.5% was the model itself at 7.2 ms
per chunk. Two things make it bearable: documents are
processed in order of value, so contracts and invoices become searchable by
meaning long before the run ends, and byte-identical chunks are computed
once — 13% of them on a real disk, which is licence headers, template
boilerplate and backup copies of the same website.

The run is resumable. If it is interrupted, starting it again skips what it
already did without re-reading those files, and it checkpoints as it goes.

**Startup.** The model is loaded before the window appears, which takes about a
second.

**Changing model means redoing the pass.** The vector file records what it was
built with: an identity made from the model's size and the first megabyte of
its GGUF header, the number of dimensions, the chunker version and the content
index generation. If any of those does not match at startup, the file is
rejected and search by meaning stays off — vectors from two different models
are not comparable, and mixing them would produce a search that is subtly and
unfixably wrong. Note that the identity is **not** the file name, on purpose:
copying a model into the folder often renames it, and a rename is not a model
swap.

### Choosing one

Three commands report on a model before you spend hours on it. All three need a
build with llama.cpp:

| | |
|---|---|
| `--evalmodel <path>` | MRR and P@5 over the evaluation corpus, with the worst case named |
| `--embedtest <path>` | one ranking task with distractors, plus cost on realistic text |
| `--embedbench <path>` | batch size, thread count, and how cost scales with length |

`--evalmodel` is the one to trust. Similarity between two sentences says almost
nothing on its own — a model that scores everything 0.96 can still rank
correctly, and one that ranks badly is useless however reasonable its numbers
look. What matters is whether, shown many texts, it brings the right ones to
the top. The corpus it measures against is Italian, because that is what Filo
was built to search.

Two model-specific things that produce no error and only worse results:

- **Pooling.** Filo does not force it; llama.cpp reads from the model file how
  it wants to be pooled. E5 models average every token, Granite and others read
  the first. `FILO_POOLING=cls|last|none` overrides it for testing.
- **Prefixes.** The E5 family wants `passage: ` and `query: ` in front of the
  text, and other families explicitly say not to use them. `FILO_E5_PREFIX`
  turns them on — but **only in `--embedtest` and `--evalmodel`**. The window
  and the `--embed` pass never add prefixes. Worth knowing before you choose an
  E5 model on the strength of an `--evalmodel` score measured with them.

One more knob: `FILO_MIN_SIMILARITY` sets the floor below which a vector hit
does not count as a hit at all. It defaults to 0.65. A nearest-neighbour search
has no way of saying "nothing matched" — it always returns its top results,
however unrelated — so this is what separates an answer from the closest
available noise.

## The instruction model — roughly 2 GB

### What it buys

Two things.

**Questions.** "Quanto ho pagato di tasse l'anno scorso" is not a keyword
search. The model reads it and returns three fields: the subject to search for,
a format, and a time — each one chosen from a closed list, or `any`. Filo then
searches for `tasse pagate` over last year, using exactly the same machinery as
every other query.

The output space is closed on purpose. The reply is constrained by a grammar,
so the model physically cannot emit anything outside those enumerations. The
failure mode is "picked the wrong option from a list we wrote", never "invented
a date format nobody parses", and that holds without a line of defensive
parsing. If the call fails for any reason, the query plan is left exactly as it
was: the model is an improvement, never a dependency.

**Descriptions on the space screen.** Most of what that screen says is a fixed
string — explaining `node_modules` with a 3B model would be a party trick. The
model earns its place in one spot: telling you what a file is when the rules
could not, so you can decide about it without opening it. That output is prose
nothing verifies, so the interface labels it as the model talking and keeps it
separate from the reasons Filo is sure of.

### What it costs

**Nothing at startup.** The file is only opened the first time a query actually
reads like a question, on the search thread — about three seconds for a 3B
model, paid once, and invisible inside a search that was already asynchronous.
Someone who only ever searches keywords never pays it at all.

After that, measured on the development machine: **0.9 to 1.1 seconds** for a
query, and **0.7 to 4.2 seconds** for one file description. An earlier version
of this page said "a few hundred milliseconds", which was optimistic by
threefold against its own benchmark.

At that price it is asked as rarely as possible. The query has to be at least
three words long, be under 200 characters, look like a question, **and** the
cheap layers have to have come back with nothing: names, full text and meaning
all run first, and the model is only consulted when none of them found anything.
It used to run before all three, which put a second on every question-shaped
query including the ones about to be answered perfectly well without it.

There is one context, shared by the search thread and the space screen, and
search never queues behind it. If the model is mid-sentence explaining a file,
the query goes through the other four layers and is none the worse for it.

### Choosing one

Both prompts are written with the **Llama 3 chat template** —
`<|begin_of_text|>`, `<|start_header_id|>`, `<|eot_id|>` — spelled out in the
source rather than read from the GGUF. A model from another family will still
answer, because the grammar forces the shape of the reply either way, but it is
being addressed in a dialect that is not its own and it will do worse. The
context is 1024 tokens, which is the prompt plus a very short answer, so a
model with a smaller window than that will not work at all.

Around 3B parameters is what the timings above were taken with. Bigger buys
slower, and there is not much for a bigger model to be right about: it is
filling three blanks from closed lists, or writing one sentence about a path.

### Checking one

```
Filo.exe --asktest <path.gguf>        what it makes of eight real questions
Filo.exe --describetest <path.gguf>   what it makes of eight real file paths
```

Neither is pass or fail, because there is no ground truth for "what did this
person mean". They print what the model decided so it can be read. The paths in
`--describetest` include ones where the honest answer is "I do not recognise
this", which is the answer that matters most and the one a model is least
inclined to give.

## The graphics card

If the build has the Vulkan backend and the machine has a Vulkan loader, both
models use the GPU, and llama.cpp falls back to the CPU on its own when it
cannot. `FILO_FORCE_CPU` pins them to the CPU whatever the build supports;
`FILO_LLAMA_LOG` brings back everything llama.cpp prints, which is the only way
to find out why a model would not load.

A build without llama.cpp ignores model files entirely: the classes are there,
they answer "not available", and the search box works exactly as it does when
the files are simply missing. See [building.md](building.md).
