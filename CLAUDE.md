# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Pebble watchapp for tracking parcels: a list of your parcels with their
current delivery status, and a scrollable timeline of carrier events for each.

There is **no server**. The watch can't make HTTP calls, so the phone-side
PebbleKit JS (`watch/src/pkjs/index.js`) talks to the
[Ship24 Tracking API](https://docs.ship24.com) directly. The API key is the
user's own — they paste it into the settings page, it lives in the phone's
`localStorage`, and it only ever leaves the phone in an `Authorization` header.

```
watch C  ⇄ (AppMessage) ⇄  pkjs  ⇄ (HTTPS + Bearer) ⇄  api.ship24.com
                              ↑
                    docs/index.html (settings, GitHub Pages)
```

| Dir      | Role                                                              |
|----------|-------------------------------------------------------------------|
| `watch/` | Pebble app: C UI + pkjs bridge                                    |
| `docs/`  | Settings page served from GitHub Pages (API key + parcel list)    |
| `tools/` | Node tests for pkjs and the settings page. Not shipped.           |

## Commands

Watch (run from `watch/` — `pebble` commands fail elsewhere):

```sh
pebble build                                    # all 7 platforms
pebble install --emulator basalt                # also: chalk (round), aplite (b/w)
pebble logs --emulator basalt                   # pkjs console.log lands here
pebble emu-button --emulator basalt click select
pebble emu-button --emulator basalt push select # long-press = force refresh
pebble screenshot --emulator basalt --no-open shot.png
pebble kill
# `pebble clean` after editing package.json messageKeys — the generated
# message-key header isn't always regenerated.
```

Tests (run from `tools/`, needs `npm install` once):

```sh
npm test              # both suites
npm run test:pkjs     # pkjs against stubbed Ship24 responses
npm run test:settings # docs/index.html driven in jsdom
```

`tools/test-pkjs.js` asserts the **byte budgets** the packed payloads must obey.
Those numbers mirror the fixed-size buffers in `watch/src/c/*.c` — if you widen
`MAX_DESC_BYTES` in pkjs, widen `EventRow.description` and the `C_BUFFERS` table
in the test to match, or the C side will truncate mid-character.

Verifying UI changes means the emulator, not just a green build. The pkjs has no
mock mode: to see real rows, either configure a Ship24 key on a phone, or
temporarily append a stub that reassigns `ship24 = function (method, path, body, cb)`
and seeds `localStorage`, then remove it before committing.

## Watch ⇄ phone protocol (AppMessage)

Keys live in `watch/package.json` `messageKeys`: `REQUEST`, `INDEX`, `TYPE`,
`PAYLOAD`, `ERROR`.

- **watch → phone:** `REQUEST` = `"list"` | `"refresh"` (ignore the cache TTL) |
  `"detail"` (+ `INDEX`, the parcel's position in the phone's list).
- **phone → watch:** `TYPE` = `"parcels"` | `"events"` | `"error"`, plus
  `PAYLOAD` or `ERROR`. On inbound messages **`INDEX` is reused as a "more is
  coming" flag** (1 = an interim answer, 0 = final).

Packed payloads use `\x1f` (unit separator) between fields and `\n` between
records — pkjs strips both from carrier text, so neither can be injected:

- `parcels`: `nickname \x1f milestoneCode \x1f label \x1f ago`
- `events`: `when \x1f description \x1f location`, newest first

**Every request is answered immediately**, from cache or with an empty payload
carrying the pending flag, because the watch re-asks after 5s of silence
(`comm.c`) and a cold Ship24 fetch easily outlasts that. An empty payload with
pending set keeps the watch on its loading screen rather than showing "none".

The very first request at app launch often lands before the phone-side JS is
running and is silently dropped; pkjs therefore also delivers the list from its
own `ready` handler.

## Watch side (`watch/src/c/`)

The C is a dumb renderer — all networking, sorting, truncation and wording
happen in pkjs. To change what a screen says, edit the JS.

- `main.c` — parcel list. Long-press SELECT force-refreshes. On colour
  rectangular screens each row gets a status-coloured bar in the left gutter
  (`status_colour.c`); round screens skip it and centre the section header,
  which the circular mask would otherwise clip.
- `timeline_window.c` — events for one parcel, under a status-coloured header
  showing nickname + current status.
- `event_window.c` — one event's full text in a `ScrollLayer`, framed in the
  status colour. This is why `MAX_DESC_BYTES` is generous (160): menu rows
  ellipsize anyway, so the budget exists for this screen.
- `comm.c` — AppMessage transport (4096/128 buffers). Two independent retries:
  three send attempts if the outbox won't take the message, and three re-asks if
  a sent request goes unanswered. Windows register handlers via
  `comm_set_handlers` in their `.appear` handler, so whichever window is visible
  receives the replies.

Buffers are fixed-size and static (aplite has 24 KB of RAM for everything);
`prv_parse_*` reads the payload one line at a time rather than copying it.

## Phone side (`watch/src/pkjs/index.js`)

- `localStorage`: `api_key`, `parcels` (array of
  `{nickname, trackingNumber, courierCode, trackerId}`), and
  `cache_<trackingNumber>` (`{fetchedAt, milestone, events}`), TTL 5 minutes.
- Ship24 flow is **tracker-based** (the per-shipment plans): `POST /trackers`
  once per new number on settings save, then
  `GET /trackers/search/{trackingNumber}/results` to refresh. Registration
  failures are logged and ignored — the search endpoint answers regardless.
- Refreshes are **sequential** (`refreshSequentially`), never parallel: parallel
  requests are the fastest way to trip the rate limiter.
- Ship24's published docs simplify the event shape. The live API sends
  `occurrenceDatetime`/`datetime` + `utcOffset` rather than `timestamp`, so
  `eventTimeMs` accepts all of them and `eventDescription` falls back through
  `description` → `status` → `statusCode` → `statusMilestone`.
- A failed refresh is reported **on the affected parcel's row** (via `s_errors`),
  not as a full-screen error — one dead tracking number must not hide the rest.
  The dedicated error screen is reserved for "no API key set".

## Settings page (`docs/index.html`)

Self-contained, no build step, ES5-only (old Pebble webviews). Opened by
`showConfiguration` with `?parcels=<json>&has_key=0|1`; the API key is **never**
sent to the page, only whether one is stored. Save returns
`pebblejs://close#<encoded JSON>`; a blank key field means "keep the stored one".

`CONFIG_URL` in the pkjs points at `https://qichuan.github.io/pebble-parcel-tracking/`,
which requires GitHub Pages to be enabled on this repo (`main` branch, `/docs`).
Until that's done the settings page 404s and no parcels can be added.
