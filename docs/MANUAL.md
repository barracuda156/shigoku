# shigoku — user manual

shigoku is a terminal application for finding, tracking, and watching anime
without leaving the terminal. It searches AniList's catalogue, renders cover
art directly in supporting terminals, resolves and plays episodes through
`mpv` (with resume and optional auto-skip of openings/endings), and keeps a
local watchlist that can optionally sync to your AniList and MyAnimeList
accounts. It's built to run comfortably on old hardware too, PowerPC Macs
included. The screen is deliberately kept sparse — a handful of focused
views rather than one crowded dashboard — so this manual exists to make sure
nothing useful stays hidden behind a key you never happened to try. A
companion app, `shigoku-manga`, does the same for manga (see
[shigoku-manga](#shigoku-manga-the-manga-reader)).

## Quick start

1. Run `shigoku` with no arguments — this opens the interactive interface (the "TUI").
2. Press `/` to start a search (you land in Browse automatically).
3. Type a title, press `Enter` to lock the search in and move focus to the results.
4. Move to a result with `j`/`k` (or click it), then press `Space` to open its page.
5. On the show's page, move around the episode grid with `h`/`j`/`k`/`l`, and press `Enter` to play the highlighted episode in `mpv`.
6. Press `q` to quit — the terminal is restored cleanly. Your playback position was checkpointed as you watched, so next time picks up where you left off.

**Requirements:** any terminal emulator; `mpv` on your `PATH` for playback
(the default player — alternates are covered under [Playback](#playback));
a network connection for searching and streaming (an episode you've already
downloaded plays with no network at all). Cover art needs a terminal that
can draw images: the Kitty graphics protocol (kitty, WezTerm, Konsole),
iTerm2's inline images (iTerm2, mintty), or SIXEL (mlterm, foot, xterm with
SIXEL on). shigoku asks the terminal at startup and picks the best one it
answers to; any other terminal shows a plain placeholder box instead and
works exactly the same otherwise.

## How the screen is organized

- **Top bar** — `SHIGOKU` plus five tabs: `[B]rowse [H]istory [D]iscover
  [C]alendar [S]ettings`. The active tab is highlighted; letters in brackets
  are the keys that jump straight to it from anywhere.
- **Wide terminal (60+ columns)** — Browse and History split into a list
  pane (left) and a detail preview pane (right). Below that width there's
  only the list, and opening a show jumps straight to its full page instead.
- **The show's page** — a dedicated full-screen page for one show: cover,
  synopsis, episode grid, and (once toggled) Characters & Recommendations.
  See [The show page](#the-show-page) below.
- **`Space` — the one universal zoom toggle.** Press it on any selected
  show or card, in any view, to jump to its full page. Press it again
  inside that page to go back to wherever you came from. `Esc` does the
  same "go back" job from inside the page or a preview pane.
- **Mouse** — click selects whatever is under the pointer (a list row, a
  Discover card, a Settings row, an episode cell, a top-bar tab); a second
  click on the same spot shortly after is the same as pressing `Enter`; the
  scroll wheel is the same as the up/down arrow, wherever the pointer sits.
  This degrades gracefully — it works even on terminals that only report
  plain button presses, not just modern ones.
- **`Ctrl-C`** quits immediately from anywhere, without the save-on-quit
  step that plain `q` does (see [Settings](#settings-key-s)).

## Browse (key `B`)

Search the whole catalogue — AniList's, or MyAnimeList's when AniList
cannot answer (see [Catalog sources](#catalog-sources)).

| Key | Action |
|---|---|
| `/` | Start typing a search query |
| `Enter` (while typing) | Lock in the query, move focus to the results list |
| `Esc` (while typing) | Cancel the search box |
| `j`/`k`, arrows, wheel | Move through the results |
| `g`/`G` | Jump to the first/last result |
| `l`, `Enter`, double-click | Open the detail preview pane on a wide terminal, or the full page directly on a narrow one |
| `Space` | Open the show's full page directly, from any width |
| `P` | Add the highlighted show to your watchlist (also fires a quiet background check of other sources, so a later play starts instantly) |
| `~` | Shows like this one, in Discover (see [Similar titles](#similar-titles-)) |
| `q` | Quit |

Scrolling to the last loaded result quietly fetches the next page.

## History (key `H`)

Your watchlist, grouped by status.

| Key | Action |
|---|---|
| `j`/`k`, arrows, wheel | Move through the list |
| `g`/`G` | Jump to the first/last entry |
| `/` | Filter the list in place by title — local and instant, no network |
| `f` / `F` | Narrow the list — Watching → Planning → Paused → Completed → Dropped → *behind* (Watching shows with aired episodes you haven't watched) → all — and back to all. The group header says when a filter is on |
| `l`, `Enter`, double-click | Open the detail preview pane on a wide terminal, or the full page directly on a narrow one |
| `Space` | Open the full page directly, from any width |
| `w` / `p` / `c` / `x` / `P` | Set status: Watching / Paused / Completed / Dropped / Planning |
| `s` | Open a score prompt: type `0`–`10` (decimals allowed, e.g. `7.5`), `Enter` to save, or leave it blank and `Enter` to clear the score |
| `r` | Recompute progress from your actual playback history |
| `u` | Undo the last status change (one level only) |
| `X`, then `y` | Delete the show and its local episode history — a second key press confirms; anything else cancels |
| `~` | Shows like this one, in Discover |
| `q` | Quit |

Each row shows your own score at the right (`★ 8.5`) when you have one.
A show that aired a new episode since you last checked carries a `NEW` tag
in this list until you open it (see [Calendar](#calendar-key-c) for where
that comes from). Your score is stored as a plain 0–10 number and converted
automatically to whatever scale your AniList account actually uses
(100-point, 10-point, 5-star, etc.) whenever it syncs — you never need to
think about the conversion yourself.

A Watching show with episodes out that you haven't watched yet carries an
amber `+2 aired` tag, and its progress bar shades those episodes (`▓`) a
step darker than the ones you've seen (`█`), so the bar reads watched /
aired / still to come. Both go by the airing times the Calendar keeps, so
they cover the shows you're Watching or Planning, and they clear by
themselves as you catch up — unlike `NEW`, which clears when you open the
show. The *behind* stop of the `f` filter lists just those shows.

## Discover (key `D`)

A cover-art grid across four ranking feeds. When the feed comes from
MyAnimeList instead of AniList a `MAL` chip sits at the right of the top
bar; if neither can answer, the grid says why ("AniList blocked us (403);
MAL: no client id") rather than guessing at your connection.

| Key | Action |
|---|---|
| `h`/`j`/`k`/`l`, arrows, wheel | Move the grid cursor |
| `g`/`G` | Jump to the first/last card of the feed |
| `1` / `2` / `3` / `4` | Jump straight to Trending / Popular / Top Rated / This Season |
| `5` | Back to the Similar feed, once one is open |
| `[` / `]` | Step to the previous/next feed |
| `f` | Open the filter overlay (below) |
| `Enter`, double-click | Open the show's full page |
| `Space` | Same, from anywhere in the grid |
| `P` | Add the highlighted show to your watchlist |
| `~` | Shows like the highlighted one (a new Similar feed) |
| `/` | Jump to Browse with a text search open — Discover itself has no free-text search |
| `q` | Quit |

### Similar titles (`~`)

`~` on any show — in Browse, History, the Calendar, a Discover card, or the
show's own page — opens Discover on a fifth feed, `[5] Similar to <title>`:
the shows AniList's users recommend alongside it, most voted first, as many
as scrolling asks for. When AniList can't answer, it falls back to the
handful MyAnimeList lists for the show. The filter overlay applies here
too. The feed stays as the fifth tab, so `5`, `[`/`]` or a click brings it
back, until `~` on another show replaces it.

### Filter overlay (`f`)

Narrows the active feed by genre, year, status, and minimum score. On a
MyAnimeList-served feed the narrowing happens on this side of the wire, so
a page can come back short; the grid keeps fetching until something
matches or five pages have come back empty.

| Key | Action |
|---|---|
| `j`/`k` | Move between the Genre / Year / Status / Minimum score rows |
| `h`/`l` | Change the highlighted row's value (on Genre, this walks the genre list instead) |
| `Space` | Toggle a genre on or off (Genre row only) |
| `c` | Clear every filter back to defaults |
| `Enter` | Apply and close |
| `Esc` | Discard and close |

## Calendar (key `C`)

Shows upcoming episodes for the shows you are Watching or Planning, one
group per day starting today (`today · sat 24 oct`, `tomorrow · sun 25 oct`,
`mon 26 oct`, …), with today's heading highlighted and days with nothing
airing left out. The week runs through the same weekday next week; anything
further off, such as a season premiere, sits in a final `later` group with
its date on the row. Each row leads with the local airing time (24-hour)
and ends with the episode number — `premiere` for an episode 1 — and, for
anything airing within the next 24 hours, a countdown such as `in 3h 12m`.
Dates and times follow your computer's time zone, daylight saving
included. Airing times come from AniList's public schedule and are refreshed
on every launch and after every sync for any followed show that lacks one
or whose last one has passed — no account needed, and a show that reached
your list through MyAnimeList is looked up by its MAL id.

| Key | Action |
|---|---|
| `j`/`k`, arrows, wheel | Move the selection |
| `g`/`G` | Jump to the first/last entry |
| `Enter`, `Space`, double-click | Open the show's full page |
| `q` | Quit |

On launch, and after every account sync, shigoku checks for episodes that
aired since you last looked and raises one toast such as "2 new episodes
aired". The affected shows pick up the same `NEW` tag shown in History,
cleared the moment you open them.

## Settings (key `S`)

Every row follows the same pattern: `j`/`k` moves between rows, `h`/`l`
cycles a value, `Space` flips a toggle, `Enter` edits a text field or fires
a connect button. `q` (or switching to another view) saves everything to
disk.

| Section | Row | Type | What it does |
|---|---|---|---|
| Player | mpv path | text | Binary used by the default player backend |
| | default quality | cycle | `worst` / `480` / `720` / `1080` / `best` |
| | translation | cycle | `sub` / `dub` |
| | resume offset | cycle | `0`/`3`/`5`/`10`/`15`/`30`s — back up this many seconds from your last checkpoint when resuming |
| | skip mode | cycle | `none` / `intro` / `outro` / `both` — AniSkip auto-skip (`mpv` only) |
| Catalog | preferred provider | cycle | Pin a specific streaming source as the default; blank follows the built-in order automatically |
| Interface | cover art | toggle | Fetch and render cover art |
| | kanji chips | toggle | Show season chips in kanji instead of plain text |
| | palette | cycle | `terminal_ghost` / `phosphor` / `nord` / `tokyonight` — only `terminal_ghost` currently has a visible effect |
| | transparent background | toggle | |
| | landing view | cycle | `history` / `browse` / `last_watched` — which view opens on launch; `last_watched` opens History with your most recently watched show already selected and its detail open |
| | title language | cycle | `romaji` / `english` / `native` |
| AniList Sync | connect | action | Opens your browser to sign in — see [Accounts and sync](#accounts-and-sync) |
| | sync enabled | toggle | Turns the *automatic* background pull/push on or off |
| Updates | check for updates | toggle | Checks GitHub for a newer release on launch (never self-updates) |
| MyAnimeList | mal client id | text | Your own registered MAL app's client ID; blank shows `(built-in)` and uses shigoku's own registration |
| | connect | action | Opens your browser to authorize the MAL mirror |
| Downloads | download dir | text | Where downloaded episodes are stored; blank = the default, under the data directory |
| | ffmpeg path | text | Binary used to save HLS streams to disk |
| Player backend | player | cycle | `mpv` / `mplayer` / `qmplay2` — see [Playback](#playback) |
| | player path | text | Binary for whichever *non-mpv* backend is selected; blank = that backend's default name. `mpv` always uses "mpv path" above instead |
| Catalog source | catalog | cycle | `auto` / `anilist` / `mal` — which site answers search and Discover; see [Catalog sources](#catalog-sources) |

## The show page

Every show — from any view — opens into this same full-screen page: cover,
title, status/score chips, a synopsis, and the episode grid.

| Key | Action |
|---|---|
| `h`/`j`/`k`/`l`, arrows | Move the episode grid: `j`/`k` hop a whole row, `h`/`l` step one cell |
| `g`/`G` | Jump to the first/last episode |
| `Enter`, double-click | Play the highlighted episode |
| `v` | Cycle which streaming source serves this show — automatic → each source in turn → back to automatic. shigoku otherwise picks a source and falls back between them on its own |
| `d` | Download the highlighted episode instead of playing it |
| `c` | Toggle the **Characters & Recommendations** section |
| `~` | Open the full list of similar shows in Discover |
| `Tab` (section open) | Move focus between the section and the episode grid |
| `j`/`k`, `g`/`G` (section focused) | Move / jump within Characters & Recommendations |
| `Enter` (a recommendation focused) | Open *that* show's own page |
| `Space`, `Esc` | Close the page, back to wherever you opened it from |
| `q` | Quit |

Unwatched episodes are brighter, watched ones dim, and the one currently
playing or downloading carries a small spinner.

**Two things easy to miss:**

- On a wide terminal, Browse and History's smaller *preview* pane (opened
  with `l`/`Enter` next to the list, without leaving it) carries the same
  play / `v` / `d` keys as the full page — but the **Characters &
  Recommendations section only opens on the full page**; `c` does nothing
  in the preview pane. If you always browse in the split view, that section
  can go entirely unnoticed.
- Status and score (`s`, `w`/`p`/`c`/`x`/`P`) are edited from the History
  list only — the show page displays your score ("you: 7.5") but doesn't
  let you change it there.

## Playback

`mpv` is the default backend and the only fully-featured one: live position
tracking, checkpointed resume, and (if configured) AniSkip auto-skipping of
openings/endings. Resume offset and skip mode both live under Settings →
Player. Whenever AniSkip knows an episode's opening or ending, they also
show as chapters on mpv's seek bar (Opening / Episode / Ending), so you can
see where they are and jump with mpv's chapter keys even with auto-skip
off.

A play counts as a watch — the show joins History, moves to Watching, and
reaches your AniList and MAL lists — only once at least half of the episode
has played (the same half-way rule music scrobblers use). A shorter play,
such as a quick check that a stream works, keeps its resume point and
changes nothing else. The episode itself is marked as watched, advancing
your progress, once it reaches its natural end (about 80%).

Two alternate backends exist for machines where `mpv` itself is
impractical, chosen via Settings → Player backend → *player*:

| Backend | Position tracking | Resume | Auto-skip |
|---|---|---|---|
| `mpv` (default) | Yes, live | Yes | Yes |
| `mplayer` | Yes (slave mode) | Yes | No |
| `qmplay2` | No — launch only | No — always starts at 0; mark progress by hand | No |

"Player path" in Settings sets the binary for whichever *alternate* backend
is chosen; `mpv` always uses its own "mpv path" row instead. Only one
playback can be in flight at a time.

### Senshi needs Node.js

The Senshi source hands out its streams through the site's own player
script, so shigoku runs that script under Node.js (18 or newer) for the
moment it takes to fetch the stream URL; nothing from npm is involved, and
the other sources need nothing extra. Node is looked up as `node` on PATH,
or at the path in the `senshi_node` key of `config.json`; set that key to
`""` to turn the step off. Without Node, Senshi is skipped for playback and
the walk moves on to the next source — its search results and episode
lists still show.

## Downloads

Save an episode to disk instead of streaming it.

| Where | How |
|---|---|
| The show page or preview pane | Press `d` on the highlighted episode |
| Command line | `shigoku download <title> [<episode>]` |

Files land under `<download dir>/<AniList id>/<sub or dub>/<episode>.<ext>`
("download dir" is set in Settings; blank defaults to `<data dir>/downloads`
— see [Files on disk](#files-on-disk)). A command-line download of a show
the catalogue couldn't place (see [Command line](#command-line)) goes under
its source's name and id instead, such as `hianime-86/`, so two such shows
never share a folder. A transfer writes to a `.part` file
and only renames it to the final name once it completes; an interrupted
download resumes from where the `.part` left off next time, and a `.part`
never counts as a finished episode. Only one download runs at a time.

Playing an episode you've already downloaded (`Enter` on it) always plays
the local file directly — no network, no source lookup — so a fully
downloaded show still plays even if every streaming source is down.

## Accounts and sync

shigoku works fully offline with a local watchlist; connecting an account
is entirely optional.

### AniList

Settings → AniList Sync → *connect* opens your browser to approve access,
then waits for the redirect on `127.0.0.1:8767`. If the browser doesn't
open on its own, or nothing answers at that address, press `c` on the
"connecting" screen to copy the URL and paste it in yourself; `Esc` stops
waiting (a callback that's already arrived still completes in the
background). Once connected:

- your list pulls in automatically on launch;
- a status, progress, or score change pushes automatically a few seconds later;
- Settings → *sync enabled* turns this automatic behaviour off without disconnecting you;
- quitting (`q`) waits up to a second for a final push before exiting, so a same-session edit is never silently lost.

### MyAnimeList (optional second tracker)

Settings → MyAnimeList → *connect* works the same way as AniList's, using
shigoku's built-in app registration. To use your own instead, register an
app at `myanimelist.net/apiconfig` (redirect URL
`http://127.0.0.1:8767/mal/callback`) and enter its client ID under *mal
client id*. Once connected:

- your MAL list pulls into History right away and again on every launch —
  every status, with progress and score — so a MAL-only account gets a full
  watchlist too;
- status, progress, and score edits push back to MAL, alongside AniList when
  that is connected as well;
- a show only MAL knows about (no AniList counterpart yet) still plays,
  downloads, and mirrors like any other; it joins AniList sync once AniList
  can name it.

A MyAnimeList sign-in expires after about a month; shigoku renews it by
itself at the next sync after that. Only if the renewal is refused does it
ask you to connect again under Settings.

An edit made on the MAL site after your last push wins over shigoku's
older copy; an edit made here since then wins over MAL's (it pushes up on
the next sync), the same rule AniList sync uses.

### Catalog sources

Search, the Discover feeds, the show page's refresh and the genre picker
all read from a catalogue site. AniList is the primary one. When it cannot
answer (it has, for days at a time, switched its public API off for
third-party apps), shigoku switches to MyAnimeList for the next ten
minutes, shows a `MAL` chip in the top bar, and tries AniList again after
that. Settings → Catalog source → *catalog* pins one site instead of the
automatic switch.

A show found through MyAnimeList behaves like any other — it can be added
to the watchlist, played, downloaded, and mirrored to MAL — with a few
gaps MAL's API simply has no data for: no character list on the show
page, no next-episode countdown in the Calendar, and it stays out of
AniList sync until AniList is reachable again. Once it is, the first time
the show is selected shigoku asks AniList for its entry and moves the
show onto it: progress, watchlist status, downloads and source choices
come along, and from then on it syncs like any other show. A show that is
playing or downloading at that moment moves the next time it is selected.

## Command line

Running `shigoku` with no arguments opens the interface described above.
Everything below runs without opening it, and prints straight to the
terminal:

| Command | Does |
|---|---|
| `shigoku <title> [--dub\|--sub] [-q <quality>] [-S <n>] [-e <episode> \| -r <a>-<b>] [-p <source>]` | Search, then pick a result and an episode from numbered lists, then play it — and after each episode, choose next / replay / previous / quit. The flags skip the prompts (below) |
| `shigoku continue [<title>] [--dub\|--sub] [-q <quality>] [-S <n>] [-p <source>]` | Pick a show from your history back up where you left it: the freshest half-watched episode, or the one after the last you finished. Without `<title>` the whole history is offered, most recently watched first; with one, only the shows whose title contains it (a single match plays straight away) |
| `shigoku download <title> [<episode>] [--dub\|--sub] [-q <quality>] [-S <n>] [-p <source>]` | Same search-and-pick flow, but downloads instead of playing; giving `<episode>` (or `-e`) skips that last prompt (it matches an exact episode label first, then falls back to a 1-based position) |
| `shigoku login [--paste]` | Sign in to AniList from the terminal — a successful sign-in immediately runs a sync too. `--paste` swaps the automatic browser-and-listener flow for manually pasting the redirect URL |
| `shigoku sync` | Pull your MyAnimeList list and push edits back if connected, then pull and push your AniList list once, then exit — this always runs, even if Settings' *sync enabled* is switched off (that toggle only gates the interface's own automatic sync) |
| `shigoku update` | Check GitHub for a newer release — shigoku never updates itself; reinstall via your package manager |
| `shigoku --paths` | Print where config, database, cache, and `mpv` currently resolve to |
| `shigoku --version` / `-V` | Print the version and exit |
| `shigoku --debug` | Add to any command above for verbose diagnostics (stderr on the command line, a log file when running the interface) |
| `shigoku --ascii` | Add to any command above to keep its output to plain ASCII — automatic when the locale is not UTF-8 |

The flags that skip a prompt, for scripts and for when you know what you
want (each has a long form too: `--quality`, `--show`, `--episode`,
`--range`, `--provider`, with or without `=`):

- `-q <quality>` — `best` (the default), `1080`, `720`, `480`, or `worst`,
  for this run instead of the `default_quality` in `config.json`. Sources
  that offer one stream ignore it, as ever, and so does a stream whose
  audio comes as a separate track (Senshi's do): one picture size alone
  would play silent, so the player gets the whole ladder and picks the
  tracks itself. A spelling shigoku does not know plays best and says so.
- `-S <n>` — take the nth search result instead of asking.
- `-e <episode>` — play that episode: its label as the list shows it, or
  its position in the list. After it, the usual next / replay / previous /
  quit question follows.
- `-r <a>-<b>` — play the span from `<a>` through `<b>` (labels or
  positions) one after another, no questions asked between or after; leave
  `<b>` off (`-r 4-`) for "through the end". An episode that fails to play
  on every source (below) ends the run there, exit 1, like a single episode
  would.
- `-p <source>` — ask this source first (its short name: `senshi`,
  `hianime`, `animehub`, `anilibria`, `anidbapp`; `shigoku --paths` lists
  the ones a search can walk, in order), for this run instead of
  `preferred_provider`.
  A name shigoku does not know is said and the run walks as if unset.

```
shigoku frieren -S 1 -e 7        # the first result, episode 7, no prompts
shigoku frieren -r 1-4           # episodes 1 through 4 in a row
shigoku continue                 # pick up where you left off
shigoku continue frieren -q 720  # this show, at 720p if the source has it
```

After an episode plays, the same picker that chose it asks what next:
*next*, *replay*, *previous* (only when such an episode exists) or *quit*.
Enter a number, or `q` — or, piped from a script, let stdin's end answer:
it reads as quit, so `shigoku frieren -S 1 -e 7 </dev/null` plays the one
episode and exits.

`shigoku continue` reads the library `shigoku` keeps for its History view.
A show joins it the same way from the command line as from the interface:
once you have watched past the middle of an episode (a shorter look leaves
only its resume point, and counts for nothing), or when you add it to your
watchlist in the interface — a source that names shows by their
MyAnimeList id files them under the matching AniList entry, like the
interface's MAL catalogue does, and one whose results carry no id at all
(HiAnime, AnimeHub) is looked up in the catalogue by its title and filed
under the entry that clearly matches it; when none clearly does, that run plays
without joining the library. It asks the
source the show was last streamed from for its episode list — `-p` names
another, when the show is known there too — and, if that source is down or
no longer lists the show, searches for the title again and takes the result
that carries the show's AniList or MyAnimeList id without asking. All
episodes watched is a plain "you're caught up".

On a terminal or in a locale that has no UTF-8 — a serial console, an
`xterm` under `LANG=C`, an old Terminal.app whose font lacks the marks — the
command line's few glyphs (✓ ✗ ▶ ↺ · …) come out as plain ASCII (`+ x > < -
...`) instead of mojibake. That switch is automatic from the locale's
codeset; `--ascii` forces it, and `shigoku --paths` says which you get and
why.

Both pick steps use a numbered prompt by default. If an fzf-compatible
picker is installed — `fzf`, or `fzf++`, the C++ reimplementation that
builds on older Macs — and you are at a terminal, the pick opens in it
instead: type to filter, Enter selects, Esc backs out (that is the "bye"
the prompt's `q` gives). Two keys in `config.json` steer this: `cli_picker`
(`auto`, the default; `prompt`; or `fzf`) and `picker_path` (the binary, when
it is not simply `fzf` on your PATH). A piped or scripted run always gets
the prompt. `shigoku --paths` shows which of the two a run from your shell
would use — and, when it is the prompt, why (no `fzf` on the PATH shigoku
sees, not at a terminal, or the config). A picker that fails to start,
exits with an error or crashes hands that pick, and the rest of the run, to
the numbered prompt with a note saying so; Esc in the picker is still just
"bye".

A command-line search walks the streaming sources that can search, your
preferred one first (`-p` for one run, `preferred_provider` in `config.json`
otherwise): a source that fails or finds nothing is noted on the spot and
the next one is tried, and the first that answers serves the run (its
episode list, its streams). AniLibria, whose titles come back in Russian, is
always the last source tried — unless it is your preferred one — and its
results show the romaji or English title alongside, in parentheses. Only
when every source fails does the command exit with an error; a source that
answered "nothing" makes it a plain "no results".

An episode that won't play on the run's source — its stream never opens
(after the three tries), or none can be found — is looked for on the other
sources, your preferred one first and then the usual order, the way the
interface fails over: each is asked for the show (MegaPlay, which can't
search, by its MyAnimeList id; the others by a title search) and then for
the same episode, and the first that plays it serves the rest of the run.
A line names each source as it is tried. Only when none has it, or mpv
itself is the problem (missing, crashing), does the run end with the
error.

A shigoku built with `-DWITH_TUI=OFF` has only this command line — no
interface, no `shigoku-manga` — and running a bare `shigoku` there prints the
usage.

## shigoku-manga (the manga reader)

`shigoku-manga` is a separate app for manga, built alongside `shigoku`
whenever the interface is. It looks and moves like `shigoku`, but keeps its
own settings, library and sign-in, so the two never touch each other's
data. Search a source, follow titles into a library, and read chapters in
[shigoku-view](#shigoku-view-the-page-viewer). A chapter is downloaded in
full before it opens, so anything you have read stays readable offline.

| Source | Notes |
|---|---|
| MangaDex | The default. Chapters in the `chapter_lang` language; the only source whose titles sync to AniList |
| WeebCentral | |
| Dynasty Reader | |
| MangaHub | Needs libcurl-impersonate installed (loaded only when used). Titles the site marks adult stay hidden unless `nsfw_sources` is on |
| nhentai | Hidden entirely unless `nsfw_sources` is on |

MangaDex has all four rankings (`1`–`4` below); WeebCentral and MangaHub
have the first three; Dynasty Reader and nhentai have none.

| Key | Action |
|---|---|
| `/` | Search the active source |
| `1`–`4` | Browse the active source's rankings instead: Popular / Latest updates / New / Top rated. Scrolling to the end loads more |
| `[` / `]` | Step to the previous / next ranking the source has |
| `s` | Switch to the next source (remembered for next time) |
| `Enter` | On a result: load its chapters. On a chapter: read it |
| `l` / `h`, `Esc` | Move into the chapter list / back to the results |
| `j`/`k`, `g`/`G`, `PgUp`/`PgDn`, wheel | Move through the list |
| `f` | Follow or unfollow the selected title |
| `L` | Switch between your library (followed titles, with unread counts) and the search results |
| `r` | Continue: open the first chapter after the last one you read |
| `D` | Download every unread chapter of the title in the background |
| `U` | Check every followed title for new chapters |
| `u` | Undo the last "marked read" |
| `A` / `S` | Connect AniList / push this title's progress to AniList now |
| `q` | Quit |

Clicks and the wheel work as in `shigoku`. Reading a chapter to its last
page marks it read; closing the viewer earlier remembers the page, and the
chapter opens there next time. Once AniList is connected (the same account
as `shigoku`'s, signed in separately with `A`), finishing a chapter of a
MangaDex title also moves your AniList progress up to it. It never moves
the entry backwards and leaves your score alone.

There is no Settings view yet: the app writes
`~/.config/shigoku-manga/config.json` with these defaults on its first run,
and you edit it there.

| Key | Default | What it does |
|---|---|---|
| `source` | `md` | The source to start in: `md`, `wc`, `dy`, `mh`, or `nh` |
| `chapter_lang` | `en` | MangaDex chapter language |
| `data_saver` | `false` | MangaDex's compressed pages, for slow links |
| `viewer` | `builtin` | `builtin` (shigoku-view), `preview` (macOS Preview), or `custom` |
| `viewer_path` | `""` | The program `custom` runs, handed the chapter's folder |
| `rtl` | `false` | Turn pages right to left |
| `nsfw_sources` | `false` | Show nhentai, and MangaHub's adult titles |
| `anilist_sync` | `true` | Send read progress to AniList when connected |

Only the built-in viewer reports where you stopped. With `preview` or a
custom viewer, chapters open but are not marked read and do not resume.
Chapters are stored as numbered images under
`~/.local/share/shigoku-manga/library/<title>/<chapter>/`, so the library is
browsable from a file manager too.

## shigoku-view (the page viewer)

`shigoku-view` is the SDL page viewer `shigoku-manga` reads chapters in. It
also works on its own, launched from the command line, for any folder of
images or a document. It opens:

- a directory or list of image files (`.jpg`/`.jpeg`/`.png`/`.webp`),
  sorted in natural order (`page2.png` before `page10.png`);
- a single PDF, EPUB, FB2, or XPS/OXPS document — only in a build with
  libmupdf (on by default; auto-off if the library isn't found at build
  time, or with `-DWITH_MUPDF=OFF`);
- a single CBZ, CBR, or CBT comic archive — only in a build with libarchive
  (same auto-off behaviour, `-DWITH_LIBARCHIVE=OFF`); the archive is
  extracted once into a temp directory of images and read like any other
  page list.

A build missing one of the two libraries still opens everything else
exactly as normal — only the unsupported format prints an error naming
which flag or missing library to fix.

```
shigoku-view [--rtl] [--start-page N] [--report-file PATH]
             [--fit page|width] [--title S]
             <dir | file... | doc.pdf/.epub/.fb2/.xps | comic.cbz/.cbr/.cbt>
```

`--rtl` mirrors the left/right page-turn direction (manga reading order);
`--start-page` opens at a given 1-based page, clamped to the document's
actual count; `--fit` picks the initial fit mode, `page` (default) or
`width`; `--title` sets the window title; `--report-file` writes the last
page viewed (`LAST_PAGE=<N>`) to a file on quit instead of stdout — this is
how `shigoku-manga` reads back where you left off.

| Key | Action |
|---|---|
| `space` / `PgDn` / `j` | Next page |
| `backspace` / `PgUp` / `k` | Previous page |
| `left` / `right` | Page (pans first when zoomed in) |
| `up` / `down` | Scroll |
| `home` / `end` | First / last page |
| `f` | Fit page ↔ fit width |
| `+` / `-` / `0` | Zoom in / out / reset (`Cmd +`/`-`/`0` on macOS) |
| `shift+f` / `F11` | Fullscreen (View menu on macOS) |
| `p` | Page-number overlay |
| `q` | Quit (`Esc` leaves fullscreen first) |

Two things worth knowing:

- **Reflowable books renumber.** An EPUB or FB2 relayouts its text on every
  resize and fullscreen toggle, so the page count — and which page a given
  spot in the book lands on — can change. `shigoku-view` tries to keep your
  place across a relayout (by chapter and fractional position within it),
  but the page number itself isn't stable the way a PDF's or an image's is.
- **Password-protected PDFs are refused**, with a clear error — there's no
  password prompt in v1.

The document and archive formats above don't change how `shigoku-manga`
uses the viewer, since a manga chapter is never a PDF, EPUB, or comic
archive.

## Files on disk

Paths respect `XDG_CONFIG_HOME` / `XDG_DATA_HOME` / `XDG_CACHE_HOME` /
`XDG_RUNTIME_DIR` if you've set them; otherwise:

| What | Path |
|---|---|
| Settings | `~/.config/shigoku/config.json` |
| Account tokens (AniList + MyAnimeList) | `~/.config/shigoku/auth.json` |
| Watchlist, history, and cache tables | `~/.local/share/shigoku/shigoku.db` |
| Downloaded episodes (default) | `~/.local/share/shigoku/downloads` |
| Cover art cache | `~/.cache/shigoku/covers` |
| AniSkip data cache | `~/.cache/shigoku/aniskip` |
| `mpv` IPC socket (while playing) | `$XDG_RUNTIME_DIR/shigoku` (or `/tmp/shigoku`) |

Run `shigoku --paths` at any time to see these resolved for your own
machine. `shigoku-manga` keeps its own set in the same places under
`shigoku-manga` instead of `shigoku`: `config.json` and `auth.json`, then
`manga.db` and the chapter `library/` under the data directory.

## Key appendix

A flat reference of everything above, grouped by context, for grepping.

**Everywhere**

| Key | Action |
|---|---|
| `Space` | Zoom toggle: open the current selection's full page, or close it from inside |
| `Esc` | One step back (close a page/pane/overlay/prompt) |
| `q` | Quit (saves Settings first if you were editing there) |
| `Ctrl-C` | Quit immediately, no save step |
| `B` / `H` / `D` / `C` / `S` | Jump to Browse / History / Discover / Calendar / Settings |
| `/` | Search AniList from anywhere — jumps to Browse with the search box open. Exception: in History, `/` filters the watchlist locally instead |
| `~` | Similar shows to the selected one, as a Discover feed |
| Arrow keys | Alias `h`/`j`/`k`/`l` everywhere outside a text box |
| Mouse click / double-click / wheel | Select / activate (`Enter`) / arrow-step |

**Browse**

| Key | Action |
|---|---|
| `/` | Search AniList |
| `j`/`k`, `g`/`G` | Move / jump the results list |
| `l`, `Enter`, double-click | Open detail preview (wide) or full page (narrow) |
| `Space` | Open full page (any width) |
| `P` | Add to watchlist |

**History**

| Key | Action |
|---|---|
| `/` | Filter the list (local) |
| `f` / `F` | Status filter (the last stop is *behind*): next / clear |
| `j`/`k`, `g`/`G` | Move / jump the list |
| `l`, `Enter`, double-click | Open detail preview (wide) or full page (narrow) |
| `Space` | Open full page (any width) |
| `w`/`p`/`c`/`x`/`P` | Watching / Paused / Completed / Dropped / Planning |
| `s` | Score prompt (0–10) |
| `r` | Recompute progress |
| `u` | Undo last status change |
| `X` then `y` | Delete show + history (confirm) |

**Discover**

| Key | Action |
|---|---|
| `h`/`j`/`k`/`l` | Move grid cursor |
| `g`/`G` | Jump to first/last card |
| `1`–`4` | Jump to Trending / Popular / Top Rated / This Season |
| `5` | The Similar feed (once `~` opened one) |
| `[`/`]` | Previous/next feed |
| `f` | Filter overlay |
| `Enter`/`Space`/double-click | Open full page |
| `P` | Add to watchlist |

**Discover filter overlay**

| Key | Action |
|---|---|
| `j`/`k` | Move between rows |
| `h`/`l` | Change value |
| `Space` | Toggle genre |
| `c` | Clear filters |
| `Enter` | Apply |
| `Esc` | Discard |

**Calendar**

| Key | Action |
|---|---|
| `j`/`k`, `g`/`G` | Move / jump |
| `Enter`/`Space`/double-click | Open full page |

**Settings**

| Key | Action |
|---|---|
| `j`/`k` | Move between rows |
| `h`/`l` | Cycle a value |
| `Space` | Flip a toggle |
| `Enter` | Edit a text row, or fire a connect action |
| `Esc` (while editing) | Cancel the edit |

**The show page (and Browse/History's preview pane)**

| Key | Action |
|---|---|
| `h`/`j`/`k`/`l` | Move episode grid (`j`/`k` by row, `h`/`l` by cell) |
| `g`/`G` | Jump to first/last episode (or first/last recommendation while that section holds focus) |
| `Enter`/double-click | Play |
| `v` | Cycle streaming source |
| `d` | Download |
| `c` | Toggle Characters & Recommendations — **full page only** |
| `Tab` | Swap focus between that section and the grid — **full page only** |
| `Enter` (on a recommendation) | Open that show — **full page only** |

**Connect modal (AniList / MyAnimeList sign-in)**

| Key | Action |
|---|---|
| `c` | Copy the sign-in URL |
| `Esc` | Stop waiting (an in-flight callback still completes) |
