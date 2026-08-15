# shigoku — user manual

shigoku is a terminal application for finding, tracking, and watching anime
without leaving the terminal. It searches AniList's catalogue, renders cover
art directly in supporting terminals, resolves and plays episodes through
`mpv` (with resume and optional auto-skip of openings/endings), and keeps a
local watchlist that can optionally sync to your AniList and MyAnimeList
accounts. It's built to run comfortably on old hardware too, PowerPC Macs
included. The screen is deliberately kept sparse — a handful of focused
views rather than one crowded dashboard — so this manual exists to make sure
nothing useful stays hidden behind a key you never happened to try.

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
speaks the Kitty graphics protocol — everything else shows a plain
placeholder box instead and works exactly the same otherwise.

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

Search AniList's whole catalogue.

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
| `q` | Quit |

Scrolling to the last loaded result quietly fetches the next page.

## History (key `H`)

Your watchlist, grouped by status.

| Key | Action |
|---|---|
| `j`/`k`, arrows, wheel | Move through the list |
| `g`/`G` | Jump to the first/last entry |
| `/` | Filter the list in place by title — local and instant, no network |
| `l`, `Enter`, double-click | Open the detail preview pane on a wide terminal, or the full page directly on a narrow one |
| `Space` | Open the full page directly, from any width |
| `w` / `p` / `c` / `x` / `P` | Set status: Watching / Paused / Completed / Dropped / Planning |
| `s` | Open a score prompt: type `0`–`10` (decimals allowed, e.g. `7.5`), `Enter` to save, or leave it blank and `Enter` to clear the score |
| `r` | Recompute progress from your actual playback history |
| `u` | Undo the last status change (one level only) |
| `X`, then `y` | Delete the show and its local episode history — a second key press confirms; anything else cancels |
| `q` | Quit |

A show that aired a new episode since you last checked carries a `NEW` tag
in this list until you open it (see [Calendar](#calendar-key-c) for where
that comes from). Your score is stored as a plain 0–10 number and converted
automatically to whatever scale your AniList account actually uses
(100-point, 10-point, 5-star, etc.) whenever it syncs — you never need to
think about the conversion yourself.

## Discover (key `D`)

A cover-art grid across four ranking feeds.

| Key | Action |
|---|---|
| `h`/`j`/`k`/`l`, arrows, wheel | Move the grid cursor |
| `g`/`G` | Jump to the first/last card of the feed |
| `1` / `2` / `3` / `4` | Jump straight to Trending / Popular / Top Rated / This Season |
| `[` / `]` | Step to the previous/next feed |
| `f` | Open the filter overlay (below) |
| `Enter`, double-click | Open the show's full page |
| `Space` | Same, from anywhere in the grid |
| `P` | Add the highlighted show to your watchlist |
| `/` | Jump to Browse with a text search open — Discover itself has no free-text search |
| `q` | Quit |

### Filter overlay (`f`)

Narrows the active feed by genre, year, status, and minimum score.

| Key | Action |
|---|---|
| `j`/`k` | Move between the Genre / Year / Status / Minimum score rows |
| `h`/`l` | Change the highlighted row's value (on Genre, this walks the genre list instead) |
| `Space` | Toggle a genre on or off (Genre row only) |
| `c` | Clear every filter back to defaults |
| `Enter` | Apply and close |
| `Esc` | Discard and close |

## Calendar (key `C`)

Shows upcoming episodes for anything already on your watchlist, grouped by
weekday and sorted soonest-first, with a countdown (`2d 4h`, `37m`, `<1m`).

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
| MyAnimeList | mal client id | text | Your own registered MAL app's client ID — required before connecting |
| | connect | action | Opens your browser to authorize the MAL mirror |
| Downloads | download dir | text | Where downloaded episodes are stored; blank = the default, under the data directory |
| | ffmpeg path | text | Binary used to save HLS streams to disk |
| Player backend | player | cycle | `mpv` / `mplayer` / `qmplay2` — see [Playback](#playback) |
| | player path | text | Binary for whichever *non-mpv* backend is selected; blank = that backend's default name. `mpv` always uses "mpv path" above instead |

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
Player.

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

## Downloads

Save an episode to disk instead of streaming it.

| Where | How |
|---|---|
| The show page or preview pane | Press `d` on the highlighted episode |
| Command line | `shigoku download <title> [<episode>]` |

Files land under `<download dir>/<AniList id>/<sub or dub>/<episode>.<ext>`
("download dir" is set in Settings; blank defaults to `<data dir>/downloads`
— see [Files on disk](#files-on-disk)). A transfer writes to a `.part` file
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

Register your own app at `myanimelist.net/apiconfig` (redirect URL
`http://127.0.0.1:8767/mal/callback`), enter its client ID under Settings →
MyAnimeList → *mal client id*, then use *connect* below it the same way as
AniList's. Once connected, MAL is a one-way mirror: status, progress, and
score push to it alongside AniList — nothing is ever pulled back from MAL.

## Command line

Running `shigoku` with no arguments opens the interface described above.
Everything below runs without opening it, and prints straight to the
terminal:

| Command | Does |
|---|---|
| `shigoku <title> [--dub\|--sub] [--quality <q>]` | Search, then pick a result and an episode from numbered lists, then play it |
| `shigoku download <title> [<episode>] [--dub\|--sub]` | Same search-and-pick flow, but downloads instead of playing; giving `<episode>` skips that last prompt (it matches an exact episode label first, then falls back to a 1-based position) |
| `shigoku login [--paste]` | Sign in to AniList from the terminal — a successful sign-in immediately runs a sync too. `--paste` swaps the automatic browser-and-listener flow for manually pasting the redirect URL |
| `shigoku sync` | Pull and push your AniList list once, and push to MyAnimeList if connected, then exit — this always runs, even if Settings' *sync enabled* is switched off (that toggle only gates the interface's own automatic sync) |
| `shigoku update` | Check GitHub for a newer release — shigoku never updates itself; reinstall via your package manager |
| `shigoku --paths` | Print where config, database, cache, and `mpv` currently resolve to |
| `shigoku --version` / `-V` | Print the version and exit |
| `shigoku --debug` | Add to any command above for verbose diagnostics (stderr on the command line, a log file when running the interface) |

`--quality` is accepted but not actually wired up yet — every command
currently plays the best direct stream available regardless of what you ask
for.

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
machine.

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
